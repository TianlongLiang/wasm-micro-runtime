/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define WAMR_TEST_POOL_STORAGE ZTEST_BMEM
#include "test_common.h"
#undef WAMR_TEST_POOL_STORAGE

#include "platform_api_extension.h"
#include "platform_api_vmcore.h"
#include "zephyr_thread_pool.h"

#define WAMR_TEST_STACK_SIZE 2048U
#define THREAD_GUARD_TIMEOUT_MS 500
#define THREAD_LIFECYCLE_ITERATIONS 16U
#define THREAD_SATURATION_ITERATIONS 8U
#define THREAD_OWNERSHIP_ITERATIONS 8U
#define THREAD_NESTED_ITERATIONS 8U
#define THREAD_POOL_CAPACITY BH_ZEPHYR_MPU_STACK_COUNT
#define MAX_BLOCKED_THREADS (BH_ZEPHYR_MPU_STACK_COUNT + 1U)
#define MAX_OWNED_THREADS (BH_ZEPHYR_MPU_STACK_COUNT + 3U)
#define NESTED_GRANDCHILD_RESULT ((void *)0x2468U)
#define NESTED_CHILD_RESULT ((void *)0x1357U)
#define EXPLICIT_EXIT_RESULT ((void *)0x369CU)

BUILD_ASSERT(THREAD_POOL_CAPACITY == 4U,
             "thread stress tests require the four-slot fixture pool");

BUILD_ASSERT(!__builtin_types_compatible_p(korp_tid, k_tid_t),
             "WAMR thread handles must be opaque to Zephyr");

enum {
    WAMR_JOIN_TEST_BEFORE_CLAIM,
    WAMR_JOIN_TEST_UNPROTECTED_LOOKUP,
    WAMR_JOIN_TEST_PROTECTED_LOOKUP,
    WAMR_JOIN_TEST_CLAIMED,
    WAMR_JOIN_TEST_BEFORE_REMOVE,
    WAMR_JOIN_TEST_ENTERED,
};

enum {
    WAMR_DETACH_TEST_EXITED_CLAIMED,
    WAMR_DETACH_TEST_COMPLETION_ENQUEUED,
};

void
wamr_thread_test_before(void *fixture);

int
wamr_zephyr_thread_test_wait(korp_tid handle, k_timeout_t timeout);
int
wamr_zephyr_thread_test_lifecycle_lock(korp_tid handle);
void
os_thread_sys_destroy(void);

struct thread_result {
    int input;
    int output;
    unsigned int writes;
};

struct identity_state {
    korp_tid handles[2];
};

struct identity_arg {
    struct identity_state *state;
    unsigned int slot;
};

struct blocked_thread_state {
    atomic_t ready;
    atomic_t release;
    atomic_t exited;
};

struct phase_worker_state {
    void *return_value;
    int release_result;
};

struct nested_thread_state {
    int create_result;
    int join_result;
    int child_release_result;
    int grandchild_release_result;
    void *grandchild_result;
};

struct concurrent_join_state {
    atomic_t hook_active;
    atomic_t before_claim_arrivals;
    atomic_t unprotected_lookup_arrivals;
    atomic_t protected_lookup_arrivals;
    atomic_t cleanup_started;
    atomic_t winner_done;
    atomic_t replacement_pending;
    korp_tid target;
    korp_tid replacement;
    int join_results[2];
    int replacement_create_result;
};

struct concurrent_join_arg {
    struct concurrent_join_state *state;
    unsigned int slot;
};

struct join_detach_race_state {
    atomic_t hook_active;
    korp_tid target;
    int hook_release_result;
    int join_result;
};

struct detached_join_state {
    korp_tid target;
    int result;
};

struct detach_after_exit_race_state {
    atomic_t hook_active;
    korp_tid target;
    int hook_release_result;
    int detach_result;
};

struct detached_reuse_race_state {
    atomic_t hook_active;
    struct phase_worker_state replacement_worker;
    korp_tid target;
    korp_tid replacement;
    int hook_release_result;
    int join_start_result;
    int creator_start_result;
    int join_result;
    int replacement_create_result;
};

struct explicit_exit_state {
    atomic_t exit_started;
    atomic_t exit_returned;
    int detach_result;
};

struct reaper_worker_state {
    struct k_sem *release;
    int release_result;
};

struct out_of_order_reaper_state {
    atomic_t hook_active;
    int hook_release_result;
    struct reaper_worker_state workers[THREAD_POOL_CAPACITY];
};

struct platform_thread_fixture {
    struct thread_result result;
    struct thread_result results[4];
    struct identity_state identities;
    struct identity_arg identity_args[2];
    struct blocked_thread_state blocked;
    struct phase_worker_state lifecycle;
    struct phase_worker_state saturation_workers[THREAD_POOL_CAPACITY];
    struct nested_thread_state nested;
    struct concurrent_join_state concurrent_join;
    struct concurrent_join_arg concurrent_join_args[2];
    struct join_detach_race_state join_detach_race;
    struct detached_join_state detached_join;
    struct detach_after_exit_race_state detach_after_exit_race;
    struct detached_reuse_race_state detached_reuse_race;
    struct explicit_exit_state explicit_exit;
    struct out_of_order_reaper_state out_of_order_reaper;
    korp_tid owned_threads[MAX_OWNED_THREADS];
    atomic_t child_exited;
    bool runtime_destroyed;
};

ZTEST_DMEM static struct platform_thread_fixture thread_fixture;
static struct k_sem phase_ready;
static struct k_sem phase_release;
static struct k_sem phase_done;
static struct k_sem join_claimed;
static struct k_sem join_release;
static struct k_sem detach_claimed;
static struct k_sem detach_release;
static struct k_sem reaper_completion_enqueued;
static struct k_sem reaper_completion_release;
static struct k_sem reaper_worker_release[THREAD_POOL_CAPACITY];
static struct k_sem detached_join_done;
static struct k_sem reuse_join_start;
static struct k_sem reuse_join_paused;
static struct k_sem reuse_join_release;
static struct k_sem reuse_join_done;
static struct k_sem reuse_creator_start;
static struct k_sem reuse_creator_started;
static struct k_sem reuse_creator_done;
static struct k_sem nested_child_ready;
static struct k_sem nested_child_release;
static struct k_sem nested_child_done;
static struct k_sem nested_grandchild_ready;
static struct k_sem nested_grandchild_release;
static struct k_sem nested_grandchild_done;

static void
own_thread(korp_tid thread);

static void *
write_result(void *arg)
{
    struct thread_result *result = arg;

    result->output = result->input * 2;
    result->writes++;
    return NULL;
}

static void *
record_identity(void *arg)
{
    struct identity_arg *identity_arg = arg;
    struct identity_state *state = identity_arg->state;

    state->handles[identity_arg->slot] = os_self_thread();
    return NULL;
}

static void *
block_for_stack_recovery(void *arg)
{
    struct blocked_thread_state *state = arg;

    atomic_inc(&state->ready);
    while (!atomic_get(&state->release)) {
        k_sleep(K_MSEC(1));
    }
    atomic_inc(&state->exited);
    return NULL;
}

