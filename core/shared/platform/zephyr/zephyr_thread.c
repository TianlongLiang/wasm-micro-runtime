/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "zephyr_pool_owner_compat.h"
#include "zephyr_sync_pool.h"
#include "zephyr_thread_pool.h"

#if defined(CONFIG_WAMR_ZEPHYR_TEST_MOCKS)
/* The dedicated Ztest scenario injects documented allocator/join failures here.
 * Ordinary builds expand directly to BH_MALLOC/BH_FREE/k_thread_join and carry
 * no fake state or runtime dispatch. See tests/platform_api/README.md. */
extern void *wamr_zephyr_thread_test_malloc(unsigned int size);
extern void wamr_zephyr_thread_test_free(void *ptr);
extern int wamr_zephyr_thread_test_join(k_tid_t thread, k_timeout_t timeout);
#define WAMR_THREAD_MALLOC(size) wamr_zephyr_thread_test_malloc(size)
#define WAMR_THREAD_FREE(ptr) wamr_zephyr_thread_test_free(ptr)
#define WAMR_THREAD_JOIN(thread, timeout) \
    wamr_zephyr_thread_test_join(thread, timeout)
#else
#define WAMR_THREAD_MALLOC(size) BH_MALLOC(size)
#define WAMR_THREAD_FREE(ptr) BH_FREE(ptr)
#define WAMR_THREAD_JOIN(thread, timeout) k_thread_join(thread, timeout)
#endif

/* clang-format off */
#define bh_assert(v) do {                                   \
    if (!(v)) {                                             \
        printf("\nASSERTION FAILED: %s, at %s, line %d\n",  \
               #v, __FILE__, __LINE__);                     \
        abort();                                            \
    }                                                       \
} while (0)
/* clang-format on */

#if defined(CONFIG_ARM_MPU) || defined(CONFIG_ARC_MPU) \
    || KERNEL_VERSION_NUMBER > 0x020300 /* version 2.3.0 */
#define BH_ENABLE_ZEPHYR_MPU_STACK 1
#elif !defined(BH_ENABLE_ZEPHYR_MPU_STACK)
#define BH_ENABLE_ZEPHYR_MPU_STACK 0
#endif

typedef enum {
    WAMR_THREAD_FREE,
    WAMR_THREAD_RESERVED,
    WAMR_THREAD_RUNNING,
    WAMR_THREAD_EXITED,
    WAMR_THREAD_DETACHED_RUNNING,
    WAMR_THREAD_JOINED,
} wamr_thread_state_t;

typedef enum {
    WAMR_SYNC_POOL_UNPREPARED,
    WAMR_SYNC_POOL_PREPARING,
    WAMR_SYNC_POOL_PREPARED,
} wamr_sync_pool_prepare_state_t;

#if defined(CONFIG_USERSPACE)
typedef enum {
    WAMR_SYNC_SLOT_FREE,
    WAMR_SYNC_SLOT_RESERVED,
    WAMR_SYNC_SLOT_ACTIVE,
    WAMR_SYNC_SLOT_DESTROYING,
} wamr_sync_slot_state_t;

typedef struct {
    wamr_sync_slot_state_t state;
    korp_mutex handle;
    struct k_mutex *native;
    uint32 active_operations;
    k_tid_t owner;
    uint32 recursion;
} wamr_mutex_slot_t;

typedef struct {
    wamr_sync_slot_state_t state;
    korp_cond handle;
    struct k_condvar *native;
    uint32 active_operations;
    uint32 active_waiters;
} wamr_cond_slot_t;
#endif

static wamr_zephyr_thread_pool_t wamr_thread_pool;
static k_tid_t wamr_thread_pool_owner;
static wamr_thread_state_t wamr_thread_pool_states[BH_ZEPHYR_MPU_STACK_COUNT];
static wamr_zephyr_sync_pool_t wamr_sync_pool;
static k_tid_t wamr_sync_pool_owner;
static atomic_t wamr_sync_pool_prepare_state;
static zmutex_t thread_pool_lock;
#if defined(CONFIG_USERSPACE)
static wamr_mutex_slot_t wamr_mutex_slots[BH_ZEPHYR_MUTEX_POOL_COUNT];
static wamr_cond_slot_t wamr_cond_slots[BH_ZEPHYR_COND_POOL_COUNT];
static uintptr_t next_mutex_handle;
static uintptr_t next_cond_handle;
static atomic_t thread_sys_destroy_pending;
#endif

static bool
sync_pool_object_range(const void *objects, size_t object_count,
                       size_t object_size, uintptr_t *range_start,
                       uintptr_t *range_end)
{
    uintptr_t start = (uintptr_t)objects;
    uintptr_t size;

    /* Every caller bounds object_count by the compile-time mutex/condition
     * pool capacity and passes a nonzero sizeof expression. */
    bh_assert(object_size != 0U);
    bh_assert(object_count <= UINTPTR_MAX / object_size);
    size = object_count * object_size;
    if (start > UINTPTR_MAX - size) {
        return false;
    }

    *range_start = start;
    *range_end = start + size;
    return true;
}

