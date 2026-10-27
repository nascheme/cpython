/* Grail's thread pool: a low-overhead parallel map.
 *
 * The API and several design choices follow spinningjenny
 * (https://github.com/Quansight/spinningjenny), a Rust/PyO3 ThreadPoolExecutor
 * by Itamar Turner-Trauring and Nathan Goldbaum at Quansight:
 * map(func, *iterables, buffersize=None, return_in_order=True), copying the
 * contextvars context when map() is called rather than when the pool is
 * created, bounding how far the input is iterated ahead of the results, and
 * keeping idle workers detached from the interpreter. This file is an
 * independent C implementation; no spinningjenny code is copied.
 *
 * spinningjenny schedules each item as a rayon job from one iterating
 * thread. Here a map needs no task queue: each worker claims a batch of
 * items by iterating the inputs itself under the map's iterator lock, runs
 * them, and writes each result into a slot numbered by its input position.
 * Batch size adapts to the measured task time, so tiny tasks take the lock
 * rarely and slow tasks are claimed one at a time. Workers stay attached to
 * the interpreter while they have work and park (detached) in the parking
 * lot when they have none.
 *
 * Result slots live in fixed-size segments, linked in input order. The
 * claiming worker appends segments; the consumer frees them once every slot
 * in them has been yielded. Ordered results are read slot by slot. Unordered
 * results are linked into an intrusive MPSC queue (Vyukov) as they complete.
 * A consumer that has nothing to read parks on its map; a worker wakes it
 * only when the consumer has said it is waiting (and, for ordered results,
 * only for the slot it is waiting for).
 *
 * A pool worker that waits for results of a nested map runs pool work while
 * it waits, so nested maps cannot deadlock the pool.
 *
 * Scopes (pool.scope(), Scope.spawn()) run individual tasks, rayon-style:
 * a task spawned on a worker goes onto that worker's Chase-Lev deque, a task
 * spawned from any other thread into a mutex-protected FIFO injector queue.
 * A worker runs its own newest task first (LIFO, for locality in recursive
 * task trees), then the injector's oldest, then steals the oldest task of
 * another worker. Leaving the scope waits until every task, including tasks
 * spawned by tasks, has finished; a worker waiting on a nested scope runs
 * pool work meanwhile. Workers that find no work recheck the deques after
 * announcing that they are about to sleep, so a spawner only has to wake
 * a worker when it sees a sleeper.
 *
 * Not yet done: GC support for the result iterator (a cycle through the
 * mapped function leaks), fork safety beyond a pid check, and a serial
 * fallback for GIL-enabled builds.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "internal/pycore_lock.h"         // PyMutex
#include "internal/pycore_parking_lot.h"  // _PyParkingLot_Park()
#include "internal/pycore_pystate.h"      // _PyThreadState_New()
#include "internal/pycore_pylifecycle.h"  // _PyThreadState_DeleteCurrent()
#include "internal/pycore_interp.h"       // interp->threads.count

#include <stdint.h>
#ifdef MS_WINDOWS
#  include <process.h>                    // _getpid()
#  define getpid _getpid
typedef int pid_t;
#else
#  include <unistd.h>                     // getpid()
#endif

#define SEG_SIZE 256
/* Capacity of a worker's task deque (a power of two). When it is full,
 * spawned tasks go to the injector queue instead. */
#define DEQUE_SIZE 4096
/* Parked waiters on a scope wake this often to check for signals. */
#define SCOPE_PARK_NS (50 * 1000 * 1000)
/* A batch should take about this long; the batch size follows from the
 * measured time per task. */
#define BATCH_TARGET_NS 20000
#define MAX_BATCH 16
/* Spin this many times before parking. */
#define WORKER_SPINS 2000
#define CONSUMER_SPINS 200
/* Parked consumers wake this often to check for signals. */
#define CONSUMER_PARK_NS (50 * 1000 * 1000)

enum { SLOT_PENDING = 0, SLOT_OK = 1, SLOT_ERROR = 2 };

struct Segment;

typedef struct Slot {
    PyObject *value;             /* result or exception; NULL once yielded */
    int state;                   /* SLOT_*, atomic */
    struct Slot *next_done;      /* unordered completion queue link */
    struct Segment *seg;
} Slot;

typedef struct Segment {
    struct Segment *next;        /* atomic: appended by the claiming worker */
    int consumed;                /* consumer only */
    Slot slots[SEG_SIZE];
} Segment;

typedef struct PoolCore PoolCore;
typedef struct ScopeObject ScopeObject;

typedef struct Task {
    struct Task *next;           /* injector link */
    ScopeObject *scope;          /* strong reference */
    PyObject *func;
    PyObject *kwargs;            /* dict or NULL */
    PyObject *context;           /* copied at spawn */
    Py_ssize_t nargs;
    PyObject *args[1];           /* nargs items */
} Task;

/* A Chase-Lev work-stealing deque with a fixed capacity, after Le, Pop,
 * Cohen and Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak
 * Memory Models" (PPoPP 2013). The owner pushes and pops at bottom;
 * thieves take from top. top and bottom are on separate cache lines. */
typedef struct {
    Py_ssize_t top;              /* atomic */
    char pad1[64 - sizeof(Py_ssize_t)];
    Py_ssize_t bottom;           /* atomic; written by the owner only */
    Task **buf;
    char pad2[64 - sizeof(Py_ssize_t) - sizeof(Task **)];
} Deque;

typedef struct Job {
    Py_ssize_t refcnt;           /* atomic */
    uint64_t id;
    struct Job *next_active;     /* pool->mutex */
    PyObject *func;
    PyObject *iters;             /* tuple of iterators */
    Py_ssize_t nargs;
    PyObject *context;           /* tasks run in per-worker copies of this */
    PyObject *iter_context;      /* the inputs are iterated in this copy */
    Py_ssize_t buffersize;       /* 0: unbounded */
    int ordered;

    /* Claiming side, under iter_mutex. */
    PyMutex iter_mutex;
    Segment *tail;
    PyObject *pull_error;        /* raised after the last result */

    Py_ssize_t next_index;       /* items claimed; atomic, written under iter_mutex */
    int done_pulling;            /* atomic, written under iter_mutex */
    int detached;                /* atomic: the result iterator is gone */
    Py_ssize_t consumed;         /* atomic: results yielded */

    /* Consumer wakeup. */
    int consumer_waiting;        /* atomic; parking address */
    Py_ssize_t wanted;           /* atomic: ordered slot the consumer waits for */

    /* Consumer side, under consumer_mutex. */
    PyMutex consumer_mutex;
    Segment *head;
    Py_ssize_t head_base;
    Slot *q_head;
    Slot *q_tail;                /* atomic */
    Slot stub;
} Job;

struct PoolCore {
    Py_ssize_t refcnt;           /* atomic */
    PyInterpreterState *interp;
    int n_threads;
    PyMutex mutex;
    Job *active_head;            /* mutex */
    Job *active_tail;            /* mutex */
    uint64_t next_job_id;        /* mutex */
    uint32_t epoch;              /* atomic; parking address for idle workers */
    int sleepers;                /* atomic */
    int shutdown;                /* atomic */
    Py_ssize_t live_workers;     /* atomic; parking address for shutdown */
    pid_t pid;                   /* the workers do not survive fork() */

    Deque *deques;               /* one per worker */
    PyMutex inj_mutex;
    Task *inj_head;              /* inj_mutex */
    Task *inj_tail;              /* inj_mutex */
    Py_ssize_t inj_count;        /* atomic */
};

