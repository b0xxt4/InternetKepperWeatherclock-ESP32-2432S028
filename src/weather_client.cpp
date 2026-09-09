#include "weather_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <time.h>

#include "config.h"
#include "internet_keeper.h"
#include "weather_ca.h"

namespace weatherclock {
namespace {

constexpr char CACHE_NAMESPACE[] = "wetteruhr";
constexpr char CACHE_KEY[] = "snapshot";
constexpr size_t MAX_RESPONSE_BYTES = 32U * 1024U;

QueueHandle_t snapshot_queue = nullptr;
TaskHandle_t network_task_handle = nullptr;
portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
RuntimeStatus runtime_status{};
std::atomic<bool> manual_refresh_requested{false};
std::atomic<bool> keeper_check_requested{false};
int64_t last_cache_epoch = 0;

// Kept out of the FreeRTOS task stack. Filtering while reading the stream
// avoids allocating a second copy of the HTTP response.
StaticJsonDocument<1024> json_filter;
StaticJsonDocument<12288> json_document;

bool millisReached(uint32_t target) {
  return static_cast<int32_t>(millis() - target) >= 0;
}

bool timeIsPlausible() { return time(nullptr) >= 1704067200; }

uint32_t calculateChecksum(const WeatherSnapshot &snapshot) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&snapshot);
  constexpr size_t CHECKED_BYTES = offsetof(WeatherSnapshot, checksum);
  uint32_t value = 2166136261UL;
  for (size_t index = 0; index < CHECKED_BYTES; ++index) {
    value ^= bytes[index];
    value *= 16777619UL;
  }
  return value;
}

bool rangesAreValid(const WeatherSnapshot &snapshot) {
  if (snapshot.temperature_tenths < -800 ||
      snapshot.temperature_tenths > 700 ||
      snapshot.apparent_temperature_tenths < -1000 ||
      snapshot.apparent_temperature_tenths > 900 ||
      snapshot.humidity > 100 || snapshot.weather_code > 99 ||
      snapshot.wind_speed_tenths > 3000 ||
      snapshot.uv_index_tenths > 400 ||
      snapshot.pressure_tenths < 8000 ||
      snapshot.pressure_tenths > 12000 || snapshot.is_day > 1) {
    return false;
  }

  for (const HourlyPoint &point : snapshot.hourly) {
    if (strnlen(point.hour, sizeof(point.hour)) != 2 ||
        point.hour[0] < '0' || point.hour[0] > '2' ||
        point.hour[1] < '0' || point.hour[1] > '9' ||
        (point.hour[0] == '2' && point.hour[1] > '3') ||
        point.temperature_tenths < -800 || point.temperature_tenths > 700 ||
        point.rain_probability > 100 || point.weather_code > 99) {
      return false;
    }
  }

  for (const DailyPoint &point : snapshot.daily) {
    if (strnlen(point.date, sizeof(point.date)) != 10 ||
        strnlen(point.sunrise, sizeof(point.sunrise)) != 5 ||
        strnlen(point.sunset, sizeof(point.sunset)) != 5 ||
        point.minimum_tenths < -800 ||
        point.minimum_tenths > 700 || point.maximum_tenths < -800 ||
        point.maximum_tenths > 700 ||
        point.minimum_tenths > point.maximum_tenths ||
        point.uv_index_max_tenths > 400 ||
        point.rain_probability > 100 || point.weather_code > 99) {
      return false;
    }
  }
  return true;
}

void setNetworkState(NetworkState state) {
  portENTER_CRITICAL(&status_mux);
  runtime_status.network = state;
  portEXIT_CRITICAL(&status_mux);
}

void setWifiRssi(int16_t rssi) {
  portENTER_CRITICAL(&status_mux);
  runtime_status.wifi_rssi = rssi;
  portEXIT_CRITICAL(&status_mux);
}

void publishKeeperStatus(const KeeperRuntimeStatus &status) {
  portENTER_CRITICAL(&status_mux);
  runtime_status.keeper = status;
  portEXIT_CRITICAL(&status_mux);
}

