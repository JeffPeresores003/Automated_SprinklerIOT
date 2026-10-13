# Automated Sprinkler Irrigation System — Implementation Guide

**Stack:** FireBeetle ESP32 (Arduino C++) · React Native (Expo) mobile app · solar powered · microSD logging
**Control approach:** *Cycle-soak hysteresis controller with flow-verified dosing and safety interlocks* (runs fully on the ESP32, the app is only a remote control + dashboard)

---

## 1. Goals and design principles

1. **Autonomous.** The ESP32 makes every irrigation decision itself. If Wi-Fi or the phone is gone, watering continues by schedule and soil readings.
2. **Safe by default.** The pump can never be left running: every run has a hard time cap, a volume cap, and a dry-run/leak watchdog.
3. **Fast.** Control loop = 1 Hz, O(1) math, no cloud round trip. Phone → ESP32 over local Wi-Fi responds in well under a second.
4. **Water-efficient.** Water in pulses with soak pauses so the soil absorbs it instead of running off, and only in cool hours (less evaporation).
5. **Data-rich.** Everything is logged to the memory card so you can tune thresholds with real data (and use it in your paper).

### Assumptions I made (please confirm)

| Assumption | Why it matters |
|---|---|
| **One irrigation zone**: 1 solenoid valve, 3 soil sensors placed in the same bed | 3 sensors = redundancy + median filtering, not 3 zones. Adding zones later = more valves + a 4-channel relay |
| **Relay module has ≥ 2 channels** (pump + valve) | Your list shows a "1/2/4 channel" listing with qty 1. Order the **2-channel** (or 4-channel for expansion) version |
| The **Fermion Serial Data Logger V2.0** is the "memory card" module | It is a UART ↔ microSD logger (OpenLog-style). Verify default baud/mode on the DFRobot wiki for your unit |
| Field has **no guaranteed Wi-Fi router** | So the ESP32 has a built-in access point fallback and can join a phone hotspot |

---

## 2. System architecture and tech stack

```
                      ┌───────────────────────────── SOLAR POWER ─────────────────────────────┐
                      │ 100W panel → 12V charge controller → 12V 25Ah gel battery → 10A fuse │
                      └───────┬───────────────────────────────┬───────────────────────────────┘
                              │ 12V                           │ 12V
                     ┌────────▼────────┐             ┌────────▼────────┐
                     │ Buck 12V → 5V   │             │ Relay module    │
                     │ (≥2A)           │             │ CH1: Pump       │
                     └────────┬────────┘             │ CH2: Solenoid   │
                              │ 5V                   └────────▲────────┘
        ┌─────────────────────▼──────────────────────────────┴──────┐
        │  FireBeetle ESP32 + IO Shield                              │
        │  ┌───────────────┐ ┌────────────┐ ┌────────────────────┐   │
        │  │ Control FSM   │ │ Web server │ │ Sensor pipeline    │   │
        │  │ (1 Hz)        │ │ REST/JSON  │ │ median→EMA→fusion  │   │
        │  └───────────────┘ └─────┬──────┘ └────────────────────┘   │
        └───▲────────▲────────▲────┼──────────▲───────────▲──────────┘
            │I2C     │ADC×3   │IRQ │          │UART       │Wi-Fi STA / AP
       ┌────┴───┐ ┌──┴─────┐ ┌┴────▼───┐ ┌───┴─────────┐ │
       │SD2405  │ │3× soil │ │Flow     │ │Serial Data  │ │
       │RTC     │ │moisture│ │sensor   │ │Logger+microSD│ │
       └────────┘ └────────┘ └─────────┘ └─────────────┘ │
                                                          │ HTTPS Sync (Every 3s)
                                                          ▼
                                            ┌───────────────────────────┐
                                            │ Cloud Relay Server        │
                                            │ (Node.js/Express on Render│
                                            │ https://sprinkler-server.onrender.com)
                                            └─────────────▲─────────────┘
                                                          │ HTTPS REST / Polling
                                                          ▼
                                            ┌───────────────────────────┐
                                            │ React Native App (Expo)   │
                                            │ (Access from anywhere:    │
                                            │  Cellular 4G/5G or Wi-Fi) │
                                            └───────────────────────────┘

 Water path:  Drum (float valve refill) → filter → 12V diaphragm pump → flow sensor
              → 12V solenoid valve → ½" pipe → micro-sprinkler nozzles
```

### 2.1 Tech stack

| Layer | Technology | Why this choice |
|---|---|---|
| **Firmware language / framework** | **C++ with the Arduino framework**, written in **Arduino IDE 2.x** with the **esp32 core by Espressif** | Fastest way to get a working ESP32 project, huge community, no extra toolchain. Everything needed (Wi-Fi, HTTPClient, web server, I2C, ADC, NVS) is built into the core |
| **Firmware libraries** | `WiFi`, `WebServer`, `HTTPClient`, `ESPmDNS`, `Preferences`, `Wire` (built in) + **`ArduinoJson` v7** (Library Manager) | Only one external library, minimizing conflicts |
| **Cloud Relay Server** | **Node.js / Express** deployed on **Render** (`https://sprinkler-server.onrender.com`) | Decouples phone and ESP32 from dynamic local IP addresses (`192.168.x.x`). Outbound HTTPS pings punch through firewalls, enabling remote monitoring from cellular data anywhere |
| **Local Fallback Protocol** | **HTTP REST + JSON** over direct SoftAP (`192.168.4.1`) or local LAN | Offline-first fail-safe: system remains fully operable even if the internet router is down |
| **In-App Wi-Fi Provisioning** | **ESP32 Wi-Fi Scanner (`/api/wifi/scan`)** + NVS credential storage | Scan and select networks from the phone UI without re-flashing firmware or modifying code |
| **Mobile app** | **React Native with Expo** (JavaScript), **Expo Go** for development | One codebase for Android and iOS, instant reload while you build, preset buttons for Cloud vs Local AP |
| **App libraries** | `react-native-svg` (chart), `@react-native-async-storage/async-storage` (saves endpoints), `react-native-safe-area-context` | Standard Expo-supported libraries |
| **On-device storage** | ESP32 **NVS** (settings, calibration, Wi-Fi credentials) · **RAM ring buffer** (24 h chart) · **microSD CSV** via the serial data logger (permanent log) | See section 4 |
| **Hardware interfaces** | **I2C** (RTC), **ADC1** (soil sensors), **GPIO interrupt** (flow pulses), **UART** (data logger), **GPIO** (relays) | Dedicated hardware interfaces for each sensor |

### 2.2 Cloud Relay & Wi-Fi Provisioning Architecture

1. **Elimination of IP Guessing:** 
   In local-only IoT setups, the user must look up dynamic router-assigned IPs (e.g. `192.168.1.142`) whenever the ESP32 reboots or changes networks. By introducing `https://sprinkler-server.onrender.com`:
   - The ESP32 pushes its status payload every 3 seconds to the cloud server.
   - The mobile app connects to `https://sprinkler-server.onrender.com` by default.
   - You can control the system while away from home using cellular data (4G/5G).
2. **In-App Wi-Fi Discovery (No Code Uploads):**
   - When moving the device to a new field or router, connect to the ESP32's hotspot `Sprinkler-AP` (`192.168.4.1`).
   - In the App's **Settings** screen, tap **"Scan Nearby"**. The ESP32 scans 2.4GHz channels via `WiFi.scanNetworks()` and lists nearby SSIDs and signal strengths.
   - Tap your desired Wi-Fi network, enter the passphrase, and hit **Save Credentials and Reboot ESP32**.
   - The ESP32 writes the credentials into non-volatile storage (`Preferences`) and reboots. No Arduino IDE or code re-upload is required.

**Quick API test via Cloud or Local:**

```bash
# Test Cloud Relay
curl -H "X-Token: sprk_9e33b52a39bd26e323871d6cd01c483e" https://sprinkler-server.onrender.com/api/status

# Test Local AP Fallback
curl -H "X-Token: change-me-123" http://192.168.4.1/api/status
curl -H "X-Token: change-me-123" http://192.168.4.1/api/wifi/scan
```

---

## 3. Components and what each one does

