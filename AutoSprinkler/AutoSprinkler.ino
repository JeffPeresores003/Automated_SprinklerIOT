/*
 * =============================================================================
 * AutoSprinkler — Automated Irrigation Controller
 * =============================================================================
 * Board:    FireBeetle ESP32 (DFRobot) with IO Shield
 * Stack:    Arduino framework, ArduinoJson v7, local Wi-Fi REST API
 * Control:  Cycle-soak hysteresis + flow-verified dosing + safety interlocks
 *
 * The ESP32 makes every irrigation decision autonomously. The React Native
 * mobile app is only a remote control + dashboard over local Wi-Fi.
 *
 * Setup (Arduino IDE 2.x):
 *   1. Boards Manager → install "esp32 by Espressif Systems"
 *   2. Select board: FireBeetle-ESP32 (or ESP32 Dev Module)
 *   3. Library Manager → install ArduinoJson v7 by Benoit Blanchon
 *   4. Upload at 115200 baud
 *
 * Refer to Wiring-Guide.md for complete wiring instructions.
 * =============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// =============================================================================
// PIN DEFINITIONS (verify with your FireBeetle pinout before soldering)
// =============================================================================
#define PIN_RELAY_PUMP   25      // Relay CH1 — controls 12V diaphragm pump
#define PIN_RELAY_VALVE  26      // Relay CH2 — controls 12V solenoid valve
#define PIN_FLOW         27      // Flow sensor pulse input (through voltage divider!)
#define LOGGER_RX        16      // ESP32 RX  <- logger TX  (Serial Data Logger)
#define LOGGER_TX        17      // ESP32 TX  -> logger RX

// Soil moisture sensors — MUST be on ADC1 (ADC2 is disabled when Wi-Fi is active)
const uint8_t SOIL_PIN[3] = {36, 39, 34};   // A0, A1, A2 on the IO Shield

#define RELAY_ON   LOW           // Active-LOW relay module (signal LOW = relay ON)
#define RELAY_OFF  HIGH

// =============================================================================
// CONFIGURATION CONSTANTS
// =============================================================================
#define API_TOKEN          "sprk_9e33b52a39bd26e323871d6cd01c483e"  // Shared token — must match server & mobile app
#define DEFAULT_CLOUD_URL  "https://sprinkler-server.onrender.com" // Cloud Relay Server URL (Render)
#define FLOW_PPL           450.0f                           // Pulses per litre (from flow sensor datasheet)
#define RTC_ADDR           0x32                             // SD2405 RTC I2C address

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================
Preferences    prefs;                // Non-volatile storage (NVS) for settings
WebServer      server(80);           // HTTP server on port 80
HardwareSerial Logger(2);            // UART2 for the Serial Data Logger

// =============================================================================
// CONFIGURATION & CALIBRATION STRUCTURES
// =============================================================================

/**
 * Runtime configuration — stored in NVS, editable from the app.
 * All timing/threshold values have validation ranges enforced by the API.
 */
struct Config {
  uint8_t  lowPct      = 35;    // Start watering when fused moisture drops below this %
  uint8_t  highPct     = 60;    // Stop watering when fused moisture reaches this %
  uint8_t  maxCycles   = 4;     // Maximum pulse-soak cycles per session
  uint16_t pulseSec    = 60;    // Pump ON duration per pulse (seconds)
  uint16_t soakSec     = 300;   // Pause between pulses for soil absorption (seconds)
  uint16_t maxRunSec   = 900;   // Hard cap: total pump runtime per session (seconds)
  uint16_t cooldownMin = 30;    // Minimum gap between sessions (minutes)
  uint16_t w1s = 360;           // Window 1 start: 06:00 (minutes-of-day)
  uint16_t w1e = 540;           // Window 1 end:   09:00
  uint16_t w2s = 1020;          // Window 2 start: 17:00
  uint16_t w2e = 1110;          // Window 2 end:   18:30
  float    maxLiters   = 40.0f; // Hard cap: litres per session
  float    minFlow     = 0.3f;  // Below this while pumping = dry-run fault (L/min)
  bool     autoMode    = true;  // Automatic watering enabled
  bool     rainSkip    = false; // Skip watering (set by user when rain expected)
} cfg;

/**
 * Per-sensor 2-point calibration.
 * dry[] = raw ADC reading in air (higher voltage = drier)
 * wet[] = raw ADC reading in water (lower voltage = wetter)
 * Calibrate via the app: hold sensors in air → "Set DRY", dip in water → "Set WET".
 */
