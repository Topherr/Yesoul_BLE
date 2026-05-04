// Yesoul_BLE bridge — single-ESP experiment.
//
// Same goal as master (bike → watch as power + cadence + speed + distance) but
// running on ONE ESP32 using BLE 5.0 extended advertising with two advertising
// instances at distinct random-static addresses. The watch sees them as two
// separate sensors and pairs each under its respective category, while sharing
// one GATT server (both CPS and CSC services). If this works, the dual-ESP
// architecture on master becomes redundant.
//
// Build flag CONFIG_BT_NIMBLE_EXT_ADV=1 (set in platformio.ini) is required.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ftms_parser.h"

#if !CONFIG_BT_NIMBLE_EXT_ADV
#error "CONFIG_BT_NIMBLE_EXT_ADV must be enabled — add -DCONFIG_BT_NIMBLE_EXT_ADV=1 to build_flags."
#endif

// ---- Configuration ----
static constexpr float    POWER_SCALE              = 1.28f;
static constexpr float    WHEEL_CIRCUMFERENCE_M    = 2.000f;
static constexpr uint32_t NOTIFICATION_WATCHDOG_MS = 5000;
static constexpr uint32_t COOLDOWN_MS              = 1000;
static constexpr uint32_t SCAN_BACKOFF_INITIAL_MS  = 5000;
static constexpr uint32_t SCAN_BACKOFF_MAX_MS      = 60000;

// Two distinct random-static addresses. Top two bits of byte 0 must be 11
// (i.e. byte 0 in 0xC0..0xFF) for a valid random-static identity address.
// Addresses are deliberately divergent across all six bytes — early
// experiments with addresses sharing an OUI hinted that the Garmin epix 2
// might dedupe two connections "from the same vendor" down to one. Different
// vendor bytes side-step that heuristic if it exists.
static constexpr const char* PWR_ADDRESS  = "C1:11:22:33:44:55";
static constexpr const char* SPD_ADDRESS  = "E2:66:77:88:99:AA";
static constexpr const char* PWR_NAME     = "Yesoul_PWR";
static constexpr const char* SPD_NAME     = "Yesoul_SPD";
static constexpr uint16_t    PWR_APPEAR   = 0x0484;  // Cycling Power Sensor
static constexpr uint16_t    SPD_APPEAR   = 0x0482;  // Cycling Speed Sensor

// Bike-data simulation (skip the bike, inject constant values). Default false.
static constexpr bool     SIMULATE_BIKE            = false;
static constexpr uint16_t SIM_SPEED_CMPS           = 2000;
static constexpr uint16_t SIM_CADENCE_HALFRPM      = 120;
static constexpr int16_t  SIM_INST_POWER_W         = 150;

// ---- BLE UUIDs ----
static const NimBLEUUID FTMS_SERVICE_UUID("1826");
static const NimBLEUUID FTMS_INDOOR_BIKE_DATA_UUID("2AD2");

static const NimBLEUUID CPS_SERVICE_UUID("1818");
static const NimBLEUUID CPS_MEASUREMENT_UUID("2A63");
static const NimBLEUUID CPS_FEATURE_UUID("2A65");
static const NimBLEUUID CPS_SENSOR_LOCATION_UUID("2A5D");

static const NimBLEUUID CSC_SERVICE_UUID("1816");
static const NimBLEUUID CSC_MEASUREMENT_UUID("2A5B");
static const NimBLEUUID CSC_FEATURE_UUID("2A5C");
static const NimBLEUUID CSC_SC_CONTROL_POINT_UUID("2A55");

// ---- Bike-side state machine ----
enum BikeState { S_SCANNING, S_CONNECTED, S_STREAMING, S_DISCONNECTED, S_COOLDOWN };

static volatile BikeState g_state                = S_SCANNING;
static volatile uint32_t  g_last_notification_ms = 0;
static volatile bool      g_doConnect            = false;
static uint32_t           g_scan_backoff_ms      = SCAN_BACKOFF_INITIAL_MS;
static uint32_t           g_cooldown_started_ms  = 0;
static uint32_t           g_consecutive_failures = 0;

static NimBLEClient*           g_pClient = nullptr;
static NimBLEAdvertisedDevice* g_pTarget = nullptr;

static QueueHandle_t g_frameQueue = nullptr;

// ---- Garmin-side state ----
static NimBLEServer*         g_pServer        = nullptr;
static NimBLECharacteristic* g_cpsMeasurement = nullptr;
static NimBLECharacteristic* g_cscMeasurement = nullptr;

