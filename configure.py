#!/usr/bin/env python3
"""Create include/secrets.h without echoing Wi-Fi or RUB passwords."""

from __future__ import annotations

import getpass
import os
from pathlib import Path


TARGET = Path(__file__).resolve().parent / "include" / "secrets.h"


def cpp_string(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "\\r")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def ask_nonempty(prompt: str) -> str:
    while True:
        value = input(prompt)
        if value:
            return value
        print("Der Wert darf nicht leer sein.")


def main() -> int:
    if TARGET.exists():
        answer = input(f"{TARGET} existiert bereits. Überschreiben? [j/N] ")
        if answer.strip().lower() not in {"j", "ja", "y", "yes"}:
            print("Abgebrochen; die vorhandene Datei blieb unverändert.")
            return 1

    networks: list[tuple[str, str]] = []
    print("\nWLAN-Zugangsdaten (nur 2,4 GHz; bei offenem WLAN Passwort leer)")
    print("Jede eingetragene SSID wird spaeter einzeln geprueft und angemeldet.")
    while True:
        ssid = ask_nonempty("SSID: ")
        password = getpass.getpass("WLAN-Passwort: ")
        networks.append((ssid, password))
        answer = input("Weiteres WLAN hinzufügen? [j/N] ")
        if answer.strip().lower() not in {"j", "ja", "y", "yes"}:
            break

    print("\nRUB Lock-and-Key-Zugang")
    login_id = ask_nonempty("RUB-loginID: ")
    rub_password = getpass.getpass("RUB-Passwort: ")
    while not rub_password:
        print("Der Wert darf nicht leer sein.")
        rub_password = getpass.getpass("RUB-Passwort: ")

    wifi_lines = "\n".join(
        f"    {{{cpp_string(ssid)}, {cpp_string(password)}}},"
        for ssid, password in networks
    )
    content = f'''#pragma once

#define WEATHERCLOCK_SECRETS_CONFIGURED 1
#define WEATHERCLOCK_RUB_CREDENTIALS_CONFIGURED 1

static const WifiCredential WIFI_CREDENTIALS[] = {{
{wifi_lines}
}};

constexpr size_t WIFI_CREDENTIAL_COUNT =
    sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);

static const char RUB_LOGIN_ID[] = {cpp_string(login_id)};
static const char RUB_PASSWORD[] = {cpp_string(rub_password)};
'''

    descriptor = os.open(
        TARGET, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600
    )
    with os.fdopen(descriptor, "w", encoding="utf-8") as target:
        target.write(content)
    os.chmod(TARGET, 0o600)
    print(f"\nGespeichert: {TARGET}")
    print("Die Datei ist per .gitignore ausgeschlossen.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
