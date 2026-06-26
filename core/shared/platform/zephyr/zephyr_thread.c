/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "zephyr_thread_internal.h"

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
#if !defined(BH_ZEPHYR_MPU_STACK_SIZE)
#define BH_ZEPHYR_MPU_STACK_SIZE APP_THREAD_STACK_SIZE_MIN
#endif
#if !defined(BH_ZEPHYR_MPU_STACK_COUNT)
#define BH_ZEPHYR_MPU_STACK_COUNT 4
#endif

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
static K_THREAD_STACK_ARRAY_DEFINE(mpu_stacks, BH_ZEPHYR_MPU_STACK_COUNT,
                                   BH_ZEPHYR_MPU_STACK_SIZE);
static bool mpu_stack_allocated[BH_ZEPHYR_MPU_STACK_COUNT];
static zmutex_t mpu_stack_lock;

static char *
mpu_stack_alloc()
{
    int i;

    zmutex_lock(&mpu_stack_lock, K_FOREVER);
    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        if (!mpu_stack_allocated[i]) {
            mpu_stack_allocated[i] = true;
            zmutex_unlock(&mpu_stack_lock);
            return (char *)mpu_stacks[i];
        }
    }
    zmutex_unlock(&mpu_stack_lock);
    return NULL;
}

static void
mpu_stack_free(char *stack)
{
    int i;

    zmutex_lock(&mpu_stack_lock, K_FOREVER);
    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        if ((char *)mpu_stacks[i] == stack)
            mpu_stack_allocated[i] = false;
    }
    zmutex_unlock(&mpu_stack_lock);
}

/*
 * Helper consumed by zephyr_thread_usermode.c: exposes addresses of the
 * static MPU stack array without making the array itself externally visible.
 */
void *
mpu_stack_addr(int i)
{
    if (i < 0 || i >= BH_ZEPHYR_MPU_STACK_COUNT)
        return NULL;
    return (void *)mpu_stacks[i];
}
#endif

typedef struct os_thread_wait_node {
    zsem_t sem;
    os_thread_wait_list next;
} os_thread_wait_node;

typedef struct os_thread_data {
    /* Next thread data */
    struct os_thread_data *next;
    /* Zephyr thread handle */
    korp_tid tid;
    /* Jeff thread local root */
    void *tlr;
    /* Lock for waiting list */
    zmutex_t wait_list_lock;
    /* Waiting list of other threads who are joining this thread */
    os_thread_wait_list thread_wait_list;
    /* Whether the thread has exited (set by os_thread_cleanup) */
    bool thread_exited;
    /* Thread stack size */
    unsigned stack_size;
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    /* Thread stack */
    char stack[1];
#else
    char *stack;
#endif
} os_thread_data;

/*
 * Unified thread-tracking node.
 *
 * Kernel-mode path: this node *is* the k_thread storage. The union puts
 * struct k_thread at offset 0 so (korp_tid)&node->thread round-trips
 * back to the node pointer.
 *
 * User-mode path (WAMR_BUILD_ZEPHYR_USERMODE_MT): the k_thread is
 * allocated separately via k_object_alloc and referenced via dyn_tid.
 * is_dyn discriminates the two branches.
 */
typedef struct thread_obj_node {
    union {
        struct k_thread thread; /* used when is_dyn == false */
        korp_tid dyn_tid;       /* used when is_dyn == true  */
    };
    /* Whether the thread is terminated and this node is to be freed
     * in the future. */
    bool to_be_freed;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    bool is_dyn;
#endif
    struct thread_obj_node *next;
} thread_obj_node;

static bool is_thread_sys_inited = false;

/* Thread data of supervisor thread */
static os_thread_data supervisor_thread_data;

/* Lock for thread data list */
static zmutex_t thread_data_lock;

/* Thread data list */
static os_thread_data *thread_data_list = NULL;

/* Lock for thread object list */
static zmutex_t thread_obj_lock;

/* Thread object list (unified for kernel-mode and user-mode threads) */
static thread_obj_node *thread_obj_list = NULL;

