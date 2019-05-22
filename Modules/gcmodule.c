/*

  Reference Cycle Garbage Collection
  ==================================

  Neil Schemenauer <nas@arctrix.com>

  Based on a post on the python-dev list.  Ideas from Guido van Rossum,
  Eric Tiedemann, and various others.

  http://www.arctrix.com/nas/python/gc/

  The following mailing list threads provide a historical perspective on
  the design of this module.  Note that a fair amount of refinement has
  occurred since those discussions.

  http://mail.python.org/pipermail/python-dev/2000-March/002385.html
  http://mail.python.org/pipermail/python-dev/2000-March/002434.html
  http://mail.python.org/pipermail/python-dev/2000-March/002497.html

  For a highlevel view of the collection process, read the collect
  function.

*/

#include "Python.h"
#include "pycore_context.h"
#include "pycore_object.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "frameobject.h"        /* for PyFrame_ClearFreeList */
#include "pydtrace.h"
#include "pytime.h"             /* for _PyTime_GetMonotonicClock() */
#include <stdbool.h>

/*[clinic input]
module gc
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b5c9690ecc842d79]*/

#define GC_DEBUG (0)  /* Enable more asserts */

#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

/* Get an object's GC head */
#define AS_GC(o) ((PyGC_Head *)(o)-1)

/* Get the object given the GC head */
#define FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

/* Queue to avoid stack overflow when marking reachable objects */
#define MARK_QUEUE_SIZE 100

struct mark_state {
    PyGC_Head *reachable;
    int generation;
    //PyObject *queue[MARK_QUEUE_SIZE];
    int queue_depth;
    int aborted; /* true if queue depth would have been exceeded, restart */
    bool finalizers; /* we are marking finalizers */
    PyObject *ref; /* for verbose debugging */
};

typedef struct _gc_runtime_state gc_state_t;

#define COLOR_BLACK 'B' // alive
#define COLOR_GREY  'G' // alive but needs traverse
#define COLOR_WHITE 'W' // dead (or potentially dead before traversal)

#define GET_COLOR(g) ((g)->gc_color)
#define SET_COLOR(g, v) ((g)->gc_color = v)

#define IS_BLACK(g) (GET_COLOR(g) == COLOR_BLACK)
#define IS_GREY(g) (GET_COLOR(g) == COLOR_GREY)
#define IS_WHITE(g) (GET_COLOR(g) == COLOR_WHITE)

#define SET_GEN(g, v) ((g)->gc_gen = v)
#define GET_GEN(g) ((g)->gc_gen)

bool debug_verbose;

static void
dump_obj(PyObject *op)
{
    PyObject *repr = NULL, *bytes = NULL;
    repr = PyObject_Repr(op);
    if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr)))
        PyErr_WriteUnraisable(op);
    else {
        PySys_WriteStderr("%s\n", PyBytes_AS_STRING(bytes));
    }
    Py_XDECREF(repr);
    Py_XDECREF(bytes);
}

static inline int
gc_is_finalized(PyGC_Head *g)
{
    return _PyGC_HAVE_FLAG(g, GC_FLAG_FINALIZED);
}

static inline Py_ssize_t
gc_get_refs(PyGC_Head *g)
{
    return g->_gc_refs;
}

static inline void
gc_set_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_refs = refs;
}

static inline void
gc_decref(PyGC_Head *g)
{
    if (g->_gc_refs == 0)
        return;
    _PyObject_ASSERT_WITH_MSG(FROM_GC(g),
                              gc_get_refs(g) > 0,
                              "refcount is too small");
    g->_gc_refs -= 1;
}

/* Python string to use if unhandled exception occurs */
static PyObject *gc_str = NULL;

/* set for debugging information */
#define DEBUG_STATS             (1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE       (1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE     (1<<2) /* print uncollectable objects */
#define DEBUG_SAVEALL           (1<<5) /* save all garbage in gc.garbage */
#define DEBUG_VERBOSE           (1<<6) /* verbose debuggin output */
#define DEBUG_LEAK              DEBUG_COLLECTABLE | \
                DEBUG_UNCOLLECTABLE | \
                DEBUG_SAVEALL

#define GEN_HEAD(state, n) (&(state)->generations[n].head)
#define HEAD(state) (&(state)->generations[0].head)

void
_PyGC_Initialize(gc_state_t *state)
{
    state->enabled = 1; /* automatic collection enabled? */

#define _GEN_HEAD(n) GEN_HEAD(state, n)
    struct gc_generation generations[NUM_GENERATIONS] = {
        /* PyGC_Head,                                    threshold,    count */
        {{_GEN_HEAD(0), _GEN_HEAD(0)},   700,        0},
        {{_GEN_HEAD(1), _GEN_HEAD(1)},   10,         0},
        {{_GEN_HEAD(2), _GEN_HEAD(2)},   10,         0},
    };
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        state->generations[i] = generations[i];
    };
    state->generation0 = GEN_HEAD(state, 0);
    struct gc_generation permanent_generation = {
          {&state->permanent_generation.head,
           &state->permanent_generation.head}, 0, 0
    };
    state->permanent_generation = permanent_generation;
}

/*
_gc_prev values
---------------

    used for doubly linked list

gc_refs
-------
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.

_gc_next values
---------------

    0
        The object is not tracked

    != 0
        Pointer to the next object in the GC list.

*/

/*** list functions ***/

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = list;
    list->_gc_next = list;
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == list);
}

#if 0
/* Append `node` to `list`. */
static inline void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
    PyGC_Head *last = (PyGC_Head *)list->_gc_prev;

    // last <-> node
    _PyGCHead_SET_PREV(node, last);
    _PyGCHead_SET_NEXT(last, node);

    // node <-> list
    _PyGCHead_SET_NEXT(node, list);
    list->_gc_prev = node;
}
#endif

/* Remove `node` from the gc list it's currently in. */
static inline void
gc_list_remove(PyGC_Head *node)
{
    PyGC_Head *prev = GC_PREV(node);
    PyGC_Head *next = GC_NEXT(node);

    _PyGCHead_SET_NEXT(prev, next);
    _PyGCHead_SET_PREV(next, prev);

    node->_gc_next = 0; /* object is not currently tracked */
}

#if 0
/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = node;
    _PyGCHead_SET_NEXT(node, list);
}
#endif

/* append list `from` onto list `to`; `from` becomes an empty list */
static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
    assert(from != to);
    if (!gc_list_is_empty(from)) {
        PyGC_Head *to_tail = GC_PREV(to);
        PyGC_Head *from_head = GC_NEXT(from);
        PyGC_Head *from_tail = GC_PREV(from);
        assert(from_head != from);
        assert(from_tail != from);

        _PyGCHead_SET_NEXT(to_tail, from_head);
        _PyGCHead_SET_PREV(from_head, to_tail);

        _PyGCHead_SET_NEXT(from_tail, to);
        _PyGCHead_SET_PREV(to, from_tail);
    }
    gc_list_init(from);
}

