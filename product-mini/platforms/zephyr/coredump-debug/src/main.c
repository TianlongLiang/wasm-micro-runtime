/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <zephyr/kernel.h>

#define MAIN_THREAD_STACK_SIZE 8192
#define MAIN_THREAD_PRIORITY 5

extern void
iwasm_main(void *arg1, void *arg2, void *arg3);

K_THREAD_STACK_DEFINE(iwasm_main_thread_stack, MAIN_THREAD_STACK_SIZE);
static struct k_thread iwasm_main_thread;

int
main(void)
{
    k_tid_t tid = k_thread_create(
        &iwasm_main_thread, iwasm_main_thread_stack, MAIN_THREAD_STACK_SIZE,
        iwasm_main, NULL, NULL, NULL, MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);

    return tid ? 0 : -1;
}
