/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "bh_platform.h"
#include "bh_log.h"
#include "wasm_export.h"

#if defined(CRASH_APP_OOB)
#include "test_wasm_oob.h"
#elif defined(CRASH_APP_STACKOVERFLOW)
#include "test_wasm_stackoverflow.h"
#else
#error "Define CRASH_APP_OOB or CRASH_APP_STACKOVERFLOW"
#endif

#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define CONFIG_APP_STACK_SIZE 8192
#define CONFIG_APP_HEAP_SIZE 8192

static char global_heap_buf[CONFIG_GLOBAL_HEAP_BUF_SIZE] = { 0 };

void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    uint8 *wasm_file_buf = NULL;
    uint32 wasm_file_size;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    wasm_function_inst_t func = NULL;
    wasm_exec_env_t exec_env = NULL;
    RuntimeInitArgs init_args;
    char error_buf[128];
    const char *exception;
    unsigned argv[2] = { 0 };

    (void)arg1;
    (void)arg2;
    (void)arg3;

    printk("Coredump debug demo: starting WAMR...\n");

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("Init runtime failed.\n");
        return;
    }

    wasm_file_buf = (uint8 *)wasm_test_file;
    wasm_file_size = sizeof(wasm_test_file);

    wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_size,
                                     error_buf, sizeof(error_buf));
    if (!wasm_module) {
        printk("Load failed: %s\n", error_buf);
        goto cleanup;
    }

    wasm_module_inst = wasm_runtime_instantiate(
        wasm_module, CONFIG_APP_STACK_SIZE, CONFIG_APP_HEAP_SIZE,
        error_buf, sizeof(error_buf));
    if (!wasm_module_inst) {
        printk("Instantiate failed: %s\n", error_buf);
        goto cleanup;
    }

    func = wasm_runtime_lookup_function(wasm_module_inst, "app_main");
    if (!func) {
        printk("Lookup 'app_main' failed.\n");
        goto cleanup;
    }

    exec_env = wasm_runtime_create_exec_env(wasm_module_inst,
                                             CONFIG_APP_STACK_SIZE);
    if (!exec_env) {
        printk("Create exec_env failed.\n");
        goto cleanup;
    }

    printk("Calling WASM app_main (expect crash)...\n");

    /* This call will trap. WAMR prints the WASM call stack automatically. */
    wasm_runtime_call_wasm(exec_env, func, 0, argv);

    exception = wasm_runtime_get_exception(wasm_module_inst);
    if (exception) {
        printk("WASM exception: %s\n", exception);
        printk("Triggering Zephyr coredump via k_panic()...\n");

        /* Trigger Zephyr fatal error -> coredump */
        k_panic();
    }

cleanup:
    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    if (wasm_module_inst)
        wasm_runtime_deinstantiate(wasm_module_inst);
    if (wasm_module)
        wasm_runtime_unload(wasm_module);
    wasm_runtime_destroy();
}
