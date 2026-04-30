// Yesoul_BLE bridge: subscribes to a Yesoul G1M Plus FTMS Indoor Bike Data
// stream and re-broadcasts the data as a standard Cycling Power Service +
// Cycling Speed and Cadence Service so Garmin Edge / watch can consume it.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ftms_parser.h"

// ---- Configuration ----
// Empirical findings (2026-04-30):
//   1. Garmin epix 2's "Add Sensor → Speed" filter rejects devices that also
//      expose CPS — only CSC-only peripherals are listed. So we ship two ESPs:
//      one CPS-only (POWER), one CSC-only (SPEED).
//   2. The Yesoul allows only one BLE central. The POWER ESP owns the bike
//      connection; the SPEED ESP receives parsed BikeFrames via ESP-NOW relay.
// Role is selected via build flag in platformio.ini (-DDEVICE_ROLE_POWER or
// -DDEVICE_ROLE_SPEED). See docs/ARCHITECTURE.md and docs/JOURNEY.md.
enum class Role { POWER, SPEED };
#if defined(DEVICE_ROLE_POWER)
static constexpr Role     DEVICE_ROLE              = Role::POWER;
#elif defined(DEVICE_ROLE_SPEED)
static constexpr Role     DEVICE_ROLE              = Role::SPEED;
#else
#error "Define DEVICE_ROLE_POWER or DEVICE_ROLE_SPEED via -D in platformio.ini"
#endif
static constexpr bool     IS_POWER                 = (DEVICE_ROLE == Role::POWER);
static constexpr bool     IS_SPEED                 = (DEVICE_ROLE == Role::SPEED);
static constexpr const char* DEVICE_NAME           = IS_POWER ? "Yesoul_PWR" : "Yesoul_SPD";
static constexpr uint16_t DEVICE_APPEARANCE        = IS_POWER ? 0x0484 : 0x0482;  // 0x0484 = Cycling Power Sensor, 0x0482 = Cycling Speed Sensor

// SIMULATE_BIKE: bypass bike-side scan/connect entirely and inject realistic
// mid-ride values into the publish pipeline. Useful for verifying pairing
// flows without pedaling. Default false → real bike.
static constexpr bool     SIMULATE_BIKE            = false;
static constexpr uint16_t SIM_SPEED_CMPS           = 2000;    // 20.00 km/h
static constexpr uint16_t SIM_CADENCE_HALFRPM      = 120;     // 60 RPM
static constexpr int16_t  SIM_INST_POWER_W         = 150;

static constexpr float    POWER_SCALE              = 1.28f;
static constexpr float    WHEEL_CIRCUMFERENCE_M    = 2.000f;
static constexpr uint32_t NOTIFICATION_WATCHDOG_MS = 5000;
static constexpr uint32_t COOLDOWN_MS              = 1000;
static constexpr uint32_t SCAN_BACKOFF_INITIAL_MS  = 5000;
static constexpr uint32_t SCAN_BACKOFF_MAX_MS      = 60000;

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
enum BikeState {
  S_SCANNING,
  S_CONNECTED,
  S_STREAMING,
  S_DISCONNECTED,
  S_COOLDOWN,
};

static volatile BikeState g_state                  = S_SCANNING;
static volatile uint32_t  g_last_notification_ms   = 0;
static volatile bool      g_doConnect              = false;
static uint32_t           g_scan_backoff_ms        = SCAN_BACKOFF_INITIAL_MS;
static uint32_t           g_cooldown_started_ms    = 0;
static uint32_t           g_consecutive_failures   = 0;

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

// Cumulative counters persist across bike-side reconnects; reset only when the
// last Garmin client disconnects, so the head-unit doesn't see counter resets
// mid-ride.
static float    g_wheel_revs_accum      = 0.0f;
static float    g_crank_revs_accum      = 0.0f;
static uint32_t g_cumulative_wheel_revs = 0;
static uint16_t g_cumulative_crank_revs = 0;
// CSC uses 1/1024 s for both wheel and crank event times.
static uint16_t g_last_wheel_event_time_1024 = 0;
static uint16_t g_last_crank_event_time_1024 = 0;
static uint32_t g_last_csc_update_ms         = 0;
static uint32_t g_last_publish_ms            = 0;

