#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "config.h"

namespace weatherclock {

constexpr uint32_t SNAPSHOT_MAGIC = 0x57434C4BUL;  // "WCLK"
constexpr uint16_t SNAPSHOT_SCHEMA = 1;

enum class NetworkState : uint8_t {
  Unconfigured,
  Disconnected,
  Connecting,
  SyncingTime,
  Online,
  NoInternet,
};

enum class FetchState : uint8_t {
  NoData,
  Cached,
  Fetching,
  Fresh,
  Failed,
};

enum class WeatherError : uint8_t {
  None,
  Wifi,
  Clock,
  Tls,
  Http,
  PayloadTooLarge,
  Json,
  InvalidData,
};

enum class KeeperPhase : uint8_t {
  Unconfigured,
  Idle,
  SwitchingNetwork,
  ConnectingWifi,
  CheckingInternet,
  BootstrappingClock,
  LoggingIn,
  VerifyingLogin,
};

enum class InternetState : uint8_t {
  Unknown,
  Online,
  CaptivePortal,
  WifiDown,
  LoginFailed,
};

struct HourlyPoint {
  char hour[3];
  int16_t temperature_tenths;
  uint8_t rain_probability;
  uint8_t weather_code;
};

struct DailyPoint {
  char date[11];
  char sunrise[6];
  char sunset[6];
  int16_t minimum_tenths;
  int16_t maximum_tenths;
  uint8_t rain_probability;
  uint8_t weather_code;
};

struct WeatherSnapshot {
  uint32_t magic;
  uint16_t schema;
  uint16_t struct_size;
  int64_t fetched_epoch;
  char source_time[17];
  int16_t temperature_tenths;
  int16_t apparent_temperature_tenths;
  uint16_t pressure_tenths;
  uint16_t wind_speed_tenths;
  uint8_t humidity;
  uint8_t weather_code;
  uint8_t is_day;
  uint8_t reserved;
  HourlyPoint hourly[weather_config::HOURLY_POINTS];
  DailyPoint daily[weather_config::DAILY_POINTS];
  uint32_t checksum;
};

struct KeeperRuntimeStatus {
  KeeperPhase phase;
  InternetState internet;
  int8_t active_network_index;
  uint8_t configured_networks;
  uint8_t reachable_networks;
  uint8_t online_networks;
  bool wifi_connected;
  int16_t wifi_rssi;
  int64_t last_check_epoch;
  int64_t last_login_epoch;
  uint32_t next_check_due_ms;
  uint32_t check_count;
  uint32_t login_attempt_count;
  uint32_t login_success_count;
  uint32_t revision;
  char ssid[33];
  char ip[16];
  char last_event[96];
};

struct RuntimeStatus {
  NetworkState network;
  FetchState fetch;
  WeatherError last_error;
  int16_t wifi_rssi;
  uint32_t fetch_attempts;
  uint32_t fetch_successes;
  int64_t last_success_epoch;
  KeeperRuntimeStatus keeper;
};

static_assert(std::is_trivially_copyable<WeatherSnapshot>::value,
              "WeatherSnapshot must remain queue- and cache-safe");
static_assert(std::is_trivially_copyable<KeeperRuntimeStatus>::value,
              "KeeperRuntimeStatus must remain safe to copy across cores");

}  // namespace weatherclock
