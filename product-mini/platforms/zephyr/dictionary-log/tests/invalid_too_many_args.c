/* Test: too many format arguments (max 8 supported) */
#include "wasm_log.h"

void test_too_many_args(void)
{
    /* 9 args — SKIPPED (exceeds max 8) */
    LOG_DBG("a=%d b=%d c=%d d=%d e=%d f=%d g=%d h=%d i=%d",
            1, 2, 3, 4, 5, 6, 7, 8, 9);

    /* Exactly 8 args — OK */
    LOG_DBG("a=%d b=%d c=%d d=%d e=%d f=%d g=%d h=%d",
            1, 2, 3, 4, 5, 6, 7, 8);

    /* 7 args — OK */
    LOG_INF("a=%d b=%d c=%d d=%d e=%d f=%d g=%d",
            1, 2, 3, 4, 5, 6, 7);
}