| # | Module | Role in this system | Notes / gotchas |
|---|---|---|---|
| 1 | **FireBeetle ESP32** | Brain. Dual-core 240 MHz, Wi-Fi + BLE. Runs the control loop, web server, sensor pipeline | Use **ADC1 pins only** for analog (ADC2 stops working when Wi-Fi is on) |
| 2 | **SD2405 RTC (I2C)** | Keeps accurate time (±5 ppm ≈ a few seconds/month) with its own backup battery. Gates watering windows and timestamps logs | Needed because the ESP32 loses time on reboot and Wi-Fi/NTP may be unavailable in the field |
| 3 | **IO Shield for FireBeetle 2** | Breaks out all pins to headers: 5 analog, digital, UART, I2C, power port | Makes wiring screw-terminal/jumper friendly |
| 4 | **Fermion Serial Data Logger V2.0 + microSD** | Permanent, power-loss-safe logbook. See **section 4** | Power it from **3.3 V** so its TX line is 3.3 V logic (safe for ESP32 RX) |
| 5 | **3× Capacitive soil moisture sensor** | Measures soil wetness (analog 1.2–2.5 V, *lower voltage = wetter*). Capacitive = no exposed metal electrodes, so it does not corrode like resistive probes | Needs 2-point calibration (air = dry, water = wet) |
| 6 | **Water flow sensor (½")** | Hall-effect pulse output, **450 pulses per litre**. Gives flow rate and delivered volume. Enables dry-run, blockage and leak detection | Rated 1–30 L/min, ±5 %. Output pulses are **5 V** → must be divided down to 3.3 V (section 6). Accuracy drops below ~1 L/min |
| 7 | **10 A relay module (12 V coil)** | Switches the 12 V pump and 12 V solenoid valve from a 3.3 V GPIO. Module is **active-LOW** (signal LOW = relay ON) | Check that your module accepts 3.3 V logic (opto-isolated boards with a separate JD-VCC jumper do). Put a **flyback diode** across each inductive load |
| 8 | **MicroSD card** | Storage medium for the logger | ≤ 32 GB, FAT32, branded / high-endurance card (it will sit in a hot enclosure) |
| 9 | **Water drum + float valve** | Reservoir. The mechanical float (level control) valve refills it automatically from the supply | Not electronic: the ESP32 cannot see tank level. The flow-sensor dry-run trip protects the pump if the drum runs empty |
| 10 | **12 V diaphragm pump (GOOD PUMPS, 72 W, 6 A, 6 L/min, 0.9 MPa)** | Pressurises the line for the micro-sprinklers | Self-priming with built-in pressure switch. Use 1N5408 flyback diode. |
| 11 | **12 V solenoid valve (½")** | Opens/closes the sprinkler line. Opens *before* the pump starts and closes *after* it stops (prevents dead-heading and water hammer) | Normally closed type = fails safe when power is lost |
| 12 | **Suction mesh filter + micro-sprinkler nozzles + fittings** | Stainless steel mesh filter protects the pump and tiny nozzle orifices from clogging | Submerge filter 5–10 cm above drum floor before pump intake |
| 13 | **100 W panel + charge controller + 12 V 25 Ah gel battery** | Off-grid power | Gel batteries should not be discharged below ~50 % → ~150 Wh usable. Daily consumption is small (electronics ≈ 10–15 Wh + pump a few minutes/day), so 100 W is generous |

### Parts missing from the list (add these)

- **12 V → 5 V buck converter, ≥ 2 A** (e.g. LM2596 / XL4015 type): powers the ESP32 board from the battery.
- **Flyback diodes** (1N5408, 3 A rated recommended for the 6 A pump; 1N4007 or 1N5408 for the solenoid valve) across each load.
- **2 resistors for the flow-sensor divider** (10 kΩ + 20 kΩ or 1 kΩ + 2 kΩ).
- **Inline fuse holder + 10 A fuse** on the battery positive.
- **IP65 enclosure, cable glands, terminal blocks, 16–18 AWG wire** for the 12 V side.
- *(Optional)* 100 kΩ + 22 kΩ divider to read battery voltage on an ADC pin, and a float switch for a real low-water cutoff.

### Why these parts matter

Ordered by how badly things go wrong without them.

**12 V → 5 V buck converter (≥ 2 A)**
The battery sits at roughly 11.5–14.4 V depending on charge, while the FireBeetle accepts only 3.3–5 V. Connecting it straight to the battery would destroy the board. A buck converter is used instead of a simple linear regulator because it wastes very little energy as heat, which matters on solar power and inside a hot enclosure. The 2 A rating is headroom: the ESP32's Wi-Fi radio draws short current spikes, and a supply that is too weak causes random brownout resets. Set it to 5.0 V with a multimeter *before* connecting the ESP32.

**Flyback diodes (across the pump and the solenoid valve)**
Both are coils. When the relay switches them off, the collapsing magnetic field creates a voltage spike far above 12 V. That spike arcs across the relay contacts, which pits them or even welds them shut, and a welded relay means a pump that will not turn off. The spike also travels through the wiring as noise, which can reset the ESP32, distort soil-sensor readings, or corrupt an SD write. The diode gives that energy a harmless path. Install it in reverse across the load, with the stripe (cathode) on the +12 V side.

**Two resistors for the flow-sensor divider (10 kΩ + 20 kΩ)**
The flow sensor outputs 5 V pulses, and ESP32 pins are only rated for 3.3 V. It may seem to work at first, but 5 V on a GPIO can damage that pin and slowly degrade the chip. The two resistors cut the pulse to about 3.3 V. They cost almost nothing, but a mistake here can kill the board.

**Inline 10 A fuse on the battery positive**
A gel battery can deliver hundreds of amps into a short circuit. Without a fuse, the wire itself becomes the fuse, meaning melted insulation and possibly a fire. The fuse protects the *wiring*, not the devices, so mount it as close to the battery as possible. Outdoors, vibration, moisture and rodents chewing cables make a short circuit realistic rather than theoretical. Check the pump's label: 10 A only works if the total load stays well below that.

**IP65 enclosure, cable glands, terminal blocks, 16–18 AWG wire**
Most field failures in outdoor electronics are not the electronics themselves. They are corroded jumper wires, moisture, insects and loose connections that cause intermittent, hard-to-diagnose faults.
- **Enclosure + glands:** keep rain and humidity off the boards.
- **Terminal blocks:** hold connections far better than breadboard jumpers under vibration.
- **Wire gauge:** the pump pulls several amps, so thin wire drops voltage and heats up, and the pump runs weakly or the system browns out.
- **Placement:** keep the enclosure in shade, since heat shortens the life of the battery, SD card and electronics.

**Optional: battery-voltage divider (100 kΩ + 22 kΩ)**
Gel batteries are damaged by repeated deep discharge. The charge controller usually has a low-voltage cutoff, but it cannot tell the ESP32 anything. Reading the battery voltage lets the app display it and lets the firmware skip watering when the battery is low. The divider scales 12–14 V down to a safe ~2.2–2.6 V for the ADC. Use an ADC1 pin (GPIO35 / A3) and calibrate against a multimeter.

**Optional: float switch for low-water cutoff**
The flow sensor only notices an empty drum after the pump has already run dry for about 10 seconds. A float switch detects low water *before* the pump starts and lets the app show "tank low" instead of a fault. Diaphragm pumps tolerate a short dry run but not repeated ones.

**If the budget forces a choice:** the buck converter, flyback diodes, divider resistors and fuse are *required*. The enclosure and proper wiring are strongly recommended for any outdoor install. The two optional items are upgrades.

---

## 4. Why a memory card — and what goes where

The ESP32 has three places to keep data. Each is used for what it is good at:

| Storage | What we keep there | Why |
|---|---|---|
| **RAM (ring buffer)** | Last 24 h of moisture samples (288 points @ 5 min) | Fast; feeds the app's history chart. Lost on reboot (by design — it's only a view) |
| **ESP32 flash (NVS / Preferences)** | Settings, calibration, lifetime litres | Tiny, rarely changes, must be readable instantly at boot. Flash has limited write cycles, so we never log here |
| **MicroSD via Serial Data Logger** | **Every sensor sample and every event**, as CSV | Large, cheap, removable, survives power loss |

### Reasons the memory card is the right choice

1. **Power-loss safe.** A solar/battery system *will* brown out at some point. The logger writes append-only text lines and flushes shortly after the last byte, so a sudden cut costs at most the last few seconds, and old data is never corrupted.
2. **Keeps the ESP32 fast.** A direct SD stack (FAT, SPI, card init) costs RAM and can block for tens of milliseconds on a write. The Serial Data Logger handles FAT/SD itself; the ESP32 just does `Logger.println(line)` over UART and moves on. It also leaves the ESP32's only SPI bus free.
3. **Protects the internal flash.** Logging every minute into flash would wear it out. The card absorbs the heavy write traffic.
4. **Real data for tuning and for your paper.** The CSV opens directly in Excel/Python. You can plot soil-moisture decay, litres per session, pump run-times, and calibrate `lowPct` / `highPct` from evidence instead of guesses.
5. **Audit trail.** Every fault (`NO_FLOW`, `LEAK`, `SENSOR`) is logged with the time and the readings that caused it, so failures can be explained later.
6. **Future ML.** A few weeks of logs is a ready-made training set if you later want a predictive model (see roadmap).

**Size check:** one ~100-byte line per minute ≈ 150 KB/day ≈ 50 MB/year. Even a 2 GB card lasts for years.

### Log format (CSV, one file per boot)

```
time,m1,m2,m3,avg,state,pump,valve,flow_lpm,session_l,event
2026-10-11 06:00:03,28,31,29,29,WATERING,1,1,2.10,0.35,PULSE_START
```

### Honest limitation

The logger is a *write-optimised* device: the ESP32 appends lines, but does not read files back during normal operation. So the **app's history chart comes from the RAM ring buffer**, and the **full history is read by pulling the card** (or later by using the logger's command mode). That is a deliberate trade-off for speed and simplicity.