struct ScopeObject {
    PyObject_HEAD
    PyObject *pool;              /* keeps the workers running */
    PoolCore *core;
    PyObject *errors;            /* list of exceptions raised by tasks */
    Py_ssize_t pending;          /* atomic: spawned tasks not yet finished */
    int cancelled;               /* atomic: tasks not yet started are skipped */
    int closed;                  /* atomic: no more spawns */
    int waiting;                 /* atomic; parking address for the waiter */
};

/* Per worker thread. */
typedef struct {
    PoolCore *core;
    int index;                   /* of this worker's deque */
    uint64_t rng;                /* xorshift state for picking victims */
    uint64_t ctx_job_id;
    PyObject *ctx;               /* cached copy of the current job's context */
} Worker;

static _Py_thread_local Worker *current_worker;

static inline void
cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static PyTime_t
now_ns(void)
{
    PyTime_t t;
    (void)PyTime_PerfCounterRaw(&t);
    return t;
}

/* ---- pool core ---------------------------------------------------------- */

static void
core_incref(PoolCore *core)
{
    _Py_atomic_add_ssize(&core->refcnt, 1);
}

static void
core_decref(PoolCore *core)
{
    if (_Py_atomic_add_ssize(&core->refcnt, -1) == 1) {
        if (core->deques != NULL) {
            for (int i = 0; i < core->n_threads; i++) {
                PyMem_RawFree(core->deques[i].buf);
            }
            PyMem_RawFree(core->deques);
        }
        PyMem_RawFree(core);
    }
}

static void
unpark_noop(void *arg, void *park_arg, int has_more_waiters)
{
}

/* New work may be available: wake one idle worker, or all of them. */
static void
core_notify(PoolCore *core, int all)
{
    _Py_atomic_add_uint32(&core->epoch, 1);
    if (_Py_atomic_load_int(&core->sleepers) > 0) {
        if (all) {
            _PyParkingLot_UnparkAll(&core->epoch);
        }
        else {
            _PyParkingLot_Unpark(&core->epoch, unpark_noop, NULL);
        }
    }
}

/* ---- jobs --------------------------------------------------------------- */

static void
job_incref(Job *job)
{
    _Py_atomic_add_ssize(&job->refcnt, 1);
}

/* Requires an attached thread state. */
static void
job_decref(Job *job)
{
    if (_Py_atomic_add_ssize(&job->refcnt, -1) != 1) {
        return;
    }
    Segment *seg = job->head;
    while (seg != NULL) {
        for (int i = 0; i < SEG_SIZE; i++) {
            Py_XDECREF(seg->slots[i].value);
        }
        Segment *next = seg->next;
        PyMem_Free(seg);
        seg = next;
    }
    Py_XDECREF(job->func);
    Py_XDECREF(job->iters);
    Py_XDECREF(job->context);
    Py_XDECREF(job->iter_context);
    Py_XDECREF(job->pull_error);
    PyMem_Free(job);
}

static inline int
job_window_open(Job *job)
{
    if (job->buffersize == 0 || _Py_atomic_load_int(&job->detached)) {
        return 1;
    }
    Py_ssize_t claimed = _Py_atomic_load_ssize_relaxed(&job->next_index);
    return claimed - _Py_atomic_load_ssize(&job->consumed) < job->buffersize;
}

static inline int
job_available(Job *job)
{
    return !_Py_atomic_load_int_acquire(&job->done_pulling) && job_window_open(job);
}

static void
wake_consumer(Job *job)
{
    if (_Py_atomic_load_int(&job->consumer_waiting)) {
        _Py_atomic_store_int(&job->consumer_waiting, 0);
        _PyParkingLot_UnparkAll(&job->consumer_waiting);
    }
}

/* Unordered completion queue: Vyukov's intrusive MPSC queue. */
static void
queue_push(Job *job, Slot *slot)
{
    _Py_atomic_store_ptr_relaxed(&slot->next_done, NULL);
    Slot *prev = _Py_atomic_exchange_ptr(&job->q_tail, slot);
    _Py_atomic_store_ptr(&prev->next_done, slot);
}

static Slot *
queue_pop(Job *job)
{
    Slot *head = job->q_head;
    Slot *next = _Py_atomic_load_ptr_acquire(&head->next_done);
    if (head == &job->stub) {
        if (next == NULL) {
            return NULL;
        }
        job->q_head = head = next;
        next = _Py_atomic_load_ptr_acquire(&next->next_done);
    }
    if (next != NULL) {
        job->q_head = next;
        return head;
    }
    if (head != _Py_atomic_load_ptr_acquire(&job->q_tail)) {
        return NULL;            /* a push is in progress */
    }
    queue_push(job, &job->stub);
    next = _Py_atomic_load_ptr_acquire(&head->next_done);
    if (next != NULL) {
        job->q_head = next;
        return head;
    }
    return NULL;
}

/* Publish a task's result. slot is NULL if the result iterator was dropped
 * before the item was claimed. */
static void
job_complete(Job *job, Slot *slot, Py_ssize_t index, PyObject *value, int state)
{
    if (slot == NULL) {
        Py_DECREF(value);
        return;
    }
    slot->value = value;
    if (job->ordered) {
        _Py_atomic_store_int(&slot->state, state);
        if (_Py_atomic_load_int(&job->consumer_waiting)
            && _Py_atomic_load_ssize(&job->wanted) == index)
        {
            wake_consumer(job);
        }
    }
    else {
        _Py_atomic_store_int_release(&slot->state, state);
        queue_push(job, slot);
        wake_consumer(job);
    }
}

typedef struct {
    PyObject *args;              /* the item, or a tuple for several iterables */
    Slot *slot;
    Py_ssize_t index;
} Claimed;

/* Mark the inputs finished. Called with iter_mutex held. */
static void
job_finish_pulling(Job *job, PyObject *error)
{
    if (error != NULL && job->pull_error == NULL
        && !_Py_atomic_load_int(&job->detached))
    {
        job->pull_error = error;
    }
    else {
        Py_XDECREF(error);
    }
    _Py_atomic_store_int_release(&job->done_pulling, 1);
}

/* Claim up to max items by iterating the inputs. Returns the number claimed.
 * May set done_pulling, in which case the consumer is woken. */
