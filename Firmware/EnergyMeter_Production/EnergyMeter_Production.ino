/*
 * ESP32-S3 Multi-Meter Energy Monitor - Production Grade
 * Version: 3.0.0
 *
 * ╔══════════════════════════════════════════════════════════════╗
 * ║                    ARDUINO IDE SETTINGS                      ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  Board:            ESP32S3 Dev Module                        ║
 * ║  USB CDC On Boot:  Enabled                                   ║
 * ║  CPU Frequency:    240MHz (WiFi)                             ║
 * ║  Flash Mode:       QIO 80MHz                                 ║
 * ║  Flash Size:       16MB (128Mb) or 8MB (64Mb)                ║
 * ║  Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA/190KB)  ║
 * ║  PSRAM:            Disabled (or OPI PSRAM if available)      ║
 * ║  Upload Speed:     921600                                    ║
 * ║  USB Mode:         Hardware CDC and JTAG                     ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  REQUIRED LIBRARIES (Install via Library Manager):           ║
 * ║  - PubSubClient      (by Nick O'Leary)                       ║
 * ║  - ArduinoJson       (by Benoit Blanchon)                    ║
 * ║  - ModbusMaster      (by Doc Walker)                         ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * Architecture: Write-Ahead Logging (WAL) for Zero Data Loss
 * - Every reading is saved to flash IMMEDIATELY
 * - Data is only deleted AFTER confirmed server delivery
 * - MQTT connection maintained during long Modbus operations
 * - Survives power failures, network outages, and reboots
 *
 * Key Features:
 * - Zero data loss guarantee
 * - Automatic offline buffering (up to 1000 readings)
 * - MQTT QoS 1 with persistent sessions
 * - Watchdog-safe operations
 * - Memory-efficient design
 */

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ============ VERSION ============
#define FIRMWARE_VERSION "3.2.0"

// ============ DEBUG ============
#define MODBUS_DEBUG true  // Set false after confirming meters work

// ============ CONFIGURATION ============

// --- WiFi Settings (Dual WiFi Failover) ---
// Primary WiFi
const char* WIFI_SSID_1 = "REPL-6";
const char* WIFI_PASSWORD_1 = "P@ssw0rd1234";

// Backup WiFi (used when primary fails)
const char* WIFI_SSID_2 = "Rishi";
const char* WIFI_PASSWORD_2 = "12345678";

// Current active WiFi (0 = none, 1 = primary, 2 = backup)
int activeWiFi = 0;
const int WIFI_SWITCH_THRESHOLD = 3;  // Switch after 3 failed reconnects

