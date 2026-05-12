/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "sensor_app.h"

/* ---------- init_subsystem: ~28 LOG calls + 8 from loop = ~36 ---------- */
void
init_subsystem(void)
{
    int32_t major = 2, minor = 5, patch = 1;
    uint32_t heap_size = 65536, stack_size = 8192;
    int32_t sensor_count = 8;
    int32_t i;

    LOG_INF("=== WASM Sensor Monitor starting ===");
    LOG_INF("Firmware version %d.%d.%d built for qemu_x86", major, minor,
            patch);
    LOG_INF("Memory config: heap=%u bytes, stack=%u bytes", heap_size,
            stack_size);
    LOG_DBG("Initializing %d sensor channels", sensor_count);
    LOG_DBG("Setting default sample rate to %d Hz for all channels", 100);
    LOG_DBG("Configuring ADC resolution to %d bits, reference voltage %d mV",
            12, 3300);
    LOG_INF("Loading calibration data from flash offset 0x%x", 0x1F000);
    LOG_DBG("Calibration table entries: %d valid, %d expired, %d empty", 6, 1,
            1);
    LOG_DBG("Applying temperature compensation factor: offset=%d scale=%d", -3,
            1024);
    LOG_INF("Sensor bus initialized: protocol=%d speed=%u baud", 2, 115200);
    LOG_DBG("Registering interrupt handler for IRQ %d priority %d", 14, 2);
    LOG_DBG(
        "DMA channel %d allocated for sensor data transfer, buffer=%u bytes", 3,
        4096);
    LOG_INF("Power management: sleep timeout=%d ms, wake source=%d", 5000, 1);
    LOG_DBG("Watchdog timer configured: timeout=%d ms, window=%d ms", 10000,
            500);
    LOG_INF("Network interface initialized: MTU=%u, TX queue depth=%d", 1500,
            16);
    LOG_DBG("Telemetry endpoint configured: port=%d, interval=%d sec", 8883,
            30);
    LOG_DBG(
        "Local storage: ring buffer capacity=%u entries, entry size=%d bytes",
        1024, 64);
    LOG_INF("System clock source: frequency=%u Hz, prescaler=%d", 32768, 1);
    LOG_DBG("GPIO pin %d configured as output, initial state=%d", 13, 0);
    LOG_INF("Initialization complete: %d subsystems ready, %d warnings", 8, 0);
    LOG_DBG("Boot reason code: 0x%x, previous uptime=%u seconds", 0x01, 86400);
    LOG_INF("Device serial number: base address 0x%x length %d bytes", 0xFF000,
            16);
    LOG_DBG("Task scheduler started with %d priority levels, tick=%d ms", 4,
            10);
    LOG_DBG("Heap fragmentation check: %d free blocks, largest=%u bytes", 12,
            32768);
    LOG_INF("Flash wear leveling: sector size=%u, erase count=%d", 4096, 127);
    LOG_DBG("I2C bus scan: found %d devices on bus %d", 5, 0);
    LOG_DBG("SPI clock divider set to %d, effective rate=%u Hz", 8, 4000000);
    LOG_INF("RTC synchronized: drift compensation=%d ppm, epoch=%u", -2,
            1700000000);

    for (i = 0; i < sensor_count; i++) {
        LOG_DBG("Sensor channel %d: type=%d, range=%d to %d", i, i % 3,
                -40 + i, 125 + i);
    }
}