static Py_ssize_t
gc_list_size(PyGC_Head *list)
{
    PyGC_Head *gc;
    Py_ssize_t n = 0;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        n++;
    }
    return n;
}

/* Append objects in a GC list to a Python list.
 * Return 0 if all OK, < 0 if error (out of memory for list).
 */
static int
append_objects(PyObject *py_list, PyGC_Head *gc_list, int generation)
{
    PyGC_Head *gc;
    for (gc = GC_NEXT(gc_list); gc != gc_list; gc = GC_NEXT(gc)) {
        if (generation != -1 && GET_GEN(gc) != generation) {
            continue;
        }
        PyObject *op = FROM_GC(gc);
        if (op != py_list) {
            if (PyList_Append(py_list, op)) {
                return -1; /* exception */
            }
        }
    }
    return 0;
}

/*** end of list stuff ***/


/* For collected generation, set all gc_refs = ob_refcnt.  Set color to
 * white. */
static void
update_refs(gc_state_t *state, int generation)
{
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (GET_GEN(gc) > generation) {
            // FIXME: should not be unneeded?
            if (!IS_BLACK(gc)) {
                SET_COLOR(gc, COLOR_BLACK);
            }
            continue;
        }
        _PyGC_CLEAR_FLAG(gc, GC_FLAG_FINIALIZER_REACHABLE);
        SET_COLOR(gc, COLOR_WHITE);
        gc_set_refs(gc, Py_REFCNT(FROM_GC(gc)));
        /* Python's cyclic gc should never see an incoming refcount
         * of 0:  if something decref'ed to 0, it should have been
         * deallocated immediately at that time.
         * Possible cause (if the assert triggers):  a tp_dealloc
         * routine left a gc-aware object tracked during its teardown
         * phase, and did something-- or allowed something to happen --
         * that called back into Python.  gc can trigger then, and may
         * see the still-tracked dying object.  Before this assert
         * was added, such mistakes went on to allow gc to try to
         * delete the object again.  In a debug build, that caused
         * a mysterious segfault, when _Py_ForgetReference tried
         * to remove the object from the doubly-linked list of all
         * objects a second time.  In a release build, an actual
         * double deallocation occurred, which leads to corruption
         * of the allocator's internal bookkeeping pointers.  That's
         * so serious that maybe this should be a release-build
         * check instead of an assert?
         */
        _PyObject_ASSERT(FROM_GC(gc), gc_get_refs(gc) != 0);
    }
}

/* A traversal callback for subtract_refs. */
static int
visit_decref(PyObject *op, void *data)
{
    assert(op != NULL);
    if (PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        // We're only interested in gc_refs for white objects
        if (gc->_gc_next != 0 && IS_WHITE(gc)) {
            gc_state_t *state = (gc_state_t *)data;
            if (state && state->debug & DEBUG_VERBOSE) {
                long n = gc_get_refs(gc);
                fprintf(stderr, "visit_decref %p %ld->%ld ", FROM_GC(gc), n, n-1);
                dump_obj(FROM_GC(gc));
            }
            gc_decref(gc);
        }
    }
    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
static void
subtract_refs(gc_state_t *state, int generation)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(HEAD(state));
    for (; gc != HEAD(state); gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        if (state && state->debug & DEBUG_VERBOSE) {
            fprintf(stderr, "subtract gen=%d ", GET_GEN(gc));
            dump_obj(FROM_GC(gc));
        }
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        (void) traverse(FROM_GC(gc),
                       (visitproc)visit_decref,
                       (void*)(state));
    }
}

/* A traversal callback for mark_reachable. */
static int
visit_reachable(PyObject *op, struct mark_state *state)
{
    PyGC_Head *gc = AS_GC(op);

    if (!PyObject_IS_GC(op)) {
        return 0;
    }

    // Ignore untracked objects
    if (gc->_gc_next == 0) {
        return 0;
    }

    switch GET_COLOR(gc) {
        case COLOR_WHITE:
            // We thought this object was dead but it turns out to be alive.
            // Mark as grey because we have to traverse it yet.
            if (debug_verbose) {
                fprintf(stderr, "became reachable %p->%p\n", state->ref,
                        FROM_GC(gc));
            }
            SET_COLOR(gc, COLOR_GREY);
            gc_set_refs(gc, 1);
            /* no break, continue to grey case */
            __attribute__ ((fallthrough));
        case COLOR_GREY:
            // Try to traverse all objects reachable from this one
            if (state->queue_depth >= MARK_QUEUE_SIZE) {
                /* we can't recurse further, leave it grey, next loop will
                 * get it */
                state->aborted = 1;
            }
            else {
                /* mark it black as it is proven reachable and we don't have
                 * to look at it anymore.  Note that every loop in
                 * mark_reachable() must at least turn some GREY to BLACK in
                 * order for us to make progress. */
                SET_COLOR(gc, COLOR_BLACK);
                if (state->finalizers) {
                    _PyGC_SET_FLAG(gc, GC_FLAG_FINIALIZER_REACHABLE);
                }
                gc_set_refs(gc, 1);
                state->queue_depth += 1;
                state->ref = FROM_GC(gc);
                traverseproc traverse = Py_TYPE(op)->tp_traverse;
                traverse(op, (visitproc)visit_reachable, state);
                state->queue_depth -= 1;
                assert(state->queue_depth >= 0);
            }
            break;
        case COLOR_BLACK:
            // Object is alive, either not in collected generation or
            // referenced from other alive objects
            break;
        default:
            assert(0); /* invalid GET_COLOR() value */
    }

    return 0;
}

static void
propagate_reachable(gc_state_t *state, int generation, bool finalizers)
{
    PyGC_Head *head = HEAD(state);
    struct mark_state mstate;
    int done = 0;
    int mark_loops = 0;
    mstate.reachable = head;
    mstate.finalizers = finalizers;
    mstate.generation = generation;

    while (!done) {
        mark_loops += 1;
        mstate.aborted = 0;
        mstate.queue_depth = 0;
        /* turn grey to black and all reachable from those also black */
        for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
            if (IS_GREY(gc)) {
                PyObject *op = FROM_GC(gc);
                assert(_PyObject_GC_IS_TRACKED(op));
                visit_reachable(op, &mstate);
            }
        }
        /* if queue depth was exceeded, need another pass */
        done = !mstate.aborted;
    }
#if 0
    if (mark_loops > 0) {
        fprintf(stderr, "mark loops needed %d\n", mark_loops);
    }
#endif
    // done propagate, there must be no grey left at this point
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);
        assert(_PyObject_GC_IS_TRACKED(op));
        if (!IS_WHITE(gc)) {
            assert(IS_BLACK(gc));
        }
    }
}

// Mark all objects that are alive as grey, potential garbage will remain white
static void
mark_reachable(gc_state_t *state, int generation)
{
    PyGC_Head *head = HEAD(state);
    PyGC_Head *gc;
    for (gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        if (gc_get_refs(gc)) {
            /* gc is definitely reachable from outside */
            /* mark grey since we still need to mark all objects reachable from
             * it */
            SET_COLOR(gc, COLOR_GREY);
        }
        else {
            /* assume it is garbage but we may find it is alive when traversing
             * grey objects */
            assert(IS_WHITE(gc));
        }
    }
    propagate_reachable(state, generation, false);
}

