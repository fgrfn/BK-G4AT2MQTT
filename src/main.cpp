#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <vector>

// ---- Konfiguration ----
char ssid[32] = "SSID";
char password[64] = "Password";
char mqtt_server[64] = "192.168.178.1"; // MQTT Broker IP
int mqtt_port = 1883;
char mqtt_topic[64] = "gaszaehler/verbrauch";
char mqtt_availability_topic[64] = "gaszaehler/availability";
char mqtt_client_id[32] = "ESP32GasClient";
unsigned long poll_interval = 60000; // Standard: 60 Sekunden

Preferences preferences;
bool haDiscoverySent = false;

// ---- WiFi AP Mode ----
bool apMode = false;
const char* ap_ssid = "ESP32-GasZaehler";
const char* ap_password = "12345678"; // Mindestens 8 Zeichen
const unsigned long AP_MODE_TIMEOUT = 300000; // 5 Minuten im AP-Modus

// ---- Status LED ----
const int STATUS_LED_PIN = 2; // Onboard LED (GPIO2)
unsigned long lastLedBlink = 0;
bool ledState = false;

// ---- NTP Zeitserver ----
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600; // UTC+1
const int daylightOffset_sec = 3600; // Sommerzeit
bool timeInitialized = false;

// ---- Error Tracking ----
struct ErrorStats {
  unsigned long mbusTimeouts = 0;
  unsigned long mbusParseErrors = 0;
  unsigned long mqttErrors = 0;
  unsigned long wifiDisconnects = 0;
  unsigned long lastError = 0;
  char lastErrorMsg[64] = "";
};
ErrorStats errorStats;

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

// ---- Verlaufsdaten ----
struct MeasurementData {
  unsigned long timestamp;
  float volume;
};
std::vector<MeasurementData> measurements;
const size_t MAX_MEASUREMENTS = 50;
float lastVolume = -1;

// ---- M-Bus UART ----
HardwareSerial mbusSerial(1); // UART1
const int MBUS_RX_PIN = 16;   // GPIO16 (RX2) für ESP32 DevKit V1
const int MBUS_TX_PIN = 17;   // GPIO17 (TX2) für ESP32 DevKit V1
const long MBUS_BAUD = 2400;

// ---- MBUS State Maschine ----
enum MBusState { MBUS_IDLE, MBUS_WAIT_RESPONSE };
MBusState mbusState = MBUS_IDLE;
unsigned long mbusLastAction = 0;
const unsigned long MBUS_POLL_INTERVAL = 60000; // 60 Sekunden
const unsigned long MBUS_RESPONSE_TIMEOUT = 500; // ms

uint8_t mbusBuffer[256];
size_t mbusLen = 0;

// ---- Konfiguration laden/speichern ----
void loadConfig() {
  preferences.begin("gas-config", false);
  preferences.getString("ssid", ssid, sizeof(ssid));
  preferences.getString("password", password, sizeof(password));
  preferences.getString("mqtt_server", mqtt_server, sizeof(mqtt_server));
  mqtt_port = preferences.getInt("mqtt_port", 1883);
  preferences.getString("mqtt_topic", mqtt_topic, sizeof(mqtt_topic));
  poll_interval = preferences.getULong("poll_interval", 60000);
  preferences.end();
  
  // Fallback auf Defaults wenn leer
  if (strlen(ssid) == 0) strcpy(ssid, "SSID");
  if (strlen(mqtt_server) == 0) strcpy(mqtt_server, "192.168.178.1");
  if (strlen(mqtt_topic) == 0) strcpy(mqtt_topic, "gaszaehler/verbrauch");
  if (poll_interval < 10000) poll_interval = 60000; // Minimum 10 Sekunden
  
  // Availability Topic generieren
  snprintf(mqtt_availability_topic, sizeof(mqtt_availability_topic), "%s_availability", mqtt_topic);
}