---

## 5. The algorithm

### 5.1 What I chose and why

**Cycle-soak hysteresis control + flow-verified dosing + safety interlocks.**

| Option | Verdict |
|---|---|
| Single threshold on/off | Chatters around the threshold, over-waters, wastes energy. Rejected |
| **PID** | Needs a *continuous* output. A relay-switched pump is on/off, and soil responds with long delay (minutes), so PID tuning is fragile and gives no benefit. Rejected |
| Fuzzy logic | Works, but with essentially one meaningful input (moisture) it adds tuning effort for no real gain |
| ML / evapotranspiration models (FAO-56) | Needs weather inputs or training data you don't have yet. Good **Phase 3** once the SD logs exist |
| **Hysteresis + cycle-soak + flow check (chosen)** | Deterministic, explainable, runs in microseconds, no training, and the flow sensor closes the loop on *how much* water was really delivered |

### 5.2 Processing pipeline (every second)

```
3× raw ADC ──► median of 9 samples (kills spikes)
          ──► raw → % using per-sensor 2-point calibration
          ──► EMA low-pass filter (α = 0.15, smooths noise/Wi-Fi radio interference)
          ──► fault check per sensor (raw < 300 or > 4000 = unplugged/shorted → ignored)
          ──► sensor fusion: median of valid sensors (needs ≥ 2 valid)
          ──► `fused` moisture %

flow pulses ──► L/min and litres delivered (450 pulses = 1 L)
RTC          ──► minutes-of-day for watering windows
```

### 5.3 Start / stop rules

**Start a session when ALL are true:** auto mode on · not "rain skip" · ≥ 2 healthy sensors · RTC time valid · `fused < lowPct` · inside an allowed time window · cooldown since last session elapsed.

**During a session (repeat up to `maxCycles` times):**

1. **Pulse:** open valve → 1 s later start pump → run `pulseSec` seconds.
2. **Soak:** pump off, valve closes after 2 s, wait `soakSec` so water spreads through the soil and the sensors "see" it.
3. **Re-evaluate:** if `fused ≥ highPct` → done. Otherwise another pulse.

**A session ends early on:** target reached · `maxLiters` delivered · `maxRunSec` pump time · sensor failure · user STOP.

### 5.4 State machine

```
            fused<low & window & ok                 pulseSec elapsed
   ┌──────┐ ───────────────────────► ┌──────────┐ ────────────────► ┌──────┐
   │ IDLE │                          │ WATERING │                   │ SOAK │
   └──▲───┘ ◄─────────────────────── └────┬─────┘ ◄──────────────── └──┬───┘
      │      target / caps / stop          │   soakSec elapsed & fused<high & cycles left
      │                                    │ no flow / sensor lost
      │            reset (app)             ▼
      └──────────────────────────────── ┌───────┐   (also: LEAK detected while idle)
                                        │ FAULT │   all outputs OFF, latched until reset
                                        └───────┘
   MANUAL: app "water now N sec" (5–600 s, always time-limited) → IDLE
```

### 5.5 Safety interlocks (the part that makes it "production-grade")

| Protection | How |
|---|---|
| **Dry-run / blocked line** | Pump on ≥ 10 s and flow < `minFlow` → `FAULT: NO_FLOW`, everything off |
| **Leak / stuck valve** | Valve & pump OFF but flow > 0.5 L/min for 10 s → `FAULT: LEAK` |
| **No endless runs** | Every pulse, session and manual run has a time limit. App disconnecting can never leave the pump on |
| **Fail-safe sensing** | < 2 healthy sensors → no automatic watering (reported in the app) |
| **Fail-safe time** | RTC missing/unset → no automatic watering |
| **Valve sequencing** | Valve opens before pump, closes after pump |
| **Fault latch** | After a fault the user must inspect and press *Reset fault* in the app |

### 5.6 Default parameters (tune with real data)

| Parameter | Default | Meaning |
|---|---|---|
| `lowPct` / `highPct` | 35 / 60 | Start below / stop at-or-above (relative to your calibration, not true volumetric water content) |
| `pulseSec` / `soakSec` | 60 / 300 | Pump ON per pulse / pause between pulses |
| `maxCycles` | 4 | Pulses per session |
| `maxLiters` / `maxRunSec` | 40 / 900 | Hard caps per session |
| Windows | 06:00–09:00, 17:00–18:30 | Cooler hours, less evaporation |
| `cooldownMin` | 30 | Minimum gap between sessions (prevents hunting) |
| `minFlow` | 0.3 L/min | Below this while pumping = no flow |

---

## 6. Wiring

### 6.1 Pin map (suggested — verify against the FireBeetle pinout before soldering)

