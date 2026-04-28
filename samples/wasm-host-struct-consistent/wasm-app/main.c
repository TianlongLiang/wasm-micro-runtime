/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "struct_consistent.h"
#include "struct_inconsistent.h"

int
process_report(struct sensor_report *rpt, int size);

int
configure_device(struct device_report *rpt, int size);

void
print_int(int value);

__attribute__((export_name("run"))) int
run(void)
{
    struct sensor_report rpt;
    struct device_report dev;
    int result;

    /* Consistent nested struct — should always work */
    rpt.sensor_id = 0x42;
    rpt.reading.raw_value = 1024;
    rpt.reading.calibrated = 23.5f;
    rpt.timestamp = 0x1234567890ABCDEFULL;
    rpt.flags = 0x00FF;
    rpt.precision = 0.001;
    rpt.status = 0x01;

    result = process_report(&rpt, sizeof(rpt));
    print_int(result);

    /* Inconsistent nested struct — will show garbage on x86-32 */
    dev.id = 0x07;
    dev.info.type = 0x03;
    dev.info.serial = 0xDEADBEEFCAFEBABEULL;
    dev.voltage = 3.3f;
    dev.status = DEV_STATUS_ERROR;
    dev.channel = 0x05;
    dev.calibration = 1.23456789;

    result += configure_device(&dev, sizeof(dev));
    print_int(result);

    return result;
}
