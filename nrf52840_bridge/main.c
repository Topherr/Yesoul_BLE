// Yesoul_BLE bridge — nRF52840 + S340 SoftDevice + ANT+ Bicycle Power + ANT+
// Bicycle Speed/Cadence on a single chip (PCA10059 USB dongle target).
//
// This is the Phase-1 / Day-1 sanity firmware: it broadcasts plausible
// Nordic-simulator data so we can verify a Garmin epix 2 enumerates BPWR
// + BSC as TWO SEPARATE NATIVE SENSORS on one ANT device — the load-bearing
// test for the entire single-board ANT+ architecture.
//
// Source provenance: derived from turbodonkey/bike_power_meter
// (https://github.com/turbodonkey/bike_power_meter) which combined Nordic
// SDK 15.3.0 bpwr_tx + bsc_tx examples. We stripped the Keiser-bike-specific
// SAADC/hall-effect/gear-pot code paths; data is supplied by Nordic's
// `ant_bpwr_simulator` + `ant_bsc_simulator` (deterministic, drives itself
// from the ANT TX event handlers — no main-loop polling needed).
//
// Once the dual-profile enumeration test passes on a real epix 2, the
// simulator hooks get replaced with a BLE-central path that subscribes to
// the Yesoul indoor bike's FTMS Indoor Bike Data characteristic and feeds
// parsed frames into the BPWR/BSC profile state. That's Phase 2.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ant.h"
#include "app_error.h"
#include "app_timer.h"
#include "ant_key_manager.h"
#include "ant_bpwr.h"
#include "ant_bpwr_simulator.h"
#include "ant_bsc.h"
#include "ant_bsc_simulator.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include <string.h>

// ---- Fault handler ----
// Override the SDK's default app_error_fault_handler so faults are *visible*
// — red strobe on LD2 (P0.08) at ~6 Hz instead of the default silent infinite
// loop. Without this, an APP_ERROR_CHECK / NRF_LOG_ASSERT / SoftDevice fault
// just stops the heartbeat blink and leaves you guessing whether the chip is
// dead, broadcasting silently, or being missed by the watch.
void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info)
{
    (void)id; (void)pc; (void)info;
    __disable_irq();
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 8));   // LD2 red, active-low
    for (;;) {
        nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 8));
        nrf_delay_ms(80);
        nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, 8));
        nrf_delay_ms(80);
    }
}

// Forward declarations needed by the *_PROFILE_CONFIG_DEF macros below.
void ant_bpwr_evt_handler(ant_bpwr_profile_t * p_profile, ant_bpwr_evt_t event);
void ant_bpwr_calib_handler(ant_bpwr_profile_t * p_profile, ant_bpwr_page1_data_t * p_page1);
void ant_bsc_evt_handler(ant_bsc_profile_t * p_profile, ant_bsc_evt_t event);

// ---- ANT channel config ----
// Two channels: BPWR on 0 (device number 4) and BSC on 1 (device number 123).
// Distinct device numbers are what make the Garmin watch enumerate them as
// two separate sensors at the (device_type, device_number) tuple — the
// architectural premise we're testing.
//
// BPWR_SENS_CHANNEL_CONFIG_DEF(name, channel_number, transmission_type,
//                              device_number, network_number)
BPWR_SENS_CHANNEL_CONFIG_DEF(m_ant_bpwr,
                             0,    // channel
                             5,    // transmission type
                             4,    // device number
                             ANTPLUS_NETWORK_NUM);
BPWR_SENS_PROFILE_CONFIG_DEF(m_ant_bpwr,
                             (ant_bpwr_torque_t)(0),   // 0 = power meter only (no torque)
                             ant_bpwr_calib_handler,
                             ant_bpwr_evt_handler);

static ant_bpwr_profile_t   m_ant_bpwr;
static ant_bpwr_simulator_t m_ant_bpwr_simulator;

// BSC_SENS_CHANNEL_CONFIG_DEF(name, channel_number, transmission_type,
//                             device_type, device_number, network_number)
BSC_SENS_CHANNEL_CONFIG_DEF(m_ant_bsc,
                            1,     // channel
                            1,     // transmission type
                            123,   // device type 123 = speed-only sensor
                            5,     // device number
                            ANTPLUS_NETWORK_NUM);
