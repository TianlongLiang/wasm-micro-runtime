/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <errno.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "wasm_export.h"
#include "zephyr_thread_pool.h"

#define MOCK_HEAP_SIZE 131072U
#define MOCK_THREAD_STACK_SIZE 2048U
#define MOCK_THREAD_CAPACITY BH_ZEPHYR_MPU_STACK_COUNT

DEFINE_FFF_GLOBALS;
FAKE_VALUE_FUNC(void *, wamr_zephyr_thread_test_malloc, unsigned int);
FAKE_VOID_FUNC(wamr_zephyr_thread_test_free, void *);
FAKE_VALUE_FUNC(int, wamr_zephyr_thread_test_join, k_tid_t, k_timeout_t);

ZTEST_BMEM static uint8_t mock_heap[MOCK_HEAP_SIZE] __aligned(8);

struct mocked_error_fixture {
    unsigned int fail_alloc_at;
    unsigned int alloc_attempt;
    bool fail_next_join;
    bool runtime_initialized;
    k_tid_t native_tid;
    struct k_sem ready;
    struct k_sem release;
    korp_tid owned[MOCK_THREAD_CAPACITY];
};

static struct mocked_error_fixture mock_fixture;

static void *
allocate_or_fail(unsigned int size)
{
    mock_fixture.alloc_attempt++;
    if (mock_fixture.fail_alloc_at == mock_fixture.alloc_attempt) {
        return NULL;
    }
    return os_malloc(size);
}

static void
free_real(void *ptr)
{
    os_free(ptr);
}

static void *
return_argument(void *arg)
{
    mock_fixture.native_tid = k_current_get();
    k_sem_give(&mock_fixture.ready);
    return arg;
}

static int
join_or_fail_once(k_tid_t thread, k_timeout_t timeout)
{
    if (mock_fixture.fail_next_join) {
        mock_fixture.fail_next_join = false;
        return -EDEADLK;
    }
    return k_thread_join(thread, timeout);
}

static void
mocked_error_before(void *fixture)
{
    RuntimeInitArgs args = { 0 };

    ARG_UNUSED(fixture);
    zassert_false(mock_fixture.runtime_initialized,
                  "previous cleanup failed; refusing to reset the owned runtime");
    memset(&mock_fixture, 0, sizeof(mock_fixture));
    RESET_FAKE(wamr_zephyr_thread_test_malloc);
    RESET_FAKE(wamr_zephyr_thread_test_free);
    RESET_FAKE(wamr_zephyr_thread_test_join);
    FFF_RESET_HISTORY();
    wamr_zephyr_thread_test_malloc_fake.custom_fake = allocate_or_fail;
    wamr_zephyr_thread_test_free_fake.custom_fake = free_real;
    wamr_zephyr_thread_test_join_fake.custom_fake = join_or_fail_once;
    k_sem_init(&mock_fixture.ready, 0, MOCK_THREAD_CAPACITY);
    k_sem_init(&mock_fixture.release, 0, MOCK_THREAD_CAPACITY);
    memset(mock_heap, 0xA5, sizeof(mock_heap));
    args.mem_alloc_type = Alloc_With_Pool;
    args.mem_alloc_option.pool.heap_buf = mock_heap;
    args.mem_alloc_option.pool.heap_size = sizeof(mock_heap);
    zassert_true(wasm_runtime_full_init(&args),
                 "runtime initialization failed");
    mock_fixture.runtime_initialized = true;
}

static void
own_for_cleanup(korp_tid thread)
{
    for (size_t i = 0; i < ARRAY_SIZE(mock_fixture.owned); i++) {
        if (mock_fixture.owned[i] == NULL) {
            mock_fixture.owned[i] = thread;
            return;
        }
    }
    zassert_unreachable("mock cleanup ownership exhausted");
}

static void
disown_after_join(korp_tid thread)
{
    for (size_t i = 0; i < ARRAY_SIZE(mock_fixture.owned); i++) {
        if (mock_fixture.owned[i] == thread) {
            mock_fixture.owned[i] = NULL;
            return;
        }
    }
    zassert_unreachable("joined thread was not owned by fixture");
}

static void
mocked_error_after(void *fixture)
{
    ARG_UNUSED(fixture);
    mock_fixture.fail_next_join = false;
    wamr_zephyr_thread_test_join_fake.custom_fake = join_or_fail_once;
    mock_fixture.fail_alloc_at = 0U;
    for (size_t i = 0; i < MOCK_THREAD_CAPACITY; i++) {
        k_sem_give(&mock_fixture.release);
    }
    for (size_t i = 0; i < ARRAY_SIZE(mock_fixture.owned); i++) {
        if (mock_fixture.owned[i] != NULL) {
            if (os_thread_join(mock_fixture.owned[i], NULL) != BHT_OK) {
                /* An after-hook failure stops Ztest's suite. Keep ownership
                 * and the runtime intact when public cleanup is impossible. */
                zassert_unreachable(
                    "cleanup join failed; preserving owned handle and runtime");
                return;
            }
            mock_fixture.owned[i] = NULL;
        }
    }
    if (mock_fixture.runtime_initialized) {
        wasm_runtime_destroy();
        mock_fixture.runtime_initialized = false;
    }
}

static void *
capacity_worker(void *arg)
{
    struct mocked_error_fixture *fixture = arg;

    k_sem_give(&fixture->ready);
    (void)k_sem_take(&fixture->release, K_FOREVER);
    return NULL;
}

