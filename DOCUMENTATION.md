# Energy Monitoring System (EMS) Documentation

## Version 2.2 - Dynamic Multi-Meter Support

---

## Table of Contents

1. [System Overview](#system-overview)
2. [Architecture](#architecture)
3. [Hardware Requirements](#hardware-requirements)
4. [Software Components](#software-components)
5. [Installation Guide](#installation-guide)
6. [Configuration](#configuration)
7. [Database Schema](#database-schema)
8. [API Reference](#api-reference)
9. [MQTT Topics](#mqtt-topics)
10. [Data Formats](#data-formats)
11. [Troubleshooting](#troubleshooting)
12. [Maintenance](#maintenance)

---

## System Overview

The Energy Monitoring System (EMS) is a complete IoT solution for monitoring energy consumption from Modbus-based energy meters. The system collects data from multiple meters, transmits it reliably to a cloud server using MQTT protocol, and stores it in a MySQL database for analysis and visualization.

### Key Features

- **Zero Data Loss**: Local buffering ensures no readings are lost during network outages
- **Real-time Monitoring**: MQTT QoS 1 guarantees message delivery
- **Dynamic Multi-meter Support**: Add any number of Modbus meters by simply editing one line of code
- **Scalable**: Supports multiple ESP32 devices reporting to single server
- **Comprehensive 3-Phase Monitoring**: 58 parameters per meter including THD, demand, and phase angles
- **Historical Data**: Stores all readings with timestamps for trend analysis
- **Auto-aggregation**: Automatic hourly and daily statistics calculation

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         FIELD DEVICES                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────┐    │
│   │ Energy Meter │  │ Energy Meter │  │ Energy Meter │  │ ... │    │
│   │   (ID: 7)    │  │   (ID: 10)   │  │   (ID: 11)   │  │     │    │
│   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──┬──┘    │
│          │      RS485 Modbus Bus (up to 10 meters)        │        │
│          └──────────────────┬─────────────────────────────┘        │
│                             │                                       │
│              ┌──────────────▼──────────────┐                        │
│              │         ESP32-S3            │                        │
│              │  ┌─────────────────────┐   │                        │
│              │  │ Dynamic Meter Array │   │  ← Configurable        │
│              │  ├─────────────────────┤   │                        │
│              │  │ Memory Buffer (10)  │   │  ← 10 readings in RAM  │
│              │  ├─────────────────────┤   │                        │
│              │  │ LittleFS (500)      │   │  ← 500 readings backup │
│              │  └─────────────────────┘   │                        │
│              └───────────────┬────────────┘                        │
│                              │ WiFi                                 │
└──────────────────────────────┼─────────────────────────────────────┘
                     │
                     │ MQTT (QoS 1)
                     ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         CLOUD SERVER                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌─────────────────┐      ┌─────────────────┐                      │
│   │   Mosquitto     │      │    FastAPI      │                      │
│   │  MQTT Broker    │─────▶│   Application   │                      │
│   │   Port: 1883    │      │   Port: 8000    │                      │
│   └─────────────────┘      └────────┬────────┘                      │
│                                     │                                │
│                            ┌────────▼────────┐                      │
│                            │     MySQL       │                      │
│                            │    Database     │                      │
│                            └─────────────────┘                      │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Hardware Requirements

### ESP32-S3 Module

| Component | Specification |
|-----------|---------------|
| MCU | ESP32-S3 (recommended) or ESP32 |
| Flash | Minimum 4MB |
| PSRAM | Optional (for extended buffering) |
| WiFi | 2.4GHz 802.11 b/g/n |

### RS485 Connection

| ESP32 Pin | Function |
|-----------|----------|
| GPIO 18 | RX (from RS485 module) |
| GPIO 17 | TX (to RS485 module) |
| 3.3V | Power |
| GND | Ground |

### Energy Meters

- **Protocol**: Modbus RTU
- **Baud Rate**: 9600
- **Parity**: 8N1
- **Supported Meters**: Any Modbus-compatible energy meter (Eastron SDM630 register map)
- **Default Slave IDs**: 7 and 10 (configurable)
- **Maximum Meters**: Up to 10 per ESP32 (can be increased in firmware)

### Server Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | 1 core | 2+ cores |
| RAM | 1 GB | 2+ GB |
| Storage | 20 GB | 50+ GB |
| OS | Ubuntu 20.04+ | Ubuntu 22.04 |

---

## Software Components

### ESP32 Firmware

| Library | Version | Purpose |
|---------|---------|---------|
| WiFi.h | Built-in | WiFi connectivity |
| PubSubClient | 2.8+ | MQTT client |
| ModbusMaster | 2.0+ | Modbus RTU communication |
| LittleFS | Built-in | Local file storage |
| ArduinoJson | 6.x | JSON serialization |

### Server Software

| Component | Version | Purpose |
|-----------|---------|---------|
| Python | 3.10+ | Runtime |
| FastAPI | 0.109+ | REST API framework |
| Mosquitto | 2.0+ | MQTT broker |
| MySQL | 8.0+ | Database |
| paho-mqtt | 1.6+ | Python MQTT client |

---

## Installation Guide

### Step 1: Server Setup

#### 1.1 Install Mosquitto MQTT Broker

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install Mosquitto
sudo apt install -y mosquitto mosquitto-clients

# Create password file
sudo mosquitto_passwd -c /etc/mosquitto/passwd esp32meter
# Enter password: meter@123

# Create configuration
sudo nano /etc/mosquitto/conf.d/meters.conf
```

Add the following configuration:

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
persistence true
persistence_location /var/lib/mosquitto/
```

```bash
# Restart Mosquitto
sudo systemctl restart mosquitto
sudo systemctl enable mosquitto

# Open firewall
sudo ufw allow 1883/tcp
```

#### 1.2 Install MySQL

```bash
# Install MySQL
sudo apt install -y mysql-server

# Secure installation
sudo mysql_secure_installation

# Create database
sudo mysql -u root -p < db_setup.sql
```

#### 1.3 Install Python Application

```bash
# Create directory
sudo mkdir -p /opt/ems
cd /opt/ems

# Copy files
cp main.py /opt/ems/
cp requirements.txt /opt/ems/

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Run application
python main.py
```

#### 1.4 Create Systemd Service

```bash
sudo nano /etc/systemd/system/ems.service
```

```ini
[Unit]
Description=Energy Monitoring System API
After=network.target mysql.service mosquitto.service

[Service]
Type=simple
User=root
WorkingDirectory=/opt/ems
ExecStart=/opt/ems/venv/bin/python main.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable ems
sudo systemctl start ems
```

### Step 2: ESP32 Setup

#### 2.1 Install Arduino IDE Libraries

1. Open Arduino IDE
2. Go to **Sketch → Include Library → Manage Libraries**
3. Install:
   - `PubSubClient` by Nick O'Leary
   - `ArduinoJson` by Benoit Blanchon
   - `ModbusMaster` by Doc Walker

#### 2.2 Configure ESP32

Edit `sketch_oct26a.ino`:

```cpp
// WiFi Settings
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Static IP (optional)
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// MQTT Settings
const char* mqtt_server = "YOUR_SERVER_IP";
const int mqtt_port = 1883;
const char* mqtt_user = "esp32meter";
const char* mqtt_password = "meter@123";

// =============================================================
// METER CONFIGURATION - Add/remove meters here
// =============================================================
#define MAX_METERS 10  // Maximum supported meters

// Define your meter slave IDs here - just add more IDs to expand
const uint8_t METER_SLAVE_IDS[] = {7, 10};  // Add more: {7, 10, 11, 12, ...}

// Example: To monitor 5 meters with IDs 1, 2, 3, 4, 5:
// const uint8_t METER_SLAVE_IDS[] = {1, 2, 3, 4, 5};
```

**Adding New Meters:**
1. Connect the new meter to the RS485 bus
2. Configure the meter's Modbus slave ID
3. Add the ID to the `METER_SLAVE_IDS` array
4. Upload the firmware - no other code changes needed!

#### 2.3 Upload Firmware

1. Select Board: **ESP32S3 Dev Module**
2. Select Port: Your ESP32 COM port
3. Click **Upload**

---

## Configuration

### ESP32 Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `readInterval` | 60000 | Meter reading interval (ms) |
| `publishInterval` | 300000 | MQTT publish interval (ms) |
| `MEMORY_BUFFER_SIZE` | 10 | Readings stored in RAM |
| `MAX_BUFFER_ENTRIES` | 500 | Readings stored in flash |
| `BUS_BAUD_RATE` | 9600 | Modbus baud rate |

### Server Configuration

Edit `main.py`:

```python
# Database Configuration
DB_CONFIG = {
    'host': 'localhost',
    'user': 'root',
    'password': 'your_password',
    'database': 'energy_monitoring',
    'pool_size': 10
}

# MQTT Configuration
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_USER = "esp32meter"
MQTT_PASSWORD = "meter@123"
```

---

## Database Schema

### Entity Relationship Diagram

```
┌─────────────┐       ┌──────────────────┐
│   devices   │       │  meter_readings  │
├─────────────┤       ├──────────────────┤
│ id (PK)     │       │ id (PK)          │
│ device_id   │◄──────│ device_id (FK)   │
│ device_mac  │       │ meter_id         │
│ ip_address  │       │ reading_timestamp│
│ rssi        │       │ server_timestamp │
│ status      │       │ voltage          │
│ last_seen   │       │ current          │
│ created_at  │       │ frequency        │
│ updated_at  │       │ power            │
└─────────────┘       │ power_factor     │
                      │ energy_kwh       │
                      │ reactive_power   │
                      │ apparent_power   │
                      └──────────────────┘
                               │
                               ▼
                      ┌──────────────────┐
                      │hourly_aggregates │
                      ├──────────────────┤
                      │ id (PK)          │
                      │ device_id        │
                      │ meter_id         │
                      │ hour_start       │
                      │ avg_voltage      │
                      │ avg_current      │
                      │ avg_power        │
                      │ max_power        │
                      │ min_power        │
                      │ total_energy     │
                      │ reading_count    │
                      └──────────────────┘
```

### Table Descriptions

#### devices
Stores registered ESP32 devices.

| Column | Type | Description |
|--------|------|-------------|
| id | INT | Auto-increment primary key |
| device_id | VARCHAR(50) | MAC address without colons |
| device_mac | VARCHAR(20) | MAC address with colons |
| ip_address | VARCHAR(45) | Last known IP |
| rssi | INT | WiFi signal strength (dBm) |
| status | ENUM | 'online' or 'offline' |
| last_seen | DATETIME | Last communication time |

#### meter_readings
Stores comprehensive 3-phase meter readings (58 parameters per meter).

| Category | Columns | Description |
|----------|---------|-------------|
| **Identity** | id, device_id, meter_id | Primary key and references |
| **Timestamps** | reading_timestamp, server_timestamp | When reading was taken/received |
| **Per-Phase Voltage** | voltage_l1, voltage_l2, voltage_l3, voltage_avg | Phase voltages (V) |
| **Line-to-Line Voltage** | voltage_l1l2, voltage_l2l3, voltage_l3l1, voltage_ll_avg | L-L voltages (V) |
| **Per-Phase Current** | current_l1, current_l2, current_l3, current_avg, current_total, current_neutral | Phase currents (A) |
| **Per-Phase Active Power** | power_l1, power_l2, power_l3, power_total | Active power (W) |
| **Per-Phase Apparent Power** | apparent_power_l1, apparent_power_l2, apparent_power_l3, apparent_power_total | Apparent power (VA) |
| **Per-Phase Reactive Power** | reactive_power_l1, reactive_power_l2, reactive_power_l3, reactive_power_total | Reactive power (VAr) |
| **Per-Phase Power Factor** | pf_l1, pf_l2, pf_l3, pf_total | Power factor (0-1) |
| **Per-Phase Phase Angle** | phase_angle_l1, phase_angle_l2, phase_angle_l3, phase_angle_total | Phase angle (degrees) |
| **Frequency** | frequency | System frequency (Hz) |
| **Energy** | energy_import, energy_export, energy_total, energy_reactive_import, energy_reactive_export, energy_reactive_total, energy_apparent_total | Energy values (kWh, kVArh, kVAh) |
| **Power Demand** | demand_current, demand_max | Power demand (W) |
| **Current Demand** | current_demand_l1, current_demand_l2, current_demand_l3, current_demand_max_l1, current_demand_max_l2, current_demand_max_l3 | Per-phase current demand (A) |
| **THD** | thd_voltage_l1, thd_voltage_l2, thd_voltage_l3, thd_voltage_avg, thd_current_l1, thd_current_l2, thd_current_l3, thd_current_avg | Total Harmonic Distortion (%) |
| **Legacy** | voltage, current, power, power_factor, energy_kwh, reactive_power, apparent_power | Backward compatibility |

---

## API Reference

### Base URL
```
http://YOUR_SERVER_IP:8000
```

### Endpoints

#### Health Check
```http
GET /
```

**Response:**
```json
{
  "status": "running",
  "service": "Energy Monitoring System",
  "version": "2.0.0",
  "mqtt": "connected"
}
```

#### Submit Meter Data
```http
POST /api/meterdata
Content-Type: application/json
```

**Request Body (Legacy Format):**
```json
{
  "deviceId": "AABBCCDDEEFF",
  "meter7": {
    "voltage": 230.5,
    "current": 5.2,
    "frequency": 50.0,
    "power": 1150.0,
    "pf": 0.95,
    "energy": 1234.56,
    "reactivePower": 350.0,
    "apparentPower": 1200.0
  },
  "meter10": {
    "voltage": 231.0,
    "current": 3.8,
    "frequency": 50.0,
    "power": 850.0,
    "pf": 0.97,
    "energy": 987.65,
    "reactivePower": 200.0,
    "apparentPower": 880.0
  }
}
```

**Request Body (Batch Format):**
```json
{
  "deviceId": "AABBCCDDEEFF",
  "deviceMac": "AA:BB:CC:DD:EE:FF",
  "ip": "192.168.1.100",
  "rssi": -65,
  "readings": [
    {
      "ts": 1705500000,
      "meter7": {"v": 230.5, "i": 5.2, "p": 1150, "e": 1234.56, "pf": 0.95, "f": 50.0},
      "meter10": {"v": 231.0, "i": 3.8, "p": 850, "e": 987.65, "pf": 0.97, "f": 50.0}
    }
  ]
}
```

**Response:**
```json
{
  "message": "Data received successfully",
  "readings_saved": 2
}
```

#### Get All Devices
```http
GET /api/devices
```

**Response:**
```json
{
  "devices": [
    {
      "device_id": "AABBCCDDEEFF",
      "device_mac": "AA:BB:CC:DD:EE:FF",
      "ip_address": "192.168.1.100",
      "rssi": -65,
      "status": "online",
      "last_seen": "2024-01-17T10:30:00"
    }
  ]
}
```

#### Get Device Readings
```http
GET /api/readings/{device_id}?limit=100&meter_id=7
```

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| device_id | string | Yes | Device MAC (no colons) |
| limit | int | No | Max readings (default: 100) |
| meter_id | int | No | Filter by meter (7 or 10) |

**Response:**
```json
{
  "device_id": "AABBCCDDEEFF",
  "readings": [
    {
      "meter_id": 7,
      "reading_timestamp": "2024-01-17T10:30:00",
      "voltage": 230.5,
      "current": 5.2,
      "power": 1150.0,
      "energy_kwh": 1234.56,
      "power_factor": 0.95,
      "frequency": 50.0
    }
  ]
}
```

#### Get Device Statistics
```http
GET /api/stats/{device_id}?meter_id=7
```

**Response:**
```json
{
  "device_id": "AABBCCDDEEFF",
  "meter_id": 7,
  "today": {
    "reading_count": 288,
    "avg_voltage": 230.2,
    "avg_current": 4.8,
    "avg_power": 1100.5,
    "max_power": 2500.0,
    "min_power": 150.0,
    "energy_consumed": 26.4,
    "avg_pf": 0.94
  },
  "hourly": [
    {"hour": "2024-01-17 00:00", "avg_power": 450.2, "energy": 0.45},
    {"hour": "2024-01-17 01:00", "avg_power": 380.5, "energy": 0.38}
  ]
}
```

---

## MQTT Topics

### Topic Structure

```
meters/
├── data                 ← Meter readings from ESP32
├── status/
│   └── {device_id}      ← Device online/offline status
└── commands/
    └── {device_id}      ← Commands to specific device
```

### Data Topic: `meters/data`

**Published by:** ESP32
**QoS:** 1 (At least once)

**Payload (Dynamic Multi-Meter Format with 58 Parameters per Meter):**
```json
{
  "deviceId": "AABBCCDDEEFF",
  "deviceMac": "AA:BB:CC:DD:EE:FF",
  "ip": "192.168.1.100",
  "rssi": -65,
  "ts": 1705500000,
  "meterCount": 3,
  "meter7": {
    "id": 7,
    "vL1": 230.5, "vL2": 231.0, "vL3": 229.8, "vAvg": 230.4,
    "vL1L2": 398.5, "vL2L3": 399.2, "vL3L1": 398.0, "vLLAvg": 398.6,
    "iL1": 5.2, "iL2": 4.8, "iL3": 5.0, "iAvg": 5.0, "iTotal": 15.0, "iN": 0.4,
    "pL1": 1150, "pL2": 1080, "pL3": 1100, "pTotal": 3330,
    "sL1": 1200, "sL2": 1120, "sL3": 1150, "sTotal": 3470,
    "qL1": 350, "qL2": 320, "qL3": 340, "qTotal": 1010,
    "pfL1": 0.96, "pfL2": 0.96, "pfL3": 0.96, "pfTotal": 0.96,
    "paL1": 15.5, "paL2": 15.2, "paL3": 15.8, "paTotal": 15.5,
    "freq": 50.0,
    "eImp": 1234.56, "eExp": 0, "eTotal": 1234.56,
    "erImp": 456.78, "erExp": 0, "erTotal": 456.78, "esTotal": 1320.5,
    "demand": 3200, "demandMax": 4500,
    "idL1": 5.0, "idL2": 4.6, "idL3": 4.8, "idMaxL1": 6.5, "idMaxL2": 6.2, "idMaxL3": 6.4,
    "thdVL1": 2.5, "thdVL2": 2.4, "thdVL3": 2.6, "thdVAvg": 2.5,
    "thdIL1": 8.2, "thdIL2": 7.8, "thdIL3": 8.0, "thdIAvg": 8.0
  },
  "meter10": {
    "id": 10,
    "vL1": 231.0, "vL2": 230.5, "vL3": 230.8, "vAvg": 230.8,
    ...
  },
  "meter11": {
    "id": 11,
    ...
  }
}
```

### Status Topic: `meters/status/{device_id}`

**Published by:** ESP32 (Last Will) / Server
**QoS:** 1
**Retained:** Yes

**Online Payload:**
```json
{
  "status": "online",
  "device": "AABBCCDDEEFF",
  "ip": "192.168.1.100"
}
```

**Offline Payload (Last Will):**
```json
{
  "status": "offline",
  "device": "AABBCCDDEEFF"
}
```

---

## Data Formats

### Field Abbreviations (MQTT) - 58 Parameters

#### Per-Phase Voltage (V)
| Short | Full Name |
|-------|-----------|
| vL1 | Voltage Phase L1 |
| vL2 | Voltage Phase L2 |
| vL3 | Voltage Phase L3 |
| vAvg | Average Voltage |

#### Line-to-Line Voltage (V)
| Short | Full Name |
|-------|-----------|
| vL1L2 | Voltage L1-L2 |
| vL2L3 | Voltage L2-L3 |
| vL3L1 | Voltage L3-L1 |
| vLLAvg | Average L-L Voltage |

#### Per-Phase Current (A)
| Short | Full Name |
|-------|-----------|
| iL1 | Current Phase L1 |
| iL2 | Current Phase L2 |
| iL3 | Current Phase L3 |
| iAvg | Average Current |
| iTotal | Total Current |
| iN | Neutral Current |

#### Per-Phase Power
| Short | Full Name | Unit |
|-------|-----------|------|
| pL1, pL2, pL3, pTotal | Active Power | W |
| sL1, sL2, sL3, sTotal | Apparent Power | VA |
| qL1, qL2, qL3, qTotal | Reactive Power | VAr |

#### Power Factor & Phase Angle
| Short | Full Name | Unit |
|-------|-----------|------|
| pfL1, pfL2, pfL3, pfTotal | Power Factor | 0-1 |
| paL1, paL2, paL3, paTotal | Phase Angle | degrees |

#### Energy Values
| Short | Full Name | Unit |
|-------|-----------|------|
| eImp | Import Active Energy | kWh |
| eExp | Export Active Energy | kWh |
| eTotal | Total Active Energy | kWh |
| erImp | Import Reactive Energy | kVArh |
| erExp | Export Reactive Energy | kVArh |
| erTotal | Total Reactive Energy | kVArh |
| esTotal | Total Apparent Energy | kVAh |

#### Demand Values
| Short | Full Name | Unit |
|-------|-----------|------|
| demand | Current Power Demand | W |
| demandMax | Maximum Power Demand | W |
| idL1, idL2, idL3 | Per-Phase Current Demand | A |
| idMaxL1, idMaxL2, idMaxL3 | Max Per-Phase Current Demand | A |

#### THD Values (%)
| Short | Full Name |
|-------|-----------|
| thdVL1, thdVL2, thdVL3 | Voltage THD per phase |
| thdVAvg | Average Voltage THD |
| thdIL1, thdIL2, thdIL3 | Current THD per phase |
| thdIAvg | Average Current THD |

#### Other
| Short | Full Name | Unit |
|-------|-----------|------|
| freq | Frequency | Hz |
| ts | Timestamp | Unix epoch |

### Modbus Register Map (Eastron SDM630 Compatible)

| Register | Parameter | Data Type |
|----------|-----------|-----------|
| 0-1 | Voltage L1 | Float32 |
| 2-3 | Voltage L2 | Float32 |
| 4-5 | Voltage L3 | Float32 |
| 6-7 | Current L1 | Float32 |
| 8-9 | Current L2 | Float32 |
| 10-11 | Current L3 | Float32 |
| 12-13 | Power L1 | Float32 |
| 14-15 | Power L2 | Float32 |
| 16-17 | Power L3 | Float32 |
| 18-19 | Apparent Power L1 | Float32 |
| 20-21 | Apparent Power L2 | Float32 |
| 22-23 | Apparent Power L3 | Float32 |
| 24-25 | Reactive Power L1 | Float32 |
| 26-27 | Reactive Power L2 | Float32 |
| 28-29 | Reactive Power L3 | Float32 |
| 30-31 | Power Factor L1 | Float32 |
| 32-33 | Power Factor L2 | Float32 |
| 34-35 | Power Factor L3 | Float32 |
| 36-37 | Phase Angle L1 | Float32 |
| 38-39 | Phase Angle L2 | Float32 |
| 40-41 | Phase Angle L3 | Float32 |
| 42-43 | Average Voltage | Float32 |
| 46-47 | Average Current | Float32 |
| 48-49 | Total Current | Float32 |
| 52-53 | Total Power | Float32 |
| 56-57 | Total Apparent Power | Float32 |
| 60-61 | Total Reactive Power | Float32 |
| 62-63 | Total Power Factor | Float32 |
| 66-67 | Total Phase Angle | Float32 |
| 70-71 | Frequency | Float32 |
| 72-73 | Import Active Energy | Float32 |
| 74-75 | Export Active Energy | Float32 |
| 76-77 | Import Reactive Energy | Float32 |
| 78-79 | Export Reactive Energy | Float32 |
| 80-81 | Total Apparent Energy | Float32 |
| 84-85 | Current Demand | Float32 |
| 86-87 | Max Demand | Float32 |
| 200-201 | Voltage L1-L2 | Float32 |
| 202-203 | Voltage L2-L3 | Float32 |
| 204-205 | Voltage L3-L1 | Float32 |
| 206-207 | Average L-L Voltage | Float32 |
| 224-225 | Neutral Current | Float32 |
| 234-235 | Voltage THD L1 | Float32 |
| 236-237 | Voltage THD L2 | Float32 |
| 238-239 | Voltage THD L3 | Float32 |
| 240-241 | Current THD L1 | Float32 |
| 242-243 | Current THD L2 | Float32 |
| 244-245 | Current THD L3 | Float32 |
| 248-249 | Average Voltage THD | Float32 |
| 250-251 | Average Current THD | Float32 |
| 258-259 | Current Demand L1 | Float32 |
| 260-261 | Current Demand L2 | Float32 |
| 262-263 | Current Demand L3 | Float32 |
| 264-265 | Max Current Demand L1 | Float32 |
| 266-267 | Max Current Demand L2 | Float32 |
| 268-269 | Max Current Demand L3 | Float32 |
| 342-343 | Total Active Energy | Float32 |
| 344-345 | Total Reactive Energy | Float32 |

---

## Troubleshooting

### ESP32 Issues

#### WiFi Connection Fails
```
Problem: ESP32 cannot connect to WiFi
Solutions:
1. Verify SSID and password
2. Check signal strength (should be > -80 dBm)
3. Ensure 2.4GHz network (not 5GHz)
4. Try static IP if DHCP fails
```

#### MQTT Connection Fails
```
Problem: MQTT connection refused
Solutions:
1. Verify broker IP and port
2. Check username/password
3. Ensure Mosquitto is running: sudo systemctl status mosquitto
4. Check firewall: sudo ufw status
5. Test from command line: mosquitto_pub -h IP -t test -m "hello" -u user -P pass
```

#### Modbus Read Errors
```
Problem: Meter readings return 0 or fail
Solutions:
1. Verify RS485 wiring (A/B not swapped)
2. Check baud rate matches meter setting
3. Confirm slave ID is correct
4. Add termination resistor if cable > 10m
5. Check power to RS485 module
```

### Server Issues

#### Database Connection Error
```
Problem: Cannot connect to MySQL
Solutions:
1. Check MySQL is running: sudo systemctl status mysql
2. Verify credentials in main.py
3. Check user permissions: GRANT ALL ON energy_monitoring.* TO 'user'@'localhost';
4. Test connection: mysql -u root -p energy_monitoring
```

#### API Not Responding
```
Problem: FastAPI not accessible
Solutions:
1. Check service status: sudo systemctl status ems
2. View logs: sudo journalctl -u ems -f
3. Verify port 8000 is open: sudo ufw allow 8000/tcp
4. Test locally: curl http://localhost:8000/
```

### Common Error Codes

| MQTT RC | Meaning |
|---------|---------|
| 0 | Connection successful |
| 1 | Incorrect protocol version |
| 2 | Invalid client ID |
| 3 | Server unavailable |
| 4 | Bad username/password |
| 5 | Not authorized |

---

## Maintenance

### Daily Tasks
- Monitor device status via `/api/devices`
- Check for offline devices

### Weekly Tasks
- Review error logs: `sudo journalctl -u ems --since "1 week ago"`
- Check disk space: `df -h`
- Verify backup integrity

### Monthly Tasks
- Update system packages: `sudo apt update && sudo apt upgrade`
- Review and archive old data
- Test backup restoration

### Database Maintenance

```sql
-- Check table sizes
SELECT table_name,
       ROUND(data_length/1024/1024, 2) as 'Data (MB)',
       ROUND(index_length/1024/1024, 2) as 'Index (MB)'
FROM information_schema.tables
WHERE table_schema = 'energy_monitoring';

-- Archive old readings (older than 1 year)
INSERT INTO meter_readings_archive
SELECT * FROM meter_readings
WHERE reading_timestamp < DATE_SUB(NOW(), INTERVAL 1 YEAR);

DELETE FROM meter_readings
WHERE reading_timestamp < DATE_SUB(NOW(), INTERVAL 1 YEAR);

-- Optimize tables
OPTIMIZE TABLE meter_readings;
OPTIMIZE TABLE hourly_aggregates;
```

### Backup Commands

```bash
# Backup database
mysqldump -u root -p energy_monitoring > backup_$(date +%Y%m%d).sql

# Backup with compression
mysqldump -u root -p energy_monitoring | gzip > backup_$(date +%Y%m%d).sql.gz

# Restore
mysql -u root -p energy_monitoring < backup_20240117.sql
```

---

## Support

For issues and feature requests:
- Check logs first
- Document error messages
- Note device IDs and timestamps
- Review this documentation

---

## Changelog

### Version 2.2.0 (Current)
- **Dynamic Multi-Meter Support**: Add any number of meters by editing one line
- Refactored ESP32 firmware for dynamic meter configuration
- Meters defined in simple array: `const uint8_t METER_SLAVE_IDS[] = {7, 10, 11, ...}`
- Backend automatically detects and processes any meter ID
- JSON payload includes meter count and ID in each meter object
- Support for up to 10 meters per ESP32 (configurable via MAX_METERS)
- Backward compatible with existing meter7/meter10 format

### Version 2.1.0
- **Comprehensive 3-Phase Monitoring**: Now reading 58 parameters per meter
- Added per-phase voltage, current, power, power factor, and phase angle readings
- Added line-to-line voltage measurements (L1-L2, L2-L3, L3-L1, Average)
- Added power quality metrics: Voltage THD and Current THD per phase
- Added per-phase current demand and max current demand
- Added total apparent energy (kVAh) tracking
- Added import/export energy tracking for bidirectional metering
- Updated database schema with 58+ columns per reading
- Full Eastron SDM630 register map support

### Version 2.0.0
- Added MQTT support with QoS 1
- Implemented local buffering on ESP32
- New database schema with device tracking
- Batch data upload support
- Automatic hourly aggregation
- Device status monitoring

### Version 1.0.0 (Legacy)
- Basic HTTP POST to API
- Single reading per request
- No offline buffering
- No device tracking

---

*Documentation generated for Energy Monitoring System v2.2*
*Last updated: January 2026*
