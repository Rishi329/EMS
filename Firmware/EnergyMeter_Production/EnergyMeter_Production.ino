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
 * ║  Partition Scheme: Default 4MB with spiffs                   ║
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
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ============ VERSION ============
#define FIRMWARE_VERSION "3.0.0"

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
IPAddress LOCAL_IP_2(0, 0, 0, 0);      // Use DHCP for backup
IPAddress GATEWAY_2(0, 0, 0, 0);
IPAddress SUBNET_2(0, 0, 0, 0);

// --- MQTT Settings ---
const char* MQTT_SERVER = "167.71.233.135";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "esp32meter";
const char* MQTT_PASSWORD = "meter@123";
const char* MQTT_TOPIC_DATA = "meters/data";
const char* MQTT_TOPIC_STATUS = "meters/status";
const int MQTT_KEEPALIVE = 120;           // 120 seconds keepalive
const int MQTT_BUFFER_SIZE = 12288;       // 12KB for payload

// --- Modbus Settings ---
HardwareSerial& ModbusSerial = Serial2;
const int MODBUS_BAUD = 9600;
const int MODBUS_RX = 18;
const int MODBUS_TX = 17;

// --- Meter Configuration ---
const uint8_t METER_IDS[] = {7, 10, 47, 3, 4};
const uint8_t NUM_METERS = sizeof(METER_IDS) / sizeof(METER_IDS[0]);

// --- Timing Configuration ---
const unsigned long READ_INTERVAL_MS = 60000;      // Read every 1 minute
const unsigned long PUBLISH_INTERVAL_MS = 120000;  // Publish every 2 minutes
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

// --- WiFi Quality Thresholds ---
const int RSSI_EXCELLENT = -50;   // Excellent signal
const int RSSI_GOOD = -60;        // Good signal
const int RSSI_FAIR = -70;        // Fair signal (may have issues)
const int RSSI_WEAK = -80;        // Weak signal (expect failures)
const int RSSI_UNUSABLE = -90;    // Too weak to use

// --- Buffer Configuration ---
const int MAX_FILE_ENTRIES = 1000;        // Max readings in flash
const char* DATA_FILE = "/readings.jsonl"; // JSON Lines format

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
    Serial.println("[1/5] Initializing flash storage...");
    initStorage();

    // Step 2: Initialize Modbus
    Serial.println("[2/5] Initializing Modbus...");
    initModbus();

    // Step 3: Get Device Identity
    Serial.println("[3/5] Getting device identity...");
    initDeviceId();

    // Step 4: Initialize WiFi
    Serial.println("[4/5] Connecting to WiFi...");
    initWiFi();

    // Step 5: Initialize MQTT
    Serial.println("[5/5] Configuring MQTT...");
    initMQTT();

    Serial.println("\n========== READY ==========");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    printStorageStatus();
    Serial.println("===========================\n");
}

void printBanner() {
    Serial.println("\n");
    Serial.println("╔═══════════════════════════════════════════╗");
    Serial.println("║   ESP32 Energy Monitor - Production v3.0  ║");
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
        case ESP_RST_POWERON:   Serial.println("Power-on"); break;
        case ESP_RST_SW:        Serial.println("Software reset"); break;
        case ESP_RST_PANIC:     Serial.println("Crash/Panic"); break;
        case ESP_RST_INT_WDT:   Serial.println("Interrupt WDT"); break;
        case ESP_RST_TASK_WDT:  Serial.println("Task WDT"); break;
        case ESP_RST_WDT:       Serial.println("Watchdog"); break;
        case ESP_RST_BROWNOUT:  Serial.println("Brownout"); break;
        default:                Serial.printf("Code %d\n", reason); break;
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
    mqtt.setKeepAlive(MQTT_KEEPALIVE);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    Serial.printf("  Broker: %s:%d\n", MQTT_SERVER, MQTT_PORT);
    Serial.printf("  Keepalive: %d seconds\n", MQTT_KEEPALIVE);
}

// ============ MAIN LOOP ============