void beginFetchStatus() {
  portENTER_CRITICAL(&status_mux);
  runtime_status.fetch = FetchState::Fetching;
  runtime_status.last_error = WeatherError::None;
  ++runtime_status.fetch_attempts;
  portEXIT_CRITICAL(&status_mux);
}

void finishFetchStatus(bool success, WeatherError error,
                       int64_t success_epoch = 0) {
  portENTER_CRITICAL(&status_mux);
  runtime_status.fetch = success ? FetchState::Fresh : FetchState::Failed;
  runtime_status.last_error = error;
  if (success) {
    ++runtime_status.fetch_successes;
    runtime_status.last_success_epoch = success_epoch;
  }
  portEXIT_CRITICAL(&status_mux);
}

void noteCachedSnapshot(int64_t fetched_epoch) {
  portENTER_CRITICAL(&status_mux);
  runtime_status.fetch = FetchState::Cached;
  runtime_status.last_success_epoch = fetched_epoch;
  portEXIT_CRITICAL(&status_mux);
}

bool readNumber(JsonVariantConst value, float minimum, float maximum,
                float &result) {
  if (value.isNull()) {
    return false;
  }
  const float parsed = value.as<float>();
  if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
    return false;
  }
  result = parsed;
  return true;
}

bool readInteger(JsonVariantConst value, int minimum, int maximum,
                 int &result) {
  if (value.isNull()) {
    return false;
  }
  const int parsed = value.as<int>();
  if (parsed < minimum || parsed > maximum) {
    return false;
  }
  result = parsed;
  return true;
}

bool copyFixedString(JsonVariantConst value, char *destination,
                     size_t destination_size, size_t expected_length) {
  const char *source = value.as<const char *>();
  if (source == nullptr || std::strlen(source) != expected_length ||
      destination_size <= expected_length) {
    return false;
  }
  std::memcpy(destination, source, expected_length);
  destination[expected_length] = '\0';
  return true;
}

bool copyHour(JsonVariantConst value, char destination[3]) {
  const char *source = value.as<const char *>();
  if (source == nullptr || std::strlen(source) < 13 || source[10] != 'T' ||
      source[11] < '0' || source[11] > '2' || source[12] < '0' ||
      source[12] > '9') {
    return false;
  }
  destination[0] = source[11];
  destination[1] = source[12];
  destination[2] = '\0';
  return true;
}

bool copyClockPart(JsonVariantConst value, char destination[6]) {
  const char *source = value.as<const char *>();
  if (source == nullptr || std::strlen(source) < 16 || source[10] != 'T') {
    return false;
  }
  std::memcpy(destination, source + 11, 5);
  destination[5] = '\0';
  return true;
}

void buildJsonFilter() {
  json_filter.clear();
  JsonObject current = json_filter["current"].to<JsonObject>();
  current["time"] = true;
  current["temperature_2m"] = true;
  current["relative_humidity_2m"] = true;
  current["apparent_temperature"] = true;
  current["weather_code"] = true;
  current["wind_speed_10m"] = true;
  current["pressure_msl"] = true;
  current["is_day"] = true;
  current["uv_index"] = true;

  JsonObject hourly = json_filter["hourly"].to<JsonObject>();
  hourly["time"] = true;
  hourly["temperature_2m"] = true;
  hourly["precipitation_probability"] = true;
  hourly["weather_code"] = true;

  JsonObject daily = json_filter["daily"].to<JsonObject>();
  daily["time"] = true;
  daily["temperature_2m_min"] = true;
  daily["temperature_2m_max"] = true;
  daily["precipitation_probability_max"] = true;
  daily["weather_code"] = true;
  daily["sunrise"] = true;
  daily["sunset"] = true;
  daily["uv_index_max"] = true;
}

