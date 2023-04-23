#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stdbool.h>

/* GC information is stored BEFORE the object structure. */
typedef struct _gc_head {
    // Pointer to next object in the list.
    // 0 means the object is not tracked
    struct _gc_head *_gc_next;

    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    struct _gc_head *_gc_prev;

#if 1
    // Add two words for "colors" version of cyclic GC.  We don't actually need
    // two words, just a few bits, but we want to preserve alignment.  Ideally
    // this state would be moved into PyObject (bits in ob_refcnt) or as
    // bitmaps in arena headers.
    uint32_t _gc_color; // white, grey or black
    uint32_t _gc_flags; // various flags used during and between collection
    int64_t _gc_gen; // current GC generation of object
#endif

} PyGC_Head;

#define _PyGC_HEAD_WORDS 4


#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)
#define _PyGC_Head_UNUSED PyGC_Head

/* True if the object is currently tracked by the GC. */
#define _PyObject_GC_IS_TRACKED(o) (_Py_AS_GC(o)->_gc_next != 0)

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && \
        (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))


#define _PyGCHead_NEXT(g)        ((PyGC_Head*)(g)->_gc_next)

#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (p))

#define _PyGCHead_PREV(g)        ((PyGC_Head*)(g)->_gc_prev)
#define _PyGCHead_SET_PREV(g, p) ((g)->_gc_prev = (PyGC_Head*)(p))

#define GC_FLAG_FINIALIZER_REACHABLE (1<<0)
/* reachable from legacy finalizer, cannot be collected */
#define GC_FLAG_LEGACY_FINIALIZER_REACHABLE (1<<1)
/* set when tp_finalize is called */
#define GC_FLAG_FINALIZED  (1<<3)
/* set when object is member of collection set, being collected */
#define GC_FLAG_COLLECTING  (1<<4)
/* set if tp_dealloc() needs to be called after collection */
#define GC_FLAG_DEALLOC  (1<<5)

static inline void
_PyGC_Set_Flag(PyObject *op, uint32_t flag)
{
    _Py_AS_GC(op)->_gc_flags |= flag;
}

static inline void
_PyGC_Clear_Flag(PyObject *op, uint32_t flag)
{
    _Py_AS_GC(op)->_gc_flags &= ~flag;
}

static inline bool
_PyGC_Have_Flag(PyObject *op, uint32_t flag)
{
    return _Py_AS_GC(op)->_gc_flags & flag;
}

static inline int
_PyGC_FINALIZED(PyObject *op)
{
    return _PyGC_Have_Flag(op, GC_FLAG_FINALIZED);
}

static inline void
_PyGC_SET_FINALIZED(PyObject *op)
{
    _PyGC_Set_Flag(op, GC_FLAG_FINALIZED);
}

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

    /* Is automatic collection enabled? */
    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head gc_head;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;
};


extern void _PyGC_InitState(struct _gc_runtime_state *);

extern Py_ssize_t _PyGC_CollectNoFail(PyThreadState *tstate);


// Functions to clear types free lists
extern void _PyTuple_ClearFreeList(PyInterpreterState *interp);
extern void _PyFloat_ClearFreeList(PyInterpreterState *interp);
extern void _PyList_ClearFreeList(PyInterpreterState *interp);
extern void _PyDict_ClearFreeList(PyInterpreterState *interp);
extern void _PyAsyncGen_ClearFreeLists(PyInterpreterState *interp);
extern void _PyContext_ClearFreeList(PyInterpreterState *interp);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_GC_H */
