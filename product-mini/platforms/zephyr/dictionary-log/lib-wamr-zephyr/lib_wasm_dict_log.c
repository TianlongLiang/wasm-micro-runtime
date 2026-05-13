/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_output_dict.h>

#include "bh_platform.h"
#include "bh_assert.h"
#include "wasm_export.h"
#include "lib_export.h"

LOG_MODULE_REGISTER(wasm_dict, LOG_LEVEL_DBG);

/*
 * Disable function name prefix for WASM App log at every severity level.
 *
 * Zephyr uses Z_LOG_FUNC_PREFIX_<level> (1=ERR … 4=DBG) to decide whether
 * to prepend __func__.  Redefining them to 0 removes the prefix for this
 * translation unit only, so other modules keep their normal behaviour.
 */
#undef  Z_LOG_FUNC_PREFIX_1
#define Z_LOG_FUNC_PREFIX_1 0
#undef  Z_LOG_FUNC_PREFIX_2
#define Z_LOG_FUNC_PREFIX_2 0
#undef  Z_LOG_FUNC_PREFIX_3
#define Z_LOG_FUNC_PREFIX_3 0
#undef  Z_LOG_FUNC_PREFIX_4
#define Z_LOG_FUNC_PREFIX_4 0


/* Log levels matching wasm-apps/wasm_log.h (WASM-side definitions).
 * These are the numeric values the WASM app passes to wasm_log(). */
#define WASM_LOG_LEVEL_NONE    0
#define WASM_LOG_LEVEL_ERR     1
#define WASM_LOG_LEVEL_WRN     2
#define WASM_LOG_LEVEL_INF     3
#define WASM_LOG_LEVEL_DBG     4
#define WASM_LOG_LEVEL_VERBOSE 5

#define validate_native_addr(addr, size) \
    wasm_runtime_validate_native_addr(module_inst, addr, size)

#ifndef WASM_LOG_BUFFERED_PRINT_SIZE
#define WASM_LOG_BUFFERED_PRINT_SIZE 128
#endif

struct str_context {
    char *str;
    uint32 max;
    uint32 count;
    char print_buf[WASM_LOG_BUFFERED_PRINT_SIZE];
    uint32 print_buf_size;
    uint32 log_level;
    const char *app_name;
};

typedef int (*out_func_t)(int c, void *ctx);

typedef char *_va_list;
#define _INTSIZEOF(n) (((uint32)sizeof(n) + 3) & (uint32)~3)
#define _va_arg(ap, t) (*(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)))

#define CHECK_VA_ARG(ap, t)                                  \
    do {                                                     \
        if ((uint8 *)ap + _INTSIZEOF(t) > native_end_addr) {\
            if (fmt_buf != temp_fmt) {                       \
                wasm_runtime_free(fmt_buf);                  \
            }                                                \
            goto fail;                                       \
        }                                                    \
    } while (0)

#define PREPARE_TEMP_FORMAT()                                              \
    char temp_fmt[32], *s, *fmt_buf = temp_fmt;                            \
    uint32 fmt_buf_len = (uint32)sizeof(temp_fmt);                         \
    int32 n;                                                               \
                                                                           \
    /* additional 2 bytes: one is the format char,                         \
       the other is `\0` */                                                \
    if ((uint32)(fmt - fmt_start_addr + 2) >= fmt_buf_len) {               \
        bh_assert((uint32)(fmt - fmt_start_addr) <= UINT32_MAX - 2);       \
        fmt_buf_len = (uint32)(fmt - fmt_start_addr + 2);                  \
        fmt_buf = wasm_runtime_malloc(fmt_buf_len);                        \
        if (!fmt_buf) {                                                    \
            print_err(out, ctx);                                           \
            break;                                                         \
        }                                                                  \
    }                                                                      \
                                                                           \
    memset(fmt_buf, 0, fmt_buf_len);                                       \
    bh_memcpy_s(fmt_buf, fmt_buf_len, fmt_start_addr,                      \
                (uint32)(fmt - fmt_start_addr + 1));

