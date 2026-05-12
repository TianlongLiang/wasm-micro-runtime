/* tests/multifile/common.h — shared header with PRI macro and LOG */
#ifndef COMMON_H
#define COMMON_H

#include <inttypes.h>
#include "wasm_log.h"

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 1

static inline void log_memory_usage(uint32_t used, uint32_t total)
{
    LOG_INF("memory: used=%" PRIu32 " total=%" PRIu32, used, total);
}

#endif /* COMMON_H */
