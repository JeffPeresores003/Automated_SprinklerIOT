# Automated Sprinkler IoT System (Aero-Irrigate)

An autonomous, solar-powered smart irrigation controller built on the **FireBeetle ESP32** and monitored via a decentralized **React Native (Expo)** mobile application.

The system performs closed-loop **cycle-soak irrigation** using capacitive soil sensor fusion, real-time flow verification, and hardware-level safety interlocks. It operates **100% locally with zero cloud dependencies**, ensuring continuous operation even in remote agricultural fields without internet access.

---

## Table of Contents
1. [Key Features](#key-features)
2. [System Architecture](#system-architecture)
3. [Hardware Bill of Materials & Pinout](#hardware-bill-of-materials--pinout)
4. [How Communication & Control Works](#how-communication--control-works)
5. [Firmware Setup (Arduino IDE)](#firmware-setup-arduino-ide)
6. [Mobile App Setup (React Native / Expo)](#mobile-app-setup-react-native--expo)
7. [REST API Reference](#rest-api-reference)
8. [Safety Interlocks & Fail-Safes](#safety-interlocks--fail-safes)
9. [Project Directory Structure](#project-directory-structure)
10. [Documentation Links](#documentation-links)

---

## Key Features

- **Autonomous Edge Control:** All irrigation logic and state machines run directly on the ESP32. The mobile app acts solely as a remote telemetry dashboard and command interface.
- **Cycle-Soak Algorithm:** Waters in timed pulses with soak intervals to allow soil water absorption and prevent surface runoff.
- **3-Sensor Redundancy:** Employs three capacitive soil moisture probes with median and EMA (Exponential Moving Average) filtering to reject noise and probe anomalies.
- **Hydraulic Verification:** Integrates an inline Hall-effect flow sensor to verify water movement, measure volume (liters), and detect dry-run or pipe burst conditions.
- **Independent RTC Synchronization:** Uses an onboard SD2405 I2C RTC module to restrict irrigation to low-evaporation hours (dawn/dusk) even without internet NTP access.
- **Persistent SD Logging:** Logs second-by-second telemetries in CSV format to an onboard microSD card via the Fermion Serial Data Logger.
- **Dual Wi-Fi Modes:** Operates in Station Mode (connecting to local Wi-Fi / hotspot) with automatic fallback to standalone Access Point (SoftAP) mode (`192.168.4.1`).

---

## System Architecture

```
                  ┌───────────────────────────── SOLAR POWER ─────────────────────────────┐
                  │ 100W Panel → 12V Charge Controller → 12V 25Ah Gel Battery → 10A Fuse   │
                  └───────┬───────────────────────────────────────────────┬───────────────┘
                          │ 12V                                           │ 12V
                 ┌────────▼────────┐                             ┌────────▼────────┐
                 │ Buck 12V → 5V   │                             │ 2-CH Relay Mod  │
                 │ (≥2A)           │                             │ CH1: Pump       │
                 └────────┬────────┘                             │ CH2: Solenoid   │
                          │ 5V                                   └────────▲────────┘
    ┌─────────────────────▼───────────────────────────────────────────────┴───────────────┐
    │  FireBeetle ESP32 + IO Shield                                                       │
    │  ┌───────────────┐  ┌────────────────┐  ┌────────────────────────────────────────┐  │
    │  │  Control FSM  │  │  Web Server    │  │  Sensor Pipeline                       │  │
    │  │  (1 Hz loop)  │  │  (REST / Port 80)│  │  3x ADC1 Probes → Median Filter      │  │
    │  └───────────────┘  └───────┬────────┘  └────────────────────────────────────────┘  │
    └───▲────────▲────────▲───────┼──────────────────────────▲───────────▲────────────────┘
        │I2C     │ADC×3   │IRQ    │                          │UART       │Wi-Fi (STA / AP)
   ┌────┴───┐ ┌──┴─────┐ ┌┴───────┴┐                    ┌────┴─────────┐ │
   │SD2405  │ │3x Soil │ │Flow     │                    │Serial Data   │ │
   │RTC     │ │Sensors │ │Sensor   │                    │Logger+MicroSD│ │
   └────────┘ └────────┘ └─────────┘                    └──────────────┘ ▼
                                                            ┌────────────────────────┐
                                                            │ React Native Mobile App│
                                                            │ (Dashboard & Controls) │
                                                            └────────────────────────┘
```

---

## Hardware Bill of Materials & Pinout

| Component | Interface | ESP32 Pin | IO Shield Label | Notes |
|---|---|---|---|---|
| **Pump Relay (CH1)** | Digital OUT | `GPIO 25` | D2 | Active-LOW, switches 12V diaphragm pump |
| **Valve Relay (CH2)** | Digital OUT | `GPIO 26` | D3 | Active-LOW, switches 12V solenoid valve |
| **Water Flow Sensor** | Digital IRQ | `GPIO 27` | D4 | 450 pulses/L; **Must use 5V → 3.3V voltage divider** |
| **Soil Sensor 1** | Analog IN | `GPIO 36` | A0 | Capacitive probe on ADC1 (Wi-Fi safe) |
| **Soil Sensor 2** | Analog IN | `GPIO 39` | A1 | Capacitive probe on ADC1 |
| **Soil Sensor 3** | Analog IN | `GPIO 34` | A2 | Capacitive probe on ADC1 |
| **SD2405 RTC** | I2C | `GPIO 21` (SDA), `GPIO 22` (SCL) | I2C | Battery-backed real-time clock (`0x32`) |
| **Serial Data Logger**| UART2 | `GPIO 16` (RX), `GPIO 17` (TX) | Serial | 9600 baud, powered at 3.3V |

> Detailed schematics, flyback diode wiring, and step-by-step soldering instructions are documented in [Wiring-Guide.md](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/Wiring-Guide.md).

---

## How Communication & Control Works

### 1. Cloud Relay Mode (Recommended — No Local IP Required)
Deploy the companion Node.js relay server from the [sprinkler_server](https://github.com/JeffPeresores003/sprinkler_server) repository to **Render** (`https://sprinkler-server.onrender.com`).
- **No IP Setup:** Both the ESP32 and mobile app communicate through the fixed cloud server address. You never have to look up or type `192.168.x.x`.
- **Global Control:** Monitor and trigger irrigation from outside the home using mobile cellular data (4G/5G).
- **Outbound HTTP/REST Sync:** The ESP32 pings the cloud server every 3 seconds to report telemetries and execute queued commands.

### 2. Local Fallback Mode (SoftAP / Direct IP)
- If the internet or router goes down, the ESP32 falls back to standalone **Access Point (SoftAP) mode**:
  - **SSID:** `Sprinkler-AP`
  - **Password:** `sprinkler123`
  - **IP:** `192.168.4.1`
- The mobile app includes a one-tap preset button in **Settings** to switch between **Cloud (Render)** and **Local SoftAP**.

---

### Step-by-Step: First Setup & In-App Wi-Fi Selection

1. **Initial Power-On & Connection**
   - On first boot (or if out of range of its previous router), the ESP32 starts `Sprinkler-AP`.
   - Connect your phone to `Sprinkler-AP` Wi-Fi.
   - Open the mobile app and tap the **Local SoftAP** button in Settings (connects to `192.168.4.1`).

2. **In-App Wi-Fi Scanning (No Code Uploads)**
   - Under **Provision Infrastructure Wi-Fi**, tap **"Scan Nearby"**.
   - The ESP32 scans 2.4GHz channels and displays available local Wi-Fi networks with signal strengths.
   - Tap your network from the list, type your passphrase, and tap **Save Credentials and Reboot ESP32**.
   - The ESP32 saves the credentials in non-volatile flash memory (NVS) and automatically restarts.

3. **Seamless Cloud Operation**
   - Once connected to your home Wi-Fi, the ESP32 begins syncing status with `https://sprinkler-server.onrender.com`.
   - In the mobile app, tap the **Cloud (Render)** button.
   - You can now monitor and control your sprinkler system anytime, from anywhere in the world!

---

## Firmware Setup (Arduino IDE)

### 1. Requirements
- **Arduino IDE 2.x**
- **ESP32 Board Package:** Install `esp32 by Espressif Systems` via Boards Manager.
- **Board Selection:** Select `FireBeetle-ESP32` (or `ESP32 Dev Module`).
- **Library:** Install `ArduinoJson` (v7.x by Benoit Blanchon) via Library Manager.

### 2. Flashing
1. Open [`AutoSprinkler/AutoSprinkler.ino`](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/AutoSprinkler/AutoSprinkler.ino).
2. Verify that `API_TOKEN` matches the token configured in the mobile app (`SprinklerApp/api.js`).
3. Connect the FireBeetle ESP32 via USB and select the appropriate COM port.
4. Set upload speed to `115200` baud and click **Upload**.
5. Open Serial Monitor at `115200` baud to verify boot output and IP address.

---

## Mobile App Setup (React Native / Expo)

### 1. Requirements
- **Node.js (LTS version)** and **npm**
- **Expo Go** app installed on your Android or iOS device (available on Google Play Store / Apple App Store).

### 2. Running Locally
1. Open a terminal and navigate to the app directory:
   ```bash
   cd SprinklerApp
   ```
2. Install dependencies:
   ```bash
   npm install
   ```
3. Start the Expo development server:
   ```bash
   npx expo start
   ```
4. Scan the QR code displayed in the terminal using the **Expo Go** app on your phone.

> **Security Note:** Ensure the `TOKEN` constant in [`SprinklerApp/api.js`](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/SprinklerApp/api.js) matches the `API_TOKEN` defined in [`AutoSprinkler/AutoSprinkler.ino`](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/AutoSprinkler/AutoSprinkler.ino).

---

## REST API Reference

All requests must include the header `X-Token: <token>`.

| Method | Endpoint | Description | Request Body | Response |
|---|---|---|---|---|
| `GET` | `/api/status` | Read real-time telemetry and state | None | `{state, pump, valve, flow, session, total, avg, moist[], time, rssi, auto, rainSkip, fault}` |
| `GET` | `/api/config` | Read runtime thresholds and schedule | None | `{lowPct, highPct, pulseSec, soakSec, maxCycles, maxLiters, w1s, w1e, w2s, w2e}` |
| `POST` | `/api/config` | Update runtime thresholds | Partial / Full config JSON | `{"ok": true}` |
| `POST` | `/api/manual` | Trigger manual watering pulse | `{"sec": 120}` | `{"ok": true, "duration": 120}` |
| `POST` | `/api/stop` | Immediate emergency shutoff | None | `{"ok": true}` |
| `POST` | `/api/reset` | Clear latched fault state | None | `{"ok": true}` |
| `POST` | `/api/calibrate`| Store 2-point calibration limit | `{"kind": "dry" \| "wet"}` | `{"ok": true}` |
| `POST` | `/api/time` | Synchronize ESP32 RTC to phone clock | `{"y", "mo", "d", "h", "mi", "s"}` | `{"ok": true}` |
| `GET` | `/api/history` | Retrieve 24-hour moisture sample buffer | None | `{"step": 300, "v": [...], "w": [...]}` |
| `POST` | `/api/wifi` | Provision Wi-Fi credentials & reboot | `{"ssid": "...", "pass": "..."}` | `{"ok": true}` |

---

## Safety Interlocks & Fail-Safes

1. **Dry-Run Watchdog (`FAULT: NO_FLOW`):** If the pump is active for 10 seconds but the flow sensor registers `< 0.3 L/min`, the pump shuts off immediately to prevent pump damage.
2. **Leak Detection (`FAULT: LEAK`):** If continuous flow (`> 0.5 L/min` for 10 seconds) occurs while both pump and valve are closed, the system trips and alerts the user.
3. **Sensor Fault Isolation:** If fewer than 2 valid soil sensors are functional, automatic cycles are halted, requiring manual override.
4. **Hard Session Caps:** Every irrigation cycle is bounded by both maximum runtime (`maxCycles * pulseSec`) and maximum volume (`maxLiters`).
5. **Anti-Water Hammer Sequencing:** Solenoid valve opens before the pump starts and closes 2 seconds after the pump stops to protect piping from pressure spikes.

---

## Project Directory Structure

```
Automated_SprinklerIOT/
├── AutoSprinkler/               # ESP32 Arduino Firmware
│   └── AutoSprinkler.ino       # State machine, REST API, sensor & relay drivers
├── SprinklerApp/                # React Native / Expo Mobile Application
│   ├── App.js                  # Main UI dashboard, charts, controls, and settings
│   ├── api.js                  # REST API client & local endpoint management
│   ├── icons.js                # Lucide vector icon mappings
│   ├── package.json            # App dependencies & scripts
│   └── app.json                # Expo project configuration
├── Wiring-Guide.md              # Complete hardware pinout, schematics, and safety rules
├── Auto-Sprinkler-Implementation.md # Engineering guide, firmware architecture & formulas
├── Mobile-App-Guide.md          # Mobile UI telemetry design specifications
└── README.md                    # Project overview & quickstart guide
```

---

## Documentation Links

- [Complete Hardware & Wiring Guide](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/Wiring-Guide.md)
- [System Implementation & Firmware Architecture](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/Auto-Sprinkler-Implementation.md)
- [Mobile App Architecture & Design Specification](file:///c:/Users/JEFFREY%20PERESORES/Documents/GitHub/Automated_SprinklerIOT/Mobile-App-Guide.md)