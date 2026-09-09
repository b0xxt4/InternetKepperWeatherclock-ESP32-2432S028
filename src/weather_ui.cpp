#include "weather_ui.h"

#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <time.h>

#include "config.h"

namespace weatherclock {
namespace {

constexpr uint16_t COLOR_BACKGROUND = 0x0842;
constexpr uint16_t COLOR_HEADER = 0x1084;
constexpr uint16_t COLOR_CARD = 0x18E6;
constexpr uint16_t COLOR_CARD_SELECTED = 0x2AA9;
constexpr uint16_t COLOR_ACCENT = 0x5DFF;
constexpr uint16_t COLOR_MUTED = 0x9CD3;
constexpr uint16_t COLOR_SUN = 0xFDC0;
constexpr uint16_t COLOR_CLOUD = 0xC618;
constexpr uint16_t COLOR_RAIN = 0x3D7F;
constexpr uint16_t COLOR_SNOW = 0xDFFF;
constexpr uint16_t COLOR_GOOD = 0x47E0;
constexpr uint16_t COLOR_WARNING = 0xFD20;
constexpr uint16_t COLOR_ERROR = 0xF986;
constexpr uint16_t COLOR_UV_MODERATE = 0xFFE0;
constexpr uint16_t COLOR_UV_HIGH = 0xFC00;
constexpr uint16_t COLOR_UV_VERY_HIGH = 0xF800;
constexpr uint16_t COLOR_UV_EXTREME = 0xF81F;

constexpr int16_t HEADER_HEIGHT = 56;
constexpr int16_t ATTRIBUTION_Y = 207;
constexpr int16_t NAVIGATION_Y = 217;
constexpr int16_t NAVIGATION_HEIGHT = 23;
constexpr uint8_t PAGE_COUNT = 5;
constexpr int16_t NAVIGATION_ITEM_WIDTH =
    weather_config::DISPLAY_WIDTH / PAGE_COUNT;
constexpr int16_t KEEPER_BUTTON_X = 190;
constexpr int16_t KEEPER_BUTTON_Y = 136;
constexpr int16_t KEEPER_BUTTON_WIDTH = 125;
constexpr int16_t KEEPER_BUTTON_HEIGHT = 66;
constexpr int16_t CALIBRATION_MARGIN = 24;
constexpr uint32_t TOUCH_CALIBRATION_MAGIC = 0x5443414CUL;  // "TCAL"

enum class Page : uint8_t { Current, Hourly, Daily, Uv, Network };
enum class BrightnessMode : uint8_t { Automatic, Bright, Dim };

static_assert(weather_config::DISPLAY_WIDTH % PAGE_COUNT == 0,
              "Navigation tabs must divide the display width evenly");
static_assert(static_cast<uint8_t>(Page::Network) + 1 == PAGE_COUNT,
              "Page enum and navigation labels must stay in sync");

struct RawPoint {
  int16_t x;
  int16_t y;
};

struct TouchCalibration {
  uint32_t magic;
  uint8_t x_source;
  uint8_t y_source;
  int16_t x_left;
  int16_t x_right;
  int16_t y_top;
  int16_t y_bottom;
  uint32_t checksum;
};

TFT_eSPI display;
SPIClass touch_spi(VSPI);
XPT2046_Touchscreen touchscreen(weather_config::TOUCH_CS,
                                 weather_config::TOUCH_IRQ);
TouchCalibration touch_calibration{};
Page current_page = Page::Current;
BrightnessMode brightness_mode = BrightnessMode::Automatic;
bool touch_was_down = false;
bool full_redraw_requested = true;
bool keeper_check_requested = false;
RuntimeStatus previous_status{};
bool previous_status_valid = false;
int64_t previous_clock_minute = -1;
uint32_t previous_countdown_second = UINT32_MAX;
uint8_t current_backlight = 0;

uint32_t calibrationChecksum(const TouchCalibration &calibration) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&calibration);
  constexpr size_t LENGTH = offsetof(TouchCalibration, checksum);
  uint32_t value = 2166136261UL;
  for (size_t index = 0; index < LENGTH; ++index) {
    value ^= bytes[index];
    value *= 16777619UL;
  }
  return value;
}

bool calibrationValid(const TouchCalibration &calibration) {
  return calibration.magic == TOUCH_CALIBRATION_MAGIC &&
         calibration.x_source <= 1 && calibration.y_source <= 1 &&
         calibration.x_source != calibration.y_source &&
         std::abs(calibration.x_right - calibration.x_left) > 500 &&
         std::abs(calibration.y_bottom - calibration.y_top) > 500 &&
         calibration.checksum == calibrationChecksum(calibration);
}

bool loadCalibration() {
  Preferences preferences;
  if (!preferences.begin("wetterui", true)) {
    return false;
  }
  const size_t length = preferences.getBytesLength("touch");
  const size_t read =
      length == sizeof(touch_calibration)
          ? preferences.getBytes("touch", &touch_calibration,
                                 sizeof(touch_calibration))
          : 0;
  preferences.end();
  return read == sizeof(touch_calibration) &&
         calibrationValid(touch_calibration);
}

void saveCalibration() {
  touch_calibration.magic = TOUCH_CALIBRATION_MAGIC;
  touch_calibration.checksum = calibrationChecksum(touch_calibration);
  Preferences preferences;
  if (preferences.begin("wetterui", false)) {
    preferences.putBytes("touch", &touch_calibration,
                         sizeof(touch_calibration));
    preferences.end();
  }
}

void setBacklight(uint8_t value) {
  if (value == current_backlight) {
    return;
  }
  ledcWrite(weather_config::BACKLIGHT_PWM_CHANNEL, value);
  current_backlight = value;
}

void applyBrightness() {
  if (brightness_mode == BrightnessMode::Bright) {
    setBacklight(255);
    return;
  }
  if (brightness_mode == BrightnessMode::Dim) {
    setBacklight(weather_config::BACKLIGHT_NIGHT);
    return;
  }

  const time_t now = time(nullptr);
  if (now < 1704067200) {
    setBacklight(weather_config::BACKLIGHT_DAY);
    return;
  }
  struct tm local_time {};
  localtime_r(&now, &local_time);
  const bool night = local_time.tm_hour < 7 || local_time.tm_hour >= 22;
  setBacklight(night ? weather_config::BACKLIGHT_NIGHT
                     : weather_config::BACKLIGHT_DAY);
}

void drawCross(int16_t x, int16_t y, uint16_t color) {
  display.drawCircle(x, y, 9, color);
  display.drawFastHLine(x - 14, y, 29, color);
  display.drawFastVLine(x, y - 14, 29, color);
}

void waitForTouchRelease() {
  while (touchscreen.touched()) {
    delay(15);
  }
  delay(120);
}