// called after collection, put live objects into next generation
static void
increment_generation(gc_state_t *state, int generation)
{
    if (generation < NUM_GENERATIONS - 1) {
        generation +=1;
    }
    //fprintf(stderr, "set gen %d\n", generation);
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc=GC_NEXT(gc)) {
        if (IS_BLACK(gc) && GET_GEN(gc) < generation) {
            SET_GEN(gc, generation);
        }
    }
}

static void
untrack_tuples(gc_state_t *state)
{
    PyGC_Head *head = HEAD(state);
    PyGC_Head *next, *gc = GC_NEXT(head);
    while (gc != head) {
        PyObject *op = FROM_GC(gc);
        next = GC_NEXT(gc);
        if (PyTuple_CheckExact(op)) {
            _PyTuple_MaybeUntrack(op);
        }
        gc = next;
    }
}

/* Try to untrack all currently tracked dictionaries */
static void
untrack_dicts(gc_state_t *state)
{
    PyGC_Head *head = HEAD(state);
    PyGC_Head *next, *gc = GC_NEXT(head);
    while (gc != head) {
        PyObject *op = FROM_GC(gc);
        next = GC_NEXT(gc);
        if (PyDict_CheckExact(op)) {
            _PyDict_MaybeUntrack(op);
        }
        gc = next;
    }
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return op->ob_type->tp_del != NULL;
}

/* Mark unreachable objects that have tp_del slots as grey.  Traverse those
 * objects and mark everything reachable as black and also set the
 * GC_FLAG_FINIALIZER_REACHABLE flag.
 */
static void
move_legacy_finalizers(gc_state_t *state, int generation)
{
    PyGC_Head *head = HEAD(state);
    bool need_propogate = false;

    /* March over unreachable.  Move objects with finalizers into
     * `finalizers`.
     */
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (IS_BLACK(gc)) {
            continue;
        }
        assert(IS_WHITE(gc));
        PyObject *op = FROM_GC(gc);

        if (has_legacy_finalizer(op)) {
            //fprintf(stderr, "move legacy %p\n", op);
            SET_COLOR(gc, COLOR_GREY);
            need_propogate = true;
        }
    }
    if (need_propogate) {
        propagate_reachable(state, generation, true);
    }
}

#if 0
/* A traversal callback for move_legacy_finalizer_reachable. */
static int
visit_move(PyObject *op, PyGC_Head *tolist)
{
    if (PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        if (gc_is_collecting(gc)) {
            gc_list_move(gc, tolist);
            gc_clear_collecting(gc);
        }
    }
    return 0;
}

/* Move objects that are reachable from finalizers, from the unreachable set
 * into finalizers set.
 */
static void
move_legacy_finalizer_reachable(PyGC_Head *finalizers)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(finalizers);
    for (; gc != finalizers; gc = GC_NEXT(gc)) {
        /* Note that the finalizers list may grow during this. */
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        (void) traverse(FROM_GC(gc),
                        (visitproc)visit_move,
                        (void *)finalizers);
    }
}
#endif

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static int
handle_weakrefs(gc_state_t *state)
{
    PyObject *op;               /* generally FROM_GC(gc) */
    PyWeakReference *wr;        /* generally a cast of op */
    PyObject *wrcb_to_call;     /* weakrefs with callbacks to call */
    int num_freed = 0;

    wrcb_to_call = PyList_New(0);
    assert(wrcb_to_call != NULL);
    _PyObject_GC_UNTRACK(wrcb_to_call);

    /* Clear all weakrefs to the objects in unreachable.  If such a weakref
     * also has a callback, move it into `wrcb_to_call` if the callback
     * needs to be invoked.  Note that we cannot invoke any callbacks until
     * all weakrefs to unreachable objects are cleared, lest the callback
     * resurrect an unreachable object via a still-active weakref.  We
     * make another pass over wrcb_to_call, invoking callbacks, after this
     * pass completes.
     */
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        PyWeakReference **wrlist;

        op = FROM_GC(gc);

        if (! PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)))
            continue;

        /* It supports weakrefs.  Does it have any? */
        wrlist = (PyWeakReference **)
                                PyObject_GET_WEAKREFS_LISTPTR(op);

        /* `op` may have some weakrefs.  March over the list, clear
         * all the weakrefs, and move the weakrefs with callbacks
         * that must be called into wrcb_to_call.
         */
        for (wr = *wrlist; wr != NULL; wr = *wrlist) {
            PyGC_Head *wrasgc;                  /* AS_GC(wr) */

            /* _PyWeakref_ClearRef clears the weakref but leaves
             * the callback pointer intact.  Obscure:  it also
             * changes *wrlist.
             */
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == op);
            //fprintf(stderr, "clear weakref %p %p\n", wr, wr->wr_object);
            _PyWeakref_ClearRef(wr);
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == Py_None);
            if (wr->wr_callback == NULL) {
                /* no callback */
                continue;
            }

            /* Headache time.  `op` is going away, and is weakly referenced by
             * `wr`, which has a callback.  Should the callback be invoked?  If wr
             * is also trash, no:
             *
             * 1. There's no need to call it.  The object and the weakref are
             *    both going away, so it's legitimate to pretend the weakref is
             *    going away first.  The user has to ensure a weakref outlives its
             *    referent if they want a guarantee that the wr callback will get
             *    invoked.
             *
             * 2. It may be catastrophic to call it.  If the callback is also in
             *    cyclic trash (CT), then although the CT is unreachable from
             *    outside the current generation, CT may be reachable from the
             *    callback.  Then the callback could resurrect insane objects.
             *
             * Since the callback is never needed and may be unsafe in this case,
             * wr is simply left in the unreachable set.  Note that because we
             * already called _PyWeakref_ClearRef(wr), its callback will never
             * trigger.
             *
             * OTOH, if wr isn't part of CT, we should invoke the callback:  the
             * weakref outlived the trash.  Note that since wr isn't CT in this
             * case, its callback can't be CT either -- wr acted as an external
             * root to this generation, and therefore its callback did too.  So
             * nothing in CT is reachable from the callback either, so it's hard
             * to imagine how calling it later could create a problem for us.  wr
             * is moved to wrcb_to_call in this case.
             */
            wrasgc = AS_GC(wr);
            if (IS_BLACK(wrasgc)) {
                /* Move wr to wrcb_to_call, for the next pass. */
                PyList_Append(wrcb_to_call, (PyObject *)wr);
            }

        }
    }

    /* Invoke the callbacks we decided to honor.  It's safe to invoke them
     * because they can't reference unreachable objects.
     */
    Py_ssize_t len = PyList_GET_SIZE(wrcb_to_call);
    for (Py_ssize_t i = 0; i < len; i++) {
        PyObject *temp;
        PyObject *callback;

        op = PyList_GET_ITEM(wrcb_to_call, i);
        _PyObject_ASSERT(op, PyWeakref_Check(op));
        wr = (PyWeakReference *)op;
        callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        //PySys_WriteStderr("calling wr callback %p\n", callback);
        temp = PyObject_CallFunctionObjArgs(callback, wr, NULL);
        if (temp == NULL)
            PyErr_WriteUnraisable(callback);
        else
            Py_DECREF(temp);

        /* Give up the reference we created in the first pass.  When
         * op's refcount hits 0 (which it may or may not do right now),
         * op's tp_dealloc will decref op->wr_callback too.  Note
         * that the refcount probably will hit 0 now, and because this
         * weakref was reachable to begin with, gc didn't already
         * add it to its count of freed objects.  Example:  a reachable
         * weak value dict maps some key to this reachable weakref.
         * The callback removes this key->weakref mapping from the
         * dict, leaving no other references to the weakref (excepting
         * ours).
         */
        PyList_SetItem(wrcb_to_call, i, Py_None);
#if 0 // FIXME
        if (wrcb_to_call._gc_next == gc) {
            /* object is still alive */
            SET_COLOR(gc, COLOR_BLACK);
        }
        else {
            ++num_freed;
        }
#endif
    }

    return num_freed;
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Handle uncollectable garbage (cycles with tp_del slots, and stuff reachable
 * only from such cycles).
 * If DEBUG_SAVEALL, all objects in finalizers are appended to the module
 * garbage list (a Python list), else only the objects in finalizers with
 * __del__ methods are appended to garbage.  All objects in finalizers are
 * merged into the old list regardless.
 */
