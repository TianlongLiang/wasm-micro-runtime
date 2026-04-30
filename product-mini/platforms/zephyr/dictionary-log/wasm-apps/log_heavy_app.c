/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_log.h"

/* ---------- init_subsystem: ~28 LOG calls + 8 from loop = ~36 ---------- */
static void
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
static void
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

/* ---------- handle_errors: ~35 LOG calls ---------- */
static void
handle_errors(void)
{
    uint32_t error_code = 0xDEAD, retry_count = 3;
    int32_t timeout_ms = 500, bus_id = 2;
    uint32_t crc_expected = 0xA3B7, crc_actual = 0xA3B8;
    int32_t sensor_id = 5, threshold = 100;

    LOG_ERR("Sensor %d read timeout after %d ms on bus %d", sensor_id,
            timeout_ms, bus_id);
    LOG_WRN("CRC mismatch: expected=0x%x actual=0x%x on packet %u",
            crc_expected, crc_actual, 1023);
    LOG_ERR("Bus %d arbitration lost, error code=0x%x", bus_id, error_code);
    LOG_WRN("Retry attempt %u of %u for sensor %d read operation", 1,
            retry_count, sensor_id);
    LOG_ERR("ADC overflow detected: raw value=%d exceeds max=%d", 4096, 4095);
    LOG_WRN("Temperature reading %d out of range [%d, %d]", 150, -40, 125);
    LOG_ERR("I2C NACK received from device address 0x%x on bus %d", 0x48,
            bus_id);
    LOG_WRN("Sensor %d data stale: age=%u ms, max allowed=%d ms", sensor_id,
            2000, 1000);
    LOG_ERR("DMA transfer error: channel=%d, status=0x%x, bytes=%u", 3, 0x04,
            0);
    LOG_WRN("Power supply voltage low: measured=%d mV, minimum=%d mV", 2800,
            3000);
    LOG_ERR("Flash write failed at address 0x%x, error=%d", 0x1F400, -5);
    LOG_WRN("Watchdog timeout approaching: remaining=%d ms, threshold=%d ms",
            1500, 2000);
    LOG_ERR("Stack overflow detected: task %d, usage=%u of %u bytes", 2, 8100,
            8192);
    LOG_WRN("Heap allocation failed: requested=%u bytes, available=%u bytes",
            4096, 2048);
    LOG_ERR(
        "Interrupt storm on IRQ %d: %u triggers in %d ms, disabling handler",
        14, 500, 10);
    LOG_WRN("Sensor %d calibration data corrupted: checksum=0x%x", sensor_id,
            0xFF01);
    LOG_ERR("Communication timeout: no response in %d ms from endpoint %d",
            timeout_ms, 3);
    LOG_WRN("Ring buffer overrun: write_idx=%u, read_idx=%u, capacity=%u", 1024,
            512, 1024);
    LOG_ERR("Invalid sensor configuration: channel=%d, mode=%d not supported",
            7, 4);
    LOG_WRN("Clock drift detected: measured=%d ppm, limit=%d ppm", 25, 20);
    LOG_ERR("SPI transfer abort: FIFO underflow on channel %d, pending=%u", 1,
            16);
    LOG_WRN("Network packet dropped: length=%u exceeds MTU=%u", 1600, 1500);
    LOG_ERR("EEPROM wear limit approaching: sector %d, erase count=%u",
            threshold, 99000);
    LOG_WRN("Analog reference voltage unstable: measured=%d mV, expected=%d mV",
            3280, 3300);
    LOG_ERR("Task deadline missed: task %d, overrun=%d us, period=%d us", 3,
            150, 1000);
    LOG_WRN(
        "Brownout event detected: voltage dip to %d mV lasted %d ms, count=%d",
        2600, 50, 3);
    LOG_ERR("Sensor bus collision: %d devices responded to broadcast on bus %d",
            3, bus_id);
    LOG_WRN("Firmware image validation failed: header CRC=0x%x, expected=0x%x",
            0xBEEF, 0xCAFE);
    LOG_ERR("Thermal shutdown imminent: junction temp=%d C, limit=%d C", 118,
            125);
    LOG_WRN("Retry limit reached for sensor %d after %u attempts, skipping",
            sensor_id, retry_count);
    LOG_ERR("Mutex deadlock detected: task %d holding lock %d, waiting on %d",
            1, 2, 3);
    LOG_WRN("GPIO input glitch on pin %d: debounce=%d ms, count=%u", 7, 50,
            12);
    LOG_ERR(
        "Memory corruption: guard pattern=0x%x at offset %u, expected=0x%x",
        0xDEADBEEF, 8192, 0xCAFEBABE);
    LOG_WRN("Backup battery low: voltage=%d mV, threshold=%d mV", 2100, 2200);
    LOG_ERR("Recovery initiated: resetting subsystem %d, attempt=%u", bus_id,
            retry_count);
}

