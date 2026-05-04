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

### 9. Try the single-ESP path on a BLE-5.0 chip — Seeed XIAO ESP32-C6

After getting Option C working on three WROOM-32s, the obvious follow-up: can a single ESP32 with BLE 5.0 extended advertising replace the dual-WROOM-32 architecture for the Garmin watch use case? Two advertising instances at distinct random-static addresses, one shared GATT, watch sees them as two devices.

Bought a Seeed XIAO ESP32-C6, set up the [`single-esp-experiment`](https://github.com/Topherr/Yesoul_BLE/tree/single-esp-experiment) branch, flashed it. Verified with nRF Connect on iPhone that both BLE addresses are advertised, both pair, and **iPhone Zwift holds two simultaneous connections to the chip without trouble** (`conns=2` confirmed on serial). So the firmware and chip are fine.

**The Garmin epix 2 only ever holds one connection at a time to the chip**, regardless of:
- distinct random-static addresses (tried `F1:0A:5E:00:00:01`/`02` initially, then completely independent OUIs `C1:11:22:33:44:55` and `E2:66:77:88:99:AA`)
- advertising-restart on disconnect
- Both pair successfully — so it's not a discovery bug. It's a connection-manager limit.

This matches [PeloMon issue #1](https://github.com/ihaque/pelomon/issues/1) on a Fenix 5 from 2021 — same architecture, same symptom, same conclusion. Garmin's [Connect IQ docs explicitly state](https://forums.garmin.com/developer/connect-iq/f/discussion/282112/ble-notification-not-appearing-for-ftms-ble) "Connect IQ apps cannot support multiple simultaneous device connections" for CSC/CPS — the watch firmware is single-active-connection-per-paired-peripheral, where "peripheral" is keyed at a level above BLE addresses. Garmin's own dual-protocol sensors (Vector 3, HRM-Dual, Speed/Cadence Gen 3) are the explicit special case.

**Verdict:** single-ESP-on-Garmin-watch is fundamentally blocked by Garmin firmware policy. Not a code or hardware bug. Documented in [`docs/SINGLE_ESP_ATTEMPT.md`](SINGLE_ESP_ATTEMPT.md) and the branch is preserved as a public dead-end record.

### 10. Repurpose the C6 as the standalone Zwift trainer

If the C6 can't replace dual-WROOM-32 for the watch, what *can* it do? Turns out: **everything** for the Zwift use case in a single board. Flash the `trainer_c6` env, and the C6:

- Connects to the bike directly as BLE central (the TRAINER role's bike-side scan is enabled when not running alongside a POWER ESP)
- Parses FTMS Indoor Bike Data
- Re-emits as FTMS smart trainer to Zwift / TR / etc. with `POWER_SCALE` applied to the inst-power field
- Implements Fitness Machine Control Point with a no-op handler that ACKs Zwift's documented start sequence (Request Control → Reset → Start → Set Indoor Bike Simulation), so the trainer is recognised as "controllable" even though we can't act on the commands

Tested with iPhone Zwift on 2026-05-01. All four metrics (power, cadence, speed, distance) plus resistance flow live, no dropouts.

If the C6 is plugged in alongside the dual-WROOM-32 watch setup, the bike's one-central limit kicks in: POWER scans first, wins the bike, and the C6's bike-side scan fails to connect → falls back automatically to receiving over ESP-NOW (the recv callback is registered for any role that isn't POWER). All three deployment options now work from one source tree.

## What I didn't try

- **Per-connection GATT subset on a single chip.** The single-ESP path could theoretically work if NimBLE supported showing CPS to one address's connections and CSC to the other's — Garmin's connection-manager policy might be sidestepped if each address looked like a structurally distinct device end-to-end. NimBLE 2.x's `ble_gatts_svc_set_visibility` is global, not per-connection. Forking mynewt-nimble to add per-connection visibility is a rewrite, not a tweak. Probably not worth the effort given the dual-WROOM-32 path works.
- **Servo-on-resistance-knob.** Closing the loop on Zwift's "smart trainer" gradient/target-power commands by physically rotating the bike's manual resistance knob with a small servo. Conceptually doable in ~30 LOC + ~$15 hardware. Out of scope for the current fork; would belong in a sibling project.
- **Battery / Device Information services.** Real BLE sensors expose them. We don't, and Garmin / Zwift / iPhone all enumerate fine without.
- **Connect IQ data field.** [FTMS All Sync](https://absolutebollockscreations.com/apps/ftmsall/) and similar can read the bike's FTMS directly from a Garmin watch with no bridge ESP at all — but only into custom Connect IQ data fields, not the watch's native activity-record fields. Different deployment model. Out of scope.

## Useful tooling

- **[nRF Connect](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Mobile)** on iPhone — invaluable for confirming what GATT structure the peripheral actually exposes vs. what we think we're publishing. Should be the *first* tool reached for during BLE pairing debugging, not the last.
- **`python3 + pyserial`** for serial capture, much cleaner than the bundled `pio device monitor` when running headless. The capture script in [`docs/captures/`](captures/) is just `pyserial.read_until(b'\n')` in a loop.
- **Subagent reviewers** — most of the architectural decisions in this fork were stress-tested by independent code-review and protocol-spec agents before commit. If you're iterating on something subtle (the watch single-peripheral limit, the FTMS Control Point op-code requirements for Zwift, the wheel-event-time units between CPS and CSC), getting a second opinion that hasn't seen your existing reasoning is genuinely valuable.

## Heritage and inheritance

The **1.28 power-scale calibration constant** is the upstream's. Looking at the original commit (`414aad8 Scaling for Power`, Feb 2022) the entire diff was the constant plus a one-line comment "incoming power is multiplied by this value for correction" — no methodology documentation in the repo. We've shipped `1.0` (raw watts) by default in this fork because there's no reason to trust 1.28 specifically over 1.0 for a bike model the upstream didn't test (they had a Yesoul S3; we tested with G1M Plus). If you have a calibrated reference power meter, derive your own scale and PR it back.

The **NimBLE callback class structure** (`ServerCallbacks`, `CharacteristicCallbacks`, `MyClientCallback`, `MyAdvertisedDeviceCallbacks`) — these were lifted from the NimBLE-Arduino example boilerplate but the upstream's framing of which callbacks to actually use, and how to structure the `connect → discover → subscribe` flow, was a clean template. Everything in this fork's bike-side state machine is descended from that.

The **CPS frame layout** (flags `0x0020`, crank-rev only, 8 bytes) is theirs verbatim. We added CSC and FTMS service emission on top.

Everything else (parser library, dual-ESP architecture, ESP-NOW relay, state machine, NimBLE 2.x port, host-side tests, multi-deployment-option build envs, single-ESP experiment, all docs) is rewritten from scratch.

Everything else (parser, dual-ESP architecture, ESP-NOW relay, state machine, NimBLE 2.x port, host-side test harness, docs) is rewritten from scratch.

## Sources cited along the way

- [PeloMon Part IV — combined CPS+CSC on Garmin Venu](https://ihaque.org/posts/2021/01/04/pelomon-part-iv-software/)
- [Bluetooth SIG Cycling Power Service 1.1](https://www.bluetooth.com/specifications/specs/cycling-power-service-1-1/)
- [Bluetooth SIG Cycling Speed and Cadence Service 1.0](https://www.bluetooth.com/specifications/specs/cycling-speed-and-cadence-service-1-0/)
- [Bluetooth SIG Fitness Machine Service 1.0](https://www.bluetooth.com/specifications/specs/fitness-machine-service-1-0/)
- [Garmin Speed Sensor 2 manual — separate physical device](https://www8.garmin.com/manuals/webhelp/cadencespeedsensors2/EN-US/GUID-422A313B-3B65-4AFD-9CFB-8A5E4CA02D95.html)
- [TSDZ2-ESP32 PR #1 — confirmed working CSC+CPS on older Vivoactive 3](https://github.com/TSDZ2-ESP32/TSDZ2-ESP32-Main/pull/1)
- [NimBLE-Arduino 1.x → 2.x migration guide](https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/1.x_to2.x_migration_guide.md)
