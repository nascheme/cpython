// Generational GC specific code, forward-ported from CPython 3.13.
// This file is included by gc.c when Py_GC_INCREMENTAL is not defined.
// Do not compile or include independently.


#define GEN_HEAD(gcstate, n) (&(gcstate)->generations[n].head)


void
_PyGC_InitState(GCState *gcstate)
{
#define INIT_HEAD(GEN) \
    do { \
        GEN.head._gc_next = (uintptr_t)&GEN.head; \
        GEN.head._gc_prev = (uintptr_t)&GEN.head; \
    } while (0)

    for (int i = 0; i < NUM_GENERATIONS; i++) {
        assert(gcstate->generations[i].count == 0);
        INIT_HEAD(gcstate->generations[i]);
    };
    gcstate->generation0 = GEN_HEAD(gcstate, 0);
    INIT_HEAD(gcstate->permanent_generation);

#undef INIT_HEAD
}


PyStatus
_PyGC_Init(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;

    gcstate->garbage = PyList_New(0);
    if (gcstate->garbage == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->callbacks = PyList_New(0);
    if (gcstate->callbacks == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}



#ifdef GC_DEBUG
static void
validate_list(PyGC_Head *head, enum flagstates flags)
{
    assert((head->_gc_prev & ~_PyGC_PREV_MASK) == 0);
    assert((head->_gc_next & ~_PyGC_PREV_MASK) == 0);
    uintptr_t prev_value = 0, next_value = 0;
    switch (flags) {
        case collecting_clear_unreachable_clear:
            break;
        case collecting_set_unreachable_clear:
            prev_value = PREV_MASK_COLLECTING;
            break;
        case collecting_clear_unreachable_set:
            next_value = NEXT_MASK_UNREACHABLE;
            break;
        case collecting_set_unreachable_set:
            prev_value = PREV_MASK_COLLECTING;
            next_value = NEXT_MASK_UNREACHABLE;
            break;
        default:
            assert(! "bad internal flags argument");
    }
    PyGC_Head *prev = head;
    PyGC_Head *gc = GC_NEXT(head);
    while (gc != head) {
        PyGC_Head *trueprev = GC_PREV(gc);
        PyGC_Head *truenext = (PyGC_Head *)(gc->_gc_next  & ~NEXT_MASK_UNREACHABLE);
        assert(truenext != NULL);
        assert(trueprev == prev);
        assert((gc->_gc_prev & PREV_MASK_COLLECTING) == prev_value);
        assert((gc->_gc_next & NEXT_MASK_UNREACHABLE) == next_value);
        prev = gc;
        gc = truenext;
    }
    assert(prev == GC_PREV(head));
}
#else
#define validate_list(x, y) do{}while(0)
#endif

/*** end of list stuff ***/


/* Set all gc_refs = ob_refcnt.  After this, gc_refs is > 0 and
 * PREV_MASK_COLLECTING bit is set for all objects in containers.
 */
static void
update_refs(PyGC_Head *containers)
{
    PyGC_Head *next;
    PyGC_Head *gc = GC_NEXT(containers);

    while (gc != containers) {
        next = GC_NEXT(gc);
        PyObject *op = FROM_GC(gc);
        if (_Py_IsImmortal(op)) {
            assert(!_Py_IsStaticImmortal(op));
            _PyObject_GC_UNTRACK(op);
           gc = next;
           continue;
        }
        gc_reset_refs(gc, Py_REFCNT(op));
        _PyObject_ASSERT(op, gc_get_refs(gc) != 0);
        gc = next;
    }
}

/* A traversal callback for subtract_refs. */
static int
visit_reachable(PyObject *op, void *arg)
{
    PyGC_Head *reachable = arg;
    OBJECT_STAT_INC(object_visits);
    if (!_PyObject_IS_GC(op)) {
        return 0;
    }

    PyGC_Head *gc = AS_GC(op);
    const Py_ssize_t gc_refs = gc_get_refs(gc);

    if (! gc_is_collecting(gc)) {
        return 0;
    }
    _PyObject_ASSERT(op, gc->_gc_next != 0);

    if (gc->_gc_next & NEXT_MASK_UNREACHABLE) {
        PyGC_Head *prev = GC_PREV(gc);
        PyGC_Head *next = (PyGC_Head*)(gc->_gc_next & ~NEXT_MASK_UNREACHABLE);
        _PyObject_ASSERT(FROM_GC(prev),
                         prev->_gc_next & NEXT_MASK_UNREACHABLE);
        _PyObject_ASSERT(FROM_GC(next),
                         next->_gc_next & NEXT_MASK_UNREACHABLE);
        prev->_gc_next = gc->_gc_next;  // copy NEXT_MASK_UNREACHABLE
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        _PyGCHead_SET_PREV(next, prev);

        gc_list_append(gc, reachable);
        gc_set_refs(gc, 1);
    }
    else if (gc_refs == 0) {
        gc_set_refs(gc, 1);
    }
    else {
        _PyObject_ASSERT_WITH_MSG(op, gc_refs > 0, "refcount is too small");
    }
    return 0;
}

static void
move_unreachable(PyGC_Head *young, PyGC_Head *unreachable)
{
    PyGC_Head *prev = young;
    PyGC_Head *gc = GC_NEXT(young);

    while (gc != young) {
        if (gc_get_refs(gc)) {
            PyObject *op = FROM_GC(gc);
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(gc) > 0,
                                      "refcount is too small");
            (void) traverse(op,
                    visit_reachable,
                    (void *)young);
            _PyGCHead_SET_PREV(gc, prev);
            gc_clear_collecting(gc);
            prev = gc;
        }
        else {
            prev->_gc_next = gc->_gc_next;

            PyGC_Head *last = GC_PREV(unreachable);
            last->_gc_next = (NEXT_MASK_UNREACHABLE | (uintptr_t)gc);
            _PyGCHead_SET_PREV(gc, last);
            gc->_gc_next = (NEXT_MASK_UNREACHABLE | (uintptr_t)unreachable);
            unreachable->_gc_prev = (uintptr_t)gc;
        }
        gc = (PyGC_Head*)prev->_gc_next;
    }
    young->_gc_prev = (uintptr_t)prev;
    unreachable->_gc_next &= ~NEXT_MASK_UNREACHABLE;
}

static void
move_legacy_finalizers(PyGC_Head *unreachable, PyGC_Head *finalizers)
{
    PyGC_Head *gc, *next;
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        (unreachable->_gc_next & NEXT_MASK_UNREACHABLE) == 0);

    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        PyObject *op = FROM_GC(gc);

        _PyObject_ASSERT(op, gc->_gc_next & NEXT_MASK_UNREACHABLE);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        next = (PyGC_Head*)gc->_gc_next;

        if (has_legacy_finalizer(op)) {
            gc_clear_collecting(gc);
            gc_list_move(gc, finalizers);
        }
    }
}

