#include <Arduino.h>

#include <cstdlib>
#include <cstring>
#include <time.h>

#include "config.h"
#include "weather_client.h"
#include "weather_model.h"
#include "weather_ui.h"

namespace {

weatherclock::WeatherSnapshot active_snapshot{};
bool has_snapshot = false;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  setenv("TZ", weather_config::TIMEZONE, 1);
  tzset();

  Serial.println("ESP32 Wetteruhr + InternetKeeper startet");
  Serial.printf("Ort: %s (%.7f, %.7f, %d m)\n",
                weather_config::LOCATION_DETAIL, weather_config::LATITUDE,
                weather_config::LONGITUDE, weather_config::ELEVATION_METERS);
  Serial.printf("Chip: %s, Flash: %u Bytes, freier Heap: %u Bytes\n",
                ESP.getChipModel(), ESP.getFlashChipSize(), ESP.getFreeHeap());

  weatherclock::beginWeatherUi();
  if (!weatherclock::startWeatherService()) {
    Serial.println("Netzwerk-Task konnte nicht gestartet werden");
  }
  if (!weatherclock::weatherServiceConfigured()) {
    Serial.println(
        "WLAN/RUB-Zugang fehlt: zuerst im Ordner weatherclock "
        "'python3 configure.py' ausfuehren");
  }
}

void loop() {
  bool snapshot_changed = false;
  weatherclock::WeatherSnapshot received{};
  if (weatherclock::receiveWeatherSnapshot(received) &&
      weatherclock::weatherSnapshotValid(received)) {
    active_snapshot = received;
    has_snapshot = true;
    snapshot_changed = true;
  }

  const weatherclock::RuntimeStatus status =
      weatherclock::getRuntimeStatus();
  weatherclock::updateWeatherUi(has_snapshot ? &active_snapshot : nullptr,
                                status, snapshot_changed);
  if (weatherclock::takeUiKeeperCheckRequest()) {
    // Der Knopf startet einen kompletten Mehrfach-SSID-Keeper-Rundlauf. Der
    // Wetterabruf bleibt vorgemerkt und folgt, sobald ein Netz online ist.
    weatherclock::requestInternetKeeperCheck();
    weatherclock::requestWeatherRefresh();
  }
  delay(10);
}
