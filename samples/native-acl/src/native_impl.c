#include "wasm_export.h"
#include <stddef.h>

static int
native_add(wasm_exec_env_t exec_env, int a, int b)
{
    return a + b;
}

static int
native_sub(wasm_exec_env_t exec_env, int a, int b)
{
    return a - b;
}

/* List of native symbols to register */
static NativeSymbol native_symbols[] = {
    { "native_add", native_add, "(ii)i", NULL },
    { "native_sub", native_sub, "(ii)i", NULL },
};

uint32_t get_symbol_count(void)
{
    return sizeof(native_symbols) / sizeof(NativeSymbol);
}

NativeSymbol *get_symbols(void)
{
    return native_symbols;
}
