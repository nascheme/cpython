/* Grail's boxed Atom.
 *
 * The cell holds one owned reference, replaced by atomic exchange or
 * compare-exchange. Readers acquire a strong reference without locking, using
 * the same protocol as the free-threaded list and dict: every stored value is
 * marked maybe-weakref so a concurrent reader's conditional incref cannot
 * resurrect an object whose last reference was dropped, and the object heap
 * delays page reuse with QSBR. Retired values are released after the exchange,
 * so finalizers may reenter the cell. swap callbacks run with no lock held.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "internal/pycore_object.h"  // _Py_DECREF_TYPE(), _Py_XGetRef()
#include "internal/pycore_pyatomic_ft_wrappers.h"

#include <stdint.h>

typedef struct {
    PyObject_HEAD
    PyObject *value;             /* NULL only after clear. */
} Atom;

typedef struct {
    PyTypeObject *atom_type;
} AtomState;

/* Prepare an owned reference for publication in the cell. */
static PyObject *
publishable(PyObject *value)
{
#ifdef Py_GIL_DISABLED
    _PyObject_SetMaybeWeakref(value);
#endif
    return Py_NewRef(value);
}

/* Replace the stored reference unconditionally; return the old one. */
static PyObject *
cell_exchange(Atom *self, PyObject *owned)
{
#ifdef Py_GIL_DISABLED
    return _Py_atomic_exchange_ptr(&self->value, owned);
#else
    PyObject *old = self->value;
    self->value = owned;
    return old;
#endif
}

static inline void
cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/* After a lost CAS, wait a randomized, exponentially growing interval before
 * retrying. Under contention each retry recomputes the candidate and touches
 * the same shared refcounts, so immediate retries mostly destroy throughput.
 * Called detached, so the wait never delays a stop-the-world request. */
static void
backoff(uint64_t *seed, int failures)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 7;
    *seed ^= *seed << 17;
    int bits = failures < 12 ? failures : 12;
    uint64_t spins = (*seed & ((UINT64_C(1) << bits) - 1)) * 16;
    for (uint64_t i = 0; i < spins; i++) {
        cpu_relax();
    }
}

static PyObject *
atom_alloc(PyTypeObject *type, PyObject *value)
{
    Atom *self = (Atom *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->value = publishable(value);  /* Not yet published. */
    return (PyObject *)self;
}

static PyObject *
atom_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *value;
    static char *keywords[] = {"value", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:Atom", keywords, &value)) {
        return NULL;
    }
    return atom_alloc(type, value);
}

static int
atom_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    /* GC traversal runs with mutators stopped (or under the GIL). */
    Py_VISIT(((Atom *)op)->value);
    return 0;
}

static int
atom_clear(PyObject *op)
{
    Py_XDECREF(cell_exchange((Atom *)op, NULL));
    return 0;
}

static void
atom_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    atom_clear(op);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static PyObject *
atom_snapshot(Atom *self)
{
#ifdef Py_GIL_DISABLED
    PyObject *value = _Py_XGetRef(&self->value);
#else
    PyObject *value = Py_XNewRef(self->value);
#endif
    if (value == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Atom has been cleared");
    }
    return value;
}

static PyObject *
atom_value(PyObject *op, void *closure)
{
    return atom_snapshot((Atom *)op);
}

static PyObject *
atom_reset(PyObject *op, PyObject *value)
{
    Atom *self = (Atom *)op;
    PyObject *owned = publishable(value);
    PyObject *retired = FT_ATOMIC_LOAD_PTR(self->value);
    do {
        if (retired == NULL) {
            Py_DECREF(owned);
            PyErr_SetString(PyExc_RuntimeError, "Atom has been cleared");
            return NULL;
        }
    } while (!_Py_atomic_compare_exchange_ptr(&self->value, &retired, owned));
    /* Release after the exchange: __del__ may read or update this Atom. */
    Py_DECREF(retired);
    return Py_NewRef(value);
}