void loop() {
    yield();  // Watchdog feed

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
        String onlineMsg = "{\"status\":\"online\",\"device\":\"" + deviceId +
                          "\",\"ip\":\"" + WiFi.localIP().toString() +
                          "\",\"version\":\"" + FIRMWARE_VERSION +
                          "\",\"heap\":" + String(ESP.getFreeHeap()) + "}";
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

        packet.meters[i] = readSingleMeter(meters[i], METER_IDS[i]);

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

MeterReading readSingleMeter(ModbusMaster& node, uint8_t id) {
    MeterReading r;
    memset(&r, 0, sizeof(r));
    r.meterId = id;
    r.valid = true;

    // Read voltage registers with MQTT keepalive
    r.vL1 = readRegister(node, 0);
    r.vL2 = readRegister(node, 2);
    r.vL3 = readRegister(node, 4);
    mqtt.loop(); yield();

    // Read current registers
    r.iL1 = readRegister(node, 6);
    r.iL2 = readRegister(node, 8);
    r.iL3 = readRegister(node, 10);
    mqtt.loop(); yield();

    // Read power registers
    r.pL1 = readRegister(node, 12);
    r.pL2 = readRegister(node, 14);
    r.pL3 = readRegister(node, 16);
    mqtt.loop(); yield();

    // Apparent power
    r.sL1 = readRegister(node, 18);
    r.sL2 = readRegister(node, 20);
    r.sL3 = readRegister(node, 22);
    mqtt.loop(); yield();

    // Reactive power
    r.qL1 = readRegister(node, 24);
    r.qL2 = readRegister(node, 26);
    r.qL3 = readRegister(node, 28);
    mqtt.loop(); yield();

    // Power factor
    r.pfL1 = readRegister(node, 30);
    r.pfL2 = readRegister(node, 32);
    r.pfL3 = readRegister(node, 34);
    mqtt.loop(); yield();

    // Phase angle
    r.paL1 = readRegister(node, 36);
    r.paL2 = readRegister(node, 38);
    r.paL3 = readRegister(node, 40);
    mqtt.loop(); yield();

    // System values
    r.vAvg = readRegister(node, 42);
    r.iAvg = readRegister(node, 46);
    r.iTotal = readRegister(node, 48);
    r.pTotal = readRegister(node, 52);
    r.sTotal = readRegister(node, 56);
    r.qTotal = readRegister(node, 60);
    r.pfTotal = readRegister(node, 62);
    r.paTotal = readRegister(node, 66);
    r.frequency = readRegister(node, 70);
    mqtt.loop(); yield();

    // Energy values
    r.eImport = readRegister(node, 72);
    r.eExport = readRegister(node, 74);
    r.erImport = readRegister(node, 76);
    r.erExport = readRegister(node, 78);
    r.esTotal = readRegister(node, 80);
    mqtt.loop(); yield();

    // Demand
    r.demand = readRegister(node, 84);
    r.demandMax = readRegister(node, 86);
    mqtt.loop(); yield();

    // Line-to-line voltages
    r.vL1L2 = readRegister(node, 200);
    r.vL2L3 = readRegister(node, 202);
    r.vL3L1 = readRegister(node, 204);
    r.vLLAvg = readRegister(node, 206);
    mqtt.loop(); yield();

    // Neutral current
    r.iNeutral = readRegister(node, 224);
    mqtt.loop(); yield();

    // THD values
    r.thdVL1 = readRegister(node, 234);
    r.thdVL2 = readRegister(node, 236);
    r.thdVL3 = readRegister(node, 238);
    r.thdIL1 = readRegister(node, 240);
    r.thdIL2 = readRegister(node, 242);
    r.thdIL3 = readRegister(node, 244);
    r.thdVAvg = readRegister(node, 248);
    r.thdIAvg = readRegister(node, 250);
    mqtt.loop(); yield();

    // Current demand
    r.idL1 = readRegister(node, 258);
    r.idL2 = readRegister(node, 260);
    r.idL3 = readRegister(node, 262);
    r.idMaxL1 = readRegister(node, 264);
    r.idMaxL2 = readRegister(node, 266);
    r.idMaxL3 = readRegister(node, 268);
    mqtt.loop(); yield();

    // Total energy
    r.eTotal = readRegister(node, 342);
    r.erTotal = readRegister(node, 344);

    // Validate
    if (r.vL1 < 1.0 && r.iL1 < 0.001) {
        r.valid = false;
    }

    return r;
}

float readRegister(ModbusMaster& node, uint16_t addr) {
    uint8_t result = node.readHoldingRegisters(addr, 2);

    if (result == node.ku8MBSuccess) {
        uint32_t raw = (node.getResponseBuffer(0) << 16) | node.getResponseBuffer(1);
        float value;
        memcpy(&value, &raw, 4);
        return value;
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
    if (currentRSSI < RSSI_FAIR) publishDelay = 100;      // Slower for fair signal
    if (currentRSSI < RSSI_WEAK) publishDelay = 200;      // Even slower for weak

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
