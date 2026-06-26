/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "bh_platform.h"
#include "bh_queue.h"
#include "wasm_export.h"

#if defined(BUILD_TARGET_RISCV64_LP64) || defined(BUILD_TARGET_RISCV32_ILP32)
#include "test_wasm_riscv64.h"
#else
#include "test_wasm.h"
#endif

#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define APP_STACK_SIZE 8192
#define APP_HEAP_SIZE 8192
#define WORKER_STACK_SIZE 4096
#define MSGS_PER_WORKER 5

static char global_heap_buf[CONFIG_GLOBAL_HEAP_BUF_SIZE] = { 0 };

struct test_msg {
    int worker_id;
    int seq;
    char payload[32];
};

struct worker_ctx {
    int id;
    bh_queue *queue;
    wasm_module_t module;
};

/*
 * Single-thread variant: init runtime, instantiate one module,
 * run main, then one bh_queue round trip to exercise the
 * condvar code path under user mode.
 *
 * bh_queue ownership: bh_post_msg stores the body pointer directly;
 * on success, the queue owns it. bh_free_msg frees both node and body.
 * Producer frees payload only if post fails.
 */
static void
iwasm_main_st(void)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t inst = NULL;
    bh_queue *queue = NULL;
    char error_buf[128];
    struct test_msg *msg = NULL;
    bh_message_t bmsg;

    printk("=== WAMR User-Mode ST + bh_queue Demo ===\n");

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n");
        return;
    }

    module = wasm_runtime_load((uint8_t *)wasm_test_file,
                               sizeof(wasm_test_file), error_buf,
                               sizeof(error_buf));
    if (!module) {
        printk("load failed: %s\n", error_buf);
        goto cleanup;
    }

    inst = wasm_runtime_instantiate(module, APP_STACK_SIZE, APP_HEAP_SIZE,
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        printk("instantiate failed: %s\n", error_buf);
        goto cleanup;
    }

    wasm_application_execute_main(inst, 0, NULL);

    /* Under CONFIG_USERSPACE, bh_queue_create internally calls k_mutex_init
     * on a BH_MALLOC'd mutex. With flag=0, the mutex is unregistered in the
     * kernel object table, causing z_vrfy_k_mutex_init to fail with
     * "not a valid k_mutex / address is not a known kernel object".
     * With flag=1, the mutex is allocated via k_object_alloc(K_OBJ_MUTEX),
     * which registers it, so validation succeeds. */
    queue = bh_queue_create();
    if (!queue) {
        printk("queue create failed\n");
        goto cleanup;
    }
    printk("bh_queue created (user mode)\n");

    msg = wasm_runtime_malloc(sizeof(struct test_msg));
    if (!msg)
        goto cleanup;
    msg->worker_id = 0;
    msg->seq = 0;
    snprintf(msg->payload, sizeof(msg->payload), "st-msg");
    if (!bh_post_msg(queue, 0, msg, sizeof(struct test_msg))) {
        wasm_runtime_free(msg);
        msg = NULL;
        printk("post failed\n");
        goto cleanup;
    }
    /* bh_post_msg takes ownership on success */
    msg = NULL;

    bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
    if (bmsg) {
        struct test_msg *got = (struct test_msg *)bh_message_payload(bmsg);
        printk("  [recv] worker %d seq %d \"%s\"\n",
               got->worker_id, got->seq, got->payload);
        bh_free_msg(bmsg);
    }

cleanup:
    if (msg)
        wasm_runtime_free(msg);
    if (queue)
        bh_queue_destroy(queue);
    if (inst)
        wasm_runtime_deinstantiate(inst);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
    printk("=== ST Demo complete ===\n");
}

/*
 * Worker thread: each worker instantiates the WASM module, runs its main
 * function, then sends messages to the shared bh_queue.
 *
 * Created via os_thread_create — under CONFIG_USERSPACE, this sets
 * K_USER | K_INHERIT_PERMS so the worker inherits access to bh_queue's
 * internal mutex/condvar and the WAMR heap lock.
 *
 * bh_queue ownership: same as ST — post takes ownership on success,
 * producer frees only on failure.
 */
static void *
worker_entry(void *arg)
{
    struct worker_ctx *ctx = (struct worker_ctx *)arg;
    wasm_module_inst_t inst;
    char error_buf[128];
    int i;

    /* Each worker gets its own WASM instance — thread-safe, no shared state */
    inst = wasm_runtime_instantiate(ctx->module, APP_STACK_SIZE, APP_HEAP_SIZE,
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        printk("  worker %d: instantiate failed: %s\n", ctx->id, error_buf);
        return NULL;
    }

    printk("  worker %d: running WASM app\n", ctx->id);
    wasm_application_execute_main(inst, 0, NULL);
    {
        const char *exc = wasm_runtime_get_exception(inst);
        if (exc)
            printk("  worker %d: WASM exception: %s\n", ctx->id, exc);
    }

    /* Now use bh_queue to send messages back to the main thread */
    for (i = 0; i < MSGS_PER_WORKER; i++) {
        struct test_msg *msg = wasm_runtime_malloc(sizeof(struct test_msg));
        if (!msg) {
            printk("  [send] worker %d: malloc failed for msg %d\n",
                   ctx->id, i);
            break;
        }
        msg->worker_id = ctx->id;
        msg->seq = i;
        snprintf(msg->payload, sizeof(msg->payload), "w%d-msg%d",
                 ctx->id, i);

        printk("  [send] worker %d: msg %d \"%s\"\n", ctx->id, i,
               msg->payload);

        if (!bh_post_msg(ctx->queue, 0, msg, sizeof(struct test_msg))) {
            printk("  [send] worker %d: post failed for msg %d\n",
                   ctx->id, i);
            wasm_runtime_free(msg);
            break;
        }
        /* bh_post_msg took ownership; do not free msg here */
    }

    wasm_runtime_deinstantiate(inst);
    return NULL;
}