#define OUTPUT_TEMP_FORMAT()                  \
    do {                                      \
        if (n > 0) {                          \
            s = buf;                          \
            while (*s)                        \
                out((int)(*s++), ctx);        \
        }                                     \
                                              \
        if (fmt_buf != temp_fmt) {            \
            wasm_runtime_free(fmt_buf);       \
        }                                     \
    } while (0)

/* Emit "ERR" as error indicator when format parsing fails */
static void
print_err(out_func_t out, void *ctx)
{
    out('E', ctx);
    out('R', ctx);
    out('R', ctx);
}

/* Printf implementation for WASM: parse format string char-by-char,
 * pull typed args from WASM linear memory via _va_list, format via snprintf.
 * Used by the baseline wasm_log() path (runtime string formatting). */
static bool
_vprintf_wa(out_func_t out, void *ctx, const char *fmt, _va_list ap,
            wasm_module_inst_t module_inst)
{
    int might_format = 0; /* 1 if encountered a '%' */
    int long_ctr = 0;
    uint8 *native_end_addr;
    const char *fmt_start_addr = NULL;

    if (!wasm_runtime_get_native_addr_range(module_inst, (uint8 *)ap, NULL,
                                            &native_end_addr)) {
        goto fail;
    }

    /* fmt has already been adjusted if needed */

    while (*fmt) {
        if (!might_format) {
            if (*fmt != '%') {
                out((int)*fmt, ctx);
            }
            else {
                might_format = 1;
                long_ctr = 0;
                fmt_start_addr = fmt;
            }
        }
        else {
            switch (*fmt) {
                case '.':
                case '+':
                case '-':
                case ' ':
                case '#':
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    goto still_might_format;

                case 't': /* ptrdiff_t */
                case 'z': /* size_t (32bit on wasm) */
                    long_ctr = 1;
                    goto still_might_format;

                case 'j':
                    /* intmax_t/uintmax_t */
                    long_ctr = 2;
                    goto still_might_format;

                case 'l':
                    long_ctr++;
                    /* Fall through */
                case 'h':
                    /* FIXME: do nothing for these modifiers */
                    goto still_might_format;

                case 'o':
                case 'd':
                case 'i':
                case 'u':
                case 'p':
                case 'x':
                case 'X':
                case 'c':
                {
                    char buf[64];
                    PREPARE_TEMP_FORMAT();

                    if (long_ctr < 2) {
                        int32 d;

                        CHECK_VA_ARG(ap, uint32);
                        d = _va_arg(ap, int32);

                        if (long_ctr == 1) {
                            uint32 fmt_end_idx =
                                (uint32)(fmt - fmt_start_addr);

                            if (fmt_buf[fmt_end_idx - 1] == 'l'
                                || fmt_buf[fmt_end_idx - 1] == 'z'
                                || fmt_buf[fmt_end_idx - 1] == 't') {
                                /* The %ld, %zd and %td should be treated
                                 * as 32bit integer in wasm */
                                fmt_buf[fmt_end_idx - 1] =
                                    fmt_buf[fmt_end_idx];
                                fmt_buf[fmt_end_idx] = '\0';
                            }
                        }

                        n = snprintf(buf, sizeof(buf), fmt_buf, d);
                    }
                    else {
                        int64 lld;

                        /* Make 8-byte aligned */
                        ap = (_va_list)(((uintptr_t)ap + 7)
                                        & ~(uintptr_t)7);
                        CHECK_VA_ARG(ap, uint64);
                        lld = _va_arg(ap, int64);
                        n = snprintf(buf, sizeof(buf), fmt_buf, lld);
                    }

                    OUTPUT_TEMP_FORMAT();
                    break;
                }

                case 's':
                {
                    char buf_tmp[128], *buf = buf_tmp;
                    char *start;
                    uint32 s_offset, str_len, buf_len;

                    PREPARE_TEMP_FORMAT();

                    CHECK_VA_ARG(ap, int32);
                    s_offset = _va_arg(ap, uint32);

                    if (!validate_app_str_addr(s_offset)) {
                        if (fmt_buf != temp_fmt) {
                            wasm_runtime_free(fmt_buf);
                        }
                        return false;
                    }

                    s = start = addr_app_to_native((uint64)s_offset);

                    str_len = (uint32)strlen(start);
                    if (str_len >= UINT32_MAX - 64) {
                        print_err(out, ctx);
                        if (fmt_buf != temp_fmt) {
                            wasm_runtime_free(fmt_buf);
                        }
                        break;
                    }

                    /* reserve 64 more bytes as there may be width
                     * description in the fmt */
                    buf_len = str_len + 64;

                    if (buf_len > (uint32)sizeof(buf_tmp)) {
                        buf = wasm_runtime_malloc(buf_len);
                        if (!buf) {
                            print_err(out, ctx);
                            if (fmt_buf != temp_fmt) {
                                wasm_runtime_free(fmt_buf);
                            }
                            break;
                        }
                    }

                    n = snprintf(
                        buf, buf_len, fmt_buf,
                        (s_offset == 0 && str_len == 0) ? NULL : start);

                    OUTPUT_TEMP_FORMAT();

                    if (buf != buf_tmp) {
                        wasm_runtime_free(buf);
                    }

                    break;
                }

                case '%':
                {
                    out((int)'%', ctx);
                    break;
                }

                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'f':
                case 'F':
                {
                    float64 f64;
                    char buf[64];
                    PREPARE_TEMP_FORMAT();

                    /* Make 8-byte aligned */
                    ap = (_va_list)(((uintptr_t)ap + 7) & ~(uintptr_t)7);
                    CHECK_VA_ARG(ap, float64);
                    f64 = _va_arg(ap, float64);
                    n = snprintf(buf, sizeof(buf), fmt_buf, f64);

                    OUTPUT_TEMP_FORMAT();
                    break;
                }

                case 'n':
                    /* print nothing */
                    break;

                default:
                    out((int)'%', ctx);
                    out((int)*fmt, ctx);
                    break;
            }

            might_format = 0;
        }

    still_might_format:
        ++fmt;
    }
    return true;

fail:
    wasm_runtime_set_exception(module_inst, "out of bounds memory access");
    return false;
}

