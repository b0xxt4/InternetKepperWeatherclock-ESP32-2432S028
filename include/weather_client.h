#pragma once

#include "weather_model.h"

namespace weatherclock {

// Starts the network worker on the ESP32's second core. A cached snapshot, if
// present and valid, is queued before the worker begins.
bool startWeatherService();

// Non-blocking: returns true only when a newer snapshot is waiting.
bool receiveWeatherSnapshot(WeatherSnapshot &snapshot);

// Thread-safe copy of the small runtime status structure.
RuntimeStatus getRuntimeStatus();

// Wakes the worker and schedules an immediate update once Wi-Fi and time are
// available.
void requestWeatherRefresh();

// Wakes the same network-owner task for a complete all-SSID check/login cycle.
void requestInternetKeeperCheck();

bool weatherServiceConfigured();
bool weatherSnapshotValid(const WeatherSnapshot &snapshot);

}  // namespace weatherclock
