/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "runtime_fixture.h"
#include "wasm_fixtures.h"
#include "zephyr_sync_pool.h"
#include "zephyr_thread_pool.h"

#define USER_WORKER_STACK_SIZE 8192U
#define USER_WORKER_PRIORITY 5
#define USER_WORKER_TIMEOUT K_SECONDS(3)
#define SMALL_POOL_SIZE 1024U
#define FIXTURE_ACCESS_TOKEN 0x57414d52U
#define RUNTIME_RESTART_ITERATIONS 8U
#define RUNTIME_RESTART_GUARD_MS 1000U
#define RUNTIME_WORKER_WAIT_MS 500U
#define RUNTIME_FORCED_WORKER_WAIT_MS 2000U
#define RUNTIME_TIMEOUT_US 10000U
#define RUNTIME_WAMR_WORKER_STACK_SIZE 2048U

enum runtime_restart_phase {
    RUNTIME_RESTART_IDLE,
    RUNTIME_RESTART_INITIALIZED,
    RUNTIME_RESTART_WORKER_WAITING,
    RUNTIME_RESTART_WORKER_RELEASED,
    RUNTIME_RESTART_WORKER_FINISHED,
    RUNTIME_RESTART_CLEANED,
};

struct runtime_restart_state {
    korp_tid worker;
    void *worker_return_value;
    korp_mutex mutex;
    korp_cond cond;
    atomic_t phase;
    uint32_t iteration;
    int thread_create_result;
    int thread_join_result;
    int ready_wait_result;
    int done_wait_result;
    int recovery_lock_result;
    int recovery_signal_result;
    int recovery_unlock_result;
    int exit_wait_result;
    int worker_lock_result;
    int worker_wait_result;
    int worker_unlock_result;
    int parent_lock_result;
    int parent_signal_result;
    int parent_unlock_result;
    int timeout_result;
    int timeout_unlock_result;
    int reacquire_lock_result;
    int reacquire_unlock_result;
    int cond_destroy_result;
    int mutex_destroy_result;
    int teardown_command_result;
    int root_join_result;
    bool runtime_owned;
    bool worker_owned;
    bool mutex_owned;
    bool cond_owned;
    bool force_done_guard_timeout;
    bool recovery_attempted;
    bool guard_expired;
    bool poisoned;
    bool cleanup_complete;
    bool workflow_succeeded;
};

struct runtime_user_mode_fixture {
    k_thread_entry_t worker_entry;
    bool worker_was_user;
    bool workflow_succeeded;
    bool negative_observed;
    bool diagnostic_present;
    bool small_pool_initialized;
    bool recovery_succeeded;
    uint32_t result;
    uint32_t run_count;
    uint32_t access_token;
    struct runtime_restart_state restart;
    char diagnostic[WAMR_TEST_ERROR_SIZE];
};

extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition ztest_mem_partition;

K_APPMEM_PARTITION_DEFINE(wamr_partition);

static struct k_mem_domain wamr_domain;
static struct k_thread runtime_worker;
K_THREAD_STACK_DEFINE(runtime_worker_stack, USER_WORKER_STACK_SIZE);
static k_tid_t runtime_worker_tid;
static struct k_sem worker_command;
static struct k_sem worker_done;
static struct k_sem runtime_child_ready;
static struct k_sem runtime_child_done;
WAMR_ZEPHYR_SYNC_POOL_DEFINE(runtime_sync, 16, 8);
WAMR_ZEPHYR_THREAD_POOL_DEFINE(runtime_threads, 4,
                               RUNTIME_WAMR_WORKER_STACK_SIZE);

K_APP_BMEM(wamr_partition) static struct loaded_runtime user_runtime;
K_APP_BMEM(wamr_partition)
static uint8_t small_pool[SMALL_POOL_SIZE] __aligned(8);
ZTEST_DMEM static struct runtime_user_mode_fixture user_results = { 0 };

static void
complete_worker(struct runtime_user_mode_fixture *results);

int
wamr_zephyr_thread_test_wait(korp_tid handle, k_timeout_t timeout);
struct k_mutex *
wamr_zephyr_sync_test_native_mutex(korp_mutex handle);

static int
prepare_runtime_thread_pool(k_tid_t owner)
{
    return wamr_zephyr_thread_pool_prepare(&runtime_threads, owner);
}

