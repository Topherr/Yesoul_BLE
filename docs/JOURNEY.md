# Journey

The path from "single-ESP CPS-only fork" to "two ESPs with an ESP-NOW relay" took several wrong turns. Documenting them here so the next person retracing this gets to the answer faster — and to make clear what assumptions don't survive contact with current Garmin firmware.

## What the original repo did

The upstream [Raelx/Yesoul_BLE](https://github.com/Raelx/Yesoul_BLE) — a single ESP32 that subscribed to the bike's FTMS Indoor Bike Data and re-broadcast power + cadence as a Cycling Power Service peripheral. Roughly 415 LOC, single source file. Worked great for power and cadence on a Garmin Edge head-unit. Speed was listed as a "Future Goal."

## The dead ends, in order

### 1. Bump speed onto CPS via wheel-rev fields

CPS Measurement supports a "wheel revolution data present" flag (bit 4). In theory, an Edge with a configured wheel circumference reads that field and renders speed.

Tried it. **Garmin watches in the Fenix 7 / epix 2 family don't consume wheel-rev from CPS.** The watch displayed power and cadence from the same frame and ignored the wheel-rev bytes entirely. Multiple Garmin forum reports confirm this is firmware-level behaviour, not a bug.

### 2. Add CSC service alongside CPS, combined wheel + crank

Cycling Speed and Cadence Service (0x1816) with Feature `0x0003` (wheel + crank). Frame layout per spec.

The watch's "Add Sensor → Speed" search **never listed our device.** Connected from nRF Connect on iPhone — confirmed the GATT structure was textbook-correct. Multiple revisions of CSC Feature bitmap, Appearance values, advertising order, full watch power-cycles — none of it changed the result.

### 3. CSC wheel-only

Reviewer suggestion: Garmin watch firmware filters combined-mode CSC out of the Speed sensor enumeration. Try wheel-only Feature (`0x0001`) and a 7-byte wheel-only Measurement frame.

Same result. Watch still didn't list the device under Speed.

### 4. FTMS pass-through

If the watch won't take CSC, expose FTMS to the watch directly. The Yesoul speaks FTMS; we'd just relay it.

Researched and abandoned. Garmin Fenix 7 / epix 2 firmware **does not consume FTMS** as a sensor type in 2026. The watch's "Add Sensor → Indoor Trainer" category is ANT+ FE-C only. Fenix 8 was the first model to add BLE Indoor Trainer support. Sources:

- [Garmin Connect IQ developer forum — BLE FTMS service not recognized with Fenix 7s](https://forums.garmin.com/developer/connect-iq/f/discussion/329965/ble-ftms-service-not-recognized-with-fenix-7s)
- [fēnix 7 Owner's Manual — Indoor Trainer is ANT+ only](https://www8.garmin.com/manuals/webhelp/GUID-C001C335-A8EC-4A41-AB0E-BAC434259F92/EN-US/GUID-5956B2AD-038A-4998-860B-032081F18F61.html)

### 5. The decisive 15-minute test: strip CPS, ship CSC-only

If neither CSC-alongside-CPS nor FTMS works, what if the watch's Speed search is filtering devices that *also* expose CPS? Built a CSC-only firmware and re-paired.

**The watch immediately listed and paired the device under Add Sensor → Speed.** Speed and distance flowed.

Confirmed: Garmin watches in this generation reject any device that exposes both CPS and CSC under the Speed-sensor pairing flow. Garmin's own [Speed Sensor 2](https://www8.garmin.com/manuals/webhelp/cadencespeedsensors2/EN-US/GUID-422A313B-3B65-4AFD-9CFB-8A5E4CA02D95.html) and Cadence Sensor 2 ship as **two separate physical devices** for exactly this reason. There's no spec mandating that, but Garmin's UI is built around "one sensor category per BLE peripheral."

### 6. Two ESPs, one CPS-only and one CSC-only

Same firmware on both, role chosen by build flag. First test: each ESP tried to connect to the bike independently. The Yesoul accepted the first BLE central, refused the second (disconnect reason `0x13` — "Remote User Terminated"). One ESP worked, one was permanently in `SCANNING`.

### 7. ESP-NOW relay

POWER ESP owns the bike connection. After parsing each FTMS frame it broadcasts the `BikeFrame` struct over ESP-NOW (a low-overhead ESP32-to-ESP32 protocol on the same 2.4 GHz radio). SPEED ESP doesn't talk to the bike at all — it just listens for ESP-NOW packets and pushes them into its publish queue.

This works. End to end:

```
bike → POWER ESP (FTMS BLE) → CPS BLE → watch
                            → ESP-NOW → SPEED ESP → CSC BLE → watch
```

Power, cadence, speed, and distance all display on the watch and record into the Bike Indoor activity.

### 8. Add a TRAINER ESP for Zwift / TrainerRoad / etc.

The Garmin watch was always the primary target, but the bike's data is just as relevant for indoor cycling apps. They want a smart trainer over **FTMS** — same protocol the Yesoul speaks natively to us, repackaged.

The dual-ESP architecture extends cleanly: a third ESP listens on the same ESP-NOW broadcast, receives every parsed `BikeFrame`, and re-emits FTMS Indoor Bike Data (0x2AD2) to the app with all eight fields (speed, cadence, distance, resistance, instantaneous power, average power, energy, elapsed time) — flags `0x09F4`, identical to what the Yesoul itself sends to us. We also expose Fitness Machine Control Point (0x2AD9) with a no-op handler that ACKs Request Control / Reset and returns `Op Code Not Supported` for everything else; that's the contract Zwift uses to bucket the device as "controllable".

The Yesoul has a manual resistance knob, so simulated-gradient commands from the app can't physically change the bike. We pretend they did (ACK them) without acting; the rider turns the knob themselves. Functionally it's a "headless smart trainer" — fully recognised, but resistance is operator-driven.

```
bike → POWER ESP (FTMS BLE) → CPS BLE → watch
                            → ESP-NOW → SPEED ESP   → CSC BLE  → watch
                            → ESP-NOW → TRAINER ESP → FTMS BLE → Zwift / TR / etc.
```

Net result: one bike, three ESPs, two consumers (watch + indoor app) running simultaneously off the same ride.

## What I didn't try

- **Single ESP with multi-advertising.** ESP32 supports BLE 5.0 Extended Advertising with multiple advertising sets, each capable of its own random-static address. With careful setup (`NimBLEDevice::setOwnAddrType`, separate advertisement instances, role-gated GATT subset per peer connection), one chip could project two distinct virtual devices to the watch. Theoretically clean. **I had two ESPs in a drawer and gave up trying to make one work.** Worth attempting if you only have one ESP — you'd need to verify that NimBLE 2.x can serve different GATT subsets per advertising-set address, which is the part I'm not sure about.
- **Random-static BLE address.** Some Garmin firmware reportedly prefers random-static over public for fitness sensors. With the dual-ESP architecture, public addresses worked fine; never had to test this.
- **Battery / Device Information services.** Real CSC sensors expose them. We don't, and the watch enumerates fine without.
- **Connect IQ data field.** Could install [FTMS All Sync](https://absolutebollockscreations.com/apps/ftmsall/) on the watch and consume FTMS directly from the bike, no bridge at all. Different deployment model, different tradeoffs (CIQ data fields write to custom screens, not native activity-record fields). Out of scope for this fork.

## Useful tooling

- **[nRF Connect](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Mobile)** on iPhone — invaluable for confirming what GATT structure the peripheral actually exposes vs. what we think we're publishing. Should be the *first* tool reached for during BLE pairing debugging, not the last.
- **`pio device monitor`** with `python3 + pyserial` for capturing — much cleaner than the bundled monitor when running headless. The capture script in [`docs/captures/`](captures/) is just `pyserial.read_until(b'\n')` in a loop.

## Heritage and inheritance

The 1.28 power-scale calibration constant in `src/main.cpp` is empirical work inherited from the upstream project — they measured the Yesoul against a reference power meter and that ratio gets close. Untouched here.

Everything else (parser, dual-ESP architecture, ESP-NOW relay, state machine, NimBLE 2.x port, host-side test harness, docs) is rewritten from scratch.

## Sources cited along the way

- [PeloMon Part IV — combined CPS+CSC on Garmin Venu](https://ihaque.org/posts/2021/01/04/pelomon-part-iv-software/)
- [Bluetooth SIG Cycling Power Service 1.1](https://www.bluetooth.com/specifications/specs/cycling-power-service-1-1/)
- [Bluetooth SIG Cycling Speed and Cadence Service 1.0](https://www.bluetooth.com/specifications/specs/cycling-speed-and-cadence-service-1-0/)
- [Bluetooth SIG Fitness Machine Service 1.0](https://www.bluetooth.com/specifications/specs/fitness-machine-service-1-0/)
- [Garmin Speed Sensor 2 manual — separate physical device](https://www8.garmin.com/manuals/webhelp/cadencespeedsensors2/EN-US/GUID-422A313B-3B65-4AFD-9CFB-8A5E4CA02D95.html)
- [TSDZ2-ESP32 PR #1 — confirmed working CSC+CPS on older Vivoactive 3](https://github.com/TSDZ2-ESP32/TSDZ2-ESP32-Main/pull/1)
- [NimBLE-Arduino 1.x → 2.x migration guide](https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/1.x_to2.x_migration_guide.md)
