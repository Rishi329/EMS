from fastapi import FastAPI, HTTPException, BackgroundTasks
from fastapi.middleware.cors import CORSMiddleware
import mysql.connector
from mysql.connector import pooling
from pydantic import BaseModel
from typing import Optional, List
from datetime import datetime
import json
import threading
import paho.mqtt.client as mqtt
import logging
import os

# ============ Logging Setup ============
LOG_LEVEL = os.getenv('LOG_LEVEL', 'INFO')
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL.upper(), logging.INFO),
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# ============ Pydantic Models ============

# Single meter reading (short keys from MQTT) - Comprehensive 3-Phase (58 parameters)
class MeterReadingShort(BaseModel):
    # Per-Phase Voltages
    vL1: Optional[float] = 0
    vL2: Optional[float] = 0
    vL3: Optional[float] = 0
    vAvg: Optional[float] = 0

    # Line-to-Line Voltages
    vL1L2: Optional[float] = 0
    vL2L3: Optional[float] = 0
    vL3L1: Optional[float] = 0
    vLLAvg: Optional[float] = 0  # Average Line-to-Line Voltage

    # Per-Phase Currents
    iL1: Optional[float] = 0
    iL2: Optional[float] = 0
    iL3: Optional[float] = 0
    iAvg: Optional[float] = 0
    iTotal: Optional[float] = 0
    iN: Optional[float] = 0      # Neutral current

    # Per-Phase Active Power
    pL1: Optional[float] = 0
    pL2: Optional[float] = 0
    pL3: Optional[float] = 0
    pTotal: Optional[float] = 0

    # Per-Phase Apparent Power
    sL1: Optional[float] = 0
    sL2: Optional[float] = 0
    sL3: Optional[float] = 0
    sTotal: Optional[float] = 0

    # Per-Phase Reactive Power
    qL1: Optional[float] = 0
    qL2: Optional[float] = 0
    qL3: Optional[float] = 0
    qTotal: Optional[float] = 0

    # Per-Phase Power Factor
    pfL1: Optional[float] = 0
    pfL2: Optional[float] = 0
    pfL3: Optional[float] = 0
    pfTotal: Optional[float] = 0

    # Per-Phase Phase Angle (degrees)
    paL1: Optional[float] = 0
    paL2: Optional[float] = 0
    paL3: Optional[float] = 0
    paTotal: Optional[float] = 0

    # Frequency
    freq: Optional[float] = 0

    # Energy Values
    eImp: Optional[float] = 0     # Import Active Energy kWh
    eExp: Optional[float] = 0     # Export Active Energy kWh
    eTotal: Optional[float] = 0   # Total Active Energy kWh
    erImp: Optional[float] = 0    # Import Reactive Energy kVArh
    erExp: Optional[float] = 0    # Export Reactive Energy kVArh
    erTotal: Optional[float] = 0  # Total Reactive Energy kVArh
    esTotal: Optional[float] = 0  # Total Apparent Energy kVAh

    # Power Demand Values
    demand: Optional[float] = 0
    demandMax: Optional[float] = 0

    # Per-Phase Current Demand (A)
    idL1: Optional[float] = 0
    idL2: Optional[float] = 0
    idL3: Optional[float] = 0
    idMaxL1: Optional[float] = 0
    idMaxL2: Optional[float] = 0
    idMaxL3: Optional[float] = 0

    # THD Values
    thdVL1: Optional[float] = 0
    thdVL2: Optional[float] = 0
    thdVL3: Optional[float] = 0
    thdVAvg: Optional[float] = 0
    thdIL1: Optional[float] = 0
    thdIL2: Optional[float] = 0
    thdIL3: Optional[float] = 0
    thdIAvg: Optional[float] = 0

    # Legacy fields (backward compatibility)
    v: Optional[float] = 0
    i: Optional[float] = 0
    f: Optional[float] = 0
    p: Optional[float] = 0
    pf: Optional[float] = 0
    e: Optional[float] = 0
    rp: Optional[float] = 0
    ap: Optional[float] = 0

