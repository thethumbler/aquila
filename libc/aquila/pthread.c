/*
 * Minimal pthreads on top of the kernel thread syscalls. Each thread has
 * its own signal mask, and can be sent a signal (pthread_kill()); a
 * detached thread is freed by the kernel when it exits. Mutexes are
 * locks a waiter polls (nanosleep), condition variables wake up their
 * waiters after a moment (spurious wakeups, which POSIX allows: callers
 * check their condition again); mutexes aren't recursive whatever their
 * type, and key destructors aren't run.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aquila/syscall.h>

#include "syscall.h"

#define THREAD_STACK_SIZE   8192

/* a thread that pthread_create() made, and its stack: the kernel sets
 * `alive` to 0 once the thread is gone and off it. pthread_join() frees
 * it; a detached thread's is freed by the next pthread_create() that
 * finds it gone */
struct thread_rec {
    struct thread_rec *next;
    pthread_t tid;
    void *stack;
    volatile int alive;
    int detached;
};

static struct thread_rec *threads;
static pthread_mutex_t threads_lock;

/* frees the stacks of the detached threads that are gone (threads_lock
 * held) */
static void threads_reap(void)
{
    struct thread_rec **p = &threads;

    while (*p) {
        struct thread_rec *rec = *p;

        if (rec->detached && !rec->alive) {
            *p = rec->next;
            free(rec->stack);
            free(rec);
        } else {
            p = &rec->next;
        }
    }
}

/* finds a thread's record, and takes it off the list if `take`
 * (threads_lock held) */
static struct thread_rec *threads_find(pthread_t tid, int take)
{
    for (struct thread_rec **p = &threads; *p; p = &(*p)->next) {
        struct thread_rec *rec = *p;

        if (rec->tid == tid) {
            if (take)
                *p = rec->next;

            return rec;
        }
    }

    return NULL;
}

/* called by the kernel on the new thread's stack (through thread_entry
 * on x86-64, which can't call a static function) */
void __aquila_thread_start(void *(*start_routine)(void *), void *arg)
{
    pthread_exit(start_routine(arg));
}

/* the kernel starts a thread at thread_entry with the start routine and
 * its argument on the stack, as an i386 call does (above a null return
 * address); on x86-64 they go to registers; aarch64's kernel puts them in
 * x0 and x1 itself, a call's: __aquila_thread_start is the entry */
#ifdef __x86_64__
void thread_entry(void);

asm(".type thread_entry, @function\n"
    "thread_entry:\n"
    "    mov 8(%rsp), %rdi\n"
    "    mov 16(%rsp), %rsi\n"
    "    jmp __aquila_thread_start\n");
#else
#define thread_entry __aquila_thread_start
#endif

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine)(void *), void *arg)
{
    struct thread_rec *rec = calloc(1, sizeof(*rec));
    char *stack = calloc(1, THREAD_STACK_SIZE);

    if (!rec || !stack) {
        free(rec);
        free(stack);
        return ENOMEM;
    }

    rec->stack = stack;
    rec->alive = 1;

    struct aquila_thread_args args = {
        .stack  = (uintptr_t) (stack + THREAD_STACK_SIZE),
        .entry  = (uintptr_t) thread_entry,
        .uentry = (uintptr_t) start_routine,
        .arg    = (uintptr_t) arg,
        .attr   = (uintptr_t) attr,
        .alive  = (uintptr_t) &rec->alive,
    };

    /* held until the record is on the list: the new thread may detach
     * itself right away */
    pthread_mutex_lock(&threads_lock);
    threads_reap();

    long tid = __syscall(SYS_thread_create, (long) &args, 0, 0);

    if (tid < 0) {
        pthread_mutex_unlock(&threads_lock);
        free(stack);
        free(rec);
        return -tid;
    }

    rec->tid = tid;
    rec->next = threads;
    threads = rec;
    pthread_mutex_unlock(&threads_lock);

    *thread = tid;

    if (attr && attr->detachstate == PTHREAD_CREATE_DETACHED)
        pthread_detach(tid);

    return 0;
}

void pthread_exit(void *value_ptr)
{
    __syscall(SYS_thread_exit, (long) value_ptr, 0, 0);

    /* not reached */
    for (;;);
}

int pthread_join(pthread_t thread, void **value_ptr)
{
    long ret = __syscall(SYS_thread_join, thread, (long) value_ptr, 0);

    if (ret < 0)
        return -ret;

    /* it's gone: its stack can go too */
    pthread_mutex_lock(&threads_lock);
    struct thread_rec *rec = threads_find(thread, 1);
    pthread_mutex_unlock(&threads_lock);

    if (rec) {
        free(rec->stack);
        free(rec);
    }

    return 0;
}

