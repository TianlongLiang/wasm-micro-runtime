/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "test_common.h"

#include "platform_api_extension.h"

ZTEST_SUITE(platform_unsupported, NULL, NULL, NULL, NULL, NULL);

/* Catches a regression where unavailable rwlock operations report success. */
ZTEST(platform_unsupported, test_rwlock_unsupported_operations_report_error)
{
    korp_rwlock lock;

    zassert_equal(os_rwlock_init(NULL), BHT_ERROR, "null rwlock accepted");
    zassert_equal(os_rwlock_init(&lock), BHT_OK, "rwlock init failed");
    zassert_equal(os_rwlock_rdlock(&lock), BHT_ERROR,
                  "unsupported read lock succeeded");
    zassert_equal(os_rwlock_destroy(&lock), BHT_ERROR,
                  "unsupported destroy succeeded");
}

/* Catches a regression where unavailable named semaphore operations succeed. */
ZTEST(platform_unsupported, test_named_semaphore_operations_report_error)
{
    zassert_is_null(os_sem_open("wamr", 0, 0, 1),
                    "unsupported semaphore open succeeded");
    zassert_equal(os_sem_close(NULL), BHT_ERROR,
                  "unsupported semaphore close succeeded");
    zassert_equal(os_sem_wait(NULL), BHT_ERROR,
                  "unsupported semaphore wait succeeded");
    zassert_equal(os_sem_trywait(NULL), BHT_ERROR,
                  "unsupported semaphore trywait succeeded");
    zassert_equal(os_sem_post(NULL), BHT_ERROR,
                  "unsupported semaphore post succeeded");
    zassert_equal(os_sem_getvalue(NULL, NULL), BHT_ERROR,
                  "unsupported semaphore getvalue succeeded");
    zassert_equal(os_sem_unlink("wamr"), BHT_ERROR,
                  "unsupported semaphore unlink succeeded");
}

/* Catches a regression where unavailable blocking operations report success. */
ZTEST(platform_unsupported, test_blocking_operations_report_error_and_are_safe)
{
    zassert_equal(os_blocking_op_init(), BHT_ERROR,
                  "unsupported blocking operation init succeeded");
    zassert_equal(os_wakeup_blocking_op(NULL), BHT_ERROR,
                  "unsupported blocking operation wakeup succeeded");
    os_begin_blocking_op();
    os_end_blocking_op();
}
