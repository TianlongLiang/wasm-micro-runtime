/* Test: PRI format macros — should be skipped with warnings */
#include <inttypes.h>
#include "wasm_log.h"

void test_pri_macros(void)
{
    uint32_t val32 = 42;
    uint64_t val64 = 123456789ULL;

    /* These should be SKIPPED (PRI macros not resolvable) */
    LOG_INF("single PRI: val=%" PRIu32, val32);
    LOG_DBG("multi PRI: big=%" PRId64 " small=%" PRIu32, val64, val32);
    LOG_ERR("hex PRI: 0x%" PRIx32, val32);

    /* This is valid and should NOT be skipped */
    LOG_INF("after PRI: works fine %d", val32);
}
