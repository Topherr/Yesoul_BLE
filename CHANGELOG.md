# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This fork has not yet cut a numbered release; entries below the
[Unreleased](#unreleased) section are anchored to the date and commit where the
work landed. Pre-fork upstream tags are listed under
[Upstream history](#upstream-history).

## [Unreleased]

### Fixed
- **Bike-side reconnect reliability.** The firmware no longer needs a USB
  power-cycle when returning to a powered-off bike. Three real failure modes
  were diagnosed and closed:
  - A use-after-free on `g_pTarget` between the NimBLE host task and the
    loopTask during reconnect — a second advert could `delete g_pTarget`
    while `connect_to_target()` was reading it.
  - `ClientCallbacks::onDisconnect` could yank the loop state machine even
    after it had already moved on to `S_COOLDOWN` / `S_SCANNING`.
  - No safety watchdog covered a wedged NimBLE host task, so any silent hang
    became permanent until physical replug.

### Added
- New `S_CONNECTING` bike-side state, set in `loop()` before the blocking
  `NimBLEClient::connect()`. `ScanCallbacks::onResult` only acts when state
  is `S_SCANNING`, so the new state closes the `g_pTarget` UAF window.
- Stuck-state watchdog: any non-`S_STREAMING` state held > 5 minutes forces
  an `S_DISCONNECTED` transition. Catches scanner-silently-dies, connect
  hang, and cooldown-doesn't-advance — the cases the existing 5 s
  notification watchdog doesn't see.
- ESP-IDF task watchdog (`esp_task_wdt`), 60 s timeout, panic-on-expire,
  with explicit `esp_task_wdt_add(NULL)` to subscribe the loopTask
  (Arduino-ESP32 3.x does NOT auto-subscribe — verified against
  `loopTaskWDTEnabled = false` in the core source). Idle tasks across all
  cores also subscribed. If `loop()` ever wedges, the chip self-reboots.
- Explicit `NimBLEClient::setConnectTimeout(10000)` (milliseconds in
  NimBLE-Arduino 2.x — was seconds in 1.x). Bounds the connect-blocking
  window well under the WDT timeout.
- `start_scan()` logs the last disconnect reason on the next scan, so
  overnight logs correlate failure cause with recovery.

### Changed
- `ClientCallbacks::onDisconnect` now only transitions to `S_DISCONNECTED`
  when state is `S_CONNECTING` / `S_CONNECTED` / `S_STREAMING`. Late
  supervision-timeout disconnects can no longer disturb the state machine
  once recovery is in progress.
- The `g_doConnect` success path is now guarded: it only promotes to
  `S_CONNECTED` if state is still `S_CONNECTING`. If the host task already
  moved us to `S_DISCONNECTED` during the blocking `connect()`, the existing
  disconnect-cleanup path runs instead of being skipped.

## 2026-05-04 — single-board TRAINER (Option A)

### Added
- TRAINER role now exposes CPS alongside FTMS on the same chip, so a Garmin
  watch can pair `Yesoul_FTMS` as a power meter while Zwift simultaneously
  pairs it as a smart trainer. Collapses Option A to a single board.
  ([9fdd4ee](../../commit/9fdd4ee))
- Standalone C6 mode and public-release documentation.
  ([d9c7ec0](../../commit/d9c7ec0))

### Changed
- `POWER_SCALE` default reverted to `1.00` (raw watts from the bike). The
  upstream `1.28` empirical calibration is preserved as a comment for
  reference; recalibrate per-bike against a reference power meter if accuracy
  matters for training.
- README and `docs/ARCHITECTURE.md` / `docs/JOURNEY.md` rewritten for public
  release.

## 2026-05-01 — TRAINER role

### Added
- TRAINER role: FTMS smart trainer for Zwift, TrainerRoad, MyWhoosh, Rouvy,
  Wahoo SYSTM. Full payload — power, cadence, speed, distance, resistance,
  energy, elapsed time. Includes Fitness Machine Control Point no-op handler
  that ACKs Zwift's documented start sequence (Request Control → Reset →
  Start → Set Indoor Bike Simulation Parameters). Manual resistance knob —
  the firmware ACKs gradient/target-power writes but the rider drives
  resistance physically. ([5eab89c](../../commit/5eab89c))

## 2026-04-30 — dual-ESP + ESP-NOW relay (Garmin watch full payload)

Full rewrite against NimBLE-Arduino 2.x; the upstream's single-file, single-ESP
CPS bridge is replaced by a multi-role architecture. ([0b94e8e](../../commit/0b94e8e))

### Added
- `DEVICE_ROLE_POWER` and `DEVICE_ROLE_SPEED` build flags. POWER owns the
  bike's BLE central slot (the Yesoul allows only one); SPEED receives
  parsed `BikeFrame`s via ESP-NOW relay and publishes CSC. Garmin watch
  now records power + cadence + speed + distance natively in a Bike Indoor
  activity.
- Explicit BLE state machine (`S_SCANNING` / `S_CONNECTED` / `S_STREAMING`
  / `S_DISCONNECTED` / `S_COOLDOWN`) with a 5 s notification watchdog and
  exponential scan backoff (5 s → 60 s).
- ESP-NOW broadcast of parsed `BikeFrame` structs (POWER → SPEED).
- Host-side parser unit test (`test/test_parser/`) — 45/45 frames PASS
  against a captured `.log` from a real Yesoul G1M Plus session.

### Changed
- Built against NimBLE-Arduino 2.x.
- Memory hygiene: `NimBLEDevice::deleteClient()` is called on every
  reconnect cycle (the upstream leaked a `BLEClient` per cycle).

---

## Upstream history

This project is a fork of [Raelx/Yesoul_BLE](https://github.com/Raelx/Yesoul_BLE)
by **Raelx** and **Jeremy Mikesell**. The tags below were cut on the upstream
repository and are preserved here as historical anchors; this fork has not
re-issued them.

### [v0.2.1] — 2025-05-19
- Compile fix. ([4a643a9](../../commit/4a643a9))

### [v0.2.0] — 2022-02-22
- `POWER_SCALE = 1.28` — empirical power calibration vs. a reference meter.
  ([414aad8](../../commit/414aad8))

### [v0.1.0] — 2022-02-03
- Initial CPS bridge: a single ESP32 subscribes to the bike's FTMS Indoor
  Bike Data and republishes power + cadence as a Cycling Power Service
  peripheral. Garmin Edge / head-unit only; watches blocked by Fenix-class
  firmware filter (see [`docs/JOURNEY.md`](docs/JOURNEY.md)).
  ([b5ada40](../../commit/b5ada40))
