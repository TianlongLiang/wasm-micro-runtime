/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Consistent nested struct — portable layout across wasm32 and native 32-bit.
 *
 * Rules applied:
 *   1. Use __attribute__((aligned(8))) on uint64_t and double members
 *   2. Align the outer struct itself to 8 bytes
 *   3. Inner struct uses natural alignment (no 8-byte types → no issue)
 */

#ifndef STRUCT_CONSISTENT_H
#define STRUCT_CONSISTENT_H

#ifdef __wasm__
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#endif

struct sensor_reading {
    uint32_t raw_value;
    float calibrated;
} __attribute__((aligned(4)));

struct sensor_report {
    uint8_t sensor_id;
    struct sensor_reading reading;
    uint64_t timestamp __attribute__((aligned(8)));
    uint32_t flags;
    double precision __attribute__((aligned(8)));
    uint8_t status;
} __attribute__((aligned(8)));

#endif /* STRUCT_CONSISTENT_H */
