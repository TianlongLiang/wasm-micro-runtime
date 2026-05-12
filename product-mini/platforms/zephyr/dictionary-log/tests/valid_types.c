/* Test: all supported format specifier types */
#include "wasm_log.h"

void test_types(void)
{
    int32_t i32 = -5;
    uint32_t u32 = 42;
    int64_t i64 = -999999LL;
    uint64_t u64 = 123456789ULL;
    double f64 = 3.14;
    char *s = "hello";

    /* int32 specifiers */
    LOG_DBG("d=%d i=%i u=%u x=%x X=%X o=%o c=%c p=%p",
            i32, i32, u32, u32, u32, u32, 'A', &i32);

    /* int64 specifiers */
    LOG_INF("ld=%ld llu=%llu llx=%llx", i64, u64, u64);

    /* float64 specifiers */
    LOG_INF("f=%f e=%e g=%g F=%F E=%E G=%G", f64, f64, f64, f64, f64, f64);

    /* string */
    LOG_INF("name=%s", s);

    /* percent literal (no arg consumed) */
    LOG_INF("100%% complete, %d items", u32);

    /* width and precision modifiers */
    LOG_DBG("%10d %-20s %08x %5.2f", i32, s, u32, f64);
}