static void
assert_full_capacity_recovery(void)
{
    for (size_t i = 0; i < MOCK_THREAD_CAPACITY; i++) {
        zassert_equal(os_thread_create(&mock_fixture.owned[i], capacity_worker,
                                       &mock_fixture, MOCK_THREAD_STACK_SIZE),
                      BHT_OK, "recovery create %zu failed", i);
    }
    for (size_t i = 0; i < MOCK_THREAD_CAPACITY; i++) {
        zassert_equal(k_sem_take(&mock_fixture.ready, K_MSEC(500)), 0,
                      "recovery worker %zu was not ready", i);
    }
    for (size_t i = 0; i < MOCK_THREAD_CAPACITY; i++) {
        k_sem_give(&mock_fixture.release);
    }
    for (size_t i = 0; i < MOCK_THREAD_CAPACITY; i++) {
        zassert_equal(os_thread_join(mock_fixture.owned[i], NULL), BHT_OK,
                      "recovery join %zu failed", i);
        disown_after_join(mock_fixture.owned[i]);
    }
}

ZTEST(platform_mocked_errors,
      test_native_thread_object_allocation_failure_recovers)
{
    korp_tid thread = NULL;
    int result;

    mock_fixture.fail_alloc_at = 1U;
    result = os_thread_create(&thread, return_argument, NULL,
                              MOCK_THREAD_STACK_SIZE);
    if (result == BHT_OK) {
        own_for_cleanup(thread);
    }
    zassert_equal(result, BHT_ERROR, "first allocation failure was ignored");
    zassert_is_null(thread, "failed create published a handle");
    zassert_equal(wamr_zephyr_thread_test_malloc_fake.call_count, 1U, NULL);
    zassert_equal(wamr_zephyr_thread_test_free_fake.call_count, 0U, NULL);

    mock_fixture.fail_alloc_at = 0U;
    assert_full_capacity_recovery();
}

ZTEST(platform_mocked_errors,
      test_thread_metadata_allocation_failure_frees_object_and_recovers)
{
    korp_tid thread = NULL;
    int result;

    mock_fixture.fail_alloc_at = 2U;
    result = os_thread_create(&thread, return_argument, NULL,
                              MOCK_THREAD_STACK_SIZE);
    if (result == BHT_OK) {
        own_for_cleanup(thread);
    }
    zassert_equal(result, BHT_ERROR, "metadata allocation failure was ignored");
    zassert_is_null(thread, "failed create published a handle");
    zassert_equal(wamr_zephyr_thread_test_malloc_fake.call_count, 2U, NULL);
    zassert_equal(wamr_zephyr_thread_test_free_fake.call_count, 1U, NULL);
    zassert_not_null(
        wamr_zephyr_thread_test_malloc_fake.return_val_history[0], NULL);
    zassert_equal_ptr(
        wamr_zephyr_thread_test_free_fake.arg0_history[0],
        wamr_zephyr_thread_test_malloc_fake.return_val_history[0],
        "metadata failure did not free the allocated thread object");

    mock_fixture.fail_alloc_at = 0U;
    assert_full_capacity_recovery();
}

ZTEST(platform_mocked_errors, test_join_failure_releases_claim_for_retry)
{
    void *expected = (void *)0x2468U;
    void *actual = NULL;
    korp_tid thread = NULL;

    zassert_equal(os_thread_create(&thread, return_argument, expected,
                                   MOCK_THREAD_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    own_for_cleanup(thread);
    zassert_equal(k_sem_take(&mock_fixture.ready, K_MSEC(500)), 0,
                  "worker did not publish its native thread identity");
    mock_fixture.fail_next_join = true;

    zassert_equal(os_thread_join(thread, &actual), BHT_ERROR,
                  "injected join failure was ignored");
    zassert_is_null(actual, "failed join published a return value");
    zassert_equal(wamr_zephyr_thread_test_join_fake.call_count, 1U, NULL);
    zassert_equal(wamr_zephyr_thread_test_free_fake.call_count, 0U,
                  "failed join freed live metadata");

    zassert_equal(os_thread_join(thread, &actual), BHT_OK,
                  "join claim was not released for retry");
    disown_after_join(thread);
    zassert_equal_ptr(actual, expected, "retry lost the return value");
    zassert_equal(wamr_zephyr_thread_test_join_fake.call_count, 2U, NULL);
    zassert_equal(wamr_zephyr_thread_test_free_fake.call_count, 2U,
                  "successful retry did not free object and metadata");
    zassert_equal(wamr_zephyr_thread_test_malloc_fake.call_count, 2U, NULL);
    for (size_t i = 0; i < 2U; i++) {
        void *allocation =
            wamr_zephyr_thread_test_malloc_fake.return_val_history[i];
        unsigned int frees = 0U;

        zassert_not_null(allocation, "successful create allocation %zu", i);
        for (size_t j = 0; j < 2U; j++) {
            if (wamr_zephyr_thread_test_free_fake.arg0_history[j] == allocation) {
                frees++;
            }
        }
        zassert_equal(frees, 1U,
                      "successful retry must free allocation %zu exactly once", i);
        zassert_equal_ptr(wamr_zephyr_thread_test_join_fake.arg0_history[i],
                          mock_fixture.native_tid,
                          "join attempt %zu used the wrong native thread", i);
        zassert_true(K_TIMEOUT_EQ(
                         wamr_zephyr_thread_test_join_fake.arg1_history[i],
                         K_FOREVER),
                     "join attempt %zu did not wait forever", i);
    }
    assert_full_capacity_recovery();
}

ZTEST_SUITE(platform_mocked_errors, NULL, NULL, mocked_error_before,
            mocked_error_after, NULL);