// ---- Server (Garmin-facing) callbacks ----
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    Serial.printf("[srv] client connected: %s\n", connInfo.getAddress().toString().c_str());
    pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 60);
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[srv] client disconnected (reason=0x%x)\n", reason);
    if (pServer->getConnectedCount() == 0) {
      g_wheel_revs_accum = 0;
      g_crank_revs_accum = 0;
      g_cumulative_wheel_revs = 0;
      g_cumulative_crank_revs = 0;
    }
    NimBLEDevice::startAdvertising();
  }
};

// SC Control Point: ACK Set Cumulative Value (op 1) as a no-op success;
// Op Code Not Supported for everything else. Garmin watches require this
// characteristic to exist for the CSC sensor to be discoverable.
class SCControlPointCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    NimBLEAttValue val = pCharacteristic->getValue();
    if (val.length() == 0) return;
    uint8_t op = val[0];
    uint8_t resp[3] = { 0x10, op, 0x02 };  // default: Op Code Not Supported
    if (op == 1 && val.length() >= 5) {
      uint32_t newVal = (uint32_t)val[1] | ((uint32_t)val[2] << 8) |
                        ((uint32_t)val[3] << 16) | ((uint32_t)val[4] << 24);
      g_wheel_revs_accum      = (float)newVal;
      g_cumulative_wheel_revs = newVal;
      resp[2] = 0x01;  // Success
    }
    pCharacteristic->setValue(resp, 3);
    pCharacteristic->indicate();
  }
};

// ---- Bike-side (client) callbacks ----
class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.printf("[bike] disconnected (reason=0x%x)\n", reason);
    g_state = S_DISCONNECTED;
  }
} g_clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (g_state != S_SCANNING) return;
    // Match on FTMS service UUID alone; the Yesoul doesn't always advertise
    // a name and the user only has one FTMS device in their environment.
    if (!dev->haveServiceUUID() || !dev->isAdvertisingService(FTMS_SERVICE_UUID)) return;

    Serial.printf("[bike] FTMS target found: '%s' @ %s\n",
                  dev->getName().c_str(), dev->getAddress().toString().c_str());
    NimBLEDevice::getScan()->stop();
    delete g_pTarget;
    g_pTarget   = new NimBLEAdvertisedDevice(*dev);
    g_doConnect = true;
  }
} g_scanCallbacks;

// notifyCallback runs on the BLE host task — do nothing heavy.
static volatile uint32_t g_notify_count = 0;

// ESP-NOW relay: the Yesoul allows only one BLE central, so the POWER ESP owns
// the bike connection and broadcasts each parsed BikeFrame to the SPEED ESP
// via ESP-NOW. Both ESPs sit on the same 2.4 GHz radio sharing time with BLE.
static const uint8_t ESPNOW_BROADCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void OnEspNowRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
  if (len != (int)sizeof(BikeFrame)) return;
  BikeFrame frame;
  memcpy(&frame, data, sizeof(BikeFrame));
  frame.millis_received = millis();
  xQueueSend(g_frameQueue, &frame, 0);
  g_last_notification_ms = millis();
  g_notify_count++;
}

static void setup_espnow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) {
    Serial.println("[espnow] init FAILED");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, ESPNOW_BROADCAST_ADDR, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  if (IS_SPEED) {
    esp_now_register_recv_cb(OnEspNowRecv);
  }
  Serial.printf("[espnow] init OK (%s)\n", IS_POWER ? "broadcasting" : "receiving");
}

static void notifyCallback(NimBLERemoteCharacteristic* /*chr*/, uint8_t* pData,
                           size_t len, bool /*isNotify*/) {
  BikeFrame frame{};
  if (parse_indoor_bike_data(pData, len, frame)) {
    frame.millis_received = millis();
    xQueueSend(g_frameQueue, &frame, 0);
    if (IS_POWER) {
      // Relay to SPEED ESP. Best-effort; we don't care if any single send drops.
      esp_now_send(ESPNOW_BROADCAST_ADDR, (const uint8_t*)&frame, sizeof(frame));
    }
  }
  g_last_notification_ms = millis();
  g_notify_count++;
}