bool parseWeatherDocument(WeatherSnapshot &snapshot) {
  std::memset(&snapshot, 0, sizeof(snapshot));
  snapshot.magic = SNAPSHOT_MAGIC;
  snapshot.schema = SNAPSHOT_SCHEMA;
  snapshot.struct_size = sizeof(snapshot);
  snapshot.fetched_epoch = static_cast<int64_t>(time(nullptr));

  JsonObjectConst current = json_document["current"].as<JsonObjectConst>();
  JsonObjectConst hourly = json_document["hourly"].as<JsonObjectConst>();
  JsonObjectConst daily = json_document["daily"].as<JsonObjectConst>();
  if (current.isNull() || hourly.isNull() || daily.isNull()) {
    return false;
  }

  float temperature = 0.0F;
  float apparent = 0.0F;
  float humidity = 0.0F;
  float wind = 0.0F;
  float pressure = 0.0F;
  float uv_index = 0.0F;
  int weather_code = 0;
  int is_day = 0;
  if (!copyFixedString(current["time"], snapshot.source_time,
                       sizeof(snapshot.source_time), 16) ||
      !readNumber(current["temperature_2m"], -80.0F, 70.0F, temperature) ||
      !readNumber(current["apparent_temperature"], -100.0F, 90.0F,
                  apparent) ||
      !readNumber(current["relative_humidity_2m"], 0.0F, 100.0F,
                  humidity) ||
      !readNumber(current["wind_speed_10m"], 0.0F, 300.0F, wind) ||
      !readNumber(current["pressure_msl"], 800.0F, 1200.0F, pressure) ||
      !readNumber(current["uv_index"], 0.0F, 40.0F, uv_index) ||
      !readInteger(current["weather_code"], 0, 99, weather_code) ||
      !readInteger(current["is_day"], 0, 1, is_day)) {
    return false;
  }
  snapshot.temperature_tenths = static_cast<int16_t>(std::lround(temperature * 10.0F));
  snapshot.apparent_temperature_tenths =
      static_cast<int16_t>(std::lround(apparent * 10.0F));
  snapshot.humidity = static_cast<uint8_t>(std::lround(humidity));
  snapshot.wind_speed_tenths =
      static_cast<uint16_t>(std::lround(wind * 10.0F));
  snapshot.pressure_tenths =
      static_cast<uint16_t>(std::lround(pressure * 10.0F));
  snapshot.uv_index_tenths =
      static_cast<uint16_t>(std::lround(uv_index * 10.0F));
  snapshot.weather_code = static_cast<uint8_t>(weather_code);
  snapshot.is_day = static_cast<uint8_t>(is_day);

  JsonArrayConst hourly_times = hourly["time"].as<JsonArrayConst>();
  JsonArrayConst hourly_temperatures =
      hourly["temperature_2m"].as<JsonArrayConst>();
  JsonArrayConst hourly_rain =
      hourly["precipitation_probability"].as<JsonArrayConst>();
  JsonArrayConst hourly_codes = hourly["weather_code"].as<JsonArrayConst>();
  if (hourly_times.size() < weather_config::HOURLY_POINTS ||
      hourly_temperatures.size() < weather_config::HOURLY_POINTS ||
      hourly_rain.size() < weather_config::HOURLY_POINTS ||
      hourly_codes.size() < weather_config::HOURLY_POINTS) {
    return false;
  }

  for (size_t index = 0; index < weather_config::HOURLY_POINTS; ++index) {
    HourlyPoint &point = snapshot.hourly[index];
    float point_temperature = 0.0F;
    int point_rain = 0;
    int point_code = 0;
    if (!copyHour(hourly_times[index], point.hour) ||
        !readNumber(hourly_temperatures[index], -80.0F, 70.0F,
                    point_temperature) ||
        !readInteger(hourly_rain[index], 0, 100, point_rain) ||
        !readInteger(hourly_codes[index], 0, 99, point_code)) {
      return false;
    }
    point.temperature_tenths =
        static_cast<int16_t>(std::lround(point_temperature * 10.0F));
    point.rain_probability = static_cast<uint8_t>(point_rain);
    point.weather_code = static_cast<uint8_t>(point_code);
  }

  JsonArrayConst daily_dates = daily["time"].as<JsonArrayConst>();
  JsonArrayConst daily_minimums =
      daily["temperature_2m_min"].as<JsonArrayConst>();
  JsonArrayConst daily_maximums =
      daily["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst daily_rain =
      daily["precipitation_probability_max"].as<JsonArrayConst>();
  JsonArrayConst daily_codes = daily["weather_code"].as<JsonArrayConst>();
  JsonArrayConst daily_sunrise = daily["sunrise"].as<JsonArrayConst>();
  JsonArrayConst daily_sunset = daily["sunset"].as<JsonArrayConst>();
  JsonArrayConst daily_uv = daily["uv_index_max"].as<JsonArrayConst>();
  if (daily_dates.size() < weather_config::DAILY_POINTS ||
      daily_minimums.size() < weather_config::DAILY_POINTS ||
      daily_maximums.size() < weather_config::DAILY_POINTS ||
      daily_rain.size() < weather_config::DAILY_POINTS ||
      daily_codes.size() < weather_config::DAILY_POINTS ||
      daily_sunrise.size() < weather_config::DAILY_POINTS ||
      daily_sunset.size() < weather_config::DAILY_POINTS ||
      daily_uv.size() < weather_config::DAILY_POINTS) {
    return false;
  }

  for (size_t index = 0; index < weather_config::DAILY_POINTS; ++index) {
    DailyPoint &point = snapshot.daily[index];
    float minimum = 0.0F;
    float maximum = 0.0F;
    float uv_maximum = 0.0F;
    int rain = 0;
    int code = 0;
    if (!copyFixedString(daily_dates[index], point.date, sizeof(point.date),
                         10) ||
        !copyClockPart(daily_sunrise[index], point.sunrise) ||
        !copyClockPart(daily_sunset[index], point.sunset) ||
        !readNumber(daily_minimums[index], -80.0F, 70.0F, minimum) ||
        !readNumber(daily_maximums[index], -80.0F, 70.0F, maximum) ||
        !readNumber(daily_uv[index], 0.0F, 40.0F, uv_maximum) ||
        minimum > maximum ||
        !readInteger(daily_rain[index], 0, 100, rain) ||
        !readInteger(daily_codes[index], 0, 99, code)) {
      return false;
    }
    point.minimum_tenths =
        static_cast<int16_t>(std::lround(minimum * 10.0F));
    point.maximum_tenths =
        static_cast<int16_t>(std::lround(maximum * 10.0F));
    point.uv_index_max_tenths =
        static_cast<uint16_t>(std::lround(uv_maximum * 10.0F));
    point.rain_probability = static_cast<uint8_t>(rain);
    point.weather_code = static_cast<uint8_t>(code);
  }

  if (!rangesAreValid(snapshot)) {
    return false;
  }
  snapshot.checksum = calculateChecksum(snapshot);
  return true;
}

bool fetchWeather(WeatherSnapshot &snapshot, WeatherError &error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = WeatherError::Wifi;
    return false;
  }
  if (!timeIsPlausible()) {
    error = WeatherError::Clock;
    return false;
  }

  WiFiClientSecure secure_client;
  secure_client.setCACert(ISRG_ROOT_X1);
  secure_client.setHandshakeTimeout(
      weather_config::HTTP_CONNECT_TIMEOUT_MS / 1000UL);

  HTTPClient request;
  request.useHTTP10(true);
  request.setConnectTimeout(weather_config::HTTP_CONNECT_TIMEOUT_MS);
  request.setTimeout(weather_config::HTTP_TIMEOUT_MS);
  request.setUserAgent("ESP32-Wetteruhr/1.0");
  if (!request.begin(secure_client, weather_config::WEATHER_URL)) {
    error = WeatherError::Tls;
    return false;
  }
  request.addHeader("Accept-Encoding", "identity");

  const int status = request.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Wetterabruf: HTTP %d (%s)\n", status,
                  status < 0 ? HTTPClient::errorToString(status).c_str() : "");
    request.end();
    error = status < 0 ? WeatherError::Tls : WeatherError::Http;
    return false;
  }

  const int announced_size = request.getSize();
  if (announced_size > static_cast<int>(MAX_RESPONSE_BYTES)) {
    request.end();
    error = WeatherError::PayloadTooLarge;
    return false;
  }

  buildJsonFilter();
  json_document.clear();
  const DeserializationError json_error = deserializeJson(
      json_document, request.getStream(),
      DeserializationOption::Filter(json_filter),
      DeserializationOption::NestingLimit(8));
  request.end();
  if (json_error) {
    Serial.printf("Wetterabruf: JSON-Fehler %s\n", json_error.c_str());
    error = WeatherError::Json;
    return false;
  }

  if (!parseWeatherDocument(snapshot)) {
    error = WeatherError::InvalidData;
    return false;
  }
  error = WeatherError::None;
  return true;
}