static void
handle_legacy_finalizers(struct _gc_runtime_state *state)
{
    assert(!PyErr_Occurred());

    PyGC_Head *gc = GC_NEXT(HEAD(state));
    if (state->garbage == NULL) {
        state->garbage = PyList_New(0);
        if (state->garbage == NULL)
            Py_FatalError("gc couldn't create gc.garbage list");
    }
    for (; gc != HEAD(state); gc = GC_NEXT(gc)) {
        if (!_PyGC_HAVE_FLAG(gc, GC_FLAG_FINIALIZER_REACHABLE)) {
            continue;
        }
        PyObject *op = FROM_GC(gc);

        if ((state->debug & DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            if (PyList_Append(state->garbage, op) < 0) {
                PyErr_Clear();
                break;
            }
        }
    }
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
static bool
finalize_garbage(gc_state_t *state)
{
    bool have_finalizers = false;
    destructor finalize;
    PyObject *garbage = PyList_New(0);
    _PyObject_GC_UNTRACK(garbage);

    /* While we're going through the loop, `finalize(op)` may cause op, or
     * other objects, to be reclaimed via refcounts falling to zero.  So
     * there's little we can rely on about the structure of the input
     * `collectable` list across iterations.  For safety, we always take the
     * first object in that list and move it to a temporary `seen` list.
     * If objects vanish from the `collectable` and `seen` lists we don't
     * care.
     */

    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        PyObject *op = FROM_GC(gc);
        if (!gc_is_finalized(gc) &&
                PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_FINALIZE) &&
                (finalize = Py_TYPE(op)->tp_finalize) != NULL) {
            PyList_Append(garbage, op);
        }
    }
    have_finalizers = PyList_GET_SIZE(garbage) > 0;
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(garbage); i++) {
        PyObject *op = PyList_GET_ITEM(garbage, i);
        if (!gc_is_finalized(AS_GC(op))) {
            _PyGC_SET_FLAG(AS_GC(op), GC_FLAG_FINALIZED);
            if ((finalize = Py_TYPE(op)->tp_finalize) != NULL) {
                finalize(op);
            }
            PyList_SetItem(garbage, i, Py_None);
        }
    }
    Py_DECREF(garbage);
    return have_finalizers;
}

/* Walk the collectable list and check that they are really unreachable
   from the outside (some objects could have been resurrected by a
   finalizer). */
static int
check_garbage(gc_state_t *state, int generation)
{
    int ret = 0;
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        gc_set_refs(gc, Py_REFCNT(FROM_GC(gc)));
        _PyObject_ASSERT(FROM_GC(gc), gc_get_refs(gc) != 0);
    }
    subtract_refs(state, generation);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        _PyObject_ASSERT_WITH_MSG(FROM_GC(gc),
                                  gc_get_refs(gc) >= 0,
                                  "refcount is too small");
        if (gc_get_refs(gc) != 0) {
            PySys_WriteStderr("gc: check_garbage failed: %p\n", FROM_GC(gc));
            ret = -1;
        }
    }
    return ret;
}

static void
revive_garbage(gc_state_t *state)
{
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }
        _PyGC_SET_FLAG(gc, GC_FLAG_REACHABLE);
        SET_COLOR(gc, COLOR_BLACK);
    }
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
static void
delete_garbage(gc_state_t *state, int generation)
{
    assert(!PyErr_Occurred());
    // FIXME: this can go away when we iterate over bitmaps
    PyObject *garbage = PyList_New(0);
    _PyObject_GC_UNTRACK(garbage);

    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!IS_WHITE(gc)) {
            continue;
        }

        PyObject *op = FROM_GC(gc);

        _PyObject_ASSERT_WITH_MSG(op, Py_REFCNT(op) > 0,
                                  "refcount is too small");

        if (state->debug & DEBUG_SAVEALL) {
            assert(state->garbage != NULL);
            if (PyList_Append(state->garbage, op) < 0) {
                PyErr_Clear();
            }
        }
        else {
            PyList_Append(garbage, FROM_GC(gc));
        }
    }

    if (debug_verbose) {
        fprintf(stderr, "delete_garbage %ld\n", PyList_GET_SIZE(garbage));
    }

    for (Py_ssize_t i=0; i<PyList_GET_SIZE(garbage); i++) {
        PyObject *op = PyList_GET_ITEM(garbage, i);
        if (debug_verbose) {
            fprintf(stderr, "call tp_clear %p refcnt=%ld\n", op, op->ob_refcnt);
        }
        if (op->ob_refcnt > 1) {
            inquiry clear;
            if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                (void) clear(op);
                if (PyErr_Occurred()) {
                    PySys_WriteStderr("Exception ignored in tp_clear of "
                                      "%.50s\n", Py_TYPE(op)->tp_name);
                    PyErr_WriteUnraisable(NULL);
                }
            }
        }
        Py_INCREF(Py_None);
        PyList_SetItem(garbage, i, Py_None);
    }
}

/* Clear all free lists
 * All free lists are cleared during the collection of the highest generation.
 * Allocated items in the free list may keep a pymalloc arena occupied.
 * Clearing the free lists may give back memory to the OS earlier.
 */
static void
clear_freelists(void)
{
    (void)PyMethod_ClearFreeList();
    (void)PyFrame_ClearFreeList();
    (void)PyCFunction_ClearFreeList();
    (void)PyTuple_ClearFreeList();
    (void)PyUnicode_ClearFreeList();
    (void)PyFloat_ClearFreeList();
    (void)PyList_ClearFreeList();
    (void)PyDict_ClearFreeList();
    (void)PySet_ClearFreeList();
    (void)PyAsyncGen_ClearFreeLists();
    (void)PyContext_ClearFreeList();
}