static bool
reset_restart_state(struct runtime_restart_state *state, uint32_t iteration)
{
    if (state->runtime_owned || state->worker_owned || state->mutex_owned
        || state->cond_owned || state->poisoned) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->iteration = iteration;
    state->thread_create_result = BHT_ERROR;
    state->thread_join_result = BHT_ERROR;
    state->ready_wait_result = -EAGAIN;
    state->done_wait_result = -EAGAIN;
    state->recovery_lock_result = BHT_ERROR;
    state->recovery_signal_result = BHT_ERROR;
    state->recovery_unlock_result = BHT_ERROR;
    state->exit_wait_result = BHT_ERROR;
    state->worker_lock_result = BHT_ERROR;
    state->worker_wait_result = BHT_ERROR;
    state->worker_unlock_result = BHT_ERROR;
    state->parent_lock_result = BHT_ERROR;
    state->parent_signal_result = BHT_ERROR;
    state->parent_unlock_result = BHT_ERROR;
    state->timeout_result = BHT_ERROR;
    state->timeout_unlock_result = BHT_ERROR;
    state->reacquire_lock_result = BHT_ERROR;
    state->reacquire_unlock_result = BHT_ERROR;
    state->cond_destroy_result = BHT_ERROR;
    state->mutex_destroy_result = BHT_ERROR;
    state->teardown_command_result = -EAGAIN;
    state->root_join_result = -EAGAIN;
    return true;
}

static void *
runtime_restart_child(void *arg)
{
    struct runtime_restart_state *state = arg;
    int64_t deadline = k_uptime_get()
                       + (state->force_done_guard_timeout
                              ? RUNTIME_FORCED_WORKER_WAIT_MS
                              : RUNTIME_WORKER_WAIT_MS);

    state->worker_lock_result = os_mutex_lock(&state->mutex);
    if (state->worker_lock_result == BHT_OK) {
        atomic_set(&state->phase, RUNTIME_RESTART_WORKER_WAITING);
        k_sem_give(&runtime_child_ready);
        do {
            int64_t remaining_ms = deadline - k_uptime_get();

            if (remaining_ms <= 0) {
                state->worker_wait_result = ETIMEDOUT;
                break;
            }
            state->worker_wait_result = os_cond_reltimedwait(
                &state->cond, &state->mutex,
                (uint64)remaining_ms * 1000U);
        } while (state->worker_wait_result == BHT_OK
                 && atomic_get(&state->phase)
                        != RUNTIME_RESTART_WORKER_RELEASED);
        state->worker_unlock_result = os_mutex_unlock(&state->mutex);
    }
    atomic_set(&state->phase, RUNTIME_RESTART_WORKER_FINISHED);
    k_sem_give(&runtime_child_done);
    return state;
}

static void
release_runtime_restart_child(struct runtime_restart_state *state)
{
    state->parent_lock_result = os_mutex_lock(&state->mutex);
    if (state->parent_lock_result != BHT_OK) {
        return;
    }
    atomic_set(&state->phase, RUNTIME_RESTART_WORKER_RELEASED);
    state->parent_signal_result = os_cond_signal(&state->cond);
    state->parent_unlock_result = os_mutex_unlock(&state->mutex);
}

static void
recover_runtime_restart_child(struct runtime_restart_state *state)
{
    struct k_mutex *native_mutex;

    if (state->recovery_attempted || !state->mutex_owned
        || !state->cond_owned
        || atomic_get(&state->phase) != RUNTIME_RESTART_WORKER_WAITING) {
        return;
    }

    state->recovery_attempted = true;
    native_mutex = wamr_zephyr_sync_test_native_mutex(state->mutex);
    if (native_mutex == NULL) {
        return;
    }
    state->recovery_lock_result = k_mutex_lock(
        native_mutex, K_MSEC(RUNTIME_RESTART_GUARD_MS));
    if (state->recovery_lock_result != 0) {
        return;
    }
    atomic_set(&state->phase, RUNTIME_RESTART_WORKER_RELEASED);
    state->recovery_signal_result = os_cond_broadcast(&state->cond);
    state->recovery_unlock_result = k_mutex_unlock(native_mutex);
}