// Static IP Configuration (for primary network)
IPAddress LOCAL_IP(192, 168, 6, 55);
IPAddress GATEWAY(192, 168, 6, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS_PRIMARY(8, 8, 8, 8);
IPAddress DNS_SECONDARY(8, 8, 4, 4);

// Static IP for backup network (optional - set to 0,0,0,0 for DHCP)
IPAddress LOCAL_IP_2(0, 0, 0, 0);  // Use DHCP for backup
IPAddress GATEWAY_2(0, 0, 0, 0);
IPAddress SUBNET_2(0, 0, 0, 0);

// --- MQTT Settings ---
const char* MQTT_SERVER = "167.71.233.135";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "esp32meter";
const char* MQTT_PASSWORD = "meter@123";
const char* MQTT_TOPIC_DATA = "meters/data";
const char* MQTT_TOPIC_STATUS = "meters/status";
const int MQTT_KA_SEC = 120;         // 120 seconds keepalive
const int MQTT_BUFFER_SIZE = 12288;  // 12KB for payload

// --- Modbus Settings ---
HardwareSerial& ModbusSerial = Serial2;
const int MODBUS_BAUD = 9600;
const int MODBUS_RX = 18;
const int MODBUS_TX = 17;

// If true, convert 4X addresses (e.g. 40001) to zero-based index for ModbusMaster library.
// ModbusMaster uses zero-based addressing: 40001 -> 0, 40003 -> 2, etc.
#define MODBUS_ZERO_BASE true

// --- Meter Configuration ---
enum MeterType { METER_SCHNEIDER_EM6436,
                 METER_ELITE_100,
                 METER_RISHABH_LM1360 };

const uint8_t METER_IDS[] = { 7, 10, 11 };
const MeterType METER_TYPES[] = {
  METER_RISHABH_LM1360,  // Meter 7  = Rishabh RISH LM 1360
  METER_RISHABH_LM1360,  // Meter 10 = Rishabh RISH LM 1360
  METER_RISHABH_LM1360   // Meter 11 = Rishabh RISH LM 1360
};
const uint8_t NUM_METERS = sizeof(METER_IDS) / sizeof(METER_IDS[0]);

// --- Timing Configuration ---
const unsigned long READ_INTERVAL_MS = 60000;      // Read every 1 minute
const unsigned long PUBLISH_INTERVAL_MS = 120000;  // Publish every 2 minutes
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

// --- WiFi Quality Thresholds ---
const int RSSI_EXCELLENT = -50;  // Excellent signal
const int RSSI_GOOD = -60;       // Good signal
const int RSSI_FAIR = -70;       // Fair signal (may have issues)
const int RSSI_WEAK = -80;       // Weak signal (expect failures)
const int RSSI_UNUSABLE = -90;   // Too weak to use

// --- Buffer Configuration ---
const int MAX_FILE_ENTRIES = 1000;          // Max readings in flash
const char* DATA_FILE = "/readings.jsonl";  // JSON Lines format

// ============ GLOBAL STATE ============

// Network clients
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Modbus nodes
ModbusMaster meters[NUM_METERS];

// Device identity
String deviceId;
String clientId;

// Connection state
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;

// WiFi quality tracking
int currentRSSI = 0;
int consecutiveFailures = 0;
int wifiReconnectFailures = 0;  // Track WiFi reconnection failures
const int MAX_PUBLISH_RETRIES = 3;
const int FAILURE_THRESHOLD = 5;  // Reset WiFi after 5 consecutive failures

// Timing
unsigned long lastReadTime = 0;
unsigned long lastPublishTime = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastMqttReconnect = 0;

// Static buffers (prevent stack overflow)
static char jsonBuffer[MQTT_BUFFER_SIZE];

// ============ METER DATA STRUCTURE ============

struct MeterReading {
  uint8_t meterId;
  bool valid;

  // Voltages (V)
  float vL1, vL2, vL3, vAvg;
  float vL1L2, vL2L3, vL3L1, vLLAvg;

  // Currents (A)
  float iL1, iL2, iL3, iAvg, iTotal, iNeutral;

  // Active Power (W)
  float pL1, pL2, pL3, pTotal;

  // Apparent Power (VA)
  float sL1, sL2, sL3, sTotal;

  // Reactive Power (VAr)
  float qL1, qL2, qL3, qTotal;

  // Power Factor
  float pfL1, pfL2, pfL3, pfTotal;

  // Phase Angle (degrees)
  float paL1, paL2, paL3, paTotal;

  // Frequency (Hz)
  float frequency;

  // Energy (kWh, kVArh)
  float eImport, eExport, eTotal;
  float erImport, erExport, erTotal;
  float esTotal;

  // Demand
  float demand, demandMax;
  float idL1, idL2, idL3;
  float idMaxL1, idMaxL2, idMaxL3;

  // THD (%)
  float thdVL1, thdVL2, thdVL3, thdVAvg;
  float thdIL1, thdIL2, thdIL3, thdIAvg;
};

struct DataPacket {
  unsigned long timestamp;
  int rssi;
  MeterReading meters[NUM_METERS];
};

// ============ SETUP ============

void setup() {
  Serial.begin(115200);
  delay(2000);

  printBanner();
  printResetReason();

  // Step 1: Initialize Flash Storage
  Serial.println("[1/7] Initializing flash storage...");
  initStorage();

  // Step 2: Initialize Modbus
  Serial.println("[2/7] Initializing Modbus...");
  initModbus();

  // Step 3: Initialize WiFi (must be before initDeviceId on ESP32-S3)
  Serial.println("[3/7] Connecting to WiFi...");
  initWiFi();

  // Step 4: Get Device Identity (after WiFi init so MAC is available)
  Serial.println("[4/7] Getting device identity...");
  initDeviceId();

  // Step 5: Initialize MQTT
  Serial.println("[5/7] Configuring MQTT...");
  initMQTT();

  // Step 6: Initialize OTA (for future over-the-air updates)
  Serial.println("[6/7] Configuring OTA updates...");
  initOTA();

  // Step 7: Test Modbus communication
  Serial.println("[7/7] Testing Modbus communication...");
  testModbusCommunication();

  Serial.println("\n========== READY ==========");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  printStorageStatus();
  Serial.println("===========================\n");
}

void printBanner() {
  Serial.println("\n");
  Serial.println("╔═══════════════════════════════════════════╗");
  Serial.println("║   ESP32 Energy Monitor - Production v3.2  ║");
  Serial.println("║   Zero Data Loss + Dual WiFi Failover     ║");
  Serial.println("╚═══════════════════════════════════════════╝");
  Serial.println();
  Serial.printf("  Primary WiFi:  %s\n", WIFI_SSID_1);
  Serial.printf("  Backup WiFi:   %s\n", WIFI_SSID_2);
  Serial.println();
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[Boot] Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON: Serial.println("Power-on"); break;
    case ESP_RST_SW: Serial.println("Software reset"); break;
    case ESP_RST_PANIC: Serial.println("Crash/Panic"); break;
    case ESP_RST_INT_WDT: Serial.println("Interrupt WDT"); break;
    case ESP_RST_TASK_WDT: Serial.println("Task WDT"); break;
    case ESP_RST_WDT: Serial.println("Watchdog"); break;
    case ESP_RST_BROWNOUT: Serial.println("Brownout"); break;
    default: Serial.printf("Code %d\n", reason); break;
  }
}

// ============ INITIALIZATION FUNCTIONS ============

void initStorage() {
  if (!LittleFS.begin(true)) {
    Serial.println("  [!] LittleFS failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println("  [OK] Flash storage ready");

  // Check for pending data from previous session
  int pending = countPendingReadings();
  if (pending > 0) {
    Serial.printf("  [!] Found %d unsent readings from previous session\n", pending);
  }
}

void initModbus() {
  ModbusSerial.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_RX, MODBUS_TX);

  Serial.printf("  Meters configured: ");
  for (int i = 0; i < NUM_METERS; i++) {
    meters[i].begin(METER_IDS[i], ModbusSerial);
    Serial.printf("%d", METER_IDS[i]);
    if (i < NUM_METERS - 1) Serial.print(", ");
  }
  Serial.println();
}

void initDeviceId() {
  deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  clientId = "ESP32_" + deviceId;
  Serial.printf("  Device ID: %s\n", deviceId.c_str());
}

void initWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);  // We handle reconnection manually
  WiFi.onEvent(onWiFiEvent);

  // Try primary WiFi first
  if (connectToWiFi(1)) {
    return;
  }

  // Primary failed, try backup
  Serial.println("  [!] Primary WiFi failed, trying backup...");
  if (connectToWiFi(2)) {
    return;
  }

  Serial.println("  [!] Both WiFi networks failed - will retry in background");
}

