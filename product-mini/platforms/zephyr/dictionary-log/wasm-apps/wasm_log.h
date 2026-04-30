/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_LOG_H
#define WASM_LOG_H

#include <stdint.h>

#define WASM_LOG_LEVEL_NONE    0
#define WASM_LOG_LEVEL_ERR     1
#define WASM_LOG_LEVEL_WRN     2
#define WASM_LOG_LEVEL_INF     3
#define WASM_LOG_LEVEL_DBG     4
#define WASM_LOG_LEVEL_VERBOSE 5

#ifndef CUR_LOG_LEVEL
#define CUR_LOG_LEVEL WASM_LOG_LEVEL_DBG
#endif

#ifdef WASM_LOG_DICT
__attribute__((__import_module__("env")))
__attribute__((__import_name__("wasm_log_dict")))
int32_t wasm_log_dict(uint32_t log_level, uint32_t string_id,
                       uint32_t arg_type_descriptor, ...);
#else
__attribute__((__import_module__("env")))
__attribute__((__import_name__("wasm_log")))
int32_t wasm_log(uint32_t log_level, const char *format, ...);
#endif

#define LOG_ENABLED(lvl) ((lvl) <= (CUR_LOG_LEVEL))

#ifndef WASM_LOG_DICT
#define LOG_AT(lvl, fmt, ...)                        \
    do {                                             \
        if (LOG_ENABLED(lvl))                        \
            wasm_log(lvl, fmt "\n", ##__VA_ARGS__);  \
    } while (0)

#define LOG_ERR(fmt, ...)     LOG_AT(WASM_LOG_LEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...)     LOG_AT(WASM_LOG_LEVEL_WRN, fmt, ##__VA_ARGS__)
#define LOG_INF(fmt, ...)     LOG_AT(WASM_LOG_LEVEL_INF, fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)     LOG_AT(WASM_LOG_LEVEL_DBG, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LOG_AT(WASM_LOG_LEVEL_VERBOSE, fmt, ##__VA_ARGS__)
#endif

#endif /* WASM_LOG_H */
