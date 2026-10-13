# SprinklerApp Mobile Architecture and Interface Specification

## 1. Executive Summary and Design Philosophy

The SprinklerApp mobile interface is engineered as an industrial-grade, client-side telemetry dashboard and control interface for the Automated Sprinkler IoT System. Operating completely decentralized over direct local wireless links (ESP32 Station or SoftAP mode), the application requires zero external cloud dependencies to deliver sub-second telemetry, continuous system monitoring, and fail-safe remote intervention.

### Design System: 2026 Industrial Telemetry Language

The visual and interaction architecture follows the latest 2026 ergonomic standards for cyber-physical IoT interfaces:

- **Bento Grid Architecture:** Information is organized into compartmentalized, high-density modular cards, separating sensor fusion telemetry, hydraulic actuator states, and control triggers without cognitive overload.
- **Surface Elevation and Dark Surface Palette:** Built on deep obsidian and slate foundations (`#090D16`, `#111827`, `#1F2937`) to minimize OLED battery consumption during outdoor inspection while maximizing outdoor sunlight readability through high-contrast typography.
- **Telemetry Typography:** Critical scalar data (flow rates, pulse counters, percentage thresholds) utilizes monospaced, high-legibility figures with distinct metric units to ensure immediate recognition.
- **Dynamic State Semantics:** Status indicators use semantic color coding with glow accents:
  - Cyan (`#06B6D4`): Active Irrigation and Manual Dosing
  - Amber (`#F59E0B`): Soak Cycle / Absorption Phase
  - Slate (`#64748B`): System Idle / Standby
  - Emerald (`#10B981`): Healthy / Stable Sensor Fusion
  - Crimson (`#EF4444`): Safety Interlock Trip / Fault Latch
- **Pure Iconography:** Iconography is rendered exclusively with vector outlines via `lucide-react-native` (no pictorial emojis) to preserve a professional, aerospace-inspired aesthetic.

---

## 2. System Architecture and Network Protocol

```
+-------------------------------------------------------------+
|                 Mobile Client (Expo / React Native)         |
|  +--------------------+  +-------------------------------+  |
|  | Lucide Icon System |  | 24h Telemetry SVG Engine      |  |
|  +--------------------+  +-------------------------------+  |
|  | Polling Engine (2s)|  | Local Cache (AsyncStorage)    |  |
+--+--------------------+--+-------------------------------+--+
               |                                ^
               | HTTP REST / JSON               | Status JSON
               | Header: X-Token                | Header: 200 OK
               v                                |
+-------------------------------------------------------------+
|              FireBeetle ESP32 IoT Controller               |
|  +-------------------------------------------------------+  |
|  | WebServer (Port 80)                                   |  |
|  +-------------------------------------------------------+  |
|  | State Machine FSM (1 Hz loop)                         |  |
|  | Sensor Pipeline: 3x Capacitive ADC1 -> Median Filter  |  |
|  | Hydraulic Drivers: Flow Interrupt + Relay Actuation   |  |
|  | MicroSD Logger via Hardware UART                      |  |
+-------------------------------------------------------------+
```

### Communication Flow and Timing

1. **Autonomous Discovery and Handshake:**
   - The app reads the cached target IP from persistent storage (`AsyncStorage.getItem('base')`), defaulting to `http://192.168.4.1`.
   - The background polling hook (`useDevice`) issues an asynchronous `GET /api/status` request every 2,000 ms with an active 4,000 ms timeout abort controller.
2. **Clock Synchronization:**
   - Upon establishing connection (`online === true`), the client dispatches a `POST /api/time` payload containing the smartphone's local UNIX date-time parameters. This maintains the hardware RTC (SD2405) synchronization without requiring internet NTP access.
3. **Fail-Safe Command Interlocking:**
   - Actuator commands (`POST /api/manual`, `POST /api/stop`, `POST /api/reset`) require explicit header authentication (`X-Token`).
   - Every manual irrigation request requires a deterministic duration parameter (e.g., `sec: 120`). The firmware automatically bounds all manual operations to a hard ceiling, preventing perpetual valve actuation if client wireless communication drops.