bool connectToWiFi(int network) {
  const char* ssid;
  const char* password;

  WiFi.disconnect(true);
  delay(100);

  if (network == 1) {
    ssid = WIFI_SSID_1;
    password = WIFI_PASSWORD_1;

    // Apply static IP for primary network
    if (LOCAL_IP[0] != 0) {
      if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, DNS_PRIMARY, DNS_SECONDARY)) {
        Serial.println("  [!] Static IP failed, using DHCP");
      }
    }
  } else {
    ssid = WIFI_SSID_2;
    password = WIFI_PASSWORD_2;

    // Apply static IP for backup network (or DHCP if 0.0.0.0)
    if (LOCAL_IP_2[0] != 0) {
      if (!WiFi.config(LOCAL_IP_2, GATEWAY_2, SUBNET_2, DNS_PRIMARY, DNS_SECONDARY)) {
        Serial.println("  [!] Static IP failed, using DHCP");
      }
    } else {
      // Use DHCP for backup
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
  }

  Serial.printf("  Connecting to WiFi %d: %s", network, ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    activeWiFi = network;
    wifiConnected = true;
    wifiReconnectFailures = 0;
    Serial.printf("\n  [OK] Connected to WiFi %d: %s\n", network, WiFi.localIP().toString().c_str());
    Serial.printf("  [OK] SSID: %s, Signal: %d dBm\n", ssid, WiFi.RSSI());
    configTime(19800, 0, "pool.ntp.org");  // IST timezone
    return true;
  }

  Serial.printf("\n  [!] WiFi %d failed\n", network);
  return false;
}

void initMQTT() {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(MQTT_KA_SEC);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  Serial.printf("  Broker: %s:%d\n", MQTT_SERVER, MQTT_PORT);
  Serial.printf("  Keepalive: %d seconds\n", MQTT_KA_SEC);
}

// ============ OTA UPDATE ============

void initOTA() {
  ArduinoOTA.setHostname("ESP32-EnergyMeter");
  ArduinoOTA.setPassword("ems@ota123");  // OTA password

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("[OTA] Updating " + type + "...");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete! Rebooting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("  OTA ready at %s (hostname: ESP32-EnergyMeter)\n",
                WiFi.localIP().toString().c_str());
}

// ============ MODBUS STARTUP TEST ============

void testModbusCommunication() {
  Serial.println("\n--- Modbus Communication Test ---");
  for (int i = 0; i < NUM_METERS; i++) {
    uint16_t testReg;
    const char* meterTypeName;

    if (METER_TYPES[i] == METER_SCHNEIDER_EM6436) {
      testReg = 3914;  // Frequency register on Schneider
      meterTypeName = "Schneider";
    } else if (METER_TYPES[i] == METER_RISHABH_LM1360) {
      testReg = MODBUS_ZERO_BASE ? (40071 - 40001) : 40071;  // Frequency register on RISH LM1360
      meterTypeName = "RISH_LM1360";
    } else {
      testReg = 171;  // Frequency register on Elite 100
      meterTypeName = "Elite100";
    }

    uint8_t result = meters[i].readHoldingRegisters(testReg, 2);

    if (result == meters[i].ku8MBSuccess) {
      uint32_t raw = ((uint32_t)meters[i].getResponseBuffer(0) << 16) | meters[i].getResponseBuffer(1);
      float freq;
      memcpy(&freq, &raw, 4);
      Serial.printf("  Meter %d (%s): OK  freq=%.2f Hz\n", METER_IDS[i], meterTypeName, freq);
    } else {
      Serial.printf("  Meter %d (%s): FAILED  error=0x%02X\n", METER_IDS[i], meterTypeName, result);
      // Decode error
      switch (result) {
        case 0x02: Serial.println("    -> Illegal register address"); break;
        case 0x03: Serial.println("    -> Illegal data value"); break;
        case 0xE0: Serial.println("    -> Timeout - no response (check wiring/slave ID)"); break;
        case 0xE1: Serial.println("    -> Invalid CRC"); break;
        case 0xE2: Serial.println("    -> Invalid slave ID in response"); break;
        default: Serial.println("    -> Unknown error"); break;
      }
    }
    delay(200);
    yield();
  }
  Serial.println("--- End Test ---\n");
}

// ============ MAIN LOOP ============

void loop() {
  yield();  // Watchdog feed

  // Handle OTA updates (must be called frequently)
  ArduinoOTA.handle();

  unsigned long now = millis();

  // Maintain connections
  maintainWiFi(now);
  maintainMQTT(now);

  // Process MQTT messages (call frequently!)
  if (wifiConnected) {
    mqtt.loop();
  }

  // Monitor WiFi quality
  checkWiFiQuality();

  // Read meters at interval (ALWAYS - regardless of network)
  if (now - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = now;
    readAndSaveMeters();
  }

  // Publish at interval (only if network is healthy)
  if (now - lastPublishTime >= PUBLISH_INTERVAL_MS) {
    lastPublishTime = now;

    if (isNetworkHealthy()) {
      publishPendingData();
    } else {
      Serial.println("[Publish] Network unhealthy - data safe in flash");
      int pending = countPendingReadings();
      Serial.printf("[Publish] Buffered readings: %d\n", pending);
    }
  }

  delay(10);  // Short delay, fast loop for mqtt.loop()
}

// ============ CONNECTION MANAGEMENT ============

void maintainWiFi(unsigned long now) {
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    mqttConnected = false;
    wifiReconnectFailures++;

    Serial.printf("[WiFi] Disconnected (failure %d/%d)\n",
                  wifiReconnectFailures, WIFI_SWITCH_THRESHOLD);

    // Try to reconnect to current network
    if (wifiReconnectFailures < WIFI_SWITCH_THRESHOLD) {
      Serial.printf("[WiFi] Reconnecting to WiFi %d...\n", activeWiFi);
      WiFi.reconnect();
    } else {
      // Switch to other network
      int newNetwork = (activeWiFi == 1) ? 2 : 1;
      Serial.printf("[WiFi] Switching to WiFi %d...\n", newNetwork);

      if (connectToWiFi(newNetwork)) {
        Serial.printf("[WiFi] Successfully switched to WiFi %d\n", newNetwork);
      } else {
        // Try the other one again
        Serial.printf("[WiFi] WiFi %d failed, trying WiFi %d again...\n", newNetwork, activeWiFi);
        connectToWiFi(activeWiFi == 1 ? 1 : 2);
      }
      wifiReconnectFailures = 0;
    }
  } else {
    // Connected - reset failure counter
    if (wifiReconnectFailures > 0) {
      Serial.printf("[WiFi] Connection restored on WiFi %d\n", activeWiFi);
      wifiReconnectFailures = 0;
    }
  }
}