// ---- helpers ----
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
  Serial.printf("[bike] scanning...\n");
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
  // CPS Measurement, flags 0x0020 (crank-rev only). Garmin watches read power
  // and cadence from CPS; speed comes from CSC. Sending wheel-rev here too
  // triggers Garmin's documented duplicate-counting bug → 2x speed.
  // 0-1 flags, 2-3 inst power (sint16), 4-5 crank revs (uint16),
  // 6-7 last crank event time (uint16, 1/1024 s).
  int16_t pwr = (int16_t)(g_inst_power_w * POWER_SCALE);
  uint16_t flags = 0x0020;
  uint8_t buf[8];
  buf[0] = flags & 0xff;
  buf[1] = (flags >> 8) & 0xff;
  buf[2] = pwr & 0xff;
  buf[3] = (pwr >> 8) & 0xff;
  buf[4] = g_cumulative_crank_revs        & 0xff;
  buf[5] = (g_cumulative_crank_revs >> 8)  & 0xff;
  buf[6] = g_last_crank_event_time_1024        & 0xff;
  buf[7] = (g_last_crank_event_time_1024 >> 8) & 0xff;
  g_cpsMeasurement->setValue(buf, 8);
  g_cpsMeasurement->notify();
}

static void publish_csc_frame() {
  // CSC Measurement, flags 0x01 (wheel-rev only). 7-byte frame:
  // 0 flags, 1-4 cumulative wheel revs (uint32), 5-6 last wheel event time (uint16, 1/1024 s).
  // Crank-rev intentionally omitted; it lives on CPS only.
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
  delay(100);
  Serial.println();
  Serial.println("=== Yesoul_BLE bridge ===");
  Serial.printf("Power scale: %.3f\n", (double)POWER_SCALE);
  Serial.printf("Wheel circumference: %.3f m\n", (double)WHEEL_CIRCUMFERENCE_M);
  if (SIMULATE_BIKE) {
    Serial.printf("[sim] BIKE SIMULATION ENABLED — speed=%.2fkmh, cadence=%uRPM, raw_power=%dW\n",
                  SIM_SPEED_CMPS / 100.0, SIM_CADENCE_HALFRPM / 2, (int)SIM_INST_POWER_W);
  }

  g_frameQueue = xQueueCreate(8, sizeof(BikeFrame));

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Bonding intentionally OFF. Tried bonding (setSecurityAuth(true, false, false))
  // but stale-bond mismatch was preventing the Garmin from re-pairing after the
  // user removed the sensor on its side. With no bond, every reconnect is a
  // fresh pair — slightly noisier UX but reliable. Re-enable later if needed.
  Serial.printf("[srv] %d bonds in NVS, clearing\n", NimBLEDevice::getNumBonds());
  NimBLEDevice::deleteAllBonds();

  g_pServer = NimBLEDevice::createServer();
  g_pServer->setCallbacks(new ServerCallbacks());

  uint8_t loc[1] = { 0x0D };  // Rear Hub.

  if (IS_POWER) {
    // ---- CPS service (this ESP plays the role of a power meter) ----
    NimBLEService* cps = g_pServer->createService(CPS_SERVICE_UUID);

    NimBLECharacteristic* cpsFeature = cps->createCharacteristic(CPS_FEATURE_UUID, NIMBLE_PROPERTY::READ);
    // CPS Feature, little-endian uint32. Bit 3 = "Crank Revolution Data Supported".
    uint8_t cpsFeatureValue[4] = { 0x08, 0x00, 0x00, 0x00 };
    cpsFeature->setValue(cpsFeatureValue, 4);

    NimBLECharacteristic* cpsLoc = cps->createCharacteristic(CPS_SENSOR_LOCATION_UUID, NIMBLE_PROPERTY::READ);
    cpsLoc->setValue(loc, 1);

    g_cpsMeasurement = cps->createCharacteristic(CPS_MEASUREMENT_UUID,
                                                 NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    uint8_t initCps[8] = {0};
    g_cpsMeasurement->setValue(initCps, 8);
  } else {
    // ---- CSC service (this ESP plays the role of a speed sensor) ----
    NimBLEService* csc = g_pServer->createService(CSC_SERVICE_UUID);

    NimBLECharacteristic* cscFeature = csc->createCharacteristic(CSC_FEATURE_UUID, NIMBLE_PROPERTY::READ);
    // CSC Feature, little-endian uint16. Bit 0 = "Wheel Revolution Data Supported" only.
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
  }

  // Register all services with the GATT database. Required in NimBLE 2.x —
  // without this, services may advertise but not be discoverable on connect.
  g_pServer->start();

  // ---- Advertising ----
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(IS_POWER ? CPS_SERVICE_UUID : CSC_SERVICE_UUID);
  adv->setAppearance(DEVICE_APPEARANCE);
  adv->setName(DEVICE_NAME);
  adv->enableScanResponse(false);
  adv->start();
  Serial.printf("[srv] advertising as %s (role=%s)\n", DEVICE_NAME, IS_POWER ? "POWER/CPS" : "SPEED/CSC");

  // ---- Bike-side scan setup ----
  // POWER ESP owns the BLE central role to the bike. SPEED ESP doesn't connect
  // to the bike at all — it receives parsed BikeFrames via ESP-NOW relay.
  if (IS_POWER && !SIMULATE_BIKE) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scanCallbacks, false);
    scan->setInterval(1349);
    scan->setWindow(449);
    scan->setActiveScan(true);
    start_scan();
  } else {
    g_state = S_STREAMING;
  }

  // ESP-NOW: POWER broadcasts BikeFrames; SPEED listens. Must come AFTER
  // NimBLE init since both share the 2.4 GHz radio and ESP-NOW needs the
  // WiFi PHY brought up.
  setup_espnow();
}

void loop() {
  // 0. Bike simulation overlay: inject constant values and keep state forced.
  if (SIMULATE_BIKE) {
    g_speed_cmps           = SIM_SPEED_CMPS;
    g_cadence_halfrpm      = SIM_CADENCE_HALFRPM;
    g_inst_power_w         = SIM_INST_POWER_W;
    g_last_notification_ms = millis();
    g_state                = S_STREAMING;
  }

  // 1. Drain bike frames into derived state.
  BikeFrame frame;
  while (xQueueReceive(g_frameQueue, &frame, 0) == pdTRUE) {
    if (frame.has_speed)      g_speed_cmps      = frame.speed_cmps;
    if (frame.has_cadence)    g_cadence_halfrpm = frame.cadence_halfrpm;
    if (frame.has_inst_power) g_inst_power_w    = frame.inst_power_w;
    if (g_state == S_CONNECTED) g_state = S_STREAMING;
  }

  // 2-4. Bike-side state machine — POWER role only.
  if (IS_POWER && !SIMULATE_BIKE) {
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

  // 5. Publish CPS + CSC at 1 Hz when at least one Garmin client is connected.
  uint32_t now = millis();
  if (now - g_last_publish_ms >= 1000) {
    g_last_publish_ms = now;
    static uint32_t last_logged_notifies = 0;
    uint32_t notifies = g_notify_count;
    Serial.printf("[1Hz] state=%d notifies=%lu (+%lu) speed=%.2fkmh cad=%uRPM pwr=%dW garmin=%d\n",
                  (int)g_state, (unsigned long)notifies,
                  (unsigned long)(notifies - last_logged_notifies),
                  g_speed_cmps / 100.0, g_cadence_halfrpm / 2,
                  (int)g_inst_power_w, (int)g_pServer->getConnectedCount());
    last_logged_notifies = notifies;
    if (g_pServer->getConnectedCount() > 0) {
      update_revolution_counters();
      if (IS_POWER) publish_cps_frame();
      else          publish_csc_frame();
    }
  }

  delay(10);
}
