/*
 * =============================================================================
 * AutoSprinkler — SENSOR-ONLY TEST SKETCH
 * =============================================================================
 * Purpose : Verify all available sensors WITHOUT the battery, pump,
 *           solenoid valve, relay, solar panel, or water system.
 *           Powers the board from USB (5V from your laptop).
 *
 * What this tests:
 *   [YES]  3x Capacitive Soil Moisture Sensors (DFRobot V1.0)
 *   [YES]  Water Flow Sensor (YF-S201) -- bench test: blow air or drip water
 *   [YES]  Wi-Fi + Cloud Relay (data visible in your mobile app live)
 *   [YES]  IO Expansion Shield (just plug sensors into it)
 *   [SKIP] Battery
 *   [SKIP] Pump / Solenoid Valve / Relay (pins configured but NOT driven)
 *   [SKIP] RTC (SD2405) -- optional, time shows "no RTC" if missing
 *
 * Board:  FireBeetle 2 ESP32-E (N16R2)  +  IO Expansion Shield
 * Power:  USB cable to laptop (5V) -- NO battery needed
 *
 * How to use:
 *   1. Wire the moisture sensors and flow sensor (see comments below)
 *   2. Set your Wi-Fi credentials in WIFI_SSID / WIFI_PASS
 *   3. Upload this sketch with Arduino IDE 2.x
 *   4. Open Serial Monitor at 115200 baud
 *   5. Stick sensors in a glass of water, then in dry air -- watch values change
 *   6. Open the mobile app -> tap "Cloud (Render)" to see live data remotely
 *
 * Wiring (FireBeetle 2 + IO Shield -- everything powered from USB at 3.3V/5V):
 *
 *   Soil Sensor 1 -> A0 (GPIO 36) on IO Shield, VCC=3.3V, GND=GND
 *   Soil Sensor 2 -> A1 (GPIO 39) on IO Shield, VCC=3.3V, GND=GND
 *   Soil Sensor 3 -> A2 (GPIO 34) on IO Shield, VCC=3.3V, GND=GND
 *
 *   Flow Sensor   -> Signal -> GPIO 27 (through 1kO + 2kO voltage divider)
 *                   VCC    -> 5V on IO Shield
 *                   GND    -> GND
 *   NOTE: YF-S201 outputs 5V pulses. Use a voltage divider:
 *         Sensor Signal -> 1kO -> GPIO27, and 2kO from GPIO27 to GND.
 *         This brings 5V down to ~3.3V (safe for ESP32).
 *
 * =============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <ArduinoJson.h>

// =============================================================================
// CONFIGURE THESE BEFORE UPLOADING
// =============================================================================
#define WIFI_SSID    "Gian's"
#define WIFI_PASS    "G!@n's2426"
#define API_TOKEN    "sprk_9e33b52a39bd26e323871d6cd01c483e"
#define CLOUD_URL    "https://sprinkler-server.onrender.com"

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
const uint8_t SOIL_PIN[3] = { 36, 39, 34 };  // A0, A1, A2 on IO Shield
#define PIN_FLOW  27                           // Flow sensor signal (via voltage divider)
#define RTC_ADDR  0x32                         // SD2405 RTC I2C address

// =============================================================================
// SENSOR STATE
// =============================================================================
float pct[3]  = { 50, 50, 50 };
float fused   = 50;
int   raws[3];
bool  ok[3];
int   nOk     = 0;
bool  emaInit = false;

// Calibration defaults (same as full firmware)
int calDry[3] = { 3100, 3100, 3100 };  // Raw ADC in DRY AIR
int calWet[3] = { 1450, 1450, 1450 };  // Raw ADC in WATER

// Flow sensor
portMUX_TYPE      mux    = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulses = 0;
float flowLpm = 0;
float totalL  = 0;

void IRAM_ATTR isrFlow() {
  portENTER_CRITICAL_ISR(&mux);
  pulses++;
  portEXIT_CRITICAL_ISR(&mux);
}

// RTC
bool rtcOk = false;

// Web server
WebServer webServer(80);

// Timing
uint32_t lastCloudSync = 0;
uint32_t lastTick      = 0;
uint32_t lastWifiTry   = 0;
uint32_t tick          = 0;

// =============================================================================
// SOIL SENSOR PIPELINE (identical algorithm to full firmware)
// =============================================================================

int readRaw(uint8_t pin) {
  int v[9];
  for (int i = 0; i < 9; i++) {
    v[i] = analogRead(pin);
    delayMicroseconds(300);
  }
  // Insertion sort to find median
  for (int i = 1; i < 9; i++) {
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[4];  // median of 9
}

float rawToPct(int i, int raw) {
  int span = calDry[i] - calWet[i];
  if (span < 200) span = 200;
  return constrain(100.0f * (calDry[i] - raw) / (float)span, 0.0f, 100.0f);
}

void sampleSoil() {
  float v[3];
  nOk = 0;
  for (int i = 0; i < 3; i++) {
    raws[i] = readRaw(SOIL_PIN[i]);
    ok[i]   = (raws[i] > 300 && raws[i] < 4000);
    if (!ok[i]) continue;

    float p  = rawToPct(i, raws[i]);
    pct[i]   = emaInit ? pct[i] + 0.15f * (p - pct[i]) : p;
    v[nOk++] = pct[i];
  }
  emaInit = true;

  // Sensor fusion: median of valid sensors
  if (nOk == 3) {
    float a = v[0], b = v[1], c = v[2];
    fused = max(min(a, b), min(max(a, b), c));
  } else if (nOk == 2) {
    fused = (v[0] + v[1]) / 2.0f;
  }
}

// =============================================================================
// FLOW SENSOR
// =============================================================================

void updateFlow() {
  portENTER_CRITICAL(&mux);
  uint32_t p = pulses;
  pulses = 0;
  portEXIT_CRITICAL(&mux);

  float litres = p / 450.0f;
  flowLpm  = 0.7f * flowLpm + 0.3f * (litres * 60.0f);
  totalL  += litres;
}

// =============================================================================
// RTC CHECK (SD2405 optional -- just checks if it responds on I2C)
// =============================================================================

void checkRTC() {
  Wire.beginTransmission(RTC_ADDR);
  rtcOk = (Wire.endTransmission() == 0);
}

// =============================================================================
// CALIBRATION HELPERS
// =============================================================================

void calibrateDry() {
  for (int i = 0; i < 3; i++) calDry[i] = readRaw(SOIL_PIN[i]);
  Serial.printf("[CAL] DRY set: %d, %d, %d\n", calDry[0], calDry[1], calDry[2]);
}

void calibrateWet() {
  for (int i = 0; i < 3; i++) calWet[i] = readRaw(SOIL_PIN[i]);
  Serial.printf("[CAL] WET set: %d, %d, %d\n", calWet[0], calWet[1], calWet[2]);
}

// =============================================================================
// HTTP REST API (same endpoints as full firmware so the app works unchanged)
// =============================================================================

bool checkToken() {
  if (!webServer.hasHeader("X-Token") || webServer.header("X-Token") != API_TOKEN) {
    webServer.send(401, "application/json", "{\"err\":\"unauthorized\"}");
    return false;
  }
  return true;
}

void sendJson(int code, const String& s) {
  webServer.send(code, "application/json", s);
}

void hStatus() {
  if (!checkToken()) return;
  JsonDocument d;
  d["state"]     = "IDLE";
  d["pump"]      = false;
  d["valve"]     = false;
  d["flow"]      = flowLpm;
  d["session"]   = 0;
  d["total"]     = totalL;
  d["avg"]       = (int)fused;
  JsonArray m = d["moist"].to<JsonArray>();
  for (int i = 0; i < 3; i++) m.add((int)(ok[i] ? pct[i] : -1));
  d["time"]      = rtcOk ? "rtc-ok" : "no-rtc";
  d["rssi"]      = WiFi.RSSI();
  d["auto"]      = false;
  d["rainSkip"]  = false;
  d["fault"]     = "NONE";
  d["sensorsOk"] = nOk >= 2;
  d["raw0"]      = raws[0];
  d["raw1"]      = raws[1];
  d["raw2"]      = raws[2];
  d["nOk"]       = nOk;
  d["testMode"]  = true;
  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

void hConfig() {
  if (!checkToken()) return;
  JsonDocument d;
  d["lowPct"] = 35; d["highPct"] = 60;
  d["pulseSec"] = 60; d["soakSec"] = 300;
  d["maxCycles"] = 4; d["maxLiters"] = 40;
  d["w1s"] = 360; d["w1e"] = 540;
  d["w2s"] = 1020; d["w2e"] = 1110;
  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

void hManual() {
  if (!checkToken()) return;
  sendJson(200, "{\"ok\":false,\"msg\":\"Pump not available in sensor-test mode\"}");
}

void hStop()    { if (!checkToken()) return; sendJson(200, "{\"ok\":true}"); }
void hReset()   { if (!checkToken()) return; sendJson(200, "{\"ok\":true}"); }
void hTime()    { if (!checkToken()) return; sendJson(200, "{\"ok\":true}"); }
void hHistory() { if (!checkToken()) return; sendJson(200, "{\"step\":300,\"v\":[],\"w\":[]}"); }

void hCal() {
  if (!checkToken()) return;
  String body = webServer.arg("plain");
  JsonDocument d;
  if (deserializeJson(d, body)) { sendJson(400, "{\"err\":\"bad json\"}"); return; }
  const char* kind = d["kind"] | "";
  if (strcmp(kind, "dry") == 0) {
    calibrateDry();
    sendJson(200, "{\"ok\":true,\"kind\":\"dry\"}");
  } else if (strcmp(kind, "wet") == 0) {
    calibrateWet();
    sendJson(200, "{\"ok\":true,\"kind\":\"wet\"}");
  } else {
    sendJson(400, "{\"err\":\"invalid kind\"}");
  }
}

void hWifi() {
  if (!checkToken()) return;
  String body = webServer.arg("plain");
  JsonDocument d;
  if (deserializeJson(d, body)) { sendJson(400, "{\"err\":\"bad json\"}"); return; }
  const char* s = d["ssid"] | "";
  const char* p = d["pass"] | "";
  Preferences prefs;
  prefs.begin("sprinkler", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
  sendJson(200, "{\"ok\":true,\"msg\":\"WiFi credentials saved. Rebooting...\"}");
  delay(500);
  ESP.restart();
}

void hWifiScan() {
  if (!checkToken()) return;
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray arr = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"]   = WiFi.SSID(i);
    o["rssi"]   = WiFi.RSSI(i);
    o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

void hRoot() {
  webServer.send(200, "text/plain",
    "AutoSprinkler SENSOR-TEST MODE\n"
    "Cloud: https://sprinkler-server.onrender.com\n"
    "Open Serial Monitor at 115200 for raw readings.\n");
}

// =============================================================================
// CLOUD SYNC -- sends sensor data to Render so the mobile app shows live data
// =============================================================================

void syncCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(CLOUD_URL) + "/api/device/telemetry");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", API_TOKEN);

  JsonDocument d;
  d["state"]     = "IDLE";
  d["pump"]      = false;
  d["valve"]     = false;
  d["flow"]      = flowLpm;
  d["session"]   = 0;
  d["total"]     = totalL;
  d["avg"]       = (int)fused;
  JsonArray m = d["moist"].to<JsonArray>();
  for (int i = 0; i < 3; i++) m.add((int)(ok[i] ? pct[i] : 0));
  d["rssi"]      = WiFi.RSSI();
  d["auto"]      = false;
  d["rainSkip"]  = false;
  d["fault"]     = "NONE";
  d["sensorsOk"] = nOk >= 2;
  d["testMode"]  = true;

  String payload;
  serializeJson(d, payload);

  int code = http.POST(payload);
  if (code == 200) {
    Serial.printf("[CLOUD] Synced OK (HTTP %d)\n", code);
    String resp = http.getString();
    JsonDocument inDoc;
    if (!deserializeJson(inDoc, resp)) {
      JsonArray cmds = inDoc["commands"].as<JsonArray>();
      for (JsonObject cmd : cmds) {
        String act = cmd["action"] | "";
        if (act == "wifi") {
          String s = cmd["ssid"] | "";
          String p = cmd["pass"] | "";
          if (s.length() > 0) {
            Preferences prefs;
            prefs.begin("sprinkler", false);
            prefs.putString("ssid", s);
            prefs.putString("pass", p);
            prefs.end();
            Serial.printf("[WiFi] Received credentials via Cloud: %s. Rebooting...\n", s.c_str());
            delay(500);
            ESP.restart();
          }
        } else if (act == "calibrate") {
          String k = cmd["kind"] | "";
          if (k == "dry") calibrateDry();
          else if (k == "wet") calibrateWet();
        }
      }
    }
  } else if (code > 0) {
    Serial.printf("[CLOUD] Synced (HTTP %d)\n", code);
  } else {
    Serial.printf("[CLOUD] Sync FAILED: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================");
  Serial.println("  AutoSprinkler SENSOR-ONLY TEST SKETCH");
  Serial.println("========================================\n");

  // ADC setup for soil sensors
  analogReadResolution(12);
  for (uint8_t p : SOIL_PIN) analogSetPinAttenuation(p, ADC_11db);

  // Flow sensor interrupt
  pinMode(PIN_FLOW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), isrFlow, FALLING);

  // I2C scan for RTC
  Wire.begin();
  checkRTC();
  Serial.printf("[RTC]  SD2405 %s\n", rtcOk ? "FOUND" : "NOT FOUND (not needed for this test)");

  // Load Wi-Fi credentials from NVS Flash (or fallback to defaults)
  Preferences prefs;
  prefs.begin("sprinkler", true);
  String activeSsid = prefs.getString("ssid", WIFI_SSID);
  String activePass = prefs.getString("pass", WIFI_PASS);
  prefs.end();

  // Wi-Fi
  Serial.printf("[WiFi] Connecting to \"%s\" ...", activeSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(activeSsid.c_str(), activePass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s  RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] FAILED -- starting fallback AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Sprinkler-Test", "sprinkler123");
    Serial.println("[WiFi] Fallback AP: Sprinkler-Test / sprinkler123");
    Serial.println("[WiFi] Connect phone to AP, then open app with Local SoftAP preset");
  }

  MDNS.begin("sprinkler");

  // HTTP server
  const char* hdrs[] = { "X-Token" };
  webServer.collectHeaders(hdrs, 1);
  webServer.on("/",               HTTP_GET,  hRoot);
  webServer.on("/api/status",     HTTP_GET,  hStatus);
  webServer.on("/api/config",     HTTP_GET,  hConfig);
  webServer.on("/api/config",     HTTP_POST, [](){ if(checkToken()) sendJson(200,"{\"ok\":true}"); });
  webServer.on("/api/manual",     HTTP_POST, hManual);
  webServer.on("/api/stop",       HTTP_POST, hStop);
  webServer.on("/api/reset",      HTTP_POST, hReset);
  webServer.on("/api/time",       HTTP_POST, hTime);
  webServer.on("/api/calibrate",  HTTP_POST, hCal);
  webServer.on("/api/history",    HTTP_GET,  hHistory);
  webServer.on("/api/wifi",       HTTP_POST, hWifi);
  webServer.on("/api/wifi/scan",  HTTP_GET,  hWifiScan);
  webServer.on("/api/cloud",      HTTP_GET,  [](){ sendJson(200, "{\"ok\":true,\"cloudUrl\":\"" CLOUD_URL "\"}"); });
  webServer.on("/api/cloud",      HTTP_POST, [](){ sendJson(200, "{\"ok\":true}"); });
  webServer.begin();

  Serial.println("[HTTP] Web server started on port 80");
  Serial.println("\n[INFO] Hold sensors in dry air for 10 seconds -- note raw values.");
  Serial.println("[INFO] Then dip all sensors in water -- raw values will drop.");
  Serial.println("[INFO] Use the app Settings to run Set DRY / Set WET calibration.\n");
  Serial.println("[INFO] Open mobile app -> tap Cloud (Render) to see live sensor data remotely.\n");
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void loop() {
  webServer.handleClient();

  // 1 Hz sensor tick
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    tick++;

    sampleSoil();
    updateFlow();

    // Print to Serial Monitor every second
    Serial.printf("---- Tick %-4lu -----------------------------------------------\n", tick);
    Serial.printf("  Soil S1: %5.1f%% (raw %4d, %s)\n", pct[0], raws[0], ok[0] ? "OK " : "ERR");
    Serial.printf("  Soil S2: %5.1f%% (raw %4d, %s)\n", pct[1], raws[1], ok[1] ? "OK " : "ERR");
    Serial.printf("  Soil S3: %5.1f%% (raw %4d, %s)\n", pct[2], raws[2], ok[2] ? "OK " : "ERR");
    Serial.printf("  Fused  : %5.1f%%  (%d/3 sensors OK)\n", fused, nOk);
    Serial.printf("  Flow   : %.3f L/min | Total: %.4f L\n", flowLpm, totalL);

    // Calibration tip on first few ticks
    if (tick == 3) {
      Serial.println("\n  [TIP] Sensors in air? Note the raw values -- those are your DRY points.");
      Serial.println("  [TIP] Dip sensors in water and watch raw values drop -- those are WET points.");
      Serial.println("  [TIP] In the app: Settings -> Calibrate -> Set DRY / Set WET\n");
    }
  }

  // Cloud sync every 3 s
  if (millis() - lastCloudSync >= 3000) {
    lastCloudSync = millis();
    syncCloud();
  }

  // Auto-reconnect Wi-Fi every 30 s
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiTry > 30000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }
}