static bool
sync_pool_ranges_overlap(uintptr_t first_start, uintptr_t first_end,
                         uintptr_t second_start, uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static bool
sync_pool_matches(const wamr_zephyr_sync_pool_t *pool, k_tid_t wamr_user_thread)
{
    return wamr_sync_pool_owner == wamr_user_thread
           && wamr_sync_pool.management_lock == pool->management_lock
           && wamr_sync_pool.mutexes == pool->mutexes
           && wamr_sync_pool.mutex_count == pool->mutex_count
           && wamr_sync_pool.condvars == pool->condvars
           && wamr_sync_pool.condvar_count == pool->condvar_count;
}

static bool
prepare_pool_owner_access(k_tid_t owner, bool *access_granted)
{
    wamr_zephyr_pool_owner_validation_t validation_result;

    *access_granted = false;
    validation_result = wamr_zephyr_pool_owner_validate(owner);

    if (validation_result == WAMR_ZEPHYR_POOL_OWNER_NEEDS_ACCESS) {
        k_object_access_grant(owner, k_current_get());
        validation_result = wamr_zephyr_pool_owner_validate(owner);
        if (validation_result != WAMR_ZEPHYR_POOL_OWNER_ACCESSIBLE) {
            k_object_access_revoke(owner, k_current_get());
            return false;
        }
        *access_granted = true;
    }
    return validation_result == WAMR_ZEPHYR_POOL_OWNER_ACCESSIBLE;
}

int
wamr_zephyr_sync_pool_prepare(const wamr_zephyr_sync_pool_t *pool,
                              k_tid_t wamr_user_thread)
{
    bool owner_access_granted;
    atomic_val_t prepare_state;
    uintptr_t management_start;
    uintptr_t management_end;
    uintptr_t mutexes_start;
    uintptr_t mutexes_end;
    uintptr_t condvars_start;
    uintptr_t condvars_end;
    size_t i;
    int native_result;

    if (k_is_user_context() || pool == NULL || wamr_user_thread == NULL
        || pool->management_lock == NULL || pool->mutexes == NULL
        || pool->condvars == NULL || pool->mutex_count == 0U
        || pool->mutex_count > BH_ZEPHYR_MUTEX_POOL_COUNT
        || pool->condvar_count == 0U
        || pool->condvar_count > BH_ZEPHYR_COND_POOL_COUNT) {
        return BHT_ERROR;
    }

    if (!sync_pool_object_range(pool->management_lock, 1U,
                                sizeof(*pool->management_lock),
                                &management_start, &management_end)
        || !sync_pool_object_range(pool->mutexes, pool->mutex_count,
                                   sizeof(*pool->mutexes), &mutexes_start,
                                   &mutexes_end)
        || !sync_pool_object_range(pool->condvars, pool->condvar_count,
                                   sizeof(*pool->condvars), &condvars_start,
                                   &condvars_end)
        || sync_pool_ranges_overlap(management_start, management_end,
                                    mutexes_start, mutexes_end)
        || sync_pool_ranges_overlap(management_start, management_end,
                                    condvars_start, condvars_end)
        || sync_pool_ranges_overlap(mutexes_start, mutexes_end, condvars_start,
                                    condvars_end)) {
        return BHT_ERROR;
    }

    prepare_state = atomic_get(&wamr_sync_pool_prepare_state);
    if (prepare_state == WAMR_SYNC_POOL_PREPARED) {
        if (!sync_pool_matches(pool, wamr_user_thread)) {
            return BHT_ERROR;
        }
        /* The exact binding already validated this owner on first prepare. */
        k_object_access_grant(wamr_user_thread, k_current_get());
        return BHT_OK;
    }

    if (prepare_state != WAMR_SYNC_POOL_UNPREPARED) {
        return BHT_ERROR;
    }
    if (!prepare_pool_owner_access(wamr_user_thread,
                                   &owner_access_granted)) {
        return BHT_ERROR;
    }
    if (!atomic_cas(&wamr_sync_pool_prepare_state, WAMR_SYNC_POOL_UNPREPARED,
                    WAMR_SYNC_POOL_PREPARING)) {
        if (owner_access_granted) {
            k_object_access_revoke(wamr_user_thread, k_current_get());
        }
        return BHT_ERROR;
    }

    /* Zephyr 3.7 and 4.4 document no recoverable initialization failure for a
     * valid object. Reaching this assertion means the validated pool contract or
     * the Zephyr kernel invariant was violated. */
    native_result = k_mutex_init(pool->management_lock);
    bh_assert(native_result == 0);
    native_result = k_mutex_lock(pool->management_lock, K_FOREVER);
    bh_assert(native_result == 0);

#if defined(CONFIG_USERSPACE)
    memset(wamr_mutex_slots, 0, sizeof(wamr_mutex_slots));
    memset(wamr_cond_slots, 0, sizeof(wamr_cond_slots));
#endif
    k_object_access_grant(pool->management_lock, wamr_user_thread);

    for (i = 0; i < pool->mutex_count; i++) {
        native_result = k_mutex_init(&pool->mutexes[i]);
        bh_assert(native_result == 0);
#if defined(CONFIG_USERSPACE)
        wamr_mutex_slots[i].native = &pool->mutexes[i];
#endif
        k_object_access_grant(&pool->mutexes[i], wamr_user_thread);
    }

    for (i = 0; i < pool->condvar_count; i++) {
        native_result = k_condvar_init(&pool->condvars[i]);
        bh_assert(native_result == 0);
#if defined(CONFIG_USERSPACE)
        wamr_cond_slots[i].native = &pool->condvars[i];
#endif
        k_object_access_grant(&pool->condvars[i], wamr_user_thread);
    }

    k_object_access_grant(wamr_user_thread, k_current_get());
    wamr_sync_pool = *pool;
    wamr_sync_pool_owner = wamr_user_thread;
    k_mutex_unlock(pool->management_lock);
    atomic_set(&wamr_sync_pool_prepare_state, WAMR_SYNC_POOL_PREPARED);
    return BHT_OK;
}

int
wamr_zephyr_thread_pool_prepare(const wamr_zephyr_thread_pool_t *pool,
                                k_tid_t wamr_user_thread)
{
    bool owner_access_granted;
    size_t i;

    if (k_is_user_context() || pool == NULL || wamr_user_thread == NULL
        || pool->threads == NULL || pool->stacks == NULL
        || pool->thread_count == 0U
        || pool->thread_count > BH_ZEPHYR_MPU_STACK_COUNT
        || pool->stack_size == 0U || pool->stack_stride < pool->stack_size) {
        return BHT_ERROR;
    }

    if (wamr_thread_pool.threads != NULL) {
        if (wamr_thread_pool_owner != wamr_user_thread
            || wamr_thread_pool.threads != pool->threads
            || wamr_thread_pool.stacks != pool->stacks
            || wamr_thread_pool.thread_count != pool->thread_count
            || wamr_thread_pool.stack_size != pool->stack_size
            || wamr_thread_pool.stack_stride != pool->stack_stride) {
            return BHT_ERROR;
        }
        /* The exact binding already validated this owner on first prepare. */
        k_object_access_grant(wamr_user_thread, k_current_get());
        return BHT_OK;
    }

    if (!prepare_pool_owner_access(wamr_user_thread,
                                   &owner_access_granted)) {
        return BHT_ERROR;
    }

    for (i = 0; i < pool->thread_count; i++) {
        k_object_access_grant(&pool->threads[i], wamr_user_thread);
        k_object_access_grant((uint8 *)pool->stacks + i * pool->stack_stride,
                              wamr_user_thread);
    }

    k_object_access_grant(wamr_user_thread, k_current_get());
    wamr_thread_pool = *pool;
    wamr_thread_pool_owner = wamr_user_thread;
    return BHT_OK;
}

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
static K_THREAD_STACK_ARRAY_DEFINE(mpu_stacks, BH_ZEPHYR_MPU_STACK_COUNT,
                                   BH_ZEPHYR_MPU_STACK_SIZE);
static wamr_thread_state_t mpu_stack_states[BH_ZEPHYR_MPU_STACK_COUNT];
#endif

typedef struct os_thread_data {
    struct os_thread_data *next;
    korp_tid handle;
    k_tid_t tid;
    void *tlr;
    void *return_value;
    wamr_thread_state_t state;
    bool join_claimed;
    bool uses_user_pool;
    size_t pool_index;
    unsigned stack_size;
    char *stack;
} os_thread_data;

typedef struct os_thread_obj {
    struct k_thread thread;
} os_thread_obj;

static bool is_thread_sys_inited = false;

/* Thread data of supervisor thread */
static os_thread_data supervisor_thread_data;

/* Thread data list */
static os_thread_data *thread_data_list = NULL;

/* Detached threads awaiting Zephyr termination before resource reuse. */
static os_thread_data *detached_thread_data_list = NULL;

/* Opaque WAMR identity; Zephyr thread objects are reusable pool storage. */
static uintptr_t next_thread_handle;

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

#if defined(CONFIG_ZTEST)
__weak void
wamr_zephyr_thread_join_test_hook(int phase)
{
    (void)phase;
}
__weak void
wamr_zephyr_thread_detach_test_hook(int phase)
{
    (void)phase;
}
#define WAMR_JOIN_TEST_HOOK(phase) wamr_zephyr_thread_join_test_hook(phase)
#define WAMR_DETACH_TEST_HOOK(phase) wamr_zephyr_thread_detach_test_hook(phase)
#else
#define WAMR_JOIN_TEST_HOOK(phase) (void)(phase)
#define WAMR_DETACH_TEST_HOOK(phase) (void)(phase)
#endif

static void
thread_data_list_add_locked(os_thread_data *thread_data)
{
    os_thread_data *p;

    for (p = thread_data_list; p != NULL; p = p->next) {
        /* A generation is allocated and inserted exactly once under the pool
         * lock; duplicate insertion is an internal lifecycle bug. */
        bh_assert(p != thread_data);
    }
    thread_data->next = thread_data_list;
    thread_data_list = thread_data;
}

static void
thread_data_list_remove_locked(os_thread_data *thread_data)
{
    os_thread_data **link = &thread_data_list;

    while (*link != NULL && *link != thread_data) {
        link = &(*link)->next;
    }
    /* Callers either looked up this generation while holding the lock or are
     * completing that mapped generation. */
    bh_assert(*link == thread_data);
    *link = thread_data->next;
}

static void
detached_thread_data_list_add_locked(os_thread_data *thread_data)
{
    thread_data->next = detached_thread_data_list;
    detached_thread_data_list = thread_data;
}

static wamr_thread_state_t *
thread_slot_state_locked(os_thread_data *thread_data)
{
    if (thread_data->uses_user_pool) {
        return &wamr_thread_pool_states[thread_data->pool_index];
    }
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    return &mpu_stack_states[thread_data->pool_index];
#else
    return NULL;
#endif
}

static void
thread_slot_set_state_locked(os_thread_data *thread_data,
                             wamr_thread_state_t state)
{
    wamr_thread_state_t *slot_state = thread_slot_state_locked(thread_data);

    if (slot_state != NULL) {
        *slot_state = state;
    }
}

static void
thread_generation_release_locked(os_thread_data *thread_data)
{
    wamr_thread_state_t *slot_state = thread_slot_state_locked(thread_data);

    thread_data->next = NULL;
    thread_data->handle = NULL;
    thread_data->tid = NULL;
    thread_data->tlr = NULL;
    thread_data->return_value = NULL;
    thread_data->state = WAMR_THREAD_FREE;
    thread_data->join_claimed = false;
    thread_data->uses_user_pool = false;
    thread_data->pool_index = 0U;
    thread_data->stack_size = 0U;
    thread_data->stack = NULL;
    if (slot_state != NULL) {
        *slot_state = WAMR_THREAD_FREE;
    }
}

static void
detached_thread_data_release(os_thread_data *thread_data)
{
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    char *stack;
#endif
    bool uses_user_pool;
    k_tid_t tid;

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    uses_user_pool = thread_data->uses_user_pool;
    tid = thread_data->tid;
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    stack = thread_data->stack;
#endif
    thread_generation_release_locked(thread_data);
    zmutex_unlock(&thread_pool_lock);

    if (!uses_user_pool) {
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
        WAMR_THREAD_FREE(stack);
#endif
        WAMR_THREAD_FREE((os_thread_obj *)tid);
    }
    WAMR_THREAD_FREE(thread_data);
}

static os_thread_data *
thread_data_list_lookup_tid_locked(k_tid_t tid)
{
    if (thread_data_list) {
        os_thread_data *p = thread_data_list;
        while (p) {
            if (p->tid == tid)
                return p;
            p = p->next;
        }
    }
    return NULL;
}

static os_thread_data *
thread_data_list_lookup_handle_locked(korp_tid handle)
{
    os_thread_data *thread_data = thread_data_list;

    while (thread_data != NULL) {
        if (thread_data->handle == handle) {
            return thread_data;
        }
        thread_data = thread_data->next;
    }
    return NULL;
}

static korp_tid
thread_handle_alloc_locked(void)
{
    next_thread_handle++;
    if (next_thread_handle == 0U) {
        next_thread_handle++;
    }
    return (korp_tid)next_thread_handle;
}

static void
detached_thread_data_reap(void)
{
    os_thread_data *thread_data;
    os_thread_data *previous;

    while (true) {
        zmutex_lock(&thread_pool_lock, K_FOREVER);
        previous = NULL;
        thread_data = detached_thread_data_list;
        while (thread_data != NULL
               && k_thread_join(thread_data->tid, K_NO_WAIT) != 0) {
            previous = thread_data;
            thread_data = thread_data->next;
        }
        if (thread_data == NULL) {
            zmutex_unlock(&thread_pool_lock);
            return;
        }

        if (previous == NULL) {
            detached_thread_data_list = thread_data->next;
        }
        else {
            previous->next = thread_data->next;
        }
        zmutex_unlock(&thread_pool_lock);

        detached_thread_data_release(thread_data);
    }
}

#if defined(CONFIG_ZTEST)
int
wamr_zephyr_thread_test_wait(korp_tid handle, k_timeout_t timeout)
{
    os_thread_data *thread_data;
    k_tid_t tid = NULL;

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    thread_data = thread_data_list_lookup_handle_locked(handle);
    if (thread_data == NULL) {
        thread_data = detached_thread_data_list;
        while (thread_data != NULL && thread_data->handle != handle) {
            thread_data = thread_data->next;
        }
    }
    if (thread_data != NULL) {
        tid = thread_data->tid;
    }
    zmutex_unlock(&thread_pool_lock);

    return tid != NULL ? k_thread_join(tid, timeout) : BHT_ERROR;
}

int
wamr_zephyr_thread_test_lifecycle_lock(korp_tid handle)
{
    os_thread_data *thread_data;
    int result = BHT_ERROR;

    if (zmutex_lock(&thread_pool_lock, K_FOREVER) != 0) {
        return BHT_ERROR;
    }

    thread_data = thread_data_list_lookup_handle_locked(handle);
    if (thread_data != NULL) {
        result = BHT_OK;
    }
    (void)zmutex_unlock(&thread_pool_lock);
    return result;
}
#endif

static os_thread_data *
thread_data_list_claim_join(korp_tid handle)
{
    os_thread_data *claimed = NULL;

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    if (thread_data_list) {
        os_thread_data *p = thread_data_list;

        while (p) {
            if (p->handle == handle) {
                WAMR_JOIN_TEST_HOOK(WAMR_JOIN_TEST_PROTECTED_LOOKUP);
                if (!p->join_claimed
                    && (p->state == WAMR_THREAD_RUNNING
                        || p->state == WAMR_THREAD_EXITED)) {
                    p->join_claimed = true;
                    claimed = p;
                }
                break;
            }
            p = p->next;
        }
    }
    zmutex_unlock(&thread_pool_lock);
    return claimed;
}

static bool
thread_pool_has_active_slots_locked(void)
{
    size_t i;

    if (thread_data_list != &supervisor_thread_data
        || supervisor_thread_data.next != NULL
        || detached_thread_data_list != NULL) {
        return true;
    }

    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        if (wamr_thread_pool_states[i] != WAMR_THREAD_FREE) {
            return true;
        }
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
        if (mpu_stack_states[i] != WAMR_THREAD_FREE) {
            return true;
        }
#endif
    }

    return false;
}

