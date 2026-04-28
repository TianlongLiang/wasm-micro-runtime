/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <string.h>
#include "wasm_export.h"
#include "struct_consistent.h"
#include "struct_inconsistent.h"

/* ── Print helpers ── */

static void
print_hex8(const char *name, uint8_t expected, uint8_t actual)
{
    char e_buf[8], a_buf[8];
    snprintf(e_buf, sizeof(e_buf), "0x%02X", expected);
    snprintf(a_buf, sizeof(a_buf), "0x%02X", actual);
    printf("  %-20s  %-20s%-20s%s\n", name, e_buf, a_buf,
           expected == actual ? "OK" : "WRONG");
}

static void
print_u32(const char *name, uint32_t expected, uint32_t actual)
{
    printf("  %-20s  %-20u%-20u%s\n",
           name, expected, actual,
           expected == actual ? "OK" : "WRONG");
}

static void
print_hex64(const char *name, uint64_t expected, uint64_t actual)
{
    char e_buf[20], a_buf[20];
    snprintf(e_buf, sizeof(e_buf), "0x%08X%08X",
             (uint32_t)(expected >> 32), (uint32_t)(expected & 0xFFFFFFFF));
    snprintf(a_buf, sizeof(a_buf), "0x%08X%08X",
             (uint32_t)(actual >> 32), (uint32_t)(actual & 0xFFFFFFFF));
    printf("  %-20s  %-20s%-20s%s\n", name, e_buf, a_buf,
           expected == actual ? "OK" : "WRONG");
}

static void
print_float(const char *name, float expected, float actual)
{
    char e_buf[20], a_buf[20];
    uint32_t raw;
    int ei = (int)expected;
    int ef = (int)((expected - ei) * 100);
    if (ef < 0) ef = -ef;
    snprintf(e_buf, sizeof(e_buf), "%d.%02d", ei, ef);
    int match = (expected > actual - 0.1f && expected < actual + 0.1f);
    if (match) {
        int ai = (int)actual;
        int af = (int)((actual - ai) * 100);
        if (af < 0) af = -af;
        snprintf(a_buf, sizeof(a_buf), "%d.%02d", ai, af);
    }
    else {
        memcpy(&raw, &actual, sizeof(raw));
        snprintf(a_buf, sizeof(a_buf), "0x%08X", raw);
    }
    printf("  %-20s  %-20s%-20s%s\n", name, e_buf, a_buf,
           match ? "OK" : "WRONG");
}

static void
print_double(const char *name, double expected, double actual)
{
    char e_buf[20], a_buf[20];
    uint32_t raw[2];
    int ei = (int)expected;
    int ef = (int)((expected - ei) * 1000000);
    if (ef < 0) ef = -ef;
    snprintf(e_buf, sizeof(e_buf), "%d.%06d", ei, ef);
    int match = (expected > actual - 0.001 && expected < actual + 0.001);
    if (match) {
        int ai = (int)actual;
        int af = (int)((actual - ai) * 1000000);
        if (af < 0) af = -af;
        snprintf(a_buf, sizeof(a_buf), "%d.%06d", ai, af);
    }
    else {
        memcpy(raw, &actual, sizeof(raw));
        snprintf(a_buf, sizeof(a_buf), "0x%08X%08X", raw[1], raw[0]);
    }
    printf("  %-20s  %-20s%-20s%s\n", name, e_buf, a_buf,
           match ? "OK" : "WRONG");
}

static int
check_u8(uint8_t expected, uint8_t actual)
{ return expected != actual; }
static int
check_u32(uint32_t expected, uint32_t actual)
{ return expected != actual; }
static int
check_u64(uint64_t expected, uint64_t actual)
{ return expected != actual; }
static int
check_float(float expected, float actual)
{ return !(expected > actual - 0.1f && expected < actual + 0.1f); }
static int
check_double(double expected, double actual)
{ return !(expected > actual - 0.001 && expected < actual + 0.001); }

static void
print_table_header(void)
{
    printf("  %-20s  %-20s%-20s%s\n",
           "Field", "Expected", "Host read", "Match");
    printf("  %-20s  %-20s%-20s%s\n",
           "────────────────────", "────────────────────",
           "────────────────────", "─────");
}