static void
cleanup_runtime_restart(struct runtime_restart_state *state)
{
    if (state->worker_owned) {
        state->done_wait_result = k_sem_take(
            &runtime_child_done, K_MSEC(RUNTIME_RESTART_GUARD_MS));
        if (state->done_wait_result != 0) {
            state->guard_expired = true;
            recover_runtime_restart_child(state);
        }
        state->exit_wait_result = wamr_zephyr_thread_test_wait(
            state->worker, K_MSEC(RUNTIME_RESTART_GUARD_MS));
        if (state->exit_wait_result == 0) {
            state->thread_join_result =
                os_thread_join(state->worker, &state->worker_return_value);
            state->worker_owned = state->thread_join_result != BHT_OK;
        }
        if (state->worker_owned) {
            state->poisoned = true;
            return;
        }
    }
    if (state->cond_owned) {
        state->cond_destroy_result = os_cond_destroy(&state->cond);
        state->cond_owned = state->cond_destroy_result != BHT_OK;
    }
    if (state->mutex_owned) {
        state->mutex_destroy_result = os_mutex_destroy(&state->mutex);
        state->mutex_owned = state->mutex_destroy_result != BHT_OK;
    }
    if (!state->cond_owned && !state->mutex_owned && state->runtime_owned) {
        wamr_test_runtime_stop(&user_runtime.runtime);
        state->runtime_owned = false;
    }
    state->cleanup_complete = !state->runtime_owned && !state->worker_owned
                              && !state->mutex_owned && !state->cond_owned;
    if (state->cleanup_complete) {
        state->poisoned = false;
        atomic_set(&state->phase, RUNTIME_RESTART_CLEANED);
    }
}

static void
runtime_worker_restart_workflow(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    struct runtime_restart_state *state = &results->restart;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    state->runtime_owned = wamr_test_runtime_init(
        &user_runtime.runtime, user_runtime.pool, sizeof(user_runtime.pool));
    if (state->runtime_owned) {
        atomic_set(&state->phase, RUNTIME_RESTART_INITIALIZED);
        state->mutex_owned = os_mutex_init(&state->mutex) == BHT_OK;
        state->cond_owned = os_cond_init(&state->cond) == BHT_OK;
    }
    if (state->mutex_owned && state->cond_owned) {
        state->thread_create_result = os_thread_create(
            &state->worker, runtime_restart_child, state,
            RUNTIME_WAMR_WORKER_STACK_SIZE);
        state->worker_owned = state->thread_create_result == BHT_OK;
    }
    if (state->worker_owned) {
        state->ready_wait_result = k_sem_take(
            &runtime_child_ready, K_MSEC(RUNTIME_RESTART_GUARD_MS));
        if (state->ready_wait_result == 0
            && atomic_get(&state->phase)
                   == RUNTIME_RESTART_WORKER_WAITING) {
            if (!state->force_done_guard_timeout) {
                release_runtime_restart_child(state);
            }
        }
        else {
            state->guard_expired = true;
        }
    }
    cleanup_runtime_restart(state);
    state->workflow_succeeded =
        state->thread_create_result == BHT_OK
        && state->thread_join_result == BHT_OK
        && state->ready_wait_result == 0 && state->done_wait_result == 0
        && state->exit_wait_result == 0
        && state->worker_return_value == state
        && state->worker_lock_result == BHT_OK
        && state->worker_wait_result == BHT_OK
        && state->worker_unlock_result == BHT_OK
        && state->parent_lock_result == BHT_OK
        && state->parent_signal_result == BHT_OK
        && state->parent_unlock_result == BHT_OK
        && state->cond_destroy_result == BHT_OK
        && state->mutex_destroy_result == BHT_OK && !state->guard_expired
        && state->cleanup_complete;
    complete_worker(results);
}

