/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>

#include <zephyr/version.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wamr_log_test, LOG_LEVEL_DBG);

LOG_INSTANCE_REGISTER(wamr, cpu,   LOG_LEVEL_DBG);
LOG_INSTANCE_REGISTER(wamr, memory,LOG_LEVEL_WRN);

static const struct log_source_const_data *log_cpu =
    LOG_INSTANCE_PTR(wamr, cpu);
static const struct log_source_const_data *log_mem =
    LOG_INSTANCE_PTR(wamr, memory);

#define MAIN_THREAD_STACK_SIZE 2048
#define MAIN_THREAD_PRIORITY 5

// K_EVENT_DEFINE(my_evt);

static struct k_thread iwasm_user_mode_thread;
K_THREAD_STACK_DEFINE(iwasm_user_mode_thread_stack, MAIN_THREAD_STACK_SIZE);

extern struct k_mem_partition z_libc_partition;
K_APPMEM_PARTITION_DEFINE(wamr_partition);

/* WAMR memory domain */
struct k_mem_domain wamr_domain;

extern void
iwasm_main(void *arg1, void *arg2, void *arg3);

bool
iwasm_user_mode(void)
{
    struct k_mem_partition *wamr_domain_parts[] = { &wamr_partition,
                                                    &z_libc_partition };

    int a = 22;
    uint32_t b = 442;
    // LOG_INST_DBG(log_cpu, "core temp dummy=%d", a);       /* wamr.cpu: DBG */
    LOG_INST_WRN(log_mem, "heap dummy=%u bytes", b); /* wamr.memory: WRN */

    LOG_ERR("error string");
    LOG_WRN("warn string");
    LOG_DBG("debug string");
    LOG_INF("info string");

    do { if (!((0 && (((4) <= 0) || ((0 == 0) && ((4) <= __log_level) && ((4) <= 0) ) )) && (0 || !0 || (4 <= ((const struct log_source_const_data *)__log_current_const_data)->level)) && (!0 || k_is_user_context() || ((4) <= ((*(&(((struct log_source_dynamic_data *)__log_current_const_data)->filters)) >> (3U * (0))) & ((1UL << (3U)) - 1U)))))) { break; } if (0) { do { z_log_minimal_printk("%c: " "debug string2" "\n", z_log_minimal_level_to_char(4)); } while (0); break; } int _mode; _Bool string_ok; string_ok = 1; if (!string_ok) { ; break; } do {  z_log_msg_runtime_create((0), (void *)(__log_current_const_data), (4), (uint8_t *)(((void*)0)), (0), ((0 << 3) | (0 ? ((1UL << (1)) | (1UL << (0))) : 0)) | (0 ? (1UL << (6)) : 0), "debug string2"); (_mode) = Z_LOG_MSG_MODE_RUNTIME; } while (0); (void)_mode; if (0) { z_log_printf_arg_checker("debug string2"); } } while (0);

    /*
     * With CONFIG_USERSPACE=y every build re-defines printk() as a system-call
     * stub (k_sys_printk / z_impl_printk). Will overwrite theCONFIG_LOG_PRINTK,
     * printk won't be redirected to the log module.
     */
    printk("wamr_partition start addr: %ld, size: %zu\n", wamr_partition.start,
           wamr_partition.size);

    /* Initialize the memory domain with single WAMR partition */
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

    // k_object_access_grant(&my_evt, tid);

#if KERNEL_VERSION_NUMBER < 0x040000 /* version 4.0.0 */
    /* k_thread_start is a legacy API for compatibility. Modern Zephyr threads
     * are initialized in the "sleeping" state and do not need special handling
     * for "start".*/
    k_thread_start(tid);
#else
    /* wakes up thread from sleeping */
    k_wakeup(tid);
#endif

    return tid ? true : false;
}

#if KERNEL_VERSION_NUMBER < 0x030400 /* version 3.4.0 */
void
main(void)
{
    iwasm_user_mode();
}
#else
int
main(void)
{
    iwasm_user_mode();
    return 0;
}
#endif