static void
thread_data_list_add(os_thread_data *thread_data)
{
    zmutex_lock(&thread_data_lock, K_FOREVER);
    if (!thread_data_list)
        thread_data_list = thread_data;
    else {
        /* If already in list, just return */
        os_thread_data *p = thread_data_list;
        while (p) {
            if (p == thread_data) {
                zmutex_unlock(&thread_data_lock);
                return;
            }
            p = p->next;
        }

        /* Set as head of list */
        thread_data->next = thread_data_list;
        thread_data_list = thread_data;
    }
    zmutex_unlock(&thread_data_lock);
}

static void
thread_data_list_remove(os_thread_data *thread_data)
{
    zmutex_lock(&thread_data_lock, K_FOREVER);
    if (thread_data_list) {
        if (thread_data_list == thread_data)
            thread_data_list = thread_data_list->next;
        else {
            /* Search and remove it from list */
            os_thread_data *p = thread_data_list;
            while (p && p->next != thread_data)
                p = p->next;
            if (p && p->next == thread_data)
                p->next = p->next->next;
        }
    }
    zmutex_unlock(&thread_data_lock);
}

static void thread_obj_list_mark_freed(korp_tid tid);

/* Release all resources associated with exited thread data.
 * Called by os_thread_join after the join is complete, or during
 * periodic cleanup of threads that exited without being joined. */
static void
thread_data_destroy(os_thread_data *thread_data)
{
    thread_data_list_remove(thread_data);
    /* Mark the matching node for deferred reclaim. Works for both
     * kernel-mode (tid is &node->thread) and user-mode (we look up
     * the node by its dyn_tid). */
    thread_obj_list_mark_freed(thread_data->tid);
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    mpu_stack_free(thread_data->stack);
#endif
    BH_FREE(thread_data);
}

static os_thread_data *
thread_data_list_lookup(k_tid_t tid)
{
    zmutex_lock(&thread_data_lock, K_FOREVER);
    if (thread_data_list) {
        os_thread_data *p = thread_data_list;
        while (p) {
            if (p->tid == tid) {
                /* Found */
                zmutex_unlock(&thread_data_lock);
                return p;
            }
            p = p->next;
        }
    }
    zmutex_unlock(&thread_data_lock);
    return NULL;
}

static void
thread_obj_list_add_node(thread_obj_node *node)
{
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    if (!thread_obj_list)
        thread_obj_list = node;
    else {
        /* Set as head of list */
        node->next = thread_obj_list;
        thread_obj_list = node;
    }
    zmutex_unlock(&thread_obj_lock);
}

static void
thread_obj_list_mark_freed(korp_tid tid)
{
    thread_obj_node *p;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p = thread_obj_list;
    while (p) {
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
        bool match = p->is_dyn ? (p->dyn_tid == tid)
                               : ((korp_tid)&p->thread == tid);
#else
        bool match = ((korp_tid)&p->thread == tid);
#endif
        if (match) {
            p->to_be_freed = true;
            break;
        }
        p = p->next;
    }
    zmutex_unlock(&thread_obj_lock);
}

static void
thread_obj_list_reclaim(void)
{
    thread_obj_node *p, *p_prev;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p_prev = NULL;
    p = thread_obj_list;
    while (p) {
        if (p->to_be_freed) {
            thread_obj_node *next = p->next;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
            if (p->is_dyn)
                dyn_thread_release(p->dyn_tid);
#endif
            BH_FREE(p);
            if (p_prev == NULL)
                thread_obj_list = next;
            else
                p_prev->next = next;
            p = next;
        }
        else {
            p_prev = p;
            p = p->next;
        }
    }
    zmutex_unlock(&thread_obj_lock);
}

int
os_thread_sys_init()
{
    if (is_thread_sys_inited)
        return BHT_OK;

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    zmutex_init(&mpu_stack_lock);
#endif
    zmutex_init(&thread_data_lock);
    zmutex_init(&thread_obj_lock);

    /* Initialize supervisor thread data */
    memset(&supervisor_thread_data, 0, sizeof(supervisor_thread_data));
    supervisor_thread_data.tid = k_current_get();
    /* Set as head of thread data list */
    thread_data_list = &supervisor_thread_data;

    is_thread_sys_inited = true;
    return BHT_OK;
}

void
os_thread_sys_destroy(void)
{
    if (is_thread_sys_inited) {
        is_thread_sys_inited = false;
    }
}

static os_thread_data *
thread_data_current()
{
    k_tid_t tid = k_current_get();
    return thread_data_list_lookup(tid);
}