/* ---------- read_sensors: ~38 LOG calls ---------- */
void
read_sensors(void)
{
    int32_t temp_raw = 2048, humidity_raw = 3100, pressure_raw = 29500;
    uint32_t timestamp_ms = 1234567, seq_num = 42;
    int32_t adc_val = 1650, offset = -12;
    uint32_t sample_rate = 100, oversampling = 4;

    LOG_INF("Starting sensor read cycle, timestamp=%u ms", timestamp_ms);
    LOG_DBG("ADC raw value=%d, offset correction=%d applied", adc_val, offset);
    LOG_DBG("Temperature sensor raw=%d, scaled=%d milli-celsius", temp_raw,
            temp_raw * 10);
    LOG_INF("Humidity reading: raw=%d, relative=%d percent", humidity_raw,
            humidity_raw / 100);
    LOG_DBG("Barometric pressure: raw=%d, converted=%d hPa", pressure_raw,
            pressure_raw / 10);
    LOG_DBG("Sample sequence number=%u, expected=%u", seq_num, seq_num);
    LOG_INF("Oversampling factor=%u, effective sample rate=%u Hz",
            oversampling, sample_rate / oversampling);
    LOG_DBG("Light sensor: ambient=%d lux, infrared=%d counts", 450, 120);
    LOG_DBG("Accelerometer X=%d Y=%d Z=%d milli-g", 12, -5, 980);
    LOG_INF("Gyroscope reading: roll=%d pitch=%d yaw=%d mdps", 150, -200, 30);
    LOG_DBG("Magnetometer: X=%d Y=%d Z=%d micro-tesla", 23, -45, 410);
    LOG_DBG("UV index sensor: raw=%d, index=%d, threshold=%d", 180, 6, 8);
    LOG_INF("Proximity sensor: distance=%d mm, signal strength=%u", 250, 4000);
    LOG_DBG("Gas sensor resistance=%u ohms, baseline=%u ohms", 120000, 150000);
    LOG_DBG("Soil moisture: capacitance=%d, volumetric=%d percent", 340, 28);
    LOG_INF("Wind speed: pulse count=%u, velocity=%d cm/s", 87, 340);
    LOG_DBG("Wind direction: encoder=%d, degrees=%d, quadrant=%d", 156, 220, 3);
    LOG_DBG("Rain gauge: tips=%u, accumulation=%d mm", 14, 7);
    LOG_INF("Battery voltage: ADC=%d, millivolts=%u, SOC=%d percent", 3200,
            4150, 85);
    LOG_DBG("Solar panel: voltage=%d mV, current=%d mA, power=%d mW", 5200,
            120, 624);
    LOG_DBG("Noise level: peak=%d dB, average=%d dB, floor=%d dB", 72, 55, 30);
    LOG_INF("Vibration sensor: RMS=%d mg, peak=%d mg, frequency=%d Hz", 15, 45,
            120);
    LOG_DBG("CO2 concentration: raw=%d ppm, compensated=%d ppm, temp=%d C",
            412, 408, 22);
    LOG_DBG("Particulate matter: PM2.5=%d ug/m3, PM10=%d ug/m3", 12, 25);
    LOG_INF("Water level: ultrasonic=%d mm, float switch=%d", 1500, 1);
    LOG_DBG("Flow rate: pulses=%u, liters_per_min=%d, total=%u liters", 230, 5,
            14500);
    LOG_DBG("Strain gauge: raw=%d, micro-strain=%d, load=%d grams", 512, 1200,
            340);
    LOG_INF(
        "Thermocouple: cold junction=%d C, hot junction=%d C, delta=%d mV", 23,
        185, 7600);
    LOG_DBG("RTD sensor: resistance=%u milliohms, temperature=%d centi-C",
            11000, 2350);
    LOG_DBG(
        "Current transformer: raw=%d, RMS current=%d mA, power factor=%d", 480,
        2200, 95);
    LOG_INF("Sensor read cycle complete: %d channels sampled in %u us", 8,
            4500);
    LOG_DBG("Data buffer fill level: %u of %u entries used", 156, 1024);
    LOG_DBG("Moving average filter: window=%d, last output=%d, variance=%d", 16,
            2045, 3);
    LOG_INF("Kalman filter state: estimate=%d, uncertainty=%d, gain=%d", 2048,
            5, 800);
    LOG_DBG("CRC check on sensor data block: computed=0x%x, length=%d bytes",
            0xA3B7, 64);
    LOG_DBG("Interpolation table lookup: input=%d, index=%d, output=%d", 2048,
            12, 2350);
    LOG_INF("Sensor fusion result: combined measurement=%d, confidence=%d",
            2150, 92);
    LOG_DBG("Timestamp delta from last read: %u ms, expected %u ms",
            timestamp_ms - 1234467, 100);
}

int
main(void)
{
    init_subsystem();
    read_sensors();
    handle_errors();
    report_statistics();
    return 0;
}