struct Cal {
  int dry[3] = {3100, 3100, 3100};
  int wet[3] = {1450, 1450, 1450};
} cal;

// =============================================================================
// STATE MACHINE TYPES
// Must be declared before the first function because Arduino IDE auto-generates
// prototypes above your functions, and they need these types to compile.
// =============================================================================
enum State : uint8_t { ST_IDLE, ST_WATERING, ST_SOAK, ST_MANUAL, ST_FAULT };
enum Fault : uint8_t { F_NONE, F_NO_FLOW, F_LEAK, F_SENSOR };

const char* STN[] = {"IDLE", "WATERING", "SOAK", "MANUAL", "FAULT"};
const char* FN[]  = {"NONE", "NO_FLOW", "LEAK", "SENSOR"};

State st    = ST_IDLE;
Fault fault = F_NONE;

// Persist config/calibration to NVS
void saveCfg() { prefs.putBytes("cfg", &cfg, sizeof(cfg)); }
void saveCal() { prefs.putBytes("cal", &cal, sizeof(cal)); }

// =============================================================================
// RTC DRIVER — SD2405 (raw I2C)
// Address 0x32, BCD time registers 0x00–0x06.
// Write-unlock via registers 0x10/0x0F.
// If time reads wrong, cross-check with DFRobot's SD2405 example.
// =============================================================================

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

void rtcWriteReg(uint8_t r, uint8_t v) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(r);
  Wire.write(v);
  Wire.endTransmission();
}

/**
 * Read current time from the RTC into a tm struct.
 * Returns false if I2C communication fails.
 */
bool rtcRead(tm &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;

  uint8_t s  = Wire.read();   // seconds
  uint8_t mi = Wire.read();   // minutes
  uint8_t h  = Wire.read();   // hours
  uint8_t wd = Wire.read();   // weekday (unused)
  uint8_t d  = Wire.read();   // day
  uint8_t mo = Wire.read();   // month
  uint8_t y  = Wire.read();   // year (2-digit)
  (void)wd;

  t.tm_sec  = bcd2dec(s  & 0x7F);
  t.tm_min  = bcd2dec(mi & 0x7F);
  t.tm_hour = bcd2dec(h  & 0x3F);
  t.tm_mday = bcd2dec(d  & 0x3F);
  t.tm_mon  = bcd2dec(mo & 0x1F) - 1;   // tm_mon is 0-based
  t.tm_year = bcd2dec(y) + 100;          // tm_year is years since 1900
  t.tm_isdst = 0;
  return true;
}

/**
 * Set the RTC to a specific date/time.
 * Unlock → write 7 BCD bytes → lock.
 */
void rtcSet(int y, int mo, int d, int h, int mi, int s) {
  rtcWriteReg(0x10, 0x80);                       // unlock write-protect
  rtcWriteReg(0x0F, 0x84);

  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(s));
  Wire.write(dec2bcd(mi));
  Wire.write(0x80 | dec2bcd(h));                  // bit7 = 24-hour mode
  Wire.write(1);                                  // weekday (unused, set to 1)
  Wire.write(dec2bcd(d));
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(y % 100));
  Wire.endTransmission();

  rtcWriteReg(0x0F, 0x00);                        // re-lock
  rtcWriteReg(0x10, 0x00);
}

// Clock state
tm     nowTm     = {};
time_t nowEpoch  = 0;
bool   rtcOk     = false;
bool   timeValid = false;
char   tsBuf[24] = "";

/**
 * Read the RTC and update all clock-related globals.
 * timeValid requires the year to be >= 2025 (catches unset RTCs).
 */
void readClock() {
  tm t = {};
  if (rtcRead(t)) {
    nowTm     = t;
    nowEpoch  = mktime(&t);
    rtcOk     = true;
    timeValid = (nowTm.tm_year + 1900) >= 2025;
    strftime(tsBuf, sizeof tsBuf, "%Y-%m-%d %H:%M:%S", &nowTm);
  } else {
    rtcOk = false;
  }
}

/** Current time as minutes-of-day (0–1439) for watering window checks. */
int minOfDay() { return nowTm.tm_hour * 60 + nowTm.tm_min; }

/** Check if the current time falls inside either watering window. */
bool inWindow() {
  int m = minOfDay();
  return (m >= cfg.w1s && m < cfg.w1e) || (m >= cfg.w2s && m < cfg.w2e);
}

// =============================================================================
// SOIL MOISTURE SENSOR PIPELINE
// 3 capacitive sensors → median-of-9 per sensor → raw→% calibration →
// EMA low-pass → fault check → sensor fusion (median of valid sensors)
// =============================================================================

