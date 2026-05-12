/* References a header that doesn't exist */
#include "nonexistent_header.h"
int32_t wasm_log(uint32_t log_level, const char *format, ...);
void test(void) { wasm_log(3, "hello"); }
