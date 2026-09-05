/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <errno.h>

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define WAMR_TEST_POOL_STORAGE ZTEST_BMEM
#include "test_common.h"
#undef WAMR_TEST_POOL_STORAGE

#include "platform_api_extension.h"
#include "platform_api_vmcore.h"
#include "zephyr_sync_pool.h"
#include "zephyr_thread_pool.h"

#if defined(CONFIG_USERSPACE)
struct korp_mutex_handle;
struct korp_cond_handle;
BUILD_ASSERT(!__builtin_types_compatible_p(korp_mutex, struct k_mutex *));
BUILD_ASSERT(!__builtin_types_compatible_p(korp_cond, struct k_condvar *));
BUILD_ASSERT(__builtin_types_compatible_p(korp_mutex,
                                          struct korp_mutex_handle *));
BUILD_ASSERT(__builtin_types_compatible_p(korp_cond,
                                          struct korp_cond_handle *));

struct k_mutex *
wamr_zephyr_sync_test_native_mutex(korp_mutex handle);
struct k_condvar *
wamr_zephyr_sync_test_native_condvar(korp_cond handle);
int
wamr_zephyr_sync_test_restore_waiting_cond(korp_cond handle,
                                           struct k_condvar *native);
int
os_thread_sys_init(void);
void
os_thread_sys_destroy(void);
#else
BUILD_ASSERT(__builtin_types_compatible_p(korp_cond, struct k_condvar));
#endif

#define WAMR_TEST_THREAD_STACK_SIZE 2048U
#define CONDITION_READY_TIMEOUT_MS 500
#define CONDITION_POLL_INTERVAL_MS 1
#define TEST_MUTEX_POOL_COUNT 8
#define TEST_COND_POOL_COUNT 4
#define SYNC_STRESS_WORKERS 4U
#define SYNC_STRESS_INCREMENTS 100U
#define SYNC_STRESS_LIGHTWEIGHT_ITERATIONS 16U
#define SYNC_STRESS_POOL_ITERATIONS 8U
#define SYNC_STRESS_COUNTER_CYCLES 8U
#define SYNC_STRESS_GUARD_MS 500U
#define SYNC_STRESS_WAIT_US ((uint64)SYNC_STRESS_GUARD_MS * 1000U)

enum {
    WAMR_SYNC_TEST_MUTEX_OPERATION_CLAIMED = 1,
};

struct mutex_counter {
    korp_mutex mutex;
    atomic_t started;
    atomic_t release;
    int value;
};

struct mutex_counter_worker {
    struct mutex_counter *counter;
    bool timed_out;
    int lock_result;
    int unlock_result;
};

struct stress_counter_worker {
    korp_mutex *mutex;
    int *counter;
    int release_result;
    int lock_result;
    int unlock_result;
};

struct stress_condition_waiter {
    korp_mutex *mutex;
    korp_cond *cond;
    atomic_t *generation;
    atomic_t *released;
    int expected_generation;
    int lock_result;
    int wait_result;
    int unlock_result;
    int wait_attempts;
    bool waiting;
    bool woke;
    bool timed_out;
    bool observed_signal;
};

struct condition_waiter {
    korp_mutex *mutex;
    korp_cond *cond;
    atomic_t ready;
    bool waiting;
    bool woke;
    int result;
    uint64 timeout_us;
};

struct mutex_operation_context {
    korp_mutex *mutex;
    int lock_result;
    int unlock_result;
};

struct concurrent_sync_context {
    korp_mutex mutex;
    korp_cond cond;
    atomic_t *initialized;
    atomic_t *release;
    int mutex_init_result;
    int cond_init_result;
    int mutex_destroy_result;
    int cond_destroy_result;
};

struct sync_claim_hook_state {
    atomic_t armed;
    atomic_t reached;
    atomic_t release;
    uintptr_t expected_handle;
    bool semaphore_mode;
    int release_result;
};

struct shutdown_recovery_result {
    korp_tid initial_identity;
    korp_tid refused_identity;
    korp_tid clean_identity;
    korp_tid recovered_identity;
    korp_tid final_identity;
    int selected_init_result;
    int selected_destroy_result;
    int reinit_result;
    int recovered_init_result;
    int recovered_destroy_result;
    bool recovered_handle_valid;
    bool cycle_results_valid;
    size_t failed_cycle;
};

struct platform_sync_fixture {
    struct mutex_counter counter;
    struct mutex_counter_worker counter_workers[2];
    korp_mutex stress_mutex;
    korp_cond stress_cond;
    int stress_counter;
    struct stress_counter_worker stress_counter_workers[SYNC_STRESS_WORKERS];
    struct stress_condition_waiter stress_condition_waiters[2];
    atomic_t stress_generation;
    atomic_t stress_released;
    korp_mutex condition_mutex;
    korp_cond condition_cond;
    korp_mutex claimed_mutex;
    struct mutex_operation_context claimed_operation;
    struct condition_waiter waiter;
    struct condition_waiter broadcast_waiters[2];
    struct concurrent_sync_context concurrent[TEST_COND_POOL_COUNT];
    atomic_t concurrent_initialized;
    atomic_t concurrent_release;
    bool runtime_destroyed;
    struct shutdown_recovery_result shutdown_recovery;
};

ZTEST_DMEM static struct platform_sync_fixture sync_results = { 0 };
ZTEST_DMEM static korp_mutex test_mutex = { 0 };
ZTEST_DMEM static struct sync_claim_hook_state sync_claim_hook;
static struct k_sem stress_counter_ready;
static struct k_sem stress_counter_release;
static struct k_sem stress_counter_done;
static struct k_sem stress_condition_ready;
static struct k_sem stress_condition_done;
static struct k_sem stress_mutex_claimed;
static struct k_sem stress_mutex_release;

int
wamr_zephyr_thread_test_wait(korp_tid handle, k_timeout_t timeout);

void
wamr_zephyr_sync_test_hook(int phase, uintptr_t handle)
{
    if (phase != WAMR_SYNC_TEST_MUTEX_OPERATION_CLAIMED
        || handle != sync_claim_hook.expected_handle
        || !atomic_cas(&sync_claim_hook.armed, 1, 0)) {
        return;
    }

    if (sync_claim_hook.semaphore_mode) {
        k_sem_give(&stress_mutex_claimed);
        sync_claim_hook.release_result =
            k_sem_take(&stress_mutex_release, K_MSEC(SYNC_STRESS_GUARD_MS));
        return;
    }

    atomic_set(&sync_claim_hook.reached, 1);
    for (int elapsed_ms = 0; elapsed_ms < CONDITION_READY_TIMEOUT_MS;
         elapsed_ms += 10) {
        if (atomic_get(&sync_claim_hook.release) != 0) {
            break;
        }
        k_sleep(K_MSEC(10));
    }
}

#if defined(CONFIG_WAMR_TEST_USER_MODE)
#define REJECTED_PREPARE_STACK_SIZE 2048U

WAMR_ZEPHYR_THREAD_POOL_DEFINE(rejected_prepare_threads, 1,
                                REJECTED_PREPARE_STACK_SIZE);
WAMR_ZEPHYR_SYNC_POOL_DEFINE(rejected_prepare_sync, 1, 1);

enum rejected_prepare_kind {
    REJECTED_THREAD_POOL_PREPARE,
    REJECTED_SYNC_POOL_PREPARE,
};

struct rejected_prepare_context {
    enum rejected_prepare_kind kind;
    k_tid_t owner;
    struct k_sem *done;
    int prepare_result;
    bool owner_access_completed;
};

struct sync_fault_state {
    atomic_t armed;
    atomic_t observed;
    k_tid_t expected_tid;
    struct k_sem *done;
};

struct missing_condvar_wait_context {
    struct k_mutex *mutex;
    struct k_condvar *condvar;
    int lock_result;
    int wait_result;
    bool reached_wait;
    bool returned;
};

struct prepared_sync_access_context {
    struct k_mutex *mutex;
    struct k_condvar *condvar;
    int lock_result;
    int unlock_result;
    int signal_result;
    bool completed;
};