/* Dispatch a WASM-side log level to the corresponding Zephyr log macro.
 * VERBOSE maps to LOG_DBG (Zephyr has no VERBOSE equivalent).
 */
#define LOG_WASM(level_value, ...)                    \
    do {                                              \
        switch (level_value) {                        \
            case WASM_LOG_LEVEL_ERR:                  \
                LOG_ERR(__VA_ARGS__);                 \
                break;                                \
            case WASM_LOG_LEVEL_WRN:                  \
                LOG_WRN(__VA_ARGS__);                 \
                break;                                \
            case WASM_LOG_LEVEL_INF:                  \
                LOG_INF(__VA_ARGS__);                 \
                break;                                \
            case WASM_LOG_LEVEL_DBG:                  \
            case WASM_LOG_LEVEL_VERBOSE:              \
                LOG_DBG(__VA_ARGS__);                 \
                break;                                \
            case WASM_LOG_LEVEL_NONE:                 \
            default: /* unknown level: no-op */       \
                break;                                \
        }                                             \
    } while (0)

/* Output callback for _vprintf_wa: buffers chars until newline or buffer full,
 * then flushes via Zephyr LOG_* macros at the appropriate level. */
static int
printf_out(int c, struct str_context *ctx)
{
    if (c == '\n') {
        ctx->print_buf[ctx->print_buf_size] = '\0';
        LOG_WASM(ctx->log_level, "%s%s", ctx->app_name, ctx->print_buf);
        ctx->print_buf_size = 0;
    }
    else if (ctx->print_buf_size >= sizeof(ctx->print_buf) - 2) {
        ctx->print_buf[ctx->print_buf_size++] = (char)c;
        ctx->print_buf[ctx->print_buf_size] = '\0';
        LOG_WASM(ctx->log_level, "%s%s", ctx->app_name, ctx->print_buf);
        ctx->print_buf_size = 0;
    }
    else {
        ctx->print_buf[ctx->print_buf_size++] = (char)c;
    }

    ctx->count++;
    return c;
}