RawPoint captureRawPoint(int16_t target_x, int16_t target_y,
                         uint8_t point_number) {
  for (;;) {
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.drawString("Touch-Kalibrierung", 160, 103, 4);
    char instruction[40];
    std::snprintf(instruction, sizeof(instruction),
                  "Punkt %u/4 genau beruehren", point_number);
    display.drawString(instruction, 160, 132, 2);
    drawCross(target_x, target_y, TFT_YELLOW);

    while (!touchscreen.touched()) {
      delay(10);
    }
    delay(80);

    int32_t x_sum = 0;
    int32_t y_sum = 0;
    uint8_t samples = 0;
    while (touchscreen.touched() && samples < 20) {
      const TS_Point point = touchscreen.getPoint();
      if (point.z > 100) {
        x_sum += point.x;
        y_sum += point.y;
        ++samples;
      }
      delay(8);
    }
    waitForTouchRelease();
    if (samples >= 5) {
      return {static_cast<int16_t>(x_sum / samples),
              static_cast<int16_t>(y_sum / samples)};
    }

    display.setTextColor(TFT_RED, TFT_BLACK);
    display.drawString("Bitte noch einmal", 160, 190, 2);
    delay(800);
  }
}

int16_t rawAxis(const RawPoint &point, uint8_t source) {
  return source == 0 ? point.x : point.y;
}

void calibrateTouch() {
  const RawPoint points[4] = {
      captureRawPoint(CALIBRATION_MARGIN, CALIBRATION_MARGIN, 1),
      captureRawPoint(weather_config::DISPLAY_WIDTH - CALIBRATION_MARGIN - 1,
                      CALIBRATION_MARGIN, 2),
      captureRawPoint(weather_config::DISPLAY_WIDTH - CALIBRATION_MARGIN - 1,
                      weather_config::DISPLAY_HEIGHT - CALIBRATION_MARGIN - 1,
                      3),
      captureRawPoint(CALIBRATION_MARGIN,
                      weather_config::DISPLAY_HEIGHT - CALIBRATION_MARGIN - 1,
                      4),
  };

  const int32_t horizontal_x =
      std::abs(((points[1].x + points[2].x) / 2) -
               ((points[0].x + points[3].x) / 2));
  const int32_t horizontal_y =
      std::abs(((points[1].y + points[2].y) / 2) -
               ((points[0].y + points[3].y) / 2));
  touch_calibration.x_source = horizontal_x >= horizontal_y ? 0 : 1;
  touch_calibration.y_source = touch_calibration.x_source == 0 ? 1 : 0;

  touch_calibration.x_left = static_cast<int16_t>(
      (rawAxis(points[0], touch_calibration.x_source) +
       rawAxis(points[3], touch_calibration.x_source)) /
      2);
  touch_calibration.x_right = static_cast<int16_t>(
      (rawAxis(points[1], touch_calibration.x_source) +
       rawAxis(points[2], touch_calibration.x_source)) /
      2);
  touch_calibration.y_top = static_cast<int16_t>(
      (rawAxis(points[0], touch_calibration.y_source) +
       rawAxis(points[1], touch_calibration.y_source)) /
      2);
  touch_calibration.y_bottom = static_cast<int16_t>(
      (rawAxis(points[2], touch_calibration.y_source) +
       rawAxis(points[3], touch_calibration.y_source)) /
      2);

  if (touch_calibration.x_source == touch_calibration.y_source ||
      std::abs(touch_calibration.x_right - touch_calibration.x_left) <= 500 ||
      std::abs(touch_calibration.y_bottom - touch_calibration.y_top) <= 500) {
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED, TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.drawString("Kalibrierung fehlgeschlagen", 160, 105, 2);
    delay(1200);
    calibrateTouch();
    return;
  }
  saveCalibration();

  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_GREEN, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.drawString("Touch gespeichert", 160, 105, 4);
  delay(800);
}

int16_t mapCalibratedAxis(int16_t raw, int16_t raw_start, int16_t raw_end,
                          int16_t screen_start, int16_t screen_end) {
  if (raw_start == raw_end) {
    return screen_start;
  }
  const int32_t mapped =
      screen_start +
      (static_cast<int32_t>(raw - raw_start) * (screen_end - screen_start)) /
          (raw_end - raw_start);
  return static_cast<int16_t>(mapped);
}

bool mapTouchPoint(const TS_Point &raw, int16_t &x, int16_t &y) {
  if (!calibrationValid(touch_calibration)) {
    return false;
  }
  const RawPoint point{static_cast<int16_t>(raw.x),
                       static_cast<int16_t>(raw.y)};
  x = mapCalibratedAxis(rawAxis(point, touch_calibration.x_source),
                        touch_calibration.x_left, touch_calibration.x_right,
                        CALIBRATION_MARGIN,
                        weather_config::DISPLAY_WIDTH -
                            CALIBRATION_MARGIN - 1);
  y = mapCalibratedAxis(rawAxis(point, touch_calibration.y_source),
                        touch_calibration.y_top, touch_calibration.y_bottom,
                        CALIBRATION_MARGIN,
                        weather_config::DISPLAY_HEIGHT -
                            CALIBRATION_MARGIN - 1);
  x = std::max<int16_t>(0, std::min<int16_t>(x,
                                             weather_config::DISPLAY_WIDTH - 1));
  y = std::max<int16_t>(0, std::min<int16_t>(
                                    y, weather_config::DISPLAY_HEIGHT - 1));
  return true;
}

const char *weatherDescription(uint8_t code) {
  if (code == 0) return "Klar";
  if (code <= 2) return "Leicht bewoelkt";
  if (code == 3) return "Bedeckt";
  if (code == 45 || code == 48) return "Nebel";
  if (code >= 51 && code <= 57) return "Nieselregen";
  if (code >= 61 && code <= 67) return "Regen";
  if (code >= 71 && code <= 77) return "Schnee";
  if (code >= 80 && code <= 82) return "Regenschauer";
  if (code >= 85 && code <= 86) return "Schneeschauer";
  if (code >= 95) return "Gewitter";
  return "Wetterwechsel";
}

