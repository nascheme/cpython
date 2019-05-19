#ifndef Py_CPYTHON_OBJIMPL_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* This function returns the number of allocated memory blocks, regardless of size */
PyAPI_FUNC(Py_ssize_t) _Py_GetAllocatedBlocks(void);

/* Macros */
#ifdef WITH_PYMALLOC
PyAPI_FUNC(int) _PyObject_DebugMallocStats(FILE *out);
#endif


typedef struct {
    /* user context passed as the first argument to the 2 functions */
    void *ctx;

    /* allocate an arena of size bytes */
    void* (*alloc) (void *ctx, size_t size);

    /* free an arena */
    void (*free) (void *ctx, void *ptr, size_t size);
} PyObjectArenaAllocator;

/* Get the arena allocator. */
PyAPI_FUNC(void) PyObject_GetArenaAllocator(PyObjectArenaAllocator *allocator);

/* Set the arena allocator. */
PyAPI_FUNC(void) PyObject_SetArenaAllocator(PyObjectArenaAllocator *allocator);


PyAPI_FUNC(Py_ssize_t) _PyGC_CollectNoFail(void);
PyAPI_FUNC(Py_ssize_t) _PyGC_CollectIfEnabled(void);


/* Test if an object has a GC head */
#define PyObject_IS_GC(o) \
    (PyType_IS_GC(Py_TYPE(o)) \
     && (Py_TYPE(o)->tp_is_gc == NULL || Py_TYPE(o)->tp_is_gc(o)))

/* GC information is stored BEFORE the object structure. */
typedef struct _gc_head {
    // Pointer to next object in the list.
    // 0 means the object is not tracked
    struct _gc_head *_gc_next;

    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    struct _gc_head *_gc_prev;
    Py_ssize_t _gc_refs;
    unsigned char gc_color;
    unsigned char gc_flags;
    unsigned char gc_gen;
} PyGC_Head;

#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)

/* True if the object is currently tracked by the GC. */
#define _PyObject_GC_IS_TRACKED(o) (_Py_AS_GC(o)->_gc_next != 0)

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && \
        (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))


// Lowest bit of _gc_next is used for flags only in GC.
// But it is always 0 for normal code.
#define _PyGCHead_NEXT(g)        ((PyGC_Head*)(g)->_gc_next)
#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (p))

#define _PyGCHead_PREV(g)        ((PyGC_Head*)(g)->_gc_prev)
#define _PyGCHead_SET_PREV(g, p) ((g)->_gc_prev = (PyGC_Head*)(p))


#define GC_FLAG_HEAP (1<<0)
#define GC_FLAG_FINIALIZER_REACHABLE (1<<1)
/* reachable from legacy finalizer, cannot be collected */
#define GC_FLAG_LEGACY_FINIALIZER_REACHABLE (1<<2)
/* we think this is garbage, could be revived by finalizers */
#define GC_FLAG_GARBAGE (1<<3)
/* set when the object is in generation which is GCed currently. */
// No objects in interpreter have this flag after GC ends.
#define GC_FLAG_COLLECTING (1<<4)
/* set when tp_finalize is called */
#define GC_FLAG_FINALIZED  (1<<5)
#define GC_FLAG_UNREACHABLE (1<<6)
#define GC_FLAG_REACHABLE (1<<7)

#define _PyGC_SET_FLAG(g, v) ((g)->gc_flags |= v)
#define _PyGC_CLEAR_FLAG(g, v) ((g)->gc_flags &= ~(v))
#define _PyGC_HAVE_FLAG(g, v) ((g)->gc_flags & v)

#define _PyGC_FINALIZED(o) \
    _PyGC_HAVE_FLAG(_Py_AS_GC(o), GC_FLAG_FINALIZED)
#define _PyGC_SET_FINALIZED(o) \
    _PyGC_SET_FLAG(_Py_AS_GC(o), GC_FLAG_FINALIZED)


PyAPI_FUNC(PyObject *) _PyObject_GC_Malloc(size_t size);
PyAPI_FUNC(PyObject *) _PyObject_GC_Calloc(size_t size);


/* Test if a type supports weak references */
#define PyType_SUPPORTS_WEAKREFS(t) ((t)->tp_weaklistoffset > 0)

#define PyObject_GET_WEAKREFS_LISTPTR(o) \
    ((PyObject **) (((char *) (o)) + Py_TYPE(o)->tp_weaklistoffset))

#ifdef __cplusplus
}
#endif