static void
runtime_timeout_restart_workflow(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    struct runtime_restart_state *state = &results->restart;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    state->runtime_owned = wamr_test_runtime_init(
        &user_runtime.runtime, user_runtime.pool, sizeof(user_runtime.pool));
    if (state->runtime_owned) {
        atomic_set(&state->phase, RUNTIME_RESTART_INITIALIZED);
        state->mutex_owned = os_mutex_init(&state->mutex) == BHT_OK;
        state->cond_owned = os_cond_init(&state->cond) == BHT_OK;
    }
    if (state->mutex_owned && state->cond_owned) {
        state->parent_lock_result = os_mutex_lock(&state->mutex);
        if (state->parent_lock_result == BHT_OK) {
            state->timeout_result = os_cond_reltimedwait(
                &state->cond, &state->mutex, RUNTIME_TIMEOUT_US);
            state->timeout_unlock_result = os_mutex_unlock(&state->mutex);
            state->reacquire_lock_result = os_mutex_lock(&state->mutex);
            if (state->reacquire_lock_result == BHT_OK) {
                state->reacquire_unlock_result =
                    os_mutex_unlock(&state->mutex);
            }
        }
    }
    cleanup_runtime_restart(state);
    state->workflow_succeeded = state->timeout_result == ETIMEDOUT
                                && state->timeout_unlock_result == BHT_OK
                                && state->reacquire_lock_result == BHT_OK
                                && state->reacquire_unlock_result == BHT_OK
                                && state->cond_destroy_result == BHT_OK
                                && state->mutex_destroy_result == BHT_OK
                                && state->cleanup_complete;
    complete_worker(results);
}

static void
runtime_restart_teardown_workflow(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    cleanup_runtime_restart(&results->restart);
    complete_worker(results);
}

static bool
restart_state_has_ownership(const struct runtime_restart_state *state)
{
    return state->runtime_owned || state->worker_owned || state->mutex_owned
           || state->cond_owned || state->poisoned;
}

static bool
run_add_lifecycle(uint32_t *result)
{
    char error[WAMR_TEST_ERROR_SIZE] = { 0 };

    return wamr_test_run_add_copy(
        &user_runtime.runtime, user_runtime.pool, sizeof(user_runtime.pool),
        wasm_add, sizeof(wasm_add), result, error, sizeof(error));
}

static void
complete_worker(struct runtime_user_mode_fixture *results)
{
    results->worker_was_user = k_is_user_context();
    k_sem_give(&worker_done);
}

static void
valid_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    results->workflow_succeeded = run_add_lifecycle(&results->result);
    complete_worker(results);
}

static void
valid_recovery_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    uint32_t result = 0U;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    results->recovery_succeeded = run_add_lifecycle(&result) && result == 42U;
    complete_worker(results);
}

static void
twice_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    uint32_t result = 0U;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (run_add_lifecycle(&result) && result == 42U) {
        results->run_count++;
    }
    result = 0U;
    if (run_add_lifecycle(&result) && result == 42U) {
        results->run_count++;
    }
    results->workflow_succeeded = results->run_count == 2U;
    complete_worker(results);
}

static void
fixture_access_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    results->access_token = FIXTURE_ACCESS_TOKEN;
    complete_worker(results);
}

static void
malformed_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    char error[WAMR_TEST_ERROR_SIZE] = { 0 };
    bool started = wamr_test_runtime_start_copy(
        &user_runtime.runtime, user_runtime.pool, sizeof(user_runtime.pool),
        malformed_wasm, sizeof(malformed_wasm), error, sizeof(error));

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (started) {
        wamr_test_runtime_stop(&user_runtime.runtime);
    }
    results->negative_observed = !started;
    results->diagnostic_present = error[0] != '\0';
    memcpy(results->diagnostic, error, sizeof(results->diagnostic));
    complete_worker(results);
}

static void
small_pool_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    struct wamr_test_runtime small_runtime = { 0 };
    char error[WAMR_TEST_ERROR_SIZE] = { 0 };
    bool started;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    started = wamr_test_runtime_start_copy(
        &small_runtime, small_pool, sizeof(small_pool), wasm_add,
        sizeof(wasm_add), error, sizeof(error));
    results->negative_observed = !started;
    results->small_pool_initialized = !started && error[0] != '\0';
    if (started) {
        wamr_test_runtime_stop(&small_runtime);
    }
    memcpy(results->diagnostic, error, sizeof(results->diagnostic));
    results->diagnostic_present = error[0] != '\0';
    complete_worker(results);
}

