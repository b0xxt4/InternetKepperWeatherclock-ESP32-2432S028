#include "internet_keeper.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "weather_ca.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// Alte, mit frueheren Wetteruhr-Versionen erzeugte secrets.h-Dateien
// enthalten noch keinen RUB-Zugang. Damit bleibt die Firmware baubar und
// zeigt stattdessen klar "nicht konfiguriert" an, bis configure.py erneut
// ausgefuehrt wurde.
#if !defined(WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED)
#define WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED 0
static const char RUB_LOGIN_ID[] = "";
static const char RUB_PASSWORD[] = "";
#endif

namespace weatherclock {
namespace {

constexpr size_t NO_WIFI_INDEX = static_cast<size_t>(-1);

#if defined(WEATHERCLOCK_SECRETS_CONFIGURED) && \
    WEATHERCLOCK_SECRETS_CONFIGURED == 1 && \
    defined(WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED) && \
    WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED == 1
constexpr bool SECRETS_CONFIGURED = true;
#else
constexpr bool SECRETS_CONFIGURED = false;
#endif

static_assert(WIFI_CREDENTIAL_COUNT > 0,
              "At least one Wi-Fi entry must exist in the secrets template");
static_assert(WIFI_CREDENTIAL_COUNT <= 127,
              "The display status supports at most 127 Wi-Fi entries");

struct WifiRuntimeState {
  bool reachable = false;
  bool checked = false;
  bool online = false;
  bool login_failed = false;
  int64_t last_check_epoch = 0;
  int64_t last_login_epoch = 0;
  uint32_t check_count = 0;
  uint32_t login_attempt_count = 0;
  uint32_t login_success_count = 0;
  uint32_t last_login_attempt_ms = 0;
};

WifiRuntimeState wifi_states[WIFI_CREDENTIAL_COUNT];
size_t active_wifi_index = NO_WIFI_INDEX;

bool credentialsConfigured() {
  if (!SECRETS_CONFIGURED || RUB_LOGIN_ID[0] == '\0' ||
      RUB_PASSWORD[0] == '\0') {
    return false;
  }
  for (size_t index = 0; index < WIFI_CREDENTIAL_COUNT; ++index) {
    if (WIFI_CREDENTIALS[index].ssid == nullptr ||
        WIFI_CREDENTIALS[index].ssid[0] == '\0' ||
        WIFI_CREDENTIALS[index].password == nullptr) {
      return false;
    }
  }
  return true;
}

bool timeIsPlausible() { return time(nullptr) >= 1704067200; }

void copyText(char *destination, size_t size, const char *source) {
  if (size == 0) return;
  std::snprintf(destination, size, "%s", source == nullptr ? "" : source);
}

void updateConnectionFields(KeeperRuntimeStatus &status) {
  status.wifi_connected = WiFi.status() == WL_CONNECTED;
  if (!status.wifi_connected) {
    status.wifi_rssi = 0;
    status.ip[0] = '\0';
    return;
  }
  status.wifi_rssi = static_cast<int16_t>(WiFi.RSSI());
  const String ip = WiFi.localIP().toString();
  copyText(status.ip, sizeof(status.ip), ip.c_str());
}

void publish(KeeperRuntimeStatus &status, KeeperStatusPublisher publisher) {
  updateConnectionFields(status);
  ++status.revision;
  if (publisher != nullptr) publisher(status);
}

void setEvent(KeeperRuntimeStatus &status, KeeperPhase phase,
              KeeperStatusPublisher publisher, const char *format, ...) {
  status.phase = phase;
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(status.last_event, sizeof(status.last_event), format,
                 arguments);
  va_end(arguments);
  Serial.printf("[InternetKeeper] %s\n", status.last_event);
  publish(status, publisher);
}

int monthNumber(const char *month) {
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr",
                                      "May", "Jun", "Jul", "Aug",
                                      "Sep", "Oct", "Nov", "Dec"};
  for (int index = 0; index < 12; ++index) {
    if (std::strcmp(month, MONTHS[index]) == 0) return index + 1;
  }
  return 0;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

bool setClockFromHttpDate(const String &date_header) {
  char weekday[4] = {};
  char month_name[4] = {};
  char timezone[4] = {};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  const int fields =
      std::sscanf(date_header.c_str(), "%3s, %d %3s %d %d:%d:%d %3s",
                  weekday, &day, month_name, &year, &hour, &minute, &second,
                  timezone);
  const int month = monthNumber(month_name);
  if (fields != 8 || std::strcmp(timezone, "GMT") != 0 || month == 0 ||
      year < 2024 || year > 2039 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return false;
  }

  const int64_t epoch =
      daysFromCivil(year, static_cast<unsigned>(month),
                    static_cast<unsigned>(day)) *
          86400LL +
      hour * 3600LL + minute * 60LL + second;
  struct timeval value {
    static_cast<time_t>(epoch), 0
  };
  return settimeofday(&value, nullptr) == 0;
}

bool bootstrapClockFromPortal(KeeperRuntimeStatus &status,
                              KeeperStatusPublisher publisher) {
  setEvent(status, KeeperPhase::BootstrappingClock, publisher,
           "Setze Uhr fuer sicheren RUB-Login");
  WiFiClient client;
  HTTPClient request;
  request.setConnectTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  request.setTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  const char *headers[] = {"Date"};
  request.collectHeaders(headers, 1);
  if (!request.begin(client, weather_config::PORTAL_TIME_URL)) {
    setEvent(status, KeeperPhase::Idle, publisher,
             "Zeitabfrage konnte nicht starten");
    return false;
  }

  const int http_status = request.GET();
  const String date_header = request.header("Date");
  request.end();
  if (http_status <= 0 || date_header.isEmpty() ||
      !setClockFromHttpDate(date_header)) {
    setEvent(status, KeeperPhase::Idle, publisher,
             "Zeitabfrage fehlgeschlagen (HTTP %d)", http_status);
    return false;
  }
  setEvent(status, KeeperPhase::Idle, publisher,
           "Uhr ueber RUB-Portal gesetzt");
  return true;
}

String urlEncode(const char *input) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(std::strlen(input) * 3);
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(input);
       *cursor != '\0'; ++cursor) {
    const unsigned char value = *cursor;
    const bool unreserved =
        (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '-' || value == '_' ||
        value == '.' || value == '~';
    if (unreserved) {
      encoded += static_cast<char>(value);
    } else {
      encoded += '%';
      encoded += HEX_DIGITS[value >> 4];
      encoded += HEX_DIGITS[value & 0x0F];
    }
  }
  return encoded;
}

bool checkConnectivityEndpoint(const char *url, int expected_status,
                               const char *expected_body,
                               int &actual_status) {
  WiFiClient client;
  HTTPClient request;
  request.setConnectTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  request.setTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  request.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!request.begin(client, url)) {
    actual_status = HTTPC_ERROR_CONNECTION_REFUSED;
    return false;
  }
  actual_status = request.GET();
  String body;
  if (actual_status == expected_status && expected_body != nullptr) {
    body = request.getString();
    body.trim();
  }
  request.end();
  return actual_status == expected_status &&
         (expected_body == nullptr || body == expected_body);
}

