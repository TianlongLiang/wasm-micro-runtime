#include "wasm_log.h"

void too_many(void)
{
    LOG_INF("a=%d b=%d c=%d d=%d e=%d f=%d g=%d h=%d i=%d",
            1, 2, 3, 4, 5, 6, 7, 8, 9);
}