# Single meter reading (full keys - backward compatible)
class MeterReading(BaseModel):
    voltage: Optional[float] = 0
    current: Optional[float] = 0
    frequency: Optional[float] = 0
    power: Optional[float] = 0
    pf: Optional[float] = 0
    energy: Optional[float] = 0
    reactivePower: Optional[float] = 0
    apparentPower: Optional[float] = 0

# Single reading with timestamp (MQTT batch format)
class TimestampedReading(BaseModel):
    ts: int
    meter7: Optional[MeterReadingShort] = None
    meter10: Optional[MeterReadingShort] = None

# Batch data from MQTT (new format)
class MQTTBatchData(BaseModel):
    deviceId: str
    deviceMac: Optional[str] = None
    ip: Optional[str] = None
    rssi: Optional[int] = None
    readings: List[TimestampedReading]

# Legacy format (backward compatible with old ESP32 code)
class LegacyData(BaseModel):
    deviceId: Optional[str] = None
    meter7: MeterReading
    meter10: MeterReading

# ============ Database Configuration ============
DB_CONFIG = {
    'host': os.getenv('DB_HOST', 'localhost'),
    'port': int(os.getenv('DB_PORT', '3306')),
    'user': os.getenv('DB_USER', 'root'),
    'password': os.getenv('DB_PASSWORD', 'password'),
    'database': os.getenv('DB_NAME', 'energy_monitoring'),
    'pool_name': 'energy_pool',
    'pool_size': int(os.getenv('DB_POOL_SIZE', '10'))
}

# Create connection pool
try:
    connection_pool = pooling.MySQLConnectionPool(**DB_CONFIG)
    logger.info("Database connection pool created")
except Exception as e:
    logger.error(f"Failed to create connection pool: {e}")
    connection_pool = None

# ============ MQTT Configuration ============
MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost')
MQTT_PORT = int(os.getenv('MQTT_PORT', '1883'))
MQTT_USER = os.getenv('MQTT_USER', 'esp32meter')
MQTT_PASSWORD = os.getenv('MQTT_PASSWORD', 'meter@123')
MQTT_TOPIC = os.getenv('MQTT_TOPIC', 'meters/#')

mqtt_client = None

# ============ Database Functions ============

def get_db_connection():
    """Get connection from pool"""
    try:
        if connection_pool:
            return connection_pool.get_connection()
        return None
    except Exception as e:
        logger.error(f"Error getting connection: {e}")
        return None