static inline void
clear_unreachable_mask(PyGC_Head *unreachable)
{
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        ((uintptr_t)unreachable & NEXT_MASK_UNREACHABLE) == 0);
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        (unreachable->_gc_next & NEXT_MASK_UNREACHABLE) == 0);

    PyGC_Head *gc, *next;
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        _PyObject_ASSERT((PyObject*)FROM_GC(gc), gc->_gc_next & NEXT_MASK_UNREACHABLE);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        next = (PyGC_Head*)gc->_gc_next;
    }
    validate_list(unreachable, collecting_set_unreachable_clear);
}

/* A traversal callback for move_legacy_finalizer_reachable. */
static int
handle_weakrefs(PyGC_Head *unreachable, PyGC_Head *old)
{
    PyGC_Head *gc;
    PyObject *op;
    PyWeakReference *wr;
    PyGC_Head wrcb_to_call;
    PyGC_Head *next;
    int num_freed = 0;

    gc_list_init(&wrcb_to_call);

    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        PyWeakReference **wrlist;

        op = FROM_GC(gc);
        next = GC_NEXT(gc);

        if (PyWeakref_Check(op)) {
            _PyWeakref_ClearRef((PyWeakReference *)op);
        }

        if (! _PyType_SUPPORTS_WEAKREFS(Py_TYPE(op))) {
            continue;
        }

        wrlist = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(op);

        for (wr = *wrlist; wr != NULL; wr = *wrlist) {
            PyGC_Head *wrasgc;

            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == op);
            _PyWeakref_ClearRef(wr);
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == Py_None);
            if (wr->wr_callback == NULL) {
                continue;
            }

            if (gc_is_collecting(AS_GC((PyObject *)wr))) {
                _PyObject_ASSERT((PyObject*)wr, wr->wr_object == Py_None);
                continue;
            }

            Py_INCREF(wr);

            wrasgc = AS_GC((PyObject *)wr);
            _PyObject_ASSERT((PyObject *)wr, wrasgc != next);
            gc_list_move(wrasgc, &wrcb_to_call);
        }
    }

    while (! gc_list_is_empty(&wrcb_to_call)) {
        PyObject *temp;
        PyObject *callback;

        gc = (PyGC_Head*)wrcb_to_call._gc_next;
        op = FROM_GC(gc);
        _PyObject_ASSERT(op, PyWeakref_Check(op));
        wr = (PyWeakReference *)op;
        callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        temp = PyObject_CallOneArg(callback, (PyObject *)wr);
        if (temp == NULL) {
            PyErr_FormatUnraisable("Exception ignored on "
                                   "calling weakref callback %R", callback);
        }
        else {
            Py_DECREF(temp);
        }

        Py_DECREF(op);
        if (wrcb_to_call._gc_next == (uintptr_t)gc) {
            gc_list_move(gc, old);
        }
        else {
            ++num_freed;
        }
    }

    return num_freed;
}