| Function | ESP32 GPIO | Notes |
|---|---|---|
| Relay CH1 (pump) | 25 | Active LOW |
| Relay CH2 (valve) | 26 | Active LOW |
| Flow sensor pulse | 27 | Through voltage divider (below) |
| Soil sensor 1 / 2 / 3 | 36 / 39 / 34 | ADC1, input-only (A0 / A1 / A2 on the shield) |
| I2C SDA / SCL (RTC) | 21 / 22 | Default I2C, RTC address `0x32` |
| Logger UART RX / TX | 16 / 17 | ESP32 RX ← logger TX; ESP32 TX → logger RX (use the shield's UART header if your board routes it elsewhere and change `LOGGER_RX/TX`) |

Avoid GPIO 0, 2, 12, 15 (boot strapping pins) for relays, and GPIO 6–11 (flash).

### 6.2 Flow sensor level shifting (important)

The sensor outputs a **5 V** pulse when powered at 5 V. ESP32 pins are **not 5 V tolerant**:

```
Flow OUT (5V) ──[ 10 kΩ ]──┬── GPIO27
                           │
                        [ 20 kΩ ]
                           │
                          GND        → 5 V × 20/30 = 3.33 V
```

### 6.3 Power tree

- Battery 12 V → **10 A fuse** → relay module (coil supply) + pump + valve + buck converter.
- Buck → **5 V** → FireBeetle power input / shield power port.
- Soil sensors, RTC, data logger → **3.3 V** rail (keeps the logger's TX at 3.3 V logic). Add a 100 µF capacitor near the logger.
- Flow sensor → 5 V (with divider on its output).
- Common ground for everything.
- Flyback diode across pump and valve (stripe/cathode to the +12 V side).
- After wiring, **power-cycle with the pump disconnected** and confirm neither relay clicks at boot.

### 6.4 Solar sanity check

100 W panel ≈ 300–400 Wh/day in realistic Philippine sun. Load: electronics ≈ 10–15 Wh/day + pump (read its label, e.g. 4 A × 12 V = 48 W × 20 min ≈ 16 Wh). Plenty of margin; the gel battery's ~150 Wh usable covers several cloudy days.

---

## 7. Firmware (Arduino IDE)

### 7.1 Arduino IDE setup

1. Install **Arduino IDE 2.x**.
2. **Boards Manager** → search `esp32` → install **"esp32 by Espressif Systems"**.
3. **Tools → Board → esp32** → pick the entry that matches your exact board (*FireBeetle-ESP32* or *DFRobot Firebeetle 2 ESP32-E*). If unsure, **ESP32 Dev Module** works.
4. **Library Manager** → install **ArduinoJson** by Benoit Blanchon, version **7.x** (the code uses the v7 `JsonDocument` API; v6 will not compile). Everything else is built into the ESP32 core.
5. Create a sketch folder named `AutoSprinkler` containing `AutoSprinkler.ino` (folder and file name must match), and paste the whole code below into that **single** file.
6. Select the port, then **Upload**. Open **Serial Monitor at 115200 baud**.
7. If no port appears: use a USB *data* cable and install the CP210x / CH340 USB-serial driver.

> **Why the enums are near the top:** Arduino IDE auto-generates function prototypes for `.ino` files, and it puts them *before* your type definitions. Any function that takes a custom type (here `State` and `Fault`) fails to compile if that type is defined after the first function. So the types are declared before all functions.

> **Confirm your board model.** Your list pairs a "FireBeetle ESP32" with an IO shield made for **FireBeetle 2 (ESP32-E/M0)**. Check the exact board you have, because pin numbering and the shield's analog pins (A0–A4) can differ. The GPIO numbers in section 6.1 are suggestions; verify them against your board's pinout.

Create `AutoSprinkler.ino`:

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>

// ================= PINS (verify with FireBeetle pinout) =================
#define PIN_RELAY_PUMP   25
#define PIN_RELAY_VALVE  26
#define PIN_FLOW         27
#define LOGGER_RX        16     // ESP32 RX  <- logger TX
#define LOGGER_TX        17     // ESP32 TX  -> logger RX
const uint8_t SOIL_PIN[3] = {36, 39, 34};   // ADC1 only
#define RELAY_ON   LOW          // active-LOW relay module
#define RELAY_OFF  HIGH

#define API_TOKEN  "change-me-123"          // must match the app
#define FLOW_PPL   450.0f                   // pulses per litre (datasheet)
#define RTC_ADDR   0x32                     // SD2405

Preferences prefs;
WebServer   server(80);
HardwareSerial Logger(2);

// ================= CONFIG & CALIBRATION =================
struct Config {
  uint8_t  lowPct = 35, highPct = 60, maxCycles = 4;
  uint16_t pulseSec = 60, soakSec = 300, maxRunSec = 900, cooldownMin = 30;
  uint16_t w1s = 360, w1e = 540, w2s = 1020, w2e = 1110;   // minutes-of-day
  float    maxLiters = 40.0f, minFlow = 0.3f;
  bool     autoMode = true, rainSkip = false;
} cfg;

struct Cal { int dry[3] = {3100, 3100, 3100}; int wet[3] = {1450, 1450, 1450}; } cal;

// Types used in function signatures MUST be declared before the first function
// (Arduino IDE auto-generates prototypes above your functions).
enum State : uint8_t { ST_IDLE, ST_WATERING, ST_SOAK, ST_MANUAL, ST_FAULT };
enum Fault : uint8_t { F_NONE, F_NO_FLOW, F_LEAK, F_SENSOR };
const char* STN[] = {"IDLE", "WATERING", "SOAK", "MANUAL", "FAULT"};
const char* FN[]  = {"NONE", "NO_FLOW", "LEAK", "SENSOR"};
State st = ST_IDLE; Fault fault = F_NONE;

void saveCfg() { prefs.putBytes("cfg", &cfg, sizeof(cfg)); }
void saveCal() { prefs.putBytes("cal", &cal, sizeof(cal)); }

// ================= RTC (SD2405, raw I2C) =================
uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

void rtcWriteReg(uint8_t r, uint8_t v) {
  Wire.beginTransmission(RTC_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission();
}
bool rtcRead(tm &t) {
  Wire.beginTransmission(RTC_ADDR); Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;
  uint8_t s = Wire.read(), mi = Wire.read(), h = Wire.read(), wd = Wire.read();
  uint8_t d = Wire.read(), mo = Wire.read(), y = Wire.read();
  (void)wd;
  t.tm_sec = bcd2dec(s & 0x7F);  t.tm_min = bcd2dec(mi & 0x7F);
  t.tm_hour = bcd2dec(h & 0x3F); t.tm_mday = bcd2dec(d & 0x3F);
  t.tm_mon = bcd2dec(mo & 0x1F) - 1; t.tm_year = bcd2dec(y) + 100; t.tm_isdst = 0;
  return true;
}
void rtcSet(int y, int mo, int d, int h, int mi, int s) {
  rtcWriteReg(0x10, 0x80); rtcWriteReg(0x0F, 0x84);            // unlock write
  Wire.beginTransmission(RTC_ADDR); Wire.write(0x00);
  Wire.write(dec2bcd(s)); Wire.write(dec2bcd(mi));
  Wire.write(0x80 | dec2bcd(h));                               // bit7 = 24h mode
  Wire.write(1);                                               // weekday (unused)
  Wire.write(dec2bcd(d)); Wire.write(dec2bcd(mo)); Wire.write(dec2bcd(y % 100));
  Wire.endTransmission();
  rtcWriteReg(0x0F, 0x00); rtcWriteReg(0x10, 0x00);            // lock
}

tm nowTm = {}; time_t nowEpoch = 0; bool rtcOk = false, timeValid = false; char tsBuf[24] = "";
void readClock() {
  tm t = {};
  if (rtcRead(t)) {
    nowTm = t; nowEpoch = mktime(&t); rtcOk = true;
    timeValid = (nowTm.tm_year + 1900) >= 2025;
    strftime(tsBuf, sizeof tsBuf, "%Y-%m-%d %H:%M:%S", &nowTm);
  } else rtcOk = false;
}
int minOfDay() { return nowTm.tm_hour * 60 + nowTm.tm_min; }
bool inWindow() {
  int m = minOfDay();
  return (m >= cfg.w1s && m < cfg.w1e) || (m >= cfg.w2s && m < cfg.w2e);
}

// ================= SOIL SENSORS =================
float pct[3] = {50, 50, 50}, fused = 50; int raws[3]; bool ok[3]; int nOk = 0; bool emaInit = false;

int readRaw(uint8_t pin) {                       // median of 9
  int v[9];
  for (int i = 0; i < 9; i++) { v[i] = analogRead(pin); delayMicroseconds(300); }
  for (int i = 1; i < 9; i++) { int k = v[i], j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
  return v[4];
}
float rawToPct(int i, int raw) {
  int span = cal.dry[i] - cal.wet[i]; if (span < 200) span = 200;
  return constrain(100.0f * (cal.dry[i] - raw) / (float)span, 0.0f, 100.0f);
}
void sampleSoil() {
  float v[3]; nOk = 0;
  for (int i = 0; i < 3; i++) {
    raws[i] = readRaw(SOIL_PIN[i]);
    ok[i] = raws[i] > 300 && raws[i] < 4000;     // unplugged / shorted = ignore
    if (!ok[i]) continue;
    float p = rawToPct(i, raws[i]);
    pct[i] = emaInit ? pct[i] + 0.15f * (p - pct[i]) : p;   // EMA low-pass
    v[nOk++] = pct[i];
  }
  emaInit = true;
  if (nOk == 3) {                                // median of three
    float a = v[0], b = v[1], c = v[2];
    fused = max(min(a, b), min(max(a, b), c));
  } else if (nOk == 2) fused = (v[0] + v[1]) / 2;
}

// ================= FLOW SENSOR =================
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulses = 0;
void IRAM_ATTR isrFlow() { portENTER_CRITICAL_ISR(&mux); pulses++; portEXIT_CRITICAL_ISR(&mux); }
float flowLpm = 0, sessionL = 0, totalL = 0;
void updateFlow() {                              // call once per second
  portENTER_CRITICAL(&mux); uint32_t p = pulses; pulses = 0; portEXIT_CRITICAL(&mux);
  float litres = p / FLOW_PPL;
  flowLpm = 0.7f * flowLpm + 0.3f * (litres * 60.0f);
  sessionL += litres; totalL += litres;
}

// ================= OUTPUTS =================
bool pumpOn = false, valveOn = false; uint32_t valveCloseAt = 0;
void setPump(bool on)  { pumpOn = on;  digitalWrite(PIN_RELAY_PUMP,  on ? RELAY_ON : RELAY_OFF); }
void setValve(bool on) { valveOn = on; digitalWrite(PIN_RELAY_VALVE, on ? RELAY_ON : RELAY_OFF); }
void allOff() { setPump(false); setValve(false); valveCloseAt = 0; }

// ================= LOGGING (microSD via Serial Data Logger) =================
void logEvent(const char* ev) {
  char b[200];
  snprintf(b, sizeof b, "%s,%.0f,%.0f,%.0f,%.0f,%s,%d,%d,%.2f,%.2f,%s",
           tsBuf, pct[0], pct[1], pct[2], fused, STN[st], pumpOn, valveOn, flowLpm, sessionL, ev);
  Logger.println(b);                              // returns immediately; logger writes to SD
}

// ================= HISTORY RING BUFFER (RAM, 24 h @ 5 min) =================
struct Sample { uint32_t t; uint8_t avg; uint8_t wet; };
Sample hist[288]; uint16_t hHead = 0, hCount = 0;
void pushHist() {
  hist[hHead] = { (uint32_t)nowEpoch, (uint8_t)fused, (uint8_t)(pumpOn ? 1 : 0) };
  hHead = (hHead + 1) % 288; if (hCount < 288) hCount++;
}

// ================= CONTROL STATE MACHINE =================
uint32_t stMs = 0, lastEndMs = 0, manualSec = 0, tick = 0;
uint8_t cycles = 0, leakTicks = 0; uint16_t runSec = 0; bool everRan = false;

void enter(State s) { st = s; stMs = millis(); }
uint32_t elapsed() { return (millis() - stMs) / 1000; }

void beginPulse() { cycles++; valveCloseAt = 0; setValve(true); enter(ST_WATERING); logEvent("PULSE_START"); }
void endPulse()   { setPump(false); valveCloseAt = millis() + 2000; enter(ST_SOAK); logEvent("PULSE_END"); }
void startSession(const char* why) { sessionL = 0; cycles = 0; runSec = 0; logEvent(why); beginPulse(); }
void endSession(const char* why) {
  setPump(false); valveCloseAt = millis() + 2000;
  lastEndMs = millis(); everRan = true; enter(ST_IDLE);
  logEvent(why); prefs.putFloat("tot", totalL);
}
void trip(Fault f) { allOff(); fault = f; enter(ST_FAULT); logEvent(FN[f]); }

void controlTick() {
  bool sensorsOk = nOk >= 2;
  switch (st) {
    case ST_IDLE:
      if (cfg.autoMode && !cfg.rainSkip && sensorsOk && rtcOk && timeValid && tick > 20 &&
          fused < cfg.lowPct && inWindow() &&
          (!everRan || (millis() - lastEndMs) / 60000UL >= cfg.cooldownMin))
        startSession("AUTO_START");
      break;

    case ST_WATERING: {
      uint32_t e = elapsed(); runSec++;
      if (e >= 1 && !pumpOn) setPump(true);                    // valve first, pump 1 s later
      if (!sensorsOk)                          { trip(F_SENSOR);  break; }
      if (e >= 10 && flowLpm < cfg.minFlow)    { trip(F_NO_FLOW); break; }   // dry-run / blocked
      if (fused >= cfg.highPct)                { endSession("TARGET_REACHED"); break; }
      if (sessionL >= cfg.maxLiters)           { endSession("VOLUME_CAP");     break; }
      if (runSec >= cfg.maxRunSec)             { endSession("RUNTIME_CAP");    break; }
      if (e >= cfg.pulseSec) endPulse();
      break;
    }

    case ST_SOAK:
      if (elapsed() >= cfg.soakSec) {
        if (fused >= cfg.highPct)       endSession("TARGET_REACHED");
        else if (!sensorsOk)            endSession("SENSOR_ABORT");
        else if (cycles >= cfg.maxCycles) endSession("MAX_CYCLES");
        else                            beginPulse();
      }
      break;

    case ST_MANUAL: {
      uint32_t e = elapsed();
      if (e >= 1 && !pumpOn) setPump(true);
      if (e >= 10 && flowLpm < cfg.minFlow) { trip(F_NO_FLOW); break; }
      if (e >= manualSec) endSession("MANUAL_DONE");
      break;
    }
    case ST_FAULT: break;                                      // latched until reset
  }

  // leak watch: valve + pump are OFF but water is still moving
  if (!valveOn && !pumpOn && st != ST_FAULT) {
    if (flowLpm > 0.5f) { if (++leakTicks >= 10) trip(F_LEAK); } else leakTicks = 0;
  } else leakTicks = 0;
}

bool manualStart(uint32_t sec) {
  if (st != ST_IDLE) return false;
  manualSec = constrain(sec, 5, 600); sessionL = 0;            // always time-limited
  valveCloseAt = 0; setValve(true); enter(ST_MANUAL); logEvent("MANUAL_START");
  return true;
}
void stopNow() { if (st == ST_WATERING || st == ST_SOAK || st == ST_MANUAL) endSession("USER_STOP"); }
void resetFault() {
  if (st != ST_FAULT) return;
  allOff(); fault = F_NONE; lastEndMs = millis(); everRan = true; enter(ST_IDLE); logEvent("FAULT_CLEARED");
}

// ================= HTTP API =================
bool auth() {
  if (server.header("X-Token") != API_TOKEN) { server.send(401, "application/json", "{\"err\":\"auth\"}"); return false; }
  return true;
}
void sendJson(JsonDocument &d, int code) { String o; serializeJson(d, o); server.send(code, "application/json", o); }
void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }
void sendErr(const char* m) { String o = String("{\"err\":\"") + m + "\"}"; server.send(400, "application/json", o); }
bool body(JsonDocument &d) { return !deserializeJson(d, server.arg("plain")); }

void hStatus() {
  if (!auth()) return;
  JsonDocument d;
  JsonArray m = d["moist"].to<JsonArray>();
  for (int i = 0; i < 3; i++) m.add(ok[i] ? (int)pct[i] : -1);
  d["avg"] = (int)fused; d["state"] = STN[st]; d["fault"] = FN[fault];
  d["pump"] = pumpOn; d["valve"] = valveOn; d["flow"] = flowLpm;
  d["session"] = sessionL; d["total"] = totalL; d["cycles"] = cycles;
  d["auto"] = cfg.autoMode; d["rainSkip"] = cfg.rainSkip;
  d["sensorsOk"] = (nOk >= 2); d["timeValid"] = (rtcOk && timeValid);
  d["time"] = tsBuf; d["rssi"] = WiFi.RSSI();
  d["soakLeft"] = (st == ST_SOAK && cfg.soakSec > elapsed()) ? (int)(cfg.soakSec - elapsed()) : 0;
  sendJson(d, 200);
}

#define GETF(k) d[#k] = cfg.k
#define SETF(k) if (!d[#k].isNull()) cfg.k = d[#k].as<decltype(cfg.k)>()
void hCfgGet() {
  if (!auth()) return;
  JsonDocument d;
  GETF(lowPct); GETF(highPct); GETF(maxCycles); GETF(pulseSec); GETF(soakSec); GETF(maxRunSec);
  GETF(cooldownMin); GETF(w1s); GETF(w1e); GETF(w2s); GETF(w2e);
  GETF(maxLiters); GETF(minFlow); GETF(autoMode); GETF(rainSkip);
  sendJson(d, 200);
}
void hCfgSet() {
  if (!auth()) return;
  JsonDocument d; if (!body(d)) return sendErr("json");
  Config old = cfg;
  SETF(lowPct); SETF(highPct); SETF(maxCycles); SETF(pulseSec); SETF(soakSec); SETF(maxRunSec);
  SETF(cooldownMin); SETF(w1s); SETF(w1e); SETF(w2s); SETF(w2e);
  SETF(maxLiters); SETF(minFlow); SETF(autoMode); SETF(rainSkip);
  bool valid = cfg.lowPct + 5 <= cfg.highPct && cfg.highPct <= 100 &&
               cfg.pulseSec >= 5 && cfg.pulseSec <= 600 && cfg.soakSec >= 30 && cfg.soakSec <= 3600 &&
               cfg.maxCycles >= 1 && cfg.maxCycles <= 10 && cfg.maxLiters > 0 && cfg.maxRunSec >= cfg.pulseSec &&
               cfg.w1s < 1440 && cfg.w1e <= 1440 && cfg.w2s < 1440 && cfg.w2e <= 1440;
  if (!valid) { cfg = old; return sendErr("invalid values"); }
  saveCfg(); sendOk();
}
void hManual() {
  if (!auth()) return;
  JsonDocument d; if (!body(d)) return sendErr("json");
  if (!manualStart(d["sec"] | 60)) return sendErr("busy or fault");
  sendOk();
}
void hStop()  { if (!auth()) return; stopNow();   sendOk(); }
void hReset() { if (!auth()) return; resetFault(); sendOk(); }
void hTime() {
  if (!auth()) return;
  JsonDocument d; if (!body(d)) return sendErr("json");
  tm t = {}; t.tm_year = (int)d["y"] - 1900; t.tm_mon = (int)d["mo"] - 1; t.tm_mday = d["d"];
  t.tm_hour = d["h"]; t.tm_min = d["mi"]; t.tm_sec = d["s"];
  long diff = labs((long)(mktime(&t) - nowEpoch));
  if (!timeValid || !rtcOk || diff > 5) { rtcSet(d["y"], d["mo"], d["d"], d["h"], d["mi"], d["s"]); readClock(); }
  sendOk();
}
void hCal() {
  if (!auth()) return;
  JsonDocument d; if (!body(d)) return sendErr("json");
  String k = d["kind"] | "";
  for (int i = 0; i < 3; i++) {
    int r = readRaw(SOIL_PIN[i]);
    if (k == "dry") cal.dry[i] = r; else if (k == "wet") cal.wet[i] = r; else return sendErr("kind");
  }
  saveCal(); sendOk();
}
void hHistory() {
  if (!auth()) return;
  JsonDocument d; d["step"] = 300;
  JsonArray v = d["v"].to<JsonArray>(); JsonArray w = d["w"].to<JsonArray>();
  uint16_t start = (hHead + 288 - hCount) % 288;
  for (uint16_t i = 0; i < hCount; i++) { Sample &s = hist[(start + i) % 288]; v.add(s.avg); w.add(s.wet); }
  sendJson(d, 200);
}
void hWifi() {
  if (!auth()) return;
  JsonDocument d; if (!body(d)) return sendErr("json");
  prefs.putString("ssid", d["ssid"] | ""); prefs.putString("pass", d["pass"] | "");
  sendOk(); delay(500); ESP.restart();
}

// ================= WI-FI =================
void startWifi() {
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  if (ssid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {            // fallback: own access point -> http://192.168.4.1
    WiFi.mode(WIFI_AP); WiFi.softAP("Sprinkler-AP", "sprinkler123");
  }
  MDNS.begin("sprinkler");
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);  pinMode(PIN_RELAY_PUMP, OUTPUT);   // OFF before output = no boot glitch
  digitalWrite(PIN_RELAY_VALVE, RELAY_OFF); pinMode(PIN_RELAY_VALVE, OUTPUT);
  analogReadResolution(12);
  for (uint8_t p : SOIL_PIN) analogSetPinAttenuation(p, ADC_11db);
  pinMode(PIN_FLOW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), isrFlow, FALLING);
  Wire.begin();
  Logger.begin(9600, SERIAL_8N1, LOGGER_RX, LOGGER_TX);     // check your logger's default baud

  prefs.begin("spr", false);
  if (prefs.getBytesLength("cfg") == sizeof(cfg)) prefs.getBytes("cfg", &cfg, sizeof(cfg));
  if (prefs.getBytesLength("cal") == sizeof(cal)) prefs.getBytes("cal", &cal, sizeof(cal));
  totalL = prefs.getFloat("tot", 0);

  startWifi();
  const char* hdrs[] = {"X-Token"}; server.collectHeaders(hdrs, 1);
  server.on("/api/status", HTTP_GET, hStatus);
  server.on("/api/config", HTTP_GET, hCfgGet);
  server.on("/api/config", HTTP_POST, hCfgSet);
  server.on("/api/manual", HTTP_POST, hManual);
  server.on("/api/stop", HTTP_POST, hStop);
  server.on("/api/reset", HTTP_POST, hReset);
  server.on("/api/time", HTTP_POST, hTime);
  server.on("/api/calibrate", HTTP_POST, hCal);
  server.on("/api/history", HTTP_GET, hHistory);
  server.on("/api/wifi", HTTP_POST, hWifi);
  server.begin();

  delay(1500);                                              // let the logger boot
  Logger.println("time,m1,m2,m3,avg,state,pump,valve,flow_lpm,session_l,event");
  readClock(); logEvent("BOOT");
}

uint32_t lastTick = 0, lastWifiTry = 0;
void loop() {
  server.handleClient();

  if (millis() - lastTick >= 1000) {                        // 1 Hz control loop
    lastTick = millis(); tick++;
    readClock(); sampleSoil(); updateFlow(); controlTick();
    if (tick % 60 == 0)  logEvent("SAMPLE");
    if (tick % 300 == 0) pushHist();
  }
  if (valveCloseAt && (int32_t)(millis() - valveCloseAt) >= 0) { setValve(false); valveCloseAt = 0; }

  if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 30000) {
    lastWifiTry = millis(); WiFi.reconnect();               // control keeps running meanwhile
  }
}
```

> **RTC driver note:** the SD2405 is driven with raw I2C (address `0x32`, BCD time registers 0x00–0x06, write-unlock via registers 0x10/0x0F). If the time reads wrong on your module, cross-check with DFRobot's SD2405 example. The rest of the code only needs `rtcRead()` / `rtcSet()`.

---

## 8. REST API contract

All requests need header `X-Token: <API_TOKEN>`. 
Base URLs:
- **Cloud Relay (Recommended):** `https://sprinkler-server.onrender.com` (works anywhere without local IP)
- **Local Fallback:** `http://<device-ip>` (AP mode: `http://192.168.4.1`)

| Method | Path | Body | Purpose |
|---|---|---|---|
| GET | `/api/status` | – | Live state: moisture ×3, avg, state, pump/valve, flow, litres, faults |
| GET / POST | `/api/config` | partial JSON | Read / update thresholds, timing, windows, caps, auto mode, rain-skip |
| POST | `/api/manual` | `{"sec":120}` | Water now (5–600 s, always time-limited) |
| POST | `/api/stop` | – | Stop immediately |
| POST | `/api/reset` | – | Clear a latched fault |
| POST | `/api/time` | `{y,mo,d,h,mi,s}` | Sync RTC (only applied if invalid or drift > 5 s) |
| POST | `/api/calibrate` | `{"kind":"dry"\|"wet"}` | Capture current raw readings as dry/wet point |
| GET | `/api/history` | – | Last 24 h moisture (5-min steps) |
| POST | `/api/wifi` | `{ssid,pass}` | Store home/hotspot Wi-Fi and reboot |
| GET | `/api/wifi/scan` | – | Scan nearby 2.4GHz Wi-Fi networks (returns SSIDs, RSSI, encryption) |
| GET / POST | `/api/cloud` | `{"cloudUrl":"..."}` | Read or configure cloud relay endpoint |
| POST (Device) | `/api/device/telemetry` | full telemetry | ESP32 periodic heartbeat & status sync with Render server; returns command queue |

---

## 9. Mobile app (React Native with Expo)

### 9.1 Create the project

```bash
npx create-expo-app@latest SprinklerApp --template blank
cd SprinklerApp
npx expo install react-native-svg react-native-safe-area-context @react-native-async-storage/async-storage expo-build-properties
npx expo start
```

Open it on your phone with **Expo Go** (phone and ESP32 on the same network, or the phone joined to `Sprinkler-AP`).

### 9.2 `app.json` (needed for plain-HTTP to the device in release builds)

Merge into your existing `expo` object:

```json
{
  "expo": {
    "plugins": [
      ["expo-build-properties", { "android": { "usesCleartextTraffic": true } }]
    ],
    "ios": {
      "infoPlist": {
        "NSAppTransportSecurity": { "NSAllowsLocalNetworking": true },
        "NSLocalNetworkUsageDescription": "Connect to the sprinkler controller on your Wi-Fi."
      }
    }
  }
}
```

### 9.3 `api.js`

```js
import AsyncStorage from '@react-native-async-storage/async-storage';

let BASE = 'http://192.168.4.1';          // default = device access-point address
const TOKEN = 'change-me-123';            // must match API_TOKEN in the firmware

export async function loadBase() {
  const b = await AsyncStorage.getItem('base');
  if (b) BASE = b;
  return BASE;
}
export async function setBase(ip) {
  BASE = `http://${ip.trim()}`;
  await AsyncStorage.setItem('base', BASE);
}

