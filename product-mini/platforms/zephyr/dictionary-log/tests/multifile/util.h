/* tests/multifile/util.h — utility with inline LOG call */
#ifndef UTIL_H
#define UTIL_H

#include "wasm_log.h"

static inline void util_log_init(int module_id)
{
    LOG_DBG("util: module %d initialized", module_id);
}

static inline int util_clamp(int val, int lo, int hi)
{
    if (val < lo) {
        LOG_WRN("util: clamping %d to min %d", val, lo);
        return lo;
    }
    if (val > hi) {
        LOG_WRN("util: clamping %d to max %d", val, hi);
        return hi;
    }
    return val;
}

#endif /* UTIL_H */
