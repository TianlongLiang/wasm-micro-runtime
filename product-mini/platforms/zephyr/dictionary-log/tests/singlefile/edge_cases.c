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
