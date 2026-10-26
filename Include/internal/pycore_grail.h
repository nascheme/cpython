#ifndef Py_INTERNAL_GRAIL_H
#define Py_INTERNAL_GRAIL_H
/* Experimental, private Grail runtime. No layout here is an extension API. */
#include "Python.h"
/* Record instance layout, shared so the core can recognize value records. */
typedef struct {
    PyObject_HEAD
    int conforming;
    /* Cached default hash of a conforming value record: valid while hash_tag
     * equals the current hash-hook epoch (see record_hash); 0 when empty. */
    uint32_t hash_tag;
    Py_hash_t hash;
    PyObject *fields[];
} RecordObject;

/* Heap-type schema: (names, plans, defaults, annotations, eligible, nodes).  A
 * conforming instance of an eligible type holds only values, so it has no
 * finalizer or weakrefs and never keeps one alive. */
static inline int
_PyGrail_IsConformingValueRecord(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return 0;
    }
    PyObject *schema = ((PyHeapTypeObject *)type)->_ht_grail_schema;
    return schema != NULL && PyTuple_GET_ITEM(schema, 4) == Py_True &&
           ((RecordObject *)op)->conforming;
}

#ifdef Py_BUILD_CORE
#include "pycore_interp.h"
#include "pycore_pystate.h"
#include "opcode.h"  // NB_*

#include "pycore_stackref.h"

/* Record types are frozen heap types whose private schema is installed
 * before publication. True when a positional CALL with nargs arguments
 * supplies every field (CALL_RECORD's per-execution guard). */
static inline int
_PyGrail_IsPositionalRecordCall(PyObject *callable, int nargs)
{
    if (!PyType_Check(callable) ||
        !PyType_HasFeature((PyTypeObject *)callable, Py_TPFLAGS_HEAPTYPE)) {
        return 0;
    }
    PyObject *schema = ((PyHeapTypeObject *)callable)->_ht_grail_schema;
    return schema != NULL &&
           PyTuple_GET_SIZE(PyTuple_GET_ITEM(schema, 1)) == nargs;
}

/* Type of records parked on the per-thread freelist: their own type may
 * already be gone, and the free-threaded GC reads the type of every block. */
extern PyTypeObject _PyGrail_FreeRecord_Type;

/* True for a record type: its private schema is installed before
 * publication (and only a cleared, dying type loses it). */
static inline int
_PyGrail_IsRecordTypeFast(PyTypeObject *tp)
{
    return PyType_HasFeature(tp, Py_TPFLAGS_HEAPTYPE) &&
           ((PyHeapTypeObject *)tp)->_ht_grail_schema != NULL;
}

PyAPI_FUNC(PyObject *) _PyGrail_MatchRecordPositional(PyThreadState *tstate,
    PyObject *subject, PyObject *type, Py_ssize_t nargs, PyObject *names);
/* Code units from a record match site's MATCH_CLASS to its UNPACK_SEQUENCE. */
#define _PyGrail_MATCH_TO_UNPACK 6
PyAPI_FUNC(int) _PyGrail_IsRecordMatchSite(PyCodeObject *code, Py_ssize_t match,
                                           int nargs);
struct _PyInterpreterFrame;
PyAPI_FUNC(int) _PyGrail_UnpackMatchedRecord(struct _PyInterpreterFrame *frame,
    PyObject *record, int n, _PyStackRef *sp);

PyAPI_FUNC(PyTypeObject *) _PyGrail_ConformingFieldType(PyTypeObject *type,
                                                       Py_ssize_t offset);

/* Field count of a record (UNPACK_SEQUENCE_RECORD's guard). */
static inline Py_ssize_t
_PyGrail_RecordFieldCount(PyObject *record)
{
    return (Py_TYPE(record)->tp_basicsize - offsetof(RecordObject, fields)) /
           (Py_ssize_t)sizeof(PyObject *);
}
PyAPI_FUNC(PyObject *) _PyGrail_RecordFromStackRefsStealOnSuccess(PyTypeObject *,
                                                                _PyStackRef *, Py_ssize_t);
