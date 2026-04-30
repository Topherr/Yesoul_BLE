# Protocol notes

How the Yesoul speaks FTMS, how we parse it, and what we re-emit.

## Bike → bridge: FTMS Indoor Bike Data (0x2AD2)

The Yesoul advertises the standard Bluetooth Fitness Machine Service (`0x1826`) and notifies on the Indoor Bike Data characteristic at ~1 Hz. Frames are **22 bytes** with a constant flags value of **`0x09F4`**.

### Flag bits → fields present

`0x09F4 = 0b 0000 1001 1111 0100`

| Bit | Set? | Field | Notes |
|-----|------|-------|-------|
| 0 | 0 | Instantaneous Speed | Bit-0 is **inverted**: clear means speed *is* present |
| 1 | 0 | Average Speed | absent |
| 2 | 1 | Instantaneous Cadence | |
| 3 | 0 | Average Cadence | absent |
| 4 | 1 | Total Distance | |
| 5 | 1 | Resistance Level | |
| 6 | 1 | Instantaneous Power | |
| 7 | 1 | Average Power | |
| 8 | 1 | Total Energy + per-hour + per-minute | |
| 9 | 0 | Heart Rate | absent (bike has no HRM input) |
| 10 | 0 | Metabolic Equivalent | absent |
| 11 | 1 | Elapsed Time | |
| 12 | 0 | Remaining Time | absent |

### Byte layout

| Offset | Bytes | Field | Type / units | Example (t=20s frame) |
|--------|------:|-------|--------------|-----------------------|
| 0–1 | 2 | Flags | uint16 LE | `0x09F4` |
| 2–3 | 2 | Instantaneous Speed | uint16 LE, 0.01 km/h | `0x04E5` = 12.53 km/h |
| 4–5 | 2 | Instantaneous Cadence | uint16 LE, 0.5 RPM | `0x0042` = 33 RPM |
| 6–8 | 3 | Total Distance | uint24 LE, m | `0x000003` = 3 m |
| 9–10 | 2 | Resistance Level | sint16 LE | `0x0021` = 33 |
| 11–12 | 2 | Instantaneous Power | sint16 LE, W | `0x0012` = 18 W |
| 13–14 | 2 | Average Power | sint16 LE, W | `0x0006` = 6 W |
| 15–16 | 2 | Total Energy | uint16 LE, kcal | `0x0000` |
| 17–18 | 2 | Energy Per Hour | uint16 LE, kcal/hr | `0xFFFF` (= "data not available" per spec) |
| 19 | 1 | Energy Per Minute | uint8, kcal/min | `0xFF` (= "data not available") |
| 20–21 | 2 | Elapsed Time | uint16 LE, s | `0x0003` = 3 s |

The `0xFF` / `0xFFFF` sentinels in the energy fields are spec-correct ("Data Not Available"). The bike sets the flag bit but doesn't compute kcal/hr or kcal/min.

## Parser

The parser ([`src/ftms_parser.cpp`](../src/ftms_parser.cpp)) walks the buffer field-by-field driven by the flags — it doesn't hard-code offsets — so it tolerates any flag combination the bike might emit, including future firmware that toggles fields on or off.

```cpp
struct BikeFrame {
    uint32_t millis_received;
    uint16_t flags;
    uint16_t speed_cmps;        // 0.01 km/h, raw from spec
    uint16_t cadence_halfrpm;   // 0.5 RPM, raw from spec
    uint32_t distance_m;
    int16_t  resistance;
    int16_t  inst_power_w;
    int16_t  avg_power_w;
    uint16_t total_energy_kcal;
    uint16_t elapsed_s;

    bool has_speed, has_cadence, has_distance, has_resistance,
         has_inst_power, has_avg_power, has_energy, has_elapsed;
};

bool parse_indoor_bike_data(const uint8_t* buf, size_t len, BikeFrame& out);
```

Returns false on truncated input. Each `has_*` flag indicates whether the bike actually sent the corresponding field on this notification.

## Captures

Ground-truth samples live in [`docs/captures/`](captures/). Each line is timestamped (relative seconds since capture start) and contains the raw FTMS bytes prefixed by a length and flags decode:

