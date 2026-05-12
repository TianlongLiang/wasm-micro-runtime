/* Test: basic valid LOG calls with various arg types */
#include "wasm_log.h"

void test_valid_basic(void)
{
    int32_t i = 42;
    uint32_t u = 100;

    LOG_INF("hello world");
    LOG_ERR("error code %d", i);
    LOG_WRN("count %u limit %u", u, u);
    LOG_DBG("hex value 0x%x", u);
    LOG_VERBOSE("verbose %d %u %d", i, u, i);
}