---

## 3. Screen and Component Breakdown

The interface consists of a bottom navigation bar managing three primary modules: **Dashboard**, **History**, and **Settings**.

### 3.1 Dashboard Module (Live Telemetry and Operations)

The Dashboard provides immediate visibility into the physical environment and operational status.

#### A. Telemetry Header and Connection Monitor
- **Connection Beacon:** A pulsating dual-state vector icon (`Radio` / `WifiOff`) indicating live link health.
- **State Pill:** Dynamically highlights current FSM state (`IDLE`, `WATERING`, `SOAK`, `MANUAL`, `FAULT`).
- **Signal Quality & Time Reference:** Displays the ESP32 received Wi-Fi signal strength in dBm along with the synchronized controller clock timestamp.
- **Safety Alert Banner:** Automatically expands if safety interlocks trip (e.g., `FAULT: NO_FLOW`, `FAULT: LEAK`, or `SENSOR FAULT: < 2 Valid Sensors`).

#### B. Soil Moisture Fusion Card
- **Fusion Aggregate Display:** Calculates and displays the primary fused moisture metric derived from the median of healthy capacitive probes.
- **Individual Sensor Bars:** Each probe (Sensor 1, Sensor 2, Sensor 3) features a linear progress track with color thresholds:
  - Red (< 0%): Probe fault (disconnected, damaged, or out-of-range ADC reading < 300 or > 4000).
  - Amber (< 35%): Low soil moisture (triggers irrigation start window).
  - Green (35% to 75%): Optimal vegetative root zone moisture.
  - Blue (> 75%): Saturated soil (exceeds field capacity).

#### C. Hydraulic Actuation Bento Grid
A structured four-tile telemetry grid exposing actuator and flow dynamics:
- **Pump Indicator:** Real-time state of the 12V diaphragm pump relay with dynamic `Zap` icon illumination.
- **Valve Indicator:** Status of the 12V normally-closed solenoid valve.
- **Active Flow Rate:** Real-time volumetric throughput in L/min calculated from the Hall-effect sensor interrupt frequency.
- **Session and Lifetime Delivery:** Displays total water dispensed during the current cycle alongside the cumulative lifetime volume in liters retrieved from ESP32 non-volatile storage (NVS).

#### D. Tactical Command Card
- **Manual Pulse Trigger:** Dispatches a timed 120-second pulse dose.
- **Emergency STOP:** Immediately de-energizes the pump relay, sequences valve shutoff, and forces the controller into `IDLE`.
- **Fault Reset Button:** Appears when the controller is in a latched `FAULT` condition. Pressing this verifies safety clearance and releases the interlock.
- **Automation & Weather Skips:** Dual switches to toggle autonomous schedule execution and enable manual rain-delay hold.

---

### 3.2 History Module (24-Hour Telemetry Sparkline)

The History view visualizes soil moisture trends over the preceding 24 hours:

- **Vector Sparkline (`react-native-svg`):** Renders up to 288 data points captured at 5-minute intervals from the controller's internal RAM ring buffer.
- **Hysteresis Boundary Lines:**
  - Amber Dashed Line: The configured lower threshold (`lowPct`). When the fused reading dips below this line within an active time window, irrigation initiates.
  - Emerald Dashed Line: The configured upper target (`highPct`). Irrigation cycles terminate once the moisture curve ascends to this marker.
- **Permanent Logging Indicator:** Details that full granular, per-second CSV records (including raw ADC, flow pulses, and transition events) are permanently preserved on the onboard microSD card via the serial data logger.

---

### 3.3 Settings Module (Configuration and Calibration)

The Settings view provides field engineers and operators with full control over operational parameters.

