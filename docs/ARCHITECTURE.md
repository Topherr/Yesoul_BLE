# Architecture

This is the technical companion to the README. Topology, components, every role's GATT layout, what each ESP does on the wire.

## Roles

The same firmware compiles for one of three roles, selected by build flag in [`platformio.ini`](../platformio.ini):

| Role flag | Build env(s) | Default name | What it does |
|---|---|---|---|
| `-DDEVICE_ROLE_POWER` | `power` | `Yesoul_PWR` | BLE central to the bike. Publishes Cycling Power Service (CPS, `0x1818`) to a Garmin watch. Broadcasts each parsed frame over ESP-NOW for siblings. |
| `-DDEVICE_ROLE_SPEED` | `speed` | `Yesoul_SPD` | Receives `BikeFrame`s over ESP-NOW. Publishes Cycling Speed and Cadence Service (CSC, `0x1816`) to a Garmin watch. |
| `-DDEVICE_ROLE_TRAINER` | `trainer`, `trainer_c6` | `Yesoul_FTMS` | Tries to be BLE central to the bike directly (so a single C6 with this role is a complete standalone Zwift trainer). If another ESP already owns the bike, falls back to receiving over ESP-NOW. Either way, publishes Fitness Machine Service (FTMS, `0x1826`) to indoor-cycling apps. |

## Three deployment topologies

### Option A — Standalone Zwift / TrainerRoad / etc.

```mermaid
flowchart LR
    bike["Yesoul"]
    c6["ESP32-C6<br/><b>Yesoul_FTMS</b>"]
    zwift["Zwift / TR / etc.<br/>(phone, tablet, PC)"]

    bike -->|"FTMS BLE 0x1826<br/>Indoor Bike Data"| c6
    c6 -->|"FTMS BLE 0x1826<br/>full Indoor Bike Data"| zwift
```