static void *
publish_ready_and_return(void *arg)
{
    struct phase_worker_state *state = arg;

    k_sem_give(&phase_ready);
    state->release_result =
        k_sem_take(&phase_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    if (state->release_result != 0) {
        return NULL;
    }
    k_sem_give(&phase_done);
    return state->return_value;
}

static void *
return_nested_grandchild_result(void *arg)
{
    struct nested_thread_state *state = arg;

    k_sem_give(&nested_grandchild_ready);
    state->grandchild_release_result = k_sem_take(
        &nested_grandchild_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    if (state->grandchild_release_result != 0) {
        return NULL;
    }
    k_sem_give(&nested_grandchild_done);
    return NESTED_GRANDCHILD_RESULT;
}

static void *
wait_for_reaper_worker_release(void *arg)
{
    struct reaper_worker_state *state = arg;

    k_sem_give(&phase_ready);
    state->release_result =
        k_sem_take(state->release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    return NULL;
}

static void *
create_and_join_grandchild(void *arg)
{
    struct nested_thread_state *state = arg;
    korp_tid grandchild;

    k_sem_give(&nested_child_ready);
    state->child_release_result =
        k_sem_take(&nested_child_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    if (state->child_release_result != 0) {
        return NULL;
    }
    state->create_result =
        os_thread_create(&grandchild, return_nested_grandchild_result, state,
                         WAMR_TEST_STACK_SIZE);
    if (state->create_result != BHT_OK) {
        return NULL;
    }
    state->join_result = os_thread_join(grandchild, &state->grandchild_result);
    k_sem_give(&nested_child_done);
    return state->join_result == BHT_OK
                   && state->grandchild_result == NESTED_GRANDCHILD_RESULT
               ? NESTED_CHILD_RESULT
               : NULL;
}

static void *
publish_exit(void *arg)
{
    atomic_set(arg, 1);
    return NULL;
}

static void *
return_argument(void *arg)
{
    return arg;
}

void
wamr_zephyr_thread_join_test_hook(int phase)
{
    struct join_detach_race_state *join_detach_state =
        &thread_fixture.join_detach_race;
    struct detached_reuse_race_state *reuse_state =
        &thread_fixture.detached_reuse_race;
    struct concurrent_join_state *state = &thread_fixture.concurrent_join;

    if (atomic_get(&reuse_state->hook_active)
        && phase == WAMR_JOIN_TEST_ENTERED) {
        k_sem_give(&reuse_join_paused);
        reuse_state->hook_release_result = k_sem_take(
            &reuse_join_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        return;
    }

    if (atomic_get(&join_detach_state->hook_active)
        && phase == WAMR_JOIN_TEST_CLAIMED) {
        k_sem_give(&join_claimed);
        join_detach_state->hook_release_result =
            k_sem_take(&join_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        return;
    }

    if (!atomic_get(&state->hook_active)) {
        return;
    }

    if (phase == WAMR_JOIN_TEST_BEFORE_CLAIM) {
        atomic_inc(&state->before_claim_arrivals);
        while (atomic_get(&state->before_claim_arrivals) < 2) {
            k_yield();
        }
    }
    else if (phase == WAMR_JOIN_TEST_UNPROTECTED_LOOKUP) {
        if (atomic_inc(&state->unprotected_lookup_arrivals) == 0) {
            while (!atomic_get(&state->winner_done)) {
                k_yield();
            }
        }
    }
    else if (phase == WAMR_JOIN_TEST_PROTECTED_LOOKUP) {
        if (atomic_inc(&state->protected_lookup_arrivals) != 0) {
            while (!atomic_get(&state->cleanup_started)) {
                k_yield();
            }
        }
    }
    else if (phase == WAMR_JOIN_TEST_CLAIMED) {
        while (atomic_get(&state->protected_lookup_arrivals) < 2) {
            k_yield();
        }
    }
    else if (phase == WAMR_JOIN_TEST_BEFORE_REMOVE) {
        atomic_set(&state->cleanup_started, 1);
    }
}

void
wamr_zephyr_thread_detach_test_hook(int phase)
{
    struct out_of_order_reaper_state *reaper =
        &thread_fixture.out_of_order_reaper;
    struct detach_after_exit_race_state *state =
        &thread_fixture.detach_after_exit_race;

    if (atomic_get(&reaper->hook_active)
        && phase == WAMR_DETACH_TEST_COMPLETION_ENQUEUED) {
        k_sem_give(&reaper_completion_enqueued);
        reaper->hook_release_result = k_sem_take(
            &reaper_completion_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        return;
    }

    if (!atomic_get(&state->hook_active)
        || phase != WAMR_DETACH_TEST_EXITED_CLAIMED) {
        return;
    }

    k_sem_give(&detach_claimed);
    state->hook_release_result =
        k_sem_take(&detach_release, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
}

static void *
join_target_concurrently(void *arg)
{
    struct concurrent_join_arg *join_arg = arg;
    struct concurrent_join_state *state = join_arg->state;
    int result = os_thread_join(state->target, NULL);

    state->join_results[join_arg->slot] = result;
    if (result == BHT_OK && atomic_cas(&state->replacement_pending, 1, 0)) {
        state->replacement_create_result = os_thread_create(
            &state->replacement, return_argument, NULL, WAMR_TEST_STACK_SIZE);
        if (state->replacement_create_result == BHT_OK) {
            (void)wamr_zephyr_thread_test_wait(
                state->replacement, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        }
        atomic_set(&state->winner_done, 1);
    }

    return NULL;
}

static void *
join_detached_target(void *arg)
{
    struct detached_join_state *state = arg;

    state->result = os_thread_join(state->target, NULL);
    k_sem_give(&detached_join_done);
    return NULL;
}

static void *
join_before_competing_detach(void *arg)
{
    struct join_detach_race_state *state = arg;

    state->join_result = os_thread_join(state->target, NULL);
    return NULL;
}

static void *
detach_exited_target(void *arg)
{
    struct detach_after_exit_race_state *state = arg;

    state->detach_result = os_thread_detach(state->target);
    return NULL;
}

static void *
join_detached_target_during_reuse(void *arg)
{
    struct detached_reuse_race_state *state = arg;

    state->join_start_result =
        k_sem_take(&reuse_join_start, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    if (state->join_start_result != 0) {
        k_sem_give(&reuse_join_done);
        return NULL;
    }
    state->join_result = os_thread_join(state->target, NULL);
    k_sem_give(&reuse_join_done);
    return NULL;
}

static void *
create_replacement_during_join(void *arg)
{
    struct detached_reuse_race_state *state = arg;

    state->creator_start_result =
        k_sem_take(&reuse_creator_start, K_MSEC(THREAD_GUARD_TIMEOUT_MS));
    if (state->creator_start_result != 0) {
        k_sem_give(&reuse_creator_done);
        return NULL;
    }
    k_sem_give(&reuse_creator_started);
    state->replacement_create_result =
        os_thread_create(&state->replacement, publish_ready_and_return,
                         &state->replacement_worker, WAMR_TEST_STACK_SIZE);
    k_sem_give(&reuse_creator_done);
    return NULL;
}

static void *
detach_and_exit_explicitly(void *arg)
{
    struct explicit_exit_state *state = arg;

    state->detach_result = os_thread_detach(os_self_thread());
    atomic_set(&state->exit_started, 1);
    os_thread_exit(EXPLICIT_EXIT_RESULT);
    atomic_set(&state->exit_returned, 1);
    return NULL;
}

static void
reset_result(struct thread_result *result, int input)
{
    memset(result, 0, sizeof(*result));
    result->input = input;
}

static bool
wait_for_atomic_count(atomic_t *counter, size_t expected)
{
    int64_t deadline = k_uptime_get() + THREAD_GUARD_TIMEOUT_MS;

    while ((size_t)atomic_get(counter) < expected
           && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }

    return (size_t)atomic_get(counter) >= expected;
}

static void
own_thread(korp_tid thread)
{
    for (size_t i = 0; i < ARRAY_SIZE(thread_fixture.owned_threads); ++i) {
        if (thread_fixture.owned_threads[i] == NULL) {
            thread_fixture.owned_threads[i] = thread;
            return;
        }
    }

    zassert_unreachable("thread fixture ownership capacity exceeded");
}

static void
disown_thread(korp_tid thread)
{
    for (size_t i = 0; i < ARRAY_SIZE(thread_fixture.owned_threads); ++i) {
        if (thread_fixture.owned_threads[i] == thread) {
            thread_fixture.owned_threads[i] = NULL;
            return;
        }
    }
}

static void
init_phase_sem(struct k_sem *sem)
{
    k_sem_init(sem, 0, MAX_OWNED_THREADS);
#if defined(CONFIG_USERSPACE)
    k_object_access_grant(sem, k_current_get());
#endif
}

static void
thread_before(void *fixture)
{
    memset(&thread_fixture, 0, sizeof(thread_fixture));
    init_phase_sem(&phase_ready);
    init_phase_sem(&phase_release);
    init_phase_sem(&phase_done);
    init_phase_sem(&join_claimed);
    init_phase_sem(&join_release);
    init_phase_sem(&detach_claimed);
    init_phase_sem(&detach_release);
    init_phase_sem(&reaper_completion_enqueued);
    init_phase_sem(&reaper_completion_release);
    for (size_t i = 0; i < ARRAY_SIZE(reaper_worker_release); ++i) {
        init_phase_sem(&reaper_worker_release[i]);
    }
    init_phase_sem(&detached_join_done);
    init_phase_sem(&reuse_join_start);
    init_phase_sem(&reuse_join_paused);
    init_phase_sem(&reuse_join_release);
    init_phase_sem(&reuse_join_done);
    init_phase_sem(&reuse_creator_start);
    init_phase_sem(&reuse_creator_started);
    init_phase_sem(&reuse_creator_done);
    init_phase_sem(&nested_child_ready);
    init_phase_sem(&nested_child_release);
    init_phase_sem(&nested_child_done);
    init_phase_sem(&nested_grandchild_ready);
    init_phase_sem(&nested_grandchild_release);
    init_phase_sem(&nested_grandchild_done);
    wamr_thread_test_before(fixture);
}

static void
thread_after(void *fixture)
{
    atomic_set(&thread_fixture.blocked.release, 1);
    for (size_t i = 0; i < MAX_OWNED_THREADS; ++i) {
        k_sem_give(&phase_release);
        k_sem_give(&join_release);
        k_sem_give(&detach_release);
        k_sem_give(&reaper_completion_enqueued);
        k_sem_give(&reaper_completion_release);
        k_sem_give(&reuse_join_start);
        k_sem_give(&reuse_join_release);
        k_sem_give(&reuse_creator_start);
        k_sem_give(&nested_child_release);
        k_sem_give(&nested_grandchild_release);
    }
    for (size_t i = 0; i < ARRAY_SIZE(reaper_worker_release); ++i) {
        k_sem_give(&reaper_worker_release[i]);
    }
    atomic_set(&thread_fixture.concurrent_join.before_claim_arrivals, 2);
    atomic_set(&thread_fixture.concurrent_join.protected_lookup_arrivals, 2);
    atomic_set(&thread_fixture.concurrent_join.cleanup_started, 1);
    atomic_set(&thread_fixture.concurrent_join.winner_done, 1);

    for (size_t i = 0; i < ARRAY_SIZE(thread_fixture.owned_threads); ++i) {
        korp_tid thread = thread_fixture.owned_threads[i];

        if (thread == NULL) {
            continue;
        }
        if (wamr_zephyr_thread_test_wait(
                thread, K_MSEC(THREAD_GUARD_TIMEOUT_MS)) == 0) {
            (void)os_thread_join(thread, NULL);
        }
        else {
            (void)os_thread_detach(thread);
        }
        thread_fixture.owned_threads[i] = NULL;
    }

    if (thread_fixture.concurrent_join.replacement != NULL
        && thread_fixture.concurrent_join.replacement_create_result == BHT_OK) {
        (void)wamr_zephyr_thread_test_wait(
            thread_fixture.concurrent_join.replacement,
            K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        (void)os_thread_join(thread_fixture.concurrent_join.replacement, NULL);
    }
    if (thread_fixture.detached_reuse_race.replacement != NULL
        && thread_fixture.detached_reuse_race.replacement_create_result
               == BHT_OK) {
        (void)wamr_zephyr_thread_test_wait(
            thread_fixture.detached_reuse_race.replacement,
            K_MSEC(THREAD_GUARD_TIMEOUT_MS));
        (void)os_thread_join(thread_fixture.detached_reuse_race.replacement,
                             NULL);
    }

    if (!thread_fixture.runtime_destroyed) {
        pool_after(fixture);
    }
    zassert_is_null(os_self_thread(),
                    "thread-system mapping survived clean runtime teardown");
}

ZTEST_SUITE(platform_thread, NULL, NULL, thread_before, thread_after,
            NULL);

WAMR_CONTEXT_TEST(platform_thread,
                  test_invalid_and_stale_detach_preserve_lifecycle)
{
    korp_tid stale;
    korp_tid replacement;
    int stale_join_result;
    int replacement_join_result;

    zassert_equal(os_thread_detach(NULL), BHT_ERROR,
                  "null detach handle was accepted");
    zassert_equal(os_thread_create(&stale, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE), BHT_OK,
                  "stale-handle source creation failed");
    own_thread(stale);
    stale_join_result = os_thread_join(stale, NULL);
    if (stale_join_result == BHT_OK) {
        disown_thread(stale);
    }
    zassert_equal(stale_join_result, BHT_OK,
                  "stale-handle source join failed");
    zassert_equal(os_thread_detach(stale), BHT_ERROR,
                  "joined stale handle was detached");
    zassert_equal(os_thread_create(&replacement, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE), BHT_OK,
                  "detach rejection damaged thread lifecycle");
    own_thread(replacement);
    replacement_join_result = os_thread_join(replacement, NULL);
    if (replacement_join_result == BHT_OK) {
        disown_thread(replacement);
    }
    zassert_equal(replacement_join_result, BHT_OK,
                  "replacement join failed");
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_active_worker_defers_thread_system_teardown)
{
    struct phase_worker_state *state = &thread_fixture.lifecycle;
    RuntimeInitArgs args = { 0 };
    korp_tid initial_identity = os_self_thread();
    korp_tid worker;

    memset(state, 0, sizeof(*state));
    state->release_result = -EAGAIN;
    zassert_equal(os_thread_create(&worker, publish_ready_and_return, state,
                                   WAMR_TEST_STACK_SIZE), BHT_OK,
                  "active worker creation failed");
    own_thread(worker);
    zassert_equal(k_sem_take(&phase_ready,
                             K_MSEC(THREAD_GUARD_TIMEOUT_MS)), 0,
                  "active worker did not reach the barrier");

    os_thread_sys_destroy();
    zassert_equal_ptr(os_self_thread(), initial_identity,
                      "active worker allowed thread-system teardown");

    k_sem_give(&phase_release);
    zassert_equal(k_sem_take(&phase_done,
                             K_MSEC(THREAD_GUARD_TIMEOUT_MS)), 0,
                  "active worker did not finish");
    zassert_equal(os_thread_join(worker, NULL), BHT_OK,
                  "active worker join failed");
    disown_thread(worker);

    thread_fixture.runtime_destroyed = true;
    wasm_runtime_destroy();
    zassert_is_null(os_self_thread(),
                    "clean runtime teardown retained thread mapping");

    memset(test_pool, 0xA5, sizeof(test_pool));
    args.mem_alloc_type = Alloc_With_Pool;
    args.mem_alloc_option.pool.heap_buf = test_pool;
    args.mem_alloc_option.pool.heap_size = sizeof(test_pool);
    zassert_true(wasm_runtime_full_init(&args),
                 "runtime reinitialization after deferred teardown failed");
    thread_fixture.runtime_destroyed = false;
    zassert_not_null(os_self_thread(),
                     "reinitialized runtime has no caller mapping");
    wasm_runtime_destroy();
    thread_fixture.runtime_destroyed = true;
    zassert_is_null(os_self_thread(),
                    "reinitialized runtime did not tear down cleanly");
}

/* Catches a regression where stack metadata is ignored or exposed incorrectly. */
ZTEST(platform_thread, test_stack_boundary_matches_configuration)
{
#if defined(CONFIG_THREAD_STACK_INFO) && !defined(CONFIG_USERSPACE)
    zassert_equal(os_thread_get_stack_boundary(),
                  (uint8_t *)k_current_get()->stack_info.start,
                  "stack boundary does not match Zephyr metadata");
#else
    zassert_is_null(os_thread_get_stack_boundary(),
                    "stack boundary exists without stack metadata");
#endif
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_thread_pool_prepare_rejects_user_context_and_recovers)
{
#if defined(CONFIG_WAMR_TEST_USER_MODE)
    korp_tid worker;
    int join_result;

    zassert_equal(wamr_test_thread_pool_prepare(), BHT_ERROR,
                  "user context prepared the thread pool");
    zassert_equal(os_thread_create(&worker, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "prepared pool was damaged by rejected prepare");
    own_thread(worker);
    join_result = os_thread_join(worker, NULL);
    if (join_result == BHT_OK) {
        disown_thread(worker);
    }
    zassert_equal(join_result, BHT_OK,
                  "worker recovery after rejected prepare failed");
#else
    ztest_test_skip();
#endif
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_platform_lifecycle_exposes_current_thread)
{
    zassert_not_null(os_self_thread(), "current thread is unavailable");
}

WAMR_CONTEXT_TEST(platform_thread, test_create_and_join)
{
    korp_tid thread;

    reset_result(&thread_fixture.result, 1);
    zassert_equal(os_thread_create(&thread, write_result,
                                   &thread_fixture.result,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    zassert_equal(os_thread_join(thread, NULL), BHT_OK, "thread join failed");
    zassert_equal(thread_fixture.result.writes, 1U,
                  "thread did not run exactly once");
}

WAMR_CONTEXT_TEST(platform_thread, test_argument_reaches_thread)
{
    korp_tid thread;

    reset_result(&thread_fixture.result, 21);
    zassert_equal(os_thread_create(&thread, write_result,
                                   &thread_fixture.result,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    zassert_equal(os_thread_join(thread, NULL), BHT_OK, "thread join failed");
    zassert_equal(thread_fixture.result.output, 42,
                  "thread did not receive its argument");
}

WAMR_CONTEXT_TEST(platform_thread, test_join_after_child_exit)
{
    korp_tid thread;

    atomic_clear(&thread_fixture.child_exited);
    zassert_equal(os_thread_create(&thread, publish_exit,
                                   &thread_fixture.child_exited,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    zassert_equal(wamr_zephyr_thread_test_wait(thread, K_FOREVER), 0,
                  "child did not exit before WAMR join");
    zassert_equal(atomic_get(&thread_fixture.child_exited), 1,
                  "child did not publish its exit");
    zassert_equal(os_thread_join(thread, NULL), BHT_OK,
                  "join after child exit failed");
}

WAMR_CONTEXT_TEST(platform_thread, test_join_propagates_return_value)
{
    void *expected = (void *)0x1234U;
    void *actual = NULL;
    korp_tid thread;

    zassert_equal(os_thread_create(&thread, return_argument, expected,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "thread creation failed");
    zassert_equal(os_thread_join(thread, &actual), BHT_OK,
                  "thread join failed");
    zassert_equal_ptr(actual, expected, "thread return value was lost");
}

/* Mutation caught: join omits the return value or leaves its slot generation
 * reusable through a stale handle. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_create_join_reuse_repeats_and_rejects_stale_handle)
{
    struct phase_worker_state *state = &thread_fixture.lifecycle;

    for (size_t iteration = 0; iteration < THREAD_LIFECYCLE_ITERATIONS;
         ++iteration) {
        korp_tid thread;
        void *actual = NULL;
        void *expected = (void *)(uintptr_t)(iteration + 1U);
        int create_result;
        int join_result;
        int stale_join_result;

        memset(state, 0, sizeof(*state));
        state->return_value = expected;
        state->release_result = -EAGAIN;
        create_result = os_thread_create(&thread, publish_ready_and_return,
                                         state, WAMR_TEST_STACK_SIZE);
        if (create_result == BHT_OK) {
            own_thread(thread);
        }
        zassert_equal(create_result, BHT_OK,
                      "iteration %zu create returned %d", iteration,
                      create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu worker did not publish ready",
                      iteration);

        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu worker did not publish done",
                      iteration);
        zassert_equal(state->release_result, 0,
                      "iteration %zu worker release returned %d", iteration,
                      state->release_result);
        join_result = os_thread_join(thread, &actual);
        if (join_result == BHT_OK) {
            disown_thread(thread);
        }
        zassert_equal(join_result, BHT_OK,
                      "iteration %zu join returned %d", iteration,
                      join_result);
        zassert_equal_ptr(actual, expected,
                          "iteration %zu join returned value %p", iteration,
                          actual);

        stale_join_result = os_thread_join(thread, NULL);
        zassert_equal(stale_join_result, BHT_ERROR,
                      "iteration %zu stale join returned %d", iteration,
                      stale_join_result);
    }
}

/* Mutation caught: join lookup stops atomically claiming exclusive cleanup
 * ownership and lets both joiners accept one target generation. */
WAMR_CONTEXT_TEST(platform_thread, test_concurrent_join_has_one_owner)
{
    struct concurrent_join_state *state = &thread_fixture.concurrent_join;

    for (size_t iteration = 0; iteration < THREAD_OWNERSHIP_ITERATIONS;
         ++iteration) {
        struct phase_worker_state *target_worker = &thread_fixture.lifecycle;
        korp_tid joiners[2] = { NULL, NULL };
        unsigned int successful_joins = 0U;
        int target_create_result;
        int target_cleanup_result;
        int joiner_join_results[2];

        memset(state, 0, sizeof(*state));
        memset(target_worker, 0, sizeof(*target_worker));
        target_worker->release_result = -EAGAIN;
        state->join_results[0] = BHT_ERROR;
        state->join_results[1] = BHT_ERROR;
        state->replacement_create_result = BHT_ERROR;
        atomic_set(&state->replacement_pending, 1);
        target_create_result = os_thread_create(
            &state->target, publish_ready_and_return, target_worker,
            WAMR_TEST_STACK_SIZE);
        if (target_create_result == BHT_OK) {
            own_thread(state->target);
        }
        zassert_equal(target_create_result, BHT_OK,
                      "iteration %zu target create returned %d", iteration,
                      target_create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish ready",
                      iteration);
        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish done",
                      iteration);
        zassert_equal(target_worker->release_result, 0,
                      "iteration %zu target release returned %d", iteration,
                      target_worker->release_result);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          state->target, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not exit", iteration);

        atomic_set(&state->hook_active, 1);
        for (size_t i = 0; i < ARRAY_SIZE(joiners); ++i) {
            int create_result;

            thread_fixture.concurrent_join_args[i].state = state;
            thread_fixture.concurrent_join_args[i].slot = i;
            create_result = os_thread_create(
                &joiners[i], join_target_concurrently,
                &thread_fixture.concurrent_join_args[i],
                WAMR_TEST_STACK_SIZE);
            if (create_result == BHT_OK) {
                own_thread(joiners[i]);
            }
            zassert_equal(create_result, BHT_OK,
                          "iteration %zu joiner %zu create returned %d",
                          iteration, i, create_result);
        }

        for (size_t i = 0; i < ARRAY_SIZE(joiners); ++i) {
            zassert_equal(wamr_zephyr_thread_test_wait(
                              joiners[i], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "iteration %zu joiner %zu did not exit",
                          iteration, i);
        }
        atomic_clear(&state->hook_active);

        for (size_t i = 0; i < ARRAY_SIZE(joiners); ++i) {
            joiner_join_results[i] = os_thread_join(joiners[i], NULL);
            if (joiner_join_results[i] == BHT_OK) {
                disown_thread(joiners[i]);
            }
        }
        target_cleanup_result = os_thread_join(state->target, NULL);
        disown_thread(state->target);
        if (state->replacement_create_result == BHT_OK) {
            int replacement_join_result =
                os_thread_join(state->replacement, NULL);

            if (replacement_join_result == BHT_OK) {
                disown_thread(state->replacement);
            }
            zassert_equal(replacement_join_result, BHT_OK,
                          "iteration %zu replacement join returned %d",
                          iteration, replacement_join_result);
        }
        for (size_t i = 0; i < ARRAY_SIZE(state->join_results); ++i) {
            successful_joins += state->join_results[i] == BHT_OK;
        }

        for (size_t i = 0; i < ARRAY_SIZE(joiners); ++i) {
            zassert_equal(joiner_join_results[i], BHT_OK,
                          "iteration %zu joiner %zu cleanup returned %d",
                          iteration, i, joiner_join_results[i]);
        }
        zassert_equal(target_cleanup_result, BHT_ERROR,
                      "iteration %zu unclaimed target cleanup returned %d",
                      iteration, target_cleanup_result);
        zassert_equal(state->replacement_create_result, BHT_OK,
                      "iteration %zu replacement create returned %d",
                      iteration, state->replacement_create_result);
        zassert_equal(successful_joins, 1U,
                      "iteration %zu observed %u successful joiners",
                      iteration, successful_joins);
    }
}

/* Mutation caught: detach ignores an already claimed join and steals target
 * cleanup ownership before the worker exits. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_join_claim_serializes_against_competing_detach)
{
    struct join_detach_race_state *state = &thread_fixture.join_detach_race;

    for (size_t iteration = 0; iteration < THREAD_OWNERSHIP_ITERATIONS;
         ++iteration) {
        struct phase_worker_state *target_worker = &thread_fixture.lifecycle;
        korp_tid target;
        korp_tid joiner;
        int target_create_result;
        int joiner_create_result;
        int lifecycle_lock_result;
        int detach_result;
        int joiner_join_result;
        int target_cleanup_result;

        memset(state, 0, sizeof(*state));
        memset(target_worker, 0, sizeof(*target_worker));
        target_worker->release_result = -EAGAIN;
        state->hook_release_result = -EAGAIN;
        state->join_result = BHT_ERROR;
        target_create_result = os_thread_create(
            &target, publish_ready_and_return, target_worker,
            WAMR_TEST_STACK_SIZE);
        if (target_create_result == BHT_OK) {
            own_thread(target);
        }
        zassert_equal(target_create_result, BHT_OK,
                      "iteration %zu target create returned %d", iteration,
                      target_create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish ready",
                      iteration);
        lifecycle_lock_result = wamr_zephyr_thread_test_lifecycle_lock(target);

        state->target = target;
        atomic_set(&state->hook_active, 1);
        joiner_create_result = os_thread_create(
            &joiner, join_before_competing_detach, state,
            WAMR_TEST_STACK_SIZE);
        if (joiner_create_result == BHT_OK) {
            own_thread(joiner);
        }
        zassert_equal(joiner_create_result, BHT_OK,
                      "iteration %zu joiner create returned %d", iteration,
                      joiner_create_result);
        zassert_equal(k_sem_take(&join_claimed,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu joiner did not claim ownership",
                      iteration);

        detach_result = os_thread_detach(target);
        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish done",
                      iteration);
        zassert_equal(target_worker->release_result, 0,
                      "iteration %zu target release returned %d", iteration,
                      target_worker->release_result);
        k_sem_give(&join_release);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          joiner, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu joiner did not exit", iteration);
        atomic_clear(&state->hook_active);
        joiner_join_result = os_thread_join(joiner, NULL);
        if (joiner_join_result == BHT_OK) {
            disown_thread(joiner);
        }
        target_cleanup_result = os_thread_join(target, NULL);
        disown_thread(target);

        zassert_equal(lifecycle_lock_result, BHT_OK,
                      "iteration %zu lifecycle lock returned %d", iteration,
                      lifecycle_lock_result);
        zassert_equal(detach_result, BHT_ERROR,
                      "iteration %zu competing detach returned %d", iteration,
                      detach_result);
        zassert_equal(state->hook_release_result, 0,
                      "iteration %zu join hook release returned %d", iteration,
                      state->hook_release_result);
        zassert_equal(state->join_result, BHT_OK,
                      "iteration %zu claimed join returned %d", iteration,
                      state->join_result);
        zassert_equal(joiner_join_result, BHT_OK,
                      "iteration %zu joiner cleanup returned %d", iteration,
                      joiner_join_result);
        zassert_equal(target_cleanup_result, BHT_ERROR,
                      "iteration %zu stale target cleanup returned %d",
                      iteration, target_cleanup_result);
    }
}

WAMR_CONTEXT_TEST(platform_thread, test_thread_identities_are_distinct)
{
    /* Mutation caught: always choosing pool slot zero. */
    korp_tid threads[2];
    korp_tid parent = os_self_thread();

    memset(&thread_fixture.identities, 0, sizeof(thread_fixture.identities));
    thread_fixture.identity_args[0].state = &thread_fixture.identities;
    thread_fixture.identity_args[0].slot = 0U;
    thread_fixture.identity_args[1].state = &thread_fixture.identities;
    thread_fixture.identity_args[1].slot = 1U;
    zassert_equal(os_thread_create(&threads[0], record_identity,
                                   &thread_fixture.identity_args[0],
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "first thread creation failed");
    zassert_equal(os_thread_create(&threads[1], record_identity,
                                   &thread_fixture.identity_args[1],
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "second thread creation failed");
    zassert_equal(os_thread_join(threads[0], NULL), BHT_OK,
                  "first thread join failed");
    zassert_equal(os_thread_join(threads[1], NULL), BHT_OK,
                  "second thread join failed");
    zassert_not_null(thread_fixture.identities.handles[0],
                     "first thread identity is null");
    zassert_not_null(thread_fixture.identities.handles[1],
                     "second thread identity is null");
    zassert_not_equal(thread_fixture.identities.handles[0],
                      thread_fixture.identities.handles[1],
                      "thread identities are shared");
    zassert_not_equal(thread_fixture.identities.handles[0], parent,
                      "first thread has the parent identity");
    zassert_not_equal(thread_fixture.identities.handles[1], parent,
                      "second thread has the parent identity");
}

WAMR_CONTEXT_TEST(platform_thread, test_multiple_threads_leave_no_stale_state)
{
    /* Mutation caught: leaving metadata from a stale slot generation. */

    memset(thread_fixture.results, 0, sizeof(thread_fixture.results));
    for (size_t i = 0; i < ARRAY_SIZE(thread_fixture.results); ++i) {
        korp_tid thread;

        thread_fixture.results[i].input = i + 1;
        zassert_equal(os_thread_create(&thread, write_result,
                                       &thread_fixture.results[i],
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "thread creation failed at index %zu", i);
        zassert_equal(os_thread_join(thread, NULL), BHT_OK,
                      "thread join failed at index %zu", i);
        zassert_equal(thread_fixture.results[i].writes, 1U,
                      "thread wrote its result an unexpected number of times");
        zassert_equal(thread_fixture.results[i].output,
                      thread_fixture.results[i].input * 2,
                      "thread left stale result state");
    }
}

/* Mutation caught: user-pool creation is incorrectly restricted to the
 * original owner and rejects an authorized child creating a grandchild. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_child_nested_creation_repeats_inherited_access)
{
    struct nested_thread_state *state = &thread_fixture.nested;

    for (size_t iteration = 0; iteration < THREAD_NESTED_ITERATIONS;
         ++iteration) {
        korp_tid child;
        void *child_result = NULL;
        int child_create_result;
        int child_join_result;

        memset(state, 0, sizeof(*state));
        state->create_result = BHT_ERROR;
        state->join_result = BHT_ERROR;
        state->child_release_result = -EAGAIN;
        state->grandchild_release_result = -EAGAIN;
        child_create_result = os_thread_create(
            &child, create_and_join_grandchild, state, WAMR_TEST_STACK_SIZE);
        if (child_create_result == BHT_OK) {
            own_thread(child);
        }
        zassert_equal(child_create_result, BHT_OK,
                      "iteration %zu child create returned %d", iteration,
                      child_create_result);
        zassert_equal(k_sem_take(&nested_child_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu child did not publish ready",
                      iteration);
        k_sem_give(&nested_child_release);
        zassert_equal(k_sem_take(&nested_grandchild_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu grandchild did not publish ready",
                      iteration);
        k_sem_give(&nested_grandchild_release);
        zassert_equal(k_sem_take(&nested_grandchild_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu grandchild did not publish done",
                      iteration);
        zassert_equal(k_sem_take(&nested_child_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu child did not publish done",
                      iteration);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          child, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu child did not exit", iteration);
        child_join_result = os_thread_join(child, &child_result);
        if (child_join_result == BHT_OK) {
            disown_thread(child);
        }
        zassert_equal(child_join_result, BHT_OK,
                      "iteration %zu child join returned %d", iteration,
                      child_join_result);
        zassert_equal(state->create_result, BHT_OK,
                      "iteration %zu grandchild create returned %d", iteration,
                      state->create_result);
        zassert_equal(state->join_result, BHT_OK,
                      "iteration %zu grandchild join returned %d", iteration,
                      state->join_result);
        zassert_equal(state->child_release_result, 0,
                      "iteration %zu child release returned %d", iteration,
                      state->child_release_result);
        zassert_equal(state->grandchild_release_result, 0,
                      "iteration %zu grandchild release returned %d", iteration,
                      state->grandchild_release_result);
        zassert_equal_ptr(state->grandchild_result, NESTED_GRANDCHILD_RESULT,
                          "iteration %zu grandchild result was %p", iteration,
                          state->grandchild_result);
        zassert_equal_ptr(child_result, NESTED_CHILD_RESULT,
                          "iteration %zu child result was %p", iteration,
                          child_result);
    }
}

WAMR_CONTEXT_TEST(platform_thread, test_create_rejects_null_tid)
{
    zassert_equal(
        os_thread_create(NULL, write_result, NULL, WAMR_TEST_STACK_SIZE),
        BHT_ERROR, NULL);
}

WAMR_CONTEXT_TEST(platform_thread, test_create_rejects_null_start)
{
    korp_tid thread;

    zassert_equal(os_thread_create(&thread, NULL, NULL, WAMR_TEST_STACK_SIZE),
                  BHT_ERROR, NULL);
}

WAMR_CONTEXT_TEST(platform_thread, test_create_rejects_zero_stack)
{
    korp_tid thread;

    zassert_equal(os_thread_create(&thread, write_result, NULL, 0), BHT_ERROR,
                  NULL);
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_oversized_stack_is_rejected_without_consuming_slot)
{
    korp_tid threads[BH_ZEPHYR_MPU_STACK_COUNT];
    int oversized_result;
    int cleanup_result = BHT_OK;

    /* Mutations caught: ignoring configured stack size or failing rollback. */
    oversized_result = os_thread_create(&threads[0], return_argument, NULL,
                                        BH_ZEPHYR_MPU_STACK_SIZE + 1U);
    if (oversized_result == BHT_OK) {
        cleanup_result = os_thread_join(threads[0], NULL);
    }
    zassert_equal(oversized_result, BHT_ERROR,
                  "oversized stack request was accepted");
    zassert_equal(cleanup_result, BHT_OK,
                  "unexpected oversized thread cleanup failed");
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_create(&threads[i], return_argument, NULL,
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "rejected oversized request consumed slot %zu",
                      i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_join(threads[i], NULL), BHT_OK,
                      "thread join failed at index %zu", i);
    }
}

#if defined(CONFIG_WAMR_TEST_USER_MODE)
WAMR_CONTEXT_TEST(platform_thread, test_create_rejects_out_of_range_priority)
{
    korp_tid thread;

    zassert_equal(
        os_thread_create_with_prio(&thread, write_result,
                                   &thread_fixture.result, WAMR_TEST_STACK_SIZE,
                                   K_LOWEST_APPLICATION_THREAD_PRIO + 1),
        BHT_ERROR, "out-of-range priority was accepted");
}

WAMR_CONTEXT_TEST(platform_thread, test_create_rejects_disallowed_priority)
{
    korp_tid thread;
    int disallowed_priority = k_thread_priority_get(k_current_get()) - 1;

    zassert_equal(os_thread_create_with_prio(
                      &thread, write_result, &thread_fixture.result,
                      WAMR_TEST_STACK_SIZE, disallowed_priority),
                  BHT_ERROR, "disallowed priority was accepted");
}
#endif

/* Mutation caught: the pool scan stops before slot four or a joined generation
 * is not returned to the free list for the next saturation cycle. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_capacity_saturation_repeats_and_recovers)
{
    korp_tid threads[MAX_BLOCKED_THREADS];
    int join_results[THREAD_POOL_CAPACITY];

    for (size_t cycle = 0; cycle < THREAD_SATURATION_ITERATIONS; ++cycle) {
        struct phase_worker_state *replacement = &thread_fixture.lifecycle;
        korp_tid replacement_thread;
        int exhaustion_result;
        int unexpected_join_result = BHT_OK;
        int replacement_create_result;
        int replacement_join_result;

        memset(threads, 0, sizeof(threads));
        for (size_t i = 0; i < THREAD_POOL_CAPACITY; ++i) {
            struct phase_worker_state *worker =
                &thread_fixture.saturation_workers[i];
            int create_result;

            memset(worker, 0, sizeof(*worker));
            worker->release_result = -EAGAIN;
            create_result = os_thread_create(&threads[i],
                                             publish_ready_and_return, worker,
                                             WAMR_TEST_STACK_SIZE);
            if (create_result == BHT_OK) {
                own_thread(threads[i]);
            }
            zassert_equal(create_result, BHT_OK,
                          "cycle %zu slot %zu create returned %d", cycle, i,
                          create_result);
            zassert_equal(k_sem_take(&phase_ready,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "cycle %zu slot %zu did not publish ready", cycle,
                          i);
            for (size_t j = 0; j < i; ++j) {
                zassert_not_equal(
                    threads[i], threads[j],
                    "cycle %zu slots %zu and %zu share handle %p", cycle, i,
                    j, threads[i]);
            }
        }

        memset(replacement, 0, sizeof(*replacement));
        replacement->release_result = -EAGAIN;
        exhaustion_result = os_thread_create(
            &threads[THREAD_POOL_CAPACITY], publish_ready_and_return,
            replacement, WAMR_TEST_STACK_SIZE);
        if (exhaustion_result == BHT_OK) {
            own_thread(threads[THREAD_POOL_CAPACITY]);
            zassert_equal(k_sem_take(&phase_ready,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "cycle %zu unexpected fifth worker not ready",
                          cycle);
        }

        for (size_t i = 0;
             i < THREAD_POOL_CAPACITY + (exhaustion_result == BHT_OK); ++i) {
            k_sem_give(&phase_release);
        }
        for (size_t i = 0;
             i < THREAD_POOL_CAPACITY + (exhaustion_result == BHT_OK); ++i) {
            zassert_equal(k_sem_take(&phase_done,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "cycle %zu worker %zu did not publish done", cycle,
                          i);
        }
        for (size_t i = 0; i < THREAD_POOL_CAPACITY; ++i) {
            zassert_equal(thread_fixture.saturation_workers[i].release_result,
                          0, "cycle %zu slot %zu release returned %d", cycle,
                          i,
                          thread_fixture.saturation_workers[i].release_result);
        }
        if (exhaustion_result == BHT_OK) {
            zassert_equal(replacement->release_result, 0,
                          "cycle %zu unexpected fifth release returned %d",
                          cycle, replacement->release_result);
        }
        for (size_t i = 0; i < THREAD_POOL_CAPACITY; ++i) {
            join_results[i] = os_thread_join(threads[i], NULL);
            if (join_results[i] == BHT_OK) {
                disown_thread(threads[i]);
            }
        }
        if (exhaustion_result == BHT_OK) {
            unexpected_join_result =
                os_thread_join(threads[THREAD_POOL_CAPACITY], NULL);
            if (unexpected_join_result == BHT_OK) {
                disown_thread(threads[THREAD_POOL_CAPACITY]);
            }
        }

        zassert_equal(exhaustion_result, BHT_ERROR,
                      "cycle %zu fifth create returned %d", cycle,
                      exhaustion_result);
        zassert_equal(unexpected_join_result, BHT_OK,
                      "cycle %zu unexpected fifth join returned %d", cycle,
                      unexpected_join_result);
        for (size_t i = 0; i < THREAD_POOL_CAPACITY; ++i) {
            zassert_equal(join_results[i], BHT_OK,
                          "cycle %zu slot %zu join returned %d", cycle, i,
                          join_results[i]);
        }

        memset(replacement, 0, sizeof(*replacement));
        replacement->return_value = (void *)(uintptr_t)(cycle + 1U);
        replacement->release_result = -EAGAIN;
        replacement_create_result = os_thread_create(
            &replacement_thread, publish_ready_and_return, replacement,
            WAMR_TEST_STACK_SIZE);
        if (replacement_create_result == BHT_OK) {
            own_thread(replacement_thread);
        }
        zassert_equal(replacement_create_result, BHT_OK,
                      "cycle %zu replacement create returned %d", cycle,
                      replacement_create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "cycle %zu replacement did not publish ready", cycle);
        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "cycle %zu replacement did not publish done", cycle);
        zassert_equal(replacement->release_result, 0,
                      "cycle %zu replacement release returned %d", cycle,
                      replacement->release_result);
        replacement_join_result = os_thread_join(replacement_thread, NULL);
        if (replacement_join_result == BHT_OK) {
            disown_thread(replacement_thread);
        }
        zassert_equal(replacement_join_result, BHT_OK,
                      "cycle %zu replacement join returned %d", cycle,
                      replacement_join_result);
    }
}

WAMR_CONTEXT_TEST(platform_thread, test_exited_threads_keep_slots_until_join)
{
    korp_tid threads[BH_ZEPHYR_MPU_STACK_COUNT];
    korp_tid replacement;

    /* Mutation caught: releasing a joinable slot at exit rather than join. */
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_create(&threads[i], return_argument, NULL,
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "thread creation failed at index %zu", i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(wamr_zephyr_thread_test_wait(threads[i], K_FOREVER), 0,
                      "thread did not exit at index %zu", i);
    }
    zassert_equal(os_thread_create(&replacement, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_ERROR, "exited slot was released before WAMR join");
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_join(threads[i], NULL), BHT_OK,
                      "thread join failed at index %zu", i);
    }
    zassert_equal(os_thread_create(&replacement, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "joined slot was not recycled");
    zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                  "replacement join failed");
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_detached_running_thread_releases_slot_on_exit)
{
    korp_tid threads[BH_ZEPHYR_MPU_STACK_COUNT];
    korp_tid replacement;
    int replacement_create_result;

    /* Mutation caught: leaving a detached running generation retained. */
    atomic_clear(&thread_fixture.blocked.ready);
    atomic_clear(&thread_fixture.blocked.release);
    atomic_clear(&thread_fixture.blocked.exited);
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_create(&threads[i], block_for_stack_recovery,
                                       &thread_fixture.blocked,
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "thread creation failed at index %zu", i);
        zassert_true(
            wait_for_atomic_count(&thread_fixture.blocked.ready, i + 1U),
            "thread did not block at index %zu", i);
    }
    zassert_equal(os_thread_detach(threads[0]), BHT_OK,
                  "running-thread detach failed");

    atomic_set(&thread_fixture.blocked.release, 1);
    zassert_true(wait_for_atomic_count(&thread_fixture.blocked.exited,
                                       ARRAY_SIZE(threads)),
                 "blocked threads did not publish exit");
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(wamr_zephyr_thread_test_wait(
                          threads[i], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "thread did not exit at index %zu", i);
    }

    replacement_create_result = os_thread_create(&replacement, return_argument,
                                                 NULL, WAMR_TEST_STACK_SIZE);
    if (replacement_create_result == BHT_OK) {
        zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                      "replacement join failed");
    }
    for (size_t i = 1; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_join(threads[i], NULL), BHT_OK,
                      "joinable thread cleanup failed at index %zu", i);
    }
    if (replacement_create_result != BHT_OK) {
        zassert_equal(os_thread_join(threads[0], NULL), BHT_OK,
                      "broken detach path cleanup failed");
    }

    zassert_equal(replacement_create_result, BHT_OK,
                  "detached running thread did not release its slot on exit");
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_detach_after_exit_releases_slot_immediately)
{
    korp_tid threads[BH_ZEPHYR_MPU_STACK_COUNT];
    korp_tid replacement;
    int replacement_create_result;

    /* Mutation caught: retaining an exited generation after detach. */
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_create(&threads[i], return_argument, NULL,
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "thread creation failed at index %zu", i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(wamr_zephyr_thread_test_wait(
                          threads[i], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "thread did not exit at index %zu", i);
    }
    zassert_equal(os_thread_detach(threads[0]), BHT_OK,
                  "exited-thread detach failed");

    replacement_create_result = os_thread_create(&replacement, return_argument,
                                                 NULL, WAMR_TEST_STACK_SIZE);
    if (replacement_create_result == BHT_OK) {
        zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                      "replacement join failed");
    }
    for (size_t i = 1; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_join(threads[i], NULL), BHT_OK,
                      "joinable thread cleanup failed at index %zu", i);
    }
    if (replacement_create_result != BHT_OK) {
        zassert_equal(os_thread_join(threads[0], NULL), BHT_OK,
                      "broken detach path cleanup failed");
    }

    zassert_equal(replacement_create_result, BHT_OK,
                  "detach did not release an exited thread slot");
}

/* Mutation caught: a head-only reaper stops at live B and misses dead A. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_reaper_skips_running_head_and_reclaims_later_exit)
{
    struct out_of_order_reaper_state *state =
        &thread_fixture.out_of_order_reaper;
    /* Creation order is A, C, D, B; each owns a distinct release barrier. */
    korp_tid threads[THREAD_POOL_CAPACITY];
    korp_tid replacement;
    int fifth_create_result;
    int join_result;

    state->hook_release_result = -EAGAIN;
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        state->workers[i].release = &reaper_worker_release[i];
        state->workers[i].release_result = -EAGAIN;
        zassert_equal(os_thread_create(&threads[i],
                                       wait_for_reaper_worker_release,
                                       &state->workers[i], WAMR_TEST_STACK_SIZE),
                      BHT_OK, "reaper worker %zu creation failed", i);
        own_thread(threads[i]);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "reaper worker %zu did not reach its barrier", i);
    }

    /* All four slots must exist before A exits: any create would reap A. */
    zassert_equal(os_thread_detach(threads[0]), BHT_OK, "A detach failed");
    disown_thread(threads[0]);
    k_sem_give(&reaper_worker_release[0]);
    zassert_equal(wamr_zephyr_thread_test_wait(
                      threads[0], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                  0, "A did not exit on the detached list");

    zassert_equal(os_thread_detach(threads[3]), BHT_OK, "B detach failed");
    disown_thread(threads[3]);
    atomic_set(&state->hook_active, 1);
    k_sem_give(&reaper_worker_release[3]);
    zassert_equal(k_sem_take(&reaper_completion_enqueued,
                             K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                  0, "B did not become the running detached-list head");

    /* [B running, A dead], with C and D still holding the other slots. */
    fifth_create_result = os_thread_create(&replacement, return_argument,
                                           NULL, WAMR_TEST_STACK_SIZE);
    if (fifth_create_result == BHT_OK) {
        own_thread(replacement);
        join_result = os_thread_join(replacement, NULL);
        if (join_result == BHT_OK) {
            disown_thread(replacement);
        }
        zassert_equal(join_result, BHT_OK, "first replacement join failed");
    }

    /* Finish detached cleanup even when the deliberate mutation rejects #5. */
    k_sem_give(&reaper_completion_release);
    zassert_equal(wamr_zephyr_thread_test_wait(
                      threads[3], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                  0, "B did not exit after hook release");
    atomic_clear(&state->hook_active);
    zassert_equal(state->hook_release_result, 0,
                  "B completion hook release failed");

    zassert_equal(os_thread_create(&replacement, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "post-B replacement creation failed");
    own_thread(replacement);
    join_result = os_thread_join(replacement, NULL);
    if (join_result == BHT_OK) {
        disown_thread(replacement);
    }
    zassert_equal(join_result, BHT_OK, "post-B replacement join failed");

    for (size_t i = 1; i <= 2; ++i) {
        k_sem_give(&reaper_worker_release[i]);
        join_result = os_thread_join(threads[i], NULL);
        if (join_result == BHT_OK) {
            disown_thread(threads[i]);
        }
        zassert_equal(join_result, BHT_OK, "blocker %zu join failed", i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(state->workers[i].release_result, 0,
                      "reaper worker %zu release failed", i);
    }
    zassert_equal(fifth_create_result, BHT_OK,
                  "fifth create returned %d: reaper did not scan past running B",
                  fifth_create_result);
}

/* Mutation caught: join accepts a target after detach has already claimed
 * cleanup ownership, or waits for that detached target to exit. */
WAMR_CONTEXT_TEST(platform_thread, test_join_rejects_detached_running_thread)
{
    struct detached_join_state *state = &thread_fixture.detached_join;

    for (size_t iteration = 0; iteration < THREAD_OWNERSHIP_ITERATIONS;
         ++iteration) {
        struct phase_worker_state *target_worker = &thread_fixture.lifecycle;
        korp_tid thread;
        korp_tid joiner;
        korp_tid replacement;
        int target_create_result;
        int detach_result;
        int joiner_create_result;
        int joiner_join_result;
        int replacement_create_result;
        int replacement_join_result;

        memset(state, 0, sizeof(*state));
        memset(target_worker, 0, sizeof(*target_worker));
        target_worker->release_result = -EAGAIN;
        state->result = BHT_OK;
        target_create_result = os_thread_create(
            &thread, publish_ready_and_return, target_worker,
            WAMR_TEST_STACK_SIZE);
        if (target_create_result == BHT_OK) {
            own_thread(thread);
        }
        zassert_equal(target_create_result, BHT_OK,
                      "iteration %zu target create returned %d", iteration,
                      target_create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish ready",
                      iteration);
        detach_result = os_thread_detach(thread);

        state->target = thread;
        joiner_create_result = os_thread_create(
            &joiner, join_detached_target, state, WAMR_TEST_STACK_SIZE);
        if (joiner_create_result == BHT_OK) {
            own_thread(joiner);
        }
        zassert_equal(joiner_create_result, BHT_OK,
                      "iteration %zu joiner create returned %d", iteration,
                      joiner_create_result);
        zassert_equal(k_sem_take(&detached_join_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu join attempt did not finish",
                      iteration);

        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish done",
                      iteration);
        zassert_equal(target_worker->release_result, 0,
                      "iteration %zu target release returned %d", iteration,
                      target_worker->release_result);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          thread, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not exit", iteration);
        disown_thread(thread);
        joiner_join_result = os_thread_join(joiner, NULL);
        if (joiner_join_result == BHT_OK) {
            disown_thread(joiner);
        }

        replacement_create_result = os_thread_create(
            &replacement, return_argument, NULL, WAMR_TEST_STACK_SIZE);
        if (replacement_create_result == BHT_OK) {
            own_thread(replacement);
            replacement_join_result = os_thread_join(replacement, NULL);
            if (replacement_join_result == BHT_OK) {
                disown_thread(replacement);
            }
        }
        else {
            replacement_join_result = BHT_ERROR;
        }

        zassert_equal(detach_result, BHT_OK,
                      "iteration %zu detach returned %d", iteration,
                      detach_result);
        zassert_equal(state->result, BHT_ERROR,
                      "iteration %zu detached join returned %d", iteration,
                      state->result);
        zassert_equal(joiner_join_result, BHT_OK,
                      "iteration %zu joiner cleanup returned %d", iteration,
                      joiner_join_result);
        zassert_equal(replacement_create_result, BHT_OK,
                      "iteration %zu replacement create returned %d",
                      iteration, replacement_create_result);
        zassert_equal(replacement_join_result, BHT_OK,
                      "iteration %zu replacement join returned %d", iteration,
                      replacement_join_result);
    }
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_detach_after_exit_keeps_exclusive_cleanup_ownership)
{
    struct detach_after_exit_race_state *state =
        &thread_fixture.detach_after_exit_race;
    korp_tid threads[BH_ZEPHYR_MPU_STACK_COUNT];
    korp_tid detacher;
    korp_tid replacement;
    int first_create_result;
    int retry_create_result;

    /* Mutation caught: publishing exited detach to the generic reaper. */
    for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_create(&threads[i], return_argument, NULL,
                                       WAMR_TEST_STACK_SIZE),
                      BHT_OK, "thread creation failed at index %zu", i);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          threads[i], K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "thread did not exit at index %zu", i);
    }
    zassert_equal(os_thread_join(threads[1], NULL), BHT_OK,
                  "detacher slot preparation failed");

    memset(state, 0, sizeof(*state));
    state->target = threads[0];
    state->hook_release_result = -EAGAIN;
    state->detach_result = BHT_ERROR;
    atomic_set(&state->hook_active, 1);
    zassert_equal(os_thread_create(&detacher, detach_exited_target, state,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "detacher creation failed");
    zassert_equal(k_sem_take(&detach_claimed,
                             K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                  0, "detacher did not claim the exited target");

    first_create_result = os_thread_create(&replacement, return_argument, NULL,
                                           WAMR_TEST_STACK_SIZE);
    if (first_create_result == BHT_OK) {
        zassert_equal(wamr_zephyr_thread_test_wait(
                          replacement, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "unexpected replacement did not exit");
    }
    k_sem_give(&detach_release);
    zassert_equal(wamr_zephyr_thread_test_wait(
                      detacher, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                  0, "detacher did not finish");
    atomic_clear(&state->hook_active);

    zassert_equal(os_thread_join(detacher, NULL), BHT_OK,
                  "detacher cleanup failed");
    if (first_create_result == BHT_OK) {
        zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                      "unexpected replacement cleanup failed");
    }
    retry_create_result = os_thread_create(&replacement, return_argument, NULL,
                                           WAMR_TEST_STACK_SIZE);
    if (retry_create_result == BHT_OK) {
        zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                      "post-detach replacement cleanup failed");
    }
    for (size_t i = 2; i < ARRAY_SIZE(threads); ++i) {
        zassert_equal(os_thread_join(threads[i], NULL), BHT_OK,
                      "retained thread cleanup failed at index %zu", i);
    }

    zassert_equal(state->detach_result, BHT_OK, "exited-thread detach failed");
    zassert_equal(state->hook_release_result, 0,
                  "detacher hook release failed");
    zassert_equal(first_create_result, BHT_ERROR,
                  "creator reused the target during detach cleanup");
    zassert_equal(retry_create_result, BHT_OK,
                  "target slot was not released after detach cleanup");
}

/* Mutation caught: stale-handle lookup compares the reused native thread ID
 * and binds a detached generation to its live replacement. */
WAMR_CONTEXT_TEST(platform_thread,
                  test_detached_handle_join_cannot_bind_replacement)
{
    struct detached_reuse_race_state *state =
        &thread_fixture.detached_reuse_race;

    for (size_t iteration = 0; iteration < THREAD_OWNERSHIP_ITERATIONS;
         ++iteration) {
        struct phase_worker_state *target_worker = &thread_fixture.lifecycle;
        korp_tid target;
        korp_tid creator;
        korp_tid joiner;
        bool creator_completed_before_join_release;
        bool join_completed_before_replacement_release;
        int target_create_result;
        int detach_result;
        int creator_create_result;
        int joiner_create_result;
        int creator_join_result;
        int joiner_join_result;
        int replacement_join_result = BHT_OK;

        memset(state, 0, sizeof(*state));
        memset(target_worker, 0, sizeof(*target_worker));
        target_worker->release_result = -EAGAIN;
        state->hook_release_result = -EAGAIN;
        state->join_start_result = -EAGAIN;
        state->creator_start_result = -EAGAIN;
        state->join_result = BHT_OK;
        state->replacement_create_result = BHT_ERROR;
        state->replacement_worker.release_result = -EAGAIN;
        target_create_result = os_thread_create(
            &target, publish_ready_and_return, target_worker,
            WAMR_TEST_STACK_SIZE);
        if (target_create_result == BHT_OK) {
            own_thread(target);
        }
        zassert_equal(target_create_result, BHT_OK,
                      "iteration %zu target create returned %d", iteration,
                      target_create_result);
        zassert_equal(k_sem_take(&phase_ready,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish ready",
                      iteration);
        detach_result = os_thread_detach(target);

        state->target = target;
        atomic_set(&state->hook_active, 1);
        creator_create_result = os_thread_create(
            &creator, create_replacement_during_join, state,
            WAMR_TEST_STACK_SIZE);
        if (creator_create_result == BHT_OK) {
            own_thread(creator);
        }
        zassert_equal(creator_create_result, BHT_OK,
                      "iteration %zu creator create returned %d", iteration,
                      creator_create_result);
        joiner_create_result = os_thread_create(
            &joiner, join_detached_target_during_reuse, state,
            WAMR_TEST_STACK_SIZE);
        if (joiner_create_result == BHT_OK) {
            own_thread(joiner);
        }
        zassert_equal(joiner_create_result, BHT_OK,
                      "iteration %zu stale joiner create returned %d",
                      iteration, joiner_create_result);

        k_sem_give(&phase_release);
        zassert_equal(k_sem_take(&phase_done,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not publish done",
                      iteration);
        zassert_equal(target_worker->release_result, 0,
                      "iteration %zu target release returned %d", iteration,
                      target_worker->release_result);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          target, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu target did not exit", iteration);
        disown_thread(target);

        k_sem_give(&reuse_join_start);
        zassert_equal(k_sem_take(&reuse_join_paused,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu stale join did not pause", iteration);
        k_sem_give(&reuse_creator_start);
        zassert_equal(k_sem_take(&reuse_creator_started,
                                 K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu creator did not start", iteration);
        creator_completed_before_join_release =
            k_sem_take(&reuse_creator_done,
                       K_MSEC(THREAD_GUARD_TIMEOUT_MS)) == 0;
        k_sem_give(&reuse_join_release);

        if (state->replacement_create_result == BHT_OK) {
            zassert_equal(k_sem_take(&phase_ready,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "iteration %zu replacement not ready", iteration);
        }
        join_completed_before_replacement_release =
            k_sem_take(&reuse_join_done,
                       K_MSEC(THREAD_GUARD_TIMEOUT_MS)) == 0;
        if (state->replacement_create_result == BHT_OK) {
            k_sem_give(&phase_release);
            zassert_equal(k_sem_take(&phase_done,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "iteration %zu replacement did not finish",
                          iteration);
            zassert_equal(state->replacement_worker.release_result, 0,
                          "iteration %zu replacement release returned %d",
                          iteration,
                          state->replacement_worker.release_result);
        }
        if (!join_completed_before_replacement_release) {
            zassert_equal(k_sem_take(&reuse_join_done,
                                     K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                          0, "iteration %zu stale joiner did not finish",
                          iteration);
        }
        atomic_clear(&state->hook_active);

        zassert_equal(wamr_zephyr_thread_test_wait(
                          creator, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu creator did not exit", iteration);
        zassert_equal(wamr_zephyr_thread_test_wait(
                          joiner, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
                      0, "iteration %zu stale joiner did not exit", iteration);
        creator_join_result = os_thread_join(creator, NULL);
        if (creator_join_result == BHT_OK) {
            disown_thread(creator);
        }
        joiner_join_result = os_thread_join(joiner, NULL);
        if (joiner_join_result == BHT_OK) {
            disown_thread(joiner);
        }
        if (state->replacement_create_result == BHT_OK
            && state->join_result == BHT_ERROR) {
            replacement_join_result = os_thread_join(state->replacement, NULL);
        }

        zassert_equal(detach_result, BHT_OK,
                      "iteration %zu detach returned %d", iteration,
                      detach_result);
        zassert_true(creator_completed_before_join_release,
                     "iteration %zu creator missed the reuse boundary",
                     iteration);
        zassert_equal(state->hook_release_result, 0,
                      "iteration %zu stale join hook returned %d", iteration,
                      state->hook_release_result);
        zassert_equal(state->join_start_result, 0,
                      "iteration %zu stale join start returned %d", iteration,
                      state->join_start_result);
        zassert_equal(state->creator_start_result, 0,
                      "iteration %zu creator start returned %d", iteration,
                      state->creator_start_result);
        zassert_equal(state->replacement_create_result, BHT_OK,
                      "iteration %zu replacement create returned %d",
                      iteration, state->replacement_create_result);
        zassert_true(join_completed_before_replacement_release,
                     "iteration %zu stale join waited for replacement",
                     iteration);
        zassert_equal(state->join_result, BHT_ERROR,
                      "iteration %zu stale join returned %d", iteration,
                      state->join_result);
        zassert_equal(creator_join_result, BHT_OK,
                      "iteration %zu creator cleanup returned %d", iteration,
                      creator_join_result);
        zassert_equal(joiner_join_result, BHT_OK,
                      "iteration %zu joiner cleanup returned %d", iteration,
                      joiner_join_result);
        zassert_equal(replacement_join_result, BHT_OK,
                      "iteration %zu replacement cleanup returned %d",
                      iteration, replacement_join_result);
    }
}

WAMR_CONTEXT_TEST(platform_thread,
                  test_detached_explicit_thread_exit_releases_slot)
{
    struct explicit_exit_state *state = &thread_fixture.explicit_exit;
    korp_tid thread;
    korp_tid replacement;

    /* Mutation caught: explicit exit bypassing detached completion cleanup. */
    memset(state, 0, sizeof(*state));
    state->detach_result = BHT_ERROR;
    zassert_equal(os_thread_create(&thread, detach_and_exit_explicitly, state,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "explicit-exit thread creation failed");
    zassert_true(wait_for_atomic_count(&state->exit_started, 1U),
                 "explicit-exit thread did not detach");
    zassert_equal(
        wamr_zephyr_thread_test_wait(thread, K_MSEC(THREAD_GUARD_TIMEOUT_MS)),
        0, "explicit-exit thread did not terminate");

    zassert_equal(os_thread_create(&replacement, return_argument, NULL,
                                   WAMR_TEST_STACK_SIZE),
                  BHT_OK, "explicit exit did not release the detached slot");
    zassert_equal(os_thread_join(replacement, NULL), BHT_OK,
                  "explicit-exit replacement cleanup failed");
    zassert_equal(state->detach_result, BHT_OK, "self-detach failed");
    zassert_false(atomic_get(&state->exit_returned),
                  "os_thread_exit unexpectedly returned");
}