bool checkInternet(KeeperRuntimeStatus &status,
                   KeeperStatusPublisher publisher,
                   KeeperPhase phase = KeeperPhase::CheckingInternet) {
  ++status.check_count;
  status.last_check_epoch = timeIsPlausible() ? time(nullptr) : 0;
  WifiRuntimeState *wifi_state =
      active_wifi_index < WIFI_CREDENTIAL_COUNT
          ? &wifi_states[active_wifi_index]
          : nullptr;
  if (wifi_state != nullptr) {
    ++wifi_state->check_count;
    wifi_state->checked = true;
    wifi_state->last_check_epoch = status.last_check_epoch;
  }

  setEvent(status, phase, publisher, "Pruefe echten Internetzugang");
  if (WiFi.status() != WL_CONNECTED) {
    status.internet = InternetState::WifiDown;
    if (wifi_state != nullptr) {
      wifi_state->reachable = false;
      wifi_state->online = false;
    }
    setEvent(status, KeeperPhase::Idle, publisher,
             "Internet-Test: WLAN getrennt");
    return false;
  }

  int primary_status = 0;
  bool online = checkConnectivityEndpoint(
      weather_config::CONNECTIVITY_CHECK_URL_PRIMARY,
      weather_config::CONNECTIVITY_EXPECTED_STATUS_PRIMARY, nullptr,
      primary_status);
  int secondary_status = 0;
  if (!online) {
    online = checkConnectivityEndpoint(
        weather_config::CONNECTIVITY_CHECK_URL_SECONDARY,
        weather_config::CONNECTIVITY_EXPECTED_STATUS_SECONDARY,
        weather_config::CONNECTIVITY_EXPECTED_BODY_SECONDARY,
        secondary_status);
  }

  if (wifi_state != nullptr) {
    wifi_state->reachable = true;
    wifi_state->online = online;
  }
  status.internet = online ? InternetState::Online
                           : InternetState::CaptivePortal;
  if (online) {
    if (wifi_state != nullptr) wifi_state->login_failed = false;
    const int successful_status =
        primary_status == weather_config::CONNECTIVITY_EXPECTED_STATUS_PRIMARY
            ? primary_status
            : secondary_status;
    setEvent(status, KeeperPhase::Idle, publisher,
             "Internet erreichbar (HTTP %d)", successful_status);
  } else {
    setEvent(status, KeeperPhase::Idle, publisher,
             "Kein Internet (Tests HTTP %d/%d)", primary_status,
             secondary_status);
  }
  return online;
}