static Py_ssize_t
job_claim(Job *job, PoolCore *core, Py_ssize_t max, Claimed *out)
{
    Py_ssize_t n = 0;
    int finished = 0;
    PyMutex_Lock(&job->iter_mutex);
    if (_Py_atomic_load_int_relaxed(&job->done_pulling)) {
        PyMutex_Unlock(&job->iter_mutex);
        return 0;
    }
    if (_Py_atomic_load_int(&core->shutdown) && !_Py_atomic_load_int(&job->detached)) {
        PyErr_SetString(PyExc_RuntimeError, "the pool was shut down");
        job_finish_pulling(job, PyErr_GetRaisedException());
        PyMutex_Unlock(&job->iter_mutex);
        wake_consumer(job);
        return 0;
    }
    int detached = _Py_atomic_load_int(&job->detached);
    Py_ssize_t index = job->next_index;
    if (job->buffersize && !detached) {
        Py_ssize_t room = job->buffersize - (index - _Py_atomic_load_ssize(&job->consumed));
        if (room <= 0) {
            PyMutex_Unlock(&job->iter_mutex);
            return 0;
        }
        if (max > room) {
            max = room;
        }
    }
    if (PyContext_Enter(job->iter_context) < 0) {
        job_finish_pulling(job, PyErr_GetRaisedException());
        PyMutex_Unlock(&job->iter_mutex);
        wake_consumer(job);
        return 0;
    }
    PyObject *error = NULL;
    while (n < max) {
        PyObject *args;
        if (job->nargs == 1) {
            int r = PyIter_NextItem(PyTuple_GET_ITEM(job->iters, 0), &args);
            if (r <= 0) {
                if (r < 0) {
                    error = PyErr_GetRaisedException();
                }
                finished = 1;
                break;
            }
        }
        else {
            args = PyTuple_New(job->nargs);
            if (args == NULL) {
                error = PyErr_GetRaisedException();
                finished = 1;
                break;
            }
            for (Py_ssize_t i = 0; i < job->nargs; i++) {
                PyObject *item;
                int r = PyIter_NextItem(PyTuple_GET_ITEM(job->iters, i), &item);
                if (r <= 0) {
                    if (r < 0) {
                        error = PyErr_GetRaisedException();
                    }
                    finished = 1;
                    break;
                }
                PyTuple_SET_ITEM(args, i, item);
            }
            if (finished) {
                Py_DECREF(args);
                break;
            }
        }
        Slot *slot = NULL;
        if (!detached) {
            Py_ssize_t off = index % SEG_SIZE;
            if (off == 0 && index > 0) {
                Segment *seg = PyMem_Calloc(1, sizeof(Segment));
                if (seg == NULL) {
                    Py_DECREF(args);
                    PyErr_NoMemory();
                    error = PyErr_GetRaisedException();
                    finished = 1;
                    break;
                }
                _Py_atomic_store_ptr_release(&job->tail->next, seg);
                job->tail = seg;
            }
            slot = &job->tail->slots[off];
            slot->seg = job->tail;
        }
        out[n].args = args;
        out[n].slot = slot;
        out[n].index = index;
        n++;
        index++;
        _Py_atomic_store_ssize_release(&job->next_index, index);
    }
    if (PyContext_Exit(job->iter_context) < 0) {
        PyErr_Clear();
    }
    if (finished) {
        job_finish_pulling(job, error);
    }
    PyMutex_Unlock(&job->iter_mutex);
    if (finished) {
        wake_consumer(job);
    }
    return n;
}

/* Run one batch of the job. Returns 0 if nothing could be claimed. *ns_per_task
 * is the worker's running estimate of the task time. A worker helping while it
 * waits for a nested map passes w->ctx unusable (help=1): the cached context
 * may be the one its outer task is running in. */
static int
job_run_batch(Job *job, Worker *w, double *ns_per_task, int help)
{
    Claimed items[MAX_BATCH];
    Py_ssize_t max = MAX_BATCH;
    if (*ns_per_task > 0) {
        double b = BATCH_TARGET_NS / *ns_per_task;
        max = b < 1 ? 1 : (b > MAX_BATCH ? MAX_BATCH : (Py_ssize_t)b);
    }
    else {
        max = 1;
    }
    if (job->buffersize) {
        Py_ssize_t share = job->buffersize / w->core->n_threads;
        if (share < 1) {
            share = 1;
        }
        if (max > share) {
            max = share;
        }
    }
    Py_ssize_t n = job_claim(job, w->core, max, items);
    if (n == 0) {
        return 0;
    }

    PyObject *ctx;
    if (help) {
        ctx = PyContext_Copy(job->context);
    }
    else {
        if (w->ctx == NULL || w->ctx_job_id != job->id) {
            Py_XSETREF(w->ctx, PyContext_Copy(job->context));
            w->ctx_job_id = job->id;
        }
        ctx = Py_XNewRef(w->ctx);
    }
    PyObject *ctx_error = NULL;
    if (ctx == NULL || PyContext_Enter(ctx) < 0) {
        ctx_error = PyErr_GetRaisedException();
    }

    PyTime_t t0 = now_ns();
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *args = items[i].args;
        PyObject *res;
        if (ctx_error != NULL) {
            res = NULL;
            PyErr_SetRaisedException(Py_NewRef(ctx_error));
        }
        else if (job->nargs == 1) {
            res = PyObject_Vectorcall(job->func, &args, 1, NULL);
        }
        else {
            res = PyObject_Vectorcall(job->func, &PyTuple_GET_ITEM(args, 0),
                                      job->nargs, NULL);
        }
        Py_DECREF(args);
        if (res != NULL) {
            job_complete(job, items[i].slot, items[i].index, res, SLOT_OK);
        }
        else {
            job_complete(job, items[i].slot, items[i].index,
                         PyErr_GetRaisedException(), SLOT_ERROR);
        }
    }
    double per = (double)(now_ns() - t0) / n;
    *ns_per_task = *ns_per_task > 0 ? 0.75 * *ns_per_task + 0.25 * per : per;

    if (ctx_error != NULL) {
        Py_DECREF(ctx_error);
    }
    else if (PyContext_Exit(ctx) < 0) {
        PyErr_Clear();
    }
    Py_XDECREF(ctx);
    return 1;
}

/* Find a job with work available, dropping finished jobs from the active
 * list. Returns a new job reference or NULL. */
static Job *
core_find_job(PoolCore *core)
{
    Job *dead = NULL;
    Job *found = NULL;
    PyMutex_Lock(&core->mutex);
    Job **link = &core->active_head;
    Job *prev = NULL;
    while (*link != NULL) {
        Job *job = *link;
        if (_Py_atomic_load_int_acquire(&job->done_pulling)) {
            *link = job->next_active;
            if (core->active_tail == job) {
                core->active_tail = prev;
            }
            job->next_active = dead;
            dead = job;
            continue;
        }
        if (job_window_open(job)) {
            found = job;
            job_incref(job);
            /* Round robin: move it to the end so maps share the pool. */
            if (job->next_active != NULL) {
                *link = job->next_active;
                job->next_active = NULL;
                core->active_tail->next_active = job;
                core->active_tail = job;
            }
            break;
        }
        prev = job;
        link = &job->next_active;
    }
    PyMutex_Unlock(&core->mutex);
    while (dead != NULL) {
        Job *next = dead->next_active;
        job_decref(dead);
        dead = next;
    }
    return found;
}

/* ---- scope tasks -------------------------------------------------------- */

#define DEQUE_MASK (DEQUE_SIZE - 1)

static int
deque_push(Deque *q, Task *task)
{
    Py_ssize_t b = _Py_atomic_load_ssize_relaxed(&q->bottom);
    Py_ssize_t t = _Py_atomic_load_ssize_acquire(&q->top);
    if (b - t >= DEQUE_SIZE) {
        return -1;
    }
    _Py_atomic_store_ptr_relaxed(&q->buf[b & DEQUE_MASK], task);
    _Py_atomic_fence_release();
    _Py_atomic_store_ssize_relaxed(&q->bottom, b + 1);
    return 0;
}

static Task *
deque_pop(Deque *q)
{
    Py_ssize_t b = _Py_atomic_load_ssize_relaxed(&q->bottom) - 1;
    _Py_atomic_store_ssize_relaxed(&q->bottom, b);
    _Py_atomic_fence_seq_cst();
    Py_ssize_t t = _Py_atomic_load_ssize_relaxed(&q->top);
    Task *task = NULL;
    if (t <= b) {
        task = _Py_atomic_load_ptr_relaxed(&q->buf[b & DEQUE_MASK]);
        if (t == b) {
            /* The last task: race the thieves for it. */
            if (!_Py_atomic_compare_exchange_ssize(&q->top, &t, t + 1)) {
                task = NULL;
            }
            _Py_atomic_store_ssize_relaxed(&q->bottom, b + 1);
        }
    }
    else {
        _Py_atomic_store_ssize_relaxed(&q->bottom, b + 1);
    }
    return task;
}