bool isCloudCode(uint8_t code) { return code >= 1 && code <= 3; }
bool isFogCode(uint8_t code) { return code == 45 || code == 48; }
bool isRainCode(uint8_t code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
}
bool isSnowCode(uint8_t code) {
  return (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
}

void drawSun(int16_t x, int16_t y, int16_t radius) {
  display.fillCircle(x, y, radius, COLOR_SUN);
  for (uint8_t ray = 0; ray < 8; ++ray) {
    const float angle = ray * 0.785398F;
    const int16_t x1 = x + static_cast<int16_t>(std::cos(angle) * (radius + 3));
    const int16_t y1 = y + static_cast<int16_t>(std::sin(angle) * (radius + 3));
    const int16_t x2 = x + static_cast<int16_t>(std::cos(angle) * (radius + 8));
    const int16_t y2 = y + static_cast<int16_t>(std::sin(angle) * (radius + 8));
    display.drawLine(x1, y1, x2, y2, COLOR_SUN);
  }
}

void drawMoon(int16_t x, int16_t y, int16_t radius, uint16_t background) {
  display.fillCircle(x, y, radius, COLOR_SNOW);
  display.fillCircle(x + radius / 2, y - radius / 3, radius, background);
}

void drawCloud(int16_t x, int16_t y, int16_t size) {
  const int16_t radius = std::max<int16_t>(4, size / 5);
  display.fillCircle(x - radius, y, radius, COLOR_CLOUD);
  display.fillCircle(x, y - radius / 2, radius + 2, COLOR_CLOUD);
  display.fillCircle(x + radius, y, radius, COLOR_CLOUD);
  display.fillRoundRect(x - size / 2, y, size, radius + 5, 4, COLOR_CLOUD);
}

void drawWeatherIcon(uint8_t code, int16_t x, int16_t y, int16_t size,
                     bool is_day = true,
                     uint16_t background = COLOR_BACKGROUND) {
  const int16_t radius = std::max<int16_t>(4, size / 5);
  if (code == 0) {
    if (is_day) {
      drawSun(x, y, radius);
    } else {
      drawMoon(x, y, radius, background);
    }
    return;
  }
  if (code == 1 || code == 2) {
    if (is_day) {
      drawSun(x - size / 4, y - size / 4,
              std::max<int16_t>(3, radius - 2));
    } else {
      drawMoon(x - size / 4, y - size / 4,
               std::max<int16_t>(3, radius - 2), background);
    }
    drawCloud(x + size / 8, y + size / 8, size);
    return;
  }
  if (isFogCode(code)) {
    for (int8_t offset = -2; offset <= 2; ++offset) {
      display.drawFastHLine(x - size / 2 + (offset & 1 ? 5 : 0),
                            y + offset * 5, size - (offset & 1 ? 5 : 0),
                            COLOR_CLOUD);
    }
    return;
  }

  drawCloud(x, y - 4, size);
  if (isRainCode(code)) {
    for (int8_t offset = -1; offset <= 1; ++offset) {
      display.drawLine(x + offset * 10, y + 10, x + offset * 10 - 3,
                       y + 18, COLOR_RAIN);
    }
  } else if (isSnowCode(code)) {
    for (int8_t offset = -1; offset <= 1; ++offset) {
      const int16_t snow_x = x + offset * 10;
      display.drawFastHLine(snow_x - 3, y + 14, 7, COLOR_SNOW);
      display.drawFastVLine(snow_x, y + 11, 7, COLOR_SNOW);
    }
  } else if (code >= 95) {
    display.fillTriangle(x + 1, y + 8, x - 7, y + 20, x, y + 18,
                         TFT_YELLOW);
    display.fillTriangle(x, y + 18, x + 7, y + 16, x - 4, y + 28,
                         TFT_YELLOW);
  } else if (!isCloudCode(code)) {
    display.drawCircle(x, y, radius, COLOR_MUTED);
  }
}

void formatTemperature(int16_t tenths, char *buffer, size_t size,
                       bool decimal) {
  if (decimal) {
    std::snprintf(buffer, size, "%.1f", tenths / 10.0F);
  } else {
    std::snprintf(buffer, size, "%d", static_cast<int>(std::lround(
                                           tenths / 10.0F)));
  }
}

const char *weekdayName(int weekday) {
  static const char *const NAMES[] = {"So", "Mo", "Di", "Mi",
                                      "Do", "Fr", "Sa"};
  return weekday >= 0 && weekday < 7 ? NAMES[weekday] : "--";
}

const char *dayLabel(const DailyPoint &point, size_t index, char buffer[4]) {
  if (index == 0) return "Heute";
  if (index == 1) return "Morgen";

  struct tm date {};
  if (std::sscanf(point.date, "%d-%d-%d", &date.tm_year, &date.tm_mon,
                  &date.tm_mday) != 3) {
    return "--";
  }
  date.tm_year -= 1900;
  date.tm_mon -= 1;
  date.tm_hour = 12;
  date.tm_isdst = -1;
  mktime(&date);
  std::snprintf(buffer, 4, "%s", weekdayName(date.tm_wday));
  return buffer;
}

uint32_t dataAgeSeconds(const WeatherSnapshot *snapshot) {
  if (snapshot == nullptr || snapshot->fetched_epoch <= 0) {
    return UINT32_MAX;
  }
  const time_t now = time(nullptr);
  if (now < 1704067200 || now < snapshot->fetched_epoch) {
    return 0;
  }
  const int64_t age = static_cast<int64_t>(now) - snapshot->fetched_epoch;
  return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}

uint16_t statusColor(const WeatherSnapshot *snapshot,
                     const RuntimeStatus &status) {
  if (snapshot != nullptr) {
    if (status.fetch == FetchState::Cached && time(nullptr) < 1704067200) {
      return COLOR_WARNING;
    }
    const uint32_t age = dataAgeSeconds(snapshot);
    if (age < weather_config::DATA_STALE_SECONDS) return COLOR_GOOD;
    if (age < weather_config::DATA_EXPIRED_SECONDS) return COLOR_WARNING;
    return COLOR_ERROR;
  }
  if (status.fetch == FetchState::Fetching ||
      status.network == NetworkState::Connecting ||
      status.network == NetworkState::SyncingTime) {
    return COLOR_WARNING;
  }
  return COLOR_ERROR;
}

bool keeperBusy(KeeperPhase phase) {
  return phase != KeeperPhase::Unconfigured && phase != KeeperPhase::Idle;
}

uint16_t keeperStatusColor(const KeeperRuntimeStatus &status) {
  if (status.phase == KeeperPhase::Unconfigured) return COLOR_MUTED;
  if (keeperBusy(status.phase)) return COLOR_WARNING;

  switch (status.internet) {
    case InternetState::Online:
      return COLOR_GOOD;
    case InternetState::Unknown:
      return COLOR_WARNING;
    case InternetState::CaptivePortal:
    case InternetState::WifiDown:
    case InternetState::LoginFailed:
      return COLOR_ERROR;
  }
  return COLOR_ERROR;
}

const char *keeperHeaderLabel(const KeeperRuntimeStatus &status) {
  switch (status.phase) {
    case KeeperPhase::Unconfigured:
      return "NET ?";
    case KeeperPhase::SwitchingNetwork:
      return "WECHSEL";
    case KeeperPhase::ConnectingWifi:
      return "WLAN...";
    case KeeperPhase::CheckingInternet:
      return "PRUEFT";
    case KeeperPhase::BootstrappingClock:
      return "ZEIT...";
    case KeeperPhase::LoggingIn:
      return "LOGIN";
    case KeeperPhase::VerifyingLogin:
      return "TEST...";
    case KeeperPhase::Idle:
      break;
  }

  switch (status.internet) {
    case InternetState::Online:
      return "NET OK";
    case InternetState::CaptivePortal:
      return "PORTAL";
    case InternetState::WifiDown:
      return "WLAN AUS";
    case InternetState::LoginFailed:
      return "LOGIN!";
    case InternetState::Unknown:
      return "NET ?";
  }
  return "NET ?";
}

const char *weatherHeaderLabel(const WeatherSnapshot *snapshot,
                               const RuntimeStatus &status) {
  if (status.fetch == FetchState::Fetching) return "WET...";
  if (snapshot == nullptr) return "WET ?";
  if (status.fetch == FetchState::Cached && time(nullptr) < 1704067200) {
    return "WET ALT";
  }
  const uint32_t age = dataAgeSeconds(snapshot);
  if (age < weather_config::DATA_STALE_SECONDS) return "WET OK";
  if (age < weather_config::DATA_EXPIRED_SECONDS) return "WET ALT";
  return "WET!";
}

const char *keeperMainLabel(const KeeperRuntimeStatus &status) {
  switch (status.phase) {
    case KeeperPhase::Unconfigured:
      return "WLAN/RUB nicht eingerichtet";
    case KeeperPhase::SwitchingNetwork:
      return "Wechsle zum naechsten WLAN";
    case KeeperPhase::ConnectingWifi:
      return "WLAN wird verbunden";
    case KeeperPhase::CheckingInternet:
      return "Internetzugang wird geprueft";
    case KeeperPhase::BootstrappingClock:
      return "Uhrzeit wird vorbereitet";
    case KeeperPhase::LoggingIn:
      return "RUB-Login wird gesendet";
    case KeeperPhase::VerifyingLogin:
      return "Login wird bestaetigt";
    case KeeperPhase::Idle:
      break;
  }

  switch (status.internet) {
    case InternetState::Online:
      return "Internet erreichbar";
    case InternetState::CaptivePortal:
      return "RUB-Portal blockiert Internet";
    case InternetState::WifiDown:
      return "WLAN nicht erreichbar";
    case InternetState::LoginFailed:
      return "RUB-Login fehlgeschlagen";
    case InternetState::Unknown:
      return "Noch nicht geprueft";
  }
  return "Unbekannter Zustand";
}

const char *statusLabel(const WeatherSnapshot *snapshot,
                        const RuntimeStatus &status) {
  if (snapshot != nullptr) {
    if (status.fetch == FetchState::Cached && time(nullptr) < 1704067200) {
      return "Letzter Wetterstand";
    }
    const uint32_t age = dataAgeSeconds(snapshot);
    if (status.fetch == FetchState::Fetching) return "aktualisiert...";
    if (age < weather_config::DATA_STALE_SECONDS) return "Wetter aktuell";
    if (age < weather_config::DATA_EXPIRED_SECONDS) return "Wetterdaten alt";
    return "Wetterdaten sehr alt";
  }
  switch (status.network) {
    case NetworkState::Unconfigured:
      return "WLAN/RUB nicht eingerichtet";
    case NetworkState::Disconnected:
      return "WLAN getrennt";
    case NetworkState::Connecting:
      return "WLAN verbindet";
    case NetworkState::SyncingTime:
      return "Uhr wird gestellt";
    case NetworkState::Online:
      break;
    case NetworkState::NoInternet:
      return "Kein Internetzugang";
  }
  return status.fetch == FetchState::Fetching ? "Wetter wird geladen"
                                               : "Noch keine Wetterdaten";
}

void drawHeader(const WeatherSnapshot *snapshot,
                const RuntimeStatus &status) {
  display.fillRect(0, 0, weather_config::DISPLAY_WIDTH, HEADER_HEIGHT,
                   COLOR_HEADER);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_HEADER);

  const time_t now = time(nullptr);
  if (now >= 1704067200) {
    struct tm local_time {};
    localtime_r(&now, &local_time);
    char clock_text[6];
    char date_text[40];
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
                  local_time.tm_hour, local_time.tm_min);
    std::snprintf(date_text, sizeof(date_text), "%s, %02d.%02d.%04d",
                  weekdayName(local_time.tm_wday), local_time.tm_mday,
                  local_time.tm_mon + 1, local_time.tm_year + 1900);
    display.drawString(clock_text, 7, 2, 7);
    display.drawString(date_text, 169, 6, 2);
  } else {
    display.drawString("--:--", 7, 12, 4);
    display.drawString("Zeit noch nicht gesetzt", 169, 6, 2);
  }

  display.setTextColor(COLOR_MUTED, COLOR_HEADER);
  display.drawString(weather_config::LOCATION_DETAIL, 169, 25, 2);
  display.fillCircle(174, 46, 4, keeperStatusColor(status.keeper));
  display.setTextColor(TFT_WHITE, COLOR_HEADER);
  display.drawString(keeperHeaderLabel(status.keeper), 183, 40, 1);
  display.fillCircle(239, 46, 4, statusColor(snapshot, status));
  display.drawString(weatherHeaderLabel(snapshot, status), 248, 40, 1);

  char light_marker[4] = "A";
  if (brightness_mode == BrightnessMode::Bright) std::strcpy(light_marker, "+");
  if (brightness_mode == BrightnessMode::Dim) std::strcpy(light_marker, "-");
  display.setTextDatum(TR_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_HEADER);
  display.drawString(light_marker, 316, 40, 1);
}