float pct[3]  = {50, 50, 50};    // Per-sensor filtered moisture %
float fused   = 50;              // Fused (final) moisture reading
int   raws[3];                   // Raw ADC values (for diagnostics)
bool  ok[3];                     // Per-sensor health flag
int   nOk     = 0;               // Count of healthy sensors
bool  emaInit = false;            // EMA filter initialised flag

/**
 * Read a single ADC pin with median-of-9 filtering.
 * Takes 9 rapid samples, sorts them, returns the middle value.
 * This eliminates spike noise from Wi-Fi radio interference.
 */
int readRaw(uint8_t pin) {
  int v[9];
  for (int i = 0; i < 9; i++) {
    v[i] = analogRead(pin);
    delayMicroseconds(300);
  }
  // Insertion sort (fast for n=9)
  for (int i = 1; i < 9; i++) {
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[4];  // median
}

/**
 * Convert raw ADC to moisture percentage using 2-point calibration.
 * Capacitive sensors: lower voltage = wetter soil.
 * Result clamped to 0–100%.
 */
float rawToPct(int i, int raw) {
  int span = cal.dry[i] - cal.wet[i];
  if (span < 200) span = 200;   // prevent division issues with bad calibration
  return constrain(100.0f * (cal.dry[i] - raw) / (float)span, 0.0f, 100.0f);
}

/**
 * Full sensor pipeline — called once per second:
 * 1. Read each sensor (median-of-9)
 * 2. Fault check (raw < 300 = shorted, raw > 4000 = unplugged)
 * 3. Raw → % with per-sensor calibration
 * 4. EMA low-pass (α = 0.15) for smooth readings
 * 5. Sensor fusion: median of valid sensors (needs ≥ 2)
 */
void sampleSoil() {
  float v[3];
  nOk = 0;
  for (int i = 0; i < 3; i++) {
    raws[i] = readRaw(SOIL_PIN[i]);
    ok[i] = raws[i] > 300 && raws[i] < 4000;
    if (!ok[i]) continue;

    float p = rawToPct(i, raws[i]);
    pct[i] = emaInit ? pct[i] + 0.15f * (p - pct[i]) : p;  // EMA low-pass
    v[nOk++] = pct[i];
  }
  emaInit = true;

  // Sensor fusion
  if (nOk == 3) {
    float a = v[0], b = v[1], c = v[2];
    fused = max(min(a, b), min(max(a, b), c));   // median of three
  } else if (nOk == 2) {
    fused = (v[0] + v[1]) / 2;                   // average of two
  }
  // If nOk < 2, fused retains its last value (fail-safe: no update)
}

// =============================================================================
// FLOW SENSOR (Hall-effect, interrupt-driven)
// 450 pulses = 1 litre. Output filtered with EMA for stable L/min reading.
// =============================================================================

portMUX_TYPE   mux    = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulses = 0;

/** ISR — counts falling-edge pulses from the flow sensor. */
void IRAM_ATTR isrFlow() {
  portENTER_CRITICAL_ISR(&mux);
  pulses++;
  portEXIT_CRITICAL_ISR(&mux);
}

float flowLpm  = 0;    // Current flow rate (litres per minute), EMA-filtered
float sessionL = 0;    // Litres delivered in the current session
float totalL   = 0;    // Lifetime litres (persisted in NVS)

/**
 * Called once per second. Reads and resets the pulse counter,
 * converts to litres, updates flow rate (EMA) and cumulative totals.
 */
void updateFlow() {
  portENTER_CRITICAL(&mux);
  uint32_t p = pulses;
  pulses = 0;
  portEXIT_CRITICAL(&mux);

  float litres = p / FLOW_PPL;
  flowLpm   = 0.7f * flowLpm + 0.3f * (litres * 60.0f);   // EMA filter
  sessionL += litres;
  totalL   += litres;
}

// =============================================================================
// OUTPUT CONTROL (Pump & Valve)
// =============================================================================

bool     pumpOn       = false;
bool     valveOn      = false;
uint32_t valveCloseAt = 0;       // Delayed valve close (millis timestamp)

void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PIN_RELAY_PUMP, on ? RELAY_ON : RELAY_OFF);
}

void setValve(bool on) {
  valveOn = on;
  digitalWrite(PIN_RELAY_VALVE, on ? RELAY_ON : RELAY_OFF);
}

/** Emergency all-off: immediately cuts pump and valve. */
void allOff() {
  setPump(false);
  setValve(false);
  valveCloseAt = 0;
}

