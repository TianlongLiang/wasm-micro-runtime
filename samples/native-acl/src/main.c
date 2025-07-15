#include "wasm_export.h"
#include "bh_read_file.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Declarations from native_impl.c */
uint32_t
get_symbol_count(void);
NativeSymbol *
get_symbols(void);

int
main(int argc, char *argv[])
{
    const char *wasm_file = "wasm-apps/acl_app.wasm";
    bool aot_mode = false;
    int arg_idx = 1;

    if (argc > 1 && !strcmp(argv[1], "--aot")) {
        aot_mode = true;
        wasm_file = "wasm-apps/acl_app.aot";
        arg_idx = 2;
    }

    if (argc > arg_idx)
        wasm_file = argv[arg_idx];

    if (!aot_mode)
        printf("Run in interpreter mode\n");
    else
        printf("Run in AOT mode\n");

    static char global_heap[64 * 1024];
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(RuntimeInitArgs));

    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap);
    init_args.native_module_name = "env";
    init_args.native_symbols = get_symbols();
    init_args.n_native_symbols = get_symbol_count();

    if (!wasm_runtime_full_init(&init_args)) {
        printf("Init runtime environment failed.\n");
        return -1;
    }

    uint32_t buf_size;
    uint8_t *buf = (uint8_t *)bh_read_file_to_buffer(wasm_file, &buf_size);
    if (!buf) {
        printf("Open wasm file failed.\n");
        wasm_runtime_destroy();
        return -1;
    }

    char error_buf[128];
#if WASM_ENABLE_NATIVE_API_ACL != 0
    NativeSymbolACL *acl = NULL;
    NativeSymbol *symbols = get_symbols();
    NativeSymbol allow_symbols[] = { symbols[0] };
    /* Build allow list permitting only native_add */
    acl = wasm_runtime_create_native_symbol_acl("env", allow_symbols,
                                                sizeof(allow_symbols) /
                                                    sizeof(NativeSymbol));
    LoadArgs load_args = { 0 };
    load_args.name = "";
    load_args.native_acl_list = acl;
    load_args.native_acl_count = 1;
    wasm_module_t module = wasm_runtime_load_ex(buf, buf_size, &load_args,
                                                error_buf, sizeof(error_buf));
#else
    wasm_module_t module =
        wasm_runtime_load(buf, buf_size, error_buf, sizeof(error_buf));
#endif
    if (!module) {
        printf("Load wasm module failed. %s\n", error_buf);
        goto fail1;
    }

    wasm_module_inst_t inst = wasm_runtime_instantiate(
        module, 8192, 8192, error_buf, sizeof(error_buf));
    if (!inst) {
        printf("Instantiate module failed. %s\n", error_buf);
        goto fail2;
    }

    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 8192);
    if (!env) {
        printf("Create exec env failed.\n");
        goto fail3;
    }

    wasm_function_inst_t func;
    uint32 call_args[2];

    func = wasm_runtime_lookup_function(inst, "call_add");
    call_args[0] = 1;
    call_args[1] = 2;
    if (func && wasm_runtime_call_wasm(env, func, 2, call_args)) {
        printf("call_add result: %u\n", call_args[0]);
    }
    else {
        printf("call_add failed: %s\n", wasm_runtime_get_exception(inst));
    }

    func = wasm_runtime_lookup_function(inst, "call_sub");
    call_args[0] = 3;
    call_args[1] = 1;
    if (func && wasm_runtime_call_wasm(env, func, 2, call_args)) {
        printf("call_sub result: %u\n", call_args[0]);
    }
    else {
        printf("call_sub failed: %s\n", wasm_runtime_get_exception(inst));
    }

    wasm_runtime_destroy_exec_env(env);
fail3:
    wasm_runtime_deinstantiate(inst);
fail2:
    wasm_runtime_unload(module);
fail1:
#if WASM_ENABLE_NATIVE_API_ACL != 0
    /* Release the ACL created earlier */
    wasm_runtime_destroy_native_symbol_acl(acl);
#endif
    wasm_runtime_destroy();
    return 0;
}