BSC_SENS_PROFILE_CONFIG_DEF(m_ant_bsc,
                            true,                  // is_main_page_speed
                            true,                  // is_main_page_cadence
                            ANT_BSC_PAGE_5,        // main page
                            ant_bsc_evt_handler);

static ant_bsc_profile_t   m_ant_bsc;
static ant_bsc_simulator_t m_ant_bsc_simulator;

NRF_SDH_ANT_OBSERVER(m_ant_bpwr_observer, ANT_BPWR_ANT_OBSERVER_PRIO,
                     ant_bpwr_sens_evt_handler, &m_ant_bpwr);
NRF_SDH_ANT_OBSERVER(m_ant_bsc_observer, ANT_BSC_ANT_OBSERVER_PRIO,
                     ant_bsc_sens_evt_handler, &m_ant_bsc);

// ---- ANT BPWR event handler ----
// Nordic's official bpwr_tx pattern: call simulator_one_iteration() on EVERY
// page event. The simulator handles writing into the profile's data fields
// internally (BPWR_PROFILE_instantaneous_power, _cadence, _accumulated, etc.)
// — no manual data-copy needed.
//
// Turbodonkey's handler manually copied sensorsim_state.current_val into the
// profile on PAGE_1/PAGE_16 and only called one_iteration on PAGE_80/81.
// That made sense for their live-bike-data path (simulator kept counters in
// sync while real data drove the values), but for our simulator-only Phase-1
// build it left BPWR_PROFILE_instantaneous_power at 0 — the simulator
// advances once every ~16 s (PAGE_80/81 interleave), and the watch reads
// page 16 way more often, so it perpetually saw stale zeros.
void ant_bpwr_evt_handler(ant_bpwr_profile_t * p_profile, ant_bpwr_evt_t event)
{
    switch (event)
    {
        case ANT_BPWR_PAGE_1_UPDATED:
        case ANT_BPWR_PAGE_16_UPDATED:
        case ANT_BPWR_PAGE_17_UPDATED:
        case ANT_BPWR_PAGE_18_UPDATED:
        case ANT_BPWR_PAGE_80_UPDATED:
        case ANT_BPWR_PAGE_81_UPDATED:
            ant_bpwr_simulator_one_iteration(&m_ant_bpwr_simulator, event);
            break;
        default:
            break;
    }
}

// ---- ANT BSC event handler ----
void ant_bsc_evt_handler(ant_bsc_profile_t * p_profile, ant_bsc_evt_t event)
{
    switch (event)
    {
        case ANT_BSC_PAGE_0_UPDATED:
        case ANT_BSC_PAGE_1_UPDATED:
        case ANT_BSC_PAGE_2_UPDATED:
        case ANT_BSC_PAGE_3_UPDATED:
        case ANT_BSC_PAGE_4_UPDATED:
        case ANT_BSC_PAGE_5_UPDATED:
        case ANT_BSC_COMB_PAGE_0_UPDATED:
            ant_bsc_simulator_one_iteration(&m_ant_bsc_simulator);
            break;
        default:
            break;
    }
}