static void
os_thread_cleanup(void)
{
    os_thread_data *thread_data = thread_data_current();

    bh_assert(thread_data != NULL);
    zmutex_lock(&thread_data->wait_list_lock, K_FOREVER);
    /* Mark as exited so late os_thread_join callers can detect it */
    thread_data->thread_exited = true;
    if (thread_data->thread_wait_list) {
        /* Signal each joining thread */
        os_thread_wait_list head = thread_data->thread_wait_list;
        while (head) {
            os_thread_wait_list next = head->next;
            zsem_give(&head->sem);
            /* head will be freed by joining thread */
            head = next;
        }
        thread_data->thread_wait_list = NULL;
    }
    zmutex_unlock(&thread_data->wait_list_lock);

    /* Don't remove thread_data here — os_thread_join may still need to
     * find it via thread_data_list_lookup. The joiner cleans it up via
     * thread_data_destroy() after synchronization.
     *
     * Caveat: a thread that exits without ever being joined leaves its
     * thread_data in the list. Both in-tree samples always join workers
     * they spawn, so this is not exercised, but consumers that detach
     * threads or fire-and-forget worker pools must arrange their own join
     * eventually. os_thread_detach() is currently a no-op and does not
     * solve this. */
}

static void
os_thread_wrapper(void *start, void *arg, void *thread_data)
{
    /* Set thread custom data */
    ((os_thread_data *)thread_data)->tid = k_current_get();
    thread_data_list_add(thread_data);

    ((thread_start_routine_t)start)(arg);
    os_thread_cleanup();
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
    korp_tid tid;
    thread_obj_node *node;
    os_thread_data *thread_data;
    unsigned thread_data_size;
    int options = 0;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    bool is_user_mode = k_is_user_context();
#endif

    if (!p_tid || !stack_size)
        return BHT_ERROR;

    /* Free the thread objects of terminated threads */
    thread_obj_list_reclaim();

    /* Allocate the unified tracking node. */
    if (!(node = BH_MALLOC(sizeof(thread_obj_node))))
        return BHT_ERROR;
    memset(node, 0, sizeof(*node));

    /* Decide which arm of the union backs the k_thread:
     *   kernel-mode: the embedded struct k_thread at offset 0 of node.
     *   user-mode  : a k_object_alloc'd k_thread referenced via dyn_tid. */
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (is_user_mode) {
        node->is_dyn = true;
        if (!(node->dyn_tid = dyn_thread_alloc())) {
            BH_FREE(node);
            return BHT_ERROR;
        }
        tid = node->dyn_tid;
        options = K_USER | K_INHERIT_PERMS;
    }
    else
#endif
    {
        tid = (korp_tid)&node->thread;
    }

    /* Create and initialize thread data */
#if BH_ENABLE_ZEPHYR_MPU_STACK == 0
    if (stack_size < APP_THREAD_STACK_SIZE_MIN)
        stack_size = APP_THREAD_STACK_SIZE_MIN;
    thread_data_size = offsetof(os_thread_data, stack) + stack_size;
#else
    stack_size = BH_ZEPHYR_MPU_STACK_SIZE;
    thread_data_size = sizeof(os_thread_data);
#endif
    if (!(thread_data = BH_MALLOC(thread_data_size))) {
        goto fail1;
    }

    memset(thread_data, 0, thread_data_size);
    zmutex_init(&thread_data->wait_list_lock);
    thread_data->stack_size = stack_size;
    thread_data->tid = tid;

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    if (!(thread_data->stack = mpu_stack_alloc())) {
        goto fail2;
    }
#endif

    /* Create the thread */
    if (!((tid = k_thread_create(tid, (k_thread_stack_t *)thread_data->stack,
                                 stack_size, os_thread_wrapper, start, arg,
                                 thread_data, prio, options, K_NO_WAIT)))) {
        goto fail3;
    }

    bh_assert(tid == thread_data->tid);

    k_thread_name_set(tid, "wasm-zephyr");

    /* Set thread custom data */
    thread_data_list_add(thread_data);
    thread_obj_list_add_node(node);
    *p_tid = tid;
    return BHT_OK;

fail3:
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    mpu_stack_free(thread_data->stack);
fail2:
#endif
    BH_FREE(thread_data);
fail1:
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (node->is_dyn)
        dyn_thread_release(node->dyn_tid);
#endif
    BH_FREE(node);
    return BHT_ERROR;
}