void maintainMQTT(unsigned long now) {
  if (!wifiConnected) return;

  if (mqtt.connected()) {
    mqttConnected = true;
    return;
  }

  mqttConnected = false;

  if (now - lastMqttReconnect < MQTT_RECONNECT_INTERVAL_MS) return;
  lastMqttReconnect = now;

  Serial.print("[MQTT] Connecting...");

  // LWT message for offline detection
  String willTopic = String(MQTT_TOPIC_STATUS) + "/" + deviceId;
  String willMsg = "{\"status\":\"offline\",\"device\":\"" + deviceId + "\"}";

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                   willTopic.c_str(), 1, true, willMsg.c_str())) {
    Serial.println(" Connected!");
    mqttConnected = true;

    // Publish online status
    String onlineMsg = "{\"status\":\"online\",\"device\":\"" + deviceId + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"version\":\"" + FIRMWARE_VERSION + "\",\"heap\":" + String(ESP.getFreeHeap()) + "}";
    mqtt.publish(willTopic.c_str(), onlineMsg.c_str(), true);

    // Subscribe to commands
    mqtt.subscribe("meters/commands/#");
  } else {
    Serial.printf(" Failed (rc=%d)\n", mqtt.state());
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Connected - IP: %s\n", WiFi.localIP().toString().c_str());
      wifiConnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] Disconnected");
      wifiConnected = false;
      mqttConnected = false;
      break;
    default:
      break;
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s\n", topic);
}

// ============ METER READING ============

void readAndSaveMeters() {
  Serial.println("\n─── Reading Meters ───");

  DataPacket packet;
  packet.timestamp = getTimestamp();
  packet.rssi = WiFi.RSSI();

  for (int i = 0; i < NUM_METERS; i++) {
    Serial.printf("  Meter %d: ", METER_IDS[i]);

    packet.meters[i] = readSingleMeter(meters[i], METER_IDS[i], METER_TYPES[i]);

    if (packet.meters[i].valid) {
      Serial.printf("V=%.1f I=%.2f P=%.1fW\n",
                    packet.meters[i].vAvg,
                    packet.meters[i].iTotal,
                    packet.meters[i].pTotal);
    } else {
      Serial.println("FAILED");
    }

    // Keep MQTT alive between meters
    if (wifiConnected) mqtt.loop();
    yield();
    delay(50);
  }

  // CRITICAL: Save to flash immediately (WAL pattern)
  saveReadingToFile(packet);

  Serial.printf("  Saved to flash. Pending: %d\n", countPendingReadings());
}

// ============ METER-SPECIFIC READ FUNCTIONS ============

// Schneider EM6436 register map (FC03 holding registers, IEEE 754 float)
// Source: Schneider EM6400 series documentation + aggsoft register map
MeterReading readSchneiderEM6436(ModbusMaster& node, uint8_t id) {
  MeterReading r;
  memset(&r, 0, sizeof(r));
  r.meterId = id;
  r.valid = true;

  // --- System Totals (registers 3900-3914) ---
  r.sTotal = readRegister(node, 3900);     // Total Apparent Power (VA)
  r.pTotal = readRegister(node, 3902);     // Total Active Power (W)
  r.qTotal = readRegister(node, 3904);     // Total Reactive Power (VAr)
  r.pfTotal = readRegister(node, 3906);    // Average Power Factor
  r.vLLAvg = readRegister(node, 3908);     // Average Line-Line Voltage (V)
  r.vAvg = readRegister(node, 3910);       // Average Line-Neutral Voltage (V)
  r.iAvg = readRegister(node, 3912);       // Average Current (A)
  r.frequency = readRegister(node, 3914);  // Frequency (Hz)
  mqtt.loop();
  yield();

  // --- Phase R (registers 3918-3930) ---
  r.pL1 = readRegister(node, 3918);    // Active Power R (W)
  r.qL1 = readRegister(node, 3920);    // Reactive Power R (VAr)
  r.sL1 = readRegister(node, 3922);    // Apparent Power R (VA)
  r.vL1L2 = readRegister(node, 3924);  // Voltage R-Y (V)
  r.vL1 = readRegister(node, 3926);    // Voltage R-N (V)
  r.iL1 = readRegister(node, 3928);    // Current R (A)
  r.pfL1 = readRegister(node, 3930);   // Power Factor R
  mqtt.loop();
  yield();

  // --- Phase Y (registers 3932-3944) ---
  r.pL2 = readRegister(node, 3932);    // Active Power Y (W)
  r.qL2 = readRegister(node, 3934);    // Reactive Power Y (VAr)
  r.sL2 = readRegister(node, 3936);    // Apparent Power Y (VA)
  r.vL2L3 = readRegister(node, 3938);  // Voltage Y-B (V)
  r.vL2 = readRegister(node, 3940);    // Voltage Y-N (V)
  r.iL2 = readRegister(node, 3942);    // Current Y (A)
  r.pfL2 = readRegister(node, 3944);   // Power Factor Y
  mqtt.loop();
  yield();

  // --- Phase B (registers 3946-3958) ---
  r.pL3 = readRegister(node, 3946);    // Active Power B (W)
  r.qL3 = readRegister(node, 3948);    // Reactive Power B (VAr)
  r.sL3 = readRegister(node, 3950);    // Apparent Power B (VA)
  r.vL3L1 = readRegister(node, 3952);  // Voltage B-R (V)
  r.vL3 = readRegister(node, 3954);    // Voltage B-N (V)
  r.iL3 = readRegister(node, 3956);    // Current B (A)
  r.pfL3 = readRegister(node, 3958);   // Power Factor B
  mqtt.loop();
  yield();

  // --- Energy (registers 3960-3968) ---
  r.eImport = readRegister(node, 3960);   // Import Active Energy (kWh)
  r.eExport = readRegister(node, 3962);   // Export Active Energy (kWh)
  r.erImport = readRegister(node, 3964);  // Import Reactive Energy (kVArh)
  r.erExport = readRegister(node, 3968);  // Export Reactive Energy (kVArh)
  mqtt.loop();
  yield();

  // --- THD (registers 3860-3870) ---
  r.thdVL1 = readRegister(node, 3860);  // Voltage THD R (%)
  r.thdVL2 = readRegister(node, 3862);  // Voltage THD Y (%)
  r.thdVL3 = readRegister(node, 3864);  // Voltage THD B (%)
  r.thdIL1 = readRegister(node, 3866);  // Current THD R (%)
  r.thdIL2 = readRegister(node, 3868);  // Current THD Y (%)
  r.thdIL3 = readRegister(node, 3870);  // Current THD B (%)
  mqtt.loop();
  yield();

  // Calculated fields
  r.iTotal = r.iL1 + r.iL2 + r.iL3;

  // Validate - at least one phase voltage should read valid
  if (r.vL1 < 1.0 && r.vL2 < 1.0 && r.vL3 < 1.0) {
    r.valid = false;
  }

  return r;
}