async function req(path, method = 'GET', body) {
  const ctl = new AbortController();
  const to = setTimeout(() => ctl.abort(), 4000);
  try {
    const r = await fetch(BASE + path, {
      method,
      headers: { 'Content-Type': 'application/json', 'X-Token': TOKEN },
      body: body ? JSON.stringify(body) : undefined,
      signal: ctl.signal,
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.err || `HTTP ${r.status}`);
    return j;
  } finally {
    clearTimeout(to);
  }
}

export const getStatus   = () => req('/api/status');
export const getConfig   = () => req('/api/config');
export const setConfig   = (c) => req('/api/config', 'POST', c);
export const manual      = (sec) => req('/api/manual', 'POST', { sec });
export const stop        = () => req('/api/stop', 'POST');
export const resetFault  = () => req('/api/reset', 'POST');
export const calibrate   = (kind) => req('/api/calibrate', 'POST', { kind });
export const getHistory  = () => req('/api/history');
export const setWifi     = (ssid, pass) => req('/api/wifi', 'POST', { ssid, pass });
export const syncTime = () => {
  const n = new Date();
  return req('/api/time', 'POST', {
    y: n.getFullYear(), mo: n.getMonth() + 1, d: n.getDate(),
    h: n.getHours(), mi: n.getMinutes(), s: n.getSeconds(),
  });
};
```

### 9.4 `App.js`

```jsx
import React, { useEffect, useState } from 'react';
import {
  View, Text, TextInput, TouchableOpacity, ScrollView, Switch, StyleSheet, Alert, StatusBar,
} from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';
import Svg, { Polyline, Line } from 'react-native-svg';
import * as api from './api';

const toMin = (s) => { const [h, m] = s.split(':').map(Number); return h * 60 + m; };
const fromMin = (m) => `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
const STATE_COLOR = { IDLE: '#64748b', WATERING: '#0ea5e9', SOAK: '#f59e0b', MANUAL: '#0ea5e9', FAULT: '#ef4444' };

// ---------- polling hook ----------
function useDevice() {
  const [s, setS] = useState(null);
  const [online, setOnline] = useState(false);
  useEffect(() => {
    let alive = true, timer;
    const poll = async () => {
      try { const d = await api.getStatus(); if (alive) { setS(d); setOnline(true); } }
      catch { if (alive) setOnline(false); }
      if (alive) timer = setTimeout(poll, 2000);
    };
    poll();
    return () => { alive = false; clearTimeout(timer); };
  }, []);
  useEffect(() => { if (online) api.syncTime().catch(() => {}); }, [online]);
  return { s, online };
}

const run = async (fn) => { try { await fn(); } catch (e) { Alert.alert('Error', String(e.message || e)); } };

// ---------- small components ----------
const Card = ({ children, style }) => <View style={[st.card, style]}>{children}</View>;

function Gauge({ label, value }) {
  const bad = value < 0;
  const color = bad ? '#ef4444' : value < 35 ? '#f59e0b' : value > 75 ? '#3b82f6' : '#22c55e';
  return (
    <View style={{ marginBottom: 10 }}>
      <View style={st.row}>
        <Text style={st.label}>{label}</Text>
        <Text style={[st.label, { color }]}>{bad ? 'SENSOR FAULT' : `${value}%`}</Text>
      </View>
      <View style={st.barBg}><View style={[st.barFg, { width: `${bad ? 0 : value}%`, backgroundColor: color }]} /></View>
    </View>
  );
}

function Btn({ title, onPress, color = '#0ea5e9', style }) {
  return (
    <TouchableOpacity style={[st.btn, { backgroundColor: color }, style]} onPress={onPress}>
      <Text style={st.btnTxt}>{title}</Text>
    </TouchableOpacity>
  );
}

// ---------- Dashboard ----------
function Dashboard({ s, online }) {
  if (!s) {
    return (
      <Card>
        <Text style={st.label}>{online ? 'Loading…' : 'Not connected. Set the device address in Settings, and make sure your phone is on the same Wi-Fi (or on "Sprinkler-AP").'}</Text>
      </Card>
    );
  }
  const color = STATE_COLOR[s.state] || '#64748b';
  return (
    <>
      <Card>
        <View style={st.row}>
          <View style={[st.chip, { backgroundColor: color }]}><Text style={st.chipTxt}>{s.state}</Text></View>
          <Text style={st.small}>{s.time} · {s.rssi} dBm</Text>
        </View>
        {s.state === 'SOAK' && <Text style={st.small}>Soaking… next pulse in {s.soakLeft}s</Text>}
        {s.state === 'FAULT' && <Text style={[st.small, { color: '#ef4444' }]}>Fault: {s.fault}</Text>}
        {!s.sensorsOk && <Text style={[st.small, { color: '#f59e0b' }]}>Fewer than 2 healthy soil sensors: auto watering paused.</Text>}
        {!s.timeValid && <Text style={[st.small, { color: '#f59e0b' }]}>Clock not set: auto watering paused.</Text>}
      </Card>

      <Card>
        <Text style={st.h}>Soil moisture (avg {s.avg}%)</Text>
        {s.moist.map((m, i) => <Gauge key={i} label={`Sensor ${i + 1}`} value={m} />)}
      </Card>

      <Card>
        <View style={st.row}>
          <Stat label="Pump" value={s.pump ? 'ON' : 'off'} />
          <Stat label="Valve" value={s.valve ? 'OPEN' : 'closed'} />
          <Stat label="Flow" value={`${s.flow.toFixed(1)} L/min`} />
        </View>
        <View style={[st.row, { marginTop: 12 }]}>
          <Stat label="Session" value={`${s.session.toFixed(1)} L`} />
          <Stat label="Cycle" value={String(s.cycles)} />
          <Stat label="Lifetime" value={`${s.total.toFixed(0)} L`} />
        </View>
      </Card>

      <Card>
        <View style={st.row}>
          <Btn title="Water 2 min" onPress={() => run(() => api.manual(120))} />
          <Btn title="STOP" color="#ef4444" onPress={() => run(api.stop)} />
        </View>
        {s.state === 'FAULT' && <Btn title="Reset fault" color="#f59e0b" style={{ marginTop: 10 }} onPress={() => run(api.resetFault)} />}
        <View style={[st.row, { marginTop: 14 }]}>
          <Text style={st.label}>Automatic mode</Text>
          <Switch value={s.auto} onValueChange={(v) => run(() => api.setConfig({ autoMode: v }))} />
        </View>
        <View style={[st.row, { marginTop: 8 }]}>
          <Text style={st.label}>Skip (rain expected)</Text>
          <Switch value={s.rainSkip} onValueChange={(v) => run(() => api.setConfig({ rainSkip: v }))} />
        </View>
      </Card>
    </>
  );
}
const Stat = ({ label, value }) => (
  <View style={{ alignItems: 'center', flex: 1 }}>
    <Text style={st.small}>{label}</Text>
    <Text style={st.stat}>{value}</Text>
  </View>
);

// ---------- History (sparkline) ----------
function History() {
  const [h, setH] = useState(null);
  const [cfg, setCfg] = useState(null);
  const load = () => run(async () => { setH(await api.getHistory()); setCfg(await api.getConfig()); });
  useEffect(() => { load(); }, []);
  const W = 320, H = 140;
  const pts = h && h.v.length > 1
    ? h.v.map((v, i) => `${(i / (h.v.length - 1)) * W},${H - (v / 100) * H}`).join(' ')
    : '';
  return (
    <Card>
      <Text style={st.h}>Last 24 h average moisture</Text>
      <Svg width={W} height={H} style={{ alignSelf: 'center', marginVertical: 8 }}>
        {cfg && <Line x1="0" x2={W} y1={H - (cfg.lowPct / 100) * H} y2={H - (cfg.lowPct / 100) * H} stroke="#f59e0b" strokeDasharray="4" />}
        {cfg && <Line x1="0" x2={W} y1={H - (cfg.highPct / 100) * H} y2={H - (cfg.highPct / 100) * H} stroke="#22c55e" strokeDasharray="4" />}
        {pts ? <Polyline points={pts} fill="none" stroke="#0ea5e9" strokeWidth="2" /> : null}
      </Svg>
      <Text style={st.small}>Orange = start threshold · Green = target. Full history is on the SD card (CSV).</Text>
      <Btn title="Refresh" onPress={load} style={{ marginTop: 10 }} />
    </Card>
  );
}

// ---------- Settings ----------
function Settings() {
  const [ip, setIp] = useState('');
  const [f, setF] = useState(null);
  const [ssid, setSsid] = useState('');
  const [pass, setPass] = useState('');

  const load = () => run(async () => {
    const c = await api.getConfig();
    setF({
      lowPct: String(c.lowPct), highPct: String(c.highPct), pulseSec: String(c.pulseSec),
      soakSec: String(c.soakSec), maxCycles: String(c.maxCycles), maxLiters: String(c.maxLiters),
      w1: `${fromMin(c.w1s)}-${fromMin(c.w1e)}`, w2: `${fromMin(c.w2s)}-${fromMin(c.w2e)}`,
    });
  });
  useEffect(() => { api.loadBase().then((b) => setIp(b.replace('http://', ''))); load(); }, []);

  const save = () => run(async () => {
    const [a, b] = f.w1.split('-'); const [c, d] = f.w2.split('-');
    await api.setConfig({
      lowPct: +f.lowPct, highPct: +f.highPct, pulseSec: +f.pulseSec, soakSec: +f.soakSec,
      maxCycles: +f.maxCycles, maxLiters: +f.maxLiters,
      w1s: toMin(a), w1e: toMin(b), w2s: toMin(c), w2e: toMin(d),
    });
    Alert.alert('Saved', 'Settings stored on the controller.');
  });

  const field = (k, label) => (
    <View style={st.row} key={k}>
      <Text style={st.label}>{label}</Text>
      <TextInput style={st.input} value={f?.[k] ?? ''} onChangeText={(t) => setF({ ...f, [k]: t })} keyboardType={k.startsWith('w') ? 'default' : 'numeric'} />
    </View>
  );

  return (
    <>
      <Card>
        <Text style={st.h}>Device address</Text>
        <TextInput style={[st.input, { width: '100%' }]} value={ip} onChangeText={setIp} autoCapitalize="none" placeholder="192.168.4.1" />
        <Btn title="Save address" onPress={() => api.setBase(ip).then(load)} style={{ marginTop: 8 }} />
      </Card>

      <Card>
        <Text style={st.h}>Irrigation rules</Text>
        {f && <>
          {field('lowPct', 'Start below (%)')}
          {field('highPct', 'Stop at (%)')}
          {field('pulseSec', 'Pulse (s)')}
          {field('soakSec', 'Soak (s)')}
          {field('maxCycles', 'Max pulses')}
          {field('maxLiters', 'Max litres/session')}
          {field('w1', 'Window 1 (HH:MM-HH:MM)')}
          {field('w2', 'Window 2 (HH:MM-HH:MM)')}
        </>}
        <Btn title="Save to controller" onPress={save} style={{ marginTop: 10 }} />
      </Card>

      <Card>
        <Text style={st.h}>Sensor calibration</Text>
        <Text style={st.small}>1) Hold all 3 sensors in dry air → "Set DRY". 2) Put them in water up to the line (not the electronics) → "Set WET".</Text>
        <View style={[st.row, { marginTop: 8 }]}>
          <Btn title="Set DRY" color="#f59e0b" onPress={() => run(() => api.calibrate('dry'))} />
          <Btn title="Set WET" onPress={() => run(() => api.calibrate('wet'))} />
        </View>
      </Card>

      <Card>
        <Text style={st.h}>Join Wi-Fi / phone hotspot</Text>
        <TextInput style={[st.input, { width: '100%', marginBottom: 6 }]} placeholder="SSID" value={ssid} onChangeText={setSsid} autoCapitalize="none" />
        <TextInput style={[st.input, { width: '100%' }]} placeholder="Password" value={pass} onChangeText={setPass} secureTextEntry autoCapitalize="none" />
        <Btn title="Save & reboot controller" color="#64748b" onPress={() => run(() => api.setWifi(ssid, pass))} style={{ marginTop: 8 }} />
      </Card>
    </>
  );
}

// ---------- App shell ----------
export default function App() {
  const { s, online } = useDevice();
  const [tab, setTab] = useState('Dashboard');
  useEffect(() => { api.loadBase(); }, []);
  return (
    <SafeAreaProvider>
      <SafeAreaView style={st.root}>
        <StatusBar barStyle="dark-content" />
        <View style={st.header}>
          <Text style={st.title}>Auto Sprinkler</Text>
          <View style={[st.dot, { backgroundColor: online ? '#22c55e' : '#ef4444' }]} />
        </View>
        <ScrollView contentContainerStyle={{ padding: 14 }}>
          {tab === 'Dashboard' && <Dashboard s={s} online={online} />}
          {tab === 'History' && <History />}
          {tab === 'Settings' && <Settings />}
        </ScrollView>
        <View style={st.tabs}>
          {['Dashboard', 'History', 'Settings'].map((t) => (
            <TouchableOpacity key={t} style={st.tab} onPress={() => setTab(t)}>
              <Text style={[st.tabTxt, tab === t && { color: '#0ea5e9', fontWeight: '700' }]}>{t}</Text>
            </TouchableOpacity>
          ))}
        </View>
      </SafeAreaView>
    </SafeAreaProvider>
  );
}

const st = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#f1f5f9' },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', padding: 14, backgroundColor: '#fff' },
  title: { fontSize: 20, fontWeight: '800', color: '#0f172a' },
  dot: { width: 12, height: 12, borderRadius: 6 },
  card: { backgroundColor: '#fff', borderRadius: 14, padding: 14, marginBottom: 12 },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 8 },
  h: { fontSize: 16, fontWeight: '700', marginBottom: 10, color: '#0f172a' },
  label: { fontSize: 14, color: '#334155', flexShrink: 1 },
  small: { fontSize: 12, color: '#64748b', marginTop: 4 },
  stat: { fontSize: 16, fontWeight: '700', color: '#0f172a' },
  barBg: { height: 10, backgroundColor: '#e2e8f0', borderRadius: 5, marginTop: 4, overflow: 'hidden' },
  barFg: { height: 10, borderRadius: 5 },
  chip: { paddingHorizontal: 12, paddingVertical: 4, borderRadius: 12 },
  chipTxt: { color: '#fff', fontWeight: '700' },
  btn: { flex: 1, paddingVertical: 12, borderRadius: 10, alignItems: 'center' },
  btnTxt: { color: '#fff', fontWeight: '700' },
  input: { backgroundColor: '#f1f5f9', borderRadius: 8, paddingHorizontal: 10, paddingVertical: 6, width: 130, textAlign: 'right' },
  tabs: { flexDirection: 'row', backgroundColor: '#fff', borderTopWidth: 1, borderColor: '#e2e8f0' },
  tab: { flex: 1, padding: 14, alignItems: 'center' },
  tabTxt: { color: '#64748b' },
});
```

### 9.5 How the app talks to the device

- The app **polls `/api/status` every 2 s** (simple and robust; the ESP32 web server handles it easily).
- On every (re)connect the app **syncs the phone's clock** to the RTC (the firmware ignores it unless the RTC is unset or off by > 5 s).
- Commands are plain POSTs and every manual run is time-limited, so a dropped connection can never leave water running.
- `.local` (mDNS) names are unreliable on Android, so the app uses an **IP address** (AP mode: `192.168.4.1`).

---

## 10. Build order (fastest path)

| Phase | Task | Done when |
|---|---|---|
| 0 | Bench-wire ESP32 + RTC + logger + relays (no water, no pump) | Time reads correctly, relays click, log file appears on the SD card |
| 1 | Soil sensors + calibration | Dry air ≈ 0 %, water ≈ 100 %, a fingertip squeeze moves the reading |
| 2 | Flow sensor with divider | Fill a 10 L bucket: reported litres within ±5 % (adjust `FLOW_PPL` if not) |
| 3 | Run firmware + app on the bench | App shows live data, STOP works, fault reset works |
| 4 | Plumb the water path, test safety | See checklist below |
| 5 | Field install, tune `lowPct`/`highPct`/`soakSec` from the SD logs | Soil stays between thresholds without runoff |

**Safety test checklist (do all of these before leaving it alone):**

- [ ] Power-cycle with pump disconnected: no relay clicks at boot.
- [ ] Close the supply to the pump or empty the drum → pump stops with `NO_FLOW` in ≤ ~11 s.
- [ ] Unplug one soil sensor → app shows *SENSOR FAULT*, automatic watering continues only if 2 sensors remain healthy; unplug two → automatic watering stops.
- [ ] Turn off Wi-Fi / close the app mid-run → watering finishes by itself and stops on time.
- [ ] Pull the RTC → automatic watering is blocked and the app warns.
- [ ] Leave the valve wired open with the pump off (simulated leak) → `LEAK` after ~10 s.
- [ ] Pull the SD card → system keeps irrigating (logging is non-critical).

### Troubleshooting

- **App cannot connect while on `Sprinkler-AP`:** Android may route traffic through mobile data because the AP has no internet. Choose *Stay connected* on the prompt, or turn mobile data off temporarily.
- **No log file on the SD card:** confirm the logger's baud rate (the code assumes 9600), that the card is FAT32 and ≤ 32 GB, and that the logger is powered before the first `println`.
- **Soil readings jump when Wi-Fi is busy:** make sure sensors use ADC1 pins (36/39/34), keep sensor cables away from the pump wires, add a 100 nF capacitor from each sensor signal to GND.
- **Flow reads 0 at low flow:** the sensor is rated from 1 L/min; with only a few micro-nozzles the flow can be below that. Add nozzles, or lower `minFlow`.

---

## 11. Roadmap (after the core works)

1. **Rain-aware skipping:** the phone fetches a forecast (e.g. Open-Meteo, no API key) and sets `rainSkip` through `/api/config`. The ESP32 stays offline-capable.
2. **Remote access from anywhere:** add MQTT (ESP32 `PubSubClient` + a managed broker with TLS) publishing the same JSON as `/api/status`, and subscribing to a command topic. The app switches between local HTTP and MQTT.
3. **Data-driven tuning / ML:** use the SD logs to fit a soil-drying model (moisture decay vs. time of day) and set `lowPct` dynamically, or predict the next watering time.
4. **Multi-zone:** 4-channel relay + more solenoid valves; the state machine gets a `zone` index and rotates zones.
5. **Real tank-level sensing** (float switch / ultrasonic) and **battery-voltage gating** (skip watering below ~11.5 V).
6. **Read-back from the logger** (command mode) so the app can show history across reboots.
