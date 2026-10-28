#ifndef Py_INTERNAL_OBJECT_DEFERRED_H
#define Py_INTERNAL_OBJECT_DEFERRED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pycore_gc.h"

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Mark an object as supporting deferred reference counting. This is a no-op
// in the default (with GIL) build. Objects that use deferred reference
// counting should be tracked by the GC so that they are eventually collected.
extern void _PyObject_SetDeferredRefcount(PyObject *op);

#ifdef Py_GIL_DISABLED
// Enable deferred refcounting on an object shared between threads, tracking
// untracked exact tuples and frozensets first.  Returns 1 if newly enabled.
extern int _PyObject_EnableDeferredShared(PyObject *op);

// Called with a strong reference to a cell value loaded by a thread that
// does not own it; defers the refcount of eligible immutable values.
PyAPI_FUNC(void) _PyObject_MaybeDeferCellValue(PyObject *op);
#endif

static inline int
_PyObject_HasDeferredRefcount(PyObject *op)
{
#ifdef Py_GIL_DISABLED
    return _PyObject_HAS_GC_BITS(op, _PyGC_BITS_DEFERRED);
#else
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_OBJECT_DEFERRED_H