static void
missing_export_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;
    char error[WAMR_TEST_ERROR_SIZE] = { 0 };
    wasm_function_inst_t missing = NULL;
    bool exception_set = false;
    bool started = wamr_test_runtime_start_copy(
        &user_runtime.runtime, user_runtime.pool, sizeof(user_runtime.pool),
        wasm_add, sizeof(wasm_add), error, sizeof(error));

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (started) {
        missing = wasm_runtime_lookup_function(user_runtime.runtime.instance,
                                               "missing");
        exception_set =
            wasm_runtime_get_exception(user_runtime.runtime.instance) != NULL;
        wamr_test_runtime_stop(&user_runtime.runtime);
    }
    results->negative_observed = started && missing == NULL && !exception_set;
    memcpy(results->diagnostic, error, sizeof(results->diagnostic));
    results->diagnostic_present = error[0] != '\0';
    complete_worker(results);
}

static void
runtime_user_worker(void *arg1, void *arg2, void *arg3)
{
    struct runtime_user_mode_fixture *results = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    while (true) {
        k_sem_take(&worker_command, K_FOREVER);
        if (results->worker_entry == NULL) {
            return;
        }
        results->worker_entry(results, NULL, NULL);
    }
}

static void
run_user_worker(struct runtime_user_mode_fixture *results,
                k_thread_entry_t entry)
{
    int completion_result;

    results->worker_entry = entry;
    k_sem_give(&worker_command);
    completion_result = k_sem_take(&worker_done, USER_WORKER_TIMEOUT);

    zassert_equal(completion_result, 0,
                  "user worker did not signal completion");
    zassert_true(results->worker_was_user,
                 "runtime worker did not execute in user mode");
}

static void *
runtime_user_mode_setup(void)
{
    struct k_mem_partition *partitions[] = {
        &wamr_partition,
        &z_libc_partition,
        &ztest_mem_partition,
    };

    zassert_equal(
        k_mem_domain_init(&wamr_domain, ARRAY_SIZE(partitions), partitions), 0,
        "WAMR memory domain initialization failed");
    k_sem_init(&worker_command, 0, 1);
    k_sem_init(&worker_done, 0, 1);
    k_sem_init(&runtime_child_ready, 0, 1);
    k_sem_init(&runtime_child_done, 0, 1);
    runtime_worker_tid = k_thread_create(
        &runtime_worker, runtime_worker_stack,
        K_THREAD_STACK_SIZEOF(runtime_worker_stack), runtime_user_worker,
        &user_results, NULL, NULL, USER_WORKER_PRIORITY, K_USER, K_FOREVER);
    zassert_not_null(runtime_worker_tid, "user worker creation failed");
    zassert_equal(k_mem_domain_add_thread(&wamr_domain, runtime_worker_tid), 0,
                  "adding user worker to domain failed");
    zassert_equal(
        wamr_zephyr_sync_pool_prepare(&runtime_sync, runtime_worker_tid), 0,
        "WAMR sync pool preparation failed");
    zassert_equal(prepare_runtime_thread_pool(runtime_worker_tid), BHT_OK,
                  "WAMR thread pool preparation failed");
    k_object_access_grant(&worker_command, runtime_worker_tid);
    k_object_access_grant(&worker_done, runtime_worker_tid);
    k_object_access_grant(&runtime_child_ready, runtime_worker_tid);
    k_object_access_grant(&runtime_child_done, runtime_worker_tid);
    k_thread_start(runtime_worker_tid);
    return &user_results;
}

static void
runtime_user_mode_before(void *fixture)
{
    struct runtime_user_mode_fixture *results = fixture;

    if (restart_state_has_ownership(&results->restart)) {
        zassert_unreachable("previous runtime restart retained ownership");
        return;
    }
    memset(fixture, 0, sizeof(struct runtime_user_mode_fixture));
}

static void
runtime_user_mode_teardown(void *fixture)
{
    struct runtime_user_mode_fixture *results = fixture;
    bool cleanup_required = restart_state_has_ownership(&results->restart);

    if (cleanup_required) {
        results->worker_entry = runtime_restart_teardown_workflow;
        k_sem_give(&worker_command);
        results->restart.teardown_command_result =
            k_sem_take(&worker_done, USER_WORKER_TIMEOUT);
    }

    results->worker_entry = NULL;
    k_sem_give(&worker_command);
    results->restart.root_join_result =
        k_thread_join(runtime_worker_tid, USER_WORKER_TIMEOUT);
    if (results->restart.root_join_result != 0) {
        k_thread_abort(runtime_worker_tid);
    }
    if (cleanup_required) {
        zassert_equal(results->restart.teardown_command_result, 0,
                      "runtime cleanup command exceeded its bound");
        zassert_false(restart_state_has_ownership(&results->restart),
                      "runtime teardown retained live child ownership");
    }
    zassert_equal(results->restart.root_join_result, 0,
                  "user worker did not join within the bound");
}