bool loginAtRubPortal(KeeperRuntimeStatus &status,
                      KeeperStatusPublisher publisher) {
  if (WiFi.status() != WL_CONNECTED ||
      active_wifi_index >= WIFI_CREDENTIAL_COUNT) {
    status.internet = InternetState::WifiDown;
    setEvent(status, KeeperPhase::Idle, publisher,
             "RUB-Login: WLAN-Zuordnung fehlt");
    return false;
  }

  WifiRuntimeState &wifi_state = wifi_states[active_wifi_index];
  if (wifi_state.last_login_attempt_ms != 0 &&
      millis() - wifi_state.last_login_attempt_ms <
          weather_config::LOGIN_COOLDOWN_MS) {
    setEvent(status, KeeperPhase::Idle, publisher,
             "RUB-Login: Wiederholungssperre aktiv");
    return false;
  }
  wifi_state.last_login_attempt_ms = millis();
  wifi_state.login_failed = false;
  ++wifi_state.login_attempt_count;
  ++status.login_attempt_count;

  if (!timeIsPlausible() && !bootstrapClockFromPortal(status, publisher)) {
    wifi_state.login_failed = true;
    status.internet = InternetState::LoginFailed;
    setEvent(status, KeeperPhase::Idle, publisher,
             "RUB-Login: keine sichere Uhrzeit");
    return false;
  }

  setEvent(status, KeeperPhase::LoggingIn, publisher,
           "Sende RUB-Login ueber geprueftes HTTPS");
  WiFiClientSecure secure_client;
  secure_client.setCACert(ISRG_ROOT_X1);
  secure_client.setHandshakeTimeout(
      weather_config::KEEPER_HTTP_TIMEOUT_MS / 1000UL);
  HTTPClient request;
  request.setConnectTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  request.setTimeout(weather_config::KEEPER_HTTP_TIMEOUT_MS);
  request.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!request.begin(secure_client, weather_config::LOGIN_URL)) {
    wifi_state.login_failed = true;
    status.internet = InternetState::LoginFailed;
    setEvent(status, KeeperPhase::Idle, publisher,
             "RUB-Login konnte nicht starten");
    return false;
  }

  request.addHeader("Content-Type", "application/x-www-form-urlencoded");
  request.addHeader("Cache-Control", "no-cache");
  // ipaddr bleibt absichtlich weg: Das Portal soll die tatsaechliche
  // Quell-/NAT-IP der HTTPS-Anfrage freischalten.
  const String body = String("code=1&loginid=") + urlEncode(RUB_LOGIN_ID) +
                      "&password=" + urlEncode(RUB_PASSWORD) +
                      "&action=Login";
  const int http_status = request.POST(body);
  request.end();
  if (http_status < 200 || http_status >= 400) {
    wifi_state.login_failed = true;
    status.internet = InternetState::LoginFailed;
    if (http_status < 0) {
      setEvent(status, KeeperPhase::Idle, publisher,
               "RUB-Login Transportfehler %d", http_status);
    } else {
      setEvent(status, KeeperPhase::Idle, publisher,
               "RUB-Login antwortete HTTP %d", http_status);
    }
    return false;
  }

  for (uint8_t attempt = 0;
       attempt < weather_config::POST_LOGIN_CHECK_ATTEMPTS; ++attempt) {
    setEvent(status, KeeperPhase::VerifyingLogin, publisher,
             "Bestaetige RUB-Login (%u/%u)",
             static_cast<unsigned>(attempt + 1),
             static_cast<unsigned>(weather_config::POST_LOGIN_CHECK_ATTEMPTS));
    const uint32_t wait_ms =
        attempt == 0 ? weather_config::POST_LOGIN_FIRST_WAIT_MS
                     : weather_config::POST_LOGIN_NEXT_WAIT_MS;
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    if (checkInternet(status, publisher, KeeperPhase::VerifyingLogin)) {
      ++wifi_state.login_success_count;
      ++status.login_success_count;
      status.last_login_epoch = timeIsPlausible() ? time(nullptr) : 0;
      wifi_state.last_login_epoch = status.last_login_epoch;
      wifi_state.login_failed = false;
      status.internet = InternetState::Online;
      setEvent(status, KeeperPhase::Idle, publisher,
               "RUB-Login erfolgreich bestaetigt");
      return true;
    }
  }

  status.internet = InternetState::LoginFailed;
  wifi_state.login_failed = true;
  setEvent(status, KeeperPhase::Idle, publisher,
           "RUB-Login nicht bestaetigt");
  return false;
}