// =============================================================================
// LOGGING — CSV to microSD via Serial Data Logger (UART)
// Format: time,m1,m2,m3,avg,state,pump,valve,flow_lpm,session_l,event
// =============================================================================

void logEvent(const char* ev) {
  char b[200];
  snprintf(b, sizeof b, "%s,%.0f,%.0f,%.0f,%.0f,%s,%d,%d,%.2f,%.2f,%s",
           tsBuf, pct[0], pct[1], pct[2], fused,
           STN[st], pumpOn, valveOn, flowLpm, sessionL, ev);
  Logger.println(b);   // Non-blocking: logger handles SD write
}

// =============================================================================
// HISTORY RING BUFFER — RAM, 24 hours at 5-minute intervals (288 samples)
// Feeds the app's sparkline chart. Lost on reboot (by design).
// =============================================================================

struct Sample { uint32_t t; uint8_t avg; uint8_t wet; };
Sample   hist[288];
uint16_t hHead  = 0;
uint16_t hCount = 0;

void pushHist() {
  hist[hHead] = { (uint32_t)nowEpoch, (uint8_t)fused, (uint8_t)(pumpOn ? 1 : 0) };
  hHead = (hHead + 1) % 288;
  if (hCount < 288) hCount++;
}

// =============================================================================
// CONTROL STATE MACHINE
// States: IDLE → WATERING ↔ SOAK → IDLE (or FAULT)
//         MANUAL → IDLE (or FAULT)
// =============================================================================

uint32_t stMs       = 0;       // State-entry timestamp (millis)
uint32_t lastEndMs  = 0;       // Last session end timestamp
uint32_t manualSec  = 0;       // Manual run duration
uint32_t tick       = 0;       // 1 Hz tick counter since boot
uint8_t  cycles     = 0;       // Pulse count in current session
uint8_t  leakTicks  = 0;       // Consecutive ticks with unexpected flow
uint16_t runSec     = 0;       // Cumulative pump seconds in current session
bool     everRan    = false;   // True after first session (for cooldown logic)

void     enter(State s) { st = s; stMs = millis(); }
uint32_t elapsed()      { return (millis() - stMs) / 1000; }

/** Start a new pulse: open valve → enter WATERING state. */
void beginPulse() {
  cycles++;
  valveCloseAt = 0;
  setValve(true);
  enter(ST_WATERING);
  logEvent("PULSE_START");
}

/** End current pulse: pump off, schedule delayed valve close, enter SOAK. */
void endPulse() {
  setPump(false);
  valveCloseAt = millis() + 2000;   // Valve closes 2s after pump (prevents water hammer)
  enter(ST_SOAK);
  logEvent("PULSE_END");
}

/** Begin a new irrigation session. */
void startSession(const char* why) {
  sessionL = 0;
  cycles   = 0;
  runSec   = 0;
  logEvent(why);
  beginPulse();
}

/** End the current session gracefully. */
void endSession(const char* why) {
  setPump(false);
  valveCloseAt = millis() + 2000;
  lastEndMs    = millis();
  everRan      = true;
  enter(ST_IDLE);
  logEvent(why);
  prefs.putFloat("tot", totalL);   // Persist lifetime litres
}

/** Trip a fault: all outputs OFF, latch until manual reset from the app. */
void trip(Fault f) {
  allOff();
  fault = f;
  enter(ST_FAULT);
  logEvent(FN[f]);
}

/**
 * Main control tick — called once per second.
 * Implements the complete state machine with all safety interlocks.
 */