void drawAttribution() {
  display.fillRect(0, ATTRIBUTION_Y, weather_config::DISPLAY_WIDTH, 10,
                   COLOR_BACKGROUND);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Wetterdaten: Open-Meteo.com", 160, ATTRIBUTION_Y + 4, 1);
}

void drawNavigation() {
  static const char *const LABELS[] = {"JETZT", "STD.", "TAGE", "UV",
                                      "NETZ"};
  static_assert(sizeof(LABELS) / sizeof(LABELS[0]) == PAGE_COUNT,
                "Page enum and navigation labels must stay in sync");
  for (uint8_t index = 0; index < PAGE_COUNT; ++index) {
    const int16_t x = index * NAVIGATION_ITEM_WIDTH;
    const bool selected = static_cast<uint8_t>(current_page) == index;
    const uint16_t background = selected ? COLOR_CARD_SELECTED : COLOR_CARD;
    display.fillRect(x, NAVIGATION_Y, NAVIGATION_ITEM_WIDTH - 1,
                     NAVIGATION_HEIGHT, background);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(selected ? TFT_WHITE : COLOR_MUTED, background);
    display.drawString(LABELS[index], x + NAVIGATION_ITEM_WIDTH / 2,
                       NAVIGATION_Y + 11, 2);
  }
}