/* Returns a task, or NULL with *lost set if another thread won the race. */
static Task *
deque_steal(Deque *q, int *lost)
{
    Py_ssize_t t = _Py_atomic_load_ssize_acquire(&q->top);
    _Py_atomic_fence_seq_cst();
    Py_ssize_t b = _Py_atomic_load_ssize_acquire(&q->bottom);
    if (t >= b) {
        return NULL;
    }
    Task *task = _Py_atomic_load_ptr_relaxed(&q->buf[t & DEQUE_MASK]);
    if (!_Py_atomic_compare_exchange_ssize(&q->top, &t, t + 1)) {
        *lost = 1;
        return NULL;
    }
    return task;
}

static void
injector_push(PoolCore *core, Task *task)
{
    task->next = NULL;
    PyMutex_Lock(&core->inj_mutex);
    if (core->inj_tail != NULL) {
        core->inj_tail->next = task;
    }
    else {
        core->inj_head = task;
    }
    core->inj_tail = task;
    _Py_atomic_add_ssize(&core->inj_count, 1);
    PyMutex_Unlock(&core->inj_mutex);
}

static Task *
injector_pop(PoolCore *core)
{
    if (_Py_atomic_load_ssize_relaxed(&core->inj_count) == 0) {
        return NULL;
    }
    PyMutex_Lock(&core->inj_mutex);
    Task *task = core->inj_head;
    if (task != NULL) {
        core->inj_head = task->next;
        if (core->inj_head == NULL) {
            core->inj_tail = NULL;
        }
        _Py_atomic_add_ssize(&core->inj_count, -1);
    }
    PyMutex_Unlock(&core->inj_mutex);
    return task;
}

/* Is any task visible? Racy, used only to decide whether to sleep. */
static int
tasks_visible(PoolCore *core)
{
    if (_Py_atomic_load_ssize(&core->inj_count) > 0) {
        return 1;
    }
    for (int i = 0; i < core->n_threads; i++) {
        Deque *q = &core->deques[i];
        if (_Py_atomic_load_ssize(&q->bottom) > _Py_atomic_load_ssize(&q->top)) {
            return 1;
        }
    }
    return 0;
}

/* Find a task for worker w: its own newest, the injector's oldest, or the
 * oldest of another worker. */
static Task *
find_task(Worker *w)
{
    PoolCore *core = w->core;
    Task *task = deque_pop(&core->deques[w->index]);
    if (task != NULL) {
        return task;
    }
    task = injector_pop(core);
    if (task != NULL) {
        return task;
    }
    int n = core->n_threads;
    if (n == 1) {
        return NULL;
    }
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    int start = (int)(w->rng % (uint64_t)n);
    for (int round = 0; round < 2; round++) {
        int lost = 0;
        for (int k = 0; k < n; k++) {
            int i = (start + k) % n;
            if (i == w->index) {
                continue;
            }
            task = deque_steal(&core->deques[i], &lost);
            if (task != NULL) {
                return task;
            }
        }
        if (!lost) {
            break;
        }
    }
    return NULL;
}

static void
scope_task_done(ScopeObject *scope)
{
    if (_Py_atomic_add_ssize(&scope->pending, -1) == 1
        && _Py_atomic_load_int(&scope->waiting))
    {
        _Py_atomic_store_int(&scope->waiting, 0);
        _PyParkingLot_UnparkAll(&scope->waiting);
    }
}

static void
task_free(Task *task)
{
    Py_DECREF(task->func);
    Py_XDECREF(task->kwargs);
    Py_DECREF(task->context);
    for (Py_ssize_t i = 0; i < task->nargs; i++) {
        Py_DECREF(task->args[i]);
    }
    PyMem_Free(task);
}

static void
scope_record_error(ScopeObject *scope, PyObject *exc)
{
    _Py_atomic_store_int(&scope->cancelled, 1);
    if (PyList_Append(scope->errors, exc) < 0) {
        PyErr_FormatUnraisable("Exception ignored while recording a scope task error");
    }
    Py_DECREF(exc);
}

/* Run a task (unless its scope was cancelled) and free it. */
static void
run_task(Task *task)
{
    ScopeObject *scope = task->scope;
    if (!_Py_atomic_load_int(&scope->cancelled)) {
        PyObject *res = NULL;
        if (PyContext_Enter(task->context) == 0) {
            res = PyObject_VectorcallDict(task->func, task->args, task->nargs,
                                          task->kwargs);
            if (PyContext_Exit(task->context) < 0) {
                PyErr_Clear();
            }
        }
        if (res != NULL) {
            Py_DECREF(res);
        }
        else {
            scope_record_error(scope, PyErr_GetRaisedException());
        }
    }
    task_free(task);
    scope_task_done(scope);
    Py_DECREF(scope);
}

/* Run one task, or one batch of any map. Used by a worker waiting on a
 * nested map or scope. */
static int
worker_help(Worker *w)
{
    Task *task = find_task(w);
    if (task != NULL) {
        run_task(task);
        return 1;
    }
    Job *job = core_find_job(w->core);
    if (job == NULL) {
        return 0;
    }
    double ns = 0;
    int ran = job_run_batch(job, w, &ns, 1);
    job_decref(job);
    return ran;
}

/* ---- worker threads ----------------------------------------------------- */

typedef struct {
    PoolCore *core;
    PyThreadState *tstate;
    int index;
} WorkerBoot;

static void
worker_main(void *arg)
{
    WorkerBoot *boot = (WorkerBoot *)arg;
    PoolCore *core = boot->core;
    PyThreadState *tstate = boot->tstate;
    int index = boot->index;
    PyMem_RawFree(boot);

    if (_PyThreadState_MustExit(tstate)) {
        /* The interpreter is finalizing; it owns tstate. */
        goto exit;
    }
    _PyThreadState_Bind(tstate);
    PyEval_AcquireThread(tstate);
    _Py_atomic_add_ssize(&tstate->interp->threads.count, 1);

    Worker w = {.core = core, .index = index,
                .rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(index + 1)};
    current_worker = &w;
    Job *job = NULL;
    double ns_per_task = 0;
    for (;;) {
        uint32_t epoch = _Py_atomic_load_uint32(&core->epoch);
        Task *task = find_task(&w);
        if (task != NULL) {
            run_task(task);
            continue;
        }
        if (job != NULL && !job_available(job)) {
            job_decref(job);
            job = NULL;
        }
        if (job == NULL) {
            job = core_find_job(core);
            ns_per_task = 0;
        }
        if (job != NULL) {
            if (!job_run_batch(job, &w, &ns_per_task, 0)) {
                job_decref(job);
                job = NULL;
            }
            continue;
        }
        if (_Py_atomic_load_int(&core->shutdown)) {
            break;
        }
        /* Idle: spin briefly, then park. Detached either way, so an idle
         * worker never delays a stop-the-world pause. */
        PyThreadState *save = PyEval_SaveThread();
        int changed = 0;
        for (int i = 0; i < WORKER_SPINS; i++) {
            if (_Py_atomic_load_uint32_relaxed(&core->epoch) != epoch
                || ((i & 15) == 0 && tasks_visible(core)))
            {
                changed = 1;
                break;
            }
            cpu_relax();
        }
        if (!changed) {
            /* Announce the sleep, then recheck: a spawner that pushed a task
             * before seeing sleepers > 0 made it visible by now. Maps always
             * bump the epoch. */
            _Py_atomic_add_int(&core->sleepers, 1);
            _Py_atomic_fence_seq_cst();
            if (_Py_atomic_load_uint32(&core->epoch) == epoch
                && !tasks_visible(core)
                && !_Py_atomic_load_int(&core->shutdown))
            {
                _PyParkingLot_Park(&core->epoch, &epoch, sizeof(epoch),
                                   -1, NULL, 0);
            }
            _Py_atomic_add_int(&core->sleepers, -1);
        }
        PyEval_RestoreThread(save);
    }
    Py_CLEAR(w.ctx);
    current_worker = NULL;

    _Py_atomic_add_ssize(&tstate->interp->threads.count, -1);
    PyThreadState_Clear(tstate);
    _PyThreadState_DeleteCurrent(tstate);
exit:
    _Py_atomic_add_ssize(&core->live_workers, -1);
    _PyParkingLot_UnparkAll(&core->live_workers);
    core_decref(core);
}

