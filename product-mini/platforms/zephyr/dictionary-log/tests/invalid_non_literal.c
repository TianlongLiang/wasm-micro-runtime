/* Test: non-literal format strings — should be skipped with warnings */
#include "wasm_log.h"

#define MY_FMT "macro defined format %d"
static const char *runtime_fmt = "runtime string %d";

void test_non_literal(void)
{
    int32_t val = 10;

    /* Variable as format string — SKIPPED */
    LOG_INF(runtime_fmt, val);

    /* Macro identifier as format string — SKIPPED */
    LOG_ERR(MY_FMT, val);

    /* Empty call — SKIPPED */
    LOG_DBG();

    /* Valid call after invalid ones — should still work */
    LOG_INF("recovery after errors: %d", val);
}