def init_database():
    """Initialize/update database schema"""
    conn = get_db_connection()
    if not conn:
        logger.error("Cannot initialize database - no connection")
        return False

    cursor = conn.cursor()

    try:
        # Create devices table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS devices (
                id INT AUTO_INCREMENT PRIMARY KEY,
                device_id VARCHAR(50) UNIQUE NOT NULL,
                device_mac VARCHAR(20),
                ip_address VARCHAR(45),
                last_seen DATETIME,
                rssi INT,
                status ENUM('online', 'offline') DEFAULT 'online',
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
                INDEX idx_device_id (device_id),
                INDEX idx_status (status)
            )
        """)

        # Create meter_readings table (comprehensive 3-phase structure - 58 parameters)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS meter_readings (
                id BIGINT AUTO_INCREMENT PRIMARY KEY,
                device_id VARCHAR(50) NOT NULL,
                meter_id INT NOT NULL,
                reading_timestamp DATETIME NOT NULL,
                server_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,

                -- Per-Phase Voltages (V)
                voltage_l1 DECIMAL(10,2),
                voltage_l2 DECIMAL(10,2),
                voltage_l3 DECIMAL(10,2),
                voltage_avg DECIMAL(10,2),

                -- Line-to-Line Voltages (V)
                voltage_l1l2 DECIMAL(10,2),
                voltage_l2l3 DECIMAL(10,2),
                voltage_l3l1 DECIMAL(10,2),
                voltage_ll_avg DECIMAL(10,2),

                -- Per-Phase Currents (A)
                current_l1 DECIMAL(10,3),
                current_l2 DECIMAL(10,3),
                current_l3 DECIMAL(10,3),
                current_avg DECIMAL(10,3),
                current_total DECIMAL(10,3),
                current_neutral DECIMAL(10,3),

                -- Per-Phase Active Power (W)
                power_l1 DECIMAL(12,2),
                power_l2 DECIMAL(12,2),
                power_l3 DECIMAL(12,2),
                power_total DECIMAL(12,2),

                -- Per-Phase Apparent Power (VA)
                apparent_power_l1 DECIMAL(12,2),
                apparent_power_l2 DECIMAL(12,2),
                apparent_power_l3 DECIMAL(12,2),
                apparent_power_total DECIMAL(12,2),

                -- Per-Phase Reactive Power (VAr)
                reactive_power_l1 DECIMAL(12,2),
                reactive_power_l2 DECIMAL(12,2),
                reactive_power_l3 DECIMAL(12,2),
                reactive_power_total DECIMAL(12,2),

                -- Per-Phase Power Factor
                pf_l1 DECIMAL(5,3),
                pf_l2 DECIMAL(5,3),
                pf_l3 DECIMAL(5,3),
                pf_total DECIMAL(5,3),

                -- Per-Phase Phase Angle (degrees)
                phase_angle_l1 DECIMAL(6,2),
                phase_angle_l2 DECIMAL(6,2),
                phase_angle_l3 DECIMAL(6,2),
                phase_angle_total DECIMAL(6,2),

                -- Frequency (Hz)
                frequency DECIMAL(6,2),

                -- Energy Values (kWh, kVArh, kVAh)
                energy_import DECIMAL(12,3),
                energy_export DECIMAL(12,3),
                energy_total DECIMAL(12,3),
                energy_reactive_import DECIMAL(12,3),
                energy_reactive_export DECIMAL(12,3),
                energy_reactive_total DECIMAL(12,3),
                energy_apparent_total DECIMAL(12,3),

                -- Power Demand Values (W)
                demand_current DECIMAL(12,2),
                demand_max DECIMAL(12,2),

                -- Per-Phase Current Demand (A)
                current_demand_l1 DECIMAL(10,3),
                current_demand_l2 DECIMAL(10,3),
                current_demand_l3 DECIMAL(10,3),
                current_demand_max_l1 DECIMAL(10,3),
                current_demand_max_l2 DECIMAL(10,3),
                current_demand_max_l3 DECIMAL(10,3),

                -- THD Values (%)
                thd_voltage_l1 DECIMAL(6,2),
                thd_voltage_l2 DECIMAL(6,2),
                thd_voltage_l3 DECIMAL(6,2),
                thd_voltage_avg DECIMAL(6,2),
                thd_current_l1 DECIMAL(6,2),
                thd_current_l2 DECIMAL(6,2),
                thd_current_l3 DECIMAL(6,2),
                thd_current_avg DECIMAL(6,2),

                -- Legacy columns (backward compatibility)
                voltage DECIMAL(10,2),
                current DECIMAL(10,3),
                power DECIMAL(12,2),
                power_factor DECIMAL(5,3),
                energy_kwh DECIMAL(12,3),
                reactive_power DECIMAL(12,2),
                apparent_power DECIMAL(12,2),

                INDEX idx_device_meter (device_id, meter_id),
                INDEX idx_timestamp (reading_timestamp),
                INDEX idx_device_time (device_id, reading_timestamp),
                UNIQUE KEY unique_reading (device_id, meter_id, reading_timestamp),
                FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            )
        """)

        # Create energy_logs table (legacy - keep for backward compatibility)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS energy_logs (
                id BIGINT AUTO_INCREMENT PRIMARY KEY,
                meter_id INT NOT NULL,
                voltage DECIMAL(10,2),
                current DECIMAL(10,3),
                frequency DECIMAL(6,2),
                power DECIMAL(12,2),
                power_factor DECIMAL(5,3),
                energy_kwh DECIMAL(12,3),
                reactive_power DECIMAL(12,2),
                apparent_power DECIMAL(12,2),
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                INDEX idx_meter_id (meter_id),
                INDEX idx_created_at (created_at)
            )
        """)

        # Create hourly aggregates table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS hourly_aggregates (
                id BIGINT AUTO_INCREMENT PRIMARY KEY,
                device_id VARCHAR(50) NOT NULL,
                meter_id INT NOT NULL,
                hour_start DATETIME NOT NULL,
                avg_voltage DECIMAL(10,2),
                avg_current DECIMAL(10,3),
                avg_power DECIMAL(12,2),
                max_power DECIMAL(12,2),
                min_power DECIMAL(12,2),
                avg_pf DECIMAL(5,3),
                total_energy DECIMAL(12,3),
                reading_count INT,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                UNIQUE KEY unique_hour (device_id, meter_id, hour_start),
                INDEX idx_device_hour (device_id, hour_start)
            )
        """)

        conn.commit()
        logger.info("Database schema initialized successfully")
        return True

    except Exception as e:
        logger.error(f"Database initialization error: {e}")
        conn.rollback()
        return False
    finally:
        cursor.close()
        conn.close()