static int
atom_cas(Atom *self, PyObject *expected, PyObject *desired)
{
    /* A cheap pre-check avoids writing desired's refcount on a doomed CAS. */
    PyObject *current = FT_ATOMIC_LOAD_PTR(self->value);
    if (current != expected) {
        if (current == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "Atom has been cleared");
            return -1;
        }
        return 0;
    }
    PyObject *owned = publishable(desired);
    int result;
    if (_Py_atomic_compare_exchange_ptr(&self->value, &current, owned)) {
        /* The cell's reference to expected moves to us. */
        result = 1;
    }
    else {
        result = current == NULL ? -1 : 0;
    }
    if (result == 1) {
        Py_DECREF(expected);
    }
    else {
        Py_DECREF(owned);
        if (result < 0) {
            PyErr_SetString(PyExc_RuntimeError, "Atom has been cleared");
        }
    }
    return result;
}

static PyObject *
atom_add(PyObject *op, PyObject *delta)
{
    if (!PyLong_CheckExact(delta)) {
        PyErr_SetString(PyExc_TypeError, "add delta must be an exact int");
        return NULL;
    }
    uint64_t seed = (uintptr_t)PyThreadState_GetUnchecked() | 1;
    int failures = 0;
    while (1) {
        if (PyErr_CheckSignals() < 0) {
            return NULL;
        }
        PyObject *old = atom_snapshot((Atom *)op);
        if (old == NULL) {
            return NULL;
        }
        if (!PyLong_CheckExact(old)) {
            Py_DECREF(old);
            PyErr_SetString(PyExc_TypeError, "add value must be an exact int");
            return NULL;
        }
        /* Use the builtin slot directly, not PyNumber_Add: open arithmetic
         * registrations must not affect counters. Allocate before the CAS
         * so GC and allocation failure cannot expose a partial update. */
        PyObject *candidate = PyLong_Type.tp_as_number->nb_add(old, delta);
        if (candidate == NULL) {
            Py_DECREF(old);
            return NULL;
        }
        int changed = atom_cas((Atom *)op, old, candidate);
        /* Keep the snapshot alive through CAS, including identity ABA. */
        Py_DECREF(old);
        if (changed == 1) {
            return candidate;
        }
        Py_DECREF(candidate);
        if (changed < 0) {
            return NULL;
        }
        /* Cooperate with stop-the-world requests even on native-only retries. */
        Py_BEGIN_ALLOW_THREADS
        backoff(&seed, ++failures);
        Py_END_ALLOW_THREADS
    }
}

static PyObject *
atom_compare_and_set(PyObject *op, PyObject *args)
{
    PyObject *expected, *desired;
    if (!PyArg_UnpackTuple(args, "compare_and_set", 2, 2, &expected, &desired)) {
        return NULL;
    }
    int result = atom_cas((Atom *)op, expected, desired);
    return result < 0 ? NULL : PyBool_FromLong(result);
}