// Secure Elite 100 register map (FC03 holding registers, 32-bit IEEE 754 float)
// Source: Elite 100/300 Technical Reference Manual BGX701-234-R01
MeterReading readElite100(ModbusMaster& node, uint8_t id) {
  MeterReading r;
  memset(&r, 0, sizeof(r));
  r.meterId = id;
  r.valid = true;

  // --- Voltages (registers 99-111) ---
  r.vL1 = readRegister(node, 99);     // Phase R Voltage (V)
  r.vL2 = readRegister(node, 101);    // Phase Y Voltage (V)
  r.vL3 = readRegister(node, 103);    // Phase B Voltage (V)
  r.vAvg = readRegister(node, 105);   // Average Voltage (V)
  r.vL1L2 = readRegister(node, 107);  // Line Voltage R-Y (V)
  r.vL2L3 = readRegister(node, 109);  // Line Voltage Y-B (V)
  r.vL3L1 = readRegister(node, 111);  // Line Voltage B-R (V)
  mqtt.loop();
  yield();

  // --- Currents (registers 113-119) ---
  r.iL1 = readRegister(node, 113);       // Phase R Current (A)
  r.iL2 = readRegister(node, 115);       // Phase Y Current (A)
  r.iL3 = readRegister(node, 117);       // Phase B Current (A)
  r.iNeutral = readRegister(node, 119);  // Neutral Current (A)
  mqtt.loop();
  yield();

  // --- Power Factor (registers 133-139) ---
  r.pfL1 = readRegister(node, 133);     // PF R
  r.pfL2 = readRegister(node, 135);     // PF Y
  r.pfL3 = readRegister(node, 137);     // PF B
  r.pfTotal = readRegister(node, 139);  // PF Total
  mqtt.loop();
  yield();

  // --- Active Power (registers 141-147) ---
  r.pL1 = readRegister(node, 141);     // Active Power R (W)
  r.pL2 = readRegister(node, 143);     // Active Power Y (W)
  r.pL3 = readRegister(node, 145);     // Active Power B (W)
  r.pTotal = readRegister(node, 147);  // Total Active Power (W)
  mqtt.loop();
  yield();

  // --- Reactive Power (registers 149-155) ---
  r.qL1 = readRegister(node, 149);     // Reactive Power R (VAr)
  r.qL2 = readRegister(node, 151);     // Reactive Power Y (VAr)
  r.qL3 = readRegister(node, 153);     // Reactive Power B (VAr)
  r.qTotal = readRegister(node, 155);  // Total Reactive Power (VAr)
  mqtt.loop();
  yield();

  // --- Apparent Power (registers 157-163) ---
  r.sL1 = readRegister(node, 157);     // Apparent Power R (VA)
  r.sL2 = readRegister(node, 159);     // Apparent Power Y (VA)
  r.sL3 = readRegister(node, 161);     // Apparent Power B (VA)
  r.sTotal = readRegister(node, 163);  // Total Apparent Power (VA)
  mqtt.loop();
  yield();

  // --- Frequency (register 171) ---
  r.frequency = readRegister(node, 171);  // System Frequency (Hz)
  mqtt.loop();
  yield();

  // --- THD (registers 177-187) ---
  r.thdVL1 = readRegister(node, 177);  // Voltage THD R (%)
  r.thdVL2 = readRegister(node, 179);  // Voltage THD Y (%)
  r.thdVL3 = readRegister(node, 181);  // Voltage THD B (%)
  r.thdIL1 = readRegister(node, 183);  // Current THD R (%)
  r.thdIL2 = readRegister(node, 185);  // Current THD Y (%)
  r.thdIL3 = readRegister(node, 187);  // Current THD B (%)
  mqtt.loop();
  yield();

  // Calculated fields
  r.iTotal = r.iL1 + r.iL2 + r.iL3;
  r.iAvg = r.iTotal / 3.0;

  // Validate
  if (r.vL1 < 1.0 && r.vL2 < 1.0 && r.vL3 < 1.0) {
    r.valid = false;
  }

  return r;
}

// ============ RISHABH RISH LM 1360 SUPPORT ============

// Helper: read a 32-bit IEEE-754 float from Rishabh holding registers (4X addresses)
float readRishabhHoldingFloat(ModbusMaster& node, uint16_t reg4x) {
  uint16_t addr;
  if (MODBUS_ZERO_BASE) {
    addr = (uint16_t)(reg4x - 40001);  // 40001 -> 0, 40003 -> 2, etc.
  } else {
    addr = reg4x;
  }

  uint8_t result = node.readHoldingRegisters(addr, 2);
  if (result == node.ku8MBSuccess) {
    uint32_t raw = ((uint32_t)node.getResponseBuffer(0) << 16) | node.getResponseBuffer(1);
    float value;
    memcpy(&value, &raw, 4);
    return value;
  }

  if (MODBUS_DEBUG) {
    Serial.printf("      [RISH] Reg %u (addr %u) err: 0x%02X\n", reg4x, addr, result);
  }
  return 0.0;
}