bool connectWifi(size_t index, KeeperRuntimeStatus &status,
                 KeeperStatusPublisher publisher) {
  if (index >= WIFI_CREDENTIAL_COUNT) return false;
  const WifiCredential &credential = WIFI_CREDENTIALS[index];
  active_wifi_index = index;
  status.active_network_index = static_cast<int8_t>(index);
  copyText(status.ssid, sizeof(status.ssid), credential.ssid);

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == credential.ssid) {
    wifi_states[index].reachable = true;
    status.internet = wifi_states[index].online
                          ? InternetState::Online
                          : InternetState::Unknown;
    publish(status, publisher);
    return true;
  }

  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(250));
  status.internet = InternetState::Unknown;
  setEvent(status, KeeperPhase::ConnectingWifi, publisher,
           "Verbinde WLAN %u/%u: %s", static_cast<unsigned>(index + 1),
           static_cast<unsigned>(WIFI_CREDENTIAL_COUNT), credential.ssid);
  const char *password =
      std::strlen(credential.password) == 0 ? nullptr : credential.password;
  WiFi.begin(credential.ssid, password);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < weather_config::WIFI_CONNECT_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (WiFi.status() != WL_CONNECTED) {
    wifi_states[index].reachable = false;
    wifi_states[index].online = false;
    wifi_states[index].login_failed = false;
    status.internet = InternetState::WifiDown;
    WiFi.disconnect(false, false);
    setEvent(status, KeeperPhase::SwitchingNetwork, publisher,
             "WLAN nicht erreichbar: %s", credential.ssid);
    active_wifi_index = NO_WIFI_INDEX;
    status.active_network_index = -1;
    return false;
  }

  wifi_states[index].reachable = true;
  status.internet = wifi_states[index].online ? InternetState::Online
                                               : InternetState::Unknown;
  setEvent(status, KeeperPhase::SwitchingNetwork, publisher,
           "WLAN verbunden: %s", credential.ssid);
  return true;
}

}  // namespace

