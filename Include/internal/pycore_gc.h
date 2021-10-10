#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pystate.h"
#include "mimalloc.h"
#include "mimalloc-internal.h"

#ifdef __cplusplus
#define _Py_ALIGN_AS alignas
#elif defined(_MSC_VER)
#define _Py_ALIGN_AS(n) __declspec(align(n))
#else
#define _Py_ALIGN_AS _Alignas
#endif

/* GC information is stored BEFORE the object structure. */
typedef struct {
    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    _Py_ALIGN_AS(16) uintptr_t _gc_prev;

    // Pointer to next object in the list.
    // 0 means the object is not tracked
    uintptr_t _gc_next;
} PyGC_Head;


/* GC runtime state */

/* If we change this, we need to change the default value in the
   signature of gc.collect. */
#define NUM_GENERATIONS 3
/*
   NOTE: about untracking of mutable objects.

   Certain types of container cannot participate in a reference cycle, and
   so do not need to be tracked by the garbage collector. Untracking these
   objects reduces the cost of garbage collections. However, determining
   which objects may be untracked is not free, and the costs must be
   weighed against the benefits for garbage collection.

   There are two possible strategies for when to untrack a container:

   i) When the container is created.
   ii) When the container is examined by the garbage collector.

   Tuples containing only immutable objects (integers, strings etc, and
   recursively, tuples of immutable objects) do not need to be tracked.
   The interpreter creates a large number of tuples, many of which will
   not survive until garbage collection. It is therefore not worthwhile
   to untrack eligible tuples at creation time.

   Instead, all tuples except the empty tuple are tracked when created.
   During garbage collection it is determined whether any surviving tuples
   can be untracked. A tuple can be untracked if all of its contents are
   already not tracked. Tuples are examined for untracking in all garbage
   collection cycles. It may take more than one cycle to untrack a tuple.

   Dictionaries containing only immutable objects also do not need to be
   tracked. Dictionaries are untracked when created. If a tracked item is
   inserted into a dictionary (either as a key or value), the dictionary
   becomes tracked. During a full garbage collection (all generations),
   the collector will untrack any dictionaries whose contents are not
   tracked.

   The module provides the python function is_tracked(obj), which returns
   the CURRENT tracking status of the object. Subsequent garbage
   collections may change the tracking status of the object.

   Untracking of certain containers was introduced in issue #4688, and
   the algorithm was refined in response to issue #14775.
*/

struct gc_generation {
    PyGC_Head head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head *generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;

    int64_t gc_live;

    int64_t gc_threshold;

    int gc_scale;

    /* Number of threads that must park themselves to stop-the-world.
       Protected by HEAD_LOCK(runtime). */
    int32_t gc_thread_countdown;

    _PyRawEvent gc_stop_event;
};

#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)
#define _Py_FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

#if 0
/* See also private _PyObject_GC_IS_TRACKED() macro. */
// TODO: should this be part of the public C-API and move to object.h?
PyAPI_FUNC(int) PyObject_GC_IsTracked(void *);
#define _PyObject_GC_IS_TRACKED(o) PyObject_GC_IsTracked(o)
#endif

/* Bit flags for _gc_prev */
/* Bit 0 is set when tp_finalize is called */
// #define _PyGC_PREV_MASK_FINALIZED  (1)
/* Bit 1 is set when the object is in generation which is GCed currently. */
// #define _PyGC_PREV_MASK_COLLECTING (2)
/* The (N-2) most significant bits contain the real address. */
#define _PyGC_PREV_SHIFT           (4)
#define _PyGC_PREV_MASK            (((uintptr_t) -1) << _PyGC_PREV_SHIFT)

// Lowest bit of _gc_next is used for flags only in GC.
// But it is always 0 for normal code.
#define _PyGCHead_NEXT(g)        ((PyGC_Head*)(g)->_gc_next)
#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (uintptr_t)(p))

// Lowest two bits of _gc_prev is used for _PyGC_PREV_MASK_* flags.
#define _PyGCHead_PREV(g) ((PyGC_Head*)((g)->_gc_prev & _PyGC_PREV_MASK))
#define _PyGCHead_SET_PREV(g, p) do { \
    assert(((uintptr_t)p & ~_PyGC_PREV_MASK) == 0); \
    (g)->_gc_prev = ((g)->_gc_prev & ~_PyGC_PREV_MASK) \
        | ((uintptr_t)(p)); \
    } while (0)



//   0   1   2   3   4   5   6   7
// *-------------------------------*
// |  GEN  | U | F | RSRVD         |
// *-------------------------------*


#define _PyObject_GC_TRACK(op) \
    _PyObject_GC_TRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

#define _PyObject_GC_UNTRACK(op) \
    _PyObject_GC_UNTRACK_impl(__FILE__, __LINE__, _PyObject_CAST(op))

#undef _PyObject_GC_IS_TRACKED
#define _PyObject_GC_IS_TRACKED(o) \
    (_PyObject_GC_IS_TRACKED_impl(_Py_AS_GC(o)))

// _PyGC_FINALIZED(o) defined in objimpl.h (used by Cython)

#define _PyGC_SET_FINALIZED(o) \
    (_PyObject_GC_SET_FINALIZED_impl(_Py_AS_GC(o)))

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && \
        (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))


#define GC_TRACKED_SHIFT     (0)
#define GC_UNREACHABLE_SHIFT (2)
#define GC_FINALIZED_SHIFT   (3)

#define GC_TRACKED_MASK      (1<<GC_TRACKED_SHIFT)   // 3
#define GC_UNREACHABLE_MASK  (1<<GC_UNREACHABLE_SHIFT)  // 4
#define GC_FINALIZED_MASK    (1<<GC_FINALIZED_SHIFT)    // 8

static inline int
GC_BITS_IS_TRACKED(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_TRACKED_MASK) >> GC_TRACKED_SHIFT;
}

static inline int
GC_BITS_IS_UNREACHABLE(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_UNREACHABLE_MASK) >> GC_UNREACHABLE_SHIFT;
}

static inline int
GC_BITS_IS_FINALIZED(PyGC_Head *gc)
{
    return (gc->_gc_prev & GC_FINALIZED_MASK) >> GC_FINALIZED_SHIFT;
}

static inline void
GC_BITS_CLEAR(PyGC_Head *gc, uintptr_t mask)
{
    gc->_gc_prev &= ~mask;
}

static inline void
GC_BITS_SET(PyGC_Head *gc, uintptr_t mask)
{
    gc->_gc_prev |= mask;
}

static inline int
_PyObject_GC_IS_TRACKED_impl(PyGC_Head *gc)
{
    return GC_BITS_IS_TRACKED(gc) != 0;
}

static inline int
_PyObject_GC_FINALIZED_impl(PyGC_Head *gc)
{
    return GC_BITS_IS_FINALIZED(gc) != 0;
}

static inline void
_PyObject_GC_SET_FINALIZED_impl(PyGC_Head *gc)
{
    GC_BITS_SET(gc, GC_FINALIZED_MASK);
}

/* Tell the GC to track this object.
 *
 * NB: While the object is tracked by the collector, it must be safe to call the
 * ob_traverse method.
 *
 * Internal note: _PyRuntime.gc.generation0->_gc_prev doesn't have any bit flags
 * because it's not object header.  So we don't use _PyGCHead_PREV() and
 * _PyGCHead_SET_PREV() for it to avoid unnecessary bitwise operations.
 *
 * The PyObject_GC_Track() function is the public version of this macro.
 */
static inline void
_PyObject_GC_TRACK_impl(const char *filename, int lineno, PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_TRACK");

    PyGC_Head *gc = _Py_AS_GC(op);

    gc->_gc_prev |= (1 << GC_TRACKED_SHIFT);
    assert(GC_BITS_IS_TRACKED(gc) == 1);
}

static inline int
_PyGC_ShouldCollect(struct _gc_runtime_state *gcstate)
{
    int64_t live = _Py_atomic_load_int64_relaxed(&gcstate->gc_live);
    return !gcstate->collecting && gcstate->enabled && live >= gcstate->gc_threshold;
}

void gc_list_remove(PyGC_Head *node);

/* Tell the GC to stop tracking this object.
 *
 * Internal note: This may be called while GC. So _PyGC_PREV_MASK_COLLECTING
 * must be cleared. But _PyGC_PREV_MASK_FINALIZED bit is kept.
 *
 * The object must be tracked by the GC.
 *
 * The PyObject_GC_UnTrack() function is the public version of this macro.
 */
static inline void
_PyObject_GC_UNTRACK_impl(const char *filename, int lineno, PyObject *op)
{
    _PyObject_ASSERT_FROM(op, _PyObject_GC_IS_TRACKED(op),
                          "object not tracked by the garbage collector",
                          filename, lineno, "_PyObject_GC_UNTRACK");

    PyGC_Head *gc = _Py_AS_GC(op);
    if (gc->_gc_next != 0) {
        assert(gc->_gc_next != 0);
        assert(gc->_gc_prev != 0);
        gc_list_remove(gc);
    }
    assert(_PyGCHead_PREV(gc) == NULL);
    gc->_gc_prev &= GC_FINALIZED_MASK;
    assert(GC_BITS_IS_TRACKED(gc) == 0);
}



PyAPI_FUNC(void) _PyGC_InitState(struct _gc_runtime_state *);
PyAPI_FUNC(void) _PyGC_ResetHeap(void);
PyAPI_FUNC(Py_ssize_t) _PyGC_Collect(PyThreadState *);

// Functions to clear types free lists
extern void _PyFrame_ClearFreeList(void);
extern void _PyTuple_ClearFreeList(void);
extern void _PyFloat_ClearFreeList(void);
extern void _PyList_ClearFreeList(void);
extern void _PyDict_ClearFreeList(void);
extern void _PyAsyncGen_ClearFreeLists(void);
extern void _PyContext_ClearFreeList(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_GC_H */