bool loadCachedSnapshot(WeatherSnapshot &snapshot) {
  Preferences preferences;
  if (!preferences.begin(CACHE_NAMESPACE, true)) {
    return false;
  }
  const size_t length = preferences.getBytesLength(CACHE_KEY);
  const size_t read =
      length == sizeof(snapshot)
          ? preferences.getBytes(CACHE_KEY, &snapshot, sizeof(snapshot))
          : 0;
  preferences.end();
  if (read != sizeof(snapshot) || !weatherSnapshotValid(snapshot)) {
    return false;
  }
  last_cache_epoch = snapshot.fetched_epoch;
  return true;
}

void cacheSnapshotIfDue(const WeatherSnapshot &snapshot) {
  if (last_cache_epoch > 0 &&
      snapshot.fetched_epoch - last_cache_epoch <
          weather_config::CACHE_WRITE_INTERVAL_SECONDS) {
    return;
  }
  Preferences preferences;
  if (!preferences.begin(CACHE_NAMESPACE, false)) {
    return;
  }
  const size_t written =
      preferences.putBytes(CACHE_KEY, &snapshot, sizeof(snapshot));
  preferences.end();
  if (written == sizeof(snapshot)) {
    last_cache_epoch = snapshot.fetched_epoch;
  }
}

void networkTask(void *) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(weather_config::HOSTNAME);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);

  KeeperRuntimeStatus keeper{};
  initializeInternetKeeperStatus(keeper);
  publishKeeperStatus(keeper);

  uint32_t next_keeper_check = 0;
  uint32_t next_wifi_attempt = 0;
  uint32_t next_fetch = 0;
  uint32_t retry_delay = weather_config::WEATHER_RETRY_MIN_MS;
  bool ntp_started = false;

  for (;;) {
    const bool wifi_connected = WiFi.status() == WL_CONNECTED;
    const bool manual_keeper =
        keeper_check_requested.exchange(false, std::memory_order_relaxed);
    const bool reconnect_due =
        !wifi_connected && millisReached(next_wifi_attempt);
    if (manual_keeper || millisReached(next_keeper_check) || reconnect_due) {
      setNetworkState(NetworkState::Connecting);
      const bool was_online = keeper.internet == InternetState::Online;
      performInternetKeeperCycle(keeper, publishKeeperStatus);
      const bool connected_after_cycle = WiFi.status() == WL_CONNECTED;
      next_keeper_check =
          millis() + (connected_after_cycle
                          ? weather_config::KEEPER_CHECK_INTERVAL_MS
                          : weather_config::WIFI_RETRY_MS);
      next_wifi_attempt = millis() + weather_config::WIFI_RETRY_MS;
      keeper.next_check_due_ms = next_keeper_check;
      publishKeeperStatus(keeper);
      if (!was_online && keeper.internet == InternetState::Online) {
        next_fetch = 0;
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      setNetworkState(NetworkState::Disconnected);
      setWifiRssi(0);
      if (keeper.wifi_connected) {
        keeper.wifi_connected = false;
        keeper.wifi_rssi = 0;
        keeper.internet = InternetState::WifiDown;
        keeper.phase = KeeperPhase::Idle;
        std::snprintf(keeper.last_event, sizeof(keeper.last_event),
                      "WLAN-Verbindung verloren");
        publishKeeperStatus(keeper);
      }
    } else if (keeper.internet != InternetState::Online) {
      setNetworkState(NetworkState::NoInternet);
      setWifiRssi(static_cast<int16_t>(WiFi.RSSI()));
    } else {
      setWifiRssi(static_cast<int16_t>(WiFi.RSSI()));
      if (!ntp_started) {
        configTzTime(weather_config::TIMEZONE, weather_config::NTP_SERVER_1,
                     weather_config::NTP_SERVER_2);
        ntp_started = true;
        Serial.println("NTP-Zeitsynchronisation gestartet");
      }
      if (!timeIsPlausible()) {
        setNetworkState(NetworkState::SyncingTime);
      } else {
        setNetworkState(NetworkState::Online);
        if (manual_refresh_requested.exchange(false,
                                              std::memory_order_relaxed) ||
            millisReached(next_fetch)) {
          beginFetchStatus();
          WeatherSnapshot candidate{};
          WeatherError error = WeatherError::None;
          if (fetchWeather(candidate, error)) {
            xQueueOverwrite(snapshot_queue, &candidate);
            cacheSnapshotIfDue(candidate);
            finishFetchStatus(true, WeatherError::None,
                              candidate.fetched_epoch);
            next_fetch = millis() + weather_config::WEATHER_REFRESH_MS;
            retry_delay = weather_config::WEATHER_RETRY_MIN_MS;
            Serial.printf("Wetter aktualisiert; freier Heap: %u Bytes\n",
                          ESP.getFreeHeap());
          } else {
            finishFetchStatus(false, error);
            next_fetch = millis() + retry_delay;
            retry_delay = std::min<uint32_t>(
                retry_delay * 2U, weather_config::WEATHER_RETRY_MAX_MS);
          }
        }
      }
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  }
}

}  // namespace

