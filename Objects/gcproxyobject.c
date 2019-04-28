
#include "Python.h"

#include "gcobject.h"

/* A PyGCObject that's a proxy for a PyObject. */

/* Doesn't need its own object struct because all PyGCObjects store a
 * PyObject *. */
 
PyGCTypeObject PyGCProxy_Type;

PyGCObject *
PyGCProxyObject_New(PyObject *ref)
{
    PyGCObject *self = GC_MALLOC(sizeof(PyGCObject));
    PyGCObject_INIT(self, &PyGCProxy_Type);
    self->gcob_ref = ref;
    Py_INCREF(ref);
    return self;
}

