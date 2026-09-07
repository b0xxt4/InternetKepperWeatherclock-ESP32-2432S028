#pragma once

#define WEATHERCLOCK_SECRETS_CONFIGURED 1
#define WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED 1

static const WifiCredential WIFI_CREDENTIALS[] = {
    {"577_sts", "vWMtgdbYE4SQmB8J"},
    {"TP-Link_EF5F", "92385689"},
};

constexpr size_t WIFI_CREDENTIAL_COUNT =
    sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);

static const char RUB_LOGIN_ID[] = "rocarlqg";
static const char RUB_PASSWORD[] = "Patiguachy@1";
