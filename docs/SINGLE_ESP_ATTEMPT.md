# Single-ESP attempt — blocked by hardware

**TL;DR:** This branch attempts to collapse the dual-ESP architecture from `master` onto one chip using BLE 5.0 extended advertising with two advertising instances at distinct random-static addresses. **It does not compile on ESP32-WROOM-32 because the chip's BLE radio is 4.2 only.** It would compile on ESP32-S3, ESP32-C3, ESP32-C6, or ESP32-H2 — but no one has actually tested those with this code yet.

## What was tried

`src/main.cpp` on this branch:
- Both CPS and CSC services live in one GATT server.
- `NimBLEExtAdvertising` is set up with two instances (instance 0 and 1).
- Each instance has its own random-static address (`F1:0A:5E:00:00:01` for power, `F1:0A:5E:00:00:02` for speed), name (`Yesoul_PWR` / `Yesoul_SPD`), Appearance (`0x0484` / `0x0482`), and a single advertised service UUID (`0x1818` for power, `0x1816` for speed).
- The watch's "Add Sensor → Power" scan would see only the address advertising `0x1818`; "Add Sensor → Speed" would see only the address advertising `0x1816`. Two pairings, one chip.
- ESP-NOW relay is gone — both publish paths run from the same FreeRTOS app task.

`platformio.ini` adds `-DCONFIG_BT_NIMBLE_EXT_ADV=1`.

## Why it doesn't build on ESP32-WROOM-32

```
.pio/libdeps/single/NimBLE-Arduino/src/nimconfig.h:370:6:
error: Extended advertising is not supported on ESP32.
```

NimBLE-Arduino guards the extended-advertising API behind `#error` for `CONFIG_IDF_TARGET_ESP32`. The original ESP32 (released 2016) implements BLE 4.2, which has only legacy advertising — one advertising set, one address per device. The BLE 5.0 features (extended advertising, periodic advertising, 2M PHY, coded PHY, multiple advertising sets) require silicon support introduced in:

- ESP32-S3 (BLE 5.0, no WiFi 6) — drop-in physical replacement for many WROOM dev boards.
- ESP32-C3 (BLE 5.0, RISC-V, smaller).
- ESP32-C6 (BLE 5.3 + Thread + Zigbee + WiFi 6).
- ESP32-H2 (BLE 5.3 + Thread + Zigbee, no WiFi).

## To resume this work

1. Get an ESP32-S3 / C3 / C6 / H2 dev board.
2. Update `[env:single]` in `platformio.ini`:
   - `board = esp32-s3-devkitc-1` (or appropriate target)
   - keep the existing `-DCONFIG_BT_NIMBLE_EXT_ADV=1`
3. Build and flash:
   ```
   pio run -e single -t upload --upload-port /dev/cu.usbmodem-XXX
   ```
4. Pair under both Power and Speed on the watch. The interesting unknown — which is what made me start the experiment — is whether the watch trips on discovering both services in GATT *after* having pre-filtered on a single-UUID advertisement. The dual-ESP test on `master` passed because each ESP's GATT contains only one of the two services. With single-chip multi-advertising, GATT is shared; the watch sees both. It might be fine (the filter is at scan-time) or it might break something.

## What the branch leaves you with

- Working `src/main.cpp` for the single-ESP topology — won't compile on WROOM-32, will compile on S3/C3/C6/H2 once `board=` is updated.
- The dual-ESP build on `master` is the working production path for ESP32 classic hardware; this branch doesn't change that.
- If anyone makes single-ESP work on the right hardware, it's worth folding back into master and retiring the ESP-NOW relay.