void saveConfig() {
  preferences.begin("gas-config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putString("mqtt_server", mqtt_server);
  preferences.putInt("mqtt_port", mqtt_port);
  preferences.putString("mqtt_topic", mqtt_topic);
  preferences.putULong("poll_interval", poll_interval);
  preferences.end();
  Serial.println("Konfiguration gespeichert");
}

// ---- Fehler loggen ----
void logError(const char* msg) {
  errorStats.lastError = millis();
  strncpy(errorStats.lastErrorMsg, msg, sizeof(errorStats.lastErrorMsg) - 1);
  errorStats.lastErrorMsg[sizeof(errorStats.lastErrorMsg) - 1] = '\0';
  Serial.print("ERROR: ");
  Serial.println(msg);
}

// ---- Status LED ----
void updateStatusLED() {
  unsigned long now = millis();
  
  if (apMode) {
    // Sehr schnelles Blinken: AP-Modus aktiv
    if (now - lastLedBlink >= 100) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = now;
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // Schnelles Blinken: WLAN Problem
    if (now - lastLedBlink >= 200) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = now;
    }
  } else if (!client.connected()) {
    // Mittleres Blinken: MQTT Problem
    if (now - lastLedBlink >= 500) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = now;
    }
  } else {
    // Langsames Blinken: Alles OK
    if (now - lastLedBlink >= 2000) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = now;
    }
  }
}

// ---- WLAN Setup ----
void setup_wifi() {
  // Wenn SSID "SSID" ist, direkt in AP-Modus gehen
  if (strcmp(ssid, "SSID") == 0 || strlen(ssid) == 0) {
    Serial.println("Keine WLAN-Konfiguration gefunden. Starte Access Point...");
    startAPMode();
    return;
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Verbinde mit WLAN: ");
  Serial.println(ssid);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Verbunden! IP: ");
    Serial.println(WiFi.localIP());
    apMode = false;
  } else {
    Serial.println("WLAN-Verbindung fehlgeschlagen!");
    logError("WLAN Verbindung fehlgeschlagen");
    Serial.println("Starte Access Point für Konfiguration...");
    startAPMode();
  }
}

// ---- Access Point Modus starten ----
void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.println("\n========================================");
  Serial.println("   ACCESS POINT MODUS AKTIV");
  Serial.println("========================================");
  Serial.print("SSID: ");
  Serial.println(ap_ssid);
  Serial.print("Passwort: ");
  Serial.println(ap_password);
  Serial.print("IP-Adresse: ");
  Serial.println(IP);
  Serial.println("\nVerbinden Sie sich mit dem Access Point");
  Serial.println("und öffnen Sie http://" + IP.toString());
  Serial.println("========================================\n");
  
  apMode = true;
}

// ---- MQTT Reconnect ----
void reconnect() {
  while (!client.connected()) {
    Serial.print("Verbinde zu MQTT Broker: ");
    Serial.print(mqtt_server);
    Serial.print(":");
    Serial.println(mqtt_port);
    
    // Last Will Testament für automatische Offline-Erkennung
    if (client.connect(mqtt_client_id, mqtt_availability_topic, 1, true, "offline")) {
      Serial.println("MQTT verbunden");
      
      // Online Status senden
      client.publish(mqtt_availability_topic, "online", true);
      
      haDiscoverySent = false; // Discovery neu senden nach Reconnect
    } else {
      Serial.print("Fehler, rc=");
      Serial.print(client.state());
      Serial.println(" -> Neuer Versuch in 5 Sekunden");
      errorStats.mqttErrors++;
      logError("MQTT Verbindung fehlgeschlagen");
      delay(5000);
    }
  }
}

// ---- Home Assistant Auto-Discovery ----
void sendHomeAssistantDiscovery() {
  if (haDiscoverySent) return;
  
  String discoveryTopic = "homeassistant/sensor/gaszaehler/config";
  String payload = "{";
  payload += "\"name\":\"Gaszähler\",";
  payload += "\"state_topic\":\"" + String(mqtt_topic) + "\",";
  payload += "\"availability_topic\":\"" + String(mqtt_availability_topic) + "\",";
  payload += "\"unit_of_measurement\":\"m³\",";
  payload += "\"device_class\":\"gas\",";
  payload += "\"state_class\":\"total_increasing\",";
  payload += "\"value_template\":\"{{ value }}\",";
  payload += "\"unique_id\":\"esp32_gas_meter\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"esp32_gas_meter\"],";
  payload += "\"name\":\"ESP32 Gaszähler\",";
  payload += "\"model\":\"BK-G4 M-Bus Gateway\",";
  payload += "\"manufacturer\":\"ESP32\",";
  payload += "\"sw_version\":\"1.0\",";
  payload += "\"configuration_url\":\"http://" + WiFi.localIP().toString() + "\"";
  payload += "}";
  payload += "}";
  
  if (client.publish(discoveryTopic.c_str(), payload.c_str(), true)) {
    Serial.println("Home Assistant Discovery gesendet");
    haDiscoverySent = true;
  } else {
    Serial.println("Fehler beim Senden der Discovery");
    logError("HA Discovery fehlgeschlagen");
  }
}