/* Baseline native API: wasm_log(level, "fmt", args...).
 * Formats the message at runtime using _vprintf_wa and emits via Zephyr LOG_*. */
static int
wasm_log_wrapper(wasm_exec_env_t exec_env, uint32 log_level,
                 const char *format, _va_list va_args)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    struct str_context ctx = { 0 };
    int ret;

    ctx.log_level = log_level;
    ctx.app_name = "My_APP: ";

    /* format has been checked by runtime */
    if (!validate_native_addr(va_args, (uint64)sizeof(int32))) {
        return 0;
    }

    if (!_vprintf_wa((out_func_t)printf_out, &ctx, format, va_args,
                     module_inst)) {
        return 0;
    }

    if (ctx.print_buf_size > 0) {
        LOG_WASM(ctx.log_level, "%s%s", ctx.app_name, ctx.print_buf);
    }

    return (int)ctx.count;
}

/* --- Dictionary mode: binary packet emission --- */

#define MSG_WASM_LOG 0x80

_Static_assert(MSG_WASM_LOG != MSG_NORMAL,
               "MSG_WASM_LOG collides with Zephyr MSG_NORMAL");
_Static_assert(MSG_WASM_LOG != MSG_DROPPED_MSG,
               "MSG_WASM_LOG collides with Zephyr MSG_DROPPED_MSG");
_Static_assert(MSG_WASM_LOG >= 0x80,
               "MSG_WASM_LOG must be in vendor extension range (>= 0x80)");

#define WASM_LOG_ARG_INT32   0x01
#define WASM_LOG_ARG_INT64   0x02
#define WASM_LOG_ARG_FLOAT64 0x03
#define WASM_LOG_ARG_STRING  0x04

#define WASM_LOG_DICT_MAX_PACKET 256

/* Dictionary native API: wasm_log_dict(level, string_id, type_desc, args...).
 * Packs a 14-byte header + typed args into a binary packet (msg_type=0x80)
 * and emits it via Zephyr's LOG_HEXDUMP (works with any backend: UART, RTT, etc.)
 * No format string processing at runtime.
 * app_id is retrieved from exec_env user_data (host-assigned, not from WASM). */