/* ── Consistent nested struct ── */

static int
process_report_native(wasm_exec_env_t exec_env,
                      struct sensor_report *rpt, int size)
{
    int errors = 0;

    printf("\n=== process_report (consistent nested struct) ===\n");
    printf("  sizeof: WASM=%d  native=%d", size,
           (int)sizeof(struct sensor_report));
    if (size != (int)sizeof(struct sensor_report)) {
        printf("  MISMATCH!");
        errors++;
    }
    printf("\n\n");
    print_table_header();

    print_hex8("sensor_id", 0x42, rpt->sensor_id);
    errors += check_u8(0x42, rpt->sensor_id);

    print_u32("reading.raw_value", 1024, rpt->reading.raw_value);
    errors += check_u32(1024, rpt->reading.raw_value);

    print_float("reading.calibrated", 23.5f, rpt->reading.calibrated);
    errors += check_float(23.5f, rpt->reading.calibrated);

    print_hex64("timestamp", 0x1234567890ABCDEFULL, rpt->timestamp);
    errors += check_u64(0x1234567890ABCDEFULL, rpt->timestamp);

    print_u32("flags", 0x00FF, rpt->flags);
    errors += check_u32(0x00FF, rpt->flags);

    print_double("precision", 0.001, rpt->precision);
    errors += check_double(0.001, rpt->precision);

    print_hex8("status", 0x01, rpt->status);
    errors += check_u8(0x01, rpt->status);

    printf("\n  Result: %d errors (%s)\n", errors,
           errors == 0 ? "PASS" : "FAIL");
    return errors;
}

/* ── Inconsistent nested struct ── */

static int
configure_device_native(wasm_exec_env_t exec_env,
                        struct device_report *rpt, int size)
{
    int errors = 0;

    printf("\n=== configure_device (inconsistent nested struct) ===\n");
    printf("  sizeof: WASM=%d  native=%d", size,
           (int)sizeof(struct device_report));
    if (size != (int)sizeof(struct device_report)) {
        printf("  MISMATCH!");
        errors++;
    }
    printf("\n\n");
    print_table_header();

    print_hex8("id", 0x07, rpt->id);
    errors += check_u8(0x07, rpt->id);

    print_hex8("info.type", 0x03, rpt->info.type);
    errors += check_u8(0x03, rpt->info.type);

    print_hex64("info.serial", 0xDEADBEEFCAFEBABEULL, rpt->info.serial);
    errors += check_u64(0xDEADBEEFCAFEBABEULL, rpt->info.serial);

    print_float("voltage", 3.3f, rpt->voltage);
    errors += check_float(3.3f, rpt->voltage);

    print_hex8("channel", 0x05, rpt->channel);
    errors += check_u8(0x05, rpt->channel);

    print_double("calibration", 1.23456789, rpt->calibration);
    errors += check_double(1.23456789, rpt->calibration);

    print_u32("status", DEV_STATUS_ERROR, (uint32_t)rpt->status);
    errors += check_u32(DEV_STATUS_ERROR, (uint32_t)rpt->status);

    printf("\n  Result: %d errors (%s)\n", errors,
           errors == 0 ? "PASS" : "FAIL — layout mismatch causes wrong values");
    return errors;
}

static void
print_int_native(wasm_exec_env_t exec_env, int value)
{
    printf("WASM returned: %d\n", value);
}

/* void* API — the checker can't verify the layout because the cast target
 * is determined at runtime. This should trigger a warning. */
static int
process_raw_native(wasm_exec_env_t exec_env, void *buf, int size)
{
    (void)exec_env;
    (void)buf;
    printf("process_raw: received %d bytes (void* — unchecked)\n", size);
    return 0;
}

static NativeSymbol native_symbols[] = {
    { "process_report", process_report_native, "(*~)i", NULL },
    { "configure_device", configure_device_native, "(*~)i", NULL },
    { "process_raw", process_raw_native, "(*~)i", NULL },
    { "print_int", print_int_native, "(i)", NULL },
};

NativeSymbol *
get_native_symbols(int *count)
{
    *count = sizeof(native_symbols) / sizeof(NativeSymbol);
    return native_symbols;
}
