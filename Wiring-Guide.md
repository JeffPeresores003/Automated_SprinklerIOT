# Automated Sprinkler Irrigation System — Complete Wiring Guide

**Board:** FireBeetle ESP32 (DFRobot) with IO Shield for FireBeetle 2
**Power:** 12V Solar system (100W panel + charge controller + 12V 25Ah gel battery)

> ⚠️ **IMPORTANT:** Read this entire document before soldering or connecting anything. The order of connections matters, and some mistakes (like feeding 12V into the ESP32 or 5V into a GPIO) can permanently destroy components.

---

## Table of Contents

1. [System Overview Diagram](#1-system-overview-diagram)
2. [Pin Map — Complete GPIO Assignments](#2-pin-map--complete-gpio-assignments)
3. [Power Distribution Tree](#3-power-distribution-tree)
4. [Wiring Each Component (Step by Step)](#4-wiring-each-component-step-by-step)
   - 4.1 [12V → 5V Buck Converter](#41-12v--5v-buck-converter)
   - 4.2 [ESP32 FireBeetle + IO Shield](#42-esp32-firebeetle--io-shield)
   - 4.3 [SD2405 RTC Module (I2C)](#43-sd2405-rtc-module-i2c)
   - 4.4 [Soil Moisture Sensors ×3 (ADC)](#44-soil-moisture-sensors-3-adc)
   - 4.5 [Water Flow Sensor with Voltage Divider](#45-water-flow-sensor-with-voltage-divider)
   - 4.6 [Relay Module (2-Channel, Active-LOW)](#46-relay-module-2-channel-active-low)
   - 4.7 [12V Diaphragm Pump + Flyback Diode](#47-12v-diaphragm-pump--flyback-diode)
   - 4.8 [12V Solenoid Valve + Flyback Diode](#48-12v-solenoid-valve--flyback-diode)
   - 4.9 [Serial Data Logger + MicroSD Card](#49-serial-data-logger--microsd-card)
   - 4.10 [Solar Power System](#410-solar-power-system)
5. [Complete Wiring Table](#5-complete-wiring-table)
6. [Critical Wiring Rules](#6-critical-wiring-rules)
7. [Voltage Divider Calculations](#7-voltage-divider-calculations)
8. [Common Ground — Why It Matters](#8-common-ground--why-it-matters)
9. [Boot-Safe Relay Wiring](#9-boot-safe-relay-wiring)
10. [Wire Gauge Reference](#10-wire-gauge-reference)
11. [Pre-Power Checklist](#11-pre-power-checklist)
12. [Troubleshooting Wiring Issues](#12-troubleshooting-wiring-issues)

---

## 1. System Overview Diagram

```
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                            SOLAR POWER SYSTEM                              │
  │  [100W Solar Panel] → [Charge Controller] → [12V 25Ah Gel Battery]         │
  │                                                    │                       │
  │                                             [10A Inline Fuse]              │
  │                                                    │                       │
  │                                              12V MAIN BUS                  │
  └──────────────────────────────────┬─────────────────┬───────────────────────┘
                                     │                 │
                              ┌──────▼──────┐   ┌──────▼──────┐
                              │ Buck Conv.  │   │ Relay Module │
                              │ 12V → 5V    │   │ 2-Channel    │
                              │ (≥2A out)   │   │              │
                              └──────┬──────┘   │ CH1──→ Pump  │
                                     │ 5V       │ CH2──→ Valve │
                   ┌─────────────────▼─────┐    └──────▲──────┘
                   │    FireBeetle ESP32    │           │
                   │    + IO Shield        │     GPIO 25,26
                   │                       ├───────────┘
                   │  3.3V ─┬──┬──┬──┐     │
                   │        │  │  │  │     │
                   └────────┼──┼──┼──┼─────┘
                            │  │  │  │
                     ┌──────┘  │  │  └──────┐
                     │         │  │         │
               ┌─────▼───┐ ┌──▼──▼──┐ ┌────▼─────────┐
               │ SD2405   │ │ Soil   │ │ Serial Data  │
               │ RTC      │ │ ×3     │ │ Logger+SD    │
               │ (I2C)    │ │ (ADC)  │ │ (UART)       │
               └──────────┘ └────────┘ └──────────────┘

                     5V ───→ [Flow Sensor] ───→ GPIO 27
                              (through 10k/20k voltage divider)


  WATER PATH:
  ┌──────────┐    ┌────────┐    ┌──────────────┐    ┌─────────────┐
  │ Water    │───→│ Filter │───→│ 12V Diaphragm│───→│ Flow Sensor │
  │ Drum     │    │        │    │ Pump         │    │ (½")        │
  │ (float   │    └────────┘    └──────────────┘    └──────┬──────┘
  │ valve)   │                                             │
  └──────────┘                                      ┌──────▼──────┐
                                                    │ 12V Solenoid│
                                                    │ Valve (½")  │
                                                    └──────┬──────┘
                                                           │
                                                    ┌──────▼──────┐
                                                    │ ½" Pipe to  │
                                                    │ Micro-      │
                                                    │ Sprinklers  │
                                                    └─────────────┘
```

---

## 2. Pin Map — Complete GPIO Assignments

| Function | ESP32 GPIO | IO Shield Label | Pin Type | Direction | Voltage | Notes |
|---|---|---|---|---|---|---|
| **Relay CH1 (Pump)** | GPIO 25 | D2 | Digital | OUTPUT | 3.3V logic | Active LOW |
| **Relay CH2 (Valve)** | GPIO 26 | D3 | Digital | OUTPUT | 3.3V logic | Active LOW |
| **Flow sensor pulse** | GPIO 27 | D4 | Digital | INPUT (interrupt) | **3.3V** (after divider!) | FALLING edge |
| **Soil sensor 1** | GPIO 36 | A0 | Analog (ADC1) | INPUT | 0–3.3V | Input-only pin |
| **Soil sensor 2** | GPIO 39 | A1 | Analog (ADC1) | INPUT | 0–3.3V | Input-only pin |
| **Soil sensor 3** | GPIO 34 | A2 | Analog (ADC1) | INPUT | 0–3.3V | Input-only pin |
| **I2C SDA (RTC)** | GPIO 21 | SDA | I2C | Bidirectional | 3.3V | Default I2C bus |
| **I2C SCL (RTC)** | GPIO 22 | SCL | I2C | Bidirectional | 3.3V | Default I2C bus |
| **Logger TX** | GPIO 17 | TX2 | UART2 TX | OUTPUT | 3.3V | ESP32 TX → Logger RX |
| **Logger RX** | GPIO 16 | RX2 | UART2 RX | INPUT | 3.3V | ESP32 RX ← Logger TX |

### Pins to AVOID

| GPIO | Why |
|---|---|
| **0, 2, 12, 15** | Boot strapping pins — pulling them to unexpected levels at power-on can put the ESP32 into download mode or prevent boot. Never use for relays |
| **6, 7, 8, 9, 10, 11** | Connected to the internal SPI flash. Using them crashes the board |
| **Any ADC2 pin** (GPIO 0, 2, 4, 12–15, 25–27 for analog) | ADC2 is **disabled** when Wi-Fi is active. All soil sensors must use **ADC1** pins (32–39) |

> **Note:** GPIO 25, 26, 27 are fine as **digital** pins (relays, flow interrupt). They are only problematic as **analog** inputs when Wi-Fi is on.

---

## 3. Power Distribution Tree

```
 12V Gel Battery (25Ah)
      │
      ├──[10A FUSE]──┬──────────────────────────────────────────────────────┐
      │              │                                                      │
      │        ┌─────▼─────┐                                          ┌─────▼─────┐
      │        │ Buck Conv. │                                          │ Relay Mod. │
      │        │ IN+ ── IN- │                                          │ JD-VCC     │
      │        │ OUT+ ─ OUT-│                                          │ (coil pwr) │
      │        └──┬────┬───┘                                          └─────┬─────┘
      │           │    │                                                    │
      │          5V   GND                                            Relay COM pins
      │           │    │                                                    │
      │     ┌─────▼────▼─────┐                               ┌─────────────┼──────────────┐
      │     │  FireBeetle    │                               │             │              │
      │     │  VIN or 5V pin │                          ┌────▼────┐  ┌─────▼────┐         │
      │     │                │                          │ CH1 NO  │  │ CH2 NO   │         │
      │     │  Provides 3.3V │                          │ → Pump+ │  │ → Valve+ │         │
      │     │  rail          │                          └─────────┘  └──────────┘         │
      │     └────┬───────────┘                               │              │             │
      │          │ 3.3V                                      │              │             │
      │     ┌────▼────────────────┐                     [Pump -]       [Valve -]          │
      │     │                     │                          │              │             │
      │  ┌──▼──┐ ┌──▼──┐ ┌──▼──┐ │                          └──────┬───────┘             │
      │  │RTC  │ │Soil │ │Log- │ │                                 │                     │
      │  │VCC  │ │×3   │ │ger  │ │                                 ▼                     │
      │  │3.3V │ │VCC  │ │VCC  │ │                           Battery GND ◄───────────────┘
      │  └─────┘ │3.3V │ │3.3V │ │
      │          └─────┘ └─────┘ │
      │                          │
      │         5V ──→ Flow sensor VCC (with divider on output)
      │
      └── Battery GND ── (COMMON GROUND for everything)
```

### Power rails summary

| Rail | Source | Supplies | Current |
|---|---|---|---|
| **12V** | Battery (through fuse) | Relay coils, pump, valve, buck converter input | Up to ~8A peak (pump) |
| **5V** | Buck converter output | FireBeetle VIN, flow sensor | ~500mA typical |
| **3.3V** | FireBeetle onboard regulator | RTC, 3× soil sensors, serial data logger | ~200mA typical |

---

## 4. Wiring Each Component (Step by Step)

### 4.1 12V → 5V Buck Converter

**What it does:** Converts the battery's 11.5–14.4V to a stable 5.0V for the ESP32 and flow sensor. A buck converter (not a linear regulator) is used because it wastes very little energy as heat — critical in a sealed outdoor enclosure on solar power.

**Component:** LM2596 or XL4015 module (adjustable output, ≥ 2A)

**Wiring:**

```
Battery +12V ──[10A FUSE]──→ Buck IN+
Battery GND ────────────────→ Buck IN-
Buck OUT+ ──────────────────→ FireBeetle 5V/VIN pin
Buck OUT- ──────────────────→ FireBeetle GND
```

**⚠️ CRITICAL SETUP STEP:** Before connecting anything to the ESP32:
1. Connect battery to the buck converter input
2. Use a multimeter on the output terminals
3. Adjust the trim pot until it reads exactly **5.0V** (±0.1V)
4. Only then connect the output to the ESP32

**Why 2A minimum:** The ESP32's Wi-Fi radio draws short current spikes (~300mA). An undersized supply causes brownout resets — the board randomly reboots under Wi-Fi activity. The 2A rating provides comfortable headroom.

---

### 4.2 ESP32 FireBeetle + IO Shield

**What it does:** The IO Shield breaks out all ESP32 pins to convenient headers with labels (A0–A4, D2–D13, I2C, UART, power ports). No soldering needed for most connections.

**Wiring:**

```
Buck 5V OUT+ ──→ Shield 5V pin (or FireBeetle VIN)
Buck GND     ──→ Shield GND
```

The IO Shield then provides:
- **3.3V rail** — from the FireBeetle's onboard regulator (for RTC, soil sensors, logger)
- **Labeled analog pins** — A0 (GPIO36), A1 (GPIO39), A2 (GPIO34)
- **Labeled digital pins** — D2–D13
- **I2C header** — SDA/SCL
- **UART header** — RX2/TX2

**⚠️ Verify your board model:** Your list pairs a FireBeetle ESP32 with an IO Shield for **FireBeetle 2 (ESP32-E/M0)**. The pin numbering can differ. Check the silkscreen on your actual board and confirm the GPIO numbers match the pin labels in section 2.

---

### 4.3 SD2405 RTC Module (I2C)

**What it does:** Keeps accurate time (±5 ppm ≈ a few seconds per month) with its own coin-cell backup battery. The ESP32 loses time on every reboot, and there may be no Wi-Fi/NTP in the field. The RTC gates automatic watering windows and timestamps all log entries.

**I2C Address:** `0x32`

**Wiring:**

```
RTC VCC ──→ 3.3V (from IO Shield 3.3V pin)
RTC GND ──→ GND
RTC SDA ──→ GPIO 21 (Shield SDA)
RTC SCL ──→ GPIO 22 (Shield SCL)
```

**Why 3.3V (not 5V):** The SD2405 works at 2.0–5.5V, but powering it at 3.3V means its I2C lines operate at 3.3V logic, which is safe for the ESP32 without level shifting.

**Installation notes:**
- Insert a fresh **CR1220** or **CR1225** coin cell into the RTC module (check your module's battery type). This keeps time during power outages.
- After uploading the firmware, set the time from the app (Settings → the app auto-syncs on connect) or use `curl`:
  ```bash
  curl -X POST -H "X-Token: change-me-123" -H "Content-Type: application/json" \
    -d '{"y":2026,"mo":10,"d":11,"h":6,"mi":0,"s":0}' \
    http://192.168.4.1/api/time
  ```
- The I2C pull-up resistors (4.7kΩ) are usually built into the RTC module. If the time reads garbage, try adding external 4.7kΩ pull-ups from SDA→3.3V and SCL→3.3V.

---

### 4.4 Soil Moisture Sensors ×3 (ADC)

**What they do:** Capacitive soil moisture sensors output an analog voltage between ~1.2V (wet) and ~2.5V (dry). "Capacitive" means no exposed metal electrodes, so they don't corrode over time like resistive sensors.

Three sensors are used for **redundancy and noise rejection**, not for three separate zones. The firmware takes the median of the three readings, which eliminates a single faulty sensor from corrupting the measurement.

**Wiring (each sensor is identical):**

```
Sensor 1:
  VCC ──→ 3.3V
  GND ──→ GND
  AOUT ──→ GPIO 36 (A0 on shield)

Sensor 2:
  VCC ──→ 3.3V
  GND ──→ GND
  AOUT ──→ GPIO 39 (A1 on shield)

Sensor 3:
  VCC ──→ 3.3V
  GND ──→ GND
  AOUT ──→ GPIO 34 (A2 on shield)
```

**Why ADC1 only:** The ESP32 has two ADC units. **ADC2 is completely disabled when Wi-Fi is active** (it shares hardware with the Wi-Fi radio). Since our system uses Wi-Fi constantly, all analog sensors **must** be on ADC1 pins: GPIO 32, 33, 34, 35, 36, 39.

**Why 3.3V (not 5V):** Powering the sensors at 3.3V means their output stays within the ESP32's safe ADC input range (0–3.3V). At 5V, the output could exceed 3.3V and damage the ADC input.

**Calibration:**
1. Hold all sensors in open air → in the app, press "Set DRY"
2. Submerge the sensing area (NOT the electronics at the top) in water → press "Set WET"
3. The firmware stores these raw readings and interpolates between them

**Field placement tips:**
- Bury sensors at the root depth of your plants (typically 10–15cm)
- Space them across the irrigation zone to capture variation
- Route sensor cables **away** from the pump and relay wires (electromagnetic interference causes noisy readings)
- Add a **100nF (0.1µF) ceramic capacitor** from each sensor's signal wire to GND, close to the ESP32, to filter noise

---

### 4.5 Water Flow Sensor with Voltage Divider

**What it does:** A Hall-effect sensor with a small turbine. As water flows through, the turbine spins a magnet past the Hall sensor, producing electrical pulses. The firmware counts pulses via a hardware interrupt: **450 pulses = 1 litre** (from the datasheet — verify with a measured bucket test and adjust `FLOW_PPL` if needed).

**⚠️ CRITICAL — Voltage Divider Required:**

The flow sensor outputs **5V pulses** when powered at 5V. ESP32 GPIO pins are rated for **3.3V maximum**. Connecting the 5V output directly to GPIO 27 **will damage the pin** — it may work initially but will degrade the chip over time.

**Voltage divider circuit:**

```
                    10 kΩ
Flow OUT (5V) ────[████]────┬──── GPIO 27 (ESP32)
                            │
                         [████] 20 kΩ
                            │
                           GND

Calculation: V_out = 5V × 20k / (10k + 20k) = 3.33V ✓
```

**Resistor selection:** If you can't find a 20kΩ resistor, use two 10kΩ in series. Or use 15kΩ + 33kΩ (gives 3.44V — still safe). The exact ratio isn't critical as long as the output stays below 3.3V.

**Full wiring:**

```
Flow sensor RED wire   ──→ 5V (from buck converter)
Flow sensor BLACK wire ──→ GND
Flow sensor YELLOW wire ──→ [10kΩ] ──→ GPIO 27
                                   └──→ [20kΩ] ──→ GND
```

**Why 5V power:** The flow sensor's Hall element and internal pull-up need 5V to function properly. At 3.3V, some models produce unreliable or no pulses.

**Mounting:** Install the flow sensor **after** the pump and **before** the solenoid valve in the water path. The arrow on the sensor body must point in the direction of water flow.

**Accuracy note:** The sensor is rated for 1–30 L/min. Below ~1 L/min, accuracy drops significantly. If your micro-sprinkler setup produces very low flow, you may need to lower the `minFlow` threshold in the firmware settings.

---

### 4.6 Relay Module (2-Channel, Active-LOW)

**What it does:** The relay module uses a small signal voltage (3.3V GPIO) to switch high-power 12V loads (pump and solenoid valve). Each relay channel has a set of contacts: COM (common), NO (normally open), and NC (normally closed).

**⚠️ Verify 3.3V compatibility:** Most opto-isolated relay modules have a **JD-VCC** jumper. This jumper separates the relay coil power from the signal input power:
- **With jumper:** Coil and signal share the same VCC. Signal threshold depends on VCC.
- **Without jumper (recommended):** Connect JD-VCC to 5V separately, and VCC to 3.3V. This guarantees the optocoupler triggers reliably at 3.3V logic levels.

**Wiring:**

```
Relay Module:
  VCC    ──→ 3.3V (signal/optocoupler power)
  JD-VCC ──→ 5V   (coil power — remove the jumper between VCC and JD-VCC first)
  GND    ──→ GND
  IN1    ──→ GPIO 25 (pump control)
  IN2    ──→ GPIO 26 (valve control)
```

**Active-LOW logic:**
| GPIO State | Relay State | Load |
|---|---|---|
| HIGH (3.3V) | **OFF** (contacts open) | Pump/valve OFF |
| LOW (0V) | **ON** (contacts closed) | Pump/valve ON |

The firmware sets both GPIOs HIGH at boot **before** configuring them as outputs. This prevents a brief LOW glitch during boot from activating the relays.

**12V load connections (see sections 4.7 and 4.8):**

```
Relay CH1 (Pump):
  COM ──→ Battery +12V (through fuse)
  NO  ──→ Pump + terminal
  NC  ──  (leave disconnected)

Relay CH2 (Valve):
  COM ──→ Battery +12V (through fuse)
  NO  ──→ Solenoid valve + terminal
  NC  ──  (leave disconnected)
```

**Why NO (normally open):** If the ESP32 loses power or crashes, both relays de-energize and the contacts open. The pump and valve turn OFF. This is the **fail-safe** configuration.

---

### 4.7 12V Diaphragm Pump + Flyback Diode

**What it does:** Pressurises the water line for the micro-sprinkler nozzles. Self-priming (can pull water up a short distance), but should not run dry.

**Hardware Specs (On Hand):**
- **Model:** GOOD PUMPS 12V DC (9–14.4V operating range)
- **Rated Power & Current:** 72W, 6A (inrush surge up to 8–10A)
- **Flow & Pressure:** 6 L/min max flow, 0.9 MPa (9 bar / 130 PSI) automatic cutoff pressure switch
- **Wiring Gauge:** 14–16 AWG (handles 6A without voltage sag)
- **Suction Filter:** Stainless steel mesh suction strainer attached to intake hose inside drum

**Wiring:**

```
Relay CH1 NO ──→ Pump + (red wire)
Pump - (black) ──→ Battery GND

Flyback diode (1N5408 strongly recommended for 6A motor; 1N4007 minimum) across pump terminals:
  Anode (no stripe)  ──→ Pump - (GND side)
  Cathode (stripe ▌) ──→ Pump + (12V side)
```

**Visual:**

```
            Relay CH1 NO
                │
                ▼ (+)
         ┌──────────────┐
         │   12V PUMP   │   ◄── Flyback diode 1N5408 (stripe toward +)
         └──────────────┘        ┌──|◄──┐
                │ (-)             │      │
                ▼                 │      │
           Battery GND      Pump (-)  Pump (+)
```

**Why the flyback diode is essential:** The 72W pump motor is a heavy inductive load (it has a large coil). When Relay CH1 switches it OFF, the collapsing magnetic field generates a reverse voltage spike that can reach several hundred volts:
- Arc across relay contacts, pitting or welding them shut (a welded relay leaves the pump running continuously)
- Propagate as electrical noise, resetting the ESP32
- Corrupt SD card writes
- Distort soil sensor readings

The **1N5408 diode (rated 3A continuous, 200A surge)** easily handles the inductive discharge of this 6A motor. **Install it in reverse** across the motor — cathode (silver stripe) to the + side, anode to the – side. This way it doesn't conduct during normal operation but clamps the reverse spike instantly.

---

### 4.8 12V Solenoid Valve + Flyback Diode

**What it does:** Opens and closes the sprinkler line. It is a **normally-closed** (NC) type, meaning it is spring-closed when unpowered — water stops flowing if power is lost (fail-safe).

**Wiring (identical pattern to the pump):**

```
Relay CH2 NO ──→ Solenoid valve + wire
Valve - wire ──→ Battery GND

Flyback diode (1N5408 or 1N4007) across the valve terminals:
  Anode (no stripe)  ──→ Valve - (GND side)
  Cathode (stripe ▌) ──→ Valve + (12V side)
```

**Valve sequencing (handled by firmware):**

| Step | Valve | Pump | Why |
|---|---|---|---|
| 1. Start | OPEN first | — | Opens the water path before pressure builds |
| 2. +1 second | (still open) | START | Water has somewhere to go |
| 3. End of pulse | (still open) | STOP | Pressure drops while path is open |
| 4. +2 seconds | CLOSE | (already off) | Residual pressure has dissipated |

This sequence prevents:
- **Dead-heading:** Pump running against a closed valve (high pressure, possible burst)
- **Water hammer:** Sudden valve closure while pump is running (pressure shock in pipes)

---

### 4.9 Serial Data Logger + MicroSD Card

**What it does:** The Fermion Serial Data Logger V2.0 is a standalone UART-to-SD module. The ESP32 sends text lines over UART, and the logger writes them to a file on the microSD card. This keeps the ESP32 fast (no SPI/SD stack needed) and protects internal flash from write wear.

**Wiring:**

```
Logger VCC ──→ 3.3V (NOT 5V — see explanation below)
Logger GND ──→ GND
Logger RXI ──→ GPIO 17 (ESP32 TX2 — data from ESP32 to logger)
Logger TXO ──→ GPIO 16 (ESP32 RX2 — data from logger to ESP32)
```

**⚠️ Why 3.3V power (not 5V):** When powered at 5V, the logger's TX output is a 5V signal. The ESP32's UART RX pin (GPIO 16) is NOT 5V tolerant. Powering the logger at 3.3V ensures its TX line stays at 3.3V logic levels, making it safe for the ESP32 without a level shifter.

**MicroSD card requirements:**
- **≤ 32 GB** capacity (FAT32 format required)
- **FAT32 formatted** — not exFAT (use Windows format tool, select FAT32)
- **Branded / high-endurance** card recommended (it sits in a hot enclosure)
- Insert the card into the logger module before powering on

**Logger configuration:**
- Default baud rate is usually **9600** (matching the firmware). Check the DFRobot wiki for your specific unit.
- The logger creates a new log file per power cycle (file names like `LOG00001.TXT`).
- The firmware writes a CSV header as the first line on each boot:
  ```
  time,m1,m2,m3,avg,state,pump,valve,flow_lpm,session_l,event
  ```

**Adding a 100µF capacitor:** Place a **100µF electrolytic capacitor** across the logger's VCC and GND pins, as close to the module as possible. This helps stabilize the power supply during SD card write operations (SD cards draw brief current spikes).

```
Logger VCC ──┬── 3.3V
             │
          [100µF] (+ toward VCC)
             │
Logger GND ──┴── GND
```

---

### 4.10 Solar Power System

**Components:**
- 100W solar panel
- 12V PWM or MPPT solar charge controller
- 12V 25Ah gel (VRLA) battery
- 10A inline fuse + fuse holder

**Wiring order (follow this sequence):**

```
Step 1: Connect battery to charge controller
  Battery + ──→ Controller BATT +
  Battery - ──→ Controller BATT -

Step 2: Connect solar panel to charge controller
  Panel + ──→ Controller SOLAR +
  Panel - ──→ Controller SOLAR -
  (Some controllers require battery first, panel second — check your manual)

Step 3: Connect the load (fused)
  Controller LOAD + ──→ [10A FUSE] ──→ 12V bus (buck converter, relay module)
  Controller LOAD - ──→ GND bus
  
  OR if your controller has no LOAD terminals:
  Battery + ──→ [10A FUSE] ──→ 12V bus
```

**⚠️ Fuse placement:** The fuse goes on the **positive** wire, as close to the battery terminal as possible. Its job is to protect the **wiring** from a short circuit. A gel battery can deliver hundreds of amps into a short — without a fuse, the wire itself melts.

**Energy budget:**

| Load | Power | Daily use | Daily energy |
|---|---|---|---|
| ESP32 + sensors (continuous) | ~0.5W | 24 h | ~12 Wh |
| Pump (4A × 12V = 48W) | 48W | ~20 min/day | ~16 Wh |
| **Total** | | | **~28 Wh** |

| Supply | Daily energy |
|---|---|
| 100W panel (Philippine sun, ~4 peak hours) | ~300–400 Wh |
| Battery usable capacity (50% DoD) | ~150 Wh |

**Conclusion:** The system has generous solar margin. The battery alone can sustain ~5 days of cloudy weather without panel input.

**Placement tips:**
- Panel facing **south** (in the Philippines, near the equator, tilt ~15° from horizontal)
- Keep the electronics enclosure **in shade** — heat shortens battery and component life
- Use **IP65-rated** or better enclosure for the controller, battery, and electronics
- Route cables through **cable glands** (PG7 for signal cables, PG9/PG11 for power cables)

---

## 5. Complete Wiring Table

This is a summary of every wire connection in the system.

### Low-voltage connections (3.3V / 5V side)

| From | To | Wire | Notes |
|---|---|---|---|
| Buck 5V OUT+ | FireBeetle VIN | 20 AWG | Set to 5.0V with multimeter first |
| Buck GND | FireBeetle GND | 20 AWG | |
| Shield 3.3V | RTC VCC | 22 AWG | |
| Shield GND | RTC GND | 22 AWG | |
| Shield SDA (GPIO 21) | RTC SDA | 22 AWG | |
| Shield SCL (GPIO 22) | RTC SCL | 22 AWG | |
| Shield 3.3V | Soil sensor 1 VCC | 22 AWG | |
| Shield GND | Soil sensor 1 GND | 22 AWG | |
| Shield A0 (GPIO 36) | Soil sensor 1 AOUT | 22 AWG | |
| Shield 3.3V | Soil sensor 2 VCC | 22 AWG | |
| Shield GND | Soil sensor 2 GND | 22 AWG | |
| Shield A1 (GPIO 39) | Soil sensor 2 AOUT | 22 AWG | |
| Shield 3.3V | Soil sensor 3 VCC | 22 AWG | |
| Shield GND | Soil sensor 3 GND | 22 AWG | |
| Shield A2 (GPIO 34) | Soil sensor 3 AOUT | 22 AWG | |
| Buck 5V OUT+ | Flow sensor VCC (red) | 22 AWG | Flow sensor needs 5V |
| GND | Flow sensor GND (black) | 22 AWG | |
| Flow sensor OUT (yellow) | 10kΩ resistor → GPIO 27 | 22 AWG | **With voltage divider** |
| Junction (10kΩ–GPIO27) | 20kΩ resistor → GND | 22 AWG | Divider lower leg |
| Shield 3.3V | Logger VCC | 22 AWG | **3.3V, not 5V** |
| Shield GND | Logger GND | 22 AWG | |
| Shield TX2 (GPIO 17) | Logger RXI | 22 AWG | ESP32 TX → Logger RX |
| Shield RX2 (GPIO 16) | Logger TXO | 22 AWG | ESP32 RX ← Logger TX |
| Shield D2 (GPIO 25) | Relay IN1 | 22 AWG | Pump control |
| Shield D3 (GPIO 26) | Relay IN2 | 22 AWG | Valve control |
| Shield 3.3V | Relay VCC | 22 AWG | Signal/optocoupler power |
| Buck 5V OUT+ | Relay JD-VCC | 22 AWG | Coil power (remove jumper) |
| Shield GND | Relay GND | 22 AWG | |

### High-voltage connections (12V side)

| From | To | Wire | Notes |
|---|---|---|---|
| Battery + | 10A fuse | **16 AWG** | As short as possible |
| 10A fuse | Buck converter IN+ | **16 AWG** | |
| 10A fuse | Relay CH1 COM | **16 AWG** | Pump power supply |
| 10A fuse | Relay CH2 COM | **16 AWG** | Valve power supply |
| Relay CH1 NO | Pump + (red) | **16 AWG** | |
| Pump – (black) | Battery GND | **16 AWG** | |
| Relay CH2 NO | Solenoid valve + | **16 AWG** | |
| Solenoid valve – | Battery GND | **16 AWG** | |
| Battery – | Buck converter IN– | **16 AWG** | |

### Protection components

| Component | Across | Orientation |
|---|---|---|
| Flyback diode (1N5408) | Pump terminals | Cathode (stripe) → Pump + |
| Flyback diode (1N5408) | Valve terminals | Cathode (stripe) → Valve + |
| 100nF ceramic capacitor | Each soil sensor signal → GND | Non-polarized |
| 100µF electrolytic cap | Logger VCC → GND | + toward VCC |
| 10A fuse | Battery + line | As close to battery as possible |

---

## 6. Critical Wiring Rules

### Rule 1: Common Ground
**All GND connections must be tied together.** The battery GND, buck converter GND, ESP32 GND, relay GND, sensor GNDs, pump GND, valve GND — all connect to the same ground bus. Without common ground, signals are referenced to different potentials and nothing works reliably.

### Rule 2: No 5V on ESP32 GPIO
**Never connect a 5V signal directly to an ESP32 pin.** The only 5V device in this system is the flow sensor, which is why it needs a voltage divider. Everything else is powered at 3.3V to keep signals safe.

### Rule 3: ADC1 Only for Analog
**All analog inputs must use ADC1 pins (GPIO 32–39).** ADC2 pins share hardware with Wi-Fi and are unusable when Wi-Fi is active.

### Rule 4: Relay OFF Before OUTPUT
**Set relay GPIOs HIGH before configuring them as outputs** (already done in the firmware). This prevents a boot glitch where the pin briefly floats LOW and activates the relay.

### Rule 5: Fuse Near Battery
**The fuse protects the wiring, not the devices.** It must be as close to the battery positive terminal as physically possible.

### Rule 6: Flyback Diodes on All Inductive Loads
**Every motor, solenoid, or relay coil needs a flyback diode.** The relay module has built-in flyback diodes for its coils, but the pump and valve do not.

### Rule 7: Keep Signal and Power Cables Separate
**Route sensor cables (especially soil sensors) away from the pump and relay wires.** The pump draws several amps and creates electromagnetic interference that causes noisy analog readings.

---

## 7. Voltage Divider Calculations

### Flow sensor divider (5V → 3.3V)

```
V_out = V_in × R2 / (R1 + R2)
V_out = 5.0V × 20kΩ / (10kΩ + 20kΩ)
V_out = 5.0V × 0.667
V_out = 3.33V ✓ (safe for ESP32)
```

**Alternative resistor pairs that also work:**

| R1 | R2 | V_out | OK? |
|---|---|---|---|
| 10kΩ | 20kΩ | 3.33V | ✅ Best choice |
| 10kΩ | 10kΩ + 10kΩ (series) | 3.33V | ✅ Uses common values |
| 15kΩ | 33kΩ | 3.44V | ⚠️ Borderline, works in practice |
| 10kΩ | 15kΩ | 3.00V | ✅ Safe, pulse amplitude slightly reduced |
| 4.7kΩ | 10kΩ | 3.40V | ⚠️ Higher current draw |

### Optional: Battery voltage divider (12V → ADC range)

If you want to monitor battery voltage from the ESP32:

```
Battery + ──[100kΩ]──┬── GPIO 35 (A3, ADC1)
                     │
                  [22kΩ]
                     │
                    GND

V_out = 14.4V × 22k / (100k + 22k) = 2.60V  (fully charged, safe)
V_out = 11.0V × 22k / (100k + 22k) = 1.98V  (low battery)
```

High resistance values (100kΩ + 22kΩ) are used to minimize current drain from the battery. The trade-off is slightly more susceptibility to noise.

---

## 8. Common Ground — Why It Matters

Every electrical signal is a voltage *difference* between two points. When you measure a 3.3V signal from a sensor, that 3.3V is relative to GND. If the sensor's GND and the ESP32's GND aren't connected, the ESP32 has no reference point and the reading is meaningless (or dangerous).

**Ground loop prevention:**
- Use a **star topology** for ground connections: all ground wires go to a central GND bus (a terminal block), not daisy-chained from one device to the next.
- Keep the GND wire for the 12V loads (pump, valve) as a separate run to the bus, not shared with the signal GND. This prevents pump motor noise from appearing on the sensor ground.

```
                    ┌── RTC GND
                    ├── Soil 1 GND
                    ├── Soil 2 GND
   CENTRAL GND ────┼── Soil 3 GND
   BUS (terminal    ├── Flow sensor GND
   block)           ├── Logger GND
                    ├── Relay GND
                    ├── ESP32 GND
                    ├── Buck converter GND
                    ├── Pump GND         ◄─── heavier gauge wire
                    ├── Valve GND        ◄─── heavier gauge wire
                    └── Battery GND
```

---

## 9. Boot-Safe Relay Wiring

When the ESP32 boots, GPIOs are in an undefined state for a brief moment (microseconds to milliseconds). If a relay is connected to a pin that floats LOW during this time, the relay clicks ON momentarily — the pump or valve could jerk.

**The firmware handles this in software:**

```cpp
// Set HIGH (relay OFF) BEFORE configuring as output
digitalWrite(PIN_RELAY_PUMP,  RELAY_OFF);   // HIGH = relay off
pinMode(PIN_RELAY_PUMP, OUTPUT);            // Now configure as output
```

**Hardware backup (optional but recommended):**
Add a **10kΩ pull-up resistor** from each relay input to 3.3V. This ensures the pin is pulled HIGH (relay OFF) even during the brief boot period before the firmware runs.

```
3.3V ──[10kΩ]──┬── Relay IN1
               │
          GPIO 25
```

**Test:** After wiring, power-cycle the ESP32 with the pump physically disconnected. Watch the relay LEDs — neither should flash at boot.

---

## 10. Wire Gauge Reference

| Application | Recommended Gauge | Current | Why |
|---|---|---|---|
| Battery → fuse → 12V bus | **16 AWG** (1.3mm²) | Up to 8A (pump) | Undersized wire drops voltage and heats up |
| Pump, valve leads | **16 AWG** | 4–6A | Same reason |
| Buck converter input | **16–18 AWG** | ~1A | Could use 18 AWG |
| Buck 5V output | **20 AWG** | ~500mA | Low current |
| All signal wires | **22 AWG** | <50mA | Sensors, I2C, UART, relay inputs |
| Soil sensor leads (long runs) | **22 AWG shielded** | <10mA | Shielding reduces noise pickup |

**For outdoor runs (sensor cables going to the field):**
- Use **multi-core shielded cable** (e.g. 3-core 22 AWG shielded)
- Connect the shield to GND at the **ESP32 end only** (one-sided grounding prevents ground loops)
- Use **waterproof connectors** or **heat-shrink tubing** at every outdoor junction

---

## 11. Pre-Power Checklist

Run through this list **before** applying power for the first time:

- [ ] **Buck converter output verified at 5.0V** (with multimeter, before connecting ESP32)
- [ ] **No shorts:** Ohmmeter between each power rail and GND shows no short circuit
- [ ] **Flyback diodes installed** across pump and valve, correct orientation (stripe toward +12V)
- [ ] **Flow sensor voltage divider** built and verified (output should be ~3.3V when sensor is powered)
- [ ] **Relay module JD-VCC jumper** removed (if using separate coil power)
- [ ] **Logger powered from 3.3V** (not 5V)
- [ ] **MicroSD card inserted** in logger, FAT32 formatted, ≤ 32GB
- [ ] **RTC coin cell battery** installed
- [ ] **All GNDs connected** to common bus
- [ ] **Soil sensors on ADC1 pins** (36, 39, 34)
- [ ] **No 5V signals connected directly to ESP32 GPIO**
- [ ] **Pump physically disconnected** for first boot test
- [ ] **10A fuse installed** near battery positive terminal

### First Power-On Procedure

1. Connect battery (through fuse) to buck converter only — verify 5V output
2. Connect buck to ESP32 — should boot, serial monitor shows "AutoSprinkler ready" at 115200 baud
3. Connect I2C (RTC) — serial should show valid time after sync from app
4. Connect soil sensors — `/api/status` should show moisture readings
5. Connect logger — check that a file appears on the SD card
6. Connect relay module (with pump disconnected) — test via app, listen for clicks
7. Connect pump and valve — run the safety test checklist from the implementation guide

---

## 12. Troubleshooting Wiring Issues

| Symptom | Likely Cause | Fix |
|---|---|---|
| ESP32 keeps rebooting | Brownout: buck converter can't supply enough current during Wi-Fi spikes | Use a ≥2A buck converter. Add a 470µF capacitor on the 5V output |
| Relay clicks at boot | GPIO floats LOW before firmware runs | Add 10kΩ pull-up resistors. Verify firmware sets HIGH before OUTPUT |
| Soil readings jump wildly | Electromagnetic interference from pump wires | Separate signal and power cables. Add 100nF caps on sensor signals |
| Soil reads ~0% or ~100% constantly | Sensor unplugged, or calibration is wrong | Check wiring. Re-run calibration (Set DRY / Set WET in app) |
| Flow reads 0 even with water | Divider wired wrong, or sensor powered at 3.3V instead of 5V | Check divider with multimeter. Power sensor from 5V |
| Flow reads ~0 at low flow rates | Below sensor's 1 L/min minimum | Add more nozzles to increase flow, or lower `minFlow` in settings |
| RTC time is garbage | Wrong I2C address, or pull-ups missing | Verify address 0x32 with I2C scanner sketch. Add 4.7kΩ pull-ups |
| No file on SD card | Logger baud mismatch, wrong format, or powered after first write | Set firmware to logger's baud (default 9600). Re-format card as FAT32 |
| App can't connect on Sprinkler-AP | Android routes traffic via mobile data | Tap "Stay connected" on the captive portal prompt, or turn off mobile data |
| Random ESP32 resets during pump operation | Missing flyback diodes, or shared ground noise | Install flyback diodes. Use star grounding topology |
| Pump runs weakly | Thin wire causing voltage drop | Use 16 AWG for 12V power runs. Measure voltage at pump terminals |

---

## Appendix: I2C Scanner Sketch

If you're having trouble with the RTC, upload this sketch to find all I2C devices on the bus:

```cpp
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Serial.println("I2C Scanner");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found device at 0x");
      Serial.println(addr, HEX);
    }
  }
  Serial.println("Scan done.");
}

void loop() {}
```

The SD2405 should appear at address **0x32**. If nothing appears, check SDA/SCL wiring and ensure the RTC module has power.

---

*Document generated for the AutoSprinkler IoT project. Refer to [Auto-Sprinkler-Implementation.md](Auto-Sprinkler-Implementation.md) for the full system design, algorithm details, and build order.*
