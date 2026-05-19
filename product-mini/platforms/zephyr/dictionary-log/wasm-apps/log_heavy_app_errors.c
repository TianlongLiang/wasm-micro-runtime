/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "sensor_app.h"

/* ---------- handle_errors: ~35 LOG calls ---------- */
void
handle_errors(void)
{
    uint32_t error_code = 0xDEAD, retry_count = 3;
    int32_t timeout_ms = 500, bus_id = 2;
    uint32_t crc_expected = 0xA3B7, crc_actual = 0xA3B8;
    int32_t sensor_id = 5, threshold = 100;
    void *fault_addr = (void *)0x0000DEAD;
    void *stack_top = (void *)0x20008000;
    void *handler_fn = (void *)0x00000400;

    const char *task_name = "sensor_read";
    const char *bus_name = "i2c0";

    LOG_ERR("Sensor %d read timeout after %d ms on bus %d", sensor_id,
            timeout_ms, bus_id);
    LOG_ERR("Task '%s' faulted at address %p, stack pointer=%p", task_name,
            fault_addr, stack_top);
    LOG_WRN("Bus '%s' error: device at 0x%x not responding", bus_name, 0x48);
    LOG_WRN("ISR handler at %p replaced, new handler at %p", handler_fn,
            (char *)handler_fn + 0x100);
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
void
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