static int
wasm_log_dict_wrapper(wasm_exec_env_t exec_env, uint32 log_level,
                      uint32 string_id, uint32 arg_type_desc,
                      _va_list va_args)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint8 pkt[WASM_LOG_DICT_MAX_PACKET];
    uint32 pos = 0;
    uint32 desc = arg_type_desc;
    uint32 arg_count = 0;
    uint32 tmp;

    /* Count args: each non-zero nibble in the descriptor is one arg */
    tmp = desc;
    while (tmp) {
        if (tmp & 0x0F)
            arg_count++;
        tmp >>= 4;
    }

    /* Build 14-byte header */
    uint8 app_id = (uint8)(uintptr_t)wasm_runtime_get_user_data(exec_env);
    pkt[pos++] = MSG_WASM_LOG;                        /* msg_type */
    pkt[pos++] = app_id;                              /* app_id */
    pkt[pos++] = (uint8)log_level;                    /* log_level */
    pkt[pos++] = (uint8)(string_id & 0xFF);           /* string_id LE */
    pkt[pos++] = (uint8)((string_id >> 8) & 0xFF);
    {
        uint64 ts = (uint64)k_uptime_get_32();
        for (int b = 0; b < 8; b++)
            pkt[pos++] = (uint8)((ts >> (b * 8)) & 0xFF);
    }
    pkt[pos++] = (uint8)arg_count;                    /* arg_count */

    /* Pack each argument */
    desc = arg_type_desc;
    for (uint32 i = 0; i < arg_count; i++) {
        uint8 atype = (uint8)(desc & 0x0F);
        desc >>= 4;

        if (pos + 1 >= WASM_LOG_DICT_MAX_PACKET)
            break;
        pkt[pos++] = atype;

        switch (atype) {
            case WASM_LOG_ARG_INT32:
            {
                int32 v = _va_arg(va_args, int32);
                if (pos + 4 > WASM_LOG_DICT_MAX_PACKET)
                    goto emit;
                pkt[pos++] = (uint8)(v & 0xFF);
                pkt[pos++] = (uint8)((v >> 8) & 0xFF);
                pkt[pos++] = (uint8)((v >> 16) & 0xFF);
                pkt[pos++] = (uint8)((v >> 24) & 0xFF);
                break;
            }
            case WASM_LOG_ARG_INT64:
            {
                int64 v;
                /* Align to 8 bytes */
                va_args = (_va_list)(((uintptr_t)va_args + 7)
                                     & ~(uintptr_t)7);
                v = _va_arg(va_args, int64);
                if (pos + 8 > WASM_LOG_DICT_MAX_PACKET)
                    goto emit;
                for (int b = 0; b < 8; b++)
                    pkt[pos++] = (uint8)((v >> (b * 8)) & 0xFF);
                break;
            }
            case WASM_LOG_ARG_FLOAT64:
            {
                union { float64 f; uint64 u; } fv;
                /* Align to 8 bytes */
                va_args = (_va_list)(((uintptr_t)va_args + 7)
                                     & ~(uintptr_t)7);
                fv.f = _va_arg(va_args, float64);
                if (pos + 8 > WASM_LOG_DICT_MAX_PACKET)
                    goto emit;
                for (int b = 0; b < 8; b++)
                    pkt[pos++] = (uint8)((fv.u >> (b * 8)) & 0xFF);
                break;
            }
            case WASM_LOG_ARG_STRING:
            {
                uint32 s_offset = _va_arg(va_args, uint32);
                const char *s;
                uint16 slen;

                if (!validate_app_str_addr(s_offset)) {
                    /* invalid string, emit what we have so far */
                    goto emit;
                }
                s = addr_app_to_native((uint64)s_offset);
                slen = (uint16)strlen(s);
                if (pos + 2 + slen > WASM_LOG_DICT_MAX_PACKET)
                    goto emit;
                pkt[pos++] = (uint8)(slen & 0xFF);
                pkt[pos++] = (uint8)((slen >> 8) & 0xFF);
                memcpy(&pkt[pos], s, slen);
                pos += slen;
                break;
            }
            default:
                /* Unknown arg type, stop packing */
                goto emit;
        }
    }

emit:
    /* Emit packet through Zephyr's log subsystem — automatically uses
     * whatever backend is configured (UART, RTT, network, etc.) */
    switch (log_level) {
        case WASM_LOG_LEVEL_ERR:
            LOG_HEXDUMP_ERR(pkt, pos, "");
            break;
        case WASM_LOG_LEVEL_WRN:
            LOG_HEXDUMP_WRN(pkt, pos, "");
            break;
        case WASM_LOG_LEVEL_INF:
            LOG_HEXDUMP_INF(pkt, pos, "");
            break;
        case WASM_LOG_LEVEL_DBG:
        case WASM_LOG_LEVEL_VERBOSE:
            LOG_HEXDUMP_DBG(pkt, pos, "");
            break;
        default:
            break;
    }
    return 0;
}

#define REG_NATIVE_FUNC(func_name, signature) \
    { #func_name, func_name##_wrapper, signature, NULL }

static NativeSymbol native_symbols_lib_wasm_log[] = {
    REG_NATIVE_FUNC(wasm_log, "(i$*)i"),
    REG_NATIVE_FUNC(wasm_log_dict, "(iii*)i"),
};

/* Return the native symbol table for WAMR to register both wasm_log (baseline)
 * and wasm_log_dict (dictionary) imports available to WASM modules. */
uint32
get_lib_wasm_log_export_apis(NativeSymbol **p_native_symbols)
{
    *p_native_symbols = native_symbols_lib_wasm_log;
    return sizeof(native_symbols_lib_wasm_log) / sizeof(NativeSymbol);
}
