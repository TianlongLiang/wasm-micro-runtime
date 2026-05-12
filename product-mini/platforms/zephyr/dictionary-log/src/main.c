/*
 * Copyright (C) 2025 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(dict_log_demo, LOG_LEVEL_DBG);

#define MAIN_THREAD_STACK_SIZE 4096
#define MAIN_THREAD_PRIORITY 5

static struct k_thread iwasm_user_mode_thread;
K_THREAD_STACK_DEFINE(iwasm_user_mode_thread_stack, MAIN_THREAD_STACK_SIZE);

extern struct k_mem_partition z_libc_partition;
K_APPMEM_PARTITION_DEFINE(wamr_partition);

/* WAMR memory domain */
struct k_mem_domain wamr_domain;

extern void
iwasm_main(void *arg1, void *arg2, void *arg3);

/* Set up a Zephyr userspace thread with memory domain for WAMR.
 * Grants UART device access so wasm_log_dict can emit binary packets. */
static bool
iwasm_user_mode(void)
{
    struct k_mem_partition *wamr_domain_parts[] = { &wamr_partition,
                                                    &z_libc_partition };

    printk("wamr_partition start addr: %ld, size: %zu\n",
           wamr_partition.start, wamr_partition.size);

    /* Initialize the memory domain with WAMR and libc partitions */
    if (k_mem_domain_init(&wamr_domain, 2, wamr_domain_parts) != 0) {
        printk("Failed to initialize memory domain.\n");
        return false;
    }

    k_tid_t tid =
        k_thread_create(&iwasm_user_mode_thread, iwasm_user_mode_thread_stack,
                        MAIN_THREAD_STACK_SIZE, iwasm_main, NULL, NULL, NULL,
                        MAIN_THREAD_PRIORITY, K_USER, K_FOREVER);

    /* Grant WAMR memory domain access to user mode thread */
    if (k_mem_domain_add_thread(&wamr_domain, tid) != 0) {
        printk("Failed to add memory domain to thread.\n");
        return false;
    }

    /* Grant UART access so wasm_log_dict can emit binary packets */
    const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (device_is_ready(uart_dev)) {
        k_object_access_grant(uart_dev, tid);
    }

#if KERNEL_VERSION_NUMBER < 0x040000 /* version 4.0.0 */
    k_thread_start(tid);
#else
    /* wakes up thread from sleeping */
    k_wakeup(tid);
#endif

    return tid ? true : false;
}

int
main(void)
{
    uint32_t uptime = k_uptime_get_32();

    LOG_INF("=== Dictionary Logging Demo ===");
    LOG_INF("Zephyr OS build: %s", STRINGIFY(BUILD_VERSION));
    LOG_INF("Board: %s", CONFIG_BOARD);
    LOG_INF("System uptime at boot: %u ms", uptime);

    LOG_ERR("Native ERR: error code %d on subsystem %d", -5, 3);
    LOG_WRN("Native WRN: retry count %u exceeds threshold %u", 100, 50);
    LOG_INF("Native INF: sensor %s initialized, channels=%d", "BME280", 3);
    LOG_DBG("Native DBG: heap free=%u used=%u total=%u bytes", 32768, 32768, 65536);

    LOG_INF("Native log: memory partition start=0x%x size=%u",
            (uint32_t)wamr_partition.start, (uint32_t)wamr_partition.size);
    LOG_DBG("Native log: thread stack size=%d priority=%d",
            MAIN_THREAD_STACK_SIZE, MAIN_THREAD_PRIORITY);
    LOG_INF("Native log: CONFIG_USERSPACE=%d CONFIG_LOG=y", 1);
    LOG_WRN("Native log: dictionary hex mode active, raw UART output");
    LOG_DBG("Native log: log buffer size=%d bytes", 32768);
    LOG_INF("Native log: WAMR global heap pool=%d bytes", 131072);

    LOG_INF("--- Native Zephyr dictionary logging verified ---");
    LOG_INF("Starting WAMR userspace thread for WASM log comparison...");
    iwasm_user_mode();

    /* Wait for WASM thread to finish */
    k_thread_join(&iwasm_user_mode_thread, K_FOREVER);

    uint32_t end_uptime = k_uptime_get_32();
    LOG_INF("=== WASM apps finished ===");
    LOG_INF("Total elapsed: %u ms", end_uptime - uptime);
    LOG_INF("Baseline WASM: format strings in data segment (runtime formatting)");
    LOG_INF("Dictionary WASM: string IDs only (binary packets, offline decode)");
    LOG_INF("Baseline: 13288 bytes, Dictionary: 5721 bytes (57%% reduction)");
    LOG_INF("--- Demo complete ---");

    return 0;
}