#### A. Network Endpoint Configuration
- Allows changing the device IP address (e.g., from default AP mode `192.168.4.1` to a fixed router IP on the farm's subnet).
- Saves persistently in device storage via `AsyncStorage`.

#### B. Cycle-Soak Irrigation Parameters
Configures the closed-loop control constants stored in the controller's NVS memory:
- **Low Moisture Threshold (%):** Moisture percentage that initiates watering.
- **High Moisture Target (%):** Target percentage that ends the watering session.
- **Pulse Duration (seconds):** Active sprinkler spray runtime per pulse.
- **Soak Duration (seconds):** Absorption wait time between pulses to allow moisture penetration into the root zone.
- **Maximum Pulses:** Cycle-soak repetition limit per session to avoid runoff.
- **Volume Cap (liters):** Maximum permissible water delivery per session (safety cutoff).
- **Irrigation Windows 1 & 2:** Permitted diurnal time ranges (HH:MM - HH:MM) to restrict watering to low-evaporation dawn and dusk periods.

#### C. Two-Point Field Sensor Calibration
Direct controls for calibrating capacitive soil probes:
- **Set DRY:** Records baseline ADC reading in ambient air.
- **Set WET:** Records baseline ADC reading with probes submerged in water up to the calibration line.
- The firmware automatically maps the analog curve between these calibrated endpoints.

#### D. Local Network Provisioning
- Allows configuring target Wi-Fi SSID and password to migrate the ESP32 from standalone SoftAP mode to the local agricultural or greenhouse Wi-Fi network.

---

## 4. REST API Endpoint Reference

| Method | Endpoint | Description | Request Payload | Response Attributes |
|---|---|---|---|---|
| `GET` | `/api/status` | Real-time state query | None | `state`, `pump`, `valve`, `flow`, `session`, `total`, `avg`, `moist[]`, `time`, `rssi`, `auto`, `rainSkip`, `fault` |
| `GET` | `/api/config` | Read runtime parameters | None | `lowPct`, `highPct`, `pulseSec`, `soakSec`, `maxCycles`, `maxLiters`, `w1s`, `w1e`, `w2s`, `w2e` |
| `POST` | `/api/config` | Update runtime parameters | Partial/Full JSON config | `{"ok": true}` |
| `POST` | `/api/manual` | Trigger timed manual run | `{"sec": <seconds>}` | `{"ok": true, "duration": <sec>}` |
| `POST` | `/api/stop` | Immediate emergency shutoff | None | `{"ok": true}` |
| `POST` | `/api/reset` | Clear latched fault state | None | `{"ok": true}` |
| `POST` | `/api/calibrate` | Store 2-point ADC limits | `{"kind": "dry" \| "wet"}` | `{"ok": true}` |
| `POST` | `/api/time` | Sync RTC to phone clock | `{"y", "mo", "d", "h", "mi", "s"}` | `{"ok": true}` |
| `GET` | `/api/history` | Retrieve 24h sample buffer | None | `{"v": [m_0, m_1, ..., m_n]}` |
| `POST` | `/api/wifi` | Provision Wi-Fi credentials | `{"ssid": "...", "pass": "..."}` | `{"ok": true}` |

---

## 5. Safety Interlocks and Operational Rules

The application reflects five hardware-enforced safety interlocks executed independently by the ESP32 firmware:

1. **Dry-Run Interlock (`FAULT: NO_FLOW`):**
   If the pump is energized for 10 seconds but the flow sensor registers less than 0.5 L/min (indicating an empty tank or dry intake), power is severed instantly to prevent pump diaphragm destruction.
2. **Leak and Burst Interlock (`FAULT: LEAK`):**
   If continuous flow (> 0.5 L/min for 10 seconds) is detected while both pump and solenoid valve are closed, the system trips into a latched fault state.
3. **Sensor Redundancy Interlock:**
   Automatic irrigation requires at least two valid capacitive soil sensors. If two or more probes fail, the dashboard warns the user and suspends automatic watering while maintaining manual override capabilities.
4. **Time Window Gating:**
   If the RTC is uninitialized or unset, automatic cycles are halted to prevent inadvertent midday watering under intense sunlight.
5. **Session Safety Caps:**
   Every irrigation session is constrained by both time (`maxCycles * pulseSec`) and volume (`maxLiters`). If either threshold is reached, watering terminates regardless of moisture readings.