void controlTick() {
  bool sensorsOk = nOk >= 2;

  switch (st) {

    // ---- IDLE: wait for conditions to start automatic watering ----
    case ST_IDLE:
      if (cfg.autoMode && !cfg.rainSkip && sensorsOk && rtcOk && timeValid &&
          tick > 20 &&                                          // Boot stabilisation
          fused < cfg.lowPct && inWindow() &&                   // Soil is dry & in window
          (!everRan || (millis() - lastEndMs) / 60000UL >= cfg.cooldownMin))
        startSession("AUTO_START");
      break;

    // ---- WATERING: pump is running, delivering a pulse ----
    case ST_WATERING: {
      uint32_t e = elapsed();
      runSec++;

      // Valve-first sequencing: pump starts 1s after valve opens
      if (e >= 1 && !pumpOn) setPump(true);

      // Safety interlocks
      if (!sensorsOk)                        { trip(F_SENSOR);  break; }
      if (e >= 10 && flowLpm < cfg.minFlow)  { trip(F_NO_FLOW); break; }   // Dry-run / blocked

      // Session end conditions
      if (fused >= cfg.highPct)              { endSession("TARGET_REACHED"); break; }
      if (sessionL >= cfg.maxLiters)         { endSession("VOLUME_CAP");     break; }
      if (runSec >= cfg.maxRunSec)           { endSession("RUNTIME_CAP");    break; }

      // Pulse duration reached → soak
      if (e >= cfg.pulseSec) endPulse();
      break;
    }

    // ---- SOAK: waiting for water to absorb into soil ----
    case ST_SOAK:
      if (elapsed() >= cfg.soakSec) {
        if (fused >= cfg.highPct)         endSession("TARGET_REACHED");
        else if (!sensorsOk)             endSession("SENSOR_ABORT");
        else if (cycles >= cfg.maxCycles) endSession("MAX_CYCLES");
        else                             beginPulse();    // Another pulse
      }
      break;

    // ---- MANUAL: user-triggered run, always time-limited ----
    case ST_MANUAL: {
      uint32_t e = elapsed();
      if (e >= 1 && !pumpOn) setPump(true);
      if (e >= 10 && flowLpm < cfg.minFlow) { trip(F_NO_FLOW); break; }
      if (e >= manualSec) endSession("MANUAL_DONE");
      break;
    }

    // ---- FAULT: latched, all outputs off, wait for user reset ----
    case ST_FAULT:
      break;
  }

  // ---- LEAK DETECTION (runs in all non-fault states) ----
  // If valve + pump are OFF but water is still flowing → leak
  if (!valveOn && !pumpOn && st != ST_FAULT) {
    if (flowLpm > 0.5f) {
      if (++leakTicks >= 10) trip(F_LEAK);
    } else {
      leakTicks = 0;
    }
  } else {
    leakTicks = 0;
  }
}

/**
 * Start a manual watering run (from the app).
 * Duration clamped to 5–600 seconds. Only works from IDLE state.
 */
bool manualStart(uint32_t sec) {
  if (st != ST_IDLE) return false;
  manualSec    = constrain(sec, 5, 600);
  sessionL     = 0;
  valveCloseAt = 0;
  setValve(true);
  enter(ST_MANUAL);
  logEvent("MANUAL_START");
  return true;
}

/** Stop any active watering immediately (from the app). */
void stopNow() {
  if (st == ST_WATERING || st == ST_SOAK || st == ST_MANUAL)
    endSession("USER_STOP");
}

/** Clear a latched fault (from the app, after user inspection). */
void resetFault() {
  if (st != ST_FAULT) return;
  allOff();
  fault     = F_NONE;
  lastEndMs = millis();
  everRan   = true;
  enter(ST_IDLE);
  logEvent("FAULT_CLEARED");
}

// =============================================================================
// HTTP REST API
// All requests require header: X-Token: <API_TOKEN>
// =============================================================================

/** Verify the X-Token header. Returns false and sends 401 if invalid. */
bool auth() {
  if (server.header("X-Token") != API_TOKEN) {
    server.send(401, "application/json", "{\"err\":\"auth\"}");
    return false;
  }
  return true;
}

void sendJson(JsonDocument &d, int code) {
  String o;
  serializeJson(d, o);
  server.send(code, "application/json", o);
}

void sendOk()              { server.send(200, "application/json", "{\"ok\":true}"); }
void sendErr(const char* m) {
  String o = String("{\"err\":\"") + m + "\"}";
  server.send(400, "application/json", o);
}

bool body(JsonDocument &d) { return !deserializeJson(d, server.arg("plain")); }

// ---- GET /api/status — Live system state ----
void hStatus() {
  if (!auth()) return;
  JsonDocument d;
  JsonArray m = d["moist"].to<JsonArray>();
  for (int i = 0; i < 3; i++) m.add(ok[i] ? (int)pct[i] : -1);
  d["avg"]       = (int)fused;
  d["state"]     = STN[st];
  d["fault"]     = FN[fault];
  d["pump"]      = pumpOn;
  d["valve"]     = valveOn;
  d["flow"]      = flowLpm;
  d["session"]   = sessionL;
  d["total"]     = totalL;
  d["cycles"]    = cycles;
  d["auto"]      = cfg.autoMode;
  d["rainSkip"]  = cfg.rainSkip;
  d["sensorsOk"] = (nOk >= 2);
  d["timeValid"] = (rtcOk && timeValid);
  d["time"]      = tsBuf;
  d["rssi"]      = WiFi.RSSI();
  d["soakLeft"]  = (st == ST_SOAK && cfg.soakSec > elapsed())
                     ? (int)(cfg.soakSec - elapsed()) : 0;
  sendJson(d, 200);
}

