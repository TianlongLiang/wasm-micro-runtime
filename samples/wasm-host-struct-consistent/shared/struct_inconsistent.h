/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Inconsistent nested struct — layout differs between wasm32 and native.
 *
 * The inner struct has uint64_t which aligns to 8 bytes on wasm32-clang
 * but only 4 bytes on x86-32 gcc, making the inner struct itself different
 * in size. This cascades: the outer struct's fields after the inner member
 * shift to different offsets.
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

struct device_info {
    uint8_t type;
    uint64_t serial;
};

struct device_report {
    uint8_t id;
    struct device_info info;
    float voltage;
    uint8_t channel;
    double calibration;
    uint8_t mode;
};

#endif /* STRUCT_INCONSISTENT_H */