ZTEST_DMEM static struct sync_fault_state sync_fault_state;
ZTEST_DMEM static struct missing_condvar_wait_context missing_condvar_wait_ctx;
ZTEST_DMEM static struct prepared_sync_access_context prepared_sync_access_ctx;
ZTEST_DMEM static struct prepared_sync_access_context unrelated_sync_access_ctx;
ZTEST_DMEM static struct rejected_prepare_context rejected_prepare_ctx;
ZTEST_DMEM static struct rejected_prepare_context identical_prepare_ctx;
ZTEST_DMEM static k_tid_t private_sync_pool_owner_tid;
static struct k_thread missing_condvar_thread;
static struct k_thread prepared_sync_access_thread;
static struct k_thread unrelated_sync_access_thread;
static struct k_mutex missing_condvar_mutex;
static struct k_condvar missing_condvar;
static struct k_sem missing_condvar_fault_done;
static struct k_sem unrelated_sync_access_fault_done;
static struct k_sem rejected_prepare_fault_done;
static struct k_sem identical_prepare_fault_done;
K_THREAD_STACK_DEFINE(missing_condvar_thread_stack,
                      WAMR_TEST_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(prepared_sync_access_thread_stack,
                      WAMR_TEST_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(unrelated_sync_access_thread_stack,
                      WAMR_TEST_THREAD_STACK_SIZE);

static void
sync_fault_arm(k_tid_t tid, struct k_sem *done)
{
    atomic_clear(&sync_fault_state.observed);
    sync_fault_state.expected_tid = tid;
    sync_fault_state.done = done;
    atomic_set(&sync_fault_state.armed, 1);
}

static void
sync_fault_disarm(void)
{
    atomic_clear(&sync_fault_state.armed);
    atomic_clear(&sync_fault_state.observed);
    sync_fault_state.expected_tid = NULL;
    sync_fault_state.done = NULL;
}

void
k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    ARG_UNUSED(esf);

    if (reason != K_ERR_KERNEL_OOPS || !atomic_get(&sync_fault_state.armed)
        || atomic_get(&sync_fault_state.observed)
        || k_current_get() != sync_fault_state.expected_tid
        || sync_fault_state.done == NULL
        || !atomic_cas(&sync_fault_state.observed, 0, 1)) {
        k_fatal_halt(reason);
    }

    k_sem_give(sync_fault_state.done);
}

static void
missing_condvar_wait_worker(void *arg1, void *arg2, void *arg3)
{
    struct missing_condvar_wait_context *context = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    context->lock_result = k_mutex_lock(context->mutex, K_FOREVER);
    if (context->lock_result != 0) {
        return;
    }

    context->reached_wait = true;
    context->wait_result =
        k_condvar_wait(context->condvar, context->mutex, K_MSEC(1));
    context->returned = true;
    (void)k_mutex_unlock(context->mutex);
}

static void
prepared_sync_access_worker(void *arg1, void *arg2, void *arg3)
{
    struct prepared_sync_access_context *context = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    context->lock_result = k_mutex_lock(context->mutex, K_NO_WAIT);
    if (context->lock_result != 0) {
        return;
    }
    context->signal_result = k_condvar_signal(context->condvar);
    context->unlock_result = k_mutex_unlock(context->mutex);
    context->completed = true;
}

static void
rejected_prepare_owner(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
}

k_tid_t
wamr_test_sync_pool_owner(void)
{
    if (private_sync_pool_owner_tid == NULL) {
        private_sync_pool_owner_tid = k_thread_create(
            &prepared_sync_access_thread, prepared_sync_access_thread_stack,
            K_THREAD_STACK_SIZEOF(prepared_sync_access_thread_stack),
            rejected_prepare_owner, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
        if (private_sync_pool_owner_tid != NULL) {
            k_object_access_grant(private_sync_pool_owner_tid,
                                  k_current_get());
        }
    }
    return private_sync_pool_owner_tid;
}

static void
probe_prepare_owner(void *arg1, void *arg2, void *arg3)
{
    struct rejected_prepare_context *context = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    (void)k_thread_priority_get(context->owner);
    context->owner_access_completed = true;
    k_sem_give(context->done);
}

static void
reject_pool_prepare_then_drop_to_user(void *arg1, void *arg2, void *arg3)
{
    struct rejected_prepare_context *context = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (context->kind == REJECTED_THREAD_POOL_PREPARE) {
        context->prepare_result = wamr_zephyr_thread_pool_prepare(
            &rejected_prepare_threads, context->owner);
    }
    else {
        context->prepare_result = wamr_zephyr_sync_pool_prepare(
            &rejected_prepare_sync, context->owner);
    }
    k_thread_user_mode_enter(probe_prepare_owner, context, NULL, NULL);
}

static void
retry_pool_prepare_then_drop_to_user(void *arg1, void *arg2, void *arg3)
{
    struct rejected_prepare_context *context = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    if (context->kind == REJECTED_THREAD_POOL_PREPARE) {
        context->prepare_result =
            wamr_test_thread_pool_prepare_for(context->owner);
    }
    else {
        context->prepare_result =
            wamr_test_sync_pool_prepare_for(context->owner);
    }
    k_thread_user_mode_enter(probe_prepare_owner, context, NULL, NULL);
}

static void
assert_rejected_prepare_does_not_grant_owner(
    enum rejected_prepare_kind kind, const char *pool_kind)
{
    struct rejected_prepare_context *context = &rejected_prepare_ctx;
    k_tid_t owner_tid;
    k_tid_t caller_tid;
    int completion_result;
    int caller_join_result;
    int owner_join_result;
    bool observed_fault;

    memset(context, 0, sizeof(*context));
    k_sem_init(&rejected_prepare_fault_done, 0, 1);
    context->kind = kind;
    context->done = &rejected_prepare_fault_done;
    context->prepare_result = BHT_OK;
    owner_tid = k_thread_create(
        &missing_condvar_thread, missing_condvar_thread_stack,
        K_THREAD_STACK_SIZEOF(missing_condvar_thread_stack),
        rejected_prepare_owner, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
    zassert_not_null(owner_tid, "%s rejected owner creation failed",
                     pool_kind);
    context->owner = owner_tid;
    caller_tid = k_thread_create(
        &unrelated_sync_access_thread, unrelated_sync_access_thread_stack,
        K_THREAD_STACK_SIZEOF(unrelated_sync_access_thread_stack),
        reject_pool_prepare_then_drop_to_user, context, NULL, NULL, 5, 0,
        K_FOREVER);
    zassert_not_null(caller_tid, "%s rejected caller creation failed",
                     pool_kind);
    k_object_access_grant(&rejected_prepare_fault_done, caller_tid);
    sync_fault_arm(caller_tid, &rejected_prepare_fault_done);
    k_thread_start(caller_tid);
    completion_result =
        k_sem_take(&rejected_prepare_fault_done, K_SECONDS(1));
    caller_join_result = k_thread_join(caller_tid, K_SECONDS(1));
    observed_fault = atomic_get(&sync_fault_state.observed) != 0;
    if (caller_join_result != 0) {
        k_thread_abort(caller_tid);
    }
    k_thread_abort(owner_tid);
    owner_join_result = k_thread_join(owner_tid, K_SECONDS(1));
    sync_fault_disarm();

    zassert_equal(context->prepare_result, BHT_ERROR,
                  "%s replacement prepare returned %d", pool_kind,
                  context->prepare_result);
    zassert_equal(completion_result, 0,
                  "%s rejected-owner probe did not finish", pool_kind);
    zassert_equal(caller_join_result, 0,
                  "%s rejected caller did not terminate", pool_kind);
    zassert_equal(owner_join_result, 0,
                  "%s rejected owner did not terminate", pool_kind);
    zassert_false(context->owner_access_completed,
                  "%s rejection granted access to its supplied owner",
                  pool_kind);
    zassert_true(observed_fault,
                 "%s rejected-owner access was not observed as a fault",
                 pool_kind);
}

static void
assert_identical_prepare_grants_owner(enum rejected_prepare_kind kind,
                                      const char *pool_kind)
{
    struct rejected_prepare_context *context = &identical_prepare_ctx;
    k_tid_t owner_tid = NULL;
    k_tid_t caller_tid;
    int initial_prepare_result = BHT_OK;
    int completion_result;
    int caller_join_result;
    int owner_join_result = 0;
    int cleanup_init_result = BHT_OK;
    bool observed_fault;

    memset(context, 0, sizeof(*context));
    k_sem_init(&identical_prepare_fault_done, 0, 1);
    context->kind = kind;
    if (kind == REJECTED_THREAD_POOL_PREPARE) {
        sync_results.runtime_destroyed = true;
        wasm_runtime_destroy();
        owner_tid = k_thread_create(
            &missing_condvar_thread, missing_condvar_thread_stack,
            K_THREAD_STACK_SIZEOF(missing_condvar_thread_stack),
            rejected_prepare_owner, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
        zassert_not_null(owner_tid, "%s identical owner creation failed",
                         pool_kind);
        context->owner = owner_tid;
        k_object_access_grant(context->owner, k_current_get());
        initial_prepare_result =
            wamr_test_thread_pool_prepare_for(context->owner);
    }
    else {
        context->owner = wamr_test_sync_pool_owner();
    }
    context->done = &identical_prepare_fault_done;
    context->prepare_result = BHT_ERROR;
    caller_tid = k_thread_create(
        &unrelated_sync_access_thread, unrelated_sync_access_thread_stack,
        K_THREAD_STACK_SIZEOF(unrelated_sync_access_thread_stack),
        retry_pool_prepare_then_drop_to_user, context, NULL, NULL, 5, 0,
        K_FOREVER);
    zassert_not_null(caller_tid, "%s identical caller creation failed",
                     pool_kind);
    k_object_access_grant(&identical_prepare_fault_done, caller_tid);
    sync_fault_arm(caller_tid, &identical_prepare_fault_done);
    k_thread_start(caller_tid);
    completion_result =
        k_sem_take(&identical_prepare_fault_done, K_SECONDS(1));
    caller_join_result = k_thread_join(caller_tid, K_SECONDS(1));
    observed_fault = atomic_get(&sync_fault_state.observed) != 0;
    if (caller_join_result != 0) {
        k_thread_abort(caller_tid);
    }
    if (owner_tid != NULL) {
        k_thread_abort(owner_tid);
        owner_join_result = k_thread_join(owner_tid, K_SECONDS(1));
        cleanup_init_result = os_thread_sys_init();
        os_thread_sys_destroy();
    }
    sync_fault_disarm();

    zassert_equal(initial_prepare_result, BHT_OK,
                  "%s initial prepare returned %d", pool_kind,
                  initial_prepare_result);
    zassert_equal(context->prepare_result, BHT_OK,
                  "%s identical prepare returned %d", pool_kind,
                  context->prepare_result);
    zassert_equal(completion_result, 0,
                  "%s identical owner probe did not finish", pool_kind);
    zassert_equal(caller_join_result, 0,
                  "%s identical caller did not terminate", pool_kind);
    zassert_equal(owner_join_result, 0,
                  "%s identical owner did not terminate", pool_kind);
    zassert_equal(cleanup_init_result, BHT_OK,
                  "%s cleanup initialization returned %d", pool_kind,
                  cleanup_init_result);
    zassert_true(context->owner_access_completed,
                 "%s identical prepare did not grant owner access",
                 pool_kind);
    zassert_false(observed_fault,
                  "%s identical owner access faulted", pool_kind);
}
#endif

static bool
wait_for_atomic_value(atomic_t *value, atomic_val_t expected, int timeout_ms)
{
    for (int elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms++) {
        if (atomic_get(value) == expected) {
            return true;
        }
        k_sleep(K_MSEC(CONDITION_POLL_INTERVAL_MS));
    }

    return atomic_get(value) == expected;
}

static void
reset_mutex_counter(struct mutex_counter *counter,
                    struct mutex_counter_worker *workers)
{
    memset(counter, 0, sizeof(*counter));
    for (size_t i = 0; i < 2; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].counter = counter;
        workers[i].lock_result = BHT_ERROR;
        workers[i].unlock_result = BHT_ERROR;
    }
}

static void
reset_condition_waiter(struct condition_waiter *waiter, korp_mutex *mutex,
                       korp_cond *cond, uint64 timeout_us)
{
    memset(waiter, 0, sizeof(*waiter));
    waiter->mutex = mutex;
    waiter->cond = cond;
    waiter->result = BHT_ERROR;
    waiter->timeout_us = timeout_us;
}

static void *
increment_counter(void *arg)
{
    struct mutex_counter_worker *worker = arg;
    struct mutex_counter *counter = worker->counter;

    atomic_inc(&counter->started);
    if (!wait_for_atomic_value(&counter->release, 1,
                               CONDITION_READY_TIMEOUT_MS)) {
        worker->timed_out = true;
        return NULL;
    }
    for (int i = 0; i < 100; ++i) {
        worker->lock_result = os_mutex_lock(&counter->mutex);
        if (worker->lock_result != BHT_OK) {
            return NULL;
        }
        counter->value++;
        worker->unlock_result = os_mutex_unlock(&counter->mutex);
        if (worker->unlock_result != BHT_OK) {
            return NULL;
        }
    }
    return NULL;
}

static void *
wait_for_condition(void *arg)
{
    struct condition_waiter *waiter = arg;

    waiter->result = os_mutex_lock(waiter->mutex);
    if (waiter->result != BHT_OK) {
        return NULL;
    }

    waiter->waiting = true;
    atomic_set(&waiter->ready, 1);
    waiter->result = waiter->timeout_us == 0U
                         ? os_cond_wait(waiter->cond, waiter->mutex)
                         : os_cond_reltimedwait(waiter->cond, waiter->mutex,
                                                waiter->timeout_us);
    waiter->woke = waiter->result == BHT_OK;
    (void)os_mutex_unlock(waiter->mutex);

    return NULL;
}

static void *
run_claimed_mutex_operation(void *arg)
{
    struct mutex_operation_context *context = arg;

    context->lock_result = os_mutex_lock(context->mutex);
    if (context->lock_result == BHT_OK) {
        context->unlock_result = os_mutex_unlock(context->mutex);
    }
    return NULL;
}

static void *
run_concurrent_sync_lifecycle(void *arg)
{
    struct concurrent_sync_context *context = arg;

    context->mutex_init_result = os_mutex_init(&context->mutex);
    context->cond_init_result = os_cond_init(&context->cond);
    atomic_inc(context->initialized);
    if (wait_for_atomic_value(context->release, 1,
                              CONDITION_READY_TIMEOUT_MS)) {
        if (context->cond_init_result == BHT_OK) {
            context->cond_destroy_result = os_cond_destroy(&context->cond);
        }
        if (context->mutex_init_result == BHT_OK) {
            context->mutex_destroy_result = os_mutex_destroy(&context->mutex);
        }
    }
    return NULL;
}

static void
signal_waiter(struct condition_waiter *waiter)
{
    zassert_true(
        wait_for_atomic_value(&waiter->ready, 1, CONDITION_READY_TIMEOUT_MS),
        "waiter did not become ready");
    zassert_equal(os_mutex_lock(waiter->mutex), BHT_OK,
                  "parent failed to lock waiter mutex");
    zassert_true(waiter->waiting, "waiter did not publish its state");
    zassert_equal(os_cond_signal(waiter->cond), BHT_OK,
                  "condition signal failed");
    zassert_equal(os_mutex_unlock(waiter->mutex), BHT_OK,
                  "parent failed to unlock waiter mutex");
}

static void
broadcast_waiters(struct condition_waiter *first,
                  struct condition_waiter *second)
{
    zassert_true(
        wait_for_atomic_value(&first->ready, 1, CONDITION_READY_TIMEOUT_MS),
        "first waiter did not become ready");
    zassert_true(
        wait_for_atomic_value(&second->ready, 1, CONDITION_READY_TIMEOUT_MS),
        "second waiter did not become ready");
    zassert_equal(os_mutex_lock(first->mutex), BHT_OK,
                  "parent failed to lock waiter mutex");
    zassert_true(first->waiting, "first waiter did not publish its state");
    zassert_true(second->waiting, "second waiter did not publish its state");
    zassert_equal(os_cond_broadcast(first->cond), BHT_OK,
                  "condition broadcast failed");
    zassert_equal(os_mutex_unlock(first->mutex), BHT_OK,
                  "parent failed to unlock waiter mutex");
}

static void
join_waiter(korp_tid thread, struct condition_waiter *waiter)
{
    zassert_equal(os_thread_join(thread, NULL), BHT_OK, "thread join failed");
    zassert_equal(waiter->result, BHT_OK, "condition wait failed");
    zassert_true(waiter->woke, "waiter did not return from condition wait");
}

static void
init_stress_sem(struct k_sem *sem, unsigned int limit)
{
    k_sem_init(sem, 0, limit);
#if defined(CONFIG_USERSPACE)
    k_object_access_grant(sem, k_current_get());
#endif
}

static void *
run_stress_counter_worker(void *arg)
{
    struct stress_counter_worker *worker = arg;

    k_sem_give(&stress_counter_ready);
    worker->release_result =
        k_sem_take(&stress_counter_release, K_MSEC(SYNC_STRESS_GUARD_MS));
    if (worker->release_result == 0) {
        for (unsigned int i = 0; i < SYNC_STRESS_INCREMENTS; i++) {
            worker->lock_result = os_mutex_lock(worker->mutex);
            if (worker->lock_result != BHT_OK) {
                break;
            }
            (*worker->counter)++;
            worker->unlock_result = os_mutex_unlock(worker->mutex);
            if (worker->unlock_result != BHT_OK) {
                break;
            }
        }
    }
    k_sem_give(&stress_counter_done);
    return NULL;
}

static void *
run_stress_condition_waiter(void *arg)
{
    struct stress_condition_waiter *waiter = arg;
    int64_t deadline = k_uptime_get() + SYNC_STRESS_GUARD_MS;
    int64_t wake_time = deadline;

    waiter->lock_result = os_mutex_lock(waiter->mutex);
    if (waiter->lock_result == BHT_OK) {
        waiter->waiting = true;
        k_sem_give(&stress_condition_ready);
        while (atomic_get(waiter->generation) != waiter->expected_generation) {
            int64_t remaining_ms = deadline - k_uptime_get();

            if (remaining_ms <= 0) {
                waiter->timed_out = true;
                break;
            }
            waiter->wait_attempts++;
            waiter->wait_result = os_cond_reltimedwait(
                waiter->cond, waiter->mutex, (uint64)remaining_ms * 1000U);
            wake_time = k_uptime_get();
            if (waiter->wait_result != BHT_OK
                && waiter->wait_result != ETIMEDOUT) {
                break;
            }
        }
        waiter->observed_signal = waiter->wait_result == BHT_OK
                                  && atomic_get(waiter->generation)
                                         == waiter->expected_generation
                                  && wake_time < deadline;
        if (!waiter->observed_signal) {
            waiter->timed_out = wake_time >= deadline
                                || waiter->wait_result == ETIMEDOUT;
        }
        waiter->woke = waiter->observed_signal;
        if (waiter->woke) {
            atomic_inc(waiter->released);
        }
        waiter->unlock_result = os_mutex_unlock(waiter->mutex);
    }
    else {
        k_sem_give(&stress_condition_ready);
    }
    k_sem_give(&stress_condition_done);
    return NULL;
}

static void *
sync_setup(void)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    memset(&prepared_sync_access_ctx, 0, sizeof(prepared_sync_access_ctx));
    prepared_sync_access_ctx.mutex = wamr_test_sync_mutex();
    prepared_sync_access_ctx.condvar = wamr_test_sync_condvar();
#endif
    return &sync_results;
}

static void
sync_before(void *fixture)
{
    init_stress_sem(&stress_counter_ready, SYNC_STRESS_WORKERS);
    init_stress_sem(&stress_counter_release, SYNC_STRESS_WORKERS);
    init_stress_sem(&stress_counter_done, SYNC_STRESS_WORKERS);
    init_stress_sem(&stress_condition_ready, SYNC_STRESS_WORKERS);
    init_stress_sem(&stress_condition_done, SYNC_STRESS_WORKERS);
    init_stress_sem(&stress_mutex_claimed, 1U);
    init_stress_sem(&stress_mutex_release, 1U);
    sync_results.runtime_destroyed = false;
    pool_before(fixture);
}

static void
sync_after(void *fixture)
{
    if (!sync_results.runtime_destroyed) {
        pool_after(fixture);
    }
}

ZTEST_SUITE(platform_sync, NULL, sync_setup, sync_before, sync_after, NULL);

WAMR_CONTEXT_TEST_F(platform_sync, test_mutex_lifecycle)
{
    int init_result = os_mutex_init(&test_mutex);
    int lock_result =
        init_result == BHT_OK ? os_mutex_lock(&test_mutex) : BHT_ERROR;
    int unlock_result =
        lock_result == BHT_OK ? os_mutex_unlock(&test_mutex) : BHT_ERROR;
    int destroy_result =
        init_result == BHT_OK ? os_mutex_destroy(&test_mutex) : BHT_ERROR;

    zassert_equal(init_result, BHT_OK, "mutex init failed");
    zassert_equal(lock_result, BHT_OK, "mutex lock failed");
    zassert_equal(unlock_result, BHT_OK, "mutex unlock failed");
    zassert_equal(destroy_result, BHT_OK, "mutex destroy failed");
}

#if defined(CONFIG_USERSPACE)
static void
assert_shutdown_recovery(bool selected_mutex,
                         const struct shutdown_recovery_result *result);

static void
run_shutdown_recovery(bool selected_mutex,
                      struct shutdown_recovery_result *result)
{
    korp_mutex mutex = { 0 };
    korp_cond cond = NULL;

    memset(result, 0, sizeof(*result));
    result->selected_init_result = BHT_ERROR;
    result->selected_destroy_result = BHT_ERROR;
    result->reinit_result = BHT_ERROR;
    result->recovered_init_result = BHT_ERROR;
    result->recovered_destroy_result = BHT_ERROR;
    result->cycle_results_valid = true;
    result->failed_cycle = SYNC_STRESS_POOL_ITERATIONS;
    result->initial_identity = os_self_thread();

    for (size_t iteration = 0; iteration < SYNC_STRESS_POOL_ITERATIONS;
         iteration++) {
        int cycle_init_result;
        int active_destroy_result = BHT_ERROR;
        korp_tid cycle_refused_identity;

        cycle_init_result = selected_mutex ? os_mutex_init(&mutex)
                                           : os_cond_init(&cond);

        os_thread_sys_destroy();
        cycle_refused_identity = os_self_thread();

        if (selected_mutex && mutex != NULL) {
            active_destroy_result = os_mutex_destroy(&mutex);
        }
        else if (!selected_mutex && cond != NULL) {
            active_destroy_result = os_cond_destroy(&cond);
        }

        if (result->initial_identity == NULL || cycle_init_result != BHT_OK
            || cycle_refused_identity != result->initial_identity
            || active_destroy_result != BHT_OK
            || os_self_thread() != result->initial_identity) {
            result->cycle_results_valid = false;
            if (result->failed_cycle == SYNC_STRESS_POOL_ITERATIONS) {
                result->failed_cycle = iteration;
            }
            break;
        }
    }

    if (selected_mutex && mutex != NULL) {
        (void)os_mutex_destroy(&mutex);
    }
    else if (!selected_mutex && cond != NULL) {
        (void)os_cond_destroy(&cond);
    }

    result->selected_init_result =
        selected_mutex ? os_mutex_init(&mutex) : os_cond_init(&cond);

    /*
     * Runtime destruction releases its own synchronization slots before the
     * platform audit. The selected test slot is therefore the only reason the
     * supervisor identity remains mapped after this call.
     */
    sync_results.runtime_destroyed = true;
    wasm_runtime_destroy();
    result->refused_identity = os_self_thread();
    if (selected_mutex && mutex != NULL) {
        result->selected_destroy_result = os_mutex_destroy(&mutex);
    }
    else if (!selected_mutex && cond != NULL) {
        result->selected_destroy_result = os_cond_destroy(&cond);
    }
    result->clean_identity = os_self_thread();
    result->reinit_result = os_thread_sys_init();
    result->recovered_identity = os_self_thread();
    result->recovered_init_result =
        selected_mutex ? os_mutex_init(&mutex) : os_cond_init(&cond);
    result->recovered_handle_valid =
        selected_mutex ? mutex != NULL : cond != NULL;
    if (selected_mutex && mutex != NULL) {
        result->recovered_destroy_result = os_mutex_destroy(&mutex);
    }
    else if (!selected_mutex && cond != NULL) {
        result->recovered_destroy_result = os_cond_destroy(&cond);
    }

    os_thread_sys_destroy();
    result->final_identity = os_self_thread();
}

static void
assert_shutdown_recovery(bool selected_mutex,
                         const struct shutdown_recovery_result *result)
{
    const char *slot_kind = selected_mutex ? "mutex" : "condition";

    zassert_not_null(result->initial_identity,
                     "%s shutdown failed_cycle %zu missing identity",
                     slot_kind, result->failed_cycle);
    zassert_true(result->cycle_results_valid,
                 "%s shutdown failed_cycle %zu repeated deferral failed",
                 slot_kind, result->failed_cycle);
    zassert_equal(result->selected_init_result, BHT_OK,
                  "%s shutdown failed_cycle %zu selected init %d", slot_kind,
                  result->failed_cycle, result->selected_init_result);
    zassert_equal_ptr(result->refused_identity, result->initial_identity,
                      "%s shutdown failed_cycle %zu selected slot did not "
                      "defer runtime destroy",
                      slot_kind, result->failed_cycle);
    zassert_equal(result->selected_destroy_result, BHT_OK,
                  "%s shutdown failed_cycle %zu selected destroy %d",
                  slot_kind, result->failed_cycle,
                  result->selected_destroy_result);
    zassert_is_null(result->clean_identity,
                    "%s shutdown failed_cycle %zu selected destroy did not "
                    "complete shutdown",
                    slot_kind, result->failed_cycle);
    zassert_equal(result->reinit_result, BHT_OK,
                  "%s shutdown failed_cycle %zu reinit %d", slot_kind,
                  result->failed_cycle, result->reinit_result);
    zassert_not_null(result->recovered_identity,
                     "%s shutdown failed_cycle %zu missing recovered identity",
                     slot_kind, result->failed_cycle);
    zassert_equal(result->recovered_init_result, BHT_OK,
                  "%s shutdown failed_cycle %zu recovery init %d", slot_kind,
                  result->failed_cycle, result->recovered_init_result);
    zassert_true(result->recovered_handle_valid,
                 "%s shutdown failed_cycle %zu invalid recovered handle",
                 slot_kind, result->failed_cycle);
    zassert_equal(result->recovered_destroy_result, BHT_OK,
                  "%s shutdown failed_cycle %zu recovery destroy %d",
                  slot_kind, result->failed_cycle,
                  result->recovered_destroy_result);
    zassert_is_null(result->final_identity,
                    "%s shutdown failed_cycle %zu final shutdown incomplete",
                    slot_kind, result->failed_cycle);
}
#endif

WAMR_CONTEXT_TEST_F(platform_sync,
                    test_shutdown_rejects_active_mutex_slot_and_recovers)
{
#if defined(CONFIG_USERSPACE)
    run_shutdown_recovery(true, &fixture->shutdown_recovery);
    assert_shutdown_recovery(true, &fixture->shutdown_recovery);
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST_F(platform_sync,
                    test_shutdown_rejects_active_condition_slot_and_recovers)
{
#if defined(CONFIG_USERSPACE)
    run_shutdown_recovery(false, &fixture->shutdown_recovery);
    assert_shutdown_recovery(false, &fixture->shutdown_recovery);
#else
    ztest_test_skip();
#endif
}

ZTEST(platform_sync,
      test_prepared_native_sync_objects_require_inherited_permissions)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    k_tid_t inherited_tid;
    k_tid_t unrelated_tid;
    int join_result;
    int completion_result;
    bool observed_fault;

    /* Mutations caught: public grants or missing current-thread regrants. */
    memset(&prepared_sync_access_ctx, 0, sizeof(prepared_sync_access_ctx));
    prepared_sync_access_ctx.mutex = wamr_test_sync_mutex();
    prepared_sync_access_ctx.condvar = wamr_test_sync_condvar();
    zassert_not_null(prepared_sync_access_ctx.mutex,
                     "prepared native mutex slot missing");
    zassert_not_null(prepared_sync_access_ctx.condvar,
                     "prepared native condvar slot missing");
    inherited_tid = k_thread_create(
        &missing_condvar_thread, missing_condvar_thread_stack,
        K_THREAD_STACK_SIZEOF(missing_condvar_thread_stack),
        prepared_sync_access_worker, &prepared_sync_access_ctx, NULL, NULL, 5,
        K_USER | K_INHERIT_PERMS, K_FOREVER);
    zassert_not_null(inherited_tid,
                     "prepared-sync inherited user thread creation failed");
    k_thread_start(inherited_tid);
    join_result = k_thread_join(inherited_tid, K_SECONDS(1));
    if (join_result != 0) {
        k_thread_abort(inherited_tid);
    }

    zassert_equal(join_result, 0,
                  "prepared-sync inherited user thread did not join");
    zassert_true(prepared_sync_access_ctx.completed,
                 "prepared-sync inherited user thread did not complete");
    zassert_equal(prepared_sync_access_ctx.lock_result, 0,
                  "inherited native mutex lock failed");
    zassert_equal(prepared_sync_access_ctx.signal_result, 0,
                  "inherited native condvar signal failed");
    zassert_equal(prepared_sync_access_ctx.unlock_result, 0,
                  "inherited native mutex unlock failed");

    memset(&unrelated_sync_access_ctx, 0, sizeof(unrelated_sync_access_ctx));
    unrelated_sync_access_ctx.mutex = wamr_test_sync_mutex();
    unrelated_sync_access_ctx.condvar = wamr_test_sync_condvar();
    k_sem_init(&unrelated_sync_access_fault_done, 0, 1);
    unrelated_tid = k_thread_create(
        &unrelated_sync_access_thread, unrelated_sync_access_thread_stack,
        K_THREAD_STACK_SIZEOF(unrelated_sync_access_thread_stack),
        prepared_sync_access_worker, &unrelated_sync_access_ctx, NULL, NULL, 5,
        K_USER, K_FOREVER);
    zassert_not_null(unrelated_tid,
                     "prepared-sync unrelated user thread creation failed");
    sync_fault_arm(unrelated_tid, &unrelated_sync_access_fault_done);
    k_thread_start(unrelated_tid);
    completion_result =
        k_sem_take(&unrelated_sync_access_fault_done, K_SECONDS(1));
    join_result = k_thread_join(unrelated_tid, K_SECONDS(1));
    observed_fault = atomic_get(&sync_fault_state.observed) != 0;
    if (join_result != 0) {
        k_thread_abort(unrelated_tid);
    }

    zassert_equal(completion_result, 0,
                  "unrelated user thread accessed sync pool without a fault");
    zassert_equal(join_result, 0,
                  "faulting unrelated user thread did not terminate");
    zassert_false(unrelated_sync_access_ctx.completed,
                  "unrelated user thread completed without inherited grants");
    zassert_true(observed_fault,
                 "unrelated sync-pool access was not observed as a fault");
    sync_fault_disarm();
#else
    ztest_test_skip();
#endif
}

ZTEST(platform_sync, test_rejected_thread_pool_prepare_does_not_grant_owner)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    assert_rejected_prepare_does_not_grant_owner(
        REJECTED_THREAD_POOL_PREPARE, "thread pool");
#else
    ztest_test_skip();
#endif
}

ZTEST(platform_sync, test_rejected_sync_pool_prepare_does_not_grant_owner)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    assert_rejected_prepare_does_not_grant_owner(REJECTED_SYNC_POOL_PREPARE,
                                                  "sync pool");
#else
    ztest_test_skip();
#endif
}