void
tally_gen_counts(gc_state_t *state, Py_ssize_t *counts)
{
    PyGC_Head *head = HEAD(state);
    for (PyGC_Head *gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        counts[GET_GEN(gc)]++;
    }
}

/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
collect(gc_state_t *state, int generation,
        Py_ssize_t *n_collected, Py_ssize_t *n_uncollectable, int nofail)
{
    int i;
    Py_ssize_t m = 0; /* # objects collected */
    Py_ssize_t n = 0; /* # unreachable objects that couldn't be collected */
    PyGC_Head *head = HEAD(state); /* head of list of container objects */
    PyGC_Head *gc;
    _PyTime_t t1 = 0;   /* initialize to prevent a compiler warning */

    if (state->debug & DEBUG_STATS) {
        Py_ssize_t counts[NUM_GENERATIONS];
        PySys_WriteStderr("gc: collecting generation %d...\n",
                          generation);
        PySys_WriteStderr("gc: objects in each generation:");
        memset(counts, 0, sizeof(counts));
        tally_gen_counts(state, counts);
        for (i = 0; i < NUM_GENERATIONS; i++)
            PySys_FormatStderr(" %zd", counts[i]);
        PySys_WriteStderr("\ngc: objects in permanent generation: %zd",
                         gc_list_size(&state->permanent_generation.head));
        t1 = _PyTime_GetMonotonicClock();

        PySys_WriteStderr("\n");
    }

    if (PyDTrace_GC_START_ENABLED())
        PyDTrace_GC_START(generation);

    /* update collection and allocation counters */
    if (generation+1 < NUM_GENERATIONS)
        state->generations[generation+1].count += 1;
    for (i = 0; i <= generation; i++)
        state->generations[i].count = 0;

#if 0
    /* merge younger generations with one we are currently collecting */
    for (i = 0; i < generation; i++) {
        gc_list_merge(GEN_HEAD(state, i), GEN_HEAD(state, generation));
    }
#endif

    /* Using ob_refcnt and gc_refs, calculate which objects in the
     * container set are reachable from outside the set (i.e., have a
     * refcount greater than 0 when all the references within the
     * set are taken into account).
     */
    update_refs(state, generation);  // gc_prev is used for gc_refs
    subtract_refs(state, generation);

    /* Leave everything reachable from outside young in young, and move
     * everything else (in young) to unreachable.
     * NOTE:  This used to move the reachable objects into a reachable
     * set instead.  But most things usually turn out to be reachable,
     * so it's more efficient to move the unreachable things.
     */
    mark_reachable(state, generation);

    untrack_tuples(state);
    /* Move reachable objects to next generation. */
    if (generation < NUM_GENERATIONS - 1) {
        if (generation == NUM_GENERATIONS - 2) {
            state->long_lived_pending += gc_list_size(head); // FIXME
        }
    }
    else {
        /* We only untrack dicts in full collections, to avoid quadratic
           dict build-up. See issue #14775. */
        untrack_dicts(state);
        state->long_lived_pending = 0;
        state->long_lived_total = gc_list_size(head); // FIXME
    }
    increment_generation(state, generation);

    /* All objects in unreachable are trash, but objects reachable from
     * legacy finalizers (e.g. tp_del) can't safely be deleted.
     */
    move_legacy_finalizers(state, generation);
#if 0
    /* finalizers contains the unreachable objects with a legacy finalizer;
     * unreachable objects reachable *from* those are also uncollectable,
     * and we move those into the finalizers list too.
     */
    move_legacy_finalizer_reachable(&finalizers);
#endif

    /* Collect statistics on collectable objects found and print
     * debugging information.
     */
    for (gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (IS_WHITE(gc)) {
            m++;
            if (state->debug & DEBUG_COLLECTABLE || 0) {
                debug_cycle("collectable", FROM_GC(gc));
                //dump_obj(FROM_GC(gc));
            }
        }
    }

    /* Clear weakrefs and invoke callbacks as necessary. */
    m += handle_weakrefs(state);

    /* Call tp_finalize on objects which have one. */
    finalize_garbage(state);

    if (check_garbage(state, generation)) {
        fprintf(stderr, "check_garbage -1\n");
        revive_garbage(state);
    }
    else {
        /* Call tp_clear on objects in the unreachable set.  This will cause
         * the reference cycles to be broken.  It may also cause some objects
         * in finalizers to be freed.
         */
        delete_garbage(state, generation);
    }

    /* Collect statistics on uncollectable objects found and print
     * debugging information. */
    for (gc = GC_NEXT(head); gc != head; gc = GC_NEXT(gc)) {
        if (!_PyGC_HAVE_FLAG(gc, GC_FLAG_FINIALIZER_REACHABLE)) {
            continue;
        }
        n++;
        if (state->debug & DEBUG_UNCOLLECTABLE)
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    if (state->debug & DEBUG_STATS) {
        _PyTime_t t2 = _PyTime_GetMonotonicClock();

        if (m == 0 && n == 0)
            PySys_WriteStderr("gc: done");
        else
            PySys_FormatStderr(
                "gc: done, %zd unreachable, %zd uncollectable",
                n+m, n);
        PySys_WriteStderr(", %.4fs elapsed\n",
                          _PyTime_AsSecondsDouble(t2 - t1));
    }

    /* Append instances in the uncollectable set to a Python
     * reachable list of garbage.  The programmer has to deal with
     * this if they insist on creating this type of structure.
     */
    handle_legacy_finalizers(state);

    /* Clear free list only during the collection of the highest
     * generation */
    if (generation == NUM_GENERATIONS-1) {
        clear_freelists();
    }

    if (PyErr_Occurred()) {
        if (nofail) {
            PyErr_Clear();
        }
        else {
            if (gc_str == NULL)
                gc_str = PyUnicode_FromString("garbage collection");
            PyErr_WriteUnraisable(gc_str);
            Py_FatalError("unexpected exception during garbage collection");
        }
    }

    /* Update stats */
    if (n_collected) {
        *n_collected = m;
    }
    if (n_uncollectable) {
        *n_uncollectable = n;
    }

    struct gc_generation_stats *stats = &state->generation_stats[generation];
    stats->collections++;
    stats->collected += m;
    stats->uncollectable += n;

    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(n+m);
    }

    assert(!PyErr_Occurred());
    return n+m;
}

