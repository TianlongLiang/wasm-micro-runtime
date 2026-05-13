/* This file should be rejected by the extraction tool (C++ not supported) */
#include <stdint.h>
int32_t wasm_log(uint32_t level, const char *fmt, ...);
void test(void) { wasm_log(3, "hello from cpp"); }
