/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

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

typedef struct os_thread_obj {
    struct k_thread thread;
    /* Whether the thread is terminated and this thread object is to
     be freed in the future. */
    bool to_be_freed;
    struct os_thread_obj *next;
} os_thread_obj;

static bool is_thread_sys_inited = false;

/* Thread data of supervisor thread */
static os_thread_data supervisor_thread_data;

/* Lock for thread data list */
static zmutex_t thread_data_lock;

/* Thread data list */
static os_thread_data *thread_data_list = NULL;

/* Lock for thread object list */
static zmutex_t thread_obj_lock;

/* Thread object list */
static os_thread_obj *thread_obj_list = NULL;

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

#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
static void dyn_thread_mark_freed(korp_tid tid);
#endif

/* Release all resources associated with exited thread data.
 * Called by os_thread_join after the join is complete, or during
 * periodic cleanup of threads that exited without being joined. */
static void
thread_data_destroy(os_thread_data *thread_data)
{
    thread_data_list_remove(thread_data);
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (k_is_user_context())
        dyn_thread_mark_freed(thread_data->tid);
    else
#endif
        ((os_thread_obj *)thread_data->tid)->to_be_freed = true;
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
thread_obj_list_add(os_thread_obj *thread_obj)
{
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    if (!thread_obj_list)
        thread_obj_list = thread_obj;
    else {
        /* Set as head of list */
        thread_obj->next = thread_obj_list;
        thread_obj_list = thread_obj;
    }
    zmutex_unlock(&thread_obj_lock);
}

static void
thread_obj_list_reclaim()
{
    os_thread_obj *p, *p_prev;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p_prev = NULL;
    p = thread_obj_list;
    while (p) {
        if (p->to_be_freed) {
            if (p_prev == NULL) { /* p is the head of list */
                thread_obj_list = p->next;
                BH_FREE(p);
                p = thread_obj_list;
            }
            else { /* p is not the head of list */
                p_prev->next = p->next;
                BH_FREE(p);
                p = p_prev->next;
            }
        }
        else {
            p_prev = p;
            p = p->next;
        }
    }
    zmutex_unlock(&thread_obj_lock);
}

#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
/*
 * Under user mode, k_thread objects must be allocated via k_object_alloc
 * rather than BH_MALLOC, so they aren't wrapped in os_thread_obj. Track
 * them separately for deferred cleanup (k_object_release).
 */
struct dyn_thread_node {
    korp_tid tid;
    bool to_be_freed;
    struct dyn_thread_node *next;
};

static struct dyn_thread_node *dyn_thread_list = NULL;

static void
dyn_thread_list_add(korp_tid tid)
{
    struct dyn_thread_node *node = BH_MALLOC(sizeof(struct dyn_thread_node));
    if (node) {
        node->tid = tid;
        node->to_be_freed = false;
        zmutex_lock(&thread_obj_lock, K_FOREVER);
        node->next = dyn_thread_list;
        dyn_thread_list = node;
        zmutex_unlock(&thread_obj_lock);
    }
}

static void
dyn_thread_mark_freed(korp_tid tid)
{
    struct dyn_thread_node *p;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p = dyn_thread_list;
    while (p) {
        if (p->tid == tid) {
            p->to_be_freed = true;
            break;
        }
        p = p->next;
    }
    zmutex_unlock(&thread_obj_lock);
}

static void
dyn_thread_list_reclaim(void)
{
    struct dyn_thread_node *p, *prev;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    prev = NULL;
    p = dyn_thread_list;
    while (p) {
        if (p->to_be_freed) {
            struct dyn_thread_node *next = p->next;
            k_object_release(p->tid);
            BH_FREE(p);
            if (prev)
                prev->next = next;
            else
                dyn_thread_list = next;
            p = next;
        }
        else {
            prev = p;
            p = p->next;
        }
    }
    zmutex_unlock(&thread_obj_lock);
}
#endif /* CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS */

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

#if defined(CONFIG_USERSPACE)
/*
 * Prepare a user-mode thread to call os_thread_create().
 *
 * os_thread_create uses static MPU-aligned stacks (mpu_stacks[]) defined
 * via K_THREAD_STACK_ARRAY_DEFINE. These are Zephyr kernel objects.
 * When a user-mode thread calls k_thread_create(), Zephyr's syscall
 * validation checks that the *calling* thread has permission to use the
 * stack object being passed — not just the child thread.
 *
 * This is a chicken-and-egg problem: the user-mode thread needs stack
 * access to create child threads, but can't grant it to itself because
 * it doesn't have access yet. So this must be called from supervisor
 * (kernel) mode before the target thread starts.
 *
 * Note: mpu_stack_alloc() inside os_thread_create picks which stack
 * slot to use — that works fine from user mode since it's just array
 * indexing. The permission check happens later in k_thread_create().
 */
void
os_thread_env_init_for_usermode(k_tid_t tid)
{
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    int i;
    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        k_object_access_grant(mpu_stacks[i], tid);
    }
#endif
}
#endif /* CONFIG_USERSPACE */

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
     * find it via thread_data_list_lookup. The joiner will clean up, or
     * it will be reclaimed by os_thread_cleanup_exited_data. */
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
    os_thread_data *thread_data;
    unsigned thread_data_size;
    int options = 0;
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    bool is_user_mode = k_is_user_context();
#endif

    if (!p_tid || !stack_size)
        return BHT_ERROR;

    /* Free the thread objects of terminated threads */
    thread_obj_list_reclaim();
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    dyn_thread_list_reclaim();
#endif

    /* Create and initialize thread object.
     * Under user mode, k_thread must be allocated via k_object_alloc so
     * the kernel object table registers it for syscall validation. */
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (is_user_mode) {
        tid = k_object_alloc(K_OBJ_THREAD);
        if (!tid)
            return BHT_ERROR;
        options = K_USER | K_INHERIT_PERMS;
    }
    else
#endif
    {
        if (!(tid = BH_MALLOC(sizeof(os_thread_obj))))
            return BHT_ERROR;
        memset(tid, 0, sizeof(os_thread_obj));
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (is_user_mode)
        dyn_thread_list_add(tid);
    else
#endif
        thread_obj_list_add((os_thread_obj *)tid);
    *p_tid = tid;
    return BHT_OK;

fail3:
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    mpu_stack_free(thread_data->stack);
fail2:
#endif
    BH_FREE(thread_data);
fail1:
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (is_user_mode)
        k_object_release(tid);
    else
#endif
        BH_FREE(tid);
    return BHT_ERROR;
}

korp_tid
os_self_thread()
{
    return (korp_tid)k_current_get();
}

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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (!*mutex)
        return BHT_ERROR;
#endif
    return BHT_OK;
}

int
os_recursive_mutex_init(korp_mutex *mutex)
{
    zmutex_init(mutex);
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#endif /* CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS */
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
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