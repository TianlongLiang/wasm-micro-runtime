/* tests/multifile/main.c */
#include "util.h"
#include "common.h"

int main(void)
{
    LOG_INF("app starting v%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR);
    util_log_init(0);
    log_memory_usage(1024, 4096);
    LOG_INF("main done");
    return 0;
}
