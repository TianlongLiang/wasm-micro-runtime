/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_export.h"
#include "bh_read_file.h"
#include "bh_getopt.h"

extern NativeSymbol *
get_native_symbols(int *count);

void
print_usage(void)
{
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "  -f [path of wasm file]\n");
}

int
main(int argc, char *argv_main[])
{
    static char global_heap_buf[512 * 1024];
    char *buffer = NULL;
    char error_buf[128];
    int opt;
    char *wasm_path = NULL;

    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t func = NULL;
    uint32_t buf_size, stack_size = 8192, heap_size = 8192;
    uint32_t argv[1] = { 0 };
    int native_count;
    NativeSymbol *native_symbols;
    RuntimeInitArgs init_args;

    while ((opt = getopt(argc, argv_main, "hf:")) != -1) {
        switch (opt) {
            case 'f':
                wasm_path = optarg;
                break;
            case 'h':
                print_usage();
                return 0;
            case '?':
                print_usage();
                return 0;
        }
    }
    if (!wasm_path) {
        print_usage();
        return 0;
    }

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    native_symbols = get_native_symbols(&native_count);
    init_args.n_native_symbols = native_count;
    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;

    if (!wasm_runtime_full_init(&init_args)) {
        printf("WAMR init failed.\n");
        return -1;
    }

    buffer = bh_read_file_to_buffer(wasm_path, &buf_size);
    if (!buffer) {
        printf("Open wasm file [%s] failed.\n", wasm_path);
        goto fail;
    }

    module = wasm_runtime_load((uint8_t *)buffer, buf_size, error_buf,
                               sizeof(error_buf));
    if (!module) {
        printf("Load failed: %s\n", error_buf);
        goto fail;
    }

    module_inst = wasm_runtime_instantiate(module, stack_size, heap_size,
                                           error_buf, sizeof(error_buf));
    if (!module_inst) {
        printf("Instantiate failed: %s\n", error_buf);
        goto fail;
    }

    exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
    if (!exec_env) {
        printf("Create exec_env failed.\n");
        goto fail;
    }

    func = wasm_runtime_lookup_function(module_inst, "run");
    if (!func) {
        printf("Lookup 'run' failed.\n");
        goto fail;
    }

    if (wasm_runtime_call_wasm(exec_env, func, 0, argv)) {
        printf("\nrun() returned: %d (%s)\n",
               (int)argv[0], (int)argv[0] == 0 ? "PASS" : "FAIL");
    }
    else {
        printf("Call failed: %s\n",
               wasm_runtime_get_exception(module_inst));
    }

fail:
    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    if (module_inst)
        wasm_runtime_deinstantiate(module_inst);
    if (module)
        wasm_runtime_unload(module);
    if (buffer)
        BH_FREE(buffer);
    wasm_runtime_destroy();
    return 0;
}