static void
show_stats_each_generations(GCState *gcstate)
{
    char buf[100];
    size_t pos = 0;

    for (int i = 0; i < NUM_GENERATIONS && pos < sizeof(buf); i++) {
        pos += PyOS_snprintf(buf+pos, sizeof(buf)-pos,
                             " %zd",
                             gc_list_size(GEN_HEAD(gcstate, i)));
    }

    PySys_FormatStderr(
        "gc: objects in each generation:%s\n"
        "gc: objects in permanent generation: %zd\n",
        buf, gc_list_size(&gcstate->permanent_generation.head));
}

static inline void
deduce_unreachable(PyGC_Head *base, PyGC_Head *unreachable) {
    validate_list(base, collecting_clear_unreachable_clear);
    update_refs(base);  // gc_prev is used for gc_refs
    subtract_refs(base);
    gc_list_init(unreachable);
    move_unreachable(base, unreachable);  // gc_prev is pointer again
    validate_list(base, collecting_clear_unreachable_clear);
    validate_list(unreachable, collecting_set_unreachable_set);
}

static inline void
handle_resurrected_objects(PyGC_Head *unreachable, PyGC_Head* still_unreachable,
                           PyGC_Head *old_generation)
{
    gc_list_clear_collecting(unreachable);

    PyGC_Head* resurrected = unreachable;
    deduce_unreachable(resurrected, still_unreachable);
    clear_unreachable_mask(still_unreachable);

    gc_list_merge(resurrected, old_generation);
}