def save_reading(device_id: str, meter_id: int, timestamp: int, reading: dict):
    """Save a single meter reading with comprehensive 3-phase data (58 parameters)"""
    conn = get_db_connection()
    if not conn:
        return False

    cursor = conn.cursor()

    try:
        # Convert Unix timestamp to datetime
        reading_time = datetime.fromtimestamp(timestamp) if timestamp > 1000000000 else datetime.now()

        # ============ DEDUPLICATION CHECK ============
        # Skip if this exact reading already exists (prevents duplicates from retries)
        cursor.execute("""
            SELECT id FROM meter_readings
            WHERE device_id = %s AND meter_id = %s AND reading_timestamp = %s
            LIMIT 1
        """, (device_id, meter_id, reading_time))

        if cursor.fetchone():
            logger.debug(f"Duplicate reading skipped: device={device_id}, meter={meter_id}, time={reading_time}")
            return True  # Return True since data already exists (not an error)

        # Per-Phase Voltages
        voltage_l1 = reading.get('vL1', 0)
        voltage_l2 = reading.get('vL2', 0)
        voltage_l3 = reading.get('vL3', 0)
        voltage_avg = reading.get('vAvg', 0)

        # Line-to-Line Voltages
        voltage_l1l2 = reading.get('vL1L2', 0)
        voltage_l2l3 = reading.get('vL2L3', 0)
        voltage_l3l1 = reading.get('vL3L1', 0)
        voltage_ll_avg = reading.get('vLLAvg', 0)

        # Per-Phase Currents
        current_l1 = reading.get('iL1', 0)
        current_l2 = reading.get('iL2', 0)
        current_l3 = reading.get('iL3', 0)
        current_avg = reading.get('iAvg', 0)
        current_total = reading.get('iTotal', 0)
        current_neutral = reading.get('iN', 0)

        # Per-Phase Active Power
        power_l1 = reading.get('pL1', 0)
        power_l2 = reading.get('pL2', 0)
        power_l3 = reading.get('pL3', 0)
        power_total = reading.get('pTotal', 0)

        # Per-Phase Apparent Power
        apparent_power_l1 = reading.get('sL1', 0)
        apparent_power_l2 = reading.get('sL2', 0)
        apparent_power_l3 = reading.get('sL3', 0)
        apparent_power_total = reading.get('sTotal', 0)

        # Per-Phase Reactive Power
        reactive_power_l1 = reading.get('qL1', 0)
        reactive_power_l2 = reading.get('qL2', 0)
        reactive_power_l3 = reading.get('qL3', 0)
        reactive_power_total = reading.get('qTotal', 0)

        # Per-Phase Power Factor
        pf_l1 = reading.get('pfL1', 0)
        pf_l2 = reading.get('pfL2', 0)
        pf_l3 = reading.get('pfL3', 0)
        pf_total = reading.get('pfTotal', 0)

        # Per-Phase Phase Angle (degrees)
        phase_angle_l1 = reading.get('paL1', 0)
        phase_angle_l2 = reading.get('paL2', 0)
        phase_angle_l3 = reading.get('paL3', 0)
        phase_angle_total = reading.get('paTotal', 0)

        # Frequency
        frequency = reading.get('freq', reading.get('f', 0))

        # Energy Values
        energy_import = reading.get('eImp', 0)
        energy_export = reading.get('eExp', 0)
        energy_total = reading.get('eTotal', 0)
        energy_reactive_import = reading.get('erImp', 0)
        energy_reactive_export = reading.get('erExp', 0)
        energy_reactive_total = reading.get('erTotal', 0)
        energy_apparent_total = reading.get('esTotal', 0)

        # Power Demand Values
        demand_current = reading.get('demand', 0)
        demand_max = reading.get('demandMax', 0)

        # Per-Phase Current Demand (A)
        current_demand_l1 = reading.get('idL1', 0)
        current_demand_l2 = reading.get('idL2', 0)
        current_demand_l3 = reading.get('idL3', 0)
        current_demand_max_l1 = reading.get('idMaxL1', 0)
        current_demand_max_l2 = reading.get('idMaxL2', 0)
        current_demand_max_l3 = reading.get('idMaxL3', 0)

        # THD Values
        thd_voltage_l1 = reading.get('thdVL1', 0)
        thd_voltage_l2 = reading.get('thdVL2', 0)
        thd_voltage_l3 = reading.get('thdVL3', 0)
        thd_voltage_avg = reading.get('thdVAvg', 0)
        thd_current_l1 = reading.get('thdIL1', 0)
        thd_current_l2 = reading.get('thdIL2', 0)
        thd_current_l3 = reading.get('thdIL3', 0)
        thd_current_avg = reading.get('thdIAvg', 0)

        # Legacy fields (backward compatibility)
        voltage_legacy = reading.get('v', reading.get('voltage', voltage_l1))
        current_legacy = reading.get('i', reading.get('current', current_l1))
        power_legacy = reading.get('p', reading.get('power', power_total))
        pf_legacy = reading.get('pf', reading.get('power_factor', pf_total))
        energy_legacy = reading.get('e', reading.get('energy', energy_import))
        reactive_legacy = reading.get('rp', reading.get('reactivePower', reactive_power_total))
        apparent_legacy = reading.get('ap', reading.get('apparentPower', apparent_power_total))

        cursor.execute("""
            INSERT INTO meter_readings
            (device_id, meter_id, reading_timestamp,
             voltage_l1, voltage_l2, voltage_l3, voltage_avg,
             voltage_l1l2, voltage_l2l3, voltage_l3l1, voltage_ll_avg,
             current_l1, current_l2, current_l3, current_avg, current_total, current_neutral,
             power_l1, power_l2, power_l3, power_total,
             apparent_power_l1, apparent_power_l2, apparent_power_l3, apparent_power_total,
             reactive_power_l1, reactive_power_l2, reactive_power_l3, reactive_power_total,
             pf_l1, pf_l2, pf_l3, pf_total,
             phase_angle_l1, phase_angle_l2, phase_angle_l3, phase_angle_total,
             frequency,
             energy_import, energy_export, energy_total,
             energy_reactive_import, energy_reactive_export, energy_reactive_total, energy_apparent_total,
             demand_current, demand_max,
             current_demand_l1, current_demand_l2, current_demand_l3,
             current_demand_max_l1, current_demand_max_l2, current_demand_max_l3,
             thd_voltage_l1, thd_voltage_l2, thd_voltage_l3, thd_voltage_avg,
             thd_current_l1, thd_current_l2, thd_current_l3, thd_current_avg,
             voltage, current, power, power_factor, energy_kwh, reactive_power, apparent_power)
            VALUES (%s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s,
                    %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s,
                    %s, %s, %s,
                    %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s,
                    %s, %s, %s, %s, %s, %s, %s)
        """, (
            device_id, meter_id, reading_time,
            voltage_l1, voltage_l2, voltage_l3, voltage_avg,
            voltage_l1l2, voltage_l2l3, voltage_l3l1, voltage_ll_avg,
            current_l1, current_l2, current_l3, current_avg, current_total, current_neutral,
            power_l1, power_l2, power_l3, power_total,
            apparent_power_l1, apparent_power_l2, apparent_power_l3, apparent_power_total,
            reactive_power_l1, reactive_power_l2, reactive_power_l3, reactive_power_total,
            pf_l1, pf_l2, pf_l3, pf_total,
            phase_angle_l1, phase_angle_l2, phase_angle_l3, phase_angle_total,
            frequency,
            energy_import, energy_export, energy_total,
            energy_reactive_import, energy_reactive_export, energy_reactive_total, energy_apparent_total,
            demand_current, demand_max,
            current_demand_l1, current_demand_l2, current_demand_l3,
            current_demand_max_l1, current_demand_max_l2, current_demand_max_l3,
            thd_voltage_l1, thd_voltage_l2, thd_voltage_l3, thd_voltage_avg,
            thd_current_l1, thd_current_l2, thd_current_l3, thd_current_avg,
            voltage_legacy, current_legacy, power_legacy, pf_legacy, energy_legacy, reactive_legacy, apparent_legacy
        ))

        conn.commit()
        return True

    except mysql.connector.IntegrityError as e:
        # Handle duplicate key error (unique constraint violation)
        if e.errno == 1062:  # Duplicate entry error code
            logger.debug(f"Duplicate reading (constraint): device={device_id}, meter={meter_id}")
            return True  # Not an error - data already exists
        logger.error(f"Integrity error saving reading: {e}")
        conn.rollback()
        return False
    except Exception as e:
        logger.error(f"Error saving reading: {e}")
        conn.rollback()
        return False
    finally:
        cursor.close()
        conn.close()