static volatile uint16_t g_speed_cmps      = 0;
static volatile uint16_t g_cadence_halfrpm = 0;
static volatile int16_t  g_inst_power_w    = 0;

static float    g_wheel_revs_accum      = 0.0f;
static float    g_crank_revs_accum      = 0.0f;
static uint32_t g_cumulative_wheel_revs = 0;
static uint16_t g_cumulative_crank_revs = 0;
static uint16_t g_last_wheel_event_time_1024 = 0;
static uint16_t g_last_crank_event_time_1024 = 0;
static uint32_t g_last_csc_update_ms    = 0;
static uint32_t g_last_publish_ms       = 0;
static volatile uint32_t g_notify_count = 0;

// ---- Server callbacks ----
//
// Extended-advertising quirk: when a peripheral has multiple advertising
// instances and a peer connects to instance N, that instance automatically
// stops advertising (BLE 5.0 spec behaviour). Other instances are unaffected
// in principle — but in practice the watch only completes one of the two
// connections it wants, then later re-connects, and by the time it tries
// the second address the corresponding instance is no longer broadcasting.
//
// Fix: re-start BOTH instances on every connect and every disconnect. Calling
// start() on an already-started instance is a no-op; calling on a stopped one
// brings it back up. Idempotent and cheap.
static void resume_both_advertising_instances() {
  NimBLEExtAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->start(0, 0);
  pAdv->start(1, 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    Serial.printf("[srv] client connected: %s (handle=%u, total=%u)\n",
                  connInfo.getAddress().toString().c_str(),
                  connInfo.getConnHandle(),
                  pServer->getConnectedCount());
    pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 60);
    resume_both_advertising_instances();
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[srv] client disconnected (reason=0x%x, remaining=%u)\n",
                  reason, pServer->getConnectedCount());
    if (pServer->getConnectedCount() == 0) {
      g_wheel_revs_accum      = 0;
      g_crank_revs_accum      = 0;
      g_cumulative_wheel_revs = 0;
      g_cumulative_crank_revs = 0;
    }
    resume_both_advertising_instances();
  }
};

// SC Control Point: ACK Set Cumulative Value as no-op success; Op Code Not
// Supported for everything else. Garmin watches require this to exist.
class SCControlPointCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo&) override {
    NimBLEAttValue val = pChar->getValue();
    if (val.length() == 0) return;
    uint8_t op = val[0];
    uint8_t resp[3] = { 0x10, op, 0x02 };
    if (op == 1 && val.length() >= 5) {
      uint32_t newVal = (uint32_t)val[1] | ((uint32_t)val[2] << 8) |
                        ((uint32_t)val[3] << 16) | ((uint32_t)val[4] << 24);
      g_wheel_revs_accum      = (float)newVal;
      g_cumulative_wheel_revs = newVal;
      resp[2] = 0x01;
    }
    pChar->setValue(resp, 3);
    pChar->indicate();
  }
};

// ---- Bike-side (client) callbacks ----
class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    Serial.printf("[bike] disconnected (reason=0x%x)\n", reason);
    g_state = S_DISCONNECTED;
  }
} g_clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (g_state != S_SCANNING) return;
    if (!dev->haveServiceUUID() || !dev->isAdvertisingService(FTMS_SERVICE_UUID)) return;
    Serial.printf("[bike] FTMS target found: '%s' @ %s\n",
                  dev->getName().c_str(), dev->getAddress().toString().c_str());
    NimBLEDevice::getScan()->stop();
    delete g_pTarget;
    g_pTarget   = new NimBLEAdvertisedDevice(*dev);
    g_doConnect = true;
  }
} g_scanCallbacks;

static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* pData,
                           size_t len, bool) {
  BikeFrame frame{};
  if (parse_indoor_bike_data(pData, len, frame)) {
    frame.millis_received = millis();
    xQueueSend(g_frameQueue, &frame, 0);
  }
  g_last_notification_ms = millis();
  g_notify_count++;
}