static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   int generation, Py_ssize_t collected,
                   Py_ssize_t uncollectable)
{
    assert(!_PyErr_Occurred(tstate));

    GCState *gcstate = &tstate->interp->gc;
    if (gcstate->callbacks == NULL) {
        return;
    }

    assert(PyList_CheckExact(gcstate->callbacks));
    PyObject *info = NULL;
    if (PyList_GET_SIZE(gcstate->callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", generation,
            "collected", collected,
            "uncollectable", uncollectable);
        if (info == NULL) {
            PyErr_FormatUnraisable("Exception ignored while invoking gc callbacks");
            return;
        }
    }

    PyObject *phase_obj = PyUnicode_FromString(phase);
    if (phase_obj == NULL) {
        Py_XDECREF(info);
        PyErr_FormatUnraisable("Exception ignored while invoking gc callbacks");
        return;
    }

    PyObject *stack[] = {phase_obj, info};
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(gcstate->callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(gcstate->callbacks, i);
        Py_INCREF(cb);
        r = PyObject_Vectorcall(cb, stack, 2, NULL);
        if (r == NULL) {
            PyErr_FormatUnraisable("Exception ignored while "
                                   "calling GC callback %R", cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_DECREF(phase_obj);
    Py_XDECREF(info);
    assert(!_PyErr_Occurred(tstate));
}


/* Find the oldest generation where the count exceeds the threshold. */
static int
gc_select_generation(GCState *gcstate)
{
    for (int i = NUM_GENERATIONS-1; i >= 0; i--) {
        if (gcstate->generations[i].count > gcstate->generations[i].threshold) {
            if (i == NUM_GENERATIONS - 1
                && gcstate->long_lived_pending < gcstate->long_lived_total / 4)
            {
                continue;
            }
            return i;
        }
    }
    return -1;
}


/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
gc_collect_main(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    int i;
    Py_ssize_t m = 0; /* # objects collected */
    Py_ssize_t n = 0; /* # unreachable objects that couldn't be collected */
    PyGC_Head *young; /* the generation we are examining */
    PyGC_Head *old; /* next older generation */
    PyGC_Head unreachable; /* non-problematic unreachable trash */
    PyGC_Head finalizers;  /* objects with, & reachable from, __del__ */
    PyGC_Head *gc;
    PyTime_t t1 = 0;   /* initialize to prevent a compiler warning */
    GCState *gcstate = &tstate->interp->gc;

    // gc_collect_main() must not be called before _PyGC_Init
    // or after _PyGC_Fini()
    assert(gcstate->garbage != NULL);
    assert(!_PyErr_Occurred(tstate));

    int expected = 0;
    if (!_Py_atomic_compare_exchange_int(&gcstate->collecting, &expected, 1)) {
        return 0;
    }

    if (generation == GENERATION_AUTO) {
        generation = gc_select_generation(gcstate);
        if (generation < 0) {
            _Py_atomic_store_int(&gcstate->collecting, 0);
            return 0;
        }
    }

    assert(generation >= 0 && generation < NUM_GENERATIONS);

#ifdef Py_STATS
    if (_Py_stats) {
        _Py_stats->object_stats.object_visits = 0;
    }
#endif
    GC_STAT_ADD(generation, collections, 1);

    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "start", generation, 0, 0);
    }

    if (gcstate->debug & _PyGC_DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting generation %d...\n", generation);
        show_stats_each_generations(gcstate);
        (void)PyTime_PerfCounterRaw(&t1);
    }

    if (PyDTrace_GC_START_ENABLED()) {
        PyDTrace_GC_START(generation);
    }

    /* update collection and allocation counters */
    if (generation+1 < NUM_GENERATIONS) {
        gcstate->generations[generation+1].count += 1;
    }
    for (i = 0; i <= generation; i++) {
        gcstate->generations[i].count = 0;
    }

    /* merge younger generations with one we are currently collecting */
    for (i = 0; i < generation; i++) {
        gc_list_merge(GEN_HEAD(gcstate, i), GEN_HEAD(gcstate, generation));
    }

    /* handy references */
    young = GEN_HEAD(gcstate, generation);
    if (generation < NUM_GENERATIONS-1) {
        old = GEN_HEAD(gcstate, generation+1);
    }
    else {
        old = young;
    }
    validate_list(old, collecting_clear_unreachable_clear);

    deduce_unreachable(young, &unreachable);

    untrack_tuples(young);
    /* Move reachable objects to next generation. */
    if (young != old) {
        if (generation == NUM_GENERATIONS - 2) {
            gcstate->long_lived_pending += gc_list_size(young);
        }
        gc_list_merge(young, old);
    }
    else {
        /* We only un-track dicts in full collections, to avoid quadratic
           dict build-up. See issue #14775.
           Note: _PyDict_MaybeUntrack was removed in 3.14, so dict
           untracking during GC is no longer done. */
        gcstate->long_lived_pending = 0;
        gcstate->long_lived_total = gc_list_size(young);
    }

    /* All objects in unreachable are trash, but objects reachable from
     * legacy finalizers (e.g. tp_del) can't safely be deleted.
     */
    gc_list_init(&finalizers);
    // NEXT_MASK_UNREACHABLE is cleared here.
    move_legacy_finalizers(&unreachable, &finalizers);
    move_legacy_finalizer_reachable(&finalizers);

    validate_list(&finalizers, collecting_clear_unreachable_clear);
    validate_list(&unreachable, collecting_set_unreachable_clear);

    /* Print debugging information. */
    if (gcstate->debug & _PyGC_DEBUG_COLLECTABLE) {
        for (gc = GC_NEXT(&unreachable); gc != &unreachable; gc = GC_NEXT(gc)) {
            debug_cycle("collectable", FROM_GC(gc));
        }
    }

    /* Clear weakrefs and invoke callbacks as necessary. */
    m += handle_weakrefs(&unreachable, old);

    validate_list(old, collecting_clear_unreachable_clear);
    validate_list(&unreachable, collecting_set_unreachable_clear);

    /* Call tp_finalize on objects which have one. */
    finalize_garbage(tstate, &unreachable);

    /* Handle any objects that may have resurrected after the call
     * to 'finalize_garbage' and continue the collection with the
     * objects that are still unreachable */
    PyGC_Head final_unreachable;
    handle_resurrected_objects(&unreachable, &final_unreachable, old);

    m += gc_list_size(&final_unreachable);
    delete_garbage(tstate, gcstate, &final_unreachable, old);

    /* Collect statistics on uncollectable objects found and print
     * debugging information. */
    for (gc = GC_NEXT(&finalizers); gc != &finalizers; gc = GC_NEXT(gc)) {
        n++;
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE)
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    if (gcstate->debug & _PyGC_DEBUG_STATS) {
        PyTime_t t2;
        (void)PyTime_PerfCounterRaw(&t2);
        double d = PyTime_AsSecondsDouble(t2 - t1);
        PySys_WriteStderr(
            "gc: done, %zd unreachable, %zd uncollectable, %.4fs elapsed\n",
            n+m, n, d);
    }

    handle_legacy_finalizers(tstate, gcstate, &finalizers, old);
    validate_list(old, collecting_clear_unreachable_clear);

    /* Clear free list only during the collection of the highest
     * generation */
    if (generation == NUM_GENERATIONS-1) {
        _PyGC_ClearAllFreeLists(tstate->interp);
    }

    if (_PyErr_Occurred(tstate)) {
        if (reason == _Py_GC_REASON_SHUTDOWN) {
            _PyErr_Clear(tstate);
        }
        else {
            PyErr_FormatUnraisable("Exception ignored in garbage collection");
        }
    }

    /* Update stats */
    struct gc_generation_stats *stats = &gcstate->generation_stats[generation];
    stats->collections++;
    stats->collected += m;
    stats->uncollectable += n;

    GC_STAT_ADD(generation, objects_collected, m);
#ifdef Py_STATS
    if (_Py_stats) {
        GC_STAT_ADD(generation, object_visits,
            _Py_stats->object_stats.object_visits);
        _Py_stats->object_stats.object_visits = 0;
    }
#endif

    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(n + m);
    }

    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "stop", generation, m, n);
    }

    assert(!_PyErr_Occurred(tstate));
    _Py_atomic_store_int(&gcstate->collecting, 0);
    return n + m;
}

