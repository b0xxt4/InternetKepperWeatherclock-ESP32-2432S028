# Wetteruhr + InternetKeeper für ESP32-2432S028 Doppel-USB

Diese Firmware ist für das **ESP32-2432S028 mit Micro-USB und USB-C**
ausgelegt. Diese CYD2USB/Rv3-Variante verwendet normalerweise einen ST7789
und einen resistiven XPT2046-Touchcontroller. Die Firmware vereint Wetteruhr
und den automatischen RUB-InternetKeeper in einem Gerät.

Der fest konfigurierte Wetterort ist:

- **44801 Bochum / Bochum-Querenburg**
- 51,4475741° N, 7,2677644° E
- ungefähr 131 m über NN
- Zeitzone Europe/Berlin mit automatischer Sommerzeit

## Funktionen

- große, per NTP synchronisierte Uhr
- aktuelles Wetter mit selbst gezeichnetem Symbol
- Temperatur, gefühlte Temperatur, Feuchtigkeit, Wind und Regenrisiko
- Verlauf für die nächsten acht Stunden
- Vier-Tage-Vorhersage mit Sonnenauf- und -untergang
- große Touch-Schaltflächen für den resistiven Bildschirm
- automatische Abdunkelung zwischen 22:00 und 07:00 Uhr
- manueller Helligkeitswechsel durch Tippen auf die Uhr
- TLS-geprüfter Wetterabruf ohne API-Schlüssel
- kompletter Rundlauf durch alle eingetragenen 2,4-GHz-WLANs alle fünf Minuten
- eigener Internetcheck und nötigenfalls RUB-Login für jede einzelne SSID
- echte Internetprüfung über zwei unabhängige Captive-Portal-Endpunkte
- automatischer RUB-Lock-and-Key-Login über zertifikatsgeprüftes HTTPS
- Login gilt erst, wenn eine von bis zu drei externen Gegenprüfungen gelingt
- Uhrzeit-Bootstrap aus dem HTTP-Date-Header des RUB-Portals, falls TLS nach
  einem vollständigen Stromverlust noch keine plausible Uhr besitzt
- eigene **NETZ**-Ansicht mit Internetstatus, SSID, IP, Signalstärke,
  Check-/Login-Zählern, letztem Ereignis und manuellem Prüflauf
- letzter gültiger Wetterstand bleibt bei WLAN-/API-Ausfall sichtbar
- stündlich begrenzter NVS-Cache, damit Flash-Schreibvorgänge niedrig bleiben
- gemeinsamer Netzwerk-Task auf dem zweiten ESP32-Kern; Uhr und Touch bleiben
  auch während WLAN-Wechseln und Login-Versuchen bedienbar

Die Wetterdaten werden alle 15 Minuten von
[Open-Meteo](https://open-meteo.com/) geladen. Die Daten stehen unter
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); die Quelle wird
auch direkt auf dem Display genannt.

## 1. WLAN und RUB-Zugang einrichten

Im Projektordner ausführen:

```bash
cd weatherclock
python3 configure.py
```

Das Skript fragt SSID, WLAN-Passwort, RUB-loginID und RUB-Passwort ab. Weitere
2,4-GHz-Netze können ergänzt werden; der Keeper verbindet, prüft und
authentifiziert sie der Reihe nach. Ein bereits erfolgreiches Netz überspringt
den Login, ein gesperrtes Netz erhält seinen eigenen Login-Versuch.
Passwörter werden bei der Eingabe nicht angezeigt. Die erzeugte
Datei `include/secrets.h` erhält Dateirechte `0600` und wird von Git ignoriert.

Wichtig: Wurde `secrets.h` schon mit einer früheren, reinen Wetteruhr-Version
erzeugt, das Skript noch einmal starten. Die alte Datei bleibt zwar baubar,
aktiviert den InternetKeeper aber bewusst nicht, weil ihr der RUB-Zugang
fehlt.

Ohne `secrets.h` lässt sich die Firmware trotzdem kompilieren und für den
Display-/Touchtest starten; auf dem Bildschirm erscheint dann der Hinweis zur
Konfiguration.

## 2. Kompilieren und flashen

PlatformIO muss installiert sein. Danach:

```bash
cd weatherclock
platformio run
platformio run --target upload
```

Falls mehrere serielle Geräte angeschlossen sind:

```bash
platformio device list
platformio run --target upload --upload-port /dev/ttyUSB0
```

Das klassische ESP32-WROOM erscheint unter Linux meistens als
`/dev/ttyUSB0`. Ein USB-Datenkabel verwenden; falls eine der beiden Buchsen
nur Strom liefert oder nicht erkannt wird, die andere probieren.

Die serielle Diagnose läuft mit 115200 Baud:

```bash
platformio device monitor --baud 115200
```

## 3. Erster Start und Touch-Kalibrierung

Beim Start erscheinen kurz rote, grüne und blaue Farbfelder. Damit werden
ST7789, Farbreihenfolge und Orientierung geprüft. Beim ersten Start fordert
die Firmware danach automatisch vier Berührungen an. Die Kreuze möglichst
genau mit dem Stift oder Fingernagel treffen.

