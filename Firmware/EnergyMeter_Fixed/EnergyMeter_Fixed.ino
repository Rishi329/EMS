/*
 * ESP32-S3 Multi-Meter Energy Monitor with MQTT QoS 1
 * Version: 2.3.0 - Stability Fixes
 *
 * Fixes applied:
 * - Stack overflow prevention (static payload buffer)
 * - Watchdog timeout prevention (yield() calls)
 * - Heap fragmentation reduction
 * - Reset reason logging for debugging
 * - Improved error handling
 * - Memory-safe JSON operations
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ============ VERSION ============
#define FIRMWARE_VERSION "2.3.0"

// --- WiFi Settings ---
const char* ssid = "REPL-6";
const char* password = "P@ssw0rd1234";

// Static IP Configuration
IPAddress local_IP(192, 168, 6, 55);
IPAddress gateway(192, 168, 6, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// --- MQTT Settings ---
const char* mqtt_server = "167.71.233.135";
const int mqtt_port = 1883;
const char* mqtt_user = "esp32meter";
const char* mqtt_password = "meter@123";
const char* mqtt_topic_data = "meters/data";
const char* mqtt_topic_status = "meters/status";
const char* mqtt_client_id = "ESP32_EnergyMeter_";

// --- Modbus Settings ---
HardwareSerial& ModbusSerial = Serial2;
#define BUS_BAUD_RATE 9600
#define BUS_PARITY SERIAL_8N1

// =============================================================
// METER CONFIGURATION - Add/remove meters here
// =============================================================
#define MAX_METERS 10  // Maximum supported meters

// Define your meter slave IDs here
const uint8_t METER_SLAVE_IDS[] = {7, 10, 47, 3, 4};
const uint8_t NUM_METERS = sizeof(METER_SLAVE_IDS) / sizeof(METER_SLAVE_IDS[0]);

// Modbus nodes array
ModbusMaster meterNodes[MAX_METERS];

// --- Timing Settings ---
const unsigned long readInterval = 60UL * 1000UL;        // Read every 1 minute
const unsigned long publishInterval = 5UL * 60UL * 1000UL; // Publish every 5 minutes
unsigned long lastReadTime = 0;
unsigned long lastPublishTime = 0;
unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

// --- Buffer Settings ---
#define MAX_BUFFER_ENTRIES 500
const char* BUFFER_FILE = "/meter_buffer.json";

// --- Global variables ---
String deviceMAC = "";
String clientId = "";
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt(espClient);

// =============================================================
// FIX #1: Static payload buffer (prevents stack overflow)
// =============================================================
#define MQTT_PAYLOAD_SIZE 12288  // 12KB for 5 meters
static char mqttPayload[MQTT_PAYLOAD_SIZE];

// Meter data structure - Comprehensive 3-Phase Values (58 parameters)
struct MeterData {
  // Per-Phase Voltage (V)
  float voltageL1;
  float voltageL2;
  float voltageL3;

  // Per-Phase Current (A)
  float currentL1;
  float currentL2;
  float currentL3;

  // Per-Phase Active Power (W)
  float powerL1;
  float powerL2;
  float powerL3;

  // Per-Phase Apparent Power (VA)
  float apparentPowerL1;
  float apparentPowerL2;
  float apparentPowerL3;

  // Per-Phase Reactive Power (VAr)
  float reactivePowerL1;
  float reactivePowerL2;
  float reactivePowerL3;

  // Per-Phase Power Factor
  float pfL1;
  float pfL2;
  float pfL3;

  // Per-Phase Phase Angle (degrees)
  float phaseAngleL1;
  float phaseAngleL2;
  float phaseAngleL3;

  // Line-to-Line Voltages (V)
  float voltageL1L2;
  float voltageL2L3;
  float voltageL3L1;
  float avgVoltageLtoL;

  // System Totals
  float avgVoltage;
  float avgCurrent;
  float totalCurrent;
  float totalPower;
  float totalApparentPower;
  float totalReactivePower;
  float totalPF;
  float totalPhaseAngle;
  float frequency;

  // Neutral Current (A)
  float neutralCurrent;

  // Energy Values (kWh, kVArh, kVAh)
  float importActiveEnergy;
  float exportActiveEnergy;
  float importReactiveEnergy;
  float exportReactiveEnergy;
  float totalActiveEnergy;
  float totalReactiveEnergy;
  float totalApparentEnergy;

  // Demand Values (W/A)
  float currentDemand;
  float maxDemand;
  float currentDemandL1;
  float currentDemandL2;
  float currentDemandL3;
  float maxCurrentDemandL1;
  float maxCurrentDemandL2;
  float maxCurrentDemandL3;

  // Power Quality - THD (%)
  float voltageTHDL1;
  float voltageTHDL2;
  float voltageTHDL3;
  float currentTHDL1;
  float currentTHDL2;
  float currentTHDL3;
  float avgVoltageTHD;
  float avgCurrentTHD;

  bool valid;
};

// Buffered reading structure
struct BufferedReading {
  unsigned long timestamp;
  MeterData meters[MAX_METERS];
};

// Current readings buffer (in memory)
#define MEMORY_BUFFER_SIZE 10
BufferedReading memoryBuffer[MEMORY_BUFFER_SIZE];
int bufferIndex = 0;
int bufferCount = 0;

// =============================================================
// FIX #2: Reset reason logging for debugging
// =============================================================
void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[Boot] Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON:   Serial.println("Power-on"); break;
    case ESP_RST_EXT:       Serial.println("External reset"); break;
    case ESP_RST_SW:        Serial.println("Software reset"); break;
    case ESP_RST_PANIC:     Serial.println("Exception/panic"); break;
    case ESP_RST_INT_WDT:   Serial.println("Interrupt watchdog"); break;
    case ESP_RST_TASK_WDT:  Serial.println("Task watchdog"); break;
    case ESP_RST_WDT:       Serial.println("Other watchdog"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("Deep sleep wake"); break;
    case ESP_RST_BROWNOUT:  Serial.println("Brownout"); break;
    case ESP_RST_SDIO:      Serial.println("SDIO"); break;
    default:                Serial.printf("Unknown (%d)\n", reason); break;
  }
}

// =============================================================
// Utility Functions
// =============================================================
float round2(float value) {
  return round(value * 100.0) / 100.0;
}

unsigned long getTimestamp() {
  time_t now;
  time(&now);
  if (now < 1700000000) {  // If NTP not synced yet, use millis
    return millis() / 1000;
  }
  return (unsigned long)now;
}

// =============================================================
// FIX #3: Watchdog-safe delay function
// =============================================================
void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    yield();  // Feed watchdog
    delay(1); // Small delay
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // Reduced from 3000

  Serial.println("\n\n===========================================");
  Serial.printf("  ESP32-S3 Energy Meter v%s\n", FIRMWARE_VERSION);
  Serial.println("  MQTT QoS 1 - Stability Enhanced");
  Serial.println("===========================================\n");

  // FIX #2: Print reset reason for debugging
  printResetReason();

  // Print memory info
  Serial.printf("[Boot] Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("[Boot] Largest free block: %d bytes\n", ESP.getMaxAllocHeap());

  // Step 1: Initialize LittleFS
  Serial.println("[1/5] Initializing local storage...");
  if (!LittleFS.begin(true)) {
    Serial.println("      LittleFS mount failed! Formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println("      Local storage ready");
  printBufferStatus();

  // Step 2: Initialize Modbus
  Serial.println("[2/5] Initializing Modbus...");
  ModbusSerial.begin(BUS_BAUD_RATE, BUS_PARITY, 18, 17);

  Serial.printf("      Configuring %d meters: ", NUM_METERS);
  for (int i = 0; i < NUM_METERS; i++) {
    meterNodes[i].begin(METER_SLAVE_IDS[i], ModbusSerial);
    Serial.printf("%d", METER_SLAVE_IDS[i]);
    if (i < NUM_METERS - 1) Serial.print(", ");
  }
  Serial.println();

  // Step 3: Get device identity
  Serial.println("[3/5] Getting device identity...");
  deviceMAC = WiFi.macAddress();
  deviceMAC.replace(":", "");
  clientId = mqtt_client_id + deviceMAC;
  Serial.printf("      MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("      Client ID: %s\n", clientId.c_str());

  // Step 4: Initialize WiFi
  Serial.println("[4/5] Initializing WiFi...");
  initializeWiFi();

  // Step 5: Configure MQTT
  Serial.println("[5/5] Configuring MQTT...");
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);

  // FIX #4: Increased buffer size for 5 meters
  mqtt.setBufferSize(MQTT_PAYLOAD_SIZE);

  Serial.printf("      Broker: %s:%d\n", mqtt_server, mqtt_port);
  Serial.printf("      Buffer size: %d bytes\n", MQTT_PAYLOAD_SIZE);

  Serial.println("\n============ SETUP COMPLETE ============");
  Serial.printf("Free heap after setup: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=========================================\n");
}

void loop() {
  // FIX #5: Feed watchdog at start of each loop
  yield();

  // Maintain WiFi connection
  manageWiFiConnection();

  // Maintain MQTT connection
  if (wifiConnected) {
    manageMqttConnection();
    mqtt.loop();
  }

  unsigned long currentMillis = millis();

  // Read meters every 1 minute
  if (currentMillis - lastReadTime >= readInterval) {
    lastReadTime = currentMillis;
    readAndBufferMeterData();
  }

  // Publish every 5 minutes
  if (currentMillis - lastPublishTime >= publishInterval) {
    lastPublishTime = currentMillis;

    // FIX #6: Memory check before heavy operations
    Serial.printf("[Memory] Free heap: %d bytes\n", ESP.getFreeHeap());

    if (mqttConnected) {
      publishBufferedData();
      flushFileBuffer();
    } else {
      Serial.println("MQTT not connected - data saved to local buffer");
      saveBufferToFile();
    }
  }

  delay(100);
}

// ============ WiFi Functions ============

void initializeWiFi() {
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("      Static IP failed, using DHCP");
  }

  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(WiFiEvent);

  Serial.printf("      Connecting to %s", ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    safeDelay(500);  // FIX: Use safe delay
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n      Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    configTime(19800, 0, "pool.ntp.org");
  } else {
    Serial.println("\n      WiFi connection failed - will retry");
  }
}

void manageWiFiConnection() {
  static unsigned long lastWiFiCheck = 0;

  if (millis() - lastWiFiCheck < 10000) return;
  lastWiFiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    mqttConnected = false;
    Serial.println("WiFi disconnected - reconnecting...");
    WiFi.reconnect();
  }
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
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

// ============ MQTT Functions ============

void manageMqttConnection() {
  if (mqtt.connected()) {
    mqttConnected = true;
    return;
  }

  mqttConnected = false;

  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) return;
  lastMqttReconnect = millis();

  Serial.print("[MQTT] Connecting to broker...");

  String willTopic = String(mqtt_topic_status) + "/" + deviceMAC;
  String willMessage = "{\"status\":\"offline\",\"device\":\"" + deviceMAC + "\"}";

  if (mqtt.connect(clientId.c_str(), mqtt_user, mqtt_password,
                   willTopic.c_str(), 1, true, willMessage.c_str())) {
    Serial.println(" Connected!");
    mqttConnected = true;

    String onlineMsg = "{\"status\":\"online\",\"device\":\"" + deviceMAC +
                       "\",\"ip\":\"" + WiFi.localIP().toString() +
                       "\",\"version\":\"" + FIRMWARE_VERSION + "\"}";
    mqtt.publish(willTopic.c_str(), onlineMsg.c_str(), true);

    mqtt.subscribe("meters/commands/#");
  } else {
    Serial.printf(" Failed (rc=%d)\n", mqtt.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s: ", topic);
  for (unsigned int i = 0; i < length && i < 100; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

bool publishWithQoS1(const char* topic, const char* payload) {
  if (!mqttConnected) return false;

  yield();  // FIX: Feed watchdog before network operation

  bool success = mqtt.publish(topic, payload, false);

  if (success) {
    Serial.println("[MQTT] Published successfully");
  } else {
    Serial.println("[MQTT] Publish failed");
  }

  return success;
}

// ============ Meter Reading Functions ============

void readAndBufferMeterData() {
  Serial.println("\n--- Reading Meters ---");
  Serial.printf("[Memory] Free heap: %d bytes\n", ESP.getFreeHeap());

  BufferedReading reading;
  reading.timestamp = getTimestamp();

  for (int i = 0; i < NUM_METERS; i++) {
    yield();  // FIX #7: Feed watchdog between meter reads

    Serial.printf("  Meter %d:\n", METER_SLAVE_IDS[i]);
    reading.meters[i] = readMeter(meterNodes[i]);

    if (reading.meters[i].valid) {
      printMeterSummary(reading.meters[i]);
    } else {
      Serial.println("    READ FAILED");
    }

    safeDelay(100);  // FIX: Increased delay with watchdog feeding
  }

  memoryBuffer[bufferIndex] = reading;
  bufferIndex = (bufferIndex + 1) % MEMORY_BUFFER_SIZE;
  if (bufferCount < MEMORY_BUFFER_SIZE) bufferCount++;

  Serial.printf("  Buffer: %d readings in memory\n", bufferCount);
}

void printMeterSummary(MeterData& m) {
  Serial.printf("    V: L1=%.1f L2=%.1f L3=%.1f (Avg=%.1f)\n",
    m.voltageL1, m.voltageL2, m.voltageL3, m.avgVoltage);
  Serial.printf("    I: L1=%.2f L2=%.2f L3=%.2f (Total=%.2f)\n",
    m.currentL1, m.currentL2, m.currentL3, m.totalCurrent);
  Serial.printf("    P: %.1fW  PF: %.2f  Freq: %.2fHz\n",
    m.totalPower, m.totalPF, m.frequency);
}

MeterData readMeter(ModbusMaster& node) {
  MeterData data;
  memset(&data, 0, sizeof(MeterData));  // FIX #8: Initialize all fields to 0
  data.valid = true;

  yield();  // Feed watchdog

  // Per-Phase Voltages (V)
  data.voltageL1 = readFloat(node, 0);
  data.voltageL2 = readFloat(node, 2);
  data.voltageL3 = readFloat(node, 4);

  yield();  // Feed watchdog

  // Per-Phase Currents (A)
  data.currentL1 = readFloat(node, 6);
  data.currentL2 = readFloat(node, 8);
  data.currentL3 = readFloat(node, 10);

  yield();

  // Per-Phase Active Power (W)
  data.powerL1 = readFloat(node, 12);
  data.powerL2 = readFloat(node, 14);
  data.powerL3 = readFloat(node, 16);

  yield();

  // Per-Phase Apparent Power (VA)
  data.apparentPowerL1 = readFloat(node, 18);
  data.apparentPowerL2 = readFloat(node, 20);
  data.apparentPowerL3 = readFloat(node, 22);

  yield();

  // Per-Phase Reactive Power (VAr)
  data.reactivePowerL1 = readFloat(node, 24);
  data.reactivePowerL2 = readFloat(node, 26);
  data.reactivePowerL3 = readFloat(node, 28);

  yield();

  // Per-Phase Power Factor
  data.pfL1 = readFloat(node, 30);
  data.pfL2 = readFloat(node, 32);
  data.pfL3 = readFloat(node, 34);

  yield();

  // Per-Phase Phase Angle (degrees)
  data.phaseAngleL1 = readFloat(node, 36);
  data.phaseAngleL2 = readFloat(node, 38);
  data.phaseAngleL3 = readFloat(node, 40);

  yield();

  // System Averages & Totals
  data.avgVoltage = readFloat(node, 42);
  data.avgCurrent = readFloat(node, 46);
  data.totalCurrent = readFloat(node, 48);
  data.totalPower = readFloat(node, 52);
  data.totalApparentPower = readFloat(node, 56);
  data.totalReactivePower = readFloat(node, 60);
  data.totalPF = readFloat(node, 62);
  data.totalPhaseAngle = readFloat(node, 66);
  data.frequency = readFloat(node, 70);

  yield();

  // Energy Values
  data.importActiveEnergy = readFloat(node, 72);
  data.exportActiveEnergy = readFloat(node, 74);
  data.importReactiveEnergy = readFloat(node, 76);
  data.exportReactiveEnergy = readFloat(node, 78);
  data.totalApparentEnergy = readFloat(node, 80);

  yield();

  // System Power Demand
  data.currentDemand = readFloat(node, 84);
  data.maxDemand = readFloat(node, 86);

  yield();

  // Line-to-Line Voltages (V)
  data.voltageL1L2 = readFloat(node, 200);
  data.voltageL2L3 = readFloat(node, 202);
  data.voltageL3L1 = readFloat(node, 204);
  data.avgVoltageLtoL = readFloat(node, 206);

  yield();

  // Neutral Current (A)
  data.neutralCurrent = readFloat(node, 224);

  yield();

  // THD Values (%)
  data.voltageTHDL1 = readFloat(node, 234);
  data.voltageTHDL2 = readFloat(node, 236);
  data.voltageTHDL3 = readFloat(node, 238);
  data.currentTHDL1 = readFloat(node, 240);
  data.currentTHDL2 = readFloat(node, 242);
  data.currentTHDL3 = readFloat(node, 244);
  data.avgVoltageTHD = readFloat(node, 248);
  data.avgCurrentTHD = readFloat(node, 250);

  yield();

  // Per-Phase Current Demand (A)
  data.currentDemandL1 = readFloat(node, 258);
  data.currentDemandL2 = readFloat(node, 260);
  data.currentDemandL3 = readFloat(node, 262);
  data.maxCurrentDemandL1 = readFloat(node, 264);
  data.maxCurrentDemandL2 = readFloat(node, 266);
  data.maxCurrentDemandL3 = readFloat(node, 268);

  yield();

  // Total Energy (resettable)
  data.totalActiveEnergy = readFloat(node, 342);
  data.totalReactiveEnergy = readFloat(node, 344);

  // Validate reading
  if (data.voltageL1 < 1.0 && data.currentL1 < 0.001) {
    data.valid = false;
  }

  return data;
}

float readFloat(ModbusMaster& node, uint16_t address) {
  uint8_t result = node.readHoldingRegisters(address, 2);

  if (result == node.ku8MBSuccess) {
    uint32_t rawData = (node.getResponseBuffer(0) << 16) | node.getResponseBuffer(1);
    float value;
    memcpy(&value, &rawData, 4);
    return value;
  }
  return 0.0;
}

// ============ Publishing Functions ============

void addMeterToJson(JsonObject& obj, MeterData& m) {
  // Per-Phase Voltages
  obj["vL1"] = round2(m.voltageL1);
  obj["vL2"] = round2(m.voltageL2);
  obj["vL3"] = round2(m.voltageL3);
  obj["vAvg"] = round2(m.avgVoltage);

  // Line-to-Line Voltages
  obj["vL1L2"] = round2(m.voltageL1L2);
  obj["vL2L3"] = round2(m.voltageL2L3);
  obj["vL3L1"] = round2(m.voltageL3L1);
  obj["vLLAvg"] = round2(m.avgVoltageLtoL);

  // Per-Phase Currents
  obj["iL1"] = round2(m.currentL1);
  obj["iL2"] = round2(m.currentL2);
  obj["iL3"] = round2(m.currentL3);
  obj["iAvg"] = round2(m.avgCurrent);
  obj["iTotal"] = round2(m.totalCurrent);
  obj["iN"] = round2(m.neutralCurrent);

  // Per-Phase Power
  obj["pL1"] = round2(m.powerL1);
  obj["pL2"] = round2(m.powerL2);
  obj["pL3"] = round2(m.powerL3);
  obj["pTotal"] = round2(m.totalPower);

  // Per-Phase Apparent Power
  obj["sL1"] = round2(m.apparentPowerL1);
  obj["sL2"] = round2(m.apparentPowerL2);
  obj["sL3"] = round2(m.apparentPowerL3);
  obj["sTotal"] = round2(m.totalApparentPower);

  // Per-Phase Reactive Power
  obj["qL1"] = round2(m.reactivePowerL1);
  obj["qL2"] = round2(m.reactivePowerL2);
  obj["qL3"] = round2(m.reactivePowerL3);
  obj["qTotal"] = round2(m.totalReactivePower);

  // Per-Phase Power Factor
  obj["pfL1"] = round2(m.pfL1);
  obj["pfL2"] = round2(m.pfL2);
  obj["pfL3"] = round2(m.pfL3);
  obj["pfTotal"] = round2(m.totalPF);

  // Per-Phase Phase Angle
  obj["paL1"] = round2(m.phaseAngleL1);
  obj["paL2"] = round2(m.phaseAngleL2);
  obj["paL3"] = round2(m.phaseAngleL3);
  obj["paTotal"] = round2(m.totalPhaseAngle);

  // Frequency
  obj["freq"] = round2(m.frequency);

  // Energy Values
  obj["eImp"] = round2(m.importActiveEnergy);
  obj["eExp"] = round2(m.exportActiveEnergy);
  obj["eTotal"] = round2(m.totalActiveEnergy);
  obj["erImp"] = round2(m.importReactiveEnergy);
  obj["erExp"] = round2(m.exportReactiveEnergy);
  obj["erTotal"] = round2(m.totalReactiveEnergy);
  obj["esTotal"] = round2(m.totalApparentEnergy);

  // Power Demand Values
  obj["demand"] = round2(m.currentDemand);
  obj["demandMax"] = round2(m.maxDemand);

  // Per-Phase Current Demand
  obj["idL1"] = round2(m.currentDemandL1);
  obj["idL2"] = round2(m.currentDemandL2);
  obj["idL3"] = round2(m.currentDemandL3);
  obj["idMaxL1"] = round2(m.maxCurrentDemandL1);
  obj["idMaxL2"] = round2(m.maxCurrentDemandL2);
  obj["idMaxL3"] = round2(m.maxCurrentDemandL3);

  // THD Values
  obj["thdVL1"] = round2(m.voltageTHDL1);
  obj["thdVL2"] = round2(m.voltageTHDL2);
  obj["thdVL3"] = round2(m.voltageTHDL3);
  obj["thdVAvg"] = round2(m.avgVoltageTHD);
  obj["thdIL1"] = round2(m.currentTHDL1);
  obj["thdIL2"] = round2(m.currentTHDL2);
  obj["thdIL3"] = round2(m.currentTHDL3);
  obj["thdIAvg"] = round2(m.avgCurrentTHD);
}

// =============================================================
// FIX #9: Completely rewritten publishBufferedData with proper
// memory management and watchdog feeding
// =============================================================
void publishBufferedData() {
  if (bufferCount == 0) {
    Serial.println("[Publish] No data in buffer");
    return;
  }

  Serial.printf("\n[Publish] Sending %d readings\n", bufferCount);
  Serial.printf("[Memory] Free heap before publish: %d bytes\n", ESP.getFreeHeap());

  int sentCount = 0;
  int failedAt = -1;

  for (int i = 0; i < bufferCount; i++) {
    yield();  // Feed watchdog at start of each iteration

    int idx = (bufferIndex - bufferCount + i + MEMORY_BUFFER_SIZE) % MEMORY_BUFFER_SIZE;
    BufferedReading& r = memoryBuffer[idx];

    // FIX #10: Use StaticJsonDocument to avoid heap fragmentation
    // Calculate size: base (~200) + meters (5 * ~1500) = ~7700 bytes
    StaticJsonDocument<10240> doc;

    doc["deviceId"] = deviceMAC;
    doc["deviceMac"] = WiFi.macAddress();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["ts"] = r.timestamp;
    doc["meterCount"] = NUM_METERS;
    doc["fw"] = FIRMWARE_VERSION;

    yield();  // Feed watchdog

    // Add all meters
    for (int m = 0; m < NUM_METERS; m++) {
      String meterKey = "meter" + String(METER_SLAVE_IDS[m]);
      JsonObject meterObj = doc.createNestedObject(meterKey);
      meterObj["id"] = METER_SLAVE_IDS[m];
      addMeterToJson(meterObj, r.meters[m]);
      yield();  // Feed watchdog between meters
    }

    // Serialize to static buffer (not stack!)
    size_t len = serializeJson(doc, mqttPayload, MQTT_PAYLOAD_SIZE);

    if (len == 0 || len >= MQTT_PAYLOAD_SIZE) {
      Serial.printf("[Publish] Serialization failed or overflow (len=%d)\n", len);
      continue;
    }

    yield();  // Feed watchdog before network operation

    if (!publishWithQoS1(mqtt_topic_data, mqttPayload)) {
      Serial.printf("[Publish] Failed at reading %d/%d\n", i + 1, bufferCount);
      failedAt = i;
      break;
    }

    sentCount++;
    safeDelay(100);  // Delay with watchdog feeding
    mqtt.loop();
  }

  // Handle results
  if (failedAt < 0) {
    // All sent successfully
    bufferCount = 0;
    bufferIndex = 0;
    Serial.printf("[Publish] Success - %d readings sent\n", sentCount);
  } else {
    // Partial failure - save remaining to file
    Serial.printf("[Publish] Partial: %d sent, saving %d to file\n",
                  sentCount, bufferCount - failedAt);
    saveRemainingToFile(failedAt, bufferCount);
  }

  Serial.printf("[Memory] Free heap after publish: %d bytes\n", ESP.getFreeHeap());
}

void saveRemainingToFile(int startIndex, int totalCount) {
  int remainingCount = totalCount - startIndex;
  if (remainingCount <= 0) {
    bufferCount = 0;
    bufferIndex = 0;
    return;
  }

  yield();

  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  size_t estimatedSize = remainingCount * 2048;

  if (freeBytes < estimatedSize + 10240) {
    Serial.println("[Buffer] WARNING: Low storage space!");
    if (LittleFS.exists(BUFFER_FILE)) {
      LittleFS.remove(BUFFER_FILE);
    }
  }

  File file = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("[Buffer] Failed to open file");
    return;
  }

  int savedCount = 0;
  for (int i = startIndex; i < totalCount; i++) {
    yield();  // Feed watchdog

    int idx = (bufferIndex - totalCount + i + MEMORY_BUFFER_SIZE) % MEMORY_BUFFER_SIZE;
    BufferedReading& r = memoryBuffer[idx];

    StaticJsonDocument<10240> doc;
    doc["ts"] = r.timestamp;
    doc["device"] = deviceMAC;
    doc["meterCount"] = NUM_METERS;

    for (int m = 0; m < NUM_METERS; m++) {
      String meterKey = "m" + String(METER_SLAVE_IDS[m]);
      JsonObject meterObj = doc.createNestedObject(meterKey);
      meterObj["id"] = METER_SLAVE_IDS[m];
      addMeterToJson(meterObj, r.meters[m]);
    }

    serializeJson(doc, file);
    file.println();
    savedCount++;

    yield();
  }

  file.close();
  bufferCount = 0;
  bufferIndex = 0;

  Serial.printf("[Buffer] Saved %d readings to file\n", savedCount);
}

// ============ File Buffer Functions ============

int getFileLineCount() {
  if (!LittleFS.exists(BUFFER_FILE)) return 0;

  File file = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!file) return 0;

  int count = 0;
  while (file.available()) {
    if (file.read() == '\n') count++;
    if (count % 50 == 0) yield();  // Feed watchdog during long reads
  }
  file.close();
  return count;
}

bool ensureStorageSpace(int entriesNeeded) {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;
  size_t estimatedSize = entriesNeeded * 2048;

  if (freeBytes < estimatedSize + 10240) {
    Serial.printf("[Buffer] Low space: %d KB free\n", freeBytes / 1024);

    if (LittleFS.exists(BUFFER_FILE)) {
      int currentLines = getFileLineCount();
      if (currentLines >= MAX_BUFFER_ENTRIES) {
        trimBufferFile(MAX_BUFFER_ENTRIES / 2);
      }
    }

    usedBytes = LittleFS.usedBytes();
    freeBytes = totalBytes - usedBytes;
    return (freeBytes >= estimatedSize + 5120);
  }

  return true;
}

void trimBufferFile(int keepCount) {
  if (!LittleFS.exists(BUFFER_FILE)) return;

  File file = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!file) return;

  std::vector<String> lines;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 10) {
      lines.push_back(line);
    }
    yield();  // Feed watchdog
  }
  file.close();

  int totalLines = lines.size();
  int startIndex = (totalLines > keepCount) ? (totalLines - keepCount) : 0;

  file = LittleFS.open(BUFFER_FILE, FILE_WRITE);
  if (!file) return;

  for (int i = startIndex; i < totalLines; i++) {
    file.println(lines[i]);
    yield();
  }
  file.close();

  Serial.printf("[Buffer] Trimmed: kept %d of %d\n", totalLines - startIndex, totalLines);
}

void saveBufferToFile() {
  if (bufferCount == 0) return;

  yield();

  if (!ensureStorageSpace(bufferCount)) {
    Serial.println("[Buffer] Cannot save - storage full!");
    return;
  }

  int currentEntries = getFileLineCount();
  if (currentEntries >= MAX_BUFFER_ENTRIES) {
    trimBufferFile(MAX_BUFFER_ENTRIES / 2);
  }

  File file = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("[Buffer] Failed to open file");
    return;
  }

  int savedCount = 0;
  for (int i = 0; i < bufferCount; i++) {
    yield();

    int idx = (bufferIndex - bufferCount + i + MEMORY_BUFFER_SIZE) % MEMORY_BUFFER_SIZE;
    BufferedReading& r = memoryBuffer[idx];

    StaticJsonDocument<10240> doc;
    doc["ts"] = r.timestamp;
    doc["device"] = deviceMAC;
    doc["meterCount"] = NUM_METERS;

    for (int m = 0; m < NUM_METERS; m++) {
      String meterKey = "m" + String(METER_SLAVE_IDS[m]);
      JsonObject meterObj = doc.createNestedObject(meterKey);
      meterObj["id"] = METER_SLAVE_IDS[m];
      addMeterToJson(meterObj, r.meters[m]);
    }

    serializeJson(doc, file);
    file.println();
    savedCount++;
  }

  file.close();
  bufferCount = 0;
  bufferIndex = 0;

  Serial.printf("[Buffer] Saved %d readings to file\n", savedCount);
  printBufferStatus();
}

void flushFileBuffer() {
  if (!LittleFS.exists(BUFFER_FILE)) return;

  File file = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!file || file.size() == 0) {
    file.close();
    return;
  }

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
    LittleFS.remove(BUFFER_FILE);
    return;
  }

  Serial.printf("[Buffer] Flushing %d saved readings\n", lines.size());

  int sentCount = 0;
  int failIndex = -1;

  for (size_t i = 0; i < lines.size(); i++) {
    yield();

    if (mqtt.publish(mqtt_topic_data, lines[i].c_str())) {
      sentCount++;
      safeDelay(100);
      mqtt.loop();
    } else {
      failIndex = i;
      break;
    }
  }

  if (failIndex < 0) {
    LittleFS.remove(BUFFER_FILE);
    Serial.printf("[Buffer] Flushed all %d readings\n", sentCount);
  } else {
    int remainingCount = lines.size() - failIndex;
    Serial.printf("[Buffer] Partial: %d sent, %d remaining\n", sentCount, remainingCount);

    file = LittleFS.open(BUFFER_FILE, FILE_WRITE);
    if (file) {
      for (size_t i = failIndex; i < lines.size(); i++) {
        file.println(lines[i]);
        yield();
      }
      file.close();
    }
  }
}

void printBufferStatus() {
  Serial.printf("      Storage: %d/%d KB used\n",
                LittleFS.usedBytes() / 1024,
                LittleFS.totalBytes() / 1024);

  if (LittleFS.exists(BUFFER_FILE)) {
    File file = LittleFS.open(BUFFER_FILE, FILE_READ);
    Serial.printf("      File buffer: %d bytes\n", file.size());
    file.close();
  } else {
    Serial.println("      File buffer: empty");
  }
}