pthread_t pthread_self(void)
{
    return __syscall(SYS_thread_self, 0, 0, 0);
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

int pthread_detach(pthread_t thread)
{
    long ret = __syscall(SYS_thread_detach, thread, 0, 0);

    if (ret < 0)
        return -ret;

    /* its stack is freed once it's gone, maybe already */
    pthread_mutex_lock(&threads_lock);
    struct thread_rec *rec = threads_find(thread, 0);

    if (rec)
        rec->detached = 1;

    threads_reap();
    pthread_mutex_unlock(&threads_lock);
    return 0;
}

/* the mask is the calling thread's, as sigprocmask()'s */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oset)
{
    long ret = __syscall(SYS_sigmask, how, (long) set, (long) oset);
    return ret < 0 ? -ret : 0;
}

int pthread_kill(pthread_t thread, int sig)
{
    long ret = __syscall(SYS_thread_kill, thread, sig, 0);
    return ret < 0 ? -ret : 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->stacksize = THREAD_STACK_SIZE;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    attr->is_initialized = 0;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{
    attr->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)
{
    *state = attr->detachstate;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    attr->stacksize = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)
{
    *size = attr->stacksize;
    return 0;
}

/* until another thread had the time to do something */
static void pause_briefly(void)
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}

/* mutexes: 1 when locked, anything else (0, the initializer) unlocked */
#define MUTEX_LOCKED 1

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void) attr;
    *mutex = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    return *mutex == MUTEX_LOCKED ? EBUSY : 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    pthread_mutex_t v = *mutex;

    if (v == MUTEX_LOCKED || !__sync_bool_compare_and_swap(mutex, v, MUTEX_LOCKED))
        return EBUSY;

    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex))
        pause_briefly();

    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    __sync_lock_release(mutex);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    attr->is_initialized = 0;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    *type = attr->type;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void) attr;
    *cond = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void) cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    (void) cond;
    pthread_mutex_unlock(mutex);
    pause_briefly();
    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
        const struct timespec *abstime)
{
    struct timespec now;

    clock_gettime(CLOCK_REALTIME, &now);

    if (now.tv_sec > abstime->tv_sec ||
        (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
        return ETIMEDOUT;

    return pthread_cond_wait(cond, mutex);
}

/* the waiters wake up by themselves */
int pthread_cond_signal(pthread_cond_t *cond)
{
    (void) cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void) cond;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    attr->is_initialized = 0;
    return 0;
}

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    if (!__sync_lock_test_and_set(&once->init_executed, 1))
        init();

    return 0;
}

/* thread specific data: a value per key and thread */
#define KEYS_MAX    64
#define THREADS_MAX 32

static char key_used[KEYS_MAX];

static struct {
    pthread_t thread;
    const void *value;
} key_values[KEYS_MAX][THREADS_MAX];

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    (void) destructor;

    for (int i = 0; i < KEYS_MAX; ++i) {
        if (!__sync_lock_test_and_set(&key_used[i], 1)) {
            memset(key_values[i], 0, sizeof(key_values[i]));
            *key = i;
            return 0;
        }
    }

    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= KEYS_MAX || !key_used[key])
        return EINVAL;

    key_used[key] = 0;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    pthread_t self = pthread_self();

    if (key >= KEYS_MAX)
        return NULL;

    for (int i = 0; i < THREADS_MAX; ++i)
        if (key_values[key][i].thread == self)
            return (void *) key_values[key][i].value;

    return NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    pthread_t self = pthread_self();
    int free_slot = -1;

    if (key >= KEYS_MAX || !key_used[key])
        return EINVAL;

    for (int i = 0; i < THREADS_MAX; ++i) {
        if (key_values[key][i].thread == self) {
            key_values[key][i].value = value;
            return 0;
        }

        if (!key_values[key][i].thread && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
        return ENOMEM;

    key_values[key][free_slot].thread = self;
    key_values[key][free_slot].value = value;
    return 0;
}

/* no cancellation: the state is kept, nothing cancels */
static int cancel_state = PTHREAD_CANCEL_ENABLE, cancel_type = PTHREAD_CANCEL_DEFERRED;

int pthread_setcancelstate(int state, int *oldstate)
{
    if (oldstate)
        *oldstate = cancel_state;
    cancel_state = state;
    return 0;
}

int pthread_setcanceltype(int type, int *oldtype)
{
    if (oldtype)
        *oldtype = cancel_type;
    cancel_type = type;
    return 0;
}

void pthread_testcancel(void)
{
}