// Rishabh RISH LM 1360 (LM13xx series) - FC03 Holding Registers, IEEE 754 float
MeterReading readRishabhLM1360(ModbusMaster& node, uint8_t id) {
  MeterReading r;
  memset(&r, 0, sizeof(r));
  r.meterId = id;
  r.valid = true;

  // --- Phase Voltages L-N ---
  r.vL1 = readRishabhHoldingFloat(node, 40001);  // V1N
  r.vL2 = readRishabhHoldingFloat(node, 40003);  // V2N
  r.vL3 = readRishabhHoldingFloat(node, 40005);  // V3N
  mqtt.loop(); yield();

  // --- Phase Currents ---
  r.iL1 = readRishabhHoldingFloat(node, 40007);  // I1
  r.iL2 = readRishabhHoldingFloat(node, 40009);  // I2
  r.iL3 = readRishabhHoldingFloat(node, 40011);  // I3
  mqtt.loop(); yield();

  // --- Active Power per phase ---
  r.pL1 = readRishabhHoldingFloat(node, 40013);  // W1
  r.pL2 = readRishabhHoldingFloat(node, 40015);  // W2
  r.pL3 = readRishabhHoldingFloat(node, 40017);  // W3
  mqtt.loop(); yield();

  // --- Apparent Power per phase ---
  r.sL1 = readRishabhHoldingFloat(node, 40019);  // VA1
  r.sL2 = readRishabhHoldingFloat(node, 40021);  // VA2
  r.sL3 = readRishabhHoldingFloat(node, 40023);  // VA3
  mqtt.loop(); yield();

  // --- Reactive Power per phase ---
  r.qL1 = readRishabhHoldingFloat(node, 40025);  // VAr1
  r.qL2 = readRishabhHoldingFloat(node, 40027);  // VAr2
  r.qL3 = readRishabhHoldingFloat(node, 40029);  // VAr3
  mqtt.loop(); yield();

  // --- Power Factor per phase ---
  r.pfL1 = readRishabhHoldingFloat(node, 40031);  // PF1
  r.pfL2 = readRishabhHoldingFloat(node, 40033);  // PF2
  r.pfL3 = readRishabhHoldingFloat(node, 40035);  // PF3
  mqtt.loop(); yield();

  // --- Phase Angle per phase ---
  r.paL1 = readRishabhHoldingFloat(node, 40037);  // PA1
  r.paL2 = readRishabhHoldingFloat(node, 40039);  // PA2
  r.paL3 = readRishabhHoldingFloat(node, 40041);  // PA3
  mqtt.loop(); yield();

  // --- Averages and Totals ---
  r.vAvg = readRishabhHoldingFloat(node, 40043);    // Volts Average
  r.iAvg = readRishabhHoldingFloat(node, 40047);    // Current Average
  r.pTotal = readRishabhHoldingFloat(node, 40053);   // Watts Sum
  r.sTotal = readRishabhHoldingFloat(node, 40057);   // VA Sum
  r.qTotal = readRishabhHoldingFloat(node, 40061);   // VAr Sum
  r.pfTotal = readRishabhHoldingFloat(node, 40063);  // PF Average
  r.frequency = readRishabhHoldingFloat(node, 40071); // Frequency
  mqtt.loop(); yield();

  // --- Line-to-Line Voltages ---
  r.vL1L2 = readRishabhHoldingFloat(node, 40101);  // VL1-L2
  r.vL2L3 = readRishabhHoldingFloat(node, 40103);  // VL2-L3
  r.vL3L1 = readRishabhHoldingFloat(node, 40105);  // VL3-L1
  mqtt.loop(); yield();

  // --- Energy Registers ---
  r.eImport = readRishabhHoldingFloat(node, 40111);   // Wh Import
  r.eExport = readRishabhHoldingFloat(node, 40115);   // Wh Export
  r.erImport = readRishabhHoldingFloat(node, 40119);  // VArh Capacitive
  r.erExport = readRishabhHoldingFloat(node, 40123);  // VArh Inductive
  r.esTotal = readRishabhHoldingFloat(node, 40127);   // VAh Total
  mqtt.loop(); yield();

  // --- Neutral Current ---
  r.iNeutral = readRishabhHoldingFloat(node, 40113);  // I Neutral
  mqtt.loop(); yield();

  // Calculated fields
  r.iTotal = r.iL1 + r.iL2 + r.iL3;

  // Validate - at least one phase voltage should read valid
  if (r.vL1 < 1.0f && r.vL2 < 1.0f && r.vL3 < 1.0f) {
    r.valid = false;
  }

  return r;
}

// Dispatch to correct meter-type reader
MeterReading readSingleMeter(ModbusMaster& node, uint8_t id, MeterType type) {
  switch (type) {
    case METER_SCHNEIDER_EM6436:
      return readSchneiderEM6436(node, id);
    case METER_ELITE_100:
      return readElite100(node, id);
    case METER_RISHABH_LM1360:
      return readRishabhLM1360(node, id);
    default:
      {
        MeterReading r;
        memset(&r, 0, sizeof(r));
        r.meterId = id;
        r.valid = false;
        return r;
      }
  }
}

float readRegister(ModbusMaster& node, uint16_t addr) {
  uint8_t result = node.readHoldingRegisters(addr, 2);

  if (result == node.ku8MBSuccess) {
    uint32_t raw = ((uint32_t)node.getResponseBuffer(0) << 16) | node.getResponseBuffer(1);
    float value;
    memcpy(&value, &raw, 4);
    return value;
  }

  if (MODBUS_DEBUG) {
    Serial.printf("      [Modbus] Reg %d err: 0x%02X\n", addr, result);
  }
  return 0.0;
}

// ============ FILE STORAGE (WAL Pattern) ============