```
[ 20.09] RAW[22] flags=0x09F4 bytes: F4 09 E5 04 42 00 03 00 00 21 00 12 00 06 00 00 00 FF FF FF 03 00
```

The current capture file is [`docs/captures/2026-04-30-yesoul-g1m-plus.log`](captures/2026-04-30-yesoul-g1m-plus.log) — 45 frames covering idle, light pedaling, harder pedaling, and a resistance ramp. New captures with `recorder` build flags can be appended to drive parser regression tests.

## Host-side parser test

```bash
c++ -std=gnu++17 -I src src/ftms_parser.cpp test/test_parser/test_main.cpp -o /tmp/test_parser
/tmp/test_parser
```

Expected output:

```
frames=45 legacy_match=45 legacy_mismatch=0 max_speed=14.05kmh max_power=67W max_res=100
PASS
```

The harness replays every line in the capture log, runs it through `parse_indoor_bike_data`, and asserts that each frame:

- Decodes successfully.
- Exposes every field the flags claim are present.
- Agrees with the upstream project's hardcoded-offset parser on cadence, resistance, and instantaneous power (the three fields the upstream parser supported). All 45 frames match — the upstream offsets happened to land on the spec offsets.

## Bridge → watch: re-emitting the data

### POWER ESP — Cycling Power Service (0x1818)

8-byte CPS Measurement notification, flags `0x0020` (crank-rev present, no wheel-rev):

| Offset | Bytes | Field | Notes |
|--------|------:|-------|-------|
| 0–1 | 2 | Flags | `0x0020` |
| 2–3 | 2 | Instantaneous Power | sint16 LE, watts. `BikeFrame.inst_power_w * POWER_SCALE` |
| 4–5 | 2 | Cumulative Crank Revolutions | uint16 LE, accumulated from cadence |
| 6–7 | 2 | Last Crank Event Time | uint16 LE, **1/1024 s** units |

CPS Feature characteristic (0x2A65): `0x00000008` (Crank Revolution Data Supported, bit 3) — wheel-rev support deliberately not declared.

### SPEED ESP — Cycling Speed and Cadence Service (0x1816)

7-byte CSC Measurement notification, flags `0x01` (wheel-rev present, no crank-rev):

| Offset | Bytes | Field | Notes |
|--------|------:|-------|-------|
| 0 | 1 | Flags | `0x01` |
| 1–4 | 4 | Cumulative Wheel Revolutions | uint32 LE, derived from `BikeFrame.speed_cmps` and `WHEEL_CIRCUMFERENCE_M` |
| 5–6 | 2 | Last Wheel Event Time | uint16 LE, **1/1024 s** units |

CSC Feature characteristic (0x2A5C): `0x0001` (Wheel Revolution Data Supported, bit 0) — crank-rev support deliberately not declared.

The CSC service also exposes Sensor Location (`0x2A5D`, value `0x0D` = Rear Hub) and SC Control Point (`0x2A55`) with a no-op handler that ACKs `Set Cumulative Value` op codes and returns `Op Code Not Supported` for everything else. Without the SC Control Point characteristic Garmin watches refuse to enumerate the device under Add Sensor → Speed.

### Wheel-revolution synthesis

```
speed_mps          = speed_cmps * 0.01 / 3.6
wheel_revs_delta   = speed_mps * dt_s / WHEEL_CIRCUMFERENCE_M
cumulative_wheel  += wheel_revs_delta   (float, then floor to uint32 for the wire)
last_wheel_event_time = (millis() * 1024 / 1000) % 65536
```

The watch computes speed from `Δrevs / Δevent_time`. Whatever wheel circumference is configured on the watch is what gets multiplied by the rev count for distance display — that's why the README asks the user to set it to 2000 mm to match the firmware constant. If they set a different value, distance and speed will both scale proportionally.

## Why event-time units differ between CPS and CSC

CPS spec uses **1/2048 s** for wheel-rev event time and **1/1024 s** for crank-rev event time. CSC spec uses **1/1024 s** for both. Easy to get wrong; easy to find on a forum thread chasing what looks like a 2× speed display bug. We never emit wheel-rev on CPS, so the only event-time unit that ever appears on the wire is 1/1024 s.