static PyObject *
atom_swap(PyObject *op, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs == 0) {
        PyErr_SetString(PyExc_TypeError, "swap requires a positional callable");
        return NULL;
    }
    if (!PyCallable_Check(args[0])) {
        PyErr_SetString(PyExc_TypeError, "swap's first argument must be callable");
        return NULL;
    }
    Py_ssize_t nkwargs = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    if (nargs > PY_SSIZE_T_MAX - nkwargs ||
        (size_t)nargs > SIZE_MAX / sizeof(PyObject *) - (size_t)nkwargs) {
        return PyErr_NoMemory();
    }
    /* Replace the callable argument with the snapshot; other positional and
     * keyword arguments stay in place. The caller keeps args alive throughout. */
    PyObject **callargs = PyMem_Malloc(((size_t)nargs + (size_t)nkwargs) * sizeof(PyObject *));
    if (callargs == NULL) {
        return PyErr_NoMemory();
    }
    for (Py_ssize_t i = 1; i < nargs + nkwargs; i++) {
        callargs[i] = args[i];
    }
    PyObject *result = NULL;
    uint64_t seed = (uintptr_t)PyThreadState_GetUnchecked() | 1;
    int failures = 0;
    while (1) {
        /* Bounded cancellation latency even if every CAS loses contention. */
        if (PyErr_CheckSignals() < 0) {
            break;
        }
        PyObject *old = atom_snapshot((Atom *)op);
        if (old == NULL) {
            break;
        }
        callargs[0] = old;
        PyObject *candidate = PyObject_Vectorcall(args[0], callargs, (size_t)nargs, kwnames);
        if (candidate == NULL) {
            Py_DECREF(old);
            break;
        }
        int changed = atom_cas((Atom *)op, old, candidate);
        /* The strong snapshot prevents stale-pointer use/address recycling.
         * Identity ABA (A -> B -> the same A) intentionally remains allowed. */
        Py_DECREF(old);
        if (changed == 1) {
            result = candidate;
            break;
        }
        Py_DECREF(candidate);
        if (changed < 0) {
            break;
        }
        /* A loop of native callbacks must also cooperate with FT stop-the-world
         * requests. No lock is held and no Python API is used while detached. */
        Py_BEGIN_ALLOW_THREADS
        backoff(&seed, ++failures);
        Py_END_ALLOW_THREADS
    }
    PyMem_Free(callargs);
    return result;
}

static PyObject *
atom_repr(PyObject *op)
{
    int recursive = Py_ReprEnter(op);
    if (recursive != 0) {
        return recursive < 0 ? NULL : PyUnicode_FromString("...");
    }
    PyObject *value = atom_snapshot((Atom *)op);
    PyObject *result = value == NULL ? NULL : PyUnicode_FromFormat("Atom(%R)", value);
    Py_XDECREF(value);
    Py_ReprLeave(op);
    return result;
}

static PyObject *
atom_copy(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyObject *value = atom_snapshot((Atom *)op);
    if (value == NULL) {
        return NULL;
    }
    PyObject *result = atom_alloc(Py_TYPE(op), value);
    Py_DECREF(value);
    return result;
}