static int
start_worker(PoolCore *core, int index)
{
    WorkerBoot *boot = PyMem_RawMalloc(sizeof(WorkerBoot));
    if (boot == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    boot->core = core;
    boot->index = index;
    boot->tstate = _PyThreadState_New(core->interp,
                                      _PyThreadState_WHENCE_THREADING_DAEMON);
    if (boot->tstate == NULL) {
        PyMem_RawFree(boot);
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        return -1;
    }
    core_incref(core);
    _Py_atomic_add_ssize(&core->live_workers, 1);
    if (PyThread_start_new_thread(worker_main, boot) == PYTHREAD_INVALID_THREAD_ID) {
        _Py_atomic_add_ssize(&core->live_workers, -1);
        PyThreadState_Clear(boot->tstate);
        PyThreadState_Delete(boot->tstate);
        PyMem_RawFree(boot);
        core_decref(core);
        PyErr_SetString(PyExc_RuntimeError, "can't start new thread");
        return -1;
    }
    return 0;
}

/* Fail maps that still have inputs and a consumer, and wake the workers so
 * they exit once no work is left. Maps whose result iterator was dropped
 * still run to completion, as with concurrent.futures. */
static void
core_shutdown(PoolCore *core, int wait)
{
    if (_Py_atomic_exchange_int(&core->shutdown, 1) == 0) {
        Job *jobs = NULL;
        PyMutex_Lock(&core->mutex);
        Job **link = &core->active_head;
        core->active_tail = NULL;
        while (*link != NULL) {
            Job *job = *link;
            if (_Py_atomic_load_int(&job->detached)) {
                core->active_tail = job;
                link = &job->next_active;
            }
            else {
                *link = job->next_active;
                job->next_active = jobs;
                jobs = job;
            }
        }
        PyMutex_Unlock(&core->mutex);
        core_notify(core, 1);
        while (jobs != NULL) {
            Job *next = jobs->next_active;
            PyMutex_Lock(&jobs->iter_mutex);
            if (!_Py_atomic_load_int_relaxed(&jobs->done_pulling)) {
                PyErr_SetString(PyExc_RuntimeError, "the pool was shut down");
                job_finish_pulling(jobs, PyErr_GetRaisedException());
            }
            PyMutex_Unlock(&jobs->iter_mutex);
            wake_consumer(jobs);
            job_decref(jobs);
            jobs = next;
        }
    }
    if (wait && current_worker == NULL && getpid() == core->pid) {
        for (;;) {
            Py_ssize_t live = _Py_atomic_load_ssize(&core->live_workers);
            if (live == 0) {
                break;
            }
            _PyParkingLot_Park(&core->live_workers, &live, sizeof(live),
                               -1, NULL, 1);
        }
    }
}

/* ---- Python types ------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    PoolCore *core;
    PyTypeObject *results_type;
    PyTypeObject *scope_type;
} PoolObject;

typedef struct {
    PyObject_HEAD
    PyObject *pool;              /* keeps the workers running */
    PoolCore *core;
    Job *job;
    Py_ssize_t next;             /* ordered: the next index to yield */
} ResultsObject;

typedef struct {
    PyTypeObject *pool_type;
    PyTypeObject *results_type;
    PyTypeObject *scope_type;
} PoolState;

static PyObject *
pool_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"n_threads", NULL};
    int n_threads;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:Pool", kwlist, &n_threads)) {
        return NULL;
    }
    if (n_threads < 1) {
        PyErr_SetString(PyExc_ValueError, "n_threads must be greater than 0");
        return NULL;
    }
    PoolState *state = PyType_GetModuleState(type);
    PoolCore *core = PyMem_RawCalloc(1, sizeof(PoolCore));
    if (core == NULL) {
        return PyErr_NoMemory();
    }
    core->refcnt = 1;
    core->interp = PyInterpreterState_Get();
    core->n_threads = n_threads;
    core->next_job_id = 1;
    core->pid = getpid();
    core->deques = PyMem_RawCalloc(n_threads, sizeof(Deque));
    if (core->deques == NULL) {
        PyMem_RawFree(core);
        return PyErr_NoMemory();
    }
    for (int i = 0; i < n_threads; i++) {
        core->deques[i].buf = PyMem_RawCalloc(DEQUE_SIZE, sizeof(Task *));
        if (core->deques[i].buf == NULL) {
            core_decref(core);
            return PyErr_NoMemory();
        }
    }
    PoolObject *self = (PoolObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        PyMem_RawFree(core);
        return NULL;
    }
    self->core = core;
    self->results_type = (PyTypeObject *)Py_NewRef(state->results_type);
    self->scope_type = (PyTypeObject *)Py_NewRef(state->scope_type);
    for (int i = 0; i < n_threads; i++) {
        if (start_worker(core, i) < 0) {
            Py_DECREF(self);
            return NULL;
        }
    }
    return (PyObject *)self;
}

static void
pool_dealloc(PyObject *op)
{
    PoolObject *self = (PoolObject *)op;
    PyTypeObject *tp = Py_TYPE(op);
    if (self->core != NULL) {
        core_shutdown(self->core, 0);
        core_decref(self->core);
    }
    Py_XDECREF(self->results_type);
    Py_XDECREF(self->scope_type);
    tp->tp_free(op);
    Py_DECREF(tp);
}

