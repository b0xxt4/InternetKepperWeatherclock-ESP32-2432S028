#pragma once

// Wird automatisch verwendet, solange include/secrets.h noch nicht existiert.
// Am einfachsten erzeugt `python3 configure.py` die echte Datei.
#define WEATHERCLOCK_SECRETS_CONFIGURED 0
#define WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED 0

static const WifiCredential WIFI_CREDENTIALS[] = {
    {"WLAN-NAME", "WLAN-PASSWORT"},
    // Jede weitere SSID wird ebenfalls geprueft und bei Bedarf angemeldet.
    // {"ZWEITES-WLAN", "ZWEITES-PASSWORT"},
};

constexpr size_t WIFI_CREDENTIAL_COUNT =
    sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);

static const char RUB_LOGIN_ID[] = "RUB-LOGINID";
static const char RUB_PASSWORD[] = "RUB-PASSWORT";