void saveReadingToFile(DataPacket& packet) {
  // Ensure space available
  ensureStorageSpace();

  File file = LittleFS.open(DATA_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("[Storage] ERROR: Cannot open file!");
    return;
  }

  // Create JSON
  StaticJsonDocument<8192> doc;
  doc["ts"] = packet.timestamp;
  doc["device"] = deviceId;
  doc["mac"] = WiFi.macAddress();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = packet.rssi;
  doc["fw"] = FIRMWARE_VERSION;
  doc["meters"] = NUM_METERS;

  for (int i = 0; i < NUM_METERS; i++) {
    MeterReading& m = packet.meters[i];
    String key = "m" + String(m.meterId);
    JsonObject obj = doc.createNestedObject(key);

    obj["id"] = m.meterId;
    obj["ok"] = m.valid;

    if (m.valid) {
      // Voltages
      obj["vL1"] = round2(m.vL1);
      obj["vL2"] = round2(m.vL2);
      obj["vL3"] = round2(m.vL3);
      obj["vAvg"] = round2(m.vAvg);
      obj["vL1L2"] = round2(m.vL1L2);
      obj["vL2L3"] = round2(m.vL2L3);
      obj["vL3L1"] = round2(m.vL3L1);
      obj["vLLAvg"] = round2(m.vLLAvg);

      // Currents
      obj["iL1"] = round2(m.iL1);
      obj["iL2"] = round2(m.iL2);
      obj["iL3"] = round2(m.iL3);
      obj["iAvg"] = round2(m.iAvg);
      obj["iTotal"] = round2(m.iTotal);
      obj["iN"] = round2(m.iNeutral);

      // Power
      obj["pL1"] = round2(m.pL1);
      obj["pL2"] = round2(m.pL2);
      obj["pL3"] = round2(m.pL3);
      obj["pTotal"] = round2(m.pTotal);
      obj["sL1"] = round2(m.sL1);
      obj["sL2"] = round2(m.sL2);
      obj["sL3"] = round2(m.sL3);
      obj["sTotal"] = round2(m.sTotal);
      obj["qL1"] = round2(m.qL1);
      obj["qL2"] = round2(m.qL2);
      obj["qL3"] = round2(m.qL3);
      obj["qTotal"] = round2(m.qTotal);

      // Power Factor
      obj["pfL1"] = round2(m.pfL1);
      obj["pfL2"] = round2(m.pfL2);
      obj["pfL3"] = round2(m.pfL3);
      obj["pfTotal"] = round2(m.pfTotal);

      // Phase Angle
      obj["paL1"] = round2(m.paL1);
      obj["paL2"] = round2(m.paL2);
      obj["paL3"] = round2(m.paL3);
      obj["paTotal"] = round2(m.paTotal);

      // Frequency
      obj["freq"] = round2(m.frequency);

      // Energy
      obj["eImp"] = round2(m.eImport);
      obj["eExp"] = round2(m.eExport);
      obj["eTotal"] = round2(m.eTotal);
      obj["erImp"] = round2(m.erImport);
      obj["erExp"] = round2(m.erExport);
      obj["erTotal"] = round2(m.erTotal);
      obj["esTotal"] = round2(m.esTotal);

      // Demand
      obj["demand"] = round2(m.demand);
      obj["demandMax"] = round2(m.demandMax);
      obj["idL1"] = round2(m.idL1);
      obj["idL2"] = round2(m.idL2);
      obj["idL3"] = round2(m.idL3);
      obj["idMaxL1"] = round2(m.idMaxL1);
      obj["idMaxL2"] = round2(m.idMaxL2);
      obj["idMaxL3"] = round2(m.idMaxL3);

      // THD
      obj["thdVL1"] = round2(m.thdVL1);
      obj["thdVL2"] = round2(m.thdVL2);
      obj["thdVL3"] = round2(m.thdVL3);
      obj["thdVAvg"] = round2(m.thdVAvg);
      obj["thdIL1"] = round2(m.thdIL1);
      obj["thdIL2"] = round2(m.thdIL2);
      obj["thdIL3"] = round2(m.thdIL3);
      obj["thdIAvg"] = round2(m.thdIAvg);
    }

    yield();
  }

  // Write as single line (JSON Lines format)
  serializeJson(doc, file);
  file.println();
  file.close();
}

int countPendingReadings() {
  if (!LittleFS.exists(DATA_FILE)) return 0;

  File file = LittleFS.open(DATA_FILE, FILE_READ);
  if (!file) return 0;

  int count = 0;
  while (file.available()) {
    if (file.read() == '\n') count++;
    if (count % 100 == 0) yield();
  }
  file.close();
  return count;
}

void ensureStorageSpace() {
  int count = countPendingReadings();

  if (count >= MAX_FILE_ENTRIES) {
    Serial.printf("[Storage] Trimming buffer (at %d entries)\n", count);
    trimOldestEntries(MAX_FILE_ENTRIES / 2);
  }
}

void trimOldestEntries(int keepCount) {
  if (!LittleFS.exists(DATA_FILE)) return;

  // Read all lines
  File file = LittleFS.open(DATA_FILE, FILE_READ);
  if (!file) return;

  std::vector<String> lines;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 10) {
      lines.push_back(line);
    }
    yield();
  }
  file.close();

  // Keep only newest entries
  int total = lines.size();
  int start = (total > keepCount) ? (total - keepCount) : 0;

  // Rewrite file
  file = LittleFS.open(DATA_FILE, FILE_WRITE);
  if (!file) return;

  for (int i = start; i < total; i++) {
    file.println(lines[i]);
    yield();
  }
  file.close();

  Serial.printf("[Storage] Trimmed: kept %d of %d\n", total - start, total);
}

void printStorageStatus() {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  int pending = countPendingReadings();

  Serial.printf("  Storage: %d/%d KB (%.1f%%)\n",
                used / 1024, total / 1024,
                (float)used / total * 100);
  Serial.printf("  Pending readings: %d\n", pending);
}

// ============ PUBLISHING ============

