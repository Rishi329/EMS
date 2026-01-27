-- ============================================
-- Energy Monitoring System - Database Setup
-- Run this on your MySQL server
-- ============================================

-- Create database
CREATE DATABASE IF NOT EXISTS energy_monitoring;
USE energy_monitoring;

-- ============================================
-- Table: devices
-- Stores information about each ESP32 device
-- ============================================
CREATE TABLE IF NOT EXISTS devices (
    id INT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(50) UNIQUE NOT NULL COMMENT 'MAC address without colons',
    device_mac VARCHAR(20) COMMENT 'MAC address with colons',
    ip_address VARCHAR(45) COMMENT 'Last known IP address',
    last_seen DATETIME COMMENT 'Last communication time',
    rssi INT COMMENT 'WiFi signal strength',
    status ENUM('online', 'offline') DEFAULT 'online',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    INDEX idx_device_id (device_id),
    INDEX idx_status (status),
    INDEX idx_last_seen (last_seen)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Table: meter_readings
-- Comprehensive 3-phase meter readings (58 parameters)
-- ============================================
CREATE TABLE IF NOT EXISTS meter_readings (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(50) NOT NULL COMMENT 'Reference to devices table',
    meter_id INT NOT NULL COMMENT 'Meter ID (7 or 10)',
    reading_timestamp DATETIME NOT NULL COMMENT 'When reading was taken on ESP32',
    server_timestamp DATETIME DEFAULT CURRENT_TIMESTAMP COMMENT 'When received by server',

    -- Per-Phase Voltages (V)
    voltage_l1 DECIMAL(10,2) COMMENT 'Phase L1 Voltage',
    voltage_l2 DECIMAL(10,2) COMMENT 'Phase L2 Voltage',
    voltage_l3 DECIMAL(10,2) COMMENT 'Phase L3 Voltage',
    voltage_avg DECIMAL(10,2) COMMENT 'Average Voltage',

    -- Line-to-Line Voltages (V)
    voltage_l1l2 DECIMAL(10,2) COMMENT 'L1-L2 Voltage',
    voltage_l2l3 DECIMAL(10,2) COMMENT 'L2-L3 Voltage',
    voltage_l3l1 DECIMAL(10,2) COMMENT 'L3-L1 Voltage',
    voltage_ll_avg DECIMAL(10,2) COMMENT 'Average L-L Voltage',

    -- Per-Phase Currents (A)
    current_l1 DECIMAL(10,3) COMMENT 'Phase L1 Current',
    current_l2 DECIMAL(10,3) COMMENT 'Phase L2 Current',
    current_l3 DECIMAL(10,3) COMMENT 'Phase L3 Current',
    current_avg DECIMAL(10,3) COMMENT 'Average Current',
    current_total DECIMAL(10,3) COMMENT 'Total Current',
    current_neutral DECIMAL(10,3) COMMENT 'Neutral Current',

    -- Per-Phase Active Power (W)
    power_l1 DECIMAL(12,2) COMMENT 'Phase L1 Active Power',
    power_l2 DECIMAL(12,2) COMMENT 'Phase L2 Active Power',
    power_l3 DECIMAL(12,2) COMMENT 'Phase L3 Active Power',
    power_total DECIMAL(12,2) COMMENT 'Total Active Power',

    -- Per-Phase Apparent Power (VA)
    apparent_power_l1 DECIMAL(12,2) COMMENT 'Phase L1 Apparent Power',
    apparent_power_l2 DECIMAL(12,2) COMMENT 'Phase L2 Apparent Power',
    apparent_power_l3 DECIMAL(12,2) COMMENT 'Phase L3 Apparent Power',
    apparent_power_total DECIMAL(12,2) COMMENT 'Total Apparent Power',

    -- Per-Phase Reactive Power (VAr)
    reactive_power_l1 DECIMAL(12,2) COMMENT 'Phase L1 Reactive Power',
    reactive_power_l2 DECIMAL(12,2) COMMENT 'Phase L2 Reactive Power',
    reactive_power_l3 DECIMAL(12,2) COMMENT 'Phase L3 Reactive Power',
    reactive_power_total DECIMAL(12,2) COMMENT 'Total Reactive Power',

    -- Per-Phase Power Factor
    pf_l1 DECIMAL(5,3) COMMENT 'Phase L1 Power Factor',
    pf_l2 DECIMAL(5,3) COMMENT 'Phase L2 Power Factor',
    pf_l3 DECIMAL(5,3) COMMENT 'Phase L3 Power Factor',
    pf_total DECIMAL(5,3) COMMENT 'Total Power Factor',

    -- Per-Phase Phase Angle (degrees)
    phase_angle_l1 DECIMAL(6,2) COMMENT 'Phase Angle L1',
    phase_angle_l2 DECIMAL(6,2) COMMENT 'Phase Angle L2',
    phase_angle_l3 DECIMAL(6,2) COMMENT 'Phase Angle L3',
    phase_angle_total DECIMAL(6,2) COMMENT 'Total Phase Angle',

    -- Frequency (Hz)
    frequency DECIMAL(6,2) COMMENT 'Frequency in Hz',

    -- Energy Values (kWh, kVArh, kVAh)
    energy_import DECIMAL(12,3) COMMENT 'Import Active Energy kWh',
    energy_export DECIMAL(12,3) COMMENT 'Export Active Energy kWh',
    energy_total DECIMAL(12,3) COMMENT 'Total Active Energy kWh',
    energy_reactive_import DECIMAL(12,3) COMMENT 'Import Reactive Energy kVArh',
    energy_reactive_export DECIMAL(12,3) COMMENT 'Export Reactive Energy kVArh',
    energy_reactive_total DECIMAL(12,3) COMMENT 'Total Reactive Energy kVArh',
    energy_apparent_total DECIMAL(12,3) COMMENT 'Total Apparent Energy kVAh',

    -- Power Demand Values (W)
    demand_current DECIMAL(12,2) COMMENT 'Current Demand',
    demand_max DECIMAL(12,2) COMMENT 'Maximum Demand',

    -- Per-Phase Current Demand (A)
    current_demand_l1 DECIMAL(10,3) COMMENT 'Current Demand L1',
    current_demand_l2 DECIMAL(10,3) COMMENT 'Current Demand L2',
    current_demand_l3 DECIMAL(10,3) COMMENT 'Current Demand L3',
    current_demand_max_l1 DECIMAL(10,3) COMMENT 'Max Current Demand L1',
    current_demand_max_l2 DECIMAL(10,3) COMMENT 'Max Current Demand L2',
    current_demand_max_l3 DECIMAL(10,3) COMMENT 'Max Current Demand L3',

    -- THD Values (%)
    thd_voltage_l1 DECIMAL(6,2) COMMENT 'Voltage THD L1',
    thd_voltage_l2 DECIMAL(6,2) COMMENT 'Voltage THD L2',
    thd_voltage_l3 DECIMAL(6,2) COMMENT 'Voltage THD L3',
    thd_voltage_avg DECIMAL(6,2) COMMENT 'Average Voltage THD',
    thd_current_l1 DECIMAL(6,2) COMMENT 'Current THD L1',
    thd_current_l2 DECIMAL(6,2) COMMENT 'Current THD L2',
    thd_current_l3 DECIMAL(6,2) COMMENT 'Current THD L3',
    thd_current_avg DECIMAL(6,2) COMMENT 'Average Current THD',

    -- Legacy columns (backward compatibility)
    voltage DECIMAL(10,2) COMMENT 'Legacy: Voltage',
    current DECIMAL(10,3) COMMENT 'Legacy: Current',
    power DECIMAL(12,2) COMMENT 'Legacy: Power',
    power_factor DECIMAL(5,3) COMMENT 'Legacy: Power Factor',
    energy_kwh DECIMAL(12,3) COMMENT 'Legacy: Energy',
    reactive_power DECIMAL(12,2) COMMENT 'Legacy: Reactive Power',
    apparent_power DECIMAL(12,2) COMMENT 'Legacy: Apparent Power',

    -- Indexes for fast queries
    INDEX idx_device_meter (device_id, meter_id),
    INDEX idx_timestamp (reading_timestamp),
    INDEX idx_device_time (device_id, reading_timestamp),
    INDEX idx_server_time (server_timestamp),

    -- Unique constraint for deduplication (prevents duplicate readings)
    UNIQUE KEY unique_reading (device_id, meter_id, reading_timestamp),

    -- Foreign key
    FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Table: hourly_aggregates
-- Pre-calculated hourly statistics
-- ============================================
CREATE TABLE IF NOT EXISTS hourly_aggregates (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(50) NOT NULL,
    meter_id INT NOT NULL,
    hour_start DATETIME NOT NULL COMMENT 'Start of the hour',

    -- Aggregated values
    avg_voltage DECIMAL(10,2),
    avg_current DECIMAL(10,3),
    avg_power DECIMAL(12,2),
    max_power DECIMAL(12,2),
    min_power DECIMAL(12,2),
    avg_pf DECIMAL(5,3),
    total_energy DECIMAL(12,3) COMMENT 'Energy consumed in this hour',
    reading_count INT COMMENT 'Number of readings in this hour',

    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY unique_hour (device_id, meter_id, hour_start),
    INDEX idx_device_hour (device_id, hour_start)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Table: daily_aggregates
-- Pre-calculated daily statistics
-- ============================================
CREATE TABLE IF NOT EXISTS daily_aggregates (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(50) NOT NULL,
    meter_id INT NOT NULL,
    date DATE NOT NULL,

    -- Daily stats
    avg_voltage DECIMAL(10,2),
    avg_current DECIMAL(10,3),
    avg_power DECIMAL(12,2),
    max_power DECIMAL(12,2),
    min_power DECIMAL(12,2),
    peak_power_time TIME COMMENT 'Time of peak power',
    avg_pf DECIMAL(5,3),
    total_energy DECIMAL(12,3) COMMENT 'Total energy consumed',
    reading_count INT,

    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY unique_day (device_id, meter_id, date),
    INDEX idx_device_date (device_id, date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Table: alerts
-- Store alerts/notifications
-- ============================================
CREATE TABLE IF NOT EXISTS alerts (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(50) NOT NULL,
    meter_id INT,
    alert_type ENUM('high_power', 'low_voltage', 'high_current', 'offline', 'low_pf') NOT NULL,
    message TEXT,
    value DECIMAL(12,2) COMMENT 'The value that triggered alert',
    threshold DECIMAL(12,2) COMMENT 'The threshold that was exceeded',
    acknowledged BOOLEAN DEFAULT FALSE,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    acknowledged_at DATETIME,

    INDEX idx_device_alert (device_id, alert_type),
    INDEX idx_unacknowledged (acknowledged, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Table: energy_logs (Legacy - backward compatible)
-- Keep for old data compatibility
-- ============================================
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================
-- Stored Procedure: Calculate hourly aggregates
-- ============================================
DELIMITER //

CREATE PROCEDURE IF NOT EXISTS calculate_hourly_aggregates()
BEGIN
    INSERT INTO hourly_aggregates
        (device_id, meter_id, hour_start, avg_voltage, avg_current, avg_power,
         max_power, min_power, avg_pf, total_energy, reading_count)
    SELECT
        device_id,
        meter_id,
        DATE_FORMAT(reading_timestamp, '%Y-%m-%d %H:00:00') as hour_start,
        AVG(voltage),
        AVG(current),
        AVG(power),
        MAX(power),
        MIN(power),
        AVG(power_factor),
        MAX(energy_kwh) - MIN(energy_kwh),
        COUNT(*)
    FROM meter_readings
    WHERE reading_timestamp >= NOW() - INTERVAL 2 HOUR
    GROUP BY device_id, meter_id, DATE_FORMAT(reading_timestamp, '%Y-%m-%d %H:00:00')
    ON DUPLICATE KEY UPDATE
        avg_voltage = VALUES(avg_voltage),
        avg_current = VALUES(avg_current),
        avg_power = VALUES(avg_power),
        max_power = VALUES(max_power),
        min_power = VALUES(min_power),
        avg_pf = VALUES(avg_pf),
        total_energy = VALUES(total_energy),
        reading_count = VALUES(reading_count);
END //

DELIMITER ;

-- ============================================
-- Event: Auto-calculate aggregates every hour
-- ============================================
SET GLOBAL event_scheduler = ON;

CREATE EVENT IF NOT EXISTS hourly_aggregate_event
ON SCHEDULE EVERY 1 HOUR
STARTS (TIMESTAMP(CURRENT_DATE) + INTERVAL 1 HOUR)
DO CALL calculate_hourly_aggregates();

-- ============================================
-- Create API user (optional)
-- ============================================
-- CREATE USER IF NOT EXISTS 'ems_api'@'localhost' IDENTIFIED BY 'your_secure_password';
-- GRANT SELECT, INSERT, UPDATE, DELETE ON energy_monitoring.* TO 'ems_api'@'localhost';
-- FLUSH PRIVILEGES;

-- ============================================
-- Verify installation
-- ============================================
SELECT 'Database setup complete!' as status;
SHOW TABLES;
