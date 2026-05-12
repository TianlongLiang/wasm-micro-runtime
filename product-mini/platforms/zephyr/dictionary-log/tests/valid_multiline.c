/* Test: multi-line LOG calls and string concatenation */
#include "wasm_log.h"

void test_multiline(void)
{
    int32_t sensor_id = 3;
    uint32_t value = 2048;
    int32_t offset = -12;

    LOG_INF("sensor %d reading: value=%u"
            " offset=%d applied",
            sensor_id,
            value,
            offset);

    LOG_DBG("string" " concat" " works");

    LOG_ERR("nested parens: val=%d",
            (int32_t)(value + offset));

    LOG_WRN("function arg: result=%d",
            sensor_id * 2);
}
