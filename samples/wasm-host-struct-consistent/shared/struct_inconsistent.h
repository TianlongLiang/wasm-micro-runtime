/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Inconsistent struct — layout differs between wasm32 and native.
 *
 * Two sources of mismatch demonstrated:
 *
 *   1. uint64_t alignment (x86-32 only): 8 bytes on wasm32-clang,
 *      4 bytes on x86-32 gcc. Inner struct size differs, cascading
 *      to all fields after it.
 *
 *   2. -fshort-enums (x86-64 and x86-32): native gcc with -fshort-enums
 *      packs enums to the smallest type (1 byte here), while wasm32-clang
 *      always uses 4-byte enums.
 *
 * Note: __attribute__((packed)) is honored by both native gcc and
 * wasm32-clang, so packed structs are actually consistent across
 * both targets — no mismatch there.
 */

#ifndef STRUCT_INCONSISTENT_H
#define STRUCT_INCONSISTENT_H

#ifdef __wasm__
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#endif

enum device_status {
    DEV_STATUS_OFF = 0,
    DEV_STATUS_ON = 1,
    DEV_STATUS_STANDBY = 2,
    DEV_STATUS_ERROR = 3,
};

struct device_info {
    uint8_t type;
    uint64_t serial;
};

struct device_report {
    uint8_t id;
    struct device_info info;
    float voltage;
    enum device_status status;
    uint8_t channel;
    double calibration;
};

#endif /* STRUCT_INCONSISTENT_H */