// ---- BCD Parser für Gaszähler ----
float parseGasVolumeBCD(const uint8_t* data, size_t len) {
    for (size_t i = 0; i + 5 < len; i++) {
        if (data[i] == 0x0C && data[i+1] == 0x13) { // DIF=0x0C, VIF=0x13
            uint32_t value = 0;
            uint32_t factor = 1;
            for (int b = 0; b < 4; b++) {
                uint8_t byte = data[i+2+b];
                uint8_t lsn = byte & 0x0F;
                uint8_t msn = (byte >> 4) & 0x0F;
                value += lsn * factor; factor *= 10;
                value += msn * factor; factor *= 10;
            }
            return value / 1000.0; // 2 Dezimalstellen → m³
        }
    }
    return -1; // nicht gefunden
}

// ---- OTA Setup ----
void setupOTA() {
  ArduinoOTA.setHostname("esp32-gas");
  ArduinoOTA.onStart([]() { Serial.println("Start OTA Update"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA Ende"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Fortschritt: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Fehler[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth fehlgeschlagen");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin fehlgeschlagen");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Verbindung fehlgeschlagen");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Empfang fehlgeschlagen");
    else if (error == OTA_END_ERROR) Serial.println("End fehlgeschlagen");
  });
  ArduinoOTA.begin();
  Serial.println("OTA bereit");
}