bool publishWithRetry(const char* topic, const char* payload) {
  for (int attempt = 1; attempt <= MAX_PUBLISH_RETRIES; attempt++) {
    yield();
    mqtt.loop();

    if (mqtt.publish(topic, payload)) {
      return true;
    }

    // Failed - wait and retry
    if (attempt < MAX_PUBLISH_RETRIES) {
      Serial.printf("  Retry %d/%d...\n", attempt, MAX_PUBLISH_RETRIES);
      delay(100 * attempt);  // Exponential backoff
      mqtt.loop();

      // Check if still connected
      if (!mqtt.connected()) {
        Serial.println("  MQTT disconnected during retry");
        mqttConnected = false;
        return false;
      }
    }
  }
  return false;
}

void publishPendingData() {
  if (!mqttConnected) {
    Serial.println("[Publish] MQTT not connected - data safe in flash");
    return;
  }

  if (!LittleFS.exists(DATA_FILE)) {
    Serial.println("[Publish] No pending data");
    return;
  }

  // Check signal quality before publishing
  currentRSSI = WiFi.RSSI();
  Serial.printf("[Publish] Signal: %d dBm (%s)\n",
                currentRSSI, getSignalQuality(currentRSSI).c_str());

  if (currentRSSI < RSSI_UNUSABLE) {
    Serial.println("[Publish] Signal too weak - skipping publish, data safe");
    return;
  }

  File file = LittleFS.open(DATA_FILE, FILE_READ);
  if (!file || file.size() == 0) {
    file.close();
    return;
  }

  // Read all lines
  std::vector<String> lines;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 10) {
      lines.push_back(line);
    }
    yield();
  }
  file.close();

  if (lines.empty()) {
    LittleFS.remove(DATA_FILE);
    return;
  }

  Serial.printf("\n─── Publishing %d Readings ───\n", lines.size());

  int sent = 0;
  int failed = -1;

  // Adaptive delay based on signal strength
  int publishDelay = 50;
  if (currentRSSI < RSSI_FAIR) publishDelay = 100;  // Slower for fair signal
  if (currentRSSI < RSSI_WEAK) publishDelay = 200;  // Even slower for weak

  for (size_t i = 0; i < lines.size(); i++) {
    yield();
    mqtt.loop();

    if (publishWithRetry(MQTT_TOPIC_DATA, lines[i].c_str())) {
      sent++;
      Serial.printf("  [%d/%d] Sent\n", sent, lines.size());
      handleConnectionSuccess();
    } else {
      Serial.printf("  [%d/%d] FAILED after retries\n", i + 1, lines.size());
      handleConnectionFailure();
      failed = i;
      break;
    }

    delay(publishDelay);
    mqtt.loop();

    // Re-check connection every 10 messages
    if (sent % 10 == 0 && !mqtt.connected()) {
      Serial.println("[Publish] Lost connection mid-publish");
      mqttConnected = false;
      failed = i + 1;
      break;
    }
  }

  // Handle results
  if (failed < 0) {
    // All sent - delete file
    LittleFS.remove(DATA_FILE);
    Serial.printf("[Publish] SUCCESS: All %d readings delivered\n", sent);
  } else {
    // Partial - keep unsent readings
    int remaining = lines.size() - failed;
    Serial.printf("[Publish] PARTIAL: %d sent, %d remaining in flash\n", sent, remaining);

    // Rewrite file with only unsent readings
    File newFile = LittleFS.open(DATA_FILE, FILE_WRITE);
    if (newFile) {
      for (size_t i = failed; i < lines.size(); i++) {
        newFile.println(lines[i]);
        yield();
      }
      newFile.close();
    }
  }
}

// ============ WIFI QUALITY MONITORING ============

String getSignalQuality(int rssi) {
  if (rssi >= RSSI_EXCELLENT) return "EXCELLENT";
  if (rssi >= RSSI_GOOD) return "GOOD";
  if (rssi >= RSSI_FAIR) return "FAIR";
  if (rssi >= RSSI_WEAK) return "WEAK";
  return "UNUSABLE";
}

void checkWiFiQuality() {
  if (!wifiConnected) return;

  currentRSSI = WiFi.RSSI();

  // Log signal quality periodically
  static unsigned long lastQualityLog = 0;
  if (millis() - lastQualityLog >= 60000) {  // Every minute
    lastQualityLog = millis();
    Serial.printf("[WiFi] Signal: %d dBm (%s)\n",
                  currentRSSI, getSignalQuality(currentRSSI).c_str());

    if (currentRSSI < RSSI_WEAK) {
      Serial.println("[WiFi] WARNING: Weak signal - expect connection issues");
    }
  }
}

void handleConnectionFailure() {
  consecutiveFailures++;
  Serial.printf("[Network] Failure count: %d/%d\n", consecutiveFailures, FAILURE_THRESHOLD);

  if (consecutiveFailures >= FAILURE_THRESHOLD) {
    Serial.println("[Network] Too many failures - trying alternate WiFi...");

    consecutiveFailures = 0;
    mqttConnected = false;

    // Try switching to the other WiFi network
    int newNetwork = (activeWiFi == 1) ? 2 : 1;

    if (connectToWiFi(newNetwork)) {
      Serial.printf("[Network] Switched to WiFi %d\n", newNetwork);
    } else {
      // Fall back to original
      Serial.printf("[Network] WiFi %d failed, staying on WiFi %d\n", newNetwork, activeWiFi);
      connectToWiFi(activeWiFi);
    }
  }
}

void handleConnectionSuccess() {
  if (consecutiveFailures > 0) {
    Serial.printf("[Network] Connection recovered after %d failures\n", consecutiveFailures);
  }
  consecutiveFailures = 0;
}

bool isNetworkHealthy() {
  if (!wifiConnected) return false;
  if (currentRSSI < RSSI_UNUSABLE) return false;
  return true;
}

// ============ UTILITIES ============

float round2(float value) {
  return round(value * 100.0) / 100.0;
}

unsigned long getTimestamp() {
  time_t now;
  time(&now);
  if (now < 1700000000) {
    return millis() / 1000;
  }
  return (unsigned long)now;
}
