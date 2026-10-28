#ifndef Py_INTERNAL_TSTATE_H
#define Py_INTERNAL_TSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_brc.h"             // struct _brc_thread_state
#include "pycore_freelist_state.h"  // struct _Py_freelists
#include "pycore_interpframe_structs.h"  // _PyInterpreterFrame
#include "pycore_mimalloc.h"        // struct _mimalloc_thread_state
#include "pycore_qsbr.h"            // struct qsbr
#include "pycore_uop.h"             // struct _PyUOpInstruction
#include "pycore_structs.h"

#ifdef Py_GIL_DISABLED
struct _gc_thread_state {
    /* Thread-local allocation count. */
    Py_ssize_t alloc_count;
};
#endif


// Every PyThreadState is actually allocated as a _PyThreadStateImpl. The
/* Entries of _PyThreadStateImpl.vo_shadow and vo_deferred. */
#define _Py_VO_SHADOW_SIZE 64
#define _Py_VO_DEFERRED_SIZE 64

// PyThreadState fields are exposed as part of the C API, although most fields
// are intended to be private. The _PyThreadStateImpl fields not exposed.
typedef struct _PyThreadStateImpl {
    // semi-public fields are in PyThreadState.
    PyThreadState base;

    // Embedded base frame - sentinel at the bottom of the frame stack.
    // Used by profiling/sampling to detect incomplete stack traces.
    _PyInterpreterFrame base_frame;

    // The reference count field is used to synchronize deallocation of the
    // thread state during runtime finalization.
    Py_ssize_t refcount;

    /* Tier 2: field values of the running executor's virtual objects (see
     * "Virtual objects" in Python/optimizer_analysis.c), owned. Every
     * entry is PyStackRef_NULL whenever no executor is running. JIT
     * stencils are compiled without _Py_TIER2, so this comes before any
     * field that depends on it. */
    _PyStackRef vo_shadow[_Py_VO_SHADOW_SIZE];
    /* Objects whose last reference a trace dropped while virtual objects
     * were live, freed by _PyVO_FlushDeferred() (owned). */
    PyObject *vo_deferred[_Py_VO_DEFERRED_SIZE];
    int vo_ndeferred;

    // These are addresses, but we need to convert to ints to avoid UB.
    uintptr_t c_stack_top;
    uintptr_t c_stack_soft_limit;
    uintptr_t c_stack_hard_limit;

    // PyUnstable_ThreadState_ResetStackProtection() values
    uintptr_t c_stack_init_base;
    uintptr_t c_stack_init_top;

    PyObject *asyncio_running_loop; // Strong reference
    PyObject *asyncio_running_task; // Strong reference

    // Distinguishes between yield and return from PyEval_EvalFrame().
    // See gen_send_ex2() in Objects/genobject.c
    enum {
        GENERATOR_RETURN = 0,
        GENERATOR_YIELD = 1,
    } generator_return_kind;

    /* Head of circular linked-list of all tasks which are instances of `asyncio.Task`
       or subclasses of it used in `asyncio.all_tasks`.
    */
    struct llist_node asyncio_tasks_head;
    struct _qsbr_thread_state *qsbr;  // only used by free-threaded build
    struct llist_node mem_free_queue; // delayed free queue

#ifdef Py_GIL_DISABLED
    // Stack references for the current thread that exist on the C stack
    struct _PyCStackRef *c_stack_refs;
    struct _gc_thread_state gc;
    struct _mimalloc_thread_state mimalloc;
    struct _Py_freelists freelists;
    struct _brc_thread_state brc;
    struct {
        // The per-thread refcounts
        Py_ssize_t *values;

        // Size of the refcounts array.
        Py_ssize_t size;

        // If set, don't use per-thread refcounts
        int is_finalized;
    } refcounts;

    // Index to use to retrieve thread-local bytecode for this thread
    int32_t tlbc_index;

    // When >1, code objects do not immortalize their non-string constants.
    int suppress_co_const_immortalization;

#ifdef Py_STATS
     // per-thread stats, will be merged into interp->pystats_struct
     PyStats *pystats_struct; // allocated by _PyStats_ThreadInit()
#endif

#endif // Py_GIL_DISABLED

#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
    Py_ssize_t reftotal;  // this thread's total refcount operations
#endif
#if _Py_TIER2
    struct _PyJitTracerState *jit_tracer_state;
#endif

    // Reused for the thread's lifetime; owns placeholders while they resolve.
    PyObject *lazy_imports;

#ifdef Py_GIL_DISABLED
    // gh-144438: Add padding to ensure that the fields above don't share a
    // cache line with other allocations.
    char __padding[64];
#endif
} _PyThreadStateImpl;

/* Tier 2: drop a reference while virtual objects are live. Freeing the
 * object could run code that inspects frames, so that waits for
 * _PyVO_FlushDeferred(). Runs no code. */
static inline void
_PyVO_DeferDecref(PyThreadState *tstate, PyObject *op)
{
    if (_Py_IsImmortal(op)) {
        return;
    }
    if (Py_REFCNT(op) > 1) {
        Py_DECREF(op);
        return;
    }
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    assert(ts->vo_ndeferred < _Py_VO_DEFERRED_SIZE);
    ts->vo_deferred[ts->vo_ndeferred++] = op;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_TSTATE_H */