static PyObject *
pool_map(PyObject *op, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PoolObject *self = (PoolObject *)op;
    PoolCore *core = self->core;
    Py_ssize_t buffersize = 0;
    int ordered = 1;
    if (nargs < 2) {
        PyErr_SetString(PyExc_TypeError,
                        "map() requires a function and at least one iterable");
        return NULL;
    }
    if (kwnames != NULL) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            PyObject *key = PyTuple_GET_ITEM(kwnames, i);
            PyObject *value = args[nargs + i];
            if (PyUnicode_EqualToUTF8(key, "buffersize")) {
                if (value == Py_None) {
                    buffersize = 0;
                    continue;
                }
                buffersize = PyLong_AsSsize_t(value);
                if (buffersize == -1 && PyErr_Occurred()) {
                    return NULL;
                }
                if (buffersize < 0) {
                    PyErr_SetString(PyExc_OverflowError,
                                    "buffersize must not be negative");
                    return NULL;
                }
                if (buffersize == 0) {
                    PyErr_SetString(PyExc_ValueError, "buffersize must be >= 1");
                    return NULL;
                }
            }
            else if (PyUnicode_EqualToUTF8(key, "return_in_order")) {
                ordered = PyObject_IsTrue(value);
                if (ordered < 0) {
                    return NULL;
                }
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "map() got an unexpected keyword argument '%U'", key);
                return NULL;
            }
        }
    }
    if (getpid() != core->pid) {
        PyErr_SetString(PyExc_RuntimeError, "the pool cannot be used after fork()");
        return NULL;
    }

    Job *job = PyMem_Calloc(1, sizeof(Job));
    if (job == NULL) {
        return PyErr_NoMemory();
    }
    job->refcnt = 1;
    job->func = Py_NewRef(args[0]);
    job->nargs = nargs - 1;
    job->buffersize = buffersize;
    job->ordered = ordered;
    job->q_head = job->q_tail = &job->stub;
    job->head = job->tail = PyMem_Calloc(1, sizeof(Segment));
    if (job->head == NULL) {
        PyErr_NoMemory();
        goto error;
    }
    job->iters = PyTuple_New(job->nargs);
    if (job->iters == NULL) {
        goto error;
    }
    for (Py_ssize_t i = 0; i < job->nargs; i++) {
        PyObject *it = PyObject_GetIter(args[i + 1]);
        if (it == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(job->iters, i, it);
    }
    job->context = PyContext_CopyCurrent();
    if (job->context == NULL) {
        goto error;
    }
    job->iter_context = PyContext_Copy(job->context);
    if (job->iter_context == NULL) {
        goto error;
    }

    ResultsObject *res = PyObject_New(ResultsObject, self->results_type);
    if (res == NULL) {
        goto error;
    }
    core_incref(core);
    res->pool = Py_NewRef(op);
    res->core = core;
    res->job = job;
    res->next = 0;

    PyMutex_Lock(&core->mutex);
    if (_Py_atomic_load_int(&core->shutdown)) {
        PyMutex_Unlock(&core->mutex);
        Py_DECREF(res);
        PyErr_SetString(PyExc_RuntimeError, "cannot map after the pool was shut down");
        return NULL;
    }
    job->id = core->next_job_id++;
    job_incref(job);
    if (core->active_tail != NULL) {
        core->active_tail->next_active = job;
    }
    else {
        core->active_head = job;
    }
    core->active_tail = job;
    PyMutex_Unlock(&core->mutex);
    core_notify(core, 1);
    return (PyObject *)res;

error:
    job_decref(job);
    return NULL;
}

static PyObject *
pool_shutdown(PyObject *op, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"wait", NULL};
    int wait = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p:shutdown", kwlist, &wait)) {
        return NULL;
    }
    core_shutdown(((PoolObject *)op)->core, wait);
    Py_RETURN_NONE;
}

static PyObject *
pool_enter(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return Py_NewRef(op);
}

static PyObject *
pool_exit(PyObject *op, PyObject *args)
{
    core_shutdown(((PoolObject *)op)->core, 1);
    Py_RETURN_FALSE;
}

static PyObject *
pool_get_n_threads(PyObject *op, void *Py_UNUSED(closure))
{
    return PyLong_FromLong(((PoolObject *)op)->core->n_threads);
}

/* Wait until ready(self) holds. Called with consumer_mutex held. */
typedef int (*ready_fn)(ResultsObject *);

static int
ordered_ready(ResultsObject *self)
{
    Job *job = self->job;
    Py_ssize_t i = self->next;
    if (i < _Py_atomic_load_ssize_acquire(&job->next_index)) {
        while (i >= job->head_base + SEG_SIZE) {
            Segment *next = _Py_atomic_load_ptr_acquire(&job->head->next);
            PyMem_Free(job->head);
            job->head = next;
            job->head_base += SEG_SIZE;
        }
        return _Py_atomic_load_int(&job->head->slots[i - job->head_base].state)
               != SLOT_PENDING;
    }
    return _Py_atomic_load_int_acquire(&job->done_pulling)
           && i >= _Py_atomic_load_ssize_acquire(&job->next_index);
}

static int
unordered_ready(ResultsObject *self)
{
    Job *job = self->job;
    if (_Py_atomic_load_ptr_acquire(&job->q_head->next_done) != NULL
        || job->q_head != &job->stub)
    {
        return 1;
    }
    return _Py_atomic_load_int_acquire(&job->done_pulling)
           && _Py_atomic_load_ssize(&job->consumed)
              >= _Py_atomic_load_ssize_acquire(&job->next_index);
}

static int
consumer_wait(ResultsObject *self, ready_fn ready)
{
    Job *job = self->job;
    Worker *w = current_worker;
    int spins = 0;
    int armed = 0;
    while (!ready(self)) {
        if (spins < CONSUMER_SPINS) {
            spins++;
            cpu_relax();
            continue;
        }
        if (w != NULL && w->core == self->core && worker_help(w)) {
            continue;
        }
        if (!armed) {
            _Py_atomic_store_ssize(&job->wanted, self->next);
            _Py_atomic_store_int(&job->consumer_waiting, 1);
            armed = 1;
            continue;
        }
        int one = 1;
        PyTime_t timeout = w != NULL ? 1000 * 1000 : CONSUMER_PARK_NS;
        _PyParkingLot_Park(&job->consumer_waiting, &one, sizeof(one),
                           timeout, NULL, 1);
        armed = 0;
        if (PyErr_CheckSignals() < 0) {
            _Py_atomic_store_int(&job->consumer_waiting, 0);
            return -1;
        }
    }
    if (armed) {
        _Py_atomic_store_int(&job->consumer_waiting, 0);
    }
    return 0;
}

/* Bookkeeping after yielding one result. */
static void
consumed_one(ResultsObject *self)
{
    Job *job = self->job;
    Py_ssize_t before = _Py_atomic_load_ssize_relaxed(&job->consumed);
    _Py_atomic_store_ssize(&job->consumed, before + 1);
    if (job->buffersize
        && _Py_atomic_load_ssize_relaxed(&job->next_index) - before >= job->buffersize)
    {
        core_notify(self->core, 0);
    }
}

static PyObject *
results_end(Job *job)
{
    PyObject *err = job->pull_error;
    if (err != NULL) {
        job->pull_error = NULL;
        PyErr_SetRaisedException(err);
    }
    return NULL;
}

static PyObject *
results_next(PyObject *op)
{
    ResultsObject *self = (ResultsObject *)op;
    Job *job = self->job;
    PyObject *value = NULL;
    int state = SLOT_OK;
    PyMutex_Lock(&job->consumer_mutex);
    if (job->ordered) {
        if (consumer_wait(self, ordered_ready) < 0) {
            goto done;
        }
        if (self->next >= _Py_atomic_load_ssize_acquire(&job->next_index)) {
            results_end(job);
            goto done;
        }
        Slot *slot = &job->head->slots[self->next - job->head_base];
        state = _Py_atomic_load_int_acquire(&slot->state);
        value = slot->value;
        slot->value = NULL;
        self->next++;
        consumed_one(self);
    }
    else {
        Slot *slot;
        for (;;) {
            if (consumer_wait(self, unordered_ready) < 0) {
                goto done;
            }
            slot = queue_pop(job);
            if (slot != NULL) {
                break;
            }
            if (_Py_atomic_load_int_acquire(&job->done_pulling)
                && _Py_atomic_load_ssize(&job->consumed)
                   >= _Py_atomic_load_ssize_acquire(&job->next_index))
            {
                results_end(job);
                goto done;
            }
            /* A push is in progress. */
            cpu_relax();
        }
        state = _Py_atomic_load_int_acquire(&slot->state);
        value = slot->value;
        slot->value = NULL;
        slot->seg->consumed++;
        while (job->head->consumed == SEG_SIZE) {
            Segment *next = _Py_atomic_load_ptr_acquire(&job->head->next);
            if (next == NULL) {
                break;
            }
            PyMem_Free(job->head);
            job->head = next;
        }
        consumed_one(self);
    }
done:
    PyMutex_Unlock(&job->consumer_mutex);
    if (value != NULL && state == SLOT_ERROR) {
        PyErr_SetRaisedException(value);
        return NULL;
    }
    return value;
}