/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
invoke_gc_callback(struct _gc_runtime_state *state, const char *phase,
                   int generation, Py_ssize_t collected,
                   Py_ssize_t uncollectable)
{
    assert(!PyErr_Occurred());

    /* we may get called very early */
    if (state->callbacks == NULL) {
        return;
    }

    /* The local variable cannot be rebound, check it for sanity */
    assert(PyList_CheckExact(state->callbacks));
    PyObject *info = NULL;
    if (PyList_GET_SIZE(state->callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", generation,
            "collected", collected,
            "uncollectable", uncollectable);
        if (info == NULL) {
            PyErr_WriteUnraisable(NULL);
            return;
        }
    }
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(state->callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(state->callbacks, i);
        Py_INCREF(cb); /* make sure cb doesn't go away */
        r = PyObject_CallFunction(cb, "sO", phase, info);
        if (r == NULL) {
            PyErr_WriteUnraisable(cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_XDECREF(info);
    assert(!PyErr_Occurred());
}

/* Perform garbage collection of a generation and invoke
 * progress callbacks.
 */
static Py_ssize_t
collect_with_callback(struct _gc_runtime_state *state, int generation)
{
    assert(!PyErr_Occurred());
    Py_ssize_t result, collected, uncollectable;
    invoke_gc_callback(state, "start", generation, 0, 0);
    result = collect(state, generation, &collected, &uncollectable, 0);
    invoke_gc_callback(state, "stop", generation, collected, uncollectable);
    assert(!PyErr_Occurred());
    return result;
}

static Py_ssize_t
collect_generations(struct _gc_runtime_state *state)
{
    /* Find the oldest generation (highest numbered) where the count
     * exceeds the threshold.  Objects in the that generation and
     * generations younger than it will be collected. */
    Py_ssize_t n = 0;
    for (int i = NUM_GENERATIONS-1; i >= 0; i--) {
        if (state->generations[i].count > state->generations[i].threshold) {
            /* Avoid quadratic performance degradation in number
               of tracked objects. See comments at the beginning
               of this file, and issue #4074.
            */
            if (i == NUM_GENERATIONS - 1
                && state->long_lived_pending < state->long_lived_total / 4)
                continue;
            n = collect_with_callback(state, i);
            break;
        }
    }
    return n;
}

#include "clinic/gcmodule.c.h"

/*[clinic input]
gc.enable

Enable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_enable_impl(PyObject *module)
/*[clinic end generated code: output=45a427e9dce9155c input=81ac4940ca579707]*/
{
    _PyRuntime.gc.enabled = 1;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.disable

Disable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_disable_impl(PyObject *module)
/*[clinic end generated code: output=97d1030f7aa9d279 input=8c2e5a14e800d83b]*/
{
    _PyRuntime.gc.enabled = 0;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.isenabled -> bool

Returns true if automatic garbage collection is enabled.
[clinic start generated code]*/

static int
gc_isenabled_impl(PyObject *module)
/*[clinic end generated code: output=1874298331c49130 input=30005e0422373b31]*/
{
    return _PyRuntime.gc.enabled;
}

/*[clinic input]
gc.collect -> Py_ssize_t

    generation: int(c_default="NUM_GENERATIONS - 1") = 2

Run the garbage collector.

With no arguments, run a full collection.  The optional argument
may be an integer specifying which generation to collect.  A ValueError
is raised if the generation number is invalid.

The number of unreachable objects is returned.
[clinic start generated code]*/

static Py_ssize_t
gc_collect_impl(PyObject *module, int generation)
/*[clinic end generated code: output=b697e633043233c7 input=40720128b682d879]*/
{

    if (generation < 0 || generation >= NUM_GENERATIONS) {
        PyErr_SetString(PyExc_ValueError, "invalid generation");
        return -1;
    }

    struct _gc_runtime_state *state = &_PyRuntime.gc;
    Py_ssize_t n;
    if (state->collecting) {
        /* already collecting, don't do anything */
        n = 0;
    }
    else {
        state->collecting = 1;
        n = collect_with_callback(state, generation);
        state->collecting = 0;
    }
    return n;
}

/*[clinic input]
gc.set_debug

    flags: int
        An integer that can have the following bits turned on:
          DEBUG_STATS - Print statistics during collection.
          DEBUG_COLLECTABLE - Print collectable objects found.
          DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects
            found.
          DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.
          DEBUG_LEAK - Debug leaking programs (everything but STATS).
    /

Set the garbage collection debugging flags.

Debugging information is written to sys.stderr.
[clinic start generated code]*/

static PyObject *
gc_set_debug_impl(PyObject *module, int flags)
/*[clinic end generated code: output=7c8366575486b228 input=5e5ce15e84fbed15]*/
{
    _PyRuntime.gc.debug = flags;
    debug_verbose = flags & DEBUG_VERBOSE;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_debug -> int

Get the garbage collection debugging flags.
[clinic start generated code]*/

static int
gc_get_debug_impl(PyObject *module)
/*[clinic end generated code: output=91242f3506cd1e50 input=91a101e1c3b98366]*/
{
    return _PyRuntime.gc.debug;
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_threshold(PyObject *self, PyObject *args)
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    if (!PyArg_ParseTuple(args, "i|ii:set_threshold",
                          &state->generations[0].threshold,
                          &state->generations[1].threshold,
                          &state->generations[2].threshold))
        return NULL;
    for (int i = 3; i < NUM_GENERATIONS; i++) {
        /* generations higher than 2 get the same threshold */
        state->generations[i].threshold = state->generations[2].threshold;
    }
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_threshold

Return the current collection thresholds.
[clinic start generated code]*/

static PyObject *
gc_get_threshold_impl(PyObject *module)
/*[clinic end generated code: output=7902bc9f41ecbbd8 input=286d79918034d6e6]*/
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    return Py_BuildValue("(iii)",
                         state->generations[0].threshold,
                         state->generations[1].threshold,
                         state->generations[2].threshold);
}

/*[clinic input]
gc.get_count

Return a three-tuple of the current collection counts.
[clinic start generated code]*/

static PyObject *
gc_get_count_impl(PyObject *module)
/*[clinic end generated code: output=354012e67b16398f input=a392794a08251751]*/
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    return Py_BuildValue("(iii)",
                         state->generations[0].count,
                         state->generations[1].count,
                         state->generations[2].count);
}

static int
referrersvisit(PyObject* obj, PyObject *objs)
{
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++)
        if (PyTuple_GET_ITEM(objs, i) == obj)
            return 1;
    return 0;
}

static int
gc_referrers_for(PyObject *objs, PyGC_Head *list, PyObject *resultlist)
{
    PyGC_Head *gc;
    PyObject *obj;
    traverseproc traverse;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        obj = FROM_GC(gc);
        traverse = Py_TYPE(obj)->tp_traverse;
        if (obj == objs || obj == resultlist)
            continue;
        if (traverse(obj, (visitproc)referrersvisit, objs)) {
            if (PyList_Append(resultlist, obj) < 0)
                return 0; /* error */
        }
    }
    return 1; /* no error */
}

PyDoc_STRVAR(gc_get_referrers__doc__,
"get_referrers(*objs) -> list\n\
Return the list of objects that directly refer to any of objs.");

static PyObject *
gc_get_referrers(PyObject *self, PyObject *args)
{
    int i;
    PyObject *result = PyList_New(0);
    if (!result) return NULL;

    struct _gc_runtime_state *state = &_PyRuntime.gc;
    for (i = 0; i < NUM_GENERATIONS; i++) {
        if (!(gc_referrers_for(args, GEN_HEAD(state, i), result))) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/* Append obj to list; return true if error (out of memory), false if OK. */
static int
referentsvisit(PyObject *obj, PyObject *list)
{
    return PyList_Append(list, obj) < 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
    Py_ssize_t i;
    PyObject *result = PyList_New(0);

    if (result == NULL)
        return NULL;

    for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
        traverseproc traverse;
        PyObject *obj = PyTuple_GET_ITEM(args, i);

        if (! PyObject_IS_GC(obj))
            continue;
        traverse = Py_TYPE(obj)->tp_traverse;
        if (! traverse)
            continue;
        if (traverse(obj, (visitproc)referentsvisit, result)) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/*[clinic input]
gc.get_objects
    generation: Py_ssize_t(accept={int, NoneType}, c_default="-1") = None
        Generation to extract the objects from.

Return a list of objects tracked by the collector (excluding the list returned).

If generation is not None, return only the objects tracked by the collector
that are in that generation.
[clinic start generated code]*/

static PyObject *
gc_get_objects_impl(PyObject *module, Py_ssize_t generation)
/*[clinic end generated code: output=48b35fea4ba6cb0e input=ef7da9df9806754c]*/
{
    PyObject* result;
    struct _gc_runtime_state *state = &_PyRuntime.gc;

    result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    /* If generation is passed, we extract only that generation */
    if (generation != -1) {
        if (generation >= NUM_GENERATIONS) {
            PyErr_Format(PyExc_ValueError,
                         "generation parameter must be less than the number of "
                         "available generations (%i)",
                          NUM_GENERATIONS);
            goto error;
        }

        if (generation < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "generation parameter cannot be negative");
            goto error;
        }
    }
    if (append_objects(result, GEN_HEAD(state, 0), generation)) {

        goto error;
    }
    return result;

error:
    Py_DECREF(result);
    return NULL;
}

/*[clinic input]
gc.get_stats

Return a list of dictionaries containing per-generation statistics.
[clinic start generated code]*/

static PyObject *
gc_get_stats_impl(PyObject *module)
/*[clinic end generated code: output=a8ab1d8a5d26f3ab input=1ef4ed9d17b1a470]*/
{
    int i;
    struct gc_generation_stats stats[NUM_GENERATIONS], *st;

    /* To get consistent values despite allocations while constructing
       the result list, we use a snapshot of the running stats. */
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    for (i = 0; i < NUM_GENERATIONS; i++) {
        stats[i] = state->generation_stats[i];
    }

    PyObject *result = PyList_New(0);
    if (result == NULL)
        return NULL;

    for (i = 0; i < NUM_GENERATIONS; i++) {
        PyObject *dict;
        st = &stats[i];
        dict = Py_BuildValue("{snsnsn}",
                             "collections", st->collections,
                             "collected", st->collected,
                             "uncollectable", st->uncollectable
                            );
        if (dict == NULL)
            goto error;
        if (PyList_Append(result, dict)) {
            Py_DECREF(dict);
            goto error;
        }
        Py_DECREF(dict);
    }
    return result;

error:
    Py_XDECREF(result);
    return NULL;
}


/*[clinic input]
gc.is_tracked

    obj: object
    /

Returns true if the object is tracked by the garbage collector.

Simple atomic objects will return false.
[clinic start generated code]*/

static PyObject *
gc_is_tracked(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=14f0103423b28e31 input=d83057f170ea2723]*/
{
    PyObject *result;

    if (PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj))
        result = Py_True;
    else
        result = Py_False;
    Py_INCREF(result);
    return result;
}

/*[clinic input]
gc.freeze

Freeze all current tracked objects and ignore them for future collections.

This can be used before a POSIX fork() call to make the gc copy-on-write friendly.
Note: collection before a POSIX fork() call may free pages for future allocation
which can cause copy-on-write.
[clinic start generated code]*/

static PyObject *
gc_freeze_impl(PyObject *module)
/*[clinic end generated code: output=502159d9cdc4c139 input=b602b16ac5febbe5]*/
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    for (int i = 0; i < NUM_GENERATIONS; ++i) {
        gc_list_merge(GEN_HEAD(state, i), &state->permanent_generation.head);
        state->generations[i].count = 0;
    }
    Py_RETURN_NONE;
}

/*[clinic input]
gc.unfreeze

Unfreeze all objects in the permanent generation.

Put all objects in the permanent generation back into oldest generation.
[clinic start generated code]*/

static PyObject *
gc_unfreeze_impl(PyObject *module)
/*[clinic end generated code: output=1c15f2043b25e169 input=2dd52b170f4cef6c]*/
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    gc_list_merge(&state->permanent_generation.head, GEN_HEAD(state, NUM_GENERATIONS-1));
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_freeze_count -> Py_ssize_t

Return the number of objects in the permanent generation.
[clinic start generated code]*/

static Py_ssize_t
gc_get_freeze_count_impl(PyObject *module)
/*[clinic end generated code: output=61cbd9f43aa032e1 input=45ffbc65cfe2a6ed]*/
{
    return gc_list_size(&_PyRuntime.gc.permanent_generation.head);
}


PyDoc_STRVAR(gc__doc__,
"This module provides access to the garbage collector for reference cycles.\n"
"\n"
"enable() -- Enable automatic garbage collection.\n"
"disable() -- Disable automatic garbage collection.\n"
"isenabled() -- Returns true if automatic collection is enabled.\n"
"collect() -- Do a full collection right now.\n"
"get_count() -- Return the current collection counts.\n"
"get_stats() -- Return list of dictionaries containing per-generation stats.\n"
"set_debug() -- Set debugging flags.\n"
"get_debug() -- Get debugging flags.\n"
"set_threshold() -- Set the collection thresholds.\n"
"get_threshold() -- Return the current the collection thresholds.\n"
"get_objects() -- Return a list of all objects tracked by the collector.\n"
"is_tracked() -- Returns true if a given object is tracked.\n"
"get_referrers() -- Return the list of objects that refer to an object.\n"
"get_referents() -- Return the list of objects that an object refers to.\n"
"freeze() -- Freeze all tracked objects and ignore them for future collections.\n"
"unfreeze() -- Unfreeze all objects in the permanent generation.\n"
"get_freeze_count() -- Return the number of objects in the permanent generation.\n");

static PyMethodDef GcMethods[] = {
    GC_ENABLE_METHODDEF
    GC_DISABLE_METHODDEF
    GC_ISENABLED_METHODDEF
    GC_SET_DEBUG_METHODDEF
    GC_GET_DEBUG_METHODDEF
    GC_GET_COUNT_METHODDEF
    {"set_threshold",  gc_set_threshold, METH_VARARGS, gc_set_thresh__doc__},
    GC_GET_THRESHOLD_METHODDEF
    GC_COLLECT_METHODDEF
    GC_GET_OBJECTS_METHODDEF
    GC_GET_STATS_METHODDEF
    GC_IS_TRACKED_METHODDEF
    {"get_referrers",  gc_get_referrers, METH_VARARGS,
        gc_get_referrers__doc__},
    {"get_referents",  gc_get_referents, METH_VARARGS,
        gc_get_referents__doc__},
    GC_FREEZE_METHODDEF
    GC_UNFREEZE_METHODDEF
    GC_GET_FREEZE_COUNT_METHODDEF
    {NULL,      NULL}           /* Sentinel */
};

static struct PyModuleDef gcmodule = {
    PyModuleDef_HEAD_INIT,
    "gc",              /* m_name */
    gc__doc__,         /* m_doc */
    -1,                /* m_size */
    GcMethods,         /* m_methods */
    NULL,              /* m_reload */
    NULL,              /* m_traverse */
    NULL,              /* m_clear */
    NULL               /* m_free */
};

PyMODINIT_FUNC
PyInit_gc(void)
{
    PyObject *m;

    m = PyModule_Create(&gcmodule);

    if (m == NULL) {
        return NULL;
    }

    struct _gc_runtime_state *state = &_PyRuntime.gc;
    if (state->garbage == NULL) {
        state->garbage = PyList_New(0);
        if (state->garbage == NULL)
            return NULL;
    }
    Py_INCREF(state->garbage);
    if (PyModule_AddObject(m, "garbage", state->garbage) < 0)
        return NULL;

    if (state->callbacks == NULL) {
        state->callbacks = PyList_New(0);
        if (state->callbacks == NULL)
            return NULL;
    }
    Py_INCREF(state->callbacks);
    if (PyModule_AddObject(m, "callbacks", state->callbacks) < 0)
        return NULL;

#define ADD_INT(NAME) if (PyModule_AddIntConstant(m, #NAME, NAME) < 0) return NULL
    ADD_INT(DEBUG_STATS);
    ADD_INT(DEBUG_COLLECTABLE);
    ADD_INT(DEBUG_UNCOLLECTABLE);
    ADD_INT(DEBUG_SAVEALL);
    ADD_INT(DEBUG_LEAK);
    ADD_INT(DEBUG_VERBOSE);
#undef ADD_INT
    return m;
}

/* API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    if (!state->enabled) {
        return 0;
    }

    Py_ssize_t n;
    if (state->collecting) {
        /* already collecting, don't do anything */
        n = 0;
    }
    else {
        PyObject *exc, *value, *tb;
        state->collecting = 1;
        PyErr_Fetch(&exc, &value, &tb);
        n = collect_with_callback(state, NUM_GENERATIONS - 1);
        PyErr_Restore(exc, value, tb);
        state->collecting = 0;
    }

    return n;
}

Py_ssize_t
_PyGC_CollectIfEnabled(void)
{
    return PyGC_Collect();
}

Py_ssize_t
_PyGC_CollectNoFail(void)
{
    assert(!PyErr_Occurred());

    struct _gc_runtime_state *state = &_PyRuntime.gc;
    Py_ssize_t n;

    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    if (state->collecting) {
        n = 0;
    }
    else {
        state->collecting = 1;
        n = collect(state, NUM_GENERATIONS - 1, NULL, NULL, 1);
        state->collecting = 0;
    }
    return n;
}

void
_PyGC_DumpShutdownStats(_PyRuntimeState *runtime)
{
    struct _gc_runtime_state *state = &runtime->gc;
    if (!(state->debug & DEBUG_SAVEALL)
        && state->garbage != NULL && PyList_GET_SIZE(state->garbage) > 0) {
        const char *message;
        if (state->debug & DEBUG_UNCOLLECTABLE)
            message = "gc: %zd uncollectable objects at " \
                "shutdown";
        else
            message = "gc: %zd uncollectable objects at " \
                "shutdown; use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(state->garbage)))
            PyErr_WriteUnraisable(NULL);
        if (state->debug & DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(state->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr)))
                PyErr_WriteUnraisable(state->garbage);
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}

void
_PyGC_Fini(_PyRuntimeState *runtime)
{
    struct _gc_runtime_state *state = &runtime->gc;
    Py_CLEAR(state->garbage);
    Py_CLEAR(state->callbacks);
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
    _PyObject_Dump(FROM_GC(g));
}

/* extension modules might be compiled with GC support so these
   functions must always be available */

void
PyObject_GC_Track(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_ASSERT_FAILED_MSG(op,
                                    "object already tracked "
                                    "by the garbage collector");
    }
    _PyObject_GC_TRACK(op);
}