const char *errorText(WeatherError error) {
  switch (error) {
    case WeatherError::None:
      return "Noch kein Abruf";
    case WeatherError::Wifi:
      return "WLAN nicht erreichbar";
    case WeatherError::Clock:
      return "Zeit fuer HTTPS fehlt";
    case WeatherError::Tls:
      return "HTTPS-Verbindung fehlgeschlagen";
    case WeatherError::Http:
      return "Wetterdienst antwortet nicht";
    case WeatherError::PayloadTooLarge:
      return "Wetterantwort zu gross";
    case WeatherError::Json:
      return "Wetterantwort unvollstaendig";
    case WeatherError::InvalidData:
      return "Wetterdaten ungueltig";
  }
  return "Unbekannter Fehler";
}

void drawNoData(const RuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  if (status.network == NetworkState::Unconfigured) {
    display.drawString("Zugang fehlt", 160, 100, 4);
    display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
    display.drawString("Im Ordner weatherclock:", 160, 136, 2);
    display.drawString("python3 configure.py", 160, 156, 2);
  } else if (status.fetch == FetchState::Fetching) {
    display.drawString("Wetter wird geladen ...", 160, 115, 4);
  } else {
    display.drawString(statusLabel(nullptr, status), 160, 105, 4);
    display.setTextColor(status.last_error == WeatherError::None
                             ? COLOR_MUTED
                             : COLOR_ERROR,
                         COLOR_BACKGROUND);
    display.drawString(errorText(status.last_error), 160, 144, 2);
  }
}

void drawMiniDay(const DailyPoint &point, size_t index, int16_t x) {
  constexpr int16_t WIDTH = 101;
  constexpr int16_t Y = 158;
  constexpr int16_t HEIGHT = 46;
  display.fillRoundRect(x, Y, WIDTH, HEIGHT, 5, COLOR_CARD);
  display.setTextDatum(TC_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_CARD);
  char day_buffer[4];
  display.drawString(dayLabel(point, index, day_buffer), x + WIDTH / 2, Y + 3,
                     1);

  char minimum[8];
  char maximum[8];
  char temperatures[20];
  formatTemperature(point.minimum_tenths, minimum, sizeof(minimum), false);
  formatTemperature(point.maximum_tenths, maximum, sizeof(maximum), false);
  std::snprintf(temperatures, sizeof(temperatures), "%s / %s C", maximum,
                minimum);
  display.setTextColor(TFT_WHITE, COLOR_CARD);
  display.drawString(temperatures, x + WIDTH / 2, Y + 14, 2);

  char rain[16];
  std::snprintf(rain, sizeof(rain), "Regen %u%%", point.rain_probability);
  display.setTextColor(COLOR_RAIN, COLOR_CARD);
  display.drawString(rain, x + WIDTH / 2, Y + 33, 1);
}

void drawCurrentPage(const WeatherSnapshot *snapshot,
                     const RuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  if (snapshot == nullptr) {
    drawNoData(status);
    return;
  }

  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  char temperature[12];
  formatTemperature(snapshot->temperature_tenths, temperature,
                    sizeof(temperature), true);
  display.drawString(temperature, 8, 62, 6);
  const int16_t temperature_width = display.textWidth(temperature, 6);
  display.drawCircle(14 + temperature_width, 68, 3, TFT_WHITE);
  display.drawString("C", 21 + temperature_width, 76, 4);

  display.setTextColor(COLOR_ACCENT, COLOR_BACKGROUND);
  display.drawString(weatherDescription(snapshot->weather_code), 9, 111, 4);
  drawWeatherIcon(snapshot->weather_code, 268, 91, 40,
                  snapshot->is_day != 0, COLOR_BACKGROUND);

  char apparent[8];
  formatTemperature(snapshot->apparent_temperature_tenths, apparent,
                    sizeof(apparent), false);
  const int16_t statistic_centers[] = {40, 120, 200, 280};
  char statistic[20];
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  std::snprintf(statistic, sizeof(statistic), "Gef. %s C", apparent);
  display.drawString(statistic, statistic_centers[0], 143, 1);
  std::snprintf(statistic, sizeof(statistic), "Feu. %u%%", snapshot->humidity);
  display.drawString(statistic, statistic_centers[1], 143, 1);
  std::snprintf(statistic, sizeof(statistic), "Wind %.0f",
                snapshot->wind_speed_tenths / 10.0F);
  display.drawString(statistic, statistic_centers[2], 143, 1);
  std::snprintf(statistic, sizeof(statistic), "Regen %u%%",
                snapshot->hourly[0].rain_probability);
  display.drawString(statistic, statistic_centers[3], 143, 1);

  for (size_t index = 0; index < 3; ++index) {
    drawMiniDay(snapshot->daily[index], index,
                4 + static_cast<int16_t>(index) * 105);
  }
}

int16_t graphY(int16_t value, int16_t minimum, int16_t maximum) {
  if (minimum == maximum) return 127;
  return static_cast<int16_t>(151 -
                              (static_cast<int32_t>(value - minimum) * 48) /
                                  (maximum - minimum));
}