static PyObject *
atom_deepcopy(PyObject *op, PyObject *memo)
{
    if (!PyDict_Check(memo)) {
        PyErr_SetString(PyExc_TypeError, "deepcopy memo must be a dict");
        return NULL;
    }
    PyObject *value = atom_snapshot((Atom *)op);
    if (value == NULL) {
        return NULL;
    }
    PyObject *result = atom_alloc(Py_TYPE(op), Py_None);
    PyObject *key = PyLong_FromVoidPtr(op);
    if (result == NULL || key == NULL || PyDict_SetItem(memo, key, result) < 0) {
        Py_XDECREF(result);
        Py_XDECREF(key);
        Py_DECREF(value);
        return NULL;
    }
    /* Memoize the new mutable cell before descending: an Atom may contain
     * itself directly or participate in arbitrary mutable cycles. */
    PyObject *copy = PyImport_ImportModule("copy");
    PyObject *copied = copy == NULL ? NULL : PyObject_CallMethod(copy, "deepcopy", "OO", value, memo);
    Py_XDECREF(copy);
    Py_DECREF(value);
    if (copied == NULL) {
        PyObject *error = PyErr_GetRaisedException();
        PyObject *entry = NULL;
        if (PyDict_GetItemRef(memo, key, &entry) > 0 && entry == result) {
            if (PyDict_DelItem(memo, key) < 0) {
                PyErr_Clear();
            }
        }
        Py_XDECREF(entry);
        Py_DECREF(key);
        Py_DECREF(result);
        PyErr_SetRaisedException(error);
        return NULL;
    }
    Py_DECREF(key);
    PyObject *updated = atom_reset(result, copied);
    Py_DECREF(copied);
    if (updated == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    Py_DECREF(updated);
    return result;
}

static PyObject *
atom_reduce(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyObject *value = atom_snapshot((Atom *)op);
    if (value == NULL) {
        return NULL;
    }
    /* State is serialized after the new Atom is memoized, preserving cycles. */
    PyObject *args = PyTuple_Pack(1, Py_None);
    PyObject *result = args == NULL ? NULL : PyTuple_Pack(3, Py_TYPE(op), args, value);
    Py_XDECREF(args);
    Py_DECREF(value);
    return result;
}

static PyObject *
atom_setstate(PyObject *op, PyObject *value)
{
    PyObject *updated = atom_reset(op, value);
    if (updated == NULL) {
        return NULL;
    }
    Py_DECREF(updated);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(atom_doc,
"Atom(value)\n--\n\n"
"A synchronized reference cell. Mutable contents are not made thread-safe.\n"
"swap callbacks must be pure: contention can cause them to run repeatedly.");
PyDoc_STRVAR(swap_doc,
"swap($self, f, /, *args, **kwargs)\n--\n\n"
"Return the committed result of f(value, *args, **kwargs), retrying as needed.");

PyDoc_STRVAR(add_doc,
"add($self, delta, /)\n--\n\n"
"Atomically add exact ints and return the committed new value.\n"
"Both the current value and delta must be exact builtin ints.\n"
"Grail operator registrations are ignored; allocation failure does not commit.");

static PyMethodDef atom_methods[] = {
    {"add", atom_add, METH_O, add_doc},
    {"reset", atom_reset, METH_O, "Replace the value and return the supplied value."},
    {"compare_and_set", atom_compare_and_set, METH_VARARGS, "Replace only when the current value is the expected object (identity)."},
    {"swap", (PyCFunction)(void(*)(void))atom_swap, METH_FASTCALL | METH_KEYWORDS, swap_doc},
    {"__copy__", atom_copy, METH_NOARGS, NULL},
    {"__deepcopy__", atom_deepcopy, METH_O, NULL},
    {"__reduce__", atom_reduce, METH_NOARGS, NULL},
    {"__setstate__", atom_setstate, METH_O, NULL},
    {"__class_getitem__", Py_GenericAlias, METH_O | METH_CLASS, NULL},
    {NULL}
};
static PyGetSetDef atom_getsets[] = {
    {"value", atom_value, NULL, "Read the current value as an owned reference.", NULL},
    {NULL}
};
static PyType_Slot atom_type_slots[] = {
    {Py_tp_new, atom_new},
    {Py_tp_dealloc, atom_dealloc},
    {Py_tp_traverse, atom_traverse},
    {Py_tp_clear, atom_clear},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_repr, atom_repr},
    {Py_tp_doc, (void *)atom_doc},
    {Py_tp_methods, atom_methods},
    {Py_tp_getset, atom_getsets},
    {0}
};
static PyType_Spec atom_spec = {
    "grail.Atom", sizeof(Atom), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    atom_type_slots
};

static int
atom_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    AtomState *state = PyModule_GetState(module);
    Py_VISIT(state->atom_type);
    return 0;
}

static int
atom_module_clear(PyObject *module)
{
    AtomState *state = PyModule_GetState(module);
    Py_CLEAR(state->atom_type);
    return 0;
}

static int
atom_exec(PyObject *module)
{
    AtomState *state = PyModule_GetState(module);
    state->atom_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &atom_spec, NULL);
    if (state->atom_type == NULL) {
        return -1;
    }
    return PyModule_AddObjectRef(module, "Atom", (PyObject *)state->atom_type);
}

PyABIInfo_VAR(atom_abi);
static PyModuleDef_Slot atom_module_slots[] = {
    {Py_mod_abi, &atom_abi},
    {Py_mod_exec, atom_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};
static struct PyModuleDef atom_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail_atom",
    .m_size = sizeof(AtomState),
    .m_slots = atom_module_slots,
    .m_traverse = atom_module_traverse,
    .m_clear = atom_module_clear,
};
PyMODINIT_FUNC
PyInit__grail_atom(void)
{
    return PyModuleDef_Init(&atom_module);
}