def update_device(device_id: str, mac: str = None, ip: str = None, rssi: int = None):
    """Update or create device record"""
    conn = get_db_connection()
    if not conn:
        return False

    cursor = conn.cursor()

    try:
        cursor.execute("""
            INSERT INTO devices (device_id, device_mac, ip_address, rssi, last_seen, status)
            VALUES (%s, %s, %s, %s, NOW(), 'online')
            ON DUPLICATE KEY UPDATE
                device_mac = COALESCE(%s, device_mac),
                ip_address = COALESCE(%s, ip_address),
                rssi = COALESCE(%s, rssi),
                last_seen = NOW(),
                status = 'online'
        """, (device_id, mac, ip, rssi, mac, ip, rssi))

        conn.commit()
        return True

    except Exception as e:
        logger.error(f"Error updating device: {e}")
        conn.rollback()
        return False
    finally:
        cursor.close()
        conn.close()

def save_batch_readings(data: dict):
    """Save batch of readings from MQTT - supports dynamic number of meters"""
    device_id = data.get('deviceId', data.get('device', 'unknown'))
    mac = data.get('deviceMac')
    ip = data.get('ip')
    rssi = data.get('rssi')

    # Update device info
    update_device(device_id, mac, ip, rssi)

    # Get readings array
    readings = data.get('readings', [])

    # If no readings array, treat the whole object as a single reading
    if not readings:
        readings = [data]

    saved_count = 0
    for reading in readings:
        ts = reading.get('ts', int(datetime.now().timestamp()))

        # Dynamically detect and process all meter keys
        # Looks for keys like "meter7", "meter10", "m7", "m10", etc.
        for key in reading.keys():
            meter_data = None
            meter_id = None

            # Handle "meter<ID>" format (e.g., meter7, meter10, meter11)
            if key.startswith('meter') and key[5:].isdigit():
                meter_id = int(key[5:])
                meter_data = reading[key]

            # Handle "m<ID>" format (e.g., m7, m10 - used in file buffer)
            elif key.startswith('m') and len(key) > 1 and key[1:].isdigit():
                meter_id = int(key[1:])
                meter_data = reading[key]

            # Save if we found valid meter data
            if meter_data and meter_id is not None:
                # If meter data has an 'id' field, use that instead
                if 'id' in meter_data:
                    meter_id = int(meter_data['id'])

                if save_reading(device_id, meter_id, ts, meter_data):
                    saved_count += 1
                    logger.debug(f"Saved meter {meter_id} reading for device {device_id}")

    logger.info(f"Saved {saved_count} readings for device {device_id}")
    return saved_count