// Macros for config get/set
#define GETF(k) d[#k] = cfg.k
#define SETF(k) if (!d[#k].isNull()) cfg.k = d[#k].as<decltype(cfg.k)>()

// ---- GET /api/config — Read all settings ----
void hCfgGet() {
  if (!auth()) return;
  JsonDocument d;
  GETF(lowPct); GETF(highPct); GETF(maxCycles); GETF(pulseSec); GETF(soakSec);
  GETF(maxRunSec); GETF(cooldownMin); GETF(w1s); GETF(w1e); GETF(w2s); GETF(w2e);
  GETF(maxLiters); GETF(minFlow); GETF(autoMode); GETF(rainSkip);
  sendJson(d, 200);
}

// ---- POST /api/config — Update settings (partial JSON) ----
void hCfgSet() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");

  Config old = cfg;
  SETF(lowPct); SETF(highPct); SETF(maxCycles); SETF(pulseSec); SETF(soakSec);
  SETF(maxRunSec); SETF(cooldownMin); SETF(w1s); SETF(w1e); SETF(w2s); SETF(w2e);
  SETF(maxLiters); SETF(minFlow); SETF(autoMode); SETF(rainSkip);

  // Validate all values
  bool valid = cfg.lowPct + 5 <= cfg.highPct && cfg.highPct <= 100 &&
               cfg.pulseSec >= 5 && cfg.pulseSec <= 600 &&
               cfg.soakSec >= 30 && cfg.soakSec <= 3600 &&
               cfg.maxCycles >= 1 && cfg.maxCycles <= 10 &&
               cfg.maxLiters > 0 && cfg.maxRunSec >= cfg.pulseSec &&
               cfg.w1s < 1440 && cfg.w1e <= 1440 &&
               cfg.w2s < 1440 && cfg.w2e <= 1440;

  if (!valid) { cfg = old; return sendErr("invalid values"); }
  saveCfg();
  sendOk();
}

// ---- POST /api/manual — Water now for N seconds ----
void hManual() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");
  if (!manualStart(d["sec"] | 60)) return sendErr("busy or fault");
  sendOk();
}

// ---- POST /api/stop — Emergency stop ----
void hStop() {
  if (!auth()) return;
  stopNow();
  sendOk();
}

// ---- POST /api/reset — Clear latched fault ----
void hReset() {
  if (!auth()) return;
  resetFault();
  sendOk();
}

// ---- POST /api/time — Sync RTC from phone clock ----
void hTime() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");

  tm t = {};
  t.tm_year = (int)d["y"] - 1900;
  t.tm_mon  = (int)d["mo"] - 1;
  t.tm_mday = d["d"];
  t.tm_hour = d["h"];
  t.tm_min  = d["mi"];
  t.tm_sec  = d["s"];

  long diff = labs((long)(mktime(&t) - nowEpoch));
  // Only update if RTC is invalid or drift exceeds 5 seconds
  if (!timeValid || !rtcOk || diff > 5) {
    rtcSet(d["y"], d["mo"], d["d"], d["h"], d["mi"], d["s"]);
    readClock();
  }
  sendOk();
}

// ---- POST /api/calibrate — Capture dry/wet calibration point ----
void hCal() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");

  String k = d["kind"] | "";
  for (int i = 0; i < 3; i++) {
    int r = readRaw(SOIL_PIN[i]);
    if      (k == "dry") cal.dry[i] = r;
    else if (k == "wet") cal.wet[i] = r;
    else return sendErr("kind");
  }
  saveCal();
  sendOk();
}

// ---- GET /api/history — Last 24h moisture data (5-min steps) ----
void hHistory() {
  if (!auth()) return;
  JsonDocument d;
  d["step"] = 300;
  JsonArray v = d["v"].to<JsonArray>();
  JsonArray w = d["w"].to<JsonArray>();
  uint16_t start = (hHead + 288 - hCount) % 288;
  for (uint16_t i = 0; i < hCount; i++) {
    Sample &s = hist[(start + i) % 288];
    v.add(s.avg);
    w.add(s.wet);
  }
  sendJson(d, 200);
}

// ---- POST /api/wifi — Store Wi-Fi credentials and reboot ----
void hWifi() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");
  prefs.putString("ssid", d["ssid"] | "");
  prefs.putString("pass", d["pass"] | "");
  sendOk();
  delay(500);
  ESP.restart();
}

