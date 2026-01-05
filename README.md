# BK-G4AT2MQTT

> **Original Projekt von [BennoB666](https://github.com/BennoB666/BK-G4AT2MQTT)**  
> Dieser Fork enthält erweiterte Features für eine verbesserte Benutzerfreundlichkeit und Home Assistant Integration.

Ein ESP32 Gateway zum Auslesen der M-Bus Schnittstelle eines Honeywell BK-G4AT Gaszählers und Übertragung der Daten an einen MQTT Server.

---

## 📖 Original README

### Hardware Setup

Verbinden Sie den ESP32 wie im Bild gezeigt:

<img width="600" height="800" alt="wires" src="https://github.com/user-attachments/assets/be611be3-ce91-446a-a3be-2242b5ae99b2" />

| ESP32 Pin | M-Bus Interface |
|-----------|-----------------|
| TX2 (GPIO17) | TX |
| RX2 (GPIO16) | RX |
| GND | GND |
| 5V | VCC |

### Original Features

- ESP32-C3 Super Mini liest die M-Bus Schnittstelle
- Übertragung der Daten an einen MQTT Server
- OTA Updates über WLAN

**Original Konfiguration:**  
Im `src`-Ordner mussten folgende Werte angepasst werden: `ssid`, `password`, `mqtt_server`, `mqtt_port`, `mqtt_topic`. Das `MBUS_POLL_INTERVAL` ist das Update-Intervall in Millisekunden.

**OTA Setup:**  
In der `platformio.ini` kann die ESP32 IP-Adresse unter `upload_port = 192.168.178.20` angegeben werden für Updates über WLAN.

---

## 🚀 Erweiterungen in diesem Fork

Dieser Fork erweitert das Original-Projekt um zahlreiche Features für eine deutlich verbesserte Benutzerfreundlichkeit und professionelle Home Assistant Integration.

### ✨ Neue Features

#### 🌐 Modernes WebUI
- **Dashboard** mit Live-Anzeige des aktuellen Gasverbrauchs
- **Verlaufs-Chart** der letzten 50 Messungen
- **System-Status** Übersicht (WLAN, MQTT, Uptime)
- **Fehlerstatistik** mit detailliertem Logging
- **Responsive Design** für Desktop und Mobile

#### ⚙️ Web-basierte Konfiguration
- **Keine Code-Änderungen** mehr nötig
- **Konfigurationsseite** im WebUI
- Einstellbar:
  - WLAN Zugangsdaten
  - MQTT Server IP & Port
  - MQTT Topic
  - Poll-Intervall (10-3600 Sekunden)
- **Persistente Speicherung** im Flash
- **Automatischer Neustart** nach Konfiguration

#### 📡 WiFi Fallback/Access Point Modus
- **Automatischer AP-Modus** bei fehlender WLAN-Konfiguration
- **Fallback** nach 15 Sekunden bei Verbindungsproblemen
- **Erstkonfiguration** ohne USB-Verbindung möglich
- AP-Zugangsdaten:
  - SSID: `ESP32-GasZaehler`
  - Passwort: `12345678`
  - IP: `192.168.4.1`

#### 🏠 Home Assistant Auto-Discovery
- **Automatische Erkennung** ohne YAML-Konfiguration
- **MQTT Discovery** mit allen notwendigen Attributen
- **Device Class:** `gas` für korrekte Icons
- **State Class:** `total_increasing` für Energie-Dashboard
- **Availability Topic** für Online/Offline Status
- **Last Will Testament** für automatische Offline-Erkennung

#### 🕐 NTP Zeit-Synchronisation
- **Automatische Zeitsynchronisation** beim Start
- **Echte Zeitstempel** für Messungen
- **Anzeige** als Uhrzeit im WebUI
- Server: `pool.ntp.org` (UTC+1 mit Sommerzeit)

#### 🚨 Status-LED (GPIO2)
- **Visuelles Feedback** ohne Serial Monitor
- **Sehr schnell blinken** (100ms): Access Point Modus
- **Schnell blinken** (200ms): WLAN Problem
- **Mittel blinken** (500ms): MQTT Problem
- **Langsam blinken** (2s): Alles OK

#### 📈 Detailliertes Error-Logging
- **Fehlerstatistik** im WebUI
- Tracking von:
  - M-Bus Timeouts
  - M-Bus Parse-Fehler
  - MQTT Fehler
  - WLAN Trennungen
- **Letzter Fehler** mit Meldung
- **Serial Monitor** Logging

#### 🔧 Weitere Verbesserungen
- **ESP32 DevKit V1** Support (zusätzlich zum ESP32-C3)
- **Größerer MQTT Buffer** (512 Bytes) für Discovery
- **Konfigurierbares Poll-Intervall**
- **MQTT Availability** für Home Assistant
- **Fehlerbehandlung** und automatisches Recovery

---

## 🚀 Installation & Einrichtung

### 1. Hardware vorbereiten
- ESP32 DevKit V1 oder ESP32-C3 verwenden
- M-Bus Interface wie oben beschrieben verkabeln

### 2. Firmware flashen
```bash
# PlatformIO
pio run -t upload
```

### 3. Erste Konfiguration (Access Point Modus)

Nach dem ersten Flash startet der ESP32 automatisch im AP-Modus:

1. **Mit WiFi verbinden:**
   - SSID: `ESP32-GasZaehler`
   - Passwort: `12345678`

2. **WebUI öffnen:**
   - Browser: `http://192.168.4.1`

3. **Konfiguration eingeben:**
   - Tab **"Konfiguration"** öffnen
   - WLAN Zugangsdaten eingeben
   - MQTT Server IP & Port eingeben
   - Optional: Topic und Poll-Intervall anpassen
   - **Speichern & Neustart**

4. **Nach Neustart:**
   - ESP32 verbindet sich mit WLAN
   - IP-Adresse im Serial Monitor angezeigt
   - WebUI unter neuer IP erreichbar

### 4. Home Assistant Integration

**Automatische Einrichtung:**
1. MQTT Broker in Home Assistant konfiguriert haben
2. Nach ESP32-Konfiguration erscheint Sensor automatisch
3. Unter **Einstellungen** → **Geräte & Dienste** → **MQTT**
4. Device: **"ESP32 Gaszähler"**

**Dashboard Karte:**
```yaml
type: entity
entity: sensor.gaszahler
name: Gasverbrauch
icon: mdi:meter-gas
```

**Energie-Dashboard:**
Der Sensor kann direkt im Energie-Dashboard verwendet werden (`state_class: total_increasing`).

---

## 🌐 WebUI

Aufruf: `http://[ESP32-IP-Adresse]`

### Dashboard
- Aktueller Verbrauch in m³
- WLAN & MQTT Status (online/offline)
- Uptime und letzte Messung
- Poll-Intervall Anzeige
- Fehlerstatistik mit Details
- Verlaufs-Chart der Messungen

### Konfiguration
- WLAN Einstellungen
- MQTT Server, Port & Topic
- Poll-Intervall (10-3600s)
- Speichern mit automatischem Neustart

---

## 📡 MQTT Topics

### Published Topics
- **Verbrauch:** `gaszaehler/verbrauch` (Wert in m³)
- **Availability:** `gaszaehler/verbrauch_availability` (online/offline)

### Home Assistant Discovery
- **Topic:** `homeassistant/sensor/gaszaehler/config`
- Automatisch beim MQTT-Connect gesendet

---

## 🔧 Unterstützte Boards

- **ESP32 DevKit V1** (empfohlen) - `board = esp32dev`
- **ESP32-C3 DevKit** - `board = esp32-c3-devkitm-1`

Anpassung in `platformio.ini`:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
```

---

## 🛠️ Fehlersuche

### WebUI Fehlerstatistik
Öffnen Sie das Dashboard → "Fehlerstatistik" zeigt:
- M-Bus Timeouts
- Parse-Fehler
- MQTT Verbindungsfehler
- WLAN Trennungen
- Letzter Fehler mit Beschreibung

### Serial Monitor
- Baudrate: **115200**
- Detailliertes Logging aller Aktionen
- AP-Modus Details beim Start
- IP-Adressen und Verbindungsstatus

### Status-LED Codes
- **100ms Blinken:** AP-Modus aktiv → Konfiguration erforderlich
- **200ms Blinken:** WLAN Problem → Zugangsdaten prüfen
- **500ms Blinken:** MQTT Problem → Broker IP prüfen
- **2s Blinken:** Alles OK

### WiFi Fallback
Bei WLAN-Problemen:
- Nach 15s automatischer AP-Modus
- Erneute Konfiguration möglich
- LED blinkt sehr schnell als Hinweis

---

## 📝 Technische Details

- **Plattform:** ESP32 (Arduino Framework)
- **M-Bus:** UART2 (GPIO16/17), 2400 Baud, 8E1
- **MQTT:** PubSubClient mit LWT (Last Will Testament)
- **WebServer:** Port 80
- **NTP:** pool.ntp.org (UTC+1 + Sommerzeit)
- **Config Storage:** Preferences (Flash)
- **OTA:** ArduinoOTA über WLAN

---

## 👏 Credits

**Original Projekt:** [BennoB666](https://github.com/BennoB666) - Danke für das Basis-Projekt!

**Fork Erweiterungen:** Zusätzliche Features für verbesserte Benutzerfreundlichkeit und Home Assistant Integration.

---

## 📄 Lizenz

Siehe [LICENSE](LICENSE)
