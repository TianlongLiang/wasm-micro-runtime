/* tests/singlefile/types_test.c — exercises all format specifier types */
#include <inttypes.h>
#include "wasm_log.h"

void test_all_format_types(void)
{
    int32_t i32 = -42;
    uint32_t u32 = 255;
    int64_t i64 = -999999LL;
    uint64_t u64 = 123456789ULL;
    double f64 = 3.14159;
    char *name = "sensor_bme280";
    char ch = 'X';
    void *ptr = &i32;

    /* int32 specifiers: %d %i %u %x %X %o %c %p */
    LOG_DBG("int types: d=%d i=%i u=%u x=0x%x X=0x%X o=%o", i32, i32, u32, u32, u32, u32);
    LOG_DBG("char and ptr: c=%c p=%p", ch, ptr);

    /* int64 specifiers: %ld %lu %lld %llu %llx */
    LOG_INF("long types: ld=%ld llu=%llu llx=0x%llx", i64, u64, u64);

    /* float64 specifiers: %f %e %g %F %E %G */
    LOG_INF("float types: f=%f e=%e g=%g", f64, f64, f64);
    LOG_DBG("FLOAT types: F=%F E=%E G=%G", f64, f64, f64);

    /* string specifier: %s */
    LOG_INF("string: name=%s", name);
    LOG_DBG("multi string: a=%s b=%s", name, "other");

    /* width and precision modifiers */
    LOG_DBG("width: %10d %-20s %08x %5.2f", i32, name, u32, f64);

    /* percent literal: %% */
    LOG_INF("progress: 100%% done, %d items processed", u32);

    /* PRI macros (resolved by preprocessor) */
    LOG_INF("PRI: u32=%" PRIu32 " i64=%" PRId64, u32, i64);

    /* mixed types in one call */
    LOG_ERR("mixed: int=%d str=%s float=%f hex=0x%x", i32, name, f64, u32);

    /* zero args */
    LOG_INF("no args at all");

    /* max 8 args */
    LOG_DBG("eight: %d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8);

    /* escaped quotes in format string */
    LOG_INF("say \"hello\" to %s", name);

    /* backslash sequences (literal \\n, not newline) */
    LOG_DBG("path: %s\\nline: %d", name, i32);

    /* very long format string */
    LOG_INF("this is a very long format string that exceeds one hundred characters to test that the extraction handles long strings without any truncation problems: val=%d end", i32);

    /* adjacent calls on same line */
    LOG_INF("adjacent_a"); LOG_INF("adjacent_b");

    /* same string as in sensor.c — should get separate ID (not deduplicated) */
    LOG_INF("sensor: read complete");
}