# ============ MQTT Functions ============

def on_mqtt_connect(client, userdata, flags, rc):
    """MQTT connection callback"""
    if rc == 0:
        logger.info("Connected to MQTT broker")
        client.subscribe(MQTT_TOPIC, qos=1)
        logger.info(f"Subscribed to: {MQTT_TOPIC}")
    else:
        logger.error(f"MQTT connection failed: {rc}")

def on_mqtt_message(client, userdata, msg):
    """MQTT message callback"""
    try:
        topic = msg.topic
        payload = msg.payload.decode('utf-8')

        logger.info(f"MQTT message on {topic}: {len(payload)} bytes")

        # Parse JSON
        data = json.loads(payload)

        # Handle status messages
        if 'status' in topic:
            device = data.get('device', 'unknown')
            status = data.get('status', 'unknown')
            logger.info(f"Device {device} status: {status}")

            # Update device status in database
            conn = get_db_connection()
            if conn:
                cursor = conn.cursor()
                cursor.execute(
                    "UPDATE devices SET status = %s, last_seen = NOW() WHERE device_id = %s",
                    (status, device)
                )
                conn.commit()
                cursor.close()
                conn.close()
            return

        # Handle data messages
        save_batch_readings(data)

    except json.JSONDecodeError as e:
        logger.error(f"Invalid JSON in MQTT message: {e}")
    except Exception as e:
        logger.error(f"Error processing MQTT message: {e}")

