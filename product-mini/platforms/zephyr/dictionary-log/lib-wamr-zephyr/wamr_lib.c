/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <string.h>
#include "bh_platform.h"
#include "bh_assert.h"
#include "bh_log.h"
#include "wasm_export.h"
#include "lib_export.h"

#include "test_wasm_baseline.h"
#include "test_wasm_dict.h"
#include "test_wasm_network.h"

#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define CONFIG_APP_STACK_SIZE 8192
#define CONFIG_APP_HEAP_SIZE 8192

static int app_argc;
static char **app_argv;

/* Declared in lib_wasm_dict_log.c */
extern uint32
get_lib_wasm_log_export_apis(NativeSymbol **p_native_symbols);

/**
 * Find the unique main function from a WASM module instance
 * and execute that function.
 */
bool
wasm_application_execute_main(wasm_module_inst_t module_inst, int argc,
                              char *argv[]);

static void *
app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;
    wasm_function_inst_t func;
    wasm_exec_env_t exec_env;
    unsigned argv[2] = { 0 };

    if (wasm_runtime_lookup_function(module_inst, "main")
        || wasm_runtime_lookup_function(module_inst, "__main_argc_argv")) {
        LOG_VERBOSE("Calling main function\n");
        wasm_application_execute_main(module_inst, app_argc, app_argv);
    }
    else if ((func = wasm_runtime_lookup_function(module_inst, "app_main"))) {
        exec_env =
            wasm_runtime_create_exec_env(module_inst, CONFIG_APP_HEAP_SIZE);
        if (!exec_env) {
            os_printf("Create exec env failed\n");
            return NULL;
        }

        LOG_VERBOSE("Calling app_main function\n");
        wasm_runtime_call_wasm(exec_env, func, 0, argv);

        if (!wasm_runtime_get_exception(module_inst)) {
            os_printf("result: 0x%x\n", argv[0]);
        }

        wasm_runtime_destroy_exec_env(exec_env);
    }
    else {
        os_printf("Failed to lookup function main or app_main to call\n");
        return NULL;
    }

    if ((exception = wasm_runtime_get_exception(module_inst)))
        os_printf("%s\n", exception);

    return NULL;
}

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
static char global_heap_buf[CONFIG_GLOBAL_HEAP_BUF_SIZE] = { 0 };
#endif

static void
run_wasm_app(const char *label, uint8 app_id, uint8 *wasm_buf, uint32 wasm_size)
{
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    char error_buf[128];

    os_printf("[%s] Loading WASM app (%u bytes)\n", label, wasm_size);

    /* load WASM module */
    if (!(wasm_module = wasm_runtime_load(wasm_buf, wasm_size, error_buf,
                                          sizeof(error_buf)))) {
        os_printf("[%s] Load error: %s\n", label, error_buf);
        return;
    }

    /* instantiate the module */
    if (!(wasm_module_inst = wasm_runtime_instantiate(
              wasm_module, CONFIG_APP_STACK_SIZE, CONFIG_APP_HEAP_SIZE,
              error_buf, sizeof(error_buf)))) {
        os_printf("[%s] Instantiate error: %s\n", label, error_buf);
        goto fail_unload;
    }

    /* Set app_id on the singleton exec_env so the native log wrapper
     * can embed it in packets without trusting the WASM app */
    wasm_exec_env_t exec_env = wasm_runtime_get_exec_env_singleton(
        wasm_module_inst);
    if (exec_env) {
        wasm_runtime_set_user_data(exec_env, (void *)(uintptr_t)app_id);
    }

    /* invoke the main function */
    app_instance_main(wasm_module_inst);

    /* destroy the module instance */
    wasm_runtime_deinstantiate(wasm_module_inst);

fail_unload:
    /* unload the module */
    wasm_runtime_unload(wasm_module);
}

void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    int start, end;
    start = k_uptime_get_32();
    RuntimeInitArgs init_args;
    NativeSymbol *native_symbols;
    uint32 n_native_symbols;
#if WASM_ENABLE_LOG != 0
    int log_verbose_level = 2;
#endif

    (void)arg1;
    (void)arg2;
    (void)arg3;

    os_printf("User mode thread: start\n");

    memset(&init_args, 0, sizeof(RuntimeInitArgs));

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#elif (defined(CONFIG_COMMON_LIBC_MALLOC)            \
       && CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE != 0) \
    || defined(CONFIG_NEWLIB_LIBC)
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#else
#error "memory allocation scheme is not defined."
#endif

    /* Register native symbols for wasm_log */
    n_native_symbols = get_lib_wasm_log_export_apis(&native_symbols);
    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;
    init_args.n_native_symbols = n_native_symbols;

    /* initialize runtime environment */
    if (!wasm_runtime_full_init(&init_args)) {
        os_printf("Init runtime environment failed.\n");
        return;
    }

#if WASM_ENABLE_LOG != 0
    bh_log_set_verbose_level(log_verbose_level);
#endif

    /* Run the baseline WASM app (app_id=0, sensor app) */
    run_wasm_app("BASELINE", 0, (uint8 *)wasm_test_file_baseline,
                 sizeof(wasm_test_file_baseline));

    /* Run the dictionary WASM app (app_id=0, sensor app) */
    run_wasm_app("DICT_SENSOR", 0, (uint8 *)wasm_test_file_dict,
                 sizeof(wasm_test_file_dict));

    /* Run the network dictionary WASM app (app_id=1) */
    run_wasm_app("DICT_NETWORK", 1, (uint8 *)wasm_test_file_network,
                 sizeof(wasm_test_file_network));

    /* destroy runtime environment */
    wasm_runtime_destroy();

    end = k_uptime_get_32();

    os_printf("User mode thread: elapsed %d\n", (end - start));
}