korp_tid
os_self_thread()
{
    return (korp_tid)k_current_get();
}

/* Note: not safe to call from multiple threads on the same korp_tid.
 * Concurrent joins after the target exits will double-free thread_data. */
int
os_thread_join(korp_tid thread, void **value_ptr)
{
    (void)value_ptr;
    os_thread_data *thread_data;
    os_thread_wait_node *node;

    /* Get thread data */
    thread_data = thread_data_list_lookup(thread);

    if (thread_data == NULL) {
        /* Thread was never created via os_thread_create, or thread_data
         * was already cleaned up by a previous join. */
        return BHT_OK;
    }

    zmutex_lock(&thread_data->wait_list_lock, K_FOREVER);
    if (thread_data->thread_exited) {
        /* Thread already exited — no need to wait, just clean up */
        zmutex_unlock(&thread_data->wait_list_lock);
        thread_data_destroy(thread_data);
        return BHT_OK;
    }
    zmutex_unlock(&thread_data->wait_list_lock);

    /* Create wait node and append it to wait list */
    if (!(node = BH_MALLOC(sizeof(os_thread_wait_node))))
        return BHT_ERROR;

    zsem_init(&node->sem, 0, 1);
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (!node->sem) {
        BH_FREE(node);
        return BHT_ERROR;
    }
    /* Grant target thread access so os_thread_cleanup can signal */
    k_object_access_grant(node->sem, thread);
#endif
    node->next = NULL;

    zmutex_lock(&thread_data->wait_list_lock, K_FOREVER);
    if (thread_data->thread_exited) {
        /* Thread exited between our check and lock — clean up directly */
        zmutex_unlock(&thread_data->wait_list_lock);
        zsem_destroy(&node->sem);
        BH_FREE(node);
        thread_data_destroy(thread_data);
        return BHT_OK;
    }
    if (!thread_data->thread_wait_list)
        thread_data->thread_wait_list = node;
    else {
        /* Add to end of waiting list */
        os_thread_wait_node *p = thread_data->thread_wait_list;
        while (p->next)
            p = p->next;
        p->next = node;
    }
    zmutex_unlock(&thread_data->wait_list_lock);

    /* Wait the sem */
    zsem_take(&node->sem, K_FOREVER);

    /* Wait some time for the thread to be actually terminated */
    k_sleep(Z_TIMEOUT_MS(100));

    /* Clean up thread resources and the join node */
    thread_data_destroy(thread_data);
    zsem_destroy(&node->sem);
    BH_FREE(node);
    return BHT_OK;
}

int
os_mutex_init(korp_mutex *mutex)
{
    zmutex_init(mutex);
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (!*mutex)
        return BHT_ERROR;
#endif
    return BHT_OK;
}

int
os_recursive_mutex_init(korp_mutex *mutex)
{
    zmutex_init(mutex);
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (!*mutex)
        return BHT_ERROR;
#endif
    return BHT_OK;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    zmutex_destroy(mutex);
    return BHT_OK;
}

int
os_mutex_lock(korp_mutex *mutex)
{
    return zmutex_lock(mutex, K_FOREVER);
}

int
os_mutex_unlock(korp_mutex *mutex)
{
#if KERNEL_VERSION_NUMBER >= 0x020200 /* version 2.2.0 */
    return zmutex_unlock(mutex);
#else
    zmutex_unlock(mutex);
    return 0;
#endif
}

int
os_cond_init(korp_cond *cond)
{
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    cond->condvar = k_object_alloc(K_OBJ_CONDVAR);
    if (!cond->condvar)
        return BHT_ERROR;
    k_condvar_init(cond->condvar);
#else
    zmutex_init(&cond->wait_list_lock);
    cond->thread_wait_list = NULL;
#endif
    return BHT_OK;
}

int
os_cond_destroy(korp_cond *cond)
{
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (cond->condvar) {
        k_object_release(cond->condvar);
        cond->condvar = NULL;
    }
#endif
    return BHT_OK;
}