def start_mqtt_client():
    """Start MQTT client in background thread"""
    global mqtt_client

    mqtt_client = mqtt.Client(client_id="ems_api_server")
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    mqtt_client.on_connect = on_mqtt_connect
    mqtt_client.on_message = on_mqtt_message

    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        mqtt_client.loop_start()
        logger.info("MQTT client started")
    except Exception as e:
        logger.error(f"Failed to start MQTT client: {e}")

# ============ FastAPI App ============

app = FastAPI(
    title="Energy Monitoring System API",
    description="API for ESP32 Energy Meters with MQTT support",
    version="2.0.0"
)

# CORS middleware - allow frontend to connect
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # In production, specify exact origins
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.on_event("startup")
async def startup_event():
    """Initialize on startup"""
    logger.info("Starting Energy Monitoring System...")

    # Initialize database
    init_database()

    # Start MQTT client
    start_mqtt_client()

    logger.info("EMS API started successfully")

@app.on_event("shutdown")
async def shutdown_event():
    """Cleanup on shutdown"""
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
    logger.info("EMS API shutdown complete")

# ============ API Endpoints ============

@app.get("/")
def read_root():
    """Health check endpoint"""
    return {
        "status": "running",
        "service": "Energy Monitoring System",
        "version": "2.0.0",
        "mqtt": "connected" if mqtt_client and mqtt_client.is_connected() else "disconnected"
    }

@app.post("/api/meterdata")
async def save_meter_data(data: dict):
    """
    Universal endpoint - handles both legacy and new MQTT format.
    Accepts any JSON and auto-detects format.
    """
    logger.info(f"Received HTTP data: {json.dumps(data)[:200]}...")

    try:
        saved = save_batch_readings(data)
        return {"message": "Data received successfully", "readings_saved": saved}
    except Exception as e:
        logger.error(f"Error processing data: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/meterdata/batch")
async def save_batch_data(data: MQTTBatchData):
    """Endpoint specifically for MQTT batch format"""
    logger.info(f"Received batch data from {data.deviceId}: {len(data.readings)} readings")

    try:
        saved = save_batch_readings(data.model_dump())
        return {"message": "Batch data saved", "readings_saved": saved}
    except Exception as e:
        logger.error(f"Error processing batch: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/api/devices")
async def get_devices():
    """Get all registered devices"""
    conn = get_db_connection()
    if not conn:
        raise HTTPException(status_code=500, detail="Database connection failed")

    cursor = conn.cursor(dictionary=True)

    try:
        cursor.execute("""
            SELECT device_id, device_mac, ip_address, rssi, status, last_seen
            FROM devices
            ORDER BY last_seen DESC
        """)
        devices = cursor.fetchall()
        return {"devices": devices}
    finally:
        cursor.close()
        conn.close()