/* ---------- report_statistics: ~33 LOG calls ---------- */
static void
report_statistics(void)
{
    uint32_t uptime_sec = 86400, total_samples = 864000;
    int32_t temp_min = -15, temp_max = 42, temp_avg = 22;
    uint32_t free_heap = 32768, used_heap = 32768;
    int32_t error_count = 47, warning_count = 213;

    LOG_INF("=== Statistics Report ===");
    LOG_INF("System uptime: %u seconds (%d hours)", uptime_sec,
            uptime_sec / 3600);
    LOG_INF("Total samples collected: %u across %d channels", total_samples, 8);
    LOG_INF("Temperature stats: min=%d max=%d avg=%d centi-Celsius", temp_min,
            temp_max, temp_avg);
    LOG_DBG("Humidity stats: min=%d max=%d avg=%d relative percent", 20, 95,
            55);
    LOG_DBG("Pressure stats: min=%d max=%d avg=%d deci-hPa", 9800, 10300,
            10130);
    LOG_INF("Memory usage: free=%u used=%u total=%u bytes", free_heap,
            used_heap, free_heap + used_heap);
    LOG_DBG("Heap high-water mark: %u bytes, current fragmentation=%d percent",
            61440, 8);
    LOG_INF("Error count: %d errors, %d warnings in last %u seconds",
            error_count, warning_count, uptime_sec);
    LOG_DBG("CRC error rate: %d per %u transmissions, ratio=%d ppm", 3, 100000,
            30);
    LOG_DBG("Sensor timeout histogram: [0-100ms]=%d [100-500ms]=%d [>500ms]=%d",
            800, 45, 2);
    LOG_INF("Average sample latency: %d us, 99th percentile=%d us", 450, 1200);
    LOG_DBG("DMA transfer count: %u successful, %d failed, throughput=%u B/s",
            total_samples, 12, 51200);
    LOG_DBG("Interrupt count: %u total, %d spurious, max latency=%d us",
            total_samples * 2, 5, 45);
    LOG_INF("Power consumption: average=%d mA, peak=%d mA, sleep=%d uA", 15,
            85, 50);
    LOG_DBG("Sleep duration stats: min=%d max=%d avg=%d ms", 4500, 5200, 4980);
    LOG_DBG("Network stats: TX=%u packets, RX=%u packets, dropped=%d", 28800,
            28750, 50);
    LOG_INF("Telemetry uploads: %u successful, %d failed, %d pending", 2880, 3,
            1);
    LOG_DBG("Flash write cycles: %u total, wear level=%d percent", 12800, 13);
    LOG_DBG("Ring buffer stats: writes=%u reads=%u overflows=%d", total_samples,
            total_samples - 100, 100);
    LOG_INF("Calibration drift: max=%d LSB across %d channels in %u hours", 2,
            8, uptime_sec / 3600);
    LOG_DBG("Task CPU usage: sensor=%d%% comm=%d%% idle=%d%%", 35, 20, 45);
    LOG_DBG("Stack usage per task: sensor=%u comm=%u main=%u bytes", 6200, 4800,
            3200);
    LOG_INF("Watchdog reset count: %d, last reset reason=0x%x", 0, 0x00);
    LOG_DBG("ADC noise floor: channel 0=%d, channel 1=%d, channel 2=%d LSB", 2,
            3, 1);
    LOG_DBG("Sensor data compression: input=%u output=%u ratio=%d percent",
            864000, 432000, 50);
    LOG_INF("Report generation time: %d ms, next report in %u seconds", 12,
            3600);
    LOG_DBG("Histogram bin 0 (low range): count=%u, percentage=%d", 120000, 14);
    LOG_DBG("Histogram bin 1 (mid-low range): count=%u, percentage=%d", 280000,
            32);
    LOG_DBG("Histogram bin 2 (mid-high range): count=%u, percentage=%d", 350000,
            41);
    LOG_DBG("Histogram bin 3 (high range): count=%u, percentage=%d", 114000,
            13);
    LOG_INF("System health score: %d out of %d, status=%d", 95, 100, 1);
    LOG_INF("=== End of Statistics Report ===");
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
