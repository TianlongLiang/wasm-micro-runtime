/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef SENSOR_APP_H
#define SENSOR_APP_H

#include "wasm_log.h"

void init_subsystem(void);
void read_sensors(void);
void handle_errors(void);
void report_statistics(void);

#endif /* SENSOR_APP_H */