@app.get("/api/readings/{device_id}")
async def get_readings(device_id: str, limit: int = 100, meter_id: Optional[int] = None):
    """Get recent readings for a device with full 3-phase data"""
    conn = get_db_connection()
    if not conn:
        raise HTTPException(status_code=500, detail="Database connection failed")

    cursor = conn.cursor(dictionary=True)

    try:
        query = """
            SELECT
                meter_id,
                reading_timestamp,
                voltage_l1 as voltage_l1_n,
                voltage_l2 as voltage_l2_n,
                voltage_l3 as voltage_l3_n,
                voltage_avg as voltage_avg_ln,
                current_l1,
                current_l2,
                current_l3,
                current_neutral as current_n,
                current_avg,
                power_l1 as active_power_l1,
                power_l2 as active_power_l2,
                power_l3 as active_power_l3,
                power_total as total_active_power,
                reactive_power_total as total_reactive_power,
                apparent_power_total as total_apparent_power,
                pf_l1 as power_factor_l1,
                pf_l2 as power_factor_l2,
                pf_l3 as power_factor_l3,
                pf_total as power_factor,
                energy_import as total_kwh_import,
                energy_export as total_kwh_export,
                frequency,
                thd_voltage_l1,
                thd_voltage_l2,
                thd_voltage_l3,
                thd_current_l1,
                thd_current_l2,
                thd_current_l3
            FROM meter_readings
            WHERE device_id = %s
        """
        params = [device_id]

        if meter_id:
            query += " AND meter_id = %s"
            params.append(meter_id)

        query += " ORDER BY reading_timestamp DESC LIMIT %s"
        params.append(limit)

        cursor.execute(query, params)
        readings = cursor.fetchall()

        # Convert datetime to string and rename to 'timestamp' for frontend
        for r in readings:
            if r['reading_timestamp']:
                r['timestamp'] = r['reading_timestamp'].isoformat()
                del r['reading_timestamp']

        return {"device_id": device_id, "readings": readings}
    finally:
        cursor.close()
        conn.close()

@app.get("/api/stats/{device_id}")
async def get_device_stats(device_id: str, meter_id: int = 7):
    """Get statistics for a device"""
    conn = get_db_connection()
    if not conn:
        raise HTTPException(status_code=500, detail="Database connection failed")

    cursor = conn.cursor(dictionary=True)

    try:
        # Get the latest reading for real-time power and power factor
        cursor.execute("""
            SELECT power_total, pf_total
            FROM meter_readings
            WHERE device_id = %s AND meter_id = %s
            ORDER BY reading_timestamp DESC LIMIT 1
        """, (device_id, meter_id))

        latest_row = cursor.fetchone()
        latest = {
            "total_active_power": float(latest_row['power_total'] or 0) if latest_row else 0,
            "power_factor": float(latest_row['pf_total'] or 0) if latest_row else 0
        }

        # Today's stats
        cursor.execute("""
            SELECT
                MAX(energy_import) - MIN(energy_import) as total_kwh_import,
                MAX(energy_export) - MIN(energy_export) as total_kwh_export,
                COUNT(*) as reading_count,
                AVG(voltage_avg) as avg_voltage,
                AVG(power_total) as avg_power,
                MAX(power_total) as max_power,
                AVG(pf_total) as avg_pf
            FROM meter_readings
            WHERE device_id = %s AND meter_id = %s
            AND DATE(reading_timestamp) = CURDATE()
        """, (device_id, meter_id))

        today_row = cursor.fetchone()
        today = {
            "total_kwh_import": float(today_row['total_kwh_import'] or 0) if today_row else 0,
            "total_kwh_export": float(today_row['total_kwh_export'] or 0) if today_row else 0,
            "reading_count": today_row['reading_count'] if today_row else 0,
            "avg_voltage": float(today_row['avg_voltage'] or 0) if today_row else 0,
            "avg_power": float(today_row['avg_power'] or 0) if today_row else 0,
            "max_power": float(today_row['max_power'] or 0) if today_row else 0,
            "avg_pf": float(today_row['avg_pf'] or 0) if today_row else 0
        }

        # Last 24 hours hourly breakdown
        cursor.execute("""
            SELECT
                DATE_FORMAT(reading_timestamp, '%%Y-%%m-%%d %%H:00') as hour,
                AVG(power_total) as avg_power,
                MAX(energy_import) - MIN(energy_import) as energy
            FROM meter_readings
            WHERE device_id = %s AND meter_id = %s
            AND reading_timestamp >= NOW() - INTERVAL 24 HOUR
            GROUP BY DATE_FORMAT(reading_timestamp, '%%Y-%%m-%%d %%H:00')
            ORDER BY hour
        """, (device_id, meter_id))

        hourly = cursor.fetchall()

        return {
            "device_id": device_id,
            "meter_id": meter_id,
            "latest": latest,
            "today": today,
            "hourly": hourly
        }
    finally:
        cursor.close()
        conn.close()


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