bool weatherSnapshotValid(const WeatherSnapshot &snapshot) {
  return snapshot.magic == SNAPSHOT_MAGIC &&
         snapshot.schema == SNAPSHOT_SCHEMA &&
         snapshot.struct_size == sizeof(snapshot) &&
         snapshot.fetched_epoch >= 1704067200 && rangesAreValid(snapshot) &&
         snapshot.checksum == calculateChecksum(snapshot);
}

bool startWeatherService() {
  if (snapshot_queue != nullptr) {
    return true;
  }
  snapshot_queue = xQueueCreate(1, sizeof(WeatherSnapshot));
  if (snapshot_queue == nullptr) {
    return false;
  }

  KeeperRuntimeStatus initial_keeper{};
  initializeInternetKeeperStatus(initial_keeper);
  publishKeeperStatus(initial_keeper);

  WeatherSnapshot cached{};
  if (loadCachedSnapshot(cached)) {
    xQueueOverwrite(snapshot_queue, &cached);
    noteCachedSnapshot(cached.fetched_epoch);
  }

  if (!internetKeeperConfigured()) {
    setNetworkState(NetworkState::Unconfigured);
    return true;
  }
  setNetworkState(NetworkState::Disconnected);
  return xTaskCreatePinnedToCore(networkTask, "weather-net", 9216, nullptr, 1,
                                 &network_task_handle, 0) == pdPASS;
}

bool receiveWeatherSnapshot(WeatherSnapshot &snapshot) {
  return snapshot_queue != nullptr &&
         xQueueReceive(snapshot_queue, &snapshot, 0) == pdTRUE;
}

RuntimeStatus getRuntimeStatus() {
  portENTER_CRITICAL(&status_mux);
  const RuntimeStatus copy = runtime_status;
  portEXIT_CRITICAL(&status_mux);
  return copy;
}

void requestWeatherRefresh() {
  manual_refresh_requested.store(true, std::memory_order_relaxed);
  if (network_task_handle != nullptr) {
    xTaskNotifyGive(network_task_handle);
  }
}

void requestInternetKeeperCheck() {
  keeper_check_requested.store(true, std::memory_order_relaxed);
  if (network_task_handle != nullptr) {
    xTaskNotifyGive(network_task_handle);
  }
}

bool weatherServiceConfigured() { return internetKeeperConfigured(); }

}  // namespace weatherclock
