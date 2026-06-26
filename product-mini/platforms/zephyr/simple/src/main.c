/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <string.h>
#include "bh_platform.h"
#include "bh_assert.h"
#include "bh_log.h"
#include "bh_queue.h"
#include "wasm_export.h"
#if defined(BUILD_TARGET_RISCV64_LP64) || defined(BUILD_TARGET_RISCV32_ILP32)
#include "test_wasm_riscv64.h"
#else
#include "test_wasm.h"
#endif /* end of BUILD_TARGET_RISCV64_LP64 || BUILD_TARGET_RISCV32_ILP32 */

#if defined(BUILD_TARGET_RISCV64_LP64) || defined(BUILD_TARGET_RISCV32_ILP32)
#define CONFIG_GLOBAL_HEAP_BUF_SIZE 5120
#define CONFIG_APP_STACK_SIZE 512
#define CONFIG_APP_HEAP_SIZE 512
#else /* else of BUILD_TARGET_RISCV64_LP64 || BUILD_TARGET_RISCV32_ILP32 */
#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define CONFIG_APP_STACK_SIZE 8192
#define CONFIG_APP_HEAP_SIZE 8192
#endif /* end of BUILD_TARGET_RISCV64_LP64 || BUILD_TARGET_RISCV32_ILP32 */

#define CONFIG_MAIN_THREAD_STACK_SIZE 8192

static int app_argc;
static char **app_argv;

/**
 * Find the unique main function from a WASM module instance
 * and execute that function.
 *
 * @param module_inst the WASM module instance
 * @param argc the number of arguments
 * @param argv the arguments array
 *
 * @return true if the main function is called, false otherwise.
 */
bool
wasm_application_execute_main(wasm_module_inst_t module_inst, int argc,
                              char *argv[]);

struct test_msg {
    int worker_id;
    int seq;
};

#define MSGS_PER_WORKER 2
#define NUM_WORKERS 2

struct worker_ctx {
    int id;
    bh_queue *queue;
    wasm_module_t module;
};

static void *
worker_entry(void *arg)
{
    struct worker_ctx *ctx = (struct worker_ctx *)arg;
    wasm_module_inst_t inst;
    char error_buf[128];
    int i;

    inst = wasm_runtime_instantiate(ctx->module, CONFIG_APP_STACK_SIZE,
                                    CONFIG_APP_HEAP_SIZE, error_buf,
                                    sizeof(error_buf));
    if (!inst) {
        printf("  worker %d: instantiate failed: %s\n", ctx->id, error_buf);
        return NULL;
    }

    wasm_application_execute_main(inst, 0, NULL);

    for (i = 0; i < MSGS_PER_WORKER; i++) {
        struct test_msg *msg = BH_MALLOC(sizeof(struct test_msg));
        if (!msg)
            break;
        msg->worker_id = ctx->id;
        msg->seq = i;
        /* bh_post_msg takes ownership of msg body */
        if (!bh_post_msg(ctx->queue, 0, msg, sizeof(struct test_msg))) {
            BH_FREE(msg);
            break;
        }
    }

    wasm_runtime_deinstantiate(inst);
    return NULL;
}

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
static char global_heap_buf[CONFIG_GLOBAL_HEAP_BUF_SIZE] = { 0 };
#endif

void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    bh_queue *queue = NULL;
    char error_buf[128];
    korp_tid worker_tids[NUM_WORKERS] = { 0 };
    struct worker_ctx worker_ctxs[NUM_WORKERS];
    int i, received = 0;

    (void)arg1; (void)arg2; (void)arg3;

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#else
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#endif
    if (!wasm_runtime_full_init(&init_args)) {
        printf("WAMR init failed\n");
        return;
    }

    module = wasm_runtime_load((uint8 *)wasm_test_file, sizeof(wasm_test_file),
                               error_buf, sizeof(error_buf));
    if (!module) {
        printf("Load failed: %s\n", error_buf);
        goto cleanup;
    }

    queue = bh_queue_create();
    if (!queue) {
        printf("Queue create failed\n");
        goto cleanup;
    }

    printf("=== simple kernel-mode MT demo: %d workers x %d msgs ===\n",
           NUM_WORKERS, MSGS_PER_WORKER);

    for (i = 0; i < NUM_WORKERS; i++) {
        worker_ctxs[i].id = i;
        worker_ctxs[i].queue = queue;
        worker_ctxs[i].module = module;
        if (os_thread_create(&worker_tids[i], worker_entry, &worker_ctxs[i],
                             CONFIG_APP_STACK_SIZE) != BHT_OK) {
            printf("Failed to create worker %d\n", i);
            break;
        }
    }

    while (received < NUM_WORKERS * MSGS_PER_WORKER) {
        bh_message_t bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
        if (!bmsg)
            continue;
        struct test_msg *m = (struct test_msg *)bh_message_payload(bmsg);
        printf("  [recv] worker %d seq %d\n", m->worker_id, m->seq);
        /* bh_free_msg frees both the node and the body */
        bh_free_msg(bmsg);
        received++;
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        if (worker_tids[i])
            os_thread_join(worker_tids[i], NULL);
    }

    printf("=== simple demo complete: received %d msgs ===\n", received);

cleanup:
    if (queue)
        bh_queue_destroy(queue);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
}

#define MAIN_THREAD_STACK_SIZE (CONFIG_MAIN_THREAD_STACK_SIZE)
#define MAIN_THREAD_PRIORITY 5

K_THREAD_STACK_DEFINE(iwasm_main_thread_stack, MAIN_THREAD_STACK_SIZE);
static struct k_thread iwasm_main_thread;

bool
iwasm_init(void)
{
    k_tid_t tid = k_thread_create(
        &iwasm_main_thread, iwasm_main_thread_stack, MAIN_THREAD_STACK_SIZE,
        iwasm_main, NULL, NULL, NULL, MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
    return tid ? true : false;
}

#if KERNEL_VERSION_NUMBER < 0x030400 /* version 3.4.0 */
void
main(void)
{
    iwasm_init();
}
#else
int
main(void)
{
    iwasm_init();
    return 0;
}
#endif