// ---- WebServer Handler ----
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Gaszähler Monitor</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
    }
    .container {
      max-width: 1000px;
      margin: 0 auto;
    }
    .header {
      text-align: center;
      color: white;
      margin-bottom: 30px;
    }
    .header h1 {
      font-size: 2.5em;
      margin-bottom: 10px;
      text-shadow: 2px 2px 4px rgba(0,0,0,0.2);
    }
    .nav {
      display: flex;
      gap: 10px;
      justify-content: center;
      margin-bottom: 20px;
    }
    .nav button {
      background: white;
      border: none;
      padding: 10px 20px;
      border-radius: 8px;
      cursor: pointer;
      font-size: 1em;
      transition: transform 0.2s;
    }
    .nav button:hover {
      transform: translateY(-2px);
    }
    .nav button.active {
      background: #667eea;
      color: white;
    }
    .card {
      background: white;
      border-radius: 15px;
      padding: 25px;
      margin-bottom: 20px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.2);
    }
    .value-display {
      text-align: center;
      padding: 30px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      border-radius: 10px;
      color: white;
    }
    .value-display .label {
      font-size: 1.2em;
      opacity: 0.9;
      margin-bottom: 10px;
    }
    .value-display .value {
      font-size: 3em;
      font-weight: bold;
      text-shadow: 2px 2px 4px rgba(0,0,0,0.2);
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 15px;
      margin-top: 20px;
    }
    .status-item {
      padding: 15px;
      background: #f8f9fa;
      border-radius: 8px;
      border-left: 4px solid #667eea;
    }
    .status-item .label {
      font-size: 0.9em;
      color: #666;
      margin-bottom: 5px;
    }
    .status-item .value {
      font-size: 1.2em;
      font-weight: bold;
      color: #333;
    }
    .status-online { border-left-color: #28a745; }
    .status-offline { border-left-color: #dc3545; }
    .chart-container {
      height: 300px;
      margin-top: 20px;
      position: relative;
    }
    .chart {
      width: 100%;
      height: 100%;
    }
    .form-group {
      margin-bottom: 20px;
    }
    .form-group label {
      display: block;
      margin-bottom: 5px;
      font-weight: bold;
      color: #333;
    }
    .form-group input {
      width: 100%;
      padding: 10px;
      border: 2px solid #ddd;
      border-radius: 8px;
      font-size: 1em;
      transition: border-color 0.3s;
    }
    .form-group input:focus {
      outline: none;
      border-color: #667eea;
    }
    .btn {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border: none;
      padding: 12px 30px;
      border-radius: 8px;
      font-size: 1em;
      cursor: pointer;
      transition: transform 0.2s;
    }
    .btn:hover {
      transform: translateY(-2px);
    }
    .page {
      display: none;
    }
    .page.active {
      display: block;
    }
    .alert {
      padding: 15px;
      border-radius: 8px;
      margin-bottom: 20px;
      display: none;
    }
    .alert.success {
      background: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
    }
    .alert.error {
      background: #f8d7da;
      color: #721c24;
      border: 1px solid #f5c6cb;
    }
    @media (max-width: 600px) {
      .header h1 { font-size: 1.8em; }
      .value-display .value { font-size: 2em; }
      .nav { flex-wrap: wrap; }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>⚡ Gaszähler Monitor</h1>
      <p>ESP32 M-Bus Gateway</p>
      <div id="apModeWarning" style="display: none; background: #ff9800; color: white; padding: 10px; border-radius: 8px; margin-top: 10px;">
        ⚠️ <strong>Access Point Modus aktiv!</strong><br>
        Bitte konfigurieren Sie WLAN unter "Konfiguration" und speichern Sie die Einstellungen.
      </div>
    </div>

    <div class="nav">
      <button onclick="showPage('dashboard')" class="active" id="navDashboard">Dashboard</button>
      <button onclick="showPage('config')" id="navConfig">Konfiguration</button>
    </div>

    <div id="dashboard" class="page active">
      <div class="card">
        <div class="value-display">
          <div class="label">Aktueller Verbrauch</div>
          <div class="value" id="gasValue">-- m³</div>
        </div>
      </div>

      <div class="card">
        <h2>System Status</h2>
        <div class="status-grid">
          <div class="status-item" id="wifiStatus">
            <div class="label">WLAN</div>
            <div class="value">--</div>
          </div>
          <div class="status-item" id="mqttStatus">
            <div class="label">MQTT</div>
            <div class="value">--</div>
          </div>
          <div class="status-item">
            <div class="label">Uptime</div>
            <div class="value" id="uptime">--</div>
          </div>
          <div class="status-item">
            <div class="label">Letzte Messung</div>
            <div class="value" id="lastUpdate">--</div>
          </div>
          <div class="status-item">
            <div class="label">Poll-Intervall</div>
            <div class="value" id="pollInterval">--</div>
          </div>
          <div class="status-item">
            <div class="label">Status-LED</div>
            <div class="value">GPIO2</div>
          </div>
        </div>
      </div>

      <div class="card">
        <h2>Fehlerstatistik</h2>
        <div class="status-grid">
          <div class="status-item">
            <div class="label">M-Bus Timeouts</div>
            <div class="value" id="errMbusTimeout">0</div>
          </div>
          <div class="status-item">
            <div class="label">M-Bus Parse-Fehler</div>
            <div class="value" id="errMbusParse">0</div>
          </div>
          <div class="status-item">
            <div class="label">MQTT Fehler</div>
            <div class="value" id="errMqtt">0</div>
          </div>
          <div class="status-item">
            <div class="label">WLAN Trennungen</div>
            <div class="value" id="errWifi">0</div>
          </div>
        </div>
        <div id="lastError" style="margin-top: 15px; padding: 10px; background: #f8f9fa; border-radius: 5px; display: none;">
          <strong>Letzter Fehler:</strong> <span id="lastErrorMsg">--</span>
        </div>
      </div>

      <div class="card">
        <h2>Verlauf</h2>
        <div class="chart-container">
          <canvas id="chart" class="chart"></canvas>
        </div>
      </div>
    </div>

    <div id="config" class="page">
      <div class="card">
        <h2>Einstellungen</h2>
        <div id="configAlert" class="alert"></div>
        <form id="configForm" onsubmit="saveConfig(event)">
          <h3>WLAN</h3>
          <div class="form-group">
            <label>SSID</label>
            <input type="text" id="ssid" name="ssid" required>
          </div>
          <div class="form-group">
            <label>Passwort</label>
            <input type="password" id="password" name="password">
          </div>
          
          <h3 style="margin-top: 30px;">MQTT</h3>
          <div class="form-group">
            <label>Server IP</label>
            <input type="text" id="mqtt_server" name="mqtt_server" required>
          </div>
          <div class="form-group">
            <label>Port</label>
            <input type="number" id="mqtt_port" name="mqtt_port" required>
          </div>
          <div class="form-group">
            <label>Topic</label>
            <input type="text" id="mqtt_topic" name="mqtt_topic" required>
          </div>
          
          <h3 style="margin-top: 30px;">Abfrage-Einstellungen</h3>
          <div class="form-group">
            <label>Poll-Intervall (Sekunden)</label>
            <input type="number" id="poll_interval" name="poll_interval" min="10" max="3600" required>
            <small style="color: #666;">Wie oft der Gaszähler abgefragt wird (10-3600 Sekunden)</small>
          </div>
          
          <button type="submit" class="btn">Speichern & Neustart</button>
        </form>
      </div>
    </div>
  </div>

  <script>
    let currentPage = 'dashboard';
    let updateInterval = null;

    function showPage(page) {
      document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
      document.querySelectorAll('.nav button').forEach(b => b.classList.remove('active'));
      document.getElementById(page).classList.add('active');
      document.getElementById('nav' + page.charAt(0).toUpperCase() + page.slice(1)).classList.add('active');
      currentPage = page;
      
      if (page === 'dashboard') {
        updateInterval = setInterval(updateData, 5000);
        updateData();
      } else {
        if (updateInterval) clearInterval(updateInterval);
        if (page === 'config') loadConfig();
      }
    }

    function updateData() {
      fetch('/api/data')
        .then(r => r.json())
        .then(data => {
          // AP-Modus Warnung anzeigen
          if (data.apMode) {
            document.getElementById('apModeWarning').style.display = 'block';
          }
          
          document.getElementById('gasValue').textContent = 
            data.volume >= 0 ? data.volume.toFixed(2) + ' m³' : '-- m³';
          
          const wifiDiv = document.getElementById('wifiStatus');
          if (data.apMode) {
            wifiDiv.querySelector('.value').textContent = 'AP-Modus (' + data.apSSID + ')';
            wifiDiv.className = 'status-item';
            wifiDiv.style.borderLeftColor = '#ff9800';
          } else {
            wifiDiv.querySelector('.value').textContent = data.wifiConnected ? 'Verbunden' : 'Getrennt';
            wifiDiv.className = data.wifiConnected ? 'status-item status-online' : 'status-item status-offline';
          }
          
          const mqttDiv = document.getElementById('mqttStatus');
          mqttDiv.querySelector('.value').textContent = data.mqttConnected ? 'Verbunden' : 'Getrennt';
          mqttDiv.className = data.mqttConnected ? 'status-item status-online' : 'status-item status-offline';
          
          document.getElementById('uptime').textContent = formatUptime(data.uptime);
          
          // Zeitstempel-Anzeige (NTP oder relative Zeit)
          if (data.timeInitialized && data.lastUpdate > 1000000000) {
            const date = new Date(data.lastUpdate * 1000);
            document.getElementById('lastUpdate').textContent = date.toLocaleTimeString('de-DE');
          } else {
            document.getElementById('lastUpdate').textContent = 
              data.lastUpdate > 0 ? Math.floor((millis() - data.lastUpdate) / 1000) + 's' : '--';
          }
          
          document.getElementById('pollInterval').textContent = data.pollInterval + 's';
          
          // Error Stats
          document.getElementById('errMbusTimeout').textContent = data.errors.mbusTimeouts;
          document.getElementById('errMbusParse').textContent = data.errors.mbusParseErrors;
          document.getElementById('errMqtt').textContent = data.errors.mqttErrors;
          document.getElementById('errWifi').textContent = data.errors.wifiDisconnects;
          
          if (data.errors.lastError) {
            document.getElementById('lastError').style.display = 'block';
            document.getElementById('lastErrorMsg').textContent = data.errors.lastError;
          }
          
          if (data.history && data.history.length > 0) {
            drawChart(data.history);
          }
        })
        .catch(e => console.error('Fehler:', e));
    }

    function loadConfig() {
      fetch('/api/config')
        .then(r => r.json())
        .then(data => {
          document.getElementById('ssid').value = data.ssid;
          document.getElementById('password').value = data.password;
          document.getElementById('mqtt_server').value = data.mqtt_server;
          document.getElementById('mqtt_port').value = data.mqtt_port;
          document.getElementById('mqtt_topic').value = data.mqtt_topic;
          document.getElementById('poll_interval').value = data.poll_interval;
        })
        .catch(e => console.error('Fehler:', e));
    }

    function saveConfig(event) {
      event.preventDefault();
      const formData = new FormData(event.target);
      const config = {
        ssid: formData.get('ssid'),
        password: formData.get('password'),
        mqtt_server: formData.get('mqtt_server'),
        mqtt_port: parseInt(formData.get('mqtt_port')),
        mqtt_topic: formData.get('mqtt_topic'),
        poll_interval: parseInt(formData.get('poll_interval'))
      };
      
      fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config)
      })
      .then(r => r.json())
      .then(data => {
        const alert = document.getElementById('configAlert');
        alert.textContent = 'Einstellungen gespeichert! ESP32 startet neu...';
        alert.className = 'alert success';
        alert.style.display = 'block';
        setTimeout(() => { location.reload(); }, 3000);
      })
      .catch(e => {
        const alert = document.getElementById('configAlert');
        alert.textContent = 'Fehler beim Speichern!';
        alert.className = 'alert error';
        alert.style.display = 'block';
      });
    }

    function formatUptime(ms) {
      const s = Math.floor(ms / 1000);
      const m = Math.floor(s / 60);
      const h = Math.floor(m / 60);
      const d = Math.floor(h / 24);
      if (d > 0) return d + 'd ' + (h % 24) + 'h';
      if (h > 0) return h + 'h ' + (m % 60) + 'm';
      return m + 'm ' + (s % 60) + 's';
    }

    function drawChart(history) {
      const canvas = document.getElementById('chart');
      const ctx = canvas.getContext('2d');
      canvas.width = canvas.offsetWidth;
      canvas.height = canvas.offsetHeight;
      
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      
      if (history.length < 2) return;
      
      const values = history.map(h => h.volume);
      const min = Math.min(...values) * 0.95;
      const max = Math.max(...values) * 1.05;
      const range = max - min || 1;
      
      const padding = 40;
      const chartWidth = canvas.width - padding * 2;
      const chartHeight = canvas.height - padding * 2;
      
      // Achsen
      ctx.strokeStyle = '#ddd';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(padding, padding);
      ctx.lineTo(padding, canvas.height - padding);
      ctx.lineTo(canvas.width - padding, canvas.height - padding);
      ctx.stroke();
      
      // Linie
      ctx.strokeStyle = '#667eea';
      ctx.lineWidth = 2;
      ctx.beginPath();
      
      history.forEach((point, i) => {
        const x = padding + (i / (history.length - 1)) * chartWidth;
        const y = canvas.height - padding - ((point.volume - min) / range) * chartHeight;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();
      
      // Punkte
      ctx.fillStyle = '#667eea';
      history.forEach((point, i) => {
        const x = padding + (i / (history.length - 1)) * chartWidth;
        const y = canvas.height - padding - ((point.volume - min) / range) * chartHeight;
        ctx.beginPath();
        ctx.arc(x, y, 4, 0, 2 * Math.PI);
        ctx.fill();
      });
      
      // Beschriftung
      ctx.fillStyle = '#666';
      ctx.font = '12px sans-serif';
      ctx.fillText(max.toFixed(2) + ' m³', 5, padding);
      ctx.fillText(min.toFixed(2) + ' m³', 5, canvas.height - padding + 5);
    }

    updateData();
    updateInterval = setInterval(updateData, 5000);
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleAPI() {
  String json = "{";
  json += "\"volume\":" + String(lastVolume, 2) + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"mqttConnected\":" + String(client.connected() ? "true" : "false") + ",";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + ",";
  json += "\"apSSID\":\"" + String(ap_ssid) + "\",";
  json += "\"ipAddress\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  json += "\"uptime\":" + String(millis()) + ",";
  json += "\"lastUpdate\":" + String(measurements.empty() ? 0 : measurements.back().timestamp) + ",";
  json += "\"timeInitialized\":" + String(timeInitialized ? "true" : "false") + ",";
  json += "\"pollInterval\":" + String(poll_interval / 1000) + ",";
  json += "\"errors\":{";
  json += "\"mbusTimeouts\":" + String(errorStats.mbusTimeouts) + ",";
  json += "\"mbusParseErrors\":" + String(errorStats.mbusParseErrors) + ",";
  json += "\"mqttErrors\":" + String(errorStats.mqttErrors) + ",";
  json += "\"wifiDisconnects\":" + String(errorStats.wifiDisconnects) + ",";
  json += "\"lastError\":\"" + String(errorStats.lastErrorMsg) + "\"";
  json += "},";
  json += "\"history\":[";
  for (size_t i = 0; i < measurements.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"timestamp\":" + String(measurements[i].timestamp) + 
            ",\"volume\":" + String(measurements[i].volume, 2) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleConfigGet() {
  String json = "{";
  json += "\"ssid\":\"" + String(ssid) + "\",";
  json += "\"password\":\"" + String(password) + "\",";
  json += "\"mqtt_server\":\"" + String(mqtt_server) + "\",";
  json += "\"mqtt_port\":" + String(mqtt_port) + ",";
  json += "\"mqtt_topic\":\"" + String(mqtt_topic) + "\",";
  json += "\"poll_interval\":" + String(poll_interval / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleConfigPost() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    
    // Einfaches JSON Parsing (für kleine Daten ausreichend)
    int idx;
    
    idx = body.indexOf("\"ssid\":\"");
    if (idx >= 0) {
      int start = idx + 8;
      int end = body.indexOf("\"", start);
      String val = body.substring(start, end);
      val.toCharArray(ssid, sizeof(ssid));
    }
    
    idx = body.indexOf("\"password\":\"");
    if (idx >= 0) {
      int start = idx + 12;
      int end = body.indexOf("\"", start);
      String val = body.substring(start, end);
      val.toCharArray(password, sizeof(password));
    }
    
    idx = body.indexOf("\"mqtt_server\":\"");
    if (idx >= 0) {
      int start = idx + 15;
      int end = body.indexOf("\"", start);
      String val = body.substring(start, end);
      val.toCharArray(mqtt_server, sizeof(mqtt_server));
    }
    
    idx = body.indexOf("\"mqtt_port\":");
    if (idx >= 0) {
      int start = idx + 12;
      int end = body.indexOf(",", start);
      if (end < 0) end = body.indexOf("}", start);
      mqtt_port = body.substring(start, end).toInt();
    }
    
    idx = body.indexOf("\"mqtt_topic\":\"");
    if (idx >= 0) {
      int start = idx + 14;
      int end = body.indexOf("\"", start);
      String val = body.substring(start, end);
      val.toCharArray(mqtt_topic, sizeof(mqtt_topic));
    }
    
    idx = body.indexOf("\"poll_interval\":");
    if (idx >= 0) {
      int start = idx + 17;
      int end = body.indexOf(",", start);
      if (end < 0) end = body.indexOf("}", start);
      poll_interval = body.substring(start, end).toInt() * 1000; // Sekunden -> ms
      if (poll_interval < 10000) poll_interval = 60000; // Minimum 10s
    }
    
    saveConfig();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    
    Serial.println("Konfiguration gespeichert.");
    if (apMode) {
      Serial.println("Wechsel zu Station-Modus in 3 Sekunden...");
    } else {
      Serial.println("Neustart in 3 Sekunden...");
    }
    delay(3000);
    ESP.restart();
  } else {
    server.send(400, "application/json", "{\"error\":\"invalid request\"}");
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/data", handleAPI);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.begin();
  Serial.println("WebServer gestartet auf http://" + WiFi.localIP().toString());
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32 Gaszähler Gateway v1.0");
  Serial.println("================================");
  
  // Status LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  
  loadConfig();
  setup_wifi();
  
  // NTP Zeit initialisieren
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Synchronisiere Zeit mit NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      timeInitialized = true;
      Serial.println("Zeit synchronisiert");
    } else {
      Serial.println("Zeit-Synchronisation fehlgeschlagen");
    }
  }
  
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(512); // Größerer Buffer für Discovery

  mbusSerial.begin(MBUS_BAUD, SERIAL_8E1, MBUS_RX_PIN, MBUS_TX_PIN);
  Serial.println("M-Bus UART bereit");

  setupOTA();
  setupWebServer();
  mbusLastAction = millis() - poll_interval; // sofort Poll starten
  
  Serial.println("Setup abgeschlossen!");
  Serial.println("================================\n");
}

// ---- Loop ----
void loop() {
  // Im AP-Modus nur WebServer und OTA
  if (apMode) {
    ArduinoOTA.handle();
    server.handleClient();
    updateStatusLED();
    return;
  }
  
  // WLAN Check
  if (WiFi.status() != WL_CONNECTED) {
    errorStats.wifiDisconnects++;
    logError("WLAN Verbindung verloren");
    setup_wifi();
  }
  
  if (!client.connected()) reconnect();
  client.loop();
  ArduinoOTA.handle();
  server.handleClient();
  updateStatusLED();
  
  // Home Assistant Discovery senden (einmalig nach Connect)
  if (client.connected() && !haDiscoverySent) {
    sendHomeAssistantDiscovery();
  }

  unsigned long now = millis();

  switch (mbusState) {
    case MBUS_IDLE:
      if (now - mbusLastAction >= poll_interval) {
        // Poll senden
        uint8_t pollFrame[5] = {0x10, 0x5B, 0x00, 0x5B, 0x16};
        mbusSerial.write(pollFrame, sizeof(pollFrame));
        mbusSerial.flush();
        mbusLen = 0;
        mbusLastAction = now;
        mbusState = MBUS_WAIT_RESPONSE;
        Serial.println("MBUS Poll gesendet, warte auf Antwort...");
      }
      break;

    case MBUS_WAIT_RESPONSE:
      while (mbusSerial.available() && mbusLen < sizeof(mbusBuffer)) {
        mbusBuffer[mbusLen++] = mbusSerial.read();
      }

      if ((now - mbusLastAction >= MBUS_RESPONSE_TIMEOUT) || mbusLen >= sizeof(mbusBuffer)) {
        if (mbusLen > 0) {
          Serial.print("MBUS Antwort empfangen (");
          Serial.print(mbusLen);
          Serial.println(" Bytes)");

          float volume = parseGasVolumeBCD(mbusBuffer, mbusLen);
          if (volume >= 0) {
            char payload[16];
            dtostrf(volume, 0, 2, payload);
            
            if (client.publish(mqtt_topic, payload)) {
              Serial.print("Verbrauch gesendet: ");
              Serial.println(payload);
            } else {
              errorStats.mqttErrors++;
              logError("MQTT Publish fehlgeschlagen");
            }
            
            // Verlauf speichern mit echter Zeit wenn verfügbar
            lastVolume = volume;
            unsigned long timestamp = timeInitialized ? time(nullptr) : millis();
            measurements.push_back({timestamp, volume});
            if (measurements.size() > MAX_MEASUREMENTS) {
              measurements.erase(measurements.begin());
            }
          } else {
            Serial.println("Kein Volumenwert gefunden!");
            errorStats.mbusParseErrors++;
            logError("M-Bus Parse Fehler");
          }
        } else {
          Serial.println("Keine MBUS Antwort erhalten");
          errorStats.mbusTimeouts++;
          logError("M-Bus Timeout");
        }
        mbusState = MBUS_IDLE; // wieder bereit für nächsten Poll
      }
      break;
  }
}
