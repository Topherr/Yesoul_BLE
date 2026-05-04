# Single-ESP attempt — tested on XIAO ESP32-C6, **blocked at the Garmin watch** (2026-05-01)

**Status: dead end for the Garmin watch use case. Works fine on iPhone Zwift.** The single-ESP path is not a drop-in replacement for `master`'s dual-WROOM-32 architecture; keep dual-ESP for Garmin watch use, use this build (or a single-service variant) for iPhone / Zwift / TrainerRoad / etc.

## TL;DR of the test outcome

- ✅ Code compiles and flashes cleanly on XIAO ESP32-C6 with pioarduino + Arduino-ESP32 3.3.8 + NimBLE-Arduino 2.5.0.
- ✅ Both BLE 5.0 extended-advertising instances are visible to scanners as two separate devices with distinct random-static addresses.
- ✅ Both pair successfully on a Garmin epix 2 — Yesoul_PWR under "Add Sensor → Power", Yesoul_SPD under "Add Sensor → Speed".
- ✅ **iPhone running Zwift** holds **two simultaneous BLE connections** to the C6 (`conns=2` confirmed via serial), reads CPS power+cadence and CSC speed concurrently. No issues.
- ❌ **Garmin epix 2** in an active Bike Indoor session holds only **one** BLE connection at a time (`conns=1`). Power streams OR speed streams, never both — even though both are paired.
- ❌ Tried structurally distinct random-static addresses (no shared OUI / no shared bytes): no change.
- ❌ Tried the advertising-restart-on-connect/disconnect fix: no change for the watch (helped iPhone hold both, but watch still caps at one).

## Why the watch is fundamentally blocked

Garmin's watch firmware treats one physical peripheral as one sensor slot, regardless of how many BLE addresses it advertises. Convergent evidence:

- **PeloMon issue #1** documents the same exact symptom on a Fenix5: <https://github.com/ihaque/pelomon/issues/1>. PeloMon advertises one BLE peripheral with both CPS and CSC, gets the same one-active-connection-at-a-time behavior. Never fixed.
- **Garmin Connect IQ docs explicitly state**: "Connect IQ apps cannot support multiple simultaneous device connections, so you can connect to any CSC or CPS capable sensor, but only one device at a time." (<https://forums.garmin.com/developer/connect-iq/f/discussion/282112/ble-notification-not-appearing-for-ftms-ble>) The central-side stack is single-active-connection-per-device, where "device" is keyed at a layer above BLE addresses.
- **Garmin's own dual-protocol sensors** (Vector 3, HRM-Dual, Speed/Cadence Gen 3) are explicitly the special case that *does* support two concurrent BLE connections — and that's described as a property of those *sensors*, not of the watch. Generic peripherals don't get that treatment.
- **`master`'s dual-WROOM-32 build works precisely because Garmin sees two physically distinct radios** with two distinct GAP roles + GATT databases. The address randomness is not the discriminator.
- **NimBLE 2.x has no per-advertising-set GATT subset**: `ble_gatts_svc_set_visibility` is global. Forking mynewt-nimble to add per-conn service visibility is a rewrite, not a fix.

The iPhone Zwift evidence proves the C6 hardware and our code are fine. **It's a Garmin firmware policy choice, not a code or hardware bug.** Chasing it further is wasted effort.

## What this branch leaves you with

A working iPhone-Zwift-style "headless smart sensor" that exposes CPS + CSC simultaneously over multi-advertising on a single chip. Useful if you ever want to ride with Zwift on iPhone without the watch involved, or for any non-Garmin-watch BLE consumer that handles multiple connections to one peripheral cleanly. Not useful for the Garmin epix 2 watch use case.

## What replaces it for the Garmin watch

`master`'s dual-WROOM-32 + ESP-NOW relay. Already shipped, already working.

## What to do with the C6 in hand

Best repurposing: flash the C6 with `feature/ftms-trainer`'s `[env:trainer]` build. The FTMS smart-trainer role only needs one advertised service (no multi-instance ext-adv required), so it works on any ESP32 family chip — and the C6's on-board antenna, USB-C, and small form factor are nicer for an "always-on Zwift trainer" device than the WROOM-32. See `feature/ftms-trainer`'s docs.

## Why this branch exists

`master` ships a dual-ESP-with-ESP-NOW-relay architecture. It works, but uses two physical boards because the original ESP32 (WROOM-32) BLE radio is 4.2 — only one advertising address per device, so the only way to look like two sensors to the watch is to *be* two devices.

This branch attempts the cleaner topology: one chip, two BLE 5.0 extended-advertising instances at distinct random-static addresses, one shared GATT containing both CPS and CSC services.

## What's in `src/main.cpp` on this branch

- Both CPS and CSC services live in the same GATT server (one ESP, both roles).
- `NimBLEExtAdvertising` is configured with two instances:
  - Instance 0: address `F1:0A:5E:00:00:01`, name `Yesoul_PWR`, Appearance `0x0484`, advertises only `0x1818` (CPS).
  - Instance 1: address `F1:0A:5E:00:00:02`, name `Yesoul_SPD`, Appearance `0x0482`, advertises only `0x1816` (CSC).
- The watch's "Add Sensor → Power" scan should see only address 1 (only CPS UUID in adv).
- The watch's "Add Sensor → Speed" scan should see only address 2 (only CSC UUID in adv).
- After pairing, the watch connects to whichever address it picked. GATT discovery reveals both services. **The open question: does the watch get confused, or does it consume only the role it paired for?**
- ESP-NOW relay code is removed. Both publish paths (CPS and CSC) run from the same FreeRTOS app task on the same chip with shared `BikeFrame` state.

## Hardware required

**Seeed XIAO ESP32-C6** (or any ESP32-C3/S3/C6/H2 board). The original ESP32-WROOM-32 cannot run this code because its BLE silicon doesn't support multiple advertising sets:

```
.pio/libdeps/single/NimBLE-Arduino/src/nimconfig.h:370:
#if defined(CONFIG_IDF_TARGET_ESP32)
#  error Extended advertising is not supported on ESP32.
#endif
```

C6 picked because: BLE 5.3, on-board PCB antenna (no clip-on antenna step needed), U.FL also reserved if you want a bigger one, ~$11 AUD.

## Build and flash (when hardware arrives)

```bash
pio run -e single -t upload --upload-port /dev/cu.usbmodemXXX
pio device monitor --port /dev/cu.usbmodemXXX
```

The `[env:single]` build env in `platformio.ini` is already configured for C6:

```ini
platform    = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board       = seeed_xiao_esp32c6
lib_deps    = h2zero/NimBLE-Arduino@^2.5
build_flags = -DCORE_DEBUG_LEVEL=0 -DCONFIG_BT_NIMBLE_EXT_ADV=1
```

Note the non-stock `platform =` — the pioarduino fork ships an Arduino-ESP32 3.x core that supports the C6. PlatformIO's official `espressif32` platform is too old at time of writing. The URL above pins to the fork's "stable" release rather than the floating default branch, so a later regression upstream won't silently break our build.

## Pairing test on the watch

1. Settings → Sensors → Remove `Yesoul_PWR` and `Yesoul_SPD` if present.
2. **Power-cycle the watch** (full reboot).
3. Add Sensor → **Power** → search → expect `Yesoul_PWR` to appear → pair.
4. Add Sensor → **Speed** → search → expect `Yesoul_SPD` to appear → pair. Set wheel size to 2000 mm.
5. Start a Bike Indoor activity. Pedal.

### Three possible outcomes

- **Both pair, all four metrics flow** → single-ESP wins. Retire dual-ESP from master, fold this branch back, declare done.
- **Both pair, only one role's data shows** → the watch is consuming only one service. Investigate via nRF Connect on iPhone whether both subscriptions are active and notifications are being sent.
- **One or both don't appear in the search** → shared GATT triggers a watch-side filter we haven't yet seen. Likely fix: rotate advertising sets so only one is broadcasting at a time during pairing, or split the GATT serve responses per advertising-set address (advanced, may need NimBLE host hooks).

## What this branch does NOT change on master

The dual-ESP build on `master` remains the working production path. This branch can be cherry-picked, merged, or abandoned without disturbing it. If the test fails, master is the answer. If it succeeds, master's `docs/JOURNEY.md` and `docs/ARCHITECTURE.md` get updated and the dual-ESP rig becomes spare hardware.

## Sources confirming compatibility

- [NimBLE-Arduino 2.5.0 README](https://github.com/h2zero/NimBLE-Arduino) — lists ESP32-C6 in supported chips.
- [NimBLE-Arduino CHANGELOG](https://github.com/h2zero/NimBLE-Arduino/blob/master/CHANGELOG.md) — C6 added in 2.3.0 (2025-05-19).
- [NimBLE-Arduino CI workflow](https://github.com/h2zero/NimBLE-Arduino/blob/master/.github/workflows/build.yml) — builds the BLE 5.0 extended-advertising example on `esp32-c6-devkitc-1` with `-DCONFIG_BT_NIMBLE_EXT_ADV=1`. Same flag we use.
- [PlatformIO board: seeed_xiao_esp32c6](https://docs.platformio.org/en/latest/boards/espressif32/seeed_xiao_esp32c6.html)
- [pioarduino platform-espressif32 fork](https://github.com/pioarduino/platform-espressif32) — required for C6 Arduino builds at time of writing.
- [Espressif ESP32-C6 product page](https://www.espressif.com/en/products/socs/esp32-c6) — BLE 5.3 with extended advertising support in silicon.