/* For tests: is the buffersize window full? */
static PyObject *
results_is_full(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Job *job = ((ResultsObject *)op)->job;
    return PyBool_FromLong(!job_window_open(job));
}

static PyMethodDef results_methods[] = {
    {"_is_full", results_is_full, METH_NOARGS, NULL},
    {NULL}
};

static void
results_dealloc(PyObject *op)
{
    ResultsObject *self = (ResultsObject *)op;
    PyTypeObject *tp = Py_TYPE(op);
    if (self->job != NULL) {
        /* Unconsumed items still run, as with concurrent.futures, but their
         * results are dropped and buffersize no longer limits them. */
        _Py_atomic_store_int(&self->job->detached, 1);
        if (!_Py_atomic_load_int_acquire(&self->job->done_pulling)) {
            core_notify(self->core, 1);
        }
        job_decref(self->job);
    }
    if (self->core != NULL) {
        core_decref(self->core);
    }
    Py_XDECREF(self->pool);
    PyObject_Free(op);
    Py_DECREF(tp);
}

/* ---- Scope ------------------------------------------------------------- */

static PyObject *
pool_scope(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PoolObject *self = (PoolObject *)op;
    PoolCore *core = self->core;
    if (getpid() != core->pid) {
        PyErr_SetString(PyExc_RuntimeError, "the pool cannot be used after fork()");
        return NULL;
    }
    if (_Py_atomic_load_int(&core->shutdown)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot open a scope after the pool was shut down");
        return NULL;
    }
    ScopeObject *scope = PyObject_GC_New(ScopeObject, self->scope_type);
    if (scope == NULL) {
        return NULL;
    }
    scope->errors = PyList_New(0);
    if (scope->errors == NULL) {
        scope->pool = NULL;
        scope->core = NULL;
        Py_DECREF(scope);
        return NULL;
    }
    scope->pool = Py_NewRef(op);
    core_incref(core);
    scope->core = core;
    scope->pending = 0;
    scope->cancelled = 0;
    scope->closed = 0;
    scope->waiting = 0;
    PyObject_GC_Track(scope);
    return (PyObject *)scope;
}

static PyObject *
scope_spawn(PyObject *op, PyObject *const *args, Py_ssize_t nargsf,
            PyObject *kwnames)
{
    ScopeObject *scope = (ScopeObject *)op;
    PoolCore *core = scope->core;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "spawn() missing required argument 'func'");
        return NULL;
    }
    if (_Py_atomic_load_int(&scope->closed)) {
        PyErr_SetString(PyExc_RuntimeError, "the scope is closed");
        return NULL;
    }
    if (_Py_atomic_load_int(&core->shutdown)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot spawn after the pool was shut down");
        return NULL;
    }
    if (_Py_atomic_load_int(&scope->cancelled)) {
        Py_RETURN_NONE;           /* the scope is failing: drop the task */
    }
    Py_ssize_t nfuncargs = nargs - 1;
    Task *task = PyMem_Malloc(sizeof(Task)
                              + (nfuncargs > 0 ? nfuncargs - 1 : 0) * sizeof(PyObject *));
    if (task == NULL) {
        return PyErr_NoMemory();
    }
    task->kwargs = NULL;
    if (kwnames != NULL && PyTuple_GET_SIZE(kwnames) > 0) {
        task->kwargs = PyDict_New();
        if (task->kwargs == NULL) {
            PyMem_Free(task);
            return NULL;
        }
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            if (PyDict_SetItem(task->kwargs, PyTuple_GET_ITEM(kwnames, i),
                               args[nargs + i]) < 0)
            {
                Py_DECREF(task->kwargs);
                PyMem_Free(task);
                return NULL;
            }
        }
    }
    task->context = PyContext_CopyCurrent();
    if (task->context == NULL) {
        Py_XDECREF(task->kwargs);
        PyMem_Free(task);
        return NULL;
    }
    task->func = Py_NewRef(args[0]);
    task->nargs = nfuncargs;
    for (Py_ssize_t i = 0; i < nfuncargs; i++) {
        task->args[i] = Py_NewRef(args[i + 1]);
    }
    task->scope = (ScopeObject *)Py_NewRef(op);
    _Py_atomic_add_ssize(&scope->pending, 1);

    Worker *w = current_worker;
    if (w == NULL || w->core != core
        || deque_push(&core->deques[w->index], task) < 0)
    {
        injector_push(core, task);
    }
    /* Pairs with the fence in an idle worker between announcing its sleep
     * and rechecking for tasks. */
    _Py_atomic_fence_seq_cst();
    if (_Py_atomic_load_int(&core->sleepers) > 0) {
        core_notify(core, 0);
    }
    Py_RETURN_NONE;
}

/* Wait until every task of the scope has finished. A pool worker runs pool
 * work meanwhile. Returns a KeyboardInterrupt (or other signal handler
 * exception) raised while waiting, or NULL; once one is raised the scope is
 * cancelled and the wait continues for the running tasks. */
static PyObject *
scope_wait(ScopeObject *scope)
{
    PoolCore *core = scope->core;
    Worker *w = current_worker;
    int helper = w != NULL && w->core == core;
    PyObject *interrupt = NULL;
    int spins = 0;
    int armed = 0;
    while (_Py_atomic_load_ssize(&scope->pending) > 0) {
        if (helper) {
            if (worker_help(w)) {
                spins = 0;
                continue;
            }
        }
        else if (_Py_atomic_load_ssize(&core->live_workers) == 0
                 || getpid() != core->pid)
        {
            /* No workers to run the tasks (shut down, or after fork()):
             * run them here. */
            Task *task = injector_pop(core);
            if (task != NULL) {
                run_task(task);
                continue;
            }
        }
        if (spins < CONSUMER_SPINS) {
            spins++;
            cpu_relax();
            continue;
        }
        if (!armed) {
            _Py_atomic_store_int(&scope->waiting, 1);
            armed = 1;
            continue;
        }
        int one = 1;
        _PyParkingLot_Park(&scope->waiting, &one, sizeof(one),
                           helper ? 1000 * 1000 : SCOPE_PARK_NS, NULL, 1);
        armed = 0;
        if (interrupt == NULL && PyErr_CheckSignals() < 0) {
            interrupt = PyErr_GetRaisedException();
            _Py_atomic_store_int(&scope->cancelled, 1);
        }
    }
    if (armed) {
        _Py_atomic_store_int(&scope->waiting, 0);
    }
    return interrupt;
}

static PyObject *
scope_enter(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    if (_Py_atomic_load_int(&((ScopeObject *)op)->closed)) {
        PyErr_SetString(PyExc_RuntimeError, "the scope is closed");
        return NULL;
    }
    return Py_NewRef(op);
}