Die Werte werden im nichtflüchtigen Speicher abgelegt. Für eine neue
Kalibrierung während der schwarzen Startanzeige kurz **BOOT** drücken. Nicht
schon beim Einstecken gedrückt halten, weil der ESP32 sonst in den
Flash-Modus wechseln kann.

## Bedienung

- **JETZT:** Uhr und aktuelles Wetter
- **STUNDEN:** Temperaturkurve und Regenwahrscheinlichkeit für acht Stunden
- **TAGE:** Vier-Tage-Ausblick
- **NETZ:** Details des InternetKeepers und Taste **JETZT PRUEFEN**
- **Uhr antippen:** Automatik, volle Helligkeit und gedimmte Helligkeit

Die Wetter- und Internetanzeigen sind getrennt: Grün bedeutet online bzw.
aktuelle Wetterdaten, Gelb einen laufenden Check oder ältere Daten und Rot
eine fehlende Verbindung oder einen fehlgeschlagenen Login. Ab 90 Minuten
wird der Wetterpunkt gelb, ab zwölf Stunden rot. Alte Wetterdaten werden nicht
durch eine fehlerhafte Antwort überschrieben.

**JETZT PRUEFEN** startet denselben vollständigen Rundlauf wie die Automatik:
WLAN verbinden, Internet prüfen, nötigenfalls anmelden, den Login extern
bestätigen und anschließend die Wetterdaten aktualisieren.
Bei zwei eingetragenen Netzen zeigt die NETZ-Seite nach einem erfolgreichen
Rundlauf beispielsweise `Netze: 2/2 da | 2 online`; während des Laufs nennt
die Ereigniszeile jeweils die gerade bearbeitete SSID.

## Ablauf des InternetKeepers

Die Reihenfolge ist absichtlich fest: WLAN-Verbindung, Internetprüfung,
gegebenenfalls Uhrzeit-Bootstrap, TLS-Login, externe Login-Bestätigung, NTP und
erst danach der Wetterabruf. Keeper und Wetterdienst greifen nie gleichzeitig
auf WLAN oder TLS zu. Dadurch kann die Uhr auch direkt hinter einem gesperrten
RUB-Portal starten.

Jede SSID besitzt eigene Erreichbarkeits-, Check-, Login- und Erfolgszähler
sowie eine eigene 60-sekündige Login-Wiederholungssperre. Zwischen den
Rundläufen bleibt das erste erreichbare WLAN aus der Liste aktiv, genau wie bei
der normalen InternetKeeper-Firmware. Ist gar kein WLAN erreichbar, beginnt
nach 30 Sekunden ein neuer Versuch. Zugangsdaten werden weder auf dem Display
noch in der seriellen Ausgabe ausgegeben.

## Hardware- und Fehlerhinweise

Das Profil verwendet:

- ST7789, 240x320, Querformat
- TFT: GPIO 12/13/14/15/2, Hintergrundbeleuchtung GPIO 21
- XPT2046: CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36
- TFT-SPI mit konservativen 40 MHz

Bleibt der Bildschirm weiß, zuerst sicherstellen, dass es wirklich die
Doppel-USB-Version ist. Zeigt er Bildfehler, `SPI_FREQUENCY` in
`platformio.ini` testweise auf `24000000` reduzieren. Hat ein gleich
beschrifteter Klon wider Erwarten einen ILI9341, muss im selben File
`ST7789_DRIVER` durch `ILI9341_2_DRIVER` ersetzt werden.

Die microSD-Karte bleibt absichtlich unbenutzt: Sie konkurriert auf diesem
Board mit dem separat verdrahteten Touchcontroller um den zweiten
Hardware-SPI-Port. Wetterdaten werden klein und Flash-schonend in NVS
gespeichert.

## Datenquelle und Grenzen

Open-Meteo kombiniert Vorhersagemodelle verschiedener Wetterdienste. Eine
Vorhersage ist keine amtliche Warnung und kann falsch sein. Die kostenlose API
ist laut Anbieter für private, nichtkommerzielle Nutzung vorgesehen und hat
Abruflimits. Mit vier Abrufen pro Stunde bleibt diese Uhr weit darunter.

Die Firmware sendet nur die fest eingetragenen Ortskoordinaten an Open-Meteo.
WLAN-Passwörter werden nicht übertragen und erscheinen nicht in der seriellen
Ausgabe. RUB-Zugangsdaten werden ausschließlich an
`https://login.ruhr-uni-bochum.de/cgi-bin/laklogin` gesendet und dabei gegen
die hinterlegte Root-CA geprüft. Der unverschlüsselte Portalaufruf dient nur
zum Lesen des öffentlichen HTTP-Date-Headers; er enthält keine Zugangsdaten.

Der RUB-Login schaltet die vom Portal gesehene Quell-/NAT-IP frei. Hinter einem
gemeinsamen Router kann das deshalb auch anderen Geräten dieses Anschlusses
helfen; bei einer eigenen Client-IP betrifft es nur die Wetteruhr. Die Funktion
ist für das RUB-Lock-and-Key-Portal gedacht, nicht für eduroam oder RUB-Guests.
