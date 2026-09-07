#pragma once

#include "weather_model.h"

namespace weatherclock {

void beginWeatherUi();

// Call frequently from Arduino loop(). The function redraws only changed
// regions; network activity happens on the other ESP32 core.
void updateWeatherUi(const WeatherSnapshot *snapshot,
                     const RuntimeStatus &status, bool snapshot_changed);

// Returns true once for every tap on the large action button on the NETZ
// page. The network-owner task performs the check; the UI never touches Wi-Fi.
bool takeUiKeeperCheckRequest();

}  // namespace weatherclock