PyObject *
_PyGC_GetReferrers(PyInterpreterState *interp, PyObject *objs)
{
    PyObject *result = PyList_New(0);
    if (!result) {
        return NULL;
    }

    GCState *gcstate = &interp->gc;
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        if (!(gc_referrers_for(objs, GEN_HEAD(gcstate, i), result))) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

PyObject *
_PyGC_GetObjects(PyInterpreterState *interp, int generation)
{
    assert(generation >= -1 && generation < NUM_GENERATIONS);
    GCState *gcstate = &interp->gc;

    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    if (generation == -1) {
        for (int i = 0; i < NUM_GENERATIONS; i++) {
            if (append_objects(result, GEN_HEAD(gcstate, i))) {
                goto error;
            }
        }
    }
    else {
        if (append_objects(result, GEN_HEAD(gcstate, generation))) {
            goto error;
        }
    }

    return result;
error:
    Py_DECREF(result);
    return NULL;
}

void
_PyGC_Freeze(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    for (int i = 0; i < NUM_GENERATIONS; ++i) {
        gc_list_merge(GEN_HEAD(gcstate, i), &gcstate->permanent_generation.head);
        gcstate->generations[i].count = 0;
    }
}

void
_PyGC_Unfreeze(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    gc_list_merge(&gcstate->permanent_generation.head,
                  GEN_HEAD(gcstate, NUM_GENERATIONS-1));
}

Py_ssize_t
PyGC_Collect(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;

    if (!gcstate->enabled) {
        return 0;
    }

    Py_ssize_t n;
    PyObject *exc = _PyErr_GetRaisedException(tstate);
    n = gc_collect_main(tstate, NUM_GENERATIONS - 1, _Py_GC_REASON_MANUAL);
    _PyErr_SetRaisedException(tstate, exc);

    return n;
}

Py_ssize_t
_PyGC_Collect(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    return gc_collect_main(tstate, generation, reason);
}

void
_PyGC_CollectNoFail(PyThreadState *tstate)
{
    gc_collect_main(tstate, NUM_GENERATIONS - 1, _Py_GC_REASON_SHUTDOWN);
}

void
_PyGC_DumpShutdownStats(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    if (!(gcstate->debug & _PyGC_DEBUG_SAVEALL)
        && gcstate->garbage != NULL && PyList_GET_SIZE(gcstate->garbage) > 0) {
        const char *message;
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            message = "gc: %zd uncollectable objects at shutdown";
        }
        else {
            message = "gc: %zd uncollectable objects at shutdown; " \
                "use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        }
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(gcstate->garbage)))
        {
            PyErr_FormatUnraisable("Exception ignored in GC shutdown");
        }
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(gcstate->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr))) {
                PyErr_FormatUnraisable("Exception ignored in GC shutdown "
                                       "while formatting garbage");
            }
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
_PyGC_Fini(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    Py_CLEAR(gcstate->garbage);
    Py_CLEAR(gcstate->callbacks);

    /* Prevent a subtle bug that affects sub-interpreters that use basic
     * single-phase init extensions (m_size == -1).  Those extensions cause objects
     * to be shared between interpreters, via the PyDict_Update(mdict, m_copy) call
     * in import_find_extension().
     *
     * If they are GC objects, their GC head next or prev links could refer to
     * the interpreter _gc_runtime_state PyGC_Head nodes.  Those nodes go away
     * when the interpreter structure is freed and so pointers to them become
     * invalid.  If those objects are still used by another interpreter and
     * UNTRACK is called on them, a crash will happen.  We untrack the nodes
     * here to avoid that.
     *
     * This bug was originally fixed when reported as gh-90228.  The bug was
     * re-introduced in gh-94673.
     */
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        finalize_unlink_gc_head(&gcstate->generations[i].head);
    }
    finalize_unlink_gc_head(&gcstate->permanent_generation.head);
}

/* for debugging */
void
_PyObject_GC_Link(PyObject *op)
{
    PyGC_Head *gc = AS_GC(op);
    // gc must be correctly aligned
    _PyObject_ASSERT(op, ((uintptr_t)gc & (sizeof(uintptr_t)-1)) == 0);

    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;
    gc->_gc_next = 0;
    gc->_gc_prev = 0;
    gcstate->generations[0].count++; /* number of allocated GC objects */
    if (gcstate->generations[0].count > gcstate->generations[0].threshold &&
        gcstate->enabled &&
        gcstate->generations[0].threshold &&
        !_Py_atomic_load_int_relaxed(&gcstate->collecting) &&
        !_PyErr_Occurred(tstate))
    {
        _Py_ScheduleGC(tstate);
    }
}

void
_Py_RunGC(PyThreadState *tstate)
{
    GCState *gcstate = get_gc_state();
    if (!gcstate->enabled) {
        return;
    }
    gc_collect_main(tstate, GENERATION_AUTO, _Py_GC_REASON_HEAP);
}

void
PyObject_GC_Del(void *op)
{
    size_t presize = _PyType_PreHeaderSize(Py_TYPE(op));
    PyGC_Head *g = AS_GC(op);
    if (_PyObject_GC_IS_TRACKED(op)) {
        gc_list_remove(g);
#ifdef Py_DEBUG
        PyObject *exc = PyErr_GetRaisedException();
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL,
                                     "Object of type %s is not untracked "
                                     "before destruction",
                                     Py_TYPE(op)->tp_name))
        {
            PyErr_FormatUnraisable("Exception ignored on object deallocation");
        }
        PyErr_SetRaisedException(exc);
#endif
    }
    GCState *gcstate = get_gc_state();
    if (gcstate->generations[0].count > 0) {
        gcstate->generations[0].count--;
    }
    PyObject_Free(((char *)op)-presize);
}

void
PyUnstable_GC_VisitObjects(gcvisitobjects_t callback, void *arg)
{
    GCState *gcstate = get_gc_state();
    int original_state = gcstate->enabled;
    gcstate->enabled = 0;
    for (size_t i = 0; i < NUM_GENERATIONS; i++) {
        if (visit_generation(callback, arg, &gcstate->generations[i]) < 0) {
            goto done;
        }
    }
    visit_generation(callback, arg, &gcstate->permanent_generation);
done:
    gcstate->enabled = original_state;
}