#if defined(CONFIG_USERSPACE)
static bool
sync_pool_has_active_slots_locked(void)
{
    size_t i;

    for (i = 0; i < wamr_sync_pool.mutex_count; i++) {
        if (wamr_mutex_slots[i].state != WAMR_SYNC_SLOT_FREE) {
            return true;
        }
    }

    for (i = 0; i < wamr_sync_pool.condvar_count; i++) {
        if (wamr_cond_slots[i].state != WAMR_SYNC_SLOT_FREE) {
            return true;
        }
    }

    return false;
}

#endif

int
os_thread_sys_init()
{
    if (is_thread_sys_inited)
        return BHT_OK;

#if defined(CONFIG_USERSPACE)
    atomic_set(&thread_sys_destroy_pending, 0);
#endif
    zmutex_init(&thread_pool_lock);
    memset(wamr_thread_pool_states, 0, sizeof(wamr_thread_pool_states));
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    memset(mpu_stack_states, 0, sizeof(mpu_stack_states));
#endif

    /* Initialize supervisor thread data */
    memset(&supervisor_thread_data, 0, sizeof(supervisor_thread_data));
    supervisor_thread_data.tid = k_current_get();
    supervisor_thread_data.handle = thread_handle_alloc_locked();
    supervisor_thread_data.state = WAMR_THREAD_RUNNING;
    /* Set as head of thread data list */
    thread_data_list = &supervisor_thread_data;

    is_thread_sys_inited = true;
    return BHT_OK;
}