ZTEST(platform_sync, test_identical_thread_pool_prepare_grants_owner)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    assert_identical_prepare_grants_owner(REJECTED_THREAD_POOL_PREPARE,
                                          "thread pool");
#else
    ztest_test_skip();
#endif
}

ZTEST(platform_sync, test_identical_sync_pool_prepare_grants_owner)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    assert_identical_prepare_grants_owner(REJECTED_SYNC_POOL_PREPARE,
                                          "sync pool");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_mutex_serializes_two_threads)
{
    struct mutex_counter *counter = &sync_results.counter;
    struct mutex_counter_worker *workers = sync_results.counter_workers;
    korp_tid first;
    korp_tid second;

    reset_mutex_counter(counter, workers);
    zassert_equal(os_mutex_init(&counter->mutex), BHT_OK, "mutex init failed");
    zassert_equal(os_thread_create(&first, increment_counter, &workers[0],
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "first thread creation failed");
    zassert_true(
        wait_for_atomic_value(&counter->started, 1, CONDITION_READY_TIMEOUT_MS),
        "first thread did not start");
    zassert_equal(os_thread_create(&second, increment_counter, &workers[1],
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "second thread creation failed");
    zassert_true(
        wait_for_atomic_value(&counter->started, 2, CONDITION_READY_TIMEOUT_MS),
        "second thread did not start");
    atomic_set(&counter->release, 1);
    zassert_equal(os_thread_join(first, NULL), BHT_OK,
                  "first thread join failed");
    zassert_equal(os_thread_join(second, NULL), BHT_OK,
                  "second thread join failed");
    for (size_t i = 0; i < ARRAY_SIZE(sync_results.counter_workers); i++) {
        zassert_false(workers[i].timed_out,
                      "worker %zu did not receive its release", i);
        zassert_equal(workers[i].lock_result, BHT_OK,
                      "worker %zu mutex lock failed", i);
        zassert_equal(workers[i].unlock_result, BHT_OK,
                      "worker %zu mutex unlock failed", i);
    }
    zassert_equal(counter->value, 200, "mutex did not serialize increments");
    zassert_equal(os_mutex_destroy(&counter->mutex), BHT_OK,
                  "mutex destroy failed");
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_signal_wakes_waiter)
{
    struct condition_waiter *waiter = &sync_results.waiter;
    korp_tid thread;

    reset_condition_waiter(waiter, &sync_results.condition_mutex,
                           &sync_results.condition_cond, 0U);
    zassert_equal(os_mutex_init(waiter->mutex), BHT_OK, "mutex init failed");
    zassert_equal(os_cond_init(waiter->cond), BHT_OK, "condition init failed");
    zassert_equal(os_thread_create(&thread, wait_for_condition, waiter,
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    signal_waiter(waiter);
    join_waiter(thread, waiter);
    zassert_equal(os_cond_destroy(waiter->cond), BHT_OK,
                  "condition destroy failed");
    zassert_equal(os_mutex_destroy(waiter->mutex), BHT_OK,
                  "mutex destroy failed");
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_timed_wait_returns)
{
    uint64 start_us;
    uint64 elapsed_us;
    int wait_result;

    zassert_equal(os_mutex_init(&sync_results.condition_mutex), BHT_OK,
                  "mutex init failed");
    zassert_equal(os_cond_init(&sync_results.condition_cond), BHT_OK,
                  "condition init failed");
    zassert_equal(os_mutex_lock(&sync_results.condition_mutex), BHT_OK,
                  "mutex lock failed");
    start_us = os_time_get_boot_us();
    wait_result = os_cond_reltimedwait(&sync_results.condition_cond,
                                       &sync_results.condition_mutex, 20000U);
    elapsed_us = os_time_get_boot_us() - start_us;
    zassert_equal(os_mutex_unlock(&sync_results.condition_mutex), BHT_OK,
                  "mutex unlock failed");
    zassert_true(elapsed_us >= 10000U, "timed wait returned too early");
    zassert_true(elapsed_us < 500000U, "timed wait exceeded its bound");
    zassert_equal(os_cond_destroy(&sync_results.condition_cond), BHT_OK,
                  "condition destroy failed");
    zassert_equal(os_mutex_destroy(&sync_results.condition_mutex), BHT_OK,
                  "mutex destroy failed");
#if defined(CONFIG_USERSPACE)
    zassert_equal(wait_result, ETIMEDOUT,
                  "timed condition wait did not report timeout");
#else
    zassert_equal(wait_result, BHT_OK, "timed condition wait failed");
#endif
}

WAMR_CONTEXT_TEST_F(platform_sync, test_mutex_is_reusable)
{
    int init_result = os_mutex_init(&test_mutex);
    int lock_results[2] = { BHT_ERROR, BHT_ERROR };
    int unlock_results[2] = { BHT_ERROR, BHT_ERROR };

    for (int i = 0; i < 2; ++i) {
        if (init_result == BHT_OK) {
            lock_results[i] = os_mutex_lock(&test_mutex);
        }
        if (lock_results[i] == BHT_OK) {
            unlock_results[i] = os_mutex_unlock(&test_mutex);
        }
    }
    int destroy_result =
        init_result == BHT_OK ? os_mutex_destroy(&test_mutex) : BHT_ERROR;

    zassert_equal(init_result, BHT_OK, "mutex init failed");
    zassert_equal(lock_results[0], BHT_OK, "first mutex lock failed");
    zassert_equal(unlock_results[0], BHT_OK, "first mutex unlock failed");
    zassert_equal(lock_results[1], BHT_OK, "second mutex lock failed");
    zassert_equal(unlock_results[1], BHT_OK, "second mutex unlock failed");
    zassert_equal(destroy_result, BHT_OK, "mutex destroy failed");
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_broadcast_wakes_waiters)
{
    struct condition_waiter *first = &sync_results.broadcast_waiters[0];
    struct condition_waiter *second = &sync_results.broadcast_waiters[1];
    korp_tid first_thread;
    korp_tid second_thread;

    reset_condition_waiter(first, &sync_results.condition_mutex,
                           &sync_results.condition_cond, 0U);
    reset_condition_waiter(second, &sync_results.condition_mutex,
                           &sync_results.condition_cond, 0U);
    zassert_equal(os_mutex_init(first->mutex), BHT_OK, "mutex init failed");
    zassert_equal(os_cond_init(first->cond), BHT_OK, "condition init failed");
    zassert_equal(os_thread_create(&first_thread, wait_for_condition, first,
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "first thread creation failed");
    zassert_equal(os_thread_create(&second_thread, wait_for_condition, second,
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "second thread creation failed");
    broadcast_waiters(first, second);
    join_waiter(first_thread, first);
    join_waiter(second_thread, second);
    zassert_equal(os_cond_destroy(first->cond), BHT_OK,
                  "condition destroy failed");
    zassert_equal(os_mutex_destroy(first->mutex), BHT_OK,
                  "mutex destroy failed");
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_is_reusable)
{
    struct condition_waiter *waiter = &sync_results.waiter;

    reset_condition_waiter(waiter, &sync_results.condition_mutex,
                           &sync_results.condition_cond, 0U);
    zassert_equal(os_mutex_init(waiter->mutex), BHT_OK, "mutex init failed");
    zassert_equal(os_cond_init(waiter->cond), BHT_OK, "condition init failed");
    for (int i = 0; i < 2; ++i) {
        korp_tid thread;

        reset_condition_waiter(waiter, waiter->mutex, waiter->cond, 0U);
        zassert_equal(os_thread_create(&thread, wait_for_condition, waiter,
                                       WAMR_TEST_THREAD_STACK_SIZE),
                      BHT_OK, "thread creation failed");
        signal_waiter(waiter);
        join_waiter(thread, waiter);
    }
    zassert_equal(os_cond_destroy(waiter->cond), BHT_OK,
                  "condition destroy failed");
    zassert_equal(os_mutex_destroy(waiter->mutex), BHT_OK,
                  "mutex destroy failed");
}

ZTEST(platform_sync, test_condvar_wait_requires_a_grant)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    k_tid_t tid;
    bool observed_fault;
    int completion_result;
    int join_result;

    memset(&missing_condvar_wait_ctx, 0, sizeof(missing_condvar_wait_ctx));
    k_sem_init(&missing_condvar_fault_done, 0, 1);
    zassert_equal(k_mutex_init(&missing_condvar_mutex), 0,
                  "negative-control mutex init failed");
    zassert_equal(k_condvar_init(&missing_condvar), 0,
                  "negative-control condvar init failed");
    missing_condvar_wait_ctx.mutex = &missing_condvar_mutex;
    missing_condvar_wait_ctx.condvar = &missing_condvar;
    tid =
        k_thread_create(&missing_condvar_thread, missing_condvar_thread_stack,
                        K_THREAD_STACK_SIZEOF(missing_condvar_thread_stack),
                        missing_condvar_wait_worker, &missing_condvar_wait_ctx,
                        NULL, NULL, 5, K_USER, K_FOREVER);
    zassert_not_null(tid, "negative-control user thread creation failed");
    k_object_access_grant(&missing_condvar_mutex, tid);
    sync_fault_arm(tid, &missing_condvar_fault_done);
    k_thread_start(tid);
    completion_result = k_sem_take(&missing_condvar_fault_done, K_SECONDS(1));
    join_result = k_thread_join(tid, K_SECONDS(1));
    observed_fault = atomic_get(&sync_fault_state.observed) != 0;
    if (join_result != 0) {
        k_thread_abort(tid);
    }

    zassert_equal(completion_result, 0,
                  "missing condvar grant did not raise a recoverable fault");
    zassert_equal(join_result, 0,
                  "faulting negative-control thread did not terminate");
    zassert_true(missing_condvar_wait_ctx.reached_wait,
                 "negative-control thread never reached k_condvar_wait()");
    zassert_false(missing_condvar_wait_ctx.returned,
                  "k_condvar_wait() returned without a condvar grant");
    zassert_true(observed_fault,
                 "missing condvar grant was not observed as a fault");
    sync_fault_disarm();
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_large_condition_timeout_is_clamped)
{
    struct condition_waiter *waiter = &sync_results.waiter;
    korp_tid thread;

    reset_condition_waiter(waiter, &sync_results.condition_mutex,
                           &sync_results.condition_cond,
                           (uint64)INT32_MAX * 1000ULL + 1000ULL);
    zassert_equal(os_mutex_init(waiter->mutex), BHT_OK, "mutex init failed");
    zassert_equal(os_cond_init(waiter->cond), BHT_OK, "condition init failed");
    zassert_equal(os_thread_create(&thread, wait_for_condition, waiter,
                                   WAMR_TEST_THREAD_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    signal_waiter(waiter);
    join_waiter(thread, waiter);
    zassert_equal(os_cond_destroy(waiter->cond), BHT_OK,
                  "condition destroy failed");
    zassert_equal(os_mutex_destroy(waiter->mutex), BHT_OK,
                  "mutex destroy failed");
}

WAMR_CONTEXT_TEST(platform_sync,
                  test_recursive_mutex_rejects_destroy_while_locked)
{
#if defined(CONFIG_USERSPACE)
    korp_mutex mutex = NULL;
    korp_mutex cleanup_handle = NULL;
    struct k_mutex *native_mutex = NULL;
    int init_result = os_recursive_mutex_init(&mutex);
    int first_lock_result = BHT_ERROR;
    int second_lock_result = BHT_ERROR;
    int busy_destroy_result = BHT_ERROR;
    int first_unlock_result = BHT_ERROR;
    int second_unlock_result = BHT_ERROR;
    int destroy_result = BHT_ERROR;

    if (init_result == BHT_OK) {
        cleanup_handle = mutex;
        native_mutex = wamr_zephyr_sync_test_native_mutex(cleanup_handle);
        first_lock_result = os_mutex_lock(&mutex);
    }
    if (first_lock_result == BHT_OK) {
        second_lock_result = os_mutex_lock(&mutex);
    }
    if (second_lock_result == BHT_OK) {
        busy_destroy_result = os_mutex_destroy(&mutex);
        if (busy_destroy_result == BHT_OK) {
            zassert_not_null(native_mutex,
                             "destroyed recursive mutex lost native slot");
            zassert_equal(k_mutex_unlock(native_mutex), 0,
                          "failed to recover first native mutex depth");
            zassert_equal(k_mutex_unlock(native_mutex), 0,
                          "failed to recover second native mutex depth");
        }
        else {
            first_unlock_result =
                os_mutex_unlock(mutex != NULL ? &mutex : &cleanup_handle);
            second_unlock_result =
                os_mutex_unlock(mutex != NULL ? &mutex : &cleanup_handle);
        }
    }
    if (busy_destroy_result != BHT_OK && mutex != NULL) {
        destroy_result = os_mutex_destroy(&mutex);
    }

    zassert_equal(init_result, BHT_OK, "recursive mutex init failed");
    zassert_equal(first_lock_result, BHT_OK, "first recursive lock failed");
    zassert_equal(second_lock_result, BHT_OK, "second recursive lock failed");
    zassert_equal(busy_destroy_result, BHT_ERROR,
                  "locked recursive mutex was destroyed");
    if (busy_destroy_result != BHT_OK) {
        zassert_equal(first_unlock_result, BHT_OK,
                      "first recursive unlock failed");
        zassert_equal(second_unlock_result, BHT_OK,
                      "second recursive unlock failed");
        zassert_equal(destroy_result, BHT_OK, "recursive mutex destroy failed");
    }
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync_pool,
                  test_condition_pool_exhaustion_is_independent_and_recovers)
{
#if defined(CONFIG_USERSPACE)
    korp_cond conditions[TEST_COND_POOL_COUNT + 1] = { NULL };
    korp_mutex independent_mutex = NULL;
    korp_cond recovered = NULL;
    int init_results[TEST_COND_POOL_COUNT + 1];
    int independent_mutex_result;
    int independent_mutex_destroy_result = BHT_ERROR;
    int recovery_result;
    int recovery_destroy_result = BHT_ERROR;
    bool cleanup_succeeded = true;

    for (int i = 0; i < ARRAY_SIZE(conditions); i++) {
        init_results[i] = os_cond_init(&conditions[i]);
    }
    independent_mutex_result = os_mutex_init(&independent_mutex);
    if (independent_mutex_result == BHT_OK) {
        independent_mutex_destroy_result = os_mutex_destroy(&independent_mutex);
    }
    for (int i = 0; i < ARRAY_SIZE(conditions); i++) {
        if (init_results[i] == BHT_OK
            && os_cond_destroy(&conditions[i]) != BHT_OK) {
            cleanup_succeeded = false;
        }
    }
    recovery_result = os_cond_init(&recovered);
    if (recovery_result == BHT_OK) {
        recovery_destroy_result = os_cond_destroy(&recovered);
    }

    for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
        zassert_equal(init_results[i], BHT_OK,
                      "condition pool exhausted before advertised capacity");
    }
    zassert_equal(init_results[TEST_COND_POOL_COUNT], BHT_ERROR,
                  "condition pool exceeded advertised capacity");
    zassert_equal(independent_mutex_result, BHT_OK,
                  "condition exhaustion consumed mutex capacity");
    zassert_equal(independent_mutex_destroy_result, BHT_OK,
                  "independent mutex cleanup failed");
    zassert_true(cleanup_succeeded, "condition exhaustion cleanup failed");
    zassert_equal(recovery_result, BHT_OK,
                  "condition pool did not recover after exhaustion");
    zassert_equal(recovery_destroy_result, BHT_OK,
                  "recovered condition cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync,
                  test_stale_mutex_handle_is_rejected_after_reuse)
{
#if defined(CONFIG_USERSPACE)
    korp_mutex original = NULL;
    korp_mutex stale = NULL;
    korp_mutex replacement = NULL;
    int init_result = os_mutex_init(&original);
    int first_destroy_result = BHT_ERROR;
    int replacement_init_result = BHT_ERROR;
    int stale_lock_result = BHT_ERROR;
    int stale_unlock_result = BHT_ERROR;
    int stale_destroy_result = BHT_ERROR;
    int replacement_destroy_result = BHT_ERROR;

    if (init_result == BHT_OK) {
        stale = original;
        first_destroy_result = os_mutex_destroy(&original);
    }
    if (first_destroy_result == BHT_OK) {
        replacement_init_result = os_mutex_init(&replacement);
        stale_lock_result = os_mutex_lock(&stale);
        if (stale_lock_result == BHT_OK) {
            stale_unlock_result = os_mutex_unlock(&stale);
        }
        stale_destroy_result = os_mutex_destroy(&stale);
    }
    if (replacement != NULL) {
        replacement_destroy_result = os_mutex_destroy(&replacement);
    }
    if (original != NULL) {
        (void)os_mutex_destroy(&original);
    }

    zassert_equal(init_result, BHT_OK, "original mutex init failed");
    zassert_equal(first_destroy_result, BHT_OK,
                  "original mutex destroy failed");
    zassert_equal(replacement_init_result, BHT_OK,
                  "replacement mutex init failed");
    zassert_equal(stale_lock_result, BHT_ERROR,
                  "stale mutex handle locked replacement slot");
    zassert_equal(stale_unlock_result, BHT_ERROR,
                  "stale mutex handle unexpectedly needed cleanup");
    zassert_equal(stale_destroy_result, BHT_ERROR,
                  "stale mutex handle destroyed replacement slot");
    zassert_equal(replacement_destroy_result, BHT_OK,
                  "replacement mutex cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync,
                  test_stale_condition_handle_is_rejected_after_reuse)
{
#if defined(CONFIG_USERSPACE)
    korp_cond original = NULL;
    korp_cond stale = NULL;
    korp_cond replacement = NULL;
    int init_result = os_cond_init(&original);
    int first_destroy_result = BHT_ERROR;
    int replacement_init_result = BHT_ERROR;
    int stale_signal_result = BHT_ERROR;
    int stale_destroy_result = BHT_ERROR;
    int replacement_destroy_result = BHT_ERROR;

    if (init_result == BHT_OK) {
        stale = original;
        first_destroy_result = os_cond_destroy(&original);
    }
    if (first_destroy_result == BHT_OK) {
        replacement_init_result = os_cond_init(&replacement);
        stale_signal_result = os_cond_signal(&stale);
        stale_destroy_result = os_cond_destroy(&stale);
    }
    if (replacement != NULL) {
        replacement_destroy_result = os_cond_destroy(&replacement);
    }
    if (original != NULL) {
        (void)os_cond_destroy(&original);
    }

    zassert_equal(init_result, BHT_OK, "original condition init failed");
    zassert_equal(first_destroy_result, BHT_OK,
                  "original condition destroy failed");
    zassert_equal(replacement_init_result, BHT_OK,
                  "replacement condition init failed");
    zassert_equal(stale_signal_result, BHT_ERROR,
                  "stale condition handle signaled replacement slot");
    zassert_equal(stale_destroy_result, BHT_ERROR,
                  "stale condition handle destroyed replacement slot");
    zassert_equal(replacement_destroy_result, BHT_OK,
                  "replacement condition cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_mutex_destroy_rejects_active_operation)
{
#if defined(CONFIG_USERSPACE)
    struct mutex_operation_context *operation = &sync_results.claimed_operation;
    korp_tid thread = NULL;
    int init_result;
    int create_result = BHT_ERROR;
    int destroy_while_claimed_result = BHT_ERROR;
    int join_result = BHT_ERROR;
    int final_destroy_result = BHT_ERROR;
    bool claim_reached = false;

    sync_results.claimed_mutex = NULL;
    memset(operation, 0, sizeof(*operation));
    operation->mutex = &sync_results.claimed_mutex;
    operation->lock_result = BHT_ERROR;
    operation->unlock_result = BHT_ERROR;
    atomic_clear(&sync_claim_hook.reached);
    atomic_clear(&sync_claim_hook.release);
    atomic_set(&sync_claim_hook.armed, 1);

    init_result = os_mutex_init(&sync_results.claimed_mutex);
    if (init_result == BHT_OK) {
        sync_claim_hook.expected_handle = (uintptr_t)sync_results.claimed_mutex;
        create_result =
            os_thread_create(&thread, run_claimed_mutex_operation, operation,
                             WAMR_TEST_THREAD_STACK_SIZE);
    }
    if (create_result == BHT_OK) {
        claim_reached = wait_for_atomic_value(&sync_claim_hook.reached, 1,
                                              CONDITION_READY_TIMEOUT_MS);
        if (claim_reached) {
            destroy_while_claimed_result =
                os_mutex_destroy(&sync_results.claimed_mutex);
        }
        atomic_set(&sync_claim_hook.release, 1);
        join_result = os_thread_join(thread, NULL);
    }
    else {
        atomic_set(&sync_claim_hook.release, 1);
    }
    if (sync_results.claimed_mutex != NULL) {
        final_destroy_result = os_mutex_destroy(&sync_results.claimed_mutex);
    }

    zassert_equal(init_result, BHT_OK, "claimed mutex init failed");
    zassert_equal(create_result, BHT_OK, "claiming worker creation failed");
    zassert_true(claim_reached, "mutex operation claim hook was not reached");
    zassert_equal(destroy_while_claimed_result, BHT_ERROR,
                  "operation-claimed mutex was destroyed");
    zassert_equal(join_result, BHT_OK, "claiming worker join failed");
    zassert_equal(operation->lock_result, BHT_OK,
                  "claimed mutex lock failed after release");
    zassert_equal(operation->unlock_result, BHT_OK,
                  "claimed mutex unlock failed after release");
    zassert_equal(final_destroy_result, BHT_OK, "claimed mutex cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_destroy_rejects_active_waiter)
{
#if defined(CONFIG_USERSPACE)
    struct condition_waiter *waiter = &sync_results.waiter;
    korp_tid thread = NULL;
    korp_cond cleanup_cond = NULL;
    struct k_condvar *native_cond = NULL;
    int mutex_init_result;
    int cond_init_result;
    int create_result = BHT_ERROR;
    int parent_lock_result = BHT_ERROR;
    int busy_destroy_result = BHT_ERROR;
    int signal_result = BHT_ERROR;
    int parent_unlock_result = BHT_ERROR;
    int join_result = BHT_ERROR;
    int cond_destroy_result = BHT_ERROR;
    int mutex_destroy_result = BHT_ERROR;
    bool ready = false;

    reset_condition_waiter(waiter, &sync_results.condition_mutex,
                           &sync_results.condition_cond, 100000U);
    sync_results.condition_mutex = NULL;
    sync_results.condition_cond = NULL;
    mutex_init_result = os_mutex_init(waiter->mutex);
    cond_init_result = os_cond_init(waiter->cond);
    if (mutex_init_result == BHT_OK && cond_init_result == BHT_OK) {
        cleanup_cond = sync_results.condition_cond;
        native_cond = wamr_zephyr_sync_test_native_condvar(cleanup_cond);
        create_result = os_thread_create(&thread, wait_for_condition, waiter,
                                         WAMR_TEST_THREAD_STACK_SIZE);
    }
    if (create_result == BHT_OK) {
        ready = wait_for_atomic_value(&waiter->ready, 1,
                                      CONDITION_READY_TIMEOUT_MS);
        if (ready) {
            parent_lock_result = os_mutex_lock(waiter->mutex);
        }
        if (parent_lock_result == BHT_OK) {
            busy_destroy_result = os_cond_destroy(waiter->cond);
            if (busy_destroy_result == BHT_OK) {
                zassert_not_null(native_cond,
                                 "destroyed condition lost native slot");
                zassert_equal(wamr_zephyr_sync_test_restore_waiting_cond(
                                  cleanup_cond, native_cond),
                              BHT_OK,
                              "failed to recover destroyed waiting condition");
                sync_results.condition_cond = cleanup_cond;
            }
            signal_result = os_cond_signal(waiter->cond);
            parent_unlock_result = os_mutex_unlock(waiter->mutex);
        }
        join_result = os_thread_join(thread, NULL);
    }
    if (sync_results.condition_cond != NULL) {
        cond_destroy_result = os_cond_destroy(&sync_results.condition_cond);
    }
    if (sync_results.condition_mutex != NULL) {
        mutex_destroy_result = os_mutex_destroy(&sync_results.condition_mutex);
    }

    zassert_equal(mutex_init_result, BHT_OK, "waiter mutex init failed");
    zassert_equal(cond_init_result, BHT_OK, "waiter condition init failed");
    zassert_equal(create_result, BHT_OK, "waiter creation failed");
    zassert_true(ready, "waiter did not reach condition wait");
    zassert_equal(parent_lock_result, BHT_OK,
                  "parent could not acquire waiter mutex");
    zassert_equal(busy_destroy_result, BHT_ERROR,
                  "condition with an active waiter was destroyed");
    zassert_equal(signal_result, BHT_OK, "active waiter signal failed");
    zassert_equal(parent_unlock_result, BHT_OK,
                  "parent waiter mutex unlock failed");
    zassert_equal(join_result, BHT_OK, "waiter join failed");
    zassert_equal(waiter->result, BHT_OK, "active waiter did not wake");
    zassert_equal(cond_destroy_result, BHT_OK,
                  "waiter condition cleanup failed");
    zassert_equal(mutex_destroy_result, BHT_OK, "waiter mutex cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync,
                  test_concurrent_sync_lifecycles_use_distinct_slots)
{
#if defined(CONFIG_USERSPACE)
    korp_tid threads[TEST_COND_POOL_COUNT] = { NULL };
    korp_cond unexpected_cond = NULL;
    korp_mutex recovered_mutex = NULL;
    korp_cond recovered_cond = NULL;
    int created = 0;
    int unexpected_cond_result = BHT_ERROR;
    int recovery_mutex_result;
    int recovery_cond_result;
    int recovery_mutex_destroy_result = BHT_ERROR;
    int recovery_cond_destroy_result = BHT_ERROR;
    bool initialized = false;
    bool handles_are_distinct = true;
    bool joins_succeeded = true;

    memset(sync_results.concurrent, 0, sizeof(sync_results.concurrent));
    atomic_clear(&sync_results.concurrent_initialized);
    atomic_clear(&sync_results.concurrent_release);
    for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
        struct concurrent_sync_context *context = &sync_results.concurrent[i];

        context->initialized = &sync_results.concurrent_initialized;
        context->release = &sync_results.concurrent_release;
        context->mutex_init_result = BHT_ERROR;
        context->cond_init_result = BHT_ERROR;
        context->mutex_destroy_result = BHT_ERROR;
        context->cond_destroy_result = BHT_ERROR;
        if (os_thread_create(&threads[i], run_concurrent_sync_lifecycle,
                             context, WAMR_TEST_THREAD_STACK_SIZE)
            == BHT_OK) {
            created++;
        }
    }
    if (created == TEST_COND_POOL_COUNT) {
        initialized = wait_for_atomic_value(
            &sync_results.concurrent_initialized, TEST_COND_POOL_COUNT,
            CONDITION_READY_TIMEOUT_MS);
    }
    if (initialized) {
        for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
            for (int j = i + 1; j < TEST_COND_POOL_COUNT; j++) {
                if (sync_results.concurrent[i].mutex
                        == sync_results.concurrent[j].mutex
                    || sync_results.concurrent[i].cond
                           == sync_results.concurrent[j].cond) {
                    handles_are_distinct = false;
                }
            }
        }
        unexpected_cond_result = os_cond_init(&unexpected_cond);
        if (unexpected_cond_result == BHT_OK) {
            (void)os_cond_destroy(&unexpected_cond);
        }
    }
    atomic_set(&sync_results.concurrent_release, 1);
    for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
        if (threads[i] != NULL && os_thread_join(threads[i], NULL) != BHT_OK) {
            joins_succeeded = false;
        }
    }
    for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
        struct concurrent_sync_context *context = &sync_results.concurrent[i];

        if (context->cond != NULL) {
            (void)os_cond_destroy(&context->cond);
        }
        if (context->mutex != NULL) {
            (void)os_mutex_destroy(&context->mutex);
        }
    }
    recovery_mutex_result = os_mutex_init(&recovered_mutex);
    recovery_cond_result = os_cond_init(&recovered_cond);
    if (recovery_cond_result == BHT_OK) {
        recovery_cond_destroy_result = os_cond_destroy(&recovered_cond);
    }
    if (recovery_mutex_result == BHT_OK) {
        recovery_mutex_destroy_result = os_mutex_destroy(&recovered_mutex);
    }

    zassert_equal(created, TEST_COND_POOL_COUNT,
                  "not all concurrent lifecycle workers were created");
    zassert_true(initialized,
                 "concurrent lifecycle workers did not initialize");
    for (int i = 0; i < TEST_COND_POOL_COUNT; i++) {
        zassert_equal(sync_results.concurrent[i].mutex_init_result, BHT_OK,
                      "concurrent mutex init failed");
        zassert_equal(sync_results.concurrent[i].cond_init_result, BHT_OK,
                      "concurrent condition init failed");
        zassert_equal(sync_results.concurrent[i].mutex_destroy_result, BHT_OK,
                      "concurrent mutex destroy failed");
        zassert_equal(sync_results.concurrent[i].cond_destroy_result, BHT_OK,
                      "concurrent condition destroy failed");
    }
    zassert_true(handles_are_distinct,
                 "concurrent lifecycles received duplicate handles");
    zassert_equal(unexpected_cond_result, BHT_ERROR,
                  "concurrent conditions exceeded pool capacity");
    zassert_true(joins_succeeded, "concurrent lifecycle worker join failed");
    zassert_equal(recovery_mutex_result, BHT_OK,
                  "mutex pool did not recover after concurrent destroy");
    zassert_equal(recovery_cond_result, BHT_OK,
                  "condition pool did not recover after concurrent destroy");
    zassert_equal(recovery_mutex_destroy_result, BHT_OK,
                  "recovered mutex cleanup failed");
    zassert_equal(recovery_cond_destroy_result, BHT_OK,
                  "recovered condition cleanup failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_mutex_serializes_exact_worker_counter)
{
    for (size_t cycle = 0; cycle < SYNC_STRESS_COUNTER_CYCLES; cycle++) {
        korp_tid threads[SYNC_STRESS_WORKERS] = { NULL };
        korp_mutex *mutex = &sync_results.stress_mutex;
        struct stress_counter_worker *workers =
            sync_results.stress_counter_workers;
        int *counter = &sync_results.stress_counter;
        int create_results[SYNC_STRESS_WORKERS] = { BHT_ERROR };
        int ready_results[SYNC_STRESS_WORKERS] = { -EAGAIN };
        int done_results[SYNC_STRESS_WORKERS] = { -EAGAIN };
        int wait_results[SYNC_STRESS_WORKERS] = { BHT_ERROR };
        int join_results[SYNC_STRESS_WORKERS] = { BHT_ERROR };
        int init_result;
        int destroy_result = BHT_ERROR;

        memset(mutex, 0, sizeof(*mutex));
        memset(workers, 0, sizeof(sync_results.stress_counter_workers));
        *counter = 0;
        init_result = os_mutex_init(mutex);
        for (size_t i = 0; i < ARRAY_SIZE(threads); i++) {
            workers[i].mutex = mutex;
            workers[i].counter = counter;
            workers[i].release_result = -EAGAIN;
            workers[i].lock_result = BHT_ERROR;
            workers[i].unlock_result = BHT_ERROR;
            if (init_result == BHT_OK) {
                create_results[i] = os_thread_create(
                    &threads[i], run_stress_counter_worker, &workers[i],
                    WAMR_TEST_THREAD_STACK_SIZE);
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(threads); i++) {
            if (create_results[i] == BHT_OK) {
                ready_results[i] = k_sem_take(
                    &stress_counter_ready, K_MSEC(SYNC_STRESS_GUARD_MS));
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(threads); i++) {
            if (create_results[i] == BHT_OK) {
                k_sem_give(&stress_counter_release);
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(threads); i++) {
            if (create_results[i] == BHT_OK) {
                done_results[i] = k_sem_take(
                    &stress_counter_done, K_MSEC(SYNC_STRESS_GUARD_MS));
                wait_results[i] = wamr_zephyr_thread_test_wait(
                    threads[i], K_MSEC(SYNC_STRESS_GUARD_MS));
                if (wait_results[i] == 0) {
                    join_results[i] = os_thread_join(threads[i], NULL);
                }
            }
        }
        if (init_result == BHT_OK) {
            destroy_result = os_mutex_destroy(mutex);
        }

        zassert_equal(init_result, BHT_OK,
                      "counter cycle %zu init result %d", cycle, init_result);
        for (size_t i = 0; i < ARRAY_SIZE(threads); i++) {
            zassert_equal(create_results[i], BHT_OK,
                          "counter cycle %zu worker %zu create result %d",
                          cycle, i, create_results[i]);
            zassert_equal(ready_results[i], 0,
                          "counter cycle %zu worker %zu ready result %d", cycle,
                          i, ready_results[i]);
            zassert_equal(done_results[i], 0,
                          "counter cycle %zu worker %zu done result %d", cycle,
                          i, done_results[i]);
            zassert_equal(wait_results[i], 0,
                          "counter cycle %zu worker %zu exit result %d", cycle,
                          i, wait_results[i]);
            zassert_equal(join_results[i], BHT_OK,
                          "counter cycle %zu worker %zu join result %d", cycle,
                          i, join_results[i]);
            zassert_equal(workers[i].release_result, 0,
                          "counter cycle %zu worker %zu release result %d",
                          cycle, i, workers[i].release_result);
            zassert_equal(workers[i].lock_result, BHT_OK,
                          "counter cycle %zu worker %zu lock result %d", cycle,
                          i, workers[i].lock_result);
            zassert_equal(workers[i].unlock_result, BHT_OK,
                          "counter cycle %zu worker %zu unlock result %d",
                          cycle, i, workers[i].unlock_result);
        }
        zassert_equal(*counter, SYNC_STRESS_WORKERS * SYNC_STRESS_INCREMENTS,
                      "counter cycle %zu total result %d", cycle, *counter);
        zassert_equal(destroy_result, BHT_OK,
                      "counter cycle %zu destroy result %d", cycle,
                      destroy_result);
    }
}

static void
run_condition_release_cycles(bool broadcast)
{
    korp_mutex *mutex = &sync_results.stress_mutex;
    korp_cond *cond = &sync_results.stress_cond;
    atomic_t *generation = &sync_results.stress_generation;
    atomic_t *released = &sync_results.stress_released;
    const size_t waiter_count = broadcast ? 2U : 1U;
    int mutex_init_result;
    int cond_init_result;
    int cond_destroy_result = BHT_ERROR;
    int mutex_destroy_result = BHT_ERROR;
    bool cycles_valid = true;
    size_t failed_iteration = SYNC_STRESS_LIGHTWEIGHT_ITERATIONS;

    memset(mutex, 0, sizeof(*mutex));
    memset(cond, 0, sizeof(*cond));
    atomic_clear(generation);
    atomic_clear(released);
    mutex_init_result = os_mutex_init(mutex);
    cond_init_result = os_cond_init(cond);
    for (size_t iteration = 0; iteration < SYNC_STRESS_LIGHTWEIGHT_ITERATIONS;
         iteration++) {
        korp_tid threads[2] = { NULL, NULL };
        struct stress_condition_waiter *waiters =
            sync_results.stress_condition_waiters;
        int create_results[2] = { BHT_ERROR, BHT_ERROR };
        int ready_results[2] = { -EAGAIN, -EAGAIN };
        int first_done_results[2] = { -EAGAIN, -EAGAIN };
        int cleanup_done_results[2] = { -EAGAIN, -EAGAIN };
        int wait_results[2] = { BHT_ERROR, BHT_ERROR };
        int join_results[2] = { BHT_ERROR, BHT_ERROR };
        int lock_result = BHT_ERROR;
        int release_result = BHT_ERROR;
        int unlock_result = BHT_ERROR;
        int cleanup_lock_result = BHT_ERROR;
        int cleanup_broadcast_result = BHT_ERROR;
        int cleanup_unlock_result = BHT_ERROR;
        bool all_ready = true;
        bool iteration_failed = false;
        bool cleanup_needed = false;

        memset(waiters, 0, sizeof(sync_results.stress_condition_waiters));
        for (size_t i = 0; i < waiter_count; i++) {
            waiters[i].mutex = mutex;
            waiters[i].cond = cond;
            waiters[i].generation = generation;
            waiters[i].released = released;
            waiters[i].expected_generation = (int)iteration + 1;
            waiters[i].lock_result = BHT_ERROR;
            waiters[i].wait_result = BHT_ERROR;
            waiters[i].unlock_result = BHT_ERROR;
            if (mutex_init_result == BHT_OK && cond_init_result == BHT_OK) {
                create_results[i] = os_thread_create(
                    &threads[i], run_stress_condition_waiter, &waiters[i],
                    WAMR_TEST_THREAD_STACK_SIZE);
            }
            if (create_results[i] == BHT_OK) {
                ready_results[i] = k_sem_take(
                    &stress_condition_ready, K_MSEC(SYNC_STRESS_GUARD_MS));
                all_ready = all_ready && ready_results[i] == 0
                            && waiters[i].waiting;
            }
            else {
                all_ready = false;
            }
        }
        if (all_ready) {
            lock_result = os_mutex_lock(mutex);
            if (lock_result == BHT_OK) {
                atomic_set(generation, (atomic_val_t)iteration + 1);
                release_result = broadcast ? os_cond_broadcast(cond)
                                           : os_cond_signal(cond);
                unlock_result = os_mutex_unlock(mutex);
            }
        }
        for (size_t i = 0; i < waiter_count; i++) {
            if (create_results[i] == BHT_OK) {
                first_done_results[i] = k_sem_take(
                    &stress_condition_done, K_MSEC(SYNC_STRESS_GUARD_MS));
            }
        }
        for (size_t i = 0; i < waiter_count; i++) {
            if (create_results[i] == BHT_OK && first_done_results[i] != 0) {
                cleanup_needed = true;
            }
        }
        if (cleanup_needed) {
            cleanup_lock_result = os_mutex_lock(mutex);
            if (cleanup_lock_result == BHT_OK) {
                cleanup_broadcast_result = os_cond_broadcast(cond);
                cleanup_unlock_result = os_mutex_unlock(mutex);
            }
        }
        for (size_t i = 0; i < waiter_count; i++) {
            if (create_results[i] == BHT_OK) {
                if (first_done_results[i] != 0) {
                    cleanup_done_results[i] = k_sem_take(
                        &stress_condition_done, K_MSEC(SYNC_STRESS_GUARD_MS));
                }
                wait_results[i] = wamr_zephyr_thread_test_wait(
                    threads[i], K_MSEC(SYNC_STRESS_GUARD_MS));
                if (wait_results[i] == 0) {
                    join_results[i] = os_thread_join(threads[i], NULL);
                }
            }
        }
        if (!all_ready || lock_result != BHT_OK || release_result != BHT_OK
            || unlock_result != BHT_OK) {
            iteration_failed = true;
        }
        for (size_t i = 0; i < waiter_count; i++) {
            if (first_done_results[i] != 0
                || wait_results[i] != 0 || join_results[i] != BHT_OK
                || waiters[i].wait_result != BHT_OK || !waiters[i].woke
                || waiters[i].timed_out || !waiters[i].observed_signal
                || waiters[i].unlock_result != BHT_OK) {
                iteration_failed = true;
            }
        }
        if (atomic_get(released) != (atomic_val_t)((iteration + 1) * waiter_count)) {
            iteration_failed = true;
        }
        if (iteration_failed && !cleanup_needed) {
            cleanup_needed = true;
            cleanup_lock_result = os_mutex_lock(mutex);
            if (cleanup_lock_result == BHT_OK) {
                cleanup_broadcast_result = os_cond_broadcast(cond);
                cleanup_unlock_result = os_mutex_unlock(mutex);
            }
        }
        if (cleanup_needed && (cleanup_lock_result != BHT_OK
                               || cleanup_broadcast_result != BHT_OK
                               || cleanup_unlock_result != BHT_OK
                               || cleanup_done_results[0] != 0)) {
            iteration_failed = true;
        }
        if (iteration_failed) {
            cycles_valid = false;
            failed_iteration = iteration;
            break;
        }
    }
    if (cond_init_result == BHT_OK) {
        cond_destroy_result = os_cond_destroy(cond);
    }
    if (mutex_init_result == BHT_OK) {
        mutex_destroy_result = os_mutex_destroy(mutex);
    }
    zassert_equal(mutex_init_result, BHT_OK, "stress mutex init returned %d",
                  mutex_init_result);
    zassert_equal(cond_init_result, BHT_OK, "stress condition init returned %d",
                  cond_init_result);
    zassert_equal(cond_destroy_result, BHT_OK,
                  "stress condition destroy returned %d", cond_destroy_result);
    zassert_equal(mutex_destroy_result, BHT_OK,
                  "stress mutex destroy returned %d", mutex_destroy_result);
    zassert_true(cycles_valid, "%s cycle %zu failed after cleanup",
                 broadcast ? "broadcast" : "signal", failed_iteration);
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_signal_releases_each_waiter)
{
    run_condition_release_cycles(false);
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_broadcast_releases_all_waiters)
{
    run_condition_release_cycles(true);
}

WAMR_CONTEXT_TEST(platform_sync, test_condition_timeout_reacquires_mutex)
{
    korp_mutex mutex = { 0 };
    korp_cond cond = { 0 };
    int protected_iteration = 0;
    int mutex_init_result = os_mutex_init(&mutex);
    int cond_init_result = os_cond_init(&cond);
    int lock_results[SYNC_STRESS_LIGHTWEIGHT_ITERATIONS];
    int wait_results[SYNC_STRESS_LIGHTWEIGHT_ITERATIONS];
    int unlock_results[SYNC_STRESS_LIGHTWEIGHT_ITERATIONS];
    int protected_results[SYNC_STRESS_LIGHTWEIGHT_ITERATIONS];
    int cond_destroy_result = BHT_ERROR;
    int mutex_destroy_result = BHT_ERROR;

    for (size_t iteration = 0; iteration < SYNC_STRESS_LIGHTWEIGHT_ITERATIONS;
         iteration++) {
        int lock_result = BHT_ERROR;
        int wait_result = BHT_ERROR;
        int unlock_result = BHT_ERROR;

        if (mutex_init_result == BHT_OK && cond_init_result == BHT_OK) {
            lock_result = os_mutex_lock(&mutex);
        }
        if (lock_result == BHT_OK) {
            wait_result = os_cond_reltimedwait(&cond, &mutex, 1000U);
#if defined(CONFIG_USERSPACE)
            if (wait_result == ETIMEDOUT) {
#else
            if (wait_result == BHT_OK) {
#endif
                protected_iteration = (int)iteration + 1;
            }
            unlock_result = os_mutex_unlock(&mutex);
        }
        lock_results[iteration] = lock_result;
        wait_results[iteration] = wait_result;
        unlock_results[iteration] = unlock_result;
        protected_results[iteration] = protected_iteration;
    }
    if (cond_init_result == BHT_OK) {
        cond_destroy_result = os_cond_destroy(&cond);
    }
    if (mutex_init_result == BHT_OK) {
        mutex_destroy_result = os_mutex_destroy(&mutex);
    }
    zassert_equal(mutex_init_result, BHT_OK, "timeout mutex init returned %d",
                  mutex_init_result);
    zassert_equal(cond_init_result, BHT_OK, "timeout condition init returned %d",
                  cond_init_result);
    zassert_equal(cond_destroy_result, BHT_OK,
                  "timeout condition destroy returned %d", cond_destroy_result);
    zassert_equal(mutex_destroy_result, BHT_OK,
                  "timeout mutex destroy returned %d", mutex_destroy_result);
    for (size_t iteration = 0; iteration < SYNC_STRESS_LIGHTWEIGHT_ITERATIONS;
         iteration++) {
        zassert_equal(lock_results[iteration], BHT_OK,
                      "timeout iteration %zu lock %d", iteration,
                      lock_results[iteration]);
#if defined(CONFIG_USERSPACE)
        zassert_equal(wait_results[iteration], ETIMEDOUT,
                      "timeout iteration %zu wait %d", iteration,
                      wait_results[iteration]);
#else
        zassert_equal(wait_results[iteration], BHT_OK,
                      "timeout iteration %zu wait %d", iteration,
                      wait_results[iteration]);
#endif
        zassert_equal(protected_results[iteration], (int)iteration + 1,
                      "timeout iteration %zu lost mutex", iteration);
        zassert_equal(unlock_results[iteration], BHT_OK,
                      "timeout iteration %zu unlock %d", iteration,
                      unlock_results[iteration]);
    }
}

WAMR_CONTEXT_TEST(platform_sync, test_active_sync_destroy_recovers_each_cycle)
{
#if defined(CONFIG_USERSPACE)
    for (size_t iteration = 0; iteration < SYNC_STRESS_LIGHTWEIGHT_ITERATIONS;
         iteration++) {
        korp_mutex *mutex = &sync_results.stress_mutex;
        korp_cond *cond = &sync_results.stress_cond;
        korp_tid mutex_thread = NULL;
        korp_tid condition_thread = NULL;
        struct mutex_operation_context *operation =
            &sync_results.claimed_operation;
        struct stress_condition_waiter *waiter =
            &sync_results.stress_condition_waiters[0];
        atomic_t *generation = &sync_results.stress_generation;
        atomic_t *released = &sync_results.stress_released;
        int mutex_init;
        int mutex_create = BHT_ERROR;
        int claim_guard = -EAGAIN;
        int mutex_busy_destroy = BHT_ERROR;
        int mutex_exit_guard = BHT_ERROR;
        int mutex_join = BHT_ERROR;
        int mutex_destroy = BHT_ERROR;
        int condition_init = BHT_ERROR;
        int cond_create = BHT_ERROR;
        int ready_guard = -EAGAIN;
        int cond_busy_destroy = BHT_ERROR;
        int cond_signal = BHT_ERROR;
        int cond_unlock = BHT_ERROR;
        int done_guard = -EAGAIN;
        int cond_exit_guard = BHT_ERROR;
        int cond_join = BHT_ERROR;
        int cond_destroy = BHT_ERROR;

        memset(mutex, 0, sizeof(*mutex));
        memset(cond, 0, sizeof(*cond));
        memset(operation, 0, sizeof(*operation));
        memset(waiter, 0, sizeof(*waiter));
        operation->mutex = mutex;
        operation->lock_result = BHT_ERROR;
        operation->unlock_result = BHT_ERROR;
        waiter->mutex = mutex;
        waiter->cond = cond;
        waiter->lock_result = BHT_ERROR;
        waiter->wait_result = BHT_ERROR;
        waiter->unlock_result = BHT_ERROR;
        atomic_clear(generation);
        atomic_clear(released);
        mutex_init = os_mutex_init(mutex);
        sync_claim_hook.semaphore_mode = true;
        sync_claim_hook.release_result = -EAGAIN;
        atomic_set(&sync_claim_hook.armed, 1);
        if (mutex_init == BHT_OK) {
            sync_claim_hook.expected_handle = (uintptr_t)*mutex;
            mutex_create = os_thread_create(&mutex_thread,
                                            run_claimed_mutex_operation,
                                            operation,
                                            WAMR_TEST_THREAD_STACK_SIZE);
        }
        if (mutex_create == BHT_OK) {
            claim_guard = k_sem_take(&stress_mutex_claimed,
                                     K_MSEC(SYNC_STRESS_GUARD_MS));
            if (claim_guard == 0) {
                mutex_busy_destroy = os_mutex_destroy(mutex);
            }
            k_sem_give(&stress_mutex_release);
            mutex_exit_guard = wamr_zephyr_thread_test_wait(
                mutex_thread, K_MSEC(SYNC_STRESS_GUARD_MS));
            if (mutex_exit_guard == 0) {
                mutex_join = os_thread_join(mutex_thread, NULL);
            }
        }
        if (*mutex != NULL) {
            mutex_destroy = os_mutex_destroy(mutex);
        }

        condition_init = os_cond_init(cond);
        waiter->generation = generation;
        waiter->released = released;
        waiter->expected_generation = 1;
        if (mutex_init == BHT_OK && condition_init == BHT_OK && *mutex == NULL) {
            mutex_init = os_mutex_init(mutex);
        }
        if (mutex_init == BHT_OK && condition_init == BHT_OK) {
            cond_create = os_thread_create(&condition_thread,
                                           run_stress_condition_waiter, waiter,
                                           WAMR_TEST_THREAD_STACK_SIZE);
        }
        if (cond_create == BHT_OK) {
            ready_guard = k_sem_take(&stress_condition_ready,
                                     K_MSEC(SYNC_STRESS_GUARD_MS));
            if (ready_guard == 0) {
                int lock = os_mutex_lock(mutex);

                if (lock == BHT_OK) {
                    cond_busy_destroy = os_cond_destroy(cond);
                    atomic_set(generation, 1);
                    cond_signal = os_cond_signal(cond);
                    cond_unlock = os_mutex_unlock(mutex);
                }
            }
            done_guard = k_sem_take(&stress_condition_done,
                                     K_MSEC(SYNC_STRESS_GUARD_MS));
            cond_exit_guard = wamr_zephyr_thread_test_wait(
                condition_thread, K_MSEC(SYNC_STRESS_GUARD_MS));
            if (cond_exit_guard == 0) {
                cond_join = os_thread_join(condition_thread, NULL);
            }
        }
        if (*cond != NULL) {
            cond_destroy = os_cond_destroy(cond);
        }
        if (*mutex != NULL) {
            mutex_destroy = os_mutex_destroy(mutex);
        }
        sync_claim_hook.semaphore_mode = false;

        zassert_equal(mutex_init, BHT_OK, "active iteration %zu mutex init %d",
                      iteration, mutex_init);
        zassert_equal(mutex_create, BHT_OK,
                      "active iteration %zu mutex worker create %d", iteration,
                      mutex_create);
        zassert_equal(claim_guard, 0,
                      "active iteration %zu mutex claim guard %d", iteration,
                      claim_guard);
        zassert_equal(mutex_busy_destroy, BHT_ERROR,
                      "active iteration %zu destroyed claimed mutex", iteration);
        zassert_equal(sync_claim_hook.release_result, 0,
                      "active iteration %zu mutex release guard %d", iteration,
                      sync_claim_hook.release_result);
        zassert_equal(mutex_exit_guard, 0,
                      "active iteration %zu mutex exit guard %d", iteration,
                      mutex_exit_guard);
        zassert_equal(mutex_join, BHT_OK,
                      "active iteration %zu mutex join %d", iteration,
                      mutex_join);
        zassert_equal(operation->lock_result, BHT_OK,
                      "active iteration %zu mutex lock %d", iteration,
                      operation->lock_result);
        zassert_equal(operation->unlock_result, BHT_OK,
                      "active iteration %zu mutex unlock %d", iteration,
                      operation->unlock_result);
        zassert_equal(condition_init, BHT_OK,
                      "active iteration %zu condition init %d", iteration,
                      condition_init);
        zassert_equal(cond_create, BHT_OK,
                      "active iteration %zu condition worker create %d",
                      iteration, cond_create);
        zassert_equal(ready_guard, 0,
                      "active iteration %zu condition ready guard %d",
                      iteration, ready_guard);
        zassert_equal(cond_busy_destroy, BHT_ERROR,
                      "active iteration %zu destroyed waiting condition",
                      iteration);
        zassert_equal(cond_signal, BHT_OK,
                      "active iteration %zu condition signal %d", iteration,
                      cond_signal);
        zassert_equal(cond_unlock, BHT_OK,
                      "active iteration %zu condition unlock %d", iteration,
                      cond_unlock);
        zassert_equal(done_guard, 0,
                      "active iteration %zu condition done guard %d", iteration,
                      done_guard);
        zassert_equal(cond_exit_guard, 0,
                      "active iteration %zu condition exit guard %d", iteration,
                      cond_exit_guard);
        zassert_equal(cond_join, BHT_OK,
                      "active iteration %zu condition join %d", iteration,
                      cond_join);
        zassert_equal(waiter->wait_result, BHT_OK,
                      "active iteration %zu condition wait %d", iteration,
                      waiter->wait_result);
        zassert_equal(waiter->unlock_result, BHT_OK,
                      "active iteration %zu condition waiter unlock %d",
                      iteration, waiter->unlock_result);
        zassert_true(waiter->woke,
                     "active iteration %zu condition waiter did not wake",
                     iteration);
        zassert_equal(cond_destroy, BHT_OK,
                      "active iteration %zu condition destroy %d", iteration,
                      cond_destroy);
        zassert_equal(mutex_destroy, BHT_OK,
                      "active iteration %zu mutex destroy %d", iteration,
                      mutex_destroy);
    }
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync_pool,
                  test_sync_pools_exhaust_and_recover_each_cycle)
{
#if defined(CONFIG_USERSPACE)
    for (size_t cycle = 0; cycle < SYNC_STRESS_POOL_ITERATIONS; cycle++) {
        korp_mutex mutexes[TEST_MUTEX_POOL_COUNT + 1] = { NULL };
        korp_cond conditions[TEST_COND_POOL_COUNT + 1] = { NULL };
        korp_cond independent_cond = NULL;
        korp_mutex independent_mutex = NULL;
        korp_mutex recovered_mutex = NULL;
        korp_cond recovered_cond = NULL;
        int mutex_results[TEST_MUTEX_POOL_COUNT + 1];
        int cond_results[TEST_COND_POOL_COUNT + 1];
        int independent_cond_result;
        int independent_mutex_result;
        int recovered_mutex_result;
        int recovered_cond_result;
        bool cleanup_ok = true;

        for (size_t i = 0; i < ARRAY_SIZE(mutexes); i++) {
            mutex_results[i] = os_mutex_init(&mutexes[i]);
        }
        independent_cond_result = os_cond_init(&independent_cond);
        if (independent_cond != NULL
            && os_cond_destroy(&independent_cond) != BHT_OK) {
            cleanup_ok = false;
        }
        for (size_t i = 0; i < ARRAY_SIZE(mutexes); i++) {
            if (mutexes[i] != NULL && os_mutex_destroy(&mutexes[i]) != BHT_OK) {
                cleanup_ok = false;
            }
        }
        recovered_mutex_result = os_mutex_init(&recovered_mutex);
        if (recovered_mutex != NULL
            && os_mutex_destroy(&recovered_mutex) != BHT_OK) {
            cleanup_ok = false;
        }
        for (size_t i = 0; i < ARRAY_SIZE(conditions); i++) {
            cond_results[i] = os_cond_init(&conditions[i]);
        }
        independent_mutex_result = os_mutex_init(&independent_mutex);
        if (independent_mutex != NULL
            && os_mutex_destroy(&independent_mutex) != BHT_OK) {
            cleanup_ok = false;
        }
        for (size_t i = 0; i < ARRAY_SIZE(conditions); i++) {
            if (conditions[i] != NULL
                && os_cond_destroy(&conditions[i]) != BHT_OK) {
                cleanup_ok = false;
            }
        }
        recovered_cond_result = os_cond_init(&recovered_cond);
        if (recovered_cond != NULL
            && os_cond_destroy(&recovered_cond) != BHT_OK) {
            cleanup_ok = false;
        }

        for (size_t i = 0; i < TEST_MUTEX_POOL_COUNT; i++) {
            zassert_equal(mutex_results[i], BHT_OK,
                          "pool cycle %zu mutex slot %zu init %d", cycle, i,
                          mutex_results[i]);
        }
        zassert_equal(mutex_results[TEST_MUTEX_POOL_COUNT], BHT_ERROR,
                      "pool cycle %zu allocated one mutex too many", cycle);
        zassert_equal(independent_cond_result, BHT_OK,
                      "pool cycle %zu mutex exhaustion used condition slot",
                      cycle);
        zassert_equal(recovered_mutex_result, BHT_OK,
                      "pool cycle %zu mutex pool did not recover", cycle);
        for (size_t i = 0; i < TEST_COND_POOL_COUNT; i++) {
            zassert_equal(cond_results[i], BHT_OK,
                          "pool cycle %zu condition slot %zu init %d", cycle,
                          i, cond_results[i]);
        }
        zassert_equal(cond_results[TEST_COND_POOL_COUNT], BHT_ERROR,
                      "pool cycle %zu allocated one condition too many", cycle);
        zassert_equal(independent_mutex_result, BHT_OK,
                      "pool cycle %zu condition exhaustion used mutex slot",
                      cycle);
        zassert_equal(recovered_cond_result, BHT_OK,
                      "pool cycle %zu condition pool did not recover", cycle);
        zassert_true(cleanup_ok, "pool cycle %zu cleanup failed", cycle);
    }
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_sync, test_named_semaphore_api_reports_unsupported)
{
    zassert_is_null(os_sem_open("wamr", 0, 0, 1), NULL);
    zassert_equal(os_sem_close(NULL), BHT_ERROR, NULL);
    zassert_equal(os_sem_wait(NULL), BHT_ERROR, NULL);
    zassert_equal(os_sem_trywait(NULL), BHT_ERROR, NULL);
    zassert_equal(os_sem_post(NULL), BHT_ERROR, NULL);
    zassert_equal(os_sem_getvalue(NULL, NULL), BHT_ERROR, NULL);
    zassert_equal(os_sem_unlink("wamr"), BHT_ERROR, NULL);
}