/*
 * Multi-thread variant: spawns worker threads that each run the WASM app
 * then send messages via bh_queue. Main thread dequeues messages using
 * blocking condvar wait.
 *
 * Consumer ownership: bh_get_msg returns a message; the payload is accessed
 * via bh_message_payload. bh_free_msg frees both the node and the body —
 * consumer must NOT separately free the payload.
 */
static void
iwasm_main_mt(int num_workers)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    bh_queue *queue = NULL;
    korp_tid *worker_tids = NULL;
    struct worker_ctx *worker_ctxs = NULL;
    char error_buf[128];
    int i, total_dequeued = 0;
    int expected;

    if (num_workers <= 0)
        num_workers = 2;
    expected = num_workers * MSGS_PER_WORKER;

    printk("=== WAMR User-Mode MT + bh_queue Demo ===\n");

    /* Initialize WAMR runtime */
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n");
        return;
    }

    /* Load the WASM module */
    module = wasm_runtime_load((uint8_t *)wasm_test_file,
                               sizeof(wasm_test_file), error_buf,
                               sizeof(error_buf));
    if (!module) {
        printk("Load WASM module failed: %s\n", error_buf);
        goto cleanup;
    }

    /* Create bh_queue — lock and condvar are allocated via k_object_alloc */
    queue = bh_queue_create();
    if (!queue) {
        printk("bh_queue_create failed\n");
        goto cleanup;
    }
    printk("bh_queue created (user mode)\n");

    /* Allocate worker tracking arrays from WAMR heap */
    worker_tids = wasm_runtime_malloc(num_workers * sizeof(korp_tid));
    worker_ctxs = wasm_runtime_malloc(num_workers * sizeof(struct worker_ctx));
    if (!worker_tids || !worker_ctxs) {
        printk("Failed to allocate worker arrays\n");
        goto cleanup;
    }
    memset(worker_tids, 0, num_workers * sizeof(korp_tid));

    /* Spawn worker threads via os_thread_create.
     * Under CONFIG_USERSPACE, os_thread_create detects user context and
     * sets K_USER | K_INHERIT_PERMS, so workers inherit access to all
     * kernel objects (bh_queue lock, condvar, WAMR heap lock, etc.). */
    printk("\nStarting %d workers (WASM + bh_queue, %d msgs each):\n",
           num_workers, MSGS_PER_WORKER);
    for (i = 0; i < num_workers; i++) {
        worker_ctxs[i].id = i;
        worker_ctxs[i].queue = queue;
        worker_ctxs[i].module = module;
        if (os_thread_create(&worker_tids[i], worker_entry,
                             &worker_ctxs[i], APP_STACK_SIZE) != BHT_OK) {
            printk("Failed to create worker %d\n", i);
            num_workers = i;
            expected = num_workers * MSGS_PER_WORKER;
            break;
        }
    }

    /* Consume messages concurrently — blocks on condvar until a producer
     * signals via bh_post_msg. */
    while (total_dequeued < expected) {
        bh_message_t bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
        if (!bmsg)
            continue;

        struct test_msg *msg = (struct test_msg *)bh_message_payload(bmsg);
        if (msg) {
            printk("  [recv] #%d from worker %d seq %d \"%s\"\n",
                   total_dequeued, msg->worker_id, msg->seq, msg->payload);
        }
        bh_free_msg(bmsg);
        total_dequeued++;
    }

    /* Join worker threads */
    for (i = 0; i < num_workers; i++) {
        if (worker_tids[i])
            os_thread_join(worker_tids[i], NULL);
    }

    printk("\nTotal: sent %d, received %d\n", expected, total_dequeued);

cleanup:
    if (worker_ctxs)
        wasm_runtime_free(worker_ctxs);
    if (worker_tids)
        wasm_runtime_free(worker_tids);
    if (queue)
        bh_queue_destroy(queue);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
    printk("=== MT Demo complete ===\n");
}

/*
 * Dispatcher — what kernel main() spawns.
 * Picks between ST and MT variants based on USER_MODE_MULTITHREAD define.
 */
void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    (void)arg2;
    (void)arg3;
#ifdef USER_MODE_MULTITHREAD
    iwasm_main_mt((int)(intptr_t)arg1);
#else
    (void)arg1;
    iwasm_main_st();
#endif
}
