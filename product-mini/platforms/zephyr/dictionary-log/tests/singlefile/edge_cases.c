/* tests/singlefile/edge_cases.c — extraction edge cases */
#include "wasm_log.h"

/* This LOG is inside #if 0 — should NOT be extracted after preprocessing */
#if 0
void dead_code(void)
{
    LOG_ERR("this should never appear in the dictionary");
}
#endif

/* Variable as format string — should be skipped silently */
void test_variable_fmt(void)
{
    const char *dynamic_fmt = "runtime string %d";
    wasm_log(3, dynamic_fmt, 42);
}

/* Valid call after skipped one */
void test_after_skip(void)
{
    LOG_INF("valid after variable fmt: %d", 99);
}

/* wasm_log text inside a format string — should NOT confuse the extractor */
void test_wasm_log_in_string(void)
{
    LOG_ERR("calling wasm_log(%d) failed with code %d", 3, -1);
    LOG_INF("debug: wasm_log(level, fmt, ...) is the native API");
    LOG_DBG("valid after wasm_log-in-string: %d", 42);
}

/* wasm_log text inside OTHER function's string argument — must not be extracted */
int dummy_printf(const char *fmt, ...);
void test_wasm_log_in_other_function_arg(void)
{
    dummy_printf("try calling wasm_log(3, \"hello\") for debug");
    dummy_printf("the wasm_log(%d, \"%s\") API is simple", 1, "x");
    LOG_INF("valid after other-function wasm_log strings: %d", 77);
}

/* wasm_log used as function pointer — must not be extracted */
typedef int32_t (*log_func_t)(uint32_t, const char *, ...);
void register_logger(log_func_t func);
void test_wasm_log_as_function_pointer(void)
{
    register_logger(wasm_log);
    log_func_t ptr = wasm_log;
    (void)ptr;
    LOG_INF("valid after function pointer usage: %d", 88);
}

/* struct field named wasm_log — must NOT be extracted as a log call */
struct fake_logger {
    int32_t (*wasm_log)(uint32_t level, const char *fmt, ...);
};
void test_struct_field_wasm_log(void)
{
    struct fake_logger logger;
    logger.wasm_log(3, "struct field call should not be extracted %d", 99);
    LOG_INF("valid after struct field wasm_log: %d", 55);
}

/* Empty source section — just a function with no LOG */
void no_logs_here(void)
{
    int x = 1 + 2;
    (void)x;
}

/* Source with only a comment before LOG */
/* this is just a comment */
void test_comment_before(void)
{
    LOG_DBG("after comment: %s %d", "test", 7);
}