static bool connect_to_target() {
  if (!g_pTarget) return false;
  if (g_pClient) {
    NimBLEDevice::deleteClient(g_pClient);
    g_pClient = nullptr;
  }
  g_pClient = NimBLEDevice::createClient();
  g_pClient->setClientCallbacks(&g_clientCallbacks, false);
  if (!g_pClient->connect(g_pTarget)) {
    NimBLEDevice::deleteClient(g_pClient);
    g_pClient = nullptr;
    return false;
  }
  NimBLERemoteService* svc = g_pClient->getService(FTMS_SERVICE_UUID);
  if (!svc) { g_pClient->disconnect(); return false; }
  NimBLERemoteCharacteristic* chr = svc->getCharacteristic(FTMS_INDOOR_BIKE_DATA_UUID);
  if (!chr) { g_pClient->disconnect(); return false; }
  if (!chr->subscribe(true, notifyCallback)) {
    g_pClient->disconnect();
    return false;
  }
  g_last_notification_ms = millis();
  return true;
}

static void start_scan() {
  Serial.println("[bike] scanning...");
  g_state = S_SCANNING;
  NimBLEDevice::getScan()->start(0, false, true);
}

static void update_revolution_counters() {
  uint32_t now = millis();
  if (g_last_csc_update_ms == 0) { g_last_csc_update_ms = now; return; }
  float dt_s = (now - g_last_csc_update_ms) / 1000.0f;
  g_last_csc_update_ms = now;

  float speed_mps = g_speed_cmps * 0.01f / 3.6f;
  g_wheel_revs_accum += speed_mps * dt_s / WHEEL_CIRCUMFERENCE_M;
  uint32_t new_wheel = (uint32_t)g_wheel_revs_accum;
  if (new_wheel > g_cumulative_wheel_revs) {
    g_cumulative_wheel_revs = new_wheel;
    g_last_wheel_event_time_1024 = (uint16_t)(((uint64_t)now * 1024 / 1000) & 0xffff);
  }

  float crank_rps = (g_cadence_halfrpm * 0.5f) / 60.0f;
  g_crank_revs_accum += crank_rps * dt_s;
  uint16_t new_crank = (uint16_t)g_crank_revs_accum;
  if (new_crank != g_cumulative_crank_revs) {
    g_cumulative_crank_revs = new_crank;
    g_last_crank_event_time_1024 = (uint16_t)(((uint64_t)now * 1024 / 1000) & 0xffff);
  }
}

static void publish_cps_frame() {
  // CPS Measurement, flags 0x0020 (crank-rev only). 8 bytes.
  int16_t pwr = (int16_t)(g_inst_power_w * POWER_SCALE);
  uint16_t flags = 0x0020;
  uint8_t buf[8];
  buf[0] = flags & 0xff;       buf[1] = (flags >> 8) & 0xff;
  buf[2] = pwr & 0xff;         buf[3] = (pwr >> 8) & 0xff;
  buf[4] = g_cumulative_crank_revs & 0xff;
  buf[5] = (g_cumulative_crank_revs >> 8) & 0xff;
  buf[6] = g_last_crank_event_time_1024 & 0xff;
  buf[7] = (g_last_crank_event_time_1024 >> 8) & 0xff;
  g_cpsMeasurement->setValue(buf, 8);
  g_cpsMeasurement->notify();
}

static void publish_csc_frame() {
  // CSC Measurement, flags 0x01 (wheel-rev only). 7 bytes.
  uint8_t buf[7];
  buf[0] = 0x01;
  buf[1] = g_cumulative_wheel_revs        & 0xff;
  buf[2] = (g_cumulative_wheel_revs >> 8)  & 0xff;
  buf[3] = (g_cumulative_wheel_revs >> 16) & 0xff;
  buf[4] = (g_cumulative_wheel_revs >> 24) & 0xff;
  buf[5] = g_last_wheel_event_time_1024        & 0xff;
  buf[6] = (g_last_wheel_event_time_1024 >> 8) & 0xff;
  g_cscMeasurement->setValue(buf, 7);
  g_cscMeasurement->notify();
}

