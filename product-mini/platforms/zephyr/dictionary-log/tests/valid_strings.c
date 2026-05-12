/* Test: string argument handling in various patterns */
#include "wasm_log.h"

void test_strings(void)
{
    char *name = "sensor_bme280";
    char *status = "OK";

    /* Single string arg */
    LOG_INF("device: %s", name);

    /* String mixed with integers */
    LOG_INF("device %s status=%d port=%u", name, 0, 8080);

    /* Multiple strings */
    LOG_DBG("src=%s dst=%s", name, status);

    /* String with width specifier */
    LOG_INF("name=%-20s id=%d", name, 42);

    /* String as only arg in a long format */
    LOG_ERR("critical failure in subsystem %s, please restart immediately", name);

    /* Empty string is valid */
    LOG_DBG("value='%s' end", status);
}