// BPWR calibration handler — the previous no-op stub caused the chip to
// fault after the watch paired, because the BPWR profile's page-1 encoder
// reads BPWR_PROFILE_calibration_id (set here) when sending the calibration
// response. Leaving it at the default (0 / ID_NONE) tripped an
// APP_ERROR_CHECK inside ant_bpwr_page_1.c — silent infinite loop, heartbeat
// stops, watch loses the sensor.
//
// We're a virtual power meter with nothing to physically calibrate, but the
// Garmin pair flow always sends a manual-zero/auto-zero probe. The contract
// is "respond with SUCCESS or FAILED for each calibration_id; never leave it
// at zero". Pattern adapted from turbodonkey's working handler + Nordic's
// bpwr_tx sample.
void ant_bpwr_calib_handler(ant_bpwr_profile_t * p_profile, ant_bpwr_page1_data_t * p_page1)
{
    (void)p_profile;
    switch (p_page1->calibration_id)
    {
        case ANT_BPWR_CALIB_ID_MANUAL:
        case ANT_BPWR_CALIB_ID_AUTO:
            m_ant_bpwr.BPWR_PROFILE_calibration_id     = ANT_BPWR_CALIB_ID_MANUAL_SUCCESS;
            m_ant_bpwr.BPWR_PROFILE_auto_zero_status   = p_page1->auto_zero_status;
            m_ant_bpwr.BPWR_PROFILE_general_calib_data = 0;
            break;
        case ANT_BPWR_CALIB_ID_CUSTOM_REQ:
            m_ant_bpwr.BPWR_PROFILE_calibration_id = ANT_BPWR_CALIB_ID_CUSTOM_REQ_SUCCESS;
            memcpy(m_ant_bpwr.BPWR_PROFILE_custom_calib_data,
                   p_page1->data.custom_calib,
                   sizeof(m_ant_bpwr.BPWR_PROFILE_custom_calib_data));
            break;
        case ANT_BPWR_CALIB_ID_CUSTOM_UPDATE:
            m_ant_bpwr.BPWR_PROFILE_calibration_id = ANT_BPWR_CALIB_ID_CUSTOM_UPDATE_SUCCESS;
            memcpy(m_ant_bpwr.BPWR_PROFILE_custom_calib_data,
                   p_page1->data.custom_calib,
                   sizeof(m_ant_bpwr.BPWR_PROFILE_custom_calib_data));
            break;
        default:
            m_ant_bpwr.BPWR_PROFILE_calibration_id = ANT_BPWR_CALIB_ID_FAILED;
            break;
    }
}

// ---- Setup helpers ----

static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

// Stripped utils_setup — just timer + power-management + ANT state indicator.
// No GPIOTE, no SAADC, no hall-effect, no Keiser-specific timers.
static void utils_setup(void)
{
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
    // ant_state_indicator dropped — it only drives onboard LEDs to reflect
    // channel state. Pulls in components/libraries/bsp transitively. We
    // monitor channel state via the watch + nrf_log instead.
}

static void softdevice_setup(void)
{
    ret_code_t err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    ASSERT(nrf_sdh_is_enabled());

    err_code = nrf_sdh_ant_enable();
    APP_ERROR_CHECK(err_code);

    err_code = ant_plus_key_set(ANTPLUS_NETWORK_NUM);
    APP_ERROR_CHECK(err_code);

    sd_clock_hfclk_request();
}

// Initialise both ANT+ simulators in AUTO mode. The third arg to *_init() is
// auto_change: false = button-driven (BSP_EVENT_KEY_*), true = auto-ramp
// on every one_iteration() call. We have no buttons wired up on the dongle
// in this Phase-1 firmware, so auto-mode is the only thing that produces
// non-zero data.
static void simulator_setup(void)
{
    const ant_bpwr_simulator_cfg_t bpwr_simulator_cfg =
    {
        .p_profile   = &m_ant_bpwr,
        .sensor_type = (ant_bpwr_torque_t)(0),    // power meter only
    };
    bpwr_simulator_cfg.p_profile->page_16.pedal_power.byte = 0xFF;   // not using pedal power distribution
    ant_bpwr_simulator_init(&m_ant_bpwr_simulator, &bpwr_simulator_cfg, true);

    const ant_bsc_simulator_cfg_t bsc_simulator_cfg =
    {
        .p_profile   = &m_ant_bsc,
        .device_type = 123,    // speed sensor only
    };
    ant_bsc_simulator_init(&m_ant_bsc_simulator, &bsc_simulator_cfg, true);
}

// Manufacturer/model identifiers for the BPWR common-data pages.
// BSC_MF_ID / BSC_MODEL_NUMBER / BSC_HW_VERSION / BSC_SW_VERSION /
// BSC_SERIAL_NUMBER and the BPWR equivalents (BPWR_HW_REVISION /
// BPWR_MANUFACTURER_ID / BPWR_MODEL_NUMBER / BPWR_SW_REVISION_* /
// BPWR_SERIAL_NUMBER) are all defined upstream in sdk_config.h.

