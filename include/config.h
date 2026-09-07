#pragma once

#include <Arduino.h>

struct WifiCredential {
  const char *ssid;
  const char *password;
};

namespace weather_config {

constexpr char HOSTNAME[] = "wetteruhr";
constexpr char LOCATION_NAME[] = "44801 Bochum";
constexpr char LOCATION_DETAIL[] = "Bochum-Querenburg";
constexpr double LATITUDE = 51.4475741;
constexpr double LONGITUDE = 7.2677644;
constexpr int ELEVATION_METERS = 131;

constexpr char TIMEZONE[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr char NTP_SERVER_1[] = "de.pool.ntp.org";
constexpr char NTP_SERVER_2[] = "pool.ntp.org";

// RUB Lock-and-Key. Die unverschluesselte URL liefert ausschliesslich einen
// HTTP-Date-Header, damit TLS auch nach einem vollstaendigen Stromverlust
// sicher validiert werden kann. Zugangsdaten gehen nur an LOGIN_URL per TLS.
constexpr char PORTAL_TIME_URL[] =
    "http://login.ruhr-uni-bochum.de/cgi-bin/start?nocheck=1";
constexpr char LOGIN_URL[] =
    "https://login.ruhr-uni-bochum.de/cgi-bin/laklogin";
constexpr char CONNECTIVITY_CHECK_URL_PRIMARY[] =
    "http://connectivitycheck.gstatic.com/generate_204";
constexpr int CONNECTIVITY_EXPECTED_STATUS_PRIMARY = 204;
constexpr char CONNECTIVITY_CHECK_URL_SECONDARY[] =
    "http://detectportal.firefox.com/success.txt";
constexpr int CONNECTIVITY_EXPECTED_STATUS_SECONDARY = 200;
constexpr char CONNECTIVITY_EXPECTED_BODY_SECONDARY[] = "success";

// Acht Stunden und vier Tage halten die JSON-Antwort klein und reichen für
// die drei Wetteransichten des 320x240-Displays.
constexpr char WEATHER_URL[] =
    "https://api.open-meteo.com/v1/forecast?"
    "latitude=51.4475741&longitude=7.2677644"
    "&current=temperature_2m%2Crelative_humidity_2m%2C"
    "apparent_temperature%2Cweather_code%2Cwind_speed_10m%2C"
    "pressure_msl%2Cis_day"
    "&hourly=temperature_2m%2Cprecipitation_probability%2Cweather_code"
    "&forecast_hours=8"
    "&daily=weather_code%2Ctemperature_2m_max%2Ctemperature_2m_min%2C"
    "precipitation_probability_max%2Csunrise%2Csunset"
    "&forecast_days=4&timezone=Europe%2FBerlin";

constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MIN_MS = 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MAX_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t KEEPER_CHECK_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MS = 30UL * 1000UL;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30UL * 1000UL;
constexpr uint32_t KEEPER_HTTP_TIMEOUT_MS = 8UL * 1000UL;
constexpr uint32_t LOGIN_COOLDOWN_MS = 60UL * 1000UL;
constexpr uint8_t POST_LOGIN_CHECK_ATTEMPTS = 3;
constexpr uint32_t POST_LOGIN_FIRST_WAIT_MS = 2UL * 1000UL;
constexpr uint32_t POST_LOGIN_NEXT_WAIT_MS = 5UL * 1000UL;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 8UL * 1000UL;
constexpr uint32_t HTTP_TIMEOUT_MS = 12UL * 1000UL;
constexpr uint32_t CACHE_WRITE_INTERVAL_SECONDS = 60UL * 60UL;
constexpr uint32_t DATA_STALE_SECONDS = 90UL * 60UL;
constexpr uint32_t DATA_EXPIRED_SECONDS = 12UL * 60UL * 60UL;

constexpr uint8_t DISPLAY_ROTATION = 1;
constexpr uint16_t DISPLAY_WIDTH = 320;
constexpr uint16_t DISPLAY_HEIGHT = 240;
constexpr uint8_t BACKLIGHT_PIN = 21;
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 0;
constexpr uint8_t BACKLIGHT_DAY = 215;
constexpr uint8_t BACKLIGHT_NIGHT = 28;

constexpr uint8_t TOUCH_CLK = 25;
constexpr uint8_t TOUCH_MOSI = 32;
constexpr uint8_t TOUCH_MISO = 39;
constexpr uint8_t TOUCH_CS = 33;
constexpr uint8_t TOUCH_IRQ = 36;

constexpr size_t HOURLY_POINTS = 8;
constexpr size_t DAILY_POINTS = 4;

static_assert(KEEPER_CHECK_INTERVAL_MS <= 20UL * 60UL * 1000UL,
              "InternetKeeper interval must stay below 30 minutes");

}  // namespace weather_config