void
os_thread_sys_destroy(void)
{
    bool thread_slots_active;
#if defined(CONFIG_USERSPACE)
    bool sync_audit_locked = false;
    bool sync_audit_failed = false;
    bool sync_slots_active;
#endif

    if (!is_thread_sys_inited) {
        return;
    }

    detached_thread_data_reap();
    zmutex_lock(&thread_pool_lock, K_FOREVER);
    thread_slots_active = thread_pool_has_active_slots_locked();
#if defined(CONFIG_USERSPACE)
    sync_slots_active = false;
    if (atomic_get(&wamr_sync_pool_prepare_state) == WAMR_SYNC_POOL_PREPARED) {
        if (wamr_sync_pool.management_lock == NULL
            || k_mutex_lock(wamr_sync_pool.management_lock, K_FOREVER) != 0) {
            sync_audit_failed = true;
            os_printf("WAMR Zephyr synchronization pool audit failed\n");
        }
        else {
            sync_audit_locked = true;
            sync_slots_active = sync_pool_has_active_slots_locked();
        }
    }
    if (sync_slots_active) {
        os_printf("WAMR Zephyr synchronization pool still has active slots\n");
    }
#endif
    if (thread_slots_active) {
        os_printf("WAMR Zephyr thread pool still has active slots\n");
#if defined(CONFIG_USERSPACE)
        atomic_set(&thread_sys_destroy_pending, 0);
        if (sync_audit_locked) {
            (void)k_mutex_unlock(wamr_sync_pool.management_lock);
        }
#endif
        zmutex_unlock(&thread_pool_lock);
        return;
    }
#if defined(CONFIG_USERSPACE)
    if (sync_audit_failed || sync_slots_active) {
        atomic_set(&thread_sys_destroy_pending,
                   sync_slots_active && sync_audit_locked ? 1 : 0);
        if (sync_audit_locked) {
            (void)k_mutex_unlock(wamr_sync_pool.management_lock);
        }
        zmutex_unlock(&thread_pool_lock);
        return;
    }
    atomic_set(&thread_sys_destroy_pending, 0);
#endif

    thread_data_list = NULL;
    detached_thread_data_list = NULL;
    memset(&supervisor_thread_data, 0, sizeof(supervisor_thread_data));
    memset(&wamr_thread_pool, 0, sizeof(wamr_thread_pool));
    wamr_thread_pool_owner = NULL;
    memset(wamr_thread_pool_states, 0, sizeof(wamr_thread_pool_states));
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    memset(mpu_stack_states, 0, sizeof(mpu_stack_states));
#endif
    is_thread_sys_inited = false;
#if defined(CONFIG_USERSPACE)
    if (sync_audit_locked) {
        (void)k_mutex_unlock(wamr_sync_pool.management_lock);
    }
#endif
    zmutex_unlock(&thread_pool_lock);
}

static void
os_thread_complete(void *return_value)
{
    os_thread_data *thread_data;
#if defined(CONFIG_ZTEST)
    bool detached_enqueued = false;
#endif

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    thread_data = thread_data_list_lookup_tid_locked(k_current_get());
    bh_assert(thread_data != NULL);
    thread_data->return_value = return_value;
    if (thread_data->state == WAMR_THREAD_RUNNING) {
        thread_data->state = WAMR_THREAD_EXITED;
        thread_slot_set_state_locked(thread_data, WAMR_THREAD_EXITED);
    }
    else if (thread_data->state == WAMR_THREAD_DETACHED_RUNNING) {
        thread_data_list_remove_locked(thread_data);
        detached_thread_data_list_add_locked(thread_data);
#if defined(CONFIG_ZTEST)
        detached_enqueued = true;
#endif
    }
    zmutex_unlock(&thread_pool_lock);
#if defined(CONFIG_ZTEST)
    if (detached_enqueued) {
        WAMR_DETACH_TEST_HOOK(WAMR_DETACH_TEST_COMPLETION_ENQUEUED);
    }
#endif
}

static void
os_thread_wrapper(void *start, void *arg, void *thread_data)
{
    void *return_value;

    bh_assert(((os_thread_data *)thread_data)->tid == k_current_get());
    return_value = ((thread_start_routine_t)start)(arg);
    os_thread_complete(return_value);
}

int
os_thread_create(korp_tid *p_tid, thread_start_routine_t start, void *arg,
                 unsigned int stack_size)
{
    return os_thread_create_with_prio(p_tid, start, arg, stack_size,
                                      BH_THREAD_DEFAULT_PRIORITY);
}

int
os_thread_create_with_prio(korp_tid *p_tid, thread_start_routine_t start,
                           void *arg, unsigned int stack_size, int prio)
{
    os_thread_obj *thread_obj = NULL;
    os_thread_data *thread_data = NULL;
    k_tid_t tid = NULL;
    k_tid_t created_tid;
    bool uses_user_pool = k_is_user_context();
    size_t pool_index = 0U;
    size_t i;

    if (!p_tid || !start || !stack_size)
        return BHT_ERROR;

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    if (!uses_user_pool && stack_size > BH_ZEPHYR_MPU_STACK_SIZE)
        return BHT_ERROR;
#endif

    if (uses_user_pool) {
        if (wamr_thread_pool.threads == NULL
            || stack_size > wamr_thread_pool.stack_size
            || prio < K_HIGHEST_APPLICATION_THREAD_PRIO
            || prio > K_LOWEST_APPLICATION_THREAD_PRIO
            || prio < k_thread_priority_get(k_current_get())) {
            return BHT_ERROR;
        }
    }

    detached_thread_data_reap();

    if (!uses_user_pool) {
        if (!(thread_obj = WAMR_THREAD_MALLOC(sizeof(os_thread_obj)))) {
            return BHT_ERROR;
        }
        memset(thread_obj, 0, sizeof(*thread_obj));
        tid = &thread_obj->thread;
    }

    /* Create and initialize thread data */
    if (!(thread_data = WAMR_THREAD_MALLOC(sizeof(os_thread_data)))) {
        goto fail;
    }

    memset(thread_data, 0, sizeof(*thread_data));
    thread_data->state = WAMR_THREAD_RESERVED;
    thread_data->uses_user_pool = uses_user_pool;

#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    if (!uses_user_pool) {
        if (stack_size < APP_THREAD_STACK_SIZE_MIN)
            stack_size = APP_THREAD_STACK_SIZE_MIN;
        if (!(thread_data->stack = WAMR_THREAD_MALLOC(stack_size))) {
            goto fail;
        }
        thread_data->stack_size = stack_size;
    }