void initializeInternetKeeperStatus(KeeperRuntimeStatus &status) {
  std::memset(&status, 0, sizeof(status));
  const bool configured = credentialsConfigured();
  status.phase = configured ? KeeperPhase::Idle : KeeperPhase::Unconfigured;
  status.internet = InternetState::Unknown;
  status.active_network_index = -1;
  status.configured_networks = static_cast<uint8_t>(WIFI_CREDENTIAL_COUNT);
  copyText(status.last_event, sizeof(status.last_event),
           configured ? "InternetKeeper bereit"
                      : "WLAN/RUB-Zugang nicht eingerichtet");
}

bool performInternetKeeperCycle(KeeperRuntimeStatus &status,
                                KeeperStatusPublisher publisher) {
  if (!credentialsConfigured()) {
    initializeInternetKeeperStatus(status);
    publish(status, publisher);
    return false;
  }
  status.reachable_networks = 0;
  status.online_networks = 0;
  setEvent(status, KeeperPhase::SwitchingNetwork, publisher,
           "Starte InternetKeeper-Rundlauf");

  size_t fallback_index = NO_WIFI_INDEX;
  bool primary_reachable = false;
  // Absichtlich kein Abbruch nach dem ersten Online-Treffer: Jede
  // konfigurierte SSID braucht ihren eigenen Check und gegebenenfalls ihren
  // eigenen Lock-and-Key-Login.
  for (size_t index = 0; index < WIFI_CREDENTIAL_COUNT; ++index) {
    if (!connectWifi(index, status, publisher)) continue;
    fallback_index = index;
    primary_reachable = primary_reachable || index == 0;
    ++status.reachable_networks;
    bool online = checkInternet(status, publisher);
    if (!online) online = loginAtRubPortal(status, publisher);
    wifi_states[index].online = online;
    if (online) ++status.online_networks;
  }

  const size_t return_index = primary_reachable ? 0 : fallback_index;
  if (return_index == NO_WIFI_INDEX) {
    WiFi.disconnect(false, false);
    active_wifi_index = NO_WIFI_INDEX;
    status.active_network_index = -1;
    status.internet = InternetState::WifiDown;
    setEvent(status, KeeperPhase::Idle, publisher,
             "Rundlauf: kein WLAN erreichbar");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED || active_wifi_index != return_index) {
    connectWifi(return_index, status, publisher);
  }
  if (WiFi.status() == WL_CONNECTED &&
      active_wifi_index < WIFI_CREDENTIAL_COUNT) {
    status.active_network_index = static_cast<int8_t>(active_wifi_index);
    copyText(status.ssid, sizeof(status.ssid),
             WIFI_CREDENTIALS[active_wifi_index].ssid);
    const WifiRuntimeState &selected = wifi_states[active_wifi_index];
    status.internet = selected.online
                          ? InternetState::Online
                          : (selected.login_failed
                                 ? InternetState::LoginFailed
                                 : InternetState::CaptivePortal);
  } else {
    status.active_network_index = -1;
    status.internet = InternetState::WifiDown;
  }
  setEvent(status, KeeperPhase::Idle, publisher,
           "Rundlauf: %u/%u WLAN, %u/%u online",
           static_cast<unsigned>(status.reachable_networks),
           static_cast<unsigned>(status.configured_networks),
           static_cast<unsigned>(status.online_networks),
           static_cast<unsigned>(status.configured_networks));
  return status.internet == InternetState::Online;
}

bool internetKeeperConfigured() { return credentialsConfigured(); }

}  // namespace weatherclock