// ---- GET /api/wifi/scan — Scan nearby 2.4GHz Wi-Fi networks ----
void hWifiScan() {
  if (!auth()) return;
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
  sendJson(d, 200);
}

// ---- GET /api/cloud — Get configured cloud relay endpoint ----
void hCloudGet() {
  if (!auth()) return;
  JsonDocument d;
  d["cloudUrl"] = prefs.getString("cloudUrl", DEFAULT_CLOUD_URL);
  sendJson(d, 200);
}

// ---- POST /api/cloud — Set cloud relay endpoint ----
void hCloudSet() {
  if (!auth()) return;
  JsonDocument d;
  if (!body(d)) return sendErr("json");
  String u = d["cloudUrl"] | DEFAULT_CLOUD_URL;
  prefs.putString("cloudUrl", u);
  sendOk();
}

// =============================================================================
// CLOUD RELAY SYNC (ESP32 <-> Render Server <-> Mobile App)
// =============================================================================
uint32_t lastCloudSync = 0;

void syncCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  String cloudUrl = prefs.getString("cloudUrl", DEFAULT_CLOUD_URL);
  if (cloudUrl.length() == 0 || cloudUrl == "none") return;

  HTTPClient http;
  String endpoint = cloudUrl + "/api/device/telemetry";
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", API_TOKEN);
  http.setTimeout(4000);

  // Serialize current status
  JsonDocument outDoc;
  JsonArray m = outDoc["moist"].to<JsonArray>();
  for (int i = 0; i < 3; i++) m.add(ok[i] ? (int)pct[i] : -1);
  outDoc["avg"]       = (int)fused;
  outDoc["state"]     = STN[st];
  outDoc["fault"]     = FN[fault];
  outDoc["pump"]      = pumpOn;
  outDoc["valve"]     = valveOn;
  outDoc["flow"]      = flowLpm;
  outDoc["session"]   = sessionL;
  outDoc["total"]     = totalL;
  outDoc["cycles"]    = cycles;
  outDoc["auto"]      = cfg.autoMode;
  outDoc["rainSkip"]  = cfg.rainSkip;
  outDoc["sensorsOk"] = (nOk >= 2);
  outDoc["timeValid"] = (rtcOk && timeValid);
  outDoc["time"]      = tsBuf;
  outDoc["rssi"]      = WiFi.RSSI();
  outDoc["soakLeft"]  = (st == ST_SOAK && cfg.soakSec > elapsed())
                          ? (int)(cfg.soakSec - elapsed()) : 0;

  String payload;
  serializeJson(outDoc, payload);

  int httpCode = http.POST(payload);
  if (httpCode == 200) {
    String resp = http.getString();
    JsonDocument inDoc;
    if (!deserializeJson(inDoc, resp)) {
      JsonArray cmds = inDoc["commands"].as<JsonArray>();
      for (JsonObject cmd : cmds) {
        String act = cmd["action"] | "";
        if (act == "manual") {
          manualStart(cmd["sec"] | 120);
        } else if (act == "stop") {
          stopNow();
        } else if (act == "reset") {
          resetFault();
        } else if (act == "config") {
          JsonObject cd = cmd["data"];
          if (!cd.isNull()) {
            if (!cd["lowPct"].isNull())    cfg.lowPct = cd["lowPct"];
            if (!cd["highPct"].isNull())   cfg.highPct = cd["highPct"];
            if (!cd["maxCycles"].isNull()) cfg.maxCycles = cd["maxCycles"];
            if (!cd["pulseSec"].isNull())  cfg.pulseSec = cd["pulseSec"];
            if (!cd["soakSec"].isNull())   cfg.soakSec = cd["soakSec"];
            if (!cd["maxLiters"].isNull()) cfg.maxLiters = cd["maxLiters"];
            if (!cd["autoMode"].isNull())  cfg.autoMode = cd["autoMode"];
            if (!cd["rainSkip"].isNull())  cfg.rainSkip = cd["rainSkip"];
            saveCfg();
          }
        } else if (act == "calibrate") {
          String k = cmd["kind"] | "";
          for (int i = 0; i < 3; i++) {
            int r = readRaw(SOIL_PIN[i]);
            if (k == "dry") cal.dry[i] = r;
            else if (k == "wet") cal.wet[i] = r;
          }
          saveCal();
        } else if (act == "wifi") {
          String s = cmd["ssid"] | "";
          String p = cmd["pass"] | "";
          if (s.length() > 0) {
            prefs.putString("ssid", s);
            prefs.putString("pass", p);
            delay(500);
            ESP.restart();
          }
        }
      }
    }
  }
  http.end();
}