static void profile_setup(void)
{
    ret_code_t err_code;

    err_code = ant_bpwr_sens_init(&m_ant_bpwr,
                                  BPWR_SENS_CHANNEL_CONFIG(m_ant_bpwr),
                                  BPWR_SENS_PROFILE_CONFIG(m_ant_bpwr));
    APP_ERROR_CHECK(err_code);

    m_ant_bpwr.page_80 = ANT_COMMON_page80(BPWR_HW_REVISION,
                                           BPWR_MANUFACTURER_ID,
                                           BPWR_MODEL_NUMBER);
    m_ant_bpwr.page_81 = ANT_COMMON_page81(BPWR_SW_REVISION_MAJOR,
                                           BPWR_SW_REVISION_MINOR,
                                           BPWR_SERIAL_NUMBER);
    m_ant_bpwr.BPWR_PROFILE_auto_zero_status = ANT_BPWR_AUTO_ZERO_OFF;

    err_code = ant_bpwr_sens_open(&m_ant_bpwr);
    APP_ERROR_CHECK(err_code);

    err_code = ant_bsc_sens_init(&m_ant_bsc,
                                 BSC_SENS_CHANNEL_CONFIG(m_ant_bsc),
                                 BSC_SENS_PROFILE_CONFIG(m_ant_bsc));
    APP_ERROR_CHECK(err_code);

    m_ant_bsc.BSC_PROFILE_manuf_id   = BSC_MF_ID;
    m_ant_bsc.BSC_PROFILE_serial_num = BSC_SERIAL_NUMBER;
    m_ant_bsc.BSC_PROFILE_hw_version = BSC_HW_VERSION;
    m_ant_bsc.BSC_PROFILE_sw_version = BSC_SW_VERSION;
    m_ant_bsc.BSC_PROFILE_model_num  = BSC_MODEL_NUMBER;

    err_code = ant_bsc_sens_open(&m_ant_bsc);
    APP_ERROR_CHECK(err_code);
}

// PCA10059 LED pinout (active low):
//   LD1     green        P0.06
//   LD2 R                P0.08
//   LD2 G                P1.09
//   LD2 B                P0.12
//
// Diagnostic blink phases (so we can tell what stage of init wedged):
//   Phase A — green LD1 fast blink   : main entered, before SoftDevice init
//   Phase B — RGB blue LD2 slow pulse: SoftDevice + ANT channels up, broadcasting
// Both LEDs are active-low. We poll-blink via a small helper that uses
// nrf_delay (busy-wait) so it works before any timer/RTC infrastructure.
#define LED_GREEN_PIN  NRF_GPIO_PIN_MAP(0, 6)
#define LED_BLUE_PIN   NRF_GPIO_PIN_MAP(0, 12)

static void diag_led_pulse(uint32_t pin, int times, uint32_t on_ms)
{
    nrf_gpio_cfg_output(pin);
    for (int i = 0; i < times; i++) {
        nrf_gpio_pin_clear(pin);     // active-low: clear = ON
        nrf_delay_ms(on_ms);
        nrf_gpio_pin_set(pin);
        nrf_delay_ms(on_ms);
    }
}

int main(void)
{
    log_init();
    utils_setup();

    // Phase A: app entered, about to init SoftDevice. Three quick green flashes.
    // If you see this but not Phase B, SoftDevice init failed (license key
    // rejection, RAM layout collision, etc.).
    diag_led_pulse(LED_GREEN_PIN, 3, 100);

    softdevice_setup();
    simulator_setup();
    profile_setup();

    // Phase B: SoftDevice up, both ANT channels opened. Three blue pulses.
    // If you see this, broadcasts SHOULD be reaching the watch — any failure
    // beyond this point is RF / pairing-flow / channel-config territory.
    diag_led_pulse(LED_BLUE_PIN, 3, 250);

    NRF_LOG_INFO("ANT+ BPWR + BSC bridge started (Phase-1 / Day-1 sim mode)");

    for (;;)
    {
        // Heartbeat: one quick green blink per second to confirm the main
        // loop is alive. If this stops, the chip wedged.
        nrf_gpio_pin_clear(LED_GREEN_PIN);
        nrf_delay_ms(30);
        nrf_gpio_pin_set(LED_GREEN_PIN);

        NRF_LOG_FLUSH();
        nrf_pwr_mgmt_run();
    }
}