void setup() {
  Serial.begin(115200);
  // C6's only serial path is USB-Serial/JTAG (no UART bridge chip), so the
  // host needs ~1-2 s to enumerate the CDC interface after reset. Without
  // this delay the boot banner is swallowed every time.
  delay(2000);
  Serial.println();
  Serial.println("=== Yesoul_BLE single-ESP experiment ===");
  Serial.printf("Power scale: %.3f, wheel: %.3f m\n",
                (double)POWER_SCALE, (double)WHEEL_CIRCUMFERENCE_M);
  if (SIMULATE_BIKE) {
    Serial.printf("[sim] BIKE SIMULATION ENABLED — speed=%.2fkmh, cadence=%uRPM, raw_power=%dW\n",
                  SIM_SPEED_CMPS / 100.0, SIM_CADENCE_HALFRPM / 2, (int)SIM_INST_POWER_W);
  }

  g_frameQueue = xQueueCreate(8, sizeof(BikeFrame));

  NimBLEDevice::init("Yesoul");
  // NimBLE 2.x setPower takes dBm directly. +9 dBm matches what ESP_PWR_LVL_P9
  // resolved to under the legacy enum API used on master.
  NimBLEDevice::setPower(9);
  Serial.printf("[srv] %d bonds in NVS, clearing\n", NimBLEDevice::getNumBonds());
  NimBLEDevice::deleteAllBonds();

  uint8_t loc[1] = { 0x0D };  // Rear Hub.

  g_pServer = NimBLEDevice::createServer();
  g_pServer->setCallbacks(new ServerCallbacks());

  // ---- CSC service (registered first so the GATT handle ordering puts CSC
  //      ahead of CPS in discovery, matching what worked in the dual-ESP
  //      build and minimising risk that the watch trips on order). ----
  NimBLEService* csc = g_pServer->createService(CSC_SERVICE_UUID);
  NimBLECharacteristic* cscFeature = csc->createCharacteristic(CSC_FEATURE_UUID, NIMBLE_PROPERTY::READ);
  uint8_t cscFeatureValue[2] = { 0x01, 0x00 };
  cscFeature->setValue(cscFeatureValue, 2);
  NimBLECharacteristic* cscLoc = csc->createCharacteristic(CPS_SENSOR_LOCATION_UUID, NIMBLE_PROPERTY::READ);
  cscLoc->setValue(loc, 1);
  g_cscMeasurement = csc->createCharacteristic(CSC_MEASUREMENT_UUID,
                                               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t initCsc[7] = {0};
  g_cscMeasurement->setValue(initCsc, 7);
  NimBLECharacteristic* scControlPoint = csc->createCharacteristic(
      CSC_SC_CONTROL_POINT_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
  scControlPoint->setCallbacks(new SCControlPointCallbacks());

  // ---- CPS service ----
  NimBLEService* cps = g_pServer->createService(CPS_SERVICE_UUID);
  NimBLECharacteristic* cpsFeature = cps->createCharacteristic(CPS_FEATURE_UUID, NIMBLE_PROPERTY::READ);
  uint8_t cpsFeatureValue[4] = { 0x08, 0x00, 0x00, 0x00 };
  cpsFeature->setValue(cpsFeatureValue, 4);
  NimBLECharacteristic* cpsLoc = cps->createCharacteristic(CPS_SENSOR_LOCATION_UUID, NIMBLE_PROPERTY::READ);
  cpsLoc->setValue(loc, 1);
  g_cpsMeasurement = cps->createCharacteristic(CPS_MEASUREMENT_UUID,
                                               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t initCps[8] = {0};
  g_cpsMeasurement->setValue(initCps, 8);

  g_pServer->start();

  // ---- Two extended-advertising instances at distinct random-static
  //      addresses. Each instance only advertises ONE service UUID — the
  //      watch's category-specific scan filters on advertised UUIDs, so
  //      each address is invisible to the wrong category. ----
  NimBLEExtAdvertisement powerAdv;
  powerAdv.setLegacyAdvertising(true);
  powerAdv.setConnectable(true);
  powerAdv.setScannable(true);
  powerAdv.setAddress(NimBLEAddress(PWR_ADDRESS, BLE_ADDR_RANDOM));
  powerAdv.setName(PWR_NAME);
  powerAdv.setAppearance(PWR_APPEAR);
  powerAdv.setCompleteServices16({CPS_SERVICE_UUID});

  NimBLEExtAdvertisement speedAdv;
  speedAdv.setLegacyAdvertising(true);
  speedAdv.setConnectable(true);
  speedAdv.setScannable(true);
  speedAdv.setAddress(NimBLEAddress(SPD_ADDRESS, BLE_ADDR_RANDOM));
  speedAdv.setName(SPD_NAME);
  speedAdv.setAppearance(SPD_APPEAR);
  speedAdv.setCompleteServices16({CSC_SERVICE_UUID});

  NimBLEExtAdvertising* pAdv = NimBLEDevice::getAdvertising();
  // Use intermediate booleans so both calls always run regardless of return
  // values — short-circuit `||` would skip the second instance if the first
  // failed, leaving the bug invisible.
  bool data0_ok = pAdv->setInstanceData(0, powerAdv);
  bool data1_ok = pAdv->setInstanceData(1, speedAdv);
  if (!data0_ok || !data1_ok) {
    Serial.printf("[srv] FAILED to register advertising data (pwr=%d, spd=%d)\n",
                  data0_ok, data1_ok);
  }
  bool start0_ok = pAdv->start(0, 0);
  bool start1_ok = pAdv->start(1, 0);
  if (!start0_ok || !start1_ok) {
    Serial.printf("[srv] FAILED to start advertising (pwr=%d, spd=%d)\n",
                  start0_ok, start1_ok);
  } else {
    Serial.printf("[srv] advertising as %s (%s) + %s (%s)\n",
                  PWR_NAME, PWR_ADDRESS, SPD_NAME, SPD_ADDRESS);
  }

  // ---- Bike-side scan setup ----
  if (!SIMULATE_BIKE) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scanCallbacks, false);
    scan->setInterval(1349);
    scan->setWindow(449);
    scan->setActiveScan(true);
    start_scan();
  } else {
    g_state = S_STREAMING;
  }
}

void loop() {
  if (SIMULATE_BIKE) {
    g_speed_cmps           = SIM_SPEED_CMPS;
    g_cadence_halfrpm      = SIM_CADENCE_HALFRPM;
    g_inst_power_w         = SIM_INST_POWER_W;
    g_last_notification_ms = millis();
    g_state                = S_STREAMING;
  }

  BikeFrame frame;
  while (xQueueReceive(g_frameQueue, &frame, 0) == pdTRUE) {
    if (frame.has_speed)      g_speed_cmps      = frame.speed_cmps;
    if (frame.has_cadence)    g_cadence_halfrpm = frame.cadence_halfrpm;
    if (frame.has_inst_power) g_inst_power_w    = frame.inst_power_w;
    if (g_state == S_CONNECTED) g_state = S_STREAMING;
  }

  if (!SIMULATE_BIKE) {
    if (g_doConnect) {
      g_doConnect = false;
      if (connect_to_target()) {
        Serial.println("[bike] connected, subscribed to FTMS Indoor Bike Data");
        g_state = S_CONNECTED;
        g_consecutive_failures = 0;
      } else {
        Serial.println("[bike] connect failed");
        g_consecutive_failures++;
        uint32_t shift = g_consecutive_failures > 4 ? 4 : g_consecutive_failures - 1;
        uint32_t b = SCAN_BACKOFF_INITIAL_MS << shift;
        g_scan_backoff_ms = b > SCAN_BACKOFF_MAX_MS ? SCAN_BACKOFF_MAX_MS : b;
        g_state = S_COOLDOWN;
        g_cooldown_started_ms = millis();
      }
    }

    if (g_state == S_STREAMING &&
        (millis() - g_last_notification_ms) > NOTIFICATION_WATCHDOG_MS) {
      Serial.println("[bike] watchdog: no notifications for 5 s");
      g_state = S_DISCONNECTED;
    }

    if (g_state == S_DISCONNECTED) {
      Serial.println("[bike] cleaning up client");
      if (g_pClient) {
        if (g_pClient->isConnected()) g_pClient->disconnect();
        NimBLEDevice::deleteClient(g_pClient);
        g_pClient = nullptr;
      }
      g_speed_cmps      = 0;
      g_cadence_halfrpm = 0;
      g_inst_power_w    = 0;
      g_state = S_COOLDOWN;
      g_cooldown_started_ms = millis();
    }
    if (g_state == S_COOLDOWN) {
      uint32_t wait = g_consecutive_failures > 0 ? g_scan_backoff_ms : COOLDOWN_MS;
      if (millis() - g_cooldown_started_ms >= wait) {
        start_scan();
      }
    }
  }

  uint32_t now = millis();
  if (now - g_last_publish_ms >= 1000) {
    g_last_publish_ms = now;
    static uint32_t last_logged_notifies = 0;
    uint32_t notifies = g_notify_count;
    Serial.printf("[1Hz] state=%d notifies=%lu (+%lu) speed=%.2fkmh cad=%uRPM pwr=%dW conns=%u\n",
                  (int)g_state, (unsigned long)notifies,
                  (unsigned long)(notifies - last_logged_notifies),
                  g_speed_cmps / 100.0, g_cadence_halfrpm / 2,
                  (int)g_inst_power_w, (unsigned)g_pServer->getConnectedCount());
    last_logged_notifies = notifies;
    if (g_pServer->getConnectedCount() > 0) {
      update_revolution_counters();
      publish_cps_frame();
      publish_csc_frame();
    }
  }

  delay(10);
}