#endif

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    if (uses_user_pool) {
        for (i = 0; i < wamr_thread_pool.thread_count; i++) {
            if (wamr_thread_pool_states[i] == WAMR_THREAD_FREE) {
                pool_index = i;
                break;
            }
        }
        if (i == wamr_thread_pool.thread_count) {
            zmutex_unlock(&thread_pool_lock);
            goto fail;
        }

        wamr_thread_pool_states[pool_index] = WAMR_THREAD_RESERVED;
        tid = &wamr_thread_pool.threads[pool_index];
        thread_data->stack = (char *)wamr_thread_pool.stacks
                             + pool_index * wamr_thread_pool.stack_stride;
        thread_data->stack_size = wamr_thread_pool.stack_size;
    }
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    else {
        for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
            if (mpu_stack_states[i] == WAMR_THREAD_FREE) {
                pool_index = i;
                break;
            }
        }
        if (i == BH_ZEPHYR_MPU_STACK_COUNT) {
            zmutex_unlock(&thread_pool_lock);
            goto fail;
        }

        mpu_stack_states[pool_index] = WAMR_THREAD_RESERVED;
        thread_data->stack = (char *)mpu_stacks[pool_index];
        thread_data->stack_size = BH_ZEPHYR_MPU_STACK_SIZE;
    }
#endif
    thread_data->pool_index = pool_index;
    thread_data->handle = thread_handle_alloc_locked();
    thread_data->tid = tid;
    thread_data_list_add_locked(thread_data);
    zmutex_unlock(&thread_pool_lock);

    created_tid = k_thread_create(
        tid, (k_thread_stack_t *)thread_data->stack, thread_data->stack_size,
        os_thread_wrapper, start, arg, thread_data, prio,
        uses_user_pool ? K_USER | K_INHERIT_PERMS : 0, K_FOREVER);
    /* Both supported Zephyr lines return the supplied thread object after valid
     * arguments; invalid objects fault/assert instead of reporting NULL. */
    bh_assert(created_tid == tid);

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    thread_data->state = WAMR_THREAD_RUNNING;
    thread_slot_set_state_locked(thread_data, WAMR_THREAD_RUNNING);
    zmutex_unlock(&thread_pool_lock);
    k_thread_name_set(tid, "wasm-zephyr");
    *p_tid = thread_data->handle;
    k_thread_start(tid);
    return BHT_OK;

fail:
    if (thread_data != NULL) {
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
        if (!uses_user_pool) {
            WAMR_THREAD_FREE(thread_data->stack);
        }
#endif
        WAMR_THREAD_FREE(thread_data);
    }
    if (!uses_user_pool) {
        WAMR_THREAD_FREE(thread_obj);
    }
    return BHT_ERROR;
}

korp_tid
os_self_thread()
{
    os_thread_data *thread_data;
    korp_tid handle = NULL;

    if (!is_thread_sys_inited
        || zmutex_lock(&thread_pool_lock, K_FOREVER) != 0) {
        return NULL;
    }

    thread_data = thread_data_list_lookup_tid_locked(k_current_get());
    if (thread_data != NULL) {
        handle = thread_data->handle;
    }
    zmutex_unlock(&thread_pool_lock);
    return handle;
}

int
os_thread_join(korp_tid thread, void **value_ptr)
{
    os_thread_data *thread_data;
    k_tid_t tid;
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    char *stack;
#endif
    bool uses_user_pool;

    WAMR_JOIN_TEST_HOOK(WAMR_JOIN_TEST_ENTERED);
    WAMR_JOIN_TEST_HOOK(WAMR_JOIN_TEST_BEFORE_CLAIM);
    thread_data = thread_data_list_claim_join(thread);
    if (thread_data == NULL) {
        return BHT_ERROR;
    }
    WAMR_JOIN_TEST_HOOK(WAMR_JOIN_TEST_CLAIMED);

    if (WAMR_THREAD_JOIN(thread_data->tid, K_FOREVER) != 0) {
        zmutex_lock(&thread_pool_lock, K_FOREVER);
        thread_data->join_claimed = false;
        zmutex_unlock(&thread_pool_lock);
        return BHT_ERROR;
    }

    WAMR_JOIN_TEST_HOOK(WAMR_JOIN_TEST_BEFORE_REMOVE);
    zmutex_lock(&thread_pool_lock, K_FOREVER);
    if (value_ptr != NULL) {
        *value_ptr = thread_data->return_value;
    }
    thread_data->state = WAMR_THREAD_JOINED;
    uses_user_pool = thread_data->uses_user_pool;
    tid = thread_data->tid;
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    stack = thread_data->stack;
#endif
    thread_data_list_remove_locked(thread_data);
    thread_slot_set_state_locked(thread_data, WAMR_THREAD_JOINED);
    thread_generation_release_locked(thread_data);
    zmutex_unlock(&thread_pool_lock);

    if (!uses_user_pool) {
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
        WAMR_THREAD_FREE(stack);
#endif
        WAMR_THREAD_FREE((os_thread_obj *)tid);
    }

    WAMR_THREAD_FREE(thread_data);
    return BHT_OK;
}

#if defined(CONFIG_USERSPACE)
enum {
    WAMR_SYNC_TEST_MUTEX_OPERATION_CLAIMED = 1,
};

#if defined(CONFIG_ZTEST)
__weak void
wamr_zephyr_sync_test_hook(int phase, uintptr_t handle)
{
    (void)phase;
    (void)handle;
}
#define WAMR_SYNC_TEST_HOOK(phase, handle) \
    wamr_zephyr_sync_test_hook(phase, (uintptr_t)(handle))
#else
#define WAMR_SYNC_TEST_HOOK(phase, handle) \
    do {                                   \
        (void)(phase);                     \
        (void)(handle);                    \
    } while (0)
#endif

static bool
sync_metadata_lock(void)
{
    int result;

    if (atomic_get(&wamr_sync_pool_prepare_state) != WAMR_SYNC_POOL_PREPARED
        || wamr_sync_pool.management_lock == NULL) {
        return false;
    }
    result = k_mutex_lock(wamr_sync_pool.management_lock, K_FOREVER);
    /* The prepared object is valid and granted to the WAMR owner; a forever
     * lock has no documented recoverable failure in thread context. */
    bh_assert(result == 0);
    return true;
}

static void
sync_metadata_unlock(void)
{
    (void)k_mutex_unlock(wamr_sync_pool.management_lock);
}

static korp_mutex
mutex_handle_alloc_locked(void)
{
    next_mutex_handle++;
    if (next_mutex_handle == 0U) {
        next_mutex_handle++;
    }
    return (korp_mutex)next_mutex_handle;
}

static korp_cond
cond_handle_alloc_locked(void)
{
    next_cond_handle++;
    if (next_cond_handle == 0U) {
        next_cond_handle++;
    }
    return (korp_cond)next_cond_handle;
}