// =============================================================================
// WI-FI — Station mode (join home/hotspot) with AP fallback
// =============================================================================

void startWifi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    // Wait up to 15 seconds for connection
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  }

  // Fallback: create own access point → http://192.168.4.1
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Sprinkler-AP", "sprinkler123");
  }

  MDNS.begin("sprinkler");
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);

  // Set relay pins to OFF *before* configuring as output → no boot glitch
  digitalWrite(PIN_RELAY_PUMP,  RELAY_OFF);
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  digitalWrite(PIN_RELAY_VALVE, RELAY_OFF);
  pinMode(PIN_RELAY_VALVE, OUTPUT);

  // ADC configuration
  analogReadResolution(12);                               // 0–4095
  for (uint8_t p : SOIL_PIN) analogSetPinAttenuation(p, ADC_11db);  // Full 0–3.3V range

  // Flow sensor interrupt
  pinMode(PIN_FLOW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), isrFlow, FALLING);

  // I2C for RTC
  Wire.begin();

  // UART for Serial Data Logger (check your logger's default baud rate)
  Logger.begin(9600, SERIAL_8N1, LOGGER_RX, LOGGER_TX);

  // Load saved settings from NVS
  prefs.begin("spr", false);
  if (prefs.getBytesLength("cfg") == sizeof(cfg)) prefs.getBytes("cfg", &cfg, sizeof(cfg));
  if (prefs.getBytesLength("cal") == sizeof(cal)) prefs.getBytes("cal", &cal, sizeof(cal));
  totalL = prefs.getFloat("tot", 0);

  // Start Wi-Fi and HTTP server
  startWifi();
  const char* hdrs[] = {"X-Token"};
  server.collectHeaders(hdrs, 1);
  server.on("/api/status",    HTTP_GET,  hStatus);
  server.on("/api/config",    HTTP_GET,  hCfgGet);
  server.on("/api/config",    HTTP_POST, hCfgSet);
  server.on("/api/manual",    HTTP_POST, hManual);
  server.on("/api/stop",      HTTP_POST, hStop);
  server.on("/api/reset",     HTTP_POST, hReset);
  server.on("/api/time",      HTTP_POST, hTime);
  server.on("/api/calibrate", HTTP_POST, hCal);
  server.on("/api/history",   HTTP_GET,  hHistory);
  server.on("/api/wifi",      HTTP_POST, hWifi);
  server.on("/api/wifi/scan", HTTP_GET,  hWifiScan);
  server.on("/api/cloud",     HTTP_GET,  hCloudGet);
  server.on("/api/cloud",     HTTP_POST, hCloudSet);
  server.begin();

  // Let the Serial Data Logger boot before first write
  delay(1500);
  Logger.println("time,m1,m2,m3,avg,state,pump,valve,flow_lpm,session_l,event");
  readClock();
  logEvent("BOOT");

  Serial.println("AutoSprinkler ready.");
  Serial.print("IP: ");
  Serial.println(WiFi.getMode() == WIFI_AP ? WiFi.softAPIP() : WiFi.localIP());
}

// =============================================================================
// MAIN LOOP — 1 Hz control tick + HTTP server + Wi-Fi reconnect + Cloud Sync
// =============================================================================

uint32_t lastTick    = 0;
uint32_t lastWifiTry = 0;

void loop() {
  server.handleClient();

  // 1 Hz control loop
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    tick++;

    readClock();
    sampleSoil();
    updateFlow();
    controlTick();

    // Log a sample every 60 seconds
    if (tick % 60 == 0) logEvent("SAMPLE");

    // Push history point every 5 minutes
    if (tick % 300 == 0) pushHist();
  }

  // Delayed valve close (2s after pump stops, prevents water hammer)
  if (valveCloseAt && (int32_t)(millis() - valveCloseAt) >= 0) {
    setValve(false);
    valveCloseAt = 0;
  }

  // Auto-reconnect Wi-Fi every 30s if in STA mode and disconnected
  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiTry > 30000) {
    lastWifiTry = millis();
    WiFi.reconnect();   // Non-blocking: control loop keeps running
  }

  // Cloud sync every 3s if connected to Wi-Fi
  if (WiFi.status() == WL_CONNECTED && millis() - lastCloudSync >= 3000) {
    lastCloudSync = millis();
    syncCloud();
  }
}
