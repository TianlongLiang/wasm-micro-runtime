/* tests/multifile/helper.c */
#include "util.h"
#include "common.h"

void helper_process(int value)
{
    int clamped = util_clamp(value, 0, 100);
    LOG_INF("helper: processed value=%d clamped=%d", value, clamped);
    log_memory_usage(3072, 4096);

    /* Same string as in sensor.c — tests that duplicates get separate IDs */
    LOG_DBG("sensor: read complete");
}