static wamr_mutex_slot_t *
mutex_slot_lookup_locked(korp_mutex handle)
{
    size_t i;

    for (i = 0; i < wamr_sync_pool.mutex_count; i++) {
        if (wamr_mutex_slots[i].state == WAMR_SYNC_SLOT_ACTIVE
            && wamr_mutex_slots[i].handle == handle) {
            return &wamr_mutex_slots[i];
        }
    }
    return NULL;
}

static wamr_cond_slot_t *
cond_slot_lookup_locked(korp_cond handle)
{
    size_t i;

    for (i = 0; i < wamr_sync_pool.condvar_count; i++) {
        if (wamr_cond_slots[i].state == WAMR_SYNC_SLOT_ACTIVE
            && wamr_cond_slots[i].handle == handle) {
            return &wamr_cond_slots[i];
        }
    }
    return NULL;
}

static wamr_cond_slot_t *
cond_slot_lookup_native_locked(struct k_condvar *native)
{
    size_t i;

    for (i = 0; i < wamr_sync_pool.condvar_count; i++) {
        if (wamr_cond_slots[i].native == native) {
            return &wamr_cond_slots[i];
        }
    }
    return NULL;
}

#if defined(CONFIG_ZTEST)
struct k_mutex *
wamr_zephyr_sync_test_native_mutex(korp_mutex handle)
{
    struct k_mutex *native = NULL;
    wamr_mutex_slot_t *slot;

    if (handle == NULL || !sync_metadata_lock()) {
        return NULL;
    }

    slot = mutex_slot_lookup_locked(handle);
    if (slot != NULL) {
        native = slot->native;
    }
    sync_metadata_unlock();
    return native;
}

struct k_condvar *
wamr_zephyr_sync_test_native_condvar(korp_cond handle)
{
    struct k_condvar *native = NULL;
    wamr_cond_slot_t *slot;

    if (handle == NULL || !sync_metadata_lock()) {
        return NULL;
    }

    slot = cond_slot_lookup_locked(handle);
    if (slot != NULL) {
        native = slot->native;
    }
    sync_metadata_unlock();
    return native;
}

int
wamr_zephyr_sync_test_restore_waiting_cond(korp_cond handle,
                                           struct k_condvar *native)
{
    wamr_cond_slot_t *slot;