ZTEST_SUITE(runtime_user_mode, NULL, runtime_user_mode_setup,
            runtime_user_mode_before, NULL, runtime_user_mode_teardown);

ZTEST_F(runtime_user_mode, test_valid_module_workflow)
{
    run_user_worker(fixture, valid_worker);
    zassert_true(fixture->workflow_succeeded,
                 "valid user-mode workflow failed");
    zassert_equal(fixture->result, 42U, "valid user-mode workflow returned %u",
                  fixture->result);
}

ZTEST_F(runtime_user_mode, test_workflow_runs_twice)
{
    run_user_worker(fixture, twice_worker);
    zassert_true(fixture->workflow_succeeded,
                 "repeated user-mode workflow failed");
    zassert_equal(fixture->run_count, 2U,
                  "user-mode workflow completed %u times", fixture->run_count);
}

ZTEST_F(runtime_user_mode, test_fixture_memory_is_accessible)
{
    run_user_worker(fixture, fixture_access_worker);
    zassert_equal(fixture->access_token, FIXTURE_ACCESS_TOKEN,
                  "user worker could not update shared fixture memory");
}

ZTEST_F(runtime_user_mode, test_malformed_module_is_rejected)
{
    run_user_worker(fixture, malformed_worker);
    run_user_worker(fixture, valid_recovery_worker);
    zassert_true(fixture->negative_observed,
                 "malformed module was accepted in user mode");
    zassert_true(fixture->diagnostic_present,
                 "malformed module had no diagnostic");
    zassert_true(fixture->recovery_succeeded,
                 "valid workflow failed after malformed module");
}

ZTEST_F(runtime_user_mode, test_small_pool_fails_cleanly)
{
    run_user_worker(fixture, small_pool_worker);
    run_user_worker(fixture, valid_recovery_worker);
    zassert_true(fixture->small_pool_initialized,
                 "small pool initialization failed early");
    zassert_true(fixture->negative_observed,
                 "small pool unexpectedly instantiated module");
    zassert_true(fixture->recovery_succeeded,
                 "valid workflow failed after small pool rejection");
}

ZTEST_F(runtime_user_mode, test_missing_export_then_valid_workflow)
{
    run_user_worker(fixture, missing_export_worker);
    run_user_worker(fixture, valid_recovery_worker);
    zassert_true(fixture->negative_observed,
                 "missing export lookup did not fail cleanly");
    zassert_true(fixture->recovery_succeeded,
                 "valid workflow failed after missing export lookup");
}

ZTEST_F(runtime_user_mode, test_runtime_worker_restart_reuses_registered_pools)
{
    for (uint32_t iteration = 0U; iteration < RUNTIME_RESTART_ITERATIONS;
         iteration++) {
        zassert_true(reset_restart_state(&fixture->restart, iteration),
                     "iteration %u reused owned restart state", iteration);
        k_sem_reset(&runtime_child_ready);
        k_sem_reset(&runtime_child_done);
        zassert_equal(prepare_runtime_thread_pool(runtime_worker_tid), BHT_OK,
                      "iteration %u thread pool preparation failed",
                      iteration);
        run_user_worker(fixture, runtime_worker_restart_workflow);

        zassert_true(
            fixture->restart.workflow_succeeded,
            "iteration %u restart failed: create %d join %d return %p "
            "wait %d signal %d destroy %d/%d phase %d guards %d/%d cleanup %d",
            iteration, fixture->restart.thread_create_result,
            fixture->restart.thread_join_result,
            fixture->restart.worker_return_value,
            fixture->restart.worker_wait_result,
            fixture->restart.parent_signal_result,
            fixture->restart.cond_destroy_result,
            fixture->restart.mutex_destroy_result,
            atomic_get(&fixture->restart.phase),
            fixture->restart.ready_wait_result,
            fixture->restart.done_wait_result,
            fixture->restart.cleanup_complete);
        zassert_equal(atomic_get(&fixture->restart.phase),
                      RUNTIME_RESTART_CLEANED,
                      "iteration %u did not reach bounded cleanup", iteration);
        zassert_false(fixture->restart.guard_expired,
                      "iteration %u exceeded a phase guard", iteration);
    }
}