void drawHourlyPage(const WeatherSnapshot *snapshot,
                    const RuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  if (snapshot == nullptr) {
    drawNoData(status);
    return;
  }

  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("Die naechsten 8 Stunden", 8, 60, 4);
  display.setTextDatum(MC_DATUM);

  int16_t minimum = snapshot->hourly[0].temperature_tenths;
  int16_t maximum = minimum;
  for (const HourlyPoint &point : snapshot->hourly) {
    minimum = std::min(minimum, point.temperature_tenths);
    maximum = std::max(maximum, point.temperature_tenths);
  }
  minimum -= 10;
  maximum += 10;

  int16_t previous_x = 0;
  int16_t previous_y = 0;
  for (size_t index = 0; index < weather_config::HOURLY_POINTS; ++index) {
    const HourlyPoint &point = snapshot->hourly[index];
    const int16_t x = 17 + static_cast<int16_t>(index) * 41;
    const int16_t y = graphY(point.temperature_tenths, minimum, maximum);
    if (index > 0) {
      display.drawLine(previous_x, previous_y, x, y, COLOR_SUN);
    }
    display.fillCircle(x, y, 3, COLOR_SUN);

    char temperature[8];
    formatTemperature(point.temperature_tenths, temperature,
                      sizeof(temperature), false);
    display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
    display.drawString(temperature, x, y - 11, 1);

    const int16_t bar_height = point.rain_probability / 4;
    display.fillRect(x - 5, 179 - bar_height, 10, bar_height, COLOR_RAIN);
    display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
    display.drawString(point.hour, x, 187, 1);

    char rain[8];
    std::snprintf(rain, sizeof(rain), "%u", point.rain_probability);
    display.setTextColor(COLOR_RAIN, COLOR_BACKGROUND);
    display.drawString(rain, x, 199, 1);
    previous_x = x;
    previous_y = y;
  }
  display.setTextDatum(TR_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Regen %", 316, 159, 1);
}

void drawDailyPage(const WeatherSnapshot *snapshot,
                   const RuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  if (snapshot == nullptr) {
    drawNoData(status);
    return;
  }

  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("Vier-Tage-Ausblick", 8, 60, 4);

  for (size_t index = 0; index < weather_config::DAILY_POINTS; ++index) {
    const DailyPoint &point = snapshot->daily[index];
    const int16_t x = 3 + static_cast<int16_t>(index) * 79;
    constexpr int16_t WIDTH = 76;
    constexpr int16_t Y = 89;
    constexpr int16_t HEIGHT = 115;
    display.fillRoundRect(x, Y, WIDTH, HEIGHT, 6, COLOR_CARD);
    display.setTextDatum(TC_DATUM);
    display.setTextColor(COLOR_ACCENT, COLOR_CARD);
    char day_buffer[4];
    display.drawString(dayLabel(point, index, day_buffer), x + WIDTH / 2,
                       Y + 4, 2);
    drawWeatherIcon(point.weather_code, x + WIDTH / 2, Y + 36, 20, true,
                    COLOR_CARD);

    char minimum[8];
    char maximum[8];
    char temperatures[20];
    formatTemperature(point.minimum_tenths, minimum, sizeof(minimum), false);
    formatTemperature(point.maximum_tenths, maximum, sizeof(maximum), false);
    std::snprintf(temperatures, sizeof(temperatures), "%s/%s C", maximum,
                  minimum);
    display.setTextColor(TFT_WHITE, COLOR_CARD);
    display.drawString(temperatures, x + WIDTH / 2, Y + 63, 2);

    char rain[12];
    std::snprintf(rain, sizeof(rain), "Regen %u%%",
                  point.rain_probability);
    display.setTextColor(COLOR_RAIN, COLOR_CARD);
    display.drawString(rain, x + WIDTH / 2, Y + 82, 1);

    char sun[20];
    std::snprintf(sun, sizeof(sun), "%s-%s", point.sunrise, point.sunset);
    display.setTextColor(COLOR_MUTED, COLOR_CARD);
    display.drawString(sun, x + WIDTH / 2, Y + 97, 1);
  }
}

uint16_t uvRiskColor(uint16_t uv_index_tenths) {
  if (uv_index_tenths < 30) return COLOR_GOOD;
  if (uv_index_tenths < 60) return COLOR_UV_MODERATE;
  if (uv_index_tenths < 80) return COLOR_UV_HIGH;
  if (uv_index_tenths < 110) return COLOR_UV_VERY_HIGH;
  return COLOR_UV_EXTREME;
}

const char *uvRiskLabel(uint16_t uv_index_tenths) {
  if (uv_index_tenths < 30) return "NIEDRIG";
  if (uv_index_tenths < 60) return "MITTEL";
  if (uv_index_tenths < 80) return "HOCH";
  if (uv_index_tenths < 110) return "SEHR HOCH";
  return "EXTREM";
}

void formatUvIndex(uint16_t tenths, char *buffer, size_t size) {
  std::snprintf(buffer, size, "%.1f", tenths / 10.0F);
}

void drawUvPage(const WeatherSnapshot *snapshot,
                const RuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  if (snapshot == nullptr) {
    drawNoData(status);
    return;
  }

  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("UV-Index", 8, 60, 4);
  display.setTextDatum(TR_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Schutz ab UVI 3", 315, 67, 2);

  constexpr int16_t CURRENT_X = 4;
  constexpr int16_t CURRENT_Y = 88;
  constexpr int16_t CURRENT_WIDTH = 312;
  constexpr int16_t CURRENT_HEIGHT = 58;
  const uint16_t current_color = uvRiskColor(snapshot->uv_index_tenths);
  display.fillRoundRect(CURRENT_X, CURRENT_Y, CURRENT_WIDTH, CURRENT_HEIGHT, 6,
                        COLOR_CARD);
  display.fillRect(CURRENT_X, CURRENT_Y + CURRENT_HEIGHT - 5, CURRENT_WIDTH, 5,
                   current_color);

  display.setTextDatum(TL_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_CARD);
  display.drawString("AKTUELL", 13, 92, 1);
  char uv_value[8];
  formatUvIndex(snapshot->uv_index_tenths, uv_value, sizeof(uv_value));
  display.setTextColor(current_color, COLOR_CARD);
  display.drawString(uv_value, 12, 98, 6);
  display.drawString(uvRiskLabel(snapshot->uv_index_tenths), 132, 95, 4);

  char today_maximum[24];
  formatUvIndex(snapshot->daily[0].uv_index_max_tenths, uv_value,
                sizeof(uv_value));
  std::snprintf(today_maximum, sizeof(today_maximum), "Tagesmax. %s", uv_value);
  display.setTextColor(COLOR_MUTED, COLOR_CARD);
  display.drawString(today_maximum, 134, 124, 2);

  constexpr int16_t CARD_Y = 151;
  constexpr int16_t CARD_WIDTH = 76;
  constexpr int16_t CARD_HEIGHT = 53;
  for (size_t index = 0; index < weather_config::DAILY_POINTS; ++index) {
    const DailyPoint &point = snapshot->daily[index];
    const int16_t x = 3 + static_cast<int16_t>(index) * 79;
    const uint16_t color = uvRiskColor(point.uv_index_max_tenths);
    display.fillRoundRect(x, CARD_Y, CARD_WIDTH, CARD_HEIGHT, 5, COLOR_CARD);
    display.fillRect(x, CARD_Y + CARD_HEIGHT - 5, CARD_WIDTH, 5, color);

    char day_buffer[4];
    display.setTextDatum(TC_DATUM);
    display.setTextColor(COLOR_MUTED, COLOR_CARD);
    display.drawString(dayLabel(point, index, day_buffer), x + CARD_WIDTH / 2,
                       CARD_Y + 4, 1);
    formatUvIndex(point.uv_index_max_tenths, uv_value, sizeof(uv_value));
    display.setTextColor(color, COLOR_CARD);
    display.drawString(uv_value, x + CARD_WIDTH / 2, CARD_Y + 18, 4);
  }
}

void formatKeeperTime(int64_t epoch, char buffer[6]) {
  if (epoch < 1704067200) {
    std::strcpy(buffer, "--:--");
    return;
  }
  const time_t value = static_cast<time_t>(epoch);
  struct tm local_time {};
  localtime_r(&value, &local_time);
  std::snprintf(buffer, 6, "%02d:%02d", local_time.tm_hour,
                local_time.tm_min);
}

void formatNextKeeperCheck(const KeeperRuntimeStatus &status, char *buffer,
                           size_t size) {
  if (keeperBusy(status.phase)) {
    std::snprintf(buffer, size, "laeuft");
    return;
  }
  if (status.next_check_due_ms == 0) {
    std::snprintf(buffer, size, "--:--");
    return;
  }

  const int32_t remaining_ms =
      static_cast<int32_t>(status.next_check_due_ms - millis());
  if (remaining_ms <= 0) {
    std::snprintf(buffer, size, "jetzt");
    return;
  }
  const uint32_t seconds =
      (static_cast<uint32_t>(remaining_ms) + 999UL) / 1000UL;
  if (seconds >= 3600UL) {
    std::snprintf(buffer, size, "%luh %02lum",
                  static_cast<unsigned long>(seconds / 3600UL),
                  static_cast<unsigned long>((seconds / 60UL) % 60UL));
  } else {
    std::snprintf(buffer, size, "%02lu:%02lu",
                  static_cast<unsigned long>(seconds / 60UL),
                  static_cast<unsigned long>(seconds % 60UL));
  }
}

void drawKeeperNextCheckLine(const KeeperRuntimeStatus &status) {
  display.fillRect(5, 146, 180, 13, COLOR_BACKGROUND);
  char next_check[16];
  char line[32];
  formatNextKeeperCheck(status, next_check, sizeof(next_check));
  std::snprintf(line, sizeof(line), "Naechster: %s", next_check);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString(line, 7, 148, 1);
}

void drawKeeperFooter(const KeeperRuntimeStatus &status) {
  display.fillRect(0, ATTRIBUTION_Y, weather_config::DISPLAY_WIDTH, 10,
                   COLOR_BACKGROUND);
  char event[52];
  if (status.last_event[0] == '\0') {
    std::snprintf(event, sizeof(event), "> Noch kein Ereignis");
  } else {
    std::snprintf(event, sizeof(event), "> %.49s", status.last_event);
  }
  display.setTextDatum(TL_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString(event, 4, ATTRIBUTION_Y + 1, 1);
}

void drawNetworkPage(const KeeperRuntimeStatus &status) {
  display.fillRect(0, HEADER_HEIGHT, weather_config::DISPLAY_WIDTH,
                   ATTRIBUTION_Y - HEADER_HEIGHT, COLOR_BACKGROUND);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("InternetKeeper", 8, 60, 4);

  constexpr int16_t STATUS_X = 5;
  constexpr int16_t STATUS_Y = 87;
  constexpr int16_t STATUS_WIDTH = 310;
  constexpr int16_t STATUS_HEIGHT = 42;
  display.fillRoundRect(STATUS_X, STATUS_Y, STATUS_WIDTH, STATUS_HEIGHT, 6,
                        COLOR_CARD);
  display.fillCircle(21, 104, 7, keeperStatusColor(status));
  display.setTextColor(TFT_WHITE, COLOR_CARD);
  display.drawString(keeperMainLabel(status), 36, 91, 2);

  char connection[72];
  if (status.wifi_connected) {
    std::snprintf(connection, sizeof(connection), "%.15s | %.15s | %d dBm",
                  status.ssid[0] == '\0' ? "-" : status.ssid,
                  status.ip[0] == '\0' ? "-" : status.ip,
                  static_cast<int>(status.wifi_rssi));
  } else {
    std::snprintf(connection, sizeof(connection), "WLAN getrennt");
  }
  display.setTextColor(COLOR_MUTED, COLOR_CARD);
  display.drawString(connection, 36, 111, 1);

  char check_time[6];
  char login_time[6];
  char line[32];
  formatKeeperTime(status.last_check_epoch, check_time);
  formatKeeperTime(status.last_login_epoch, login_time);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  std::snprintf(line, sizeof(line), "Check: %s", check_time);
  display.drawString(line, 7, 135, 1);
  drawKeeperNextCheckLine(status);
  std::snprintf(line, sizeof(line), "Login: %s", login_time);
  display.drawString(line, 7, 161, 1);
  std::snprintf(line, sizeof(line), "Netze: %u/%u da | %u online",
                static_cast<unsigned>(status.reachable_networks),
                static_cast<unsigned>(status.configured_networks),
                static_cast<unsigned>(status.online_networks));
  display.drawString(line, 7, 174, 1);
  std::snprintf(line, sizeof(line), "Checks %lu | Login %lu/%lu",
                static_cast<unsigned long>(status.check_count),
                static_cast<unsigned long>(status.login_success_count),
                static_cast<unsigned long>(status.login_attempt_count));
  display.drawString(line, 7, 187, 1);

  const bool enabled = status.phase == KeeperPhase::Idle &&
                       !keeper_check_requested;
  const uint16_t button_background =
      enabled ? COLOR_CARD_SELECTED : COLOR_CARD;
  display.fillRoundRect(KEEPER_BUTTON_X, KEEPER_BUTTON_Y, KEEPER_BUTTON_WIDTH,
                        KEEPER_BUTTON_HEIGHT, 7, button_background);
  display.drawRoundRect(KEEPER_BUTTON_X, KEEPER_BUTTON_Y, KEEPER_BUTTON_WIDTH,
                        KEEPER_BUTTON_HEIGHT, 7,
                        enabled ? COLOR_ACCENT : COLOR_MUTED);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(enabled ? TFT_WHITE : COLOR_MUTED, button_background);
  if (status.phase == KeeperPhase::Unconfigured) {
    display.drawString("NICHT", KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 23, 2);
    display.drawString("KONFIG.", KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 43, 2);
  } else if (keeperBusy(status.phase)) {
    display.drawString("LAEUFT...",
                       KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + KEEPER_BUTTON_HEIGHT / 2, 2);
  } else if (keeper_check_requested) {
    display.drawString("WIRD", KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 23, 2);
    display.drawString("GESTARTET",
                       KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 43, 2);
  } else {
    display.drawString("JETZT", KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 23, 2);
    display.drawString("PRUEFEN",
                       KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH / 2,
                       KEEPER_BUTTON_Y + 43, 2);
  }
}

void drawPage(const WeatherSnapshot *snapshot, const RuntimeStatus &status) {
  switch (current_page) {
    case Page::Current:
      drawCurrentPage(snapshot, status);
      break;
    case Page::Hourly:
      drawHourlyPage(snapshot, status);
      break;
    case Page::Daily:
      drawDailyPage(snapshot, status);
      break;
    case Page::Uv:
      drawUvPage(snapshot, status);
      break;
    case Page::Network:
      drawNetworkPage(status.keeper);
      break;
  }
  if (current_page == Page::Network) {
    drawKeeperFooter(status.keeper);
  } else {
    drawAttribution();
  }
  drawNavigation();
}

bool keeperStatusDifferent(const KeeperRuntimeStatus &left,
                           const KeeperRuntimeStatus &right) {
  return left.revision != right.revision || left.phase != right.phase ||
         left.internet != right.internet ||
         left.active_network_index != right.active_network_index ||
         left.configured_networks != right.configured_networks ||
         left.reachable_networks != right.reachable_networks ||
         left.online_networks != right.online_networks ||
         left.wifi_connected != right.wifi_connected ||
         left.wifi_rssi != right.wifi_rssi ||
         left.last_check_epoch != right.last_check_epoch ||
         left.last_login_epoch != right.last_login_epoch ||
         left.next_check_due_ms != right.next_check_due_ms ||
         left.check_count != right.check_count ||
         left.login_attempt_count != right.login_attempt_count ||
         left.login_success_count != right.login_success_count ||
         std::strncmp(left.ssid, right.ssid, sizeof(left.ssid)) != 0 ||
         std::strncmp(left.ip, right.ip, sizeof(left.ip)) != 0 ||
         std::strncmp(left.last_event, right.last_event,
                      sizeof(left.last_event)) != 0;
}

bool statusDifferent(const RuntimeStatus &left, const RuntimeStatus &right) {
  return left.network != right.network || left.fetch != right.fetch ||
         left.last_error != right.last_error ||
         left.fetch_attempts != right.fetch_attempts ||
         left.fetch_successes != right.fetch_successes ||
         left.last_success_epoch != right.last_success_epoch ||
         keeperStatusDifferent(left.keeper, right.keeper);
}

void handleTouch(const KeeperRuntimeStatus &keeper_status) {
  const bool down = touchscreen.touched();
  if (down && !touch_was_down) {
    delay(12);
    int32_t x_sum = 0;
    int32_t y_sum = 0;
    uint8_t count = 0;
    for (uint8_t sample = 0; sample < 5 && touchscreen.touched(); ++sample) {
      const TS_Point point = touchscreen.getPoint();
      if (point.z > 100) {
        x_sum += point.x;
        y_sum += point.y;
        ++count;
      }
      delay(3);
    }
    if (count > 0) {
      const TS_Point average(x_sum / count, y_sum / count, 500);
      int16_t x = 0;
      int16_t y = 0;
      if (mapTouchPoint(average, x, y)) {
        if (y >= NAVIGATION_Y) {
          const uint8_t button = std::min<uint8_t>(
              PAGE_COUNT - 1, x / NAVIGATION_ITEM_WIDTH);
          current_page = static_cast<Page>(button);
          full_redraw_requested = true;
        } else if (current_page == Page::Network &&
                   x >= KEEPER_BUTTON_X &&
                   x < KEEPER_BUTTON_X + KEEPER_BUTTON_WIDTH &&
                   y >= KEEPER_BUTTON_Y &&
                   y < KEEPER_BUTTON_Y + KEEPER_BUTTON_HEIGHT &&
                   keeper_status.phase == KeeperPhase::Idle &&
                   !keeper_check_requested) {
          keeper_check_requested = true;
          full_redraw_requested = true;
        } else if (y < HEADER_HEIGHT && x < 165) {
          brightness_mode = static_cast<BrightnessMode>(
              (static_cast<uint8_t>(brightness_mode) + 1) % 3);
          applyBrightness();
          full_redraw_requested = true;
        }
      }
    }
  }
  touch_was_down = down;
}

}  // namespace

void beginWeatherUi() {
  pinMode(0, INPUT_PULLUP);
  touch_spi.begin(weather_config::TOUCH_CLK, weather_config::TOUCH_MISO,
                  weather_config::TOUCH_MOSI, weather_config::TOUCH_CS);
  touchscreen.begin(touch_spi);
  touchscreen.setRotation(weather_config::DISPLAY_ROTATION);

  display.init();
  display.setRotation(weather_config::DISPLAY_ROTATION);
  ledcSetup(weather_config::BACKLIGHT_PWM_CHANNEL, 5000, 8);
  ledcAttachPin(weather_config::BACKLIGHT_PIN,
                weather_config::BACKLIGHT_PWM_CHANNEL);
  applyBrightness();

  display.fillRect(0, 0, 107, 240, TFT_RED);
  display.fillRect(107, 0, 106, 240, TFT_GREEN);
  display.fillRect(213, 0, 107, 240, TFT_BLUE);
  delay(500);

  display.fillScreen(TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("CYD2USB / ST7789", 160, 83, 4);
  display.drawString("BOOT jetzt: Touch neu kalibrieren", 160, 122, 2);
  bool calibration_requested = false;
  const uint32_t prompt_started = millis();
  while (millis() - prompt_started < 1300) {
    calibration_requested = calibration_requested || digitalRead(0) == LOW;
    delay(10);
  }

  if (!loadCalibration() || calibration_requested) {
    calibrateTouch();
  }
  display.fillScreen(COLOR_BACKGROUND);
  full_redraw_requested = true;
}

void updateWeatherUi(const WeatherSnapshot *snapshot,
                     const RuntimeStatus &status, bool snapshot_changed) {
  handleTouch(status.keeper);

  const time_t now = time(nullptr);
  const int64_t clock_minute = now >= 1704067200 ? now / 60 : -1;
  const bool minute_changed = clock_minute != previous_clock_minute;
  const bool keeper_changed =
      !previous_status_valid ||
      keeperStatusDifferent(status.keeper, previous_status.keeper);
  const bool status_changed =
      !previous_status_valid || statusDifferent(status, previous_status);
  const uint32_t countdown_second = millis() / 1000UL;
  const bool countdown_changed =
      countdown_second != previous_countdown_second;

  if (minute_changed) {
    previous_clock_minute = clock_minute;
    applyBrightness();
  }

  bool page_redrawn = false;
  if (full_redraw_requested || snapshot_changed ||
      (status_changed && snapshot == nullptr) ||
      (keeper_changed && current_page == Page::Network)) {
    drawHeader(snapshot, status);
    drawPage(snapshot, status);
    full_redraw_requested = false;
    page_redrawn = true;
  } else if (minute_changed || status_changed) {
    drawHeader(snapshot, status);
  }
  if (!page_redrawn && countdown_changed && current_page == Page::Network) {
    drawKeeperNextCheckLine(status.keeper);
  }

  previous_status = status;
  previous_status_valid = true;
  previous_countdown_second = countdown_second;
}

bool takeUiKeeperCheckRequest() {
  const bool value = keeper_check_requested;
  keeper_check_requested = false;
  return value;
}

}  // namespace weatherclock