    if (handle == NULL || native == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    slot = cond_slot_lookup_native_locked(native);
    if (slot == NULL || slot->state != WAMR_SYNC_SLOT_FREE
        || slot->handle != NULL || slot->active_operations != 0U
        || slot->active_waiters != 0U) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    slot->state = WAMR_SYNC_SLOT_ACTIVE;
    slot->handle = handle;
    slot->active_operations = 1U;
    slot->active_waiters = 1U;
    sync_metadata_unlock();
    return BHT_OK;
}
#endif
#endif

int
os_mutex_init(korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    size_t i;
    wamr_mutex_slot_t *slot = NULL;

    if (mutex == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    for (i = 0; i < wamr_sync_pool.mutex_count; i++) {
        if (wamr_mutex_slots[i].state == WAMR_SYNC_SLOT_FREE) {
            slot = &wamr_mutex_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    slot->state = WAMR_SYNC_SLOT_RESERVED;
    slot->handle = mutex_handle_alloc_locked();
    slot->active_operations = 0U;
    slot->owner = NULL;
    slot->recursion = 0U;
    slot->state = WAMR_SYNC_SLOT_ACTIVE;
    *mutex = slot->handle;
    sync_metadata_unlock();
    return BHT_OK;
#else
    zmutex_init(mutex);
    return BHT_OK;
#endif
}

int
os_recursive_mutex_init(korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    return os_mutex_init(mutex);
#else
    zmutex_init(mutex);
    return BHT_OK;
#endif
}

int
os_mutex_destroy(korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    wamr_mutex_slot_t *slot;
    bool finish_thread_shutdown;

    if (mutex == NULL || *mutex == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    slot = mutex_slot_lookup_locked(*mutex);
    if (slot == NULL || slot->active_operations != 0U || slot->owner != NULL
        || slot->recursion != 0U) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    slot->state = WAMR_SYNC_SLOT_DESTROYING;
    slot->handle = NULL;
    slot->active_operations = 0U;
    slot->owner = NULL;
    slot->recursion = 0U;
    *mutex = NULL;
    slot->state = WAMR_SYNC_SLOT_FREE;
    finish_thread_shutdown = atomic_get(&thread_sys_destroy_pending) != 0
                             && !sync_pool_has_active_slots_locked();
    sync_metadata_unlock();
    if (finish_thread_shutdown) {
        os_thread_sys_destroy();
    }
    return BHT_OK;
#else
    (void)mutex;
    return BHT_OK;
#endif
}

int
os_mutex_lock(korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    wamr_mutex_slot_t *slot;
    struct k_mutex *native;
    korp_mutex handle;
    k_tid_t current;
    int result;

    if (mutex == NULL || *mutex == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    handle = *mutex;
    slot = mutex_slot_lookup_locked(handle);
    if (slot == NULL) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }
    slot->active_operations++;
    native = slot->native;
    sync_metadata_unlock();

    WAMR_SYNC_TEST_HOOK(WAMR_SYNC_TEST_MUTEX_OPERATION_CLAIMED, handle);
    result = k_mutex_lock(native, K_FOREVER);
    /* Zephyr's valid-object K_FOREVER mutex lock cannot be busy or time out. */
    bh_assert(result == 0);
    /* WAMR's one-time prepared sync-pool binding survives active operations. */
    bh_assert(sync_metadata_lock());
    slot = mutex_slot_lookup_locked(handle);
    /* Destroy refuses a slot while active_operations is nonzero. */
    bh_assert(slot != NULL && slot->active_operations > 0U);
    current = k_current_get();
    if (slot->owner == current) {
        slot->recursion++;
    }
    else {
        slot->owner = current;
        slot->recursion = 1U;
    }
    slot->active_operations--;
    sync_metadata_unlock();
    return BHT_OK;
#else
    return zmutex_lock(mutex, K_FOREVER);
#endif
}

int
os_mutex_unlock(korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    wamr_mutex_slot_t *slot;
    struct k_mutex *native;
    korp_mutex handle;
    int result;

    if (mutex == NULL || *mutex == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    handle = *mutex;
    slot = mutex_slot_lookup_locked(handle);
    if (slot == NULL || slot->owner != k_current_get()
        || slot->recursion == 0U) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }
    slot->active_operations++;
    slot->recursion--;
    if (slot->recursion == 0U) {
        slot->owner = NULL;
    }
    native = slot->native;
    sync_metadata_unlock();

    result = k_mutex_unlock(native);
    /* WAMR verified current ownership and nonzero recursion before claiming
     * the operation; Zephyr's not-owned/unlocked errors cannot apply. */
    bh_assert(result == 0);
    /* WAMR's one-time prepared sync-pool binding survives active operations. */
    bh_assert(sync_metadata_lock());
    slot = mutex_slot_lookup_locked(handle);
    /* Destroy refuses a slot while active_operations is nonzero. */
    bh_assert(slot != NULL && slot->active_operations > 0U);
    slot->active_operations--;
    sync_metadata_unlock();
    return BHT_OK;
#else
#if KERNEL_VERSION_NUMBER >= 0x020200 /* version 2.2.0 */
    return zmutex_unlock(mutex);
#else
    zmutex_unlock(mutex);
    return 0;
#endif
#endif
}

int
os_cond_init(korp_cond *cond)
{
#if defined(CONFIG_USERSPACE)
    size_t i;
    wamr_cond_slot_t *slot = NULL;

    if (cond == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    for (i = 0; i < wamr_sync_pool.condvar_count; i++) {
        if (wamr_cond_slots[i].state == WAMR_SYNC_SLOT_FREE) {
            slot = &wamr_cond_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    slot->state = WAMR_SYNC_SLOT_RESERVED;
    slot->handle = cond_handle_alloc_locked();
    slot->active_operations = 0U;
    slot->active_waiters = 0U;
    slot->state = WAMR_SYNC_SLOT_ACTIVE;
    *cond = slot->handle;
    sync_metadata_unlock();
    return BHT_OK;
#else
    int result = k_condvar_init(cond);

    /* Zephyr condition initialization returns zero for a valid object. */
    bh_assert(result == 0);
    return BHT_OK;
#endif
}

int
os_cond_destroy(korp_cond *cond)
{
#if defined(CONFIG_USERSPACE)
    wamr_cond_slot_t *slot;
    bool finish_thread_shutdown;

    if (cond == NULL || *cond == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    slot = cond_slot_lookup_locked(*cond);
    if (slot == NULL || slot->active_operations != 0U
        || slot->active_waiters != 0U) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    slot->state = WAMR_SYNC_SLOT_DESTROYING;
    slot->handle = NULL;
    slot->active_operations = 0U;
    slot->active_waiters = 0U;
    *cond = NULL;
    slot->state = WAMR_SYNC_SLOT_FREE;
    finish_thread_shutdown = atomic_get(&thread_sys_destroy_pending) != 0
                             && !sync_pool_has_active_slots_locked();
    sync_metadata_unlock();
    if (finish_thread_shutdown) {
        os_thread_sys_destroy();
    }
    return BHT_OK;
#else
    (void)cond;
    return BHT_OK;
#endif
}

static int
relock_condvar_timeout_mutex_if_needed(struct k_mutex *mutex, int wait_result)
{
#if KERNEL_VERSION_NUMBER >= ZEPHYR_VERSION(4, 4, 0) \
    && KERNEL_VERSION_NUMBER < ZEPHYR_VERSION(4, 5, 0)
    /*
     * Zephyr 4.4.x returns -EAGAIN on timeout without re-locking the mutex,
     * even though the public k_condvar_wait() contract says the mutex is
     * reacquired before the wait call returns. Upstream commit 5c6c6837cc4
     * fixes this after the 4.4 release line. Keep the workaround bounded to
     * 4.4.x so older releases and fixed 4.5+ releases cannot double-lock.
     */
    if (wait_result == -EAGAIN) {
        return k_mutex_lock(mutex, K_FOREVER);
    }
#else
    ARG_UNUSED(mutex);
    ARG_UNUSED(wait_result);
#endif

    return 0;
}

#if defined(CONFIG_USERSPACE)
static int
os_cond_wait_user(korp_cond *cond, korp_mutex *mutex, k_timeout_t timeout,
                  bool timed)
{
    wamr_cond_slot_t *cond_slot;
    wamr_mutex_slot_t *mutex_slot;
    struct k_condvar *native_cond;
    struct k_mutex *native_mutex;
    korp_cond cond_handle;
    korp_mutex mutex_handle;
    bool mutex_reacquired = true;
    int result;

    if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL
        || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    cond_handle = *cond;
    mutex_handle = *mutex;
    cond_slot = cond_slot_lookup_locked(cond_handle);
    mutex_slot = mutex_slot_lookup_locked(mutex_handle);
    if (cond_slot == NULL || mutex_slot == NULL
        || mutex_slot->owner != k_current_get()
        || mutex_slot->recursion != 1U) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }

    cond_slot->active_operations++;
    cond_slot->active_waiters++;
    mutex_slot->active_operations++;
    mutex_slot->owner = NULL;
    mutex_slot->recursion = 0U;
    native_cond = cond_slot->native;
    native_mutex = mutex_slot->native;
    sync_metadata_unlock();

    result = k_condvar_wait(native_cond, native_mutex, timeout);
    if (relock_condvar_timeout_mutex_if_needed(native_mutex, result) != 0) {
        mutex_reacquired = false;
    }
    /* WAMR's one-time prepared sync-pool binding survives active operations. */
    bh_assert(sync_metadata_lock());
    cond_slot = cond_slot_lookup_locked(cond_handle);
    mutex_slot = mutex_slot_lookup_locked(mutex_handle);
    /* Condition destroy refuses active operations and waiters; mutex destroy
     * refuses the paired active operation throughout the native wait. */
    bh_assert(cond_slot != NULL && cond_slot->active_operations > 0U
              && cond_slot->active_waiters > 0U);
    bh_assert(mutex_slot != NULL && mutex_slot->active_operations > 0U);

    mutex_slot->owner = mutex_reacquired ? k_current_get() : NULL;
    mutex_slot->recursion = mutex_reacquired ? 1U : 0U;
    mutex_slot->active_operations--;
    cond_slot->active_waiters--;
    cond_slot->active_operations--;
    sync_metadata_unlock();

    if (!mutex_reacquired) {
        return BHT_ERROR;
    }
    /* Zephyr condition waits return zero or timed -EAGAIN; K_FOREVER cannot
     * time out, and the version-bounded mutex relock has succeeded. */
    bh_assert(result == 0 || (timed && result == -EAGAIN));
    return result == 0 ? BHT_OK : ETIMEDOUT;
}
#endif

int
os_cond_wait(korp_cond *cond, korp_mutex *mutex)
{
#if defined(CONFIG_USERSPACE)
    return os_cond_wait_user(cond, mutex, K_FOREVER, false);
#else
    int result = k_condvar_wait(cond, mutex, K_FOREVER);

    /* Zephyr's valid-object K_FOREVER condition wait cannot time out. */
    bh_assert(result == 0);
    return BHT_OK;
#endif
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{
#if defined(CONFIG_USERSPACE)
    uint64 mills_64;
    int32 mills;

    if (useconds == BHT_WAIT_FOREVER) {
        return os_cond_wait_user(cond, mutex, K_FOREVER, false);
    }

    mills_64 = useconds / 1000U;
    if (mills_64 < (uint64)INT32_MAX) {
        mills = (int32)mills_64;
    }
    else {
        mills = INT32_MAX;
        os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                  "set to max timeout instead\n");
    }
    return os_cond_wait_user(cond, mutex, Z_TIMEOUT_MS(mills), true);
#else
    if (useconds == BHT_WAIT_FOREVER) {
        int result = k_condvar_wait(cond, mutex, K_FOREVER);

        /* Zephyr's valid-object K_FOREVER condition wait cannot time out. */
        bh_assert(result == 0);
        return BHT_OK;
    }
    else {
        uint64 mills_64 = useconds / 1000;
        int32 mills;
        int result;

        if (mills_64 < (uint64)INT32_MAX) {
            mills = (int32)mills_64;
        }
        else {
            mills = INT32_MAX;
            os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                      "set to max timeout instead\n");
        }
        result = k_condvar_wait(cond, mutex, Z_TIMEOUT_MS(mills));
        if (relock_condvar_timeout_mutex_if_needed(mutex, result) != 0) {
            return BHT_ERROR;
        }
        /* Zephyr's timed condition wait returns zero or -EAGAIN after the
         * version-bounded mutex relock has succeeded. */
        bh_assert(result == 0 || result == -EAGAIN);
        return BHT_OK;
    }
#endif
}

int
os_cond_signal(korp_cond *cond)
{
#if defined(CONFIG_USERSPACE)
    wamr_cond_slot_t *slot;
    struct k_condvar *native;
    korp_cond handle;
    int result;

    if (cond == NULL || *cond == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    handle = *cond;
    slot = cond_slot_lookup_locked(handle);
    if (slot == NULL) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }
    slot->active_operations++;
    native = slot->native;
    sync_metadata_unlock();

    result = k_condvar_signal(native);
    /* Zephyr condition signal returns zero for the claimed valid object. */
    bh_assert(result == 0);
    /* WAMR's one-time prepared sync-pool binding survives active operations. */
    bh_assert(sync_metadata_lock());
    slot = cond_slot_lookup_locked(handle);
    /* Destroy refuses a slot while active_operations is nonzero. */
    bh_assert(slot != NULL && slot->active_operations > 0U);
    slot->active_operations--;
    sync_metadata_unlock();
    return BHT_OK;
#else
    int result = k_condvar_signal(cond);

    /* Zephyr condition signal returns zero for a valid object. */
    bh_assert(result == 0);
    return BHT_OK;
#endif
}

uint8 *
os_thread_get_stack_boundary()
{
#if defined(CONFIG_THREAD_STACK_INFO) && !defined(CONFIG_USERSPACE)
    k_tid_t thread = k_current_get();
    return (uint8 *)thread->stack_info.start;
#else
    return NULL;
#endif
}

void
os_thread_jit_write_protect_np(bool enabled)
{
}

int
os_rwlock_init(korp_rwlock *lock)
{
    if (!lock) {
        return BHT_ERROR;
    }

    k_mutex_init(&lock->mtx);
    k_sem_init(&lock->sem, 0, K_SEM_MAX_LIMIT);
    lock->read_count = 0;

    return BHT_OK;
}

int
os_rwlock_rdlock(korp_rwlock *lock)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_rwlock_wrlock(korp_rwlock *lock)
{
    // Acquire the mutex to ensure exclusive access
    if (k_mutex_lock(&lock->mtx, K_FOREVER) != 0) {
        return BHT_ERROR;
    }

    // Wait until there are no readers
    while (lock->read_count > 0) {
        // Release the mutex while we're waiting
        k_mutex_unlock(&lock->mtx);

        // Wait for a short time
        k_sleep(K_MSEC(1));

        // Re-acquire the mutex
        if (k_mutex_lock(&lock->mtx, K_FOREVER) != 0) {
            return BHT_ERROR;
        }
    }
    // At this point, we hold the mutex and there are no readers, so we have the
    // write lock
    return BHT_OK;
}

int
os_rwlock_unlock(korp_rwlock *lock)
{
    k_mutex_unlock(&lock->mtx);
    return BHT_OK;
}

int
os_rwlock_destroy(korp_rwlock *lock)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_thread_detach(korp_tid thread)
{
    os_thread_data *thread_data;
    os_thread_data *exited_thread_data = NULL;
    int result = BHT_ERROR;

    zmutex_lock(&thread_pool_lock, K_FOREVER);
    thread_data = thread_data_list_lookup_handle_locked(thread);
    if (thread_data != NULL) {
        if (!thread_data->join_claimed
            && thread_data->state == WAMR_THREAD_RUNNING) {
            thread_data->state = WAMR_THREAD_DETACHED_RUNNING;
            thread_slot_set_state_locked(thread_data,
                                         WAMR_THREAD_DETACHED_RUNNING);
            result = BHT_OK;
        }
        else if (!thread_data->join_claimed
                 && thread_data->state == WAMR_THREAD_EXITED) {
            thread_data->state = WAMR_THREAD_DETACHED_RUNNING;
            thread_slot_set_state_locked(thread_data,
                                         WAMR_THREAD_DETACHED_RUNNING);
            thread_data_list_remove_locked(thread_data);
            exited_thread_data = thread_data;
            result = BHT_OK;
        }
        else if (thread_data->state == WAMR_THREAD_DETACHED_RUNNING) {
            result = BHT_OK;
        }
    }
    zmutex_unlock(&thread_pool_lock);

    if (exited_thread_data != NULL) {
        int join_result;

        WAMR_DETACH_TEST_HOOK(WAMR_DETACH_TEST_EXITED_CLAIMED);
        join_result = k_thread_join(exited_thread_data->tid, K_FOREVER);
        /* The target was observed EXITED under the pool lock; the caller cannot
         * be that exited target, and K_FOREVER cannot report busy or timeout. */
        bh_assert(join_result == 0);
        detached_thread_data_release(exited_thread_data);
    }
    return result;
}

void
os_thread_exit(void *retval)
{
    os_thread_complete(retval);
    k_thread_abort(k_current_get());
}

int
os_cond_broadcast(korp_cond *cond)
{
#if defined(CONFIG_USERSPACE)
    wamr_cond_slot_t *slot;
    struct k_condvar *native;
    korp_cond handle;
    int result;

    if (cond == NULL || *cond == NULL || !sync_metadata_lock()) {
        return BHT_ERROR;
    }

    handle = *cond;
    slot = cond_slot_lookup_locked(handle);
    if (slot == NULL) {
        sync_metadata_unlock();
        return BHT_ERROR;
    }
    slot->active_operations++;
    native = slot->native;
    sync_metadata_unlock();

    result = k_condvar_broadcast(native);
    /* Zephyr broadcast returns a nonnegative waiter count for a valid object. */
    bh_assert(result >= 0);
    /* WAMR's one-time prepared sync-pool binding survives active operations. */
    bh_assert(sync_metadata_lock());
    slot = cond_slot_lookup_locked(handle);
    /* Destroy refuses a slot while active_operations is nonzero. */
    bh_assert(slot != NULL && slot->active_operations > 0U);
    slot->active_operations--;
    sync_metadata_unlock();
    return BHT_OK;
#else
    int result = k_condvar_broadcast(cond);

    /* Zephyr broadcast returns a nonnegative waiter count for a valid object. */
    bh_assert(result >= 0);
    return BHT_OK;
#endif
}

korp_sem *
os_sem_open(const char *name, int oflags, int mode, int val)
{
    /* Not implemented */
    return NULL;
}

int
os_sem_close(korp_sem *sem)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_sem_wait(korp_sem *sem)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_sem_trywait(korp_sem *sem)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_sem_post(korp_sem *sem)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_sem_getvalue(korp_sem *sem, int *sval)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_sem_unlink(const char *name)
{
    /* Not implemented */
    return BHT_ERROR;
}

int
os_blocking_op_init()
{
    /* Not implemented */
    return BHT_ERROR;
}

void
os_begin_blocking_op()
{
    /* Not implemented */
}

void
os_end_blocking_op()
{
    /* Not implemented */
}

int
os_wakeup_blocking_op(korp_tid tid)
{
    /* Not implemented */
    return BHT_ERROR;
}
