/* tests/multifile/sensor.c */
#include "common.h"

void sensor_read(void)
{
    int32_t temp = 2500;
    uint32_t pressure = 101325;

    LOG_INF("sensor: temp=%d mC pressure=%" PRIu32 " Pa", temp, pressure);
    log_memory_usage(2048, 4096);
    LOG_DBG("sensor: read complete");
}
