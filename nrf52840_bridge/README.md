# nrf52840_bridge — single-board ANT+ bridge

Firmware for a Nordic **nRF52840 USB Dongle (PCA10059)** that bridges a Yesoul
indoor bike to a Garmin epix 2 (and any other ANT+ consumer).

The chip exposes **two separate native ANT+ sensors** from one physical device:
- **ANT+ Bicycle Power** (device type 0x0B) — power + cadence
- **ANT+ Bicycle Speed & Cadence** (device type 0x79) — speed + distance

This is the path that succeeded after the BLE-multi-instance approach was
empirically falsified on epix 2 firmware 26.09 (see `docs/JOURNEY.md` §9 and
the `single-esp-dis-experiment` branch). ANT+ enumeration on Garmin watches
is profile-keyed `(device_type, device_number)`, not one-sensor-per-chip, so
two ANT+ profiles from a single chip pair as distinct native sensors —
exactly what BLE wouldn't allow.

## Phase status

- **Phase 1 — simulator path** ✅
  Nordic's `ant_bpwr_simulator` + `ant_bsc_simulator` generate plausible
  ramping data; both profiles broadcast concurrently and pair on a real
  epix 2; both stream into a Bike Indoor activity FIT file natively.
- **Phase 2 — BLE central wiring** (pending)
  Replace the simulator with a BLE-central path that subscribes to the
  Yesoul bike's FTMS Indoor Bike Data characteristic (0x2AD2) and feeds
  parsed frames into the BPWR/BSC profile state.

## Prerequisites (one-time setup)

| Tool | Version | Where |
|---|---|---|
| Nordic nRF5 SDK | 17.1.0 (`ddde560`) | [Nordic Download Center](https://www.nordicsemi.com/Products/Development-software/nrf5-sdk/download) |
| S340 SoftDevice | 6.1.1 | [thisisant.com](https://www.thisisant.com/) → ANT Adopter login → SoftDevice download |
| GNU Arm Embedded Toolchain | 13.x or newer (this project tested with 15.2.1) | `brew install --cask gcc-arm-embedded` |
| nrfutil | 7.x | [Nordic Download Center](https://www.nordicsemi.com/Products/Development-tools/nRF-Util/Download) (not Homebrew — that's still v5.2) |
| srecord | any recent | `brew install srecord` |
| ANT+ Adopter account | free at thisisant.com | required to download S340 |

Drop the S340 SoftDevice hex + headers into `<SDK>/components/softdevice/s340/`
mirroring the s140 layout (`hex/` + `headers/` + `headers/nrf52/`). Patch
`<SDK>/components/toolchain/gcc/Makefile.posix` to point at your installed
gcc-arm toolchain.

## Build + flash

The Makefile reads `SDK_ROOT` from env (default
`/Users/c/Developer/nrf52/nRF5_SDK_17.1.0_ddde560`) and requires
`ANT_LICENSE_KEY` at build time.

```bash
cd pca10059/s340/armgcc
make ANT_LICENSE_KEY="xxxx-xxxx-xxxx-xxxx-xxxx-xxxx-xxxx-xxxx"
```

Nordic publishes an evaluation key in `nrf_sdm.h` (line 191, commented out)
that they explicitly authorise for **non-commercial use only**. For a
personal-use bridge that key is fine. Commercial deployment requires a
licensed key from ANT Wireless.

Then put the dongle in DFU mode (side RESET button, RGB LED pulses red) and:

```bash
make flash ANT_LICENSE_KEY="..."
```

That bundles the S340 SoftDevice + the app into a Nordic DFU zip and pushes
it via `nrfutil dfu usb-serial`. The Makefile auto-discovers the
`/dev/cu.usbmodem*` port; override with `DFU_PORT=...` if needed.

## Diagnostic LEDs

Built-in to localise where the firmware dies if something goes wrong:

| LED pattern | Meaning |
|---|---|
| 3 quick green flashes at boot | `main()` entered, before SoftDevice init |
| 3 slower blue pulses | SoftDevice + ANT channels up — license key accepted, broadcasting |
| Continuous fast green blinks (~1 Hz) | Main loop alive, ANT broadcasts going out |
| Fast red strobe (~6 Hz, never stops) | `app_error_fault_handler` fired — SoftDevice or assertion fault |
| All LEDs dark | App didn't boot — linker addresses / HardFault |

## File layout

```
nrf52840_bridge/
├── main.c                                          ANT-only Phase 1 firmware
├── README.md                                       this file
└── pca10059/
    └── s340/
        ├── armgcc/
        │   ├── Makefile                            build + dfu-package + flash targets
        │   └── ant_bridge_gcc_nrf52.ld             linker (FLASH 0x31000-0xDFFFF,
        │                                            leaves Nordic Open Bootloader intact
        │                                            at 0xE0000-0xFFFFF)
        └── config/
            └── sdk_config.h                        SDK config — includes ANT_PLUS_NETWORK_KEY
                                                    override + BSC_*/BPWR_* identifier defaults
```

## Provenance

`main.c` is derived from
[turbodonkey/bike_power_meter](https://github.com/turbodonkey/bike_power_meter)
(MIT-equivalent ANT+ Shared Source License) with the Keiser-bike-specific
SAADC + hall-effect + gear-pot code paths stripped, and the
`ant_bpwr_evt_handler` reverted to Nordic's official bpwr_tx pattern
(`ant_bpwr_simulator_one_iteration` on every page event rather than
turbodonkey's manual data-copy-from-bike pattern).

The S340 SoftDevice binary itself is Garmin Canada's, distributed via
thisisant.com under the ANT+ Adopter Agreement; not redistributed here.