Single ESP32-C6 (recommended) or any ESP32 in the `trainer` role. Connects to the bike directly, parses, re-emits FTMS to the app with `POWER_SCALE` applied to the inst-power field. Implements Fitness Machine Control Point with a no-op handler so Zwift recognises the device as "controllable" (the Yesoul has a manual resistance knob; we ACK gradient/target-power commands but can't physically apply them).

### Option B — Garmin watch only

```mermaid
flowchart LR
    bike["Yesoul"]
    pwr["WROOM-32<br/><b>Yesoul_PWR</b>"]
    spd["WROOM-32<br/><b>Yesoul_SPD</b>"]
    watch["Garmin epix 2<br/>(or any Fenix-class)"]

    bike -->|"FTMS BLE 0x1826"| pwr
    pwr -->|"CPS BLE 0x1818<br/>power + cadence"| watch
    pwr -.->|"ESP-NOW<br/>BikeFrame"| spd
    spd -->|"CSC BLE 0x1816<br/>speed + distance"| watch
```

Two WROOM-32s. POWER owns the bike connection (the Yesoul allows only one BLE central). SPEED never touches the bike — it receives `BikeFrame`s via ESP-NOW broadcast and synthesises wheel revolutions from the bike's reported speed.

The dual-ESP architecture exists because Garmin Fenix 7 / epix 2 firmware refuses to enumerate a single BLE peripheral that exposes both CPS and CSC under "Add Sensor → Speed" — and even when paired, only maintains one connection per physical peripheral regardless of distinct random-static addresses. Two physically separate radios = two devices to Garmin's stack. See [`JOURNEY.md`](JOURNEY.md) for the empirical trail.

### Option C — Garmin watch + Zwift simultaneously

```mermaid
flowchart LR
    bike["Yesoul"]
    pwr["WROOM-32<br/><b>Yesoul_PWR</b>"]
    spd["WROOM-32<br/><b>Yesoul_SPD</b>"]
    trn["ESP32-C6 or WROOM-32<br/><b>Yesoul_FTMS</b>"]
    watch["Garmin watch"]
    zwift["Zwift / TR / etc."]

    bike -->|"FTMS BLE 0x1826"| pwr
    pwr -->|"CPS BLE 0x1818"| watch
    pwr -.->|"ESP-NOW BikeFrame"| spd
    pwr -.->|"ESP-NOW BikeFrame"| trn
    spd -->|"CSC BLE 0x1816"| watch
    trn -->|"FTMS BLE 0x1826"| zwift
```

Three ESPs total. POWER scans the bike first and wins the central slot; TRAINER's bike-side scan fails to connect (one-central limit) and the role automatically falls back to receiving via ESP-NOW. POWER's broadcasts feed both SPEED (for the watch's CSC sensor) and TRAINER (for Zwift's smart trainer). End result: the watch records the ride natively and Zwift simultaneously reads the same ride as a controllable smart trainer.

Booting order matters: the ESP that scans the bike first becomes the central. In Option C, power on the WROOM-32 POWER first, then the others.

## Components

## Components

### Bike-side BLE client (POWER and standalone TRAINER)

5-state machine in `src/main.cpp`:

```
SCANNING → CONNECTED → STREAMING → DISCONNECTED → COOLDOWN → SCANNING
```

- Scans for FTMS service UUID `0x1826` and matches on UUID alone — the bike's advertised name isn't always present.
- 5-second notification watchdog: if no FTMS frames arrive for 5 s, flag a state transition. Stack-touching (disconnect, deleteClient) only happens in the app task, never inline from a BLE callback or watchdog timer.
- Scan backoff: 5 s → 10 s → 30 s → 60 s on consecutive failures.
- Memory hygiene: `NimBLEDevice::deleteClient()` is called on every reconnect cycle. The original upstream code leaked a `BLEClient` per cycle.

### FTMS parser ([`src/ftms_parser.cpp`](../src/ftms_parser.cpp))

Pure C++, no BLE dependencies. Reads the 16-bit flags field, walks the buffer field-by-field per the FTMS Indoor Bike Data spec. Returns false on truncated input. Tested host-side against a captured 45-frame log — see [PROTOCOL.md](PROTOCOL.md).

### Concurrency (POWER ESP)

```
BLE host task   →  notifyCallback  →  push BikeFrame to FreeRTOS queue
                                  →  esp_now_send (broadcast)

App (loop) task →  drain queue
                →  update derived state (g_speed_cmps, g_cadence_halfrpm, g_inst_power_w)
                →  publish CPS notification @ 1 Hz
```

No globals shared mutably across tasks.

### ESP-NOW relay

- Broadcast mode (`FF:FF:FF:FF:FF:FF`). The struct is sent verbatim — `sizeof(BikeFrame)`, well under ESP-NOW's 250-byte payload cap. Both SPEED and TRAINER ESPs receive every broadcast.
- WiFi initialised in STA mode (no actual WiFi connection) before `esp_now_init`. Setup happens after BLE init; both share the 2.4 GHz radio without conflict at this duty cycle.
- POWER ESP only sends. SPEED and TRAINER ESPs only receive — their callbacks push the struct directly into each local publish queue.

### App-side BLE peripheral (role-gated)

| | POWER role | SPEED role | TRAINER role |
|---|---|---|---|
| Service | CPS (`0x1818`) | CSC (`0x1816`) | FTMS (`0x1826`) |
| Name | `Yesoul_PWR` | `Yesoul_SPD` | `Yesoul_FTMS` |
| Appearance | `0x0484` (Cycling Power Sensor) | `0x0482` (Cycling Speed Sensor) | `0x0480` (Cycling, generic) |
| Feature bitmap | `0x00000008` (crank-rev) | `0x0001` (wheel-rev) | `0x00005286` (cadence + distance + resistance + energy + elapsed + power) |
| Measurement frame | 8 bytes, flags `0x0020` | 7 bytes, flags `0x01` | 22 bytes, flags `0x09F4` (mirrors the Yesoul's payload exactly) |
| Layout | `[flags][power_s16][crank_revs_u16][last_crank_event_time_u16]` | `[flags][cumulative_wheel_revs_u32][last_wheel_event_time_u16]` | `[flags][speed][cadence][distance_u24][resistance][inst_power][avg_power][total_energy][energy/hr][energy/min][elapsed]` |
| Event-time units | 1/1024 s | 1/1024 s | n/a |
| Control Point | — | SC Control Point (no-op) | Fitness Machine Control Point (no-op, ACKs Request Control + Reset + Start/Resume + Stop/Pause + Set Target Power + Set Indoor Bike Simulation Parameters — Zwift's required sequence) |
| Bike-side BLE central | yes | no — ESP-NOW receiver only | yes (with ESP-NOW receiver as fallback) |

- Bonding intentionally OFF (`setSecurityAuth` not called). Enabling it caused stale-bond mismatches when the watch removed a sensor on its side.
- Each notification publishes only when at least one BLE client is connected on that ESP.
- SPEED ESP hosts SC Control Point (0x2A55) so Garmin watches enumerate it under "Add Sensor → Speed".
- TRAINER ESP hosts Fitness Machine Control Point (0x2AD9) so Zwift considers it "controllable" — the Yesoul has a manual resistance knob so we can't actually act on commands, but ACKing them is the contract for being recognised as a smart trainer.

## Pairing flow

### On the Garmin watch (Power + Speed sensors)

1. Settings → Sensors & Accessories → Add Sensor → **Power** → search → pair `Yesoul_PWR`.
2. Add Sensor → **Speed** → search → pair `Yesoul_SPD`. Set wheel size to **2000 mm** in that sensor's details (matches `WHEEL_CIRCUMFERENCE_M` in firmware).
3. Start a Bike Indoor activity. Power, cadence, speed, distance all record.

### On Zwift / TrainerRoad / similar (smart trainer)

1. Open the app's Devices/Pair screen on the PC/tablet/phone.
2. Search for a smart trainer / FTMS device → `Yesoul_FTMS` should appear.
3. Pair as a controllable trainer. Power, cadence, speed, distance, **resistance**, energy and elapsed time all flow.
4. The Yesoul has a manual resistance knob, so simulated gradient changes from the app are silently ignored — but the app still treats the device as a smart trainer, which is what unlocks structured workouts and proper ride recording.

## Tunables

In [`src/main.cpp`](../src/main.cpp):

- `POWER_SCALE` (default `1.28f`) — empirical correction; the Yesoul under-reports vs. a calibrated reference meter. Inherited from the upstream project. To recalibrate, ride at known watts on a reference meter and divide reference / raw.
- `WHEEL_CIRCUMFERENCE_M` (default `2.000f`) — used to synthesize wheel revolutions from speed. The watch must be configured with the same value (2000 mm) in the speed sensor's details.
- `SIMULATE_BIKE` (default `false`) — flip to `true` to bench-test pairing without pedaling. The POWER ESP injects constant fake values (20 km/h, 60 RPM, 150 W) instead of connecting to the bike.

## What was deliberately NOT shipped

- **NimBLE bonding** — caused stale-bond pairing failures during dev. Off.
- **Random-static BLE address** — public address (default) works fine once the dual-ESP architecture is in place; not needed.
- **NVS-pinned target MAC** — over-engineering for a single-bike, single-user device. Plain FTMS service UUID match is sufficient.
- **FTMS peripheral exposed to the watch** — investigated, ruled out for epix 2 in 2026 firmware. See [JOURNEY.md](JOURNEY.md).
- **Battery / Device Info services** — investigated, not needed; the watch enumerates the device correctly without them.
- **Bike-side `updateConnParams`** — the Yesoul rejects mid-session connection-parameter changes with disconnect reason `0x13`.