ZTEST_F(runtime_user_mode,
        test_runtime_timeout_restart_reacquires_mutex_each_cycle)
{
    for (uint32_t iteration = 0U; iteration < RUNTIME_RESTART_ITERATIONS;
         iteration++) {
        zassert_true(reset_restart_state(&fixture->restart, iteration),
                     "iteration %u reused owned restart state", iteration);
        k_sem_reset(&runtime_child_ready);
        k_sem_reset(&runtime_child_done);
        zassert_equal(prepare_runtime_thread_pool(runtime_worker_tid), BHT_OK,
                      "iteration %u thread pool preparation failed",
                      iteration);
        run_user_worker(fixture, runtime_timeout_restart_workflow);

        zassert_true(
            fixture->restart.workflow_succeeded,
            "iteration %u timeout restart failed: timeout %d unlock %d "
            "relock %d/%d destroy %d/%d phase %d cleanup %d",
            iteration, fixture->restart.timeout_result,
            fixture->restart.timeout_unlock_result,
            fixture->restart.reacquire_lock_result,
            fixture->restart.reacquire_unlock_result,
            fixture->restart.cond_destroy_result,
            fixture->restart.mutex_destroy_result,
            atomic_get(&fixture->restart.phase),
            fixture->restart.cleanup_complete);
        zassert_equal(fixture->restart.timeout_result, ETIMEDOUT,
                      "iteration %u did not observe the relative timeout",
                      iteration);
        zassert_equal(atomic_get(&fixture->restart.phase),
                      RUNTIME_RESTART_CLEANED,
                      "iteration %u did not reach bounded cleanup", iteration);
    }
}

ZTEST_F(runtime_user_mode,
        test_runtime_done_guard_recovers_before_reusing_fixture)
{
    zassert_true(reset_restart_state(&fixture->restart, 0U),
                 "recovery test reused owned restart state");
    fixture->restart.force_done_guard_timeout = true;
    k_sem_reset(&runtime_child_ready);
    k_sem_reset(&runtime_child_done);
    zassert_equal(prepare_runtime_thread_pool(runtime_worker_tid), BHT_OK,
                  "recovery thread pool preparation failed");
    run_user_worker(fixture, runtime_worker_restart_workflow);

    if (!fixture->restart.cleanup_complete) {
        (void)wamr_zephyr_thread_test_wait(
            fixture->restart.worker,
            K_MSEC(RUNTIME_FORCED_WORKER_WAIT_MS));
    }
    zassert_equal(fixture->restart.ready_wait_result, 0,
                  "ready guard failed: result %d phase %d",
                  fixture->restart.ready_wait_result,
                  atomic_get(&fixture->restart.phase));
    zassert_not_equal(fixture->restart.done_wait_result, 0,
                      "done guard did not expire: result %d phase %d",
                      fixture->restart.done_wait_result,
                      atomic_get(&fixture->restart.phase));
    zassert_true(fixture->restart.guard_expired,
                 "done guard expiry was not retained");
    zassert_true(fixture->restart.recovery_attempted,
                 "done guard expiry did not attempt recovery");
    zassert_equal(fixture->restart.recovery_lock_result, BHT_OK,
                  "recovery mutex lock failed");
    zassert_equal(fixture->restart.recovery_signal_result, BHT_OK,
                  "recovery condition broadcast failed");
    zassert_equal(fixture->restart.recovery_unlock_result, BHT_OK,
                  "recovery mutex unlock failed");
    zassert_equal(fixture->restart.exit_wait_result, 0,
                  "worker termination was not proven");
    zassert_equal(fixture->restart.thread_join_result, BHT_OK,
                  "recovered worker was not joined");
    zassert_false(fixture->restart.worker_owned,
                  "joined worker ownership was retained");
    zassert_true(fixture->restart.cleanup_complete,
                 "recovered worker state was not cleaned");
}