static PyObject *
scope_exit(PyObject *op, PyObject *args)
{
    ScopeObject *scope = (ScopeObject *)op;
    PyObject *exc_type, *exc, *tb;
    if (!PyArg_UnpackTuple(args, "__exit__", 3, 3, &exc_type, &exc, &tb)) {
        return NULL;
    }
    if (exc != Py_None) {
        _Py_atomic_store_int(&scope->cancelled, 1);
    }
    PyObject *interrupt = scope_wait(scope);
    _Py_atomic_store_int(&scope->closed, 1);
    /* No task can run now, so the error list is ours. */
    PyObject *errors = scope->errors;
    Py_ssize_t n = PyList_GET_SIZE(errors);
    if (n == 0) {
        if (interrupt != NULL) {
            PyErr_SetRaisedException(interrupt);
            return NULL;
        }
        Py_RETURN_FALSE;
    }
    PyObject *excs = PyList_New(0);
    if (excs == NULL) {
        Py_XDECREF(interrupt);
        return NULL;
    }
    PyObject *body = interrupt != NULL ? interrupt : (exc != Py_None ? Py_NewRef(exc) : NULL);
    if ((body != NULL && PyList_Append(excs, body) < 0)
        || PyList_SetSlice(excs, PyList_GET_SIZE(excs), PyList_GET_SIZE(excs), errors) < 0
        || PyList_SetSlice(errors, 0, n, NULL) < 0)
    {
        Py_XDECREF(body);
        Py_DECREF(excs);
        return NULL;
    }
    Py_XDECREF(body);
    PyObject *group = PyObject_CallFunction(PyExc_BaseExceptionGroup, "sO",
                                            "unhandled errors in a Scope", excs);
    Py_DECREF(excs);
    if (group != NULL) {
        PyErr_SetRaisedException(group);
    }
    return NULL;
}

static int
scope_traverse(PyObject *op, visitproc visit, void *arg)
{
    ScopeObject *scope = (ScopeObject *)op;
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(scope->pool);
    Py_VISIT(scope->errors);
    return 0;
}

static int
scope_clear(PyObject *op)
{
    ScopeObject *scope = (ScopeObject *)op;
    Py_CLEAR(scope->errors);
    return 0;
}

static void
scope_dealloc(PyObject *op)
{
    ScopeObject *scope = (ScopeObject *)op;
    PyTypeObject *tp = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    Py_CLEAR(scope->errors);
    Py_CLEAR(scope->pool);
    if (scope->core != NULL) {
        core_decref(scope->core);
    }
    PyObject_GC_Del(op);
    Py_DECREF(tp);
}

PyDoc_STRVAR(scope_doc,
"A scope for spawning tasks into a Pool; made by Pool.scope().\n\n"
"Leaving the with block waits until every spawned task, including tasks\n"
"spawned by tasks, has finished. If tasks raised, the first error cancels\n"
"the tasks not yet started and leaving raises a BaseExceptionGroup of all\n"
"of them (preceded by the with block's own exception, if any).");

PyDoc_STRVAR(spawn_doc,
"spawn($self, func, /, *args, **kwargs)\n--\n\n"
"Run func(*args, **kwargs) on the pool, in a copy of the current\n"
"contextvars context. Returns None; tasks report results through shared\n"
"state such as atoms. Ignored once a task of the scope has failed.");

static PyMethodDef scope_methods[] = {
    {"spawn", _PyCFunction_CAST(scope_spawn), METH_FASTCALL | METH_KEYWORDS, spawn_doc},
    {"__enter__", scope_enter, METH_NOARGS, NULL},
    {"__exit__", scope_exit, METH_VARARGS, NULL},
    {NULL}
};

static PyType_Slot scope_type_slots[] = {
    {Py_tp_dealloc, scope_dealloc},
    {Py_tp_traverse, scope_traverse},
    {Py_tp_clear, scope_clear},
    {Py_tp_doc, (void *)scope_doc},
    {Py_tp_methods, scope_methods},
    {0}
};

static PyType_Spec scope_spec = {
    "grail.Scope", sizeof(ScopeObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE
    | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    scope_type_slots
};

PyDoc_STRVAR(pool_doc,
"Pool(n_threads)\n--\n\n"
"A pool of worker threads with low per-task overhead.\n\n"
"Based on the design of Quansight's spinningjenny.");

PyDoc_STRVAR(map_doc,
"map($self, func, /, *iterables, buffersize=None, return_in_order=True)\n--\n\n"
"Return an iterator of func applied to the items of the iterables.\n\n"
"Tasks run in a copy of the contextvars context current at the call.\n"
"buffersize bounds the number of claimed but unyielded items. With\n"
"return_in_order=False, results are yielded as they complete.\n"
"An exception raised by func is raised when its result is reached.");

static PyMethodDef pool_methods[] = {
    {"map", _PyCFunction_CAST(pool_map), METH_FASTCALL | METH_KEYWORDS, map_doc},
    {"scope", pool_scope, METH_NOARGS,
     "Return a Scope for spawning tasks; use it as a context manager."},
    {"shutdown", _PyCFunction_CAST(pool_shutdown), METH_VARARGS | METH_KEYWORDS,
     "Stop the workers. Maps with inputs left raise RuntimeError, except\n"
     "maps whose result iterator was dropped, which run to completion."},
    {"__enter__", pool_enter, METH_NOARGS, NULL},
    {"__exit__", pool_exit, METH_VARARGS, NULL},
    {NULL}
};

static PyGetSetDef pool_getsets[] = {
    {"n_threads", pool_get_n_threads, NULL, "The number of worker threads.", NULL},
    {NULL}
};

static PyType_Slot pool_type_slots[] = {
    {Py_tp_new, pool_new},
    {Py_tp_dealloc, pool_dealloc},
    {Py_tp_doc, (void *)pool_doc},
    {Py_tp_methods, pool_methods},
    {Py_tp_getset, pool_getsets},
    {0}
};

static PyType_Spec pool_spec = {
    "grail.Pool", sizeof(PoolObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
    pool_type_slots
};

static PyType_Slot results_type_slots[] = {
    {Py_tp_dealloc, results_dealloc},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, results_next},
    {Py_tp_methods, results_methods},
    {0}
};

static PyType_Spec results_spec = {
    "grail.PoolMapResults", sizeof(ResultsObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    results_type_slots
};

static int
pool_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    PoolState *state = PyModule_GetState(module);
    Py_VISIT(state->pool_type);
    Py_VISIT(state->results_type);
    Py_VISIT(state->scope_type);
    return 0;
}

static int
pool_module_clear(PyObject *module)
{
    PoolState *state = PyModule_GetState(module);
    Py_CLEAR(state->pool_type);
    Py_CLEAR(state->results_type);
    Py_CLEAR(state->scope_type);
    return 0;
}

static int
pool_exec(PyObject *module)
{
    PoolState *state = PyModule_GetState(module);
    state->results_type = (PyTypeObject *)PyType_FromModuleAndSpec(
        module, &results_spec, NULL);
    if (state->results_type == NULL) {
        return -1;
    }
    state->scope_type = (PyTypeObject *)PyType_FromModuleAndSpec(
        module, &scope_spec, NULL);
    if (state->scope_type == NULL) {
        return -1;
    }
    state->pool_type = (PyTypeObject *)PyType_FromModuleAndSpec(
        module, &pool_spec, NULL);
    if (state->pool_type == NULL) {
        return -1;
    }
    return PyModule_AddObjectRef(module, "Pool", (PyObject *)state->pool_type);
}

PyABIInfo_VAR(pool_abi);
static PyModuleDef_Slot pool_module_slots[] = {
    {Py_mod_abi, &pool_abi},
    {Py_mod_exec, pool_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef pool_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail_pool",
    .m_size = sizeof(PoolState),
    .m_slots = pool_module_slots,
    .m_traverse = pool_module_traverse,
    .m_clear = pool_module_clear,
};

PyMODINIT_FUNC
PyInit__grail_pool(void)
{
    return PyModuleDef_Init(&pool_module);
}