static int
os_cond_wait_internal(korp_cond *cond, korp_mutex *mutex, bool timed, int mills)
{
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    /* k_condvar_wait atomically unlocks mutex, blocks, re-locks on wake.
     * mutex is korp_mutex* = struct k_mutex**, so *mutex = struct k_mutex*. */
    k_timeout_t timeout = timed ? Z_TIMEOUT_MS(mills) : K_FOREVER;
    int ret = k_condvar_wait(cond->condvar, *mutex, timeout);
    return (ret == 0 || ret == -EAGAIN) ? BHT_OK : BHT_ERROR;
#else
    os_thread_wait_node *node;

    /* Create wait node and append it to wait list */
    if (!(node = BH_MALLOC(sizeof(os_thread_wait_node))))
        return BHT_ERROR;

    zsem_init(&node->sem, 0, 1);
    node->next = NULL;

    zmutex_lock(&cond->wait_list_lock, K_FOREVER);
    if (!cond->thread_wait_list)
        cond->thread_wait_list = node;
    else {
        /* Add to end of wait list */
        os_thread_wait_node *p = cond->thread_wait_list;
        while (p->next)
            p = p->next;
        p->next = node;
    }
    zmutex_unlock(&cond->wait_list_lock);

    /* Unlock mutex, wait sem and lock mutex again */
    zmutex_unlock(mutex);
    zsem_take(&node->sem, timed ? Z_TIMEOUT_MS(mills) : K_FOREVER);
    zmutex_lock(mutex, K_FOREVER);

    /* Remove wait node from wait list */
    zmutex_lock(&cond->wait_list_lock, K_FOREVER);
    if (cond->thread_wait_list == node)
        cond->thread_wait_list = node->next;
    else {
        /* Remove from the wait list */
        os_thread_wait_node *p = cond->thread_wait_list;
        while (p->next != node)
            p = p->next;
        p->next = node->next;
    }
    BH_FREE(node);
    zmutex_unlock(&cond->wait_list_lock);

    return BHT_OK;
#endif /* WAMR_BUILD_ZEPHYR_USERMODE_MT */
}

int
os_cond_wait(korp_cond *cond, korp_mutex *mutex)
{
    return os_cond_wait_internal(cond, mutex, false, 0);
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{

    if (useconds == BHT_WAIT_FOREVER) {
        return os_cond_wait_internal(cond, mutex, false, 0);
    }
    else {
        uint64 mills_64 = useconds / 1000;
        int32 mills;

        if (mills_64 < (uint64)INT32_MAX) {
            mills = (int32)mills_64;
        }
        else {
            mills = INT32_MAX;
            os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                      "set to max timeout instead\n");
        }
        return os_cond_wait_internal(cond, mutex, true, mills);
    }
}

int
os_cond_signal(korp_cond *cond)
{
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    k_condvar_signal(cond->condvar);
#else
    /* Signal the head wait node of wait list */
    zmutex_lock(&cond->wait_list_lock, K_FOREVER);
    if (cond->thread_wait_list)
        zsem_give(&cond->thread_wait_list->sem);
    zmutex_unlock(&cond->wait_list_lock);
#endif
    return BHT_OK;
}

uint8 *
os_thread_get_stack_boundary()
{
#if defined(CONFIG_THREAD_STACK_INFO) && !defined(CONFIG_USERSPACE)
    korp_tid thread = k_current_get();
    return (uint8 *)thread->stack_info.start;
#else
    return NULL;
#endif
}

void
os_thread_jit_write_protect_np(bool enabled)
{}

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
    /* No-op: this platform does not implement detach semantics.
     * thread_data is reclaimed only via os_thread_join. Avoid
     * detaching if leak-free shutdown matters to the consumer. */
    (void)thread;
    return BHT_OK;
}

void
os_thread_exit(void *retval)
{
    (void)retval;
    os_thread_cleanup();
    k_thread_abort(k_current_get());
}

int
os_cond_broadcast(korp_cond *cond)
{
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    k_condvar_broadcast(cond->condvar);
#else
    os_thread_wait_node *node;
    zmutex_lock(&cond->wait_list_lock, K_FOREVER);
    node = cond->thread_wait_list;
    while (node) {
        os_thread_wait_node *next = node->next;
        zsem_give(&node->sem);
        node = next;
    }
    zmutex_unlock(&cond->wait_list_lock);
#endif
    return BHT_OK;
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