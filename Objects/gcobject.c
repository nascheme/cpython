
#include "Python.h"

#include "gcobject.h"

void
_PyGC_call_finalizer(void *_op, void *unused)
{
    PyGCObject *op = (PyGCObject *)_op;
    Py_CLEAR(op->gcob_ref);
    assert(PyGC_TYPE(op)->gctp_finalizer);
    PyGC_TYPE(op)->gctp_finalizer(op);
}