int _PyGrail_MatchRecord(PyObject *subject, PyObject *type, Py_ssize_t nargs,
                         PyObject *kwargs, PyObject **result);

static inline int
_PyGrail_OperatorsActive(void)
{
    return _Py_atomic_load_int_acquire(
        &_PyInterpreterState_GET()->grail_operators_active);
}

/* Operator registrations need at least one heap type, so no registered
 * pair has a builtin arithmetic specialization: every builtin kernel requires
 * exact static types. */
static inline int
_PyGrail_IsStaticPair(PyTypeObject *left, PyTypeObject *right)
{
    return !(left->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
           !(right->tp_flags & Py_TPFLAGS_HEAPTYPE);
}

#endif

/* Exact-pair arithmetic only: 1 with the final result (NULL on error), or 0
 * on a miss. */
int _PyGrail_BinaryOp(PyObject *, PyObject *, int, PyObject **);
/* Registered implementation for a BINARY_OP oparg and exact operand types,
 * or NULL. Lock-free, no errors or allocation; the borrowed result is valid
 * until the calling thread's next quiescent state. */
PyAPI_FUNC(PyObject *) _PyGrail_BinaryOpLookup(PyTypeObject *, PyTypeObject *, int);
PyAPI_FUNC(PyObject *) _PyGrail_BinaryOpGeneric(int);
/* Implementation an ordinary generic selects for its first argument, or NULL
 * (not a generic, or cold resolution needed). Same rules as above. */
PyAPI_FUNC(PyObject *) _PyGrail_GenericSelect(PyObject *, PyObject *, int);
PyAPI_FUNC(PyObject *) _PyGrail_GenericExactSelect(PyObject *, PyTypeObject *);
/* Bumped (release) after every change to a generic's registrations or
 * resolution cache, so a result derived from its selections can be tagged. */
PyAPI_FUNC(uint64_t) _PyGrail_GenericGeneration(PyObject *);
PyAPI_FUNC(int) _PyGrail_ConfigureOperators(PyObject *);

typedef struct {
    PyTypeObject *generic_type;
    PyObject *repr_hook;
    PyObject *eq_hook;
    PyObject *hash_hook;
    PyObject *operators;
    PyTypeObject *kernel_type;
} GrailState;

PyAPI_FUNC(int) _PyGrail_InitGenerics(PyObject *module);
PyAPI_FUNC(int) _PyGrail_IsRecordType(PyObject *obj);
PyAPI_FUNC(PyObject *) _PyGrail_MakeRecord(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_RecordInfo(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_RecordValues(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_IsConforming(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_DeepcopyIsSelf(PyObject *, PyObject *);
PyAPI_FUNC(int) _PyGrail_AddCopyReplace(PyObject *module);
PyAPI_FUNC(int) _PyGrail_InitKernels(PyObject *module);
PyAPI_FUNC(PyObject *) _PyGrail_MakeKernel(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_IsKernelPy(PyObject *, PyObject *);
/* Element-wise record kernels (see recordobject.c). Apply may borrow the
 * kernel; it reads it once before anything can run. */
PyAPI_FUNC(void) _PyGrail_KernelDealloc(PyObject *);
static inline int
_PyGrail_IsKernel(PyObject *obj)
{
    return Py_TYPE(obj)->tp_dealloc == _PyGrail_KernelDealloc;
}
PyAPI_FUNC(PyObject *) _PyGrail_KernelApply(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_Substitutable(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_DefaultRepr(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_DefaultEq(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyGrail_DefaultHash(PyObject *, PyObject *);
/* Returns a strong exact registration, or NULL without an error if absent. */
PyObject *_PyGrail_GenericLookup(PyObject *generic, PyObject *const *types,
                                Py_ssize_t arity);
#endif
