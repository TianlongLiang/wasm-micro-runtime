/* Deliberately broken C source */
#include <stdint.h>
int32_t wasm_log(uint32_t log_level, const char *format, ...);
void broken(void) {
    wasm_log(3, "missing semicolon"
}
