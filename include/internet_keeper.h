#pragma once

#include "weather_model.h"

namespace weatherclock {

using KeeperStatusPublisher = void (*)(const KeeperRuntimeStatus &status);

// Creates a deterministic initial snapshot. No credentials are copied into it.
void initializeInternetKeeperStatus(KeeperRuntimeStatus &status);

// Runs the same complete all-SSID check/login/return cycle as the standalone
// InternetKeeper. This is called only by the single network-owner task.
bool performInternetKeeperCycle(KeeperRuntimeStatus &status,
                                KeeperStatusPublisher publisher);

bool internetKeeperConfigured();

}  // namespace weatherclock