void
PyObject_GC_UnTrack(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    /* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
     * call PyObject_GC_UnTrack twice on an object.
     */
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
}

static PyObject *
_PyObject_GC_Alloc(int use_calloc, size_t basicsize)
{
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    PyObject *op;
    PyGC_Head *g;
    size_t size;
    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head))
        return PyErr_NoMemory();
    size = sizeof(PyGC_Head) + basicsize;
    if (use_calloc)
        g = (PyGC_Head *)PyObject_Calloc(1, size);
    else
        g = (PyGC_Head *)PyObject_Malloc(size);
    if (g == NULL)
        return PyErr_NoMemory();
    assert((((uintptr_t)g) & 3) == 0);  // g must be aligned 4bytes boundary
    g->_gc_next = 0;
    g->_gc_prev = 0;
    SET_COLOR(g, COLOR_BLACK);
    SET_GEN(g, 0);
    state->generations[0].count++; /* number of allocated GC objects */
    if (state->generations[0].count > state->generations[0].threshold &&
        state->enabled &&
        state->generations[0].threshold &&
        !state->collecting &&
        !PyErr_Occurred()) {
        state->collecting = 1;
        collect_generations(state);
        state->collecting = 0;
    }
    op = FROM_GC(g);
    return op;
}

PyObject *
_PyObject_GC_Malloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(0, basicsize);
}

PyObject *
_PyObject_GC_Calloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(1, basicsize);
}

PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    PyObject *op = _PyObject_GC_Malloc(_PyObject_SIZE(tp));
    if (op != NULL)
        op = PyObject_INIT(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    size_t size;
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *) _PyObject_GC_Malloc(size);
    if (op != NULL)
        op = PyObject_INIT_VAR(op, tp, nitems);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head)) {
        return (PyVarObject *)PyErr_NoMemory();
    }

    PyGC_Head *g = AS_GC(op);
    g = (PyGC_Head *)PyObject_REALLOC(g,  sizeof(PyGC_Head) + basicsize);
    if (g == NULL)
        return (PyVarObject *)PyErr_NoMemory();
    op = (PyVarObject *) FROM_GC(g);
    Py_SIZE(op) = nitems;
    return op;
}

void
PyObject_GC_Del(void *op)
{
    PyGC_Head *g = AS_GC(op);
    if (_PyObject_GC_IS_TRACKED(op)) {
        gc_list_remove(g);
    }
    struct _gc_runtime_state *state = &_PyRuntime.gc;
    if (state->generations[0].count > 0) {
        state->generations[0].count--;
    }
    PyObject_FREE(g);
}
