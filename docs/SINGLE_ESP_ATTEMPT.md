# Single-ESP attempt — pending hardware (XIAO ESP32-C6)

**Status:** Code is ready. Awaiting hardware delivery (Seeed XIAO ESP32-C6) for the bench test that answers the only remaining open question — whether the Garmin epix 2 trips on a shared GATT containing both CPS and CSC after pairing each via a single-UUID advertisement instance.

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
