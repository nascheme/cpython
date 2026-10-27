/* Grail dispatch: native exact/guarded inherited lookup.
 *
 * Reads are lock-free. Writers (registration, cache publication, ablation)
 * serialize on the generic's critical section, build immutable replacements,
 * publish each with one release store and retire the old one through QSBR.
 * A reader may therefore borrow anything reachable from a published table
 * until its next quiescent state; it never takes a lock or writes shared
 * memory on an exact or cached hit. Selected implementations, the fallback
 * and the generic itself use deferred reference counts, and callers hold the
 * selection in a GC-visible C stack reference across the call.
 * No user code runs under the registry critical section. */
#include "Python.h"
#include "internal/pycore_grail.h"
#include "internal/pycore_object.h"
#include "internal/pycore_optimizer.h"  // _Py_Executors_InvalidateDependency()
#include "internal/pycore_pyatomic_ft_wrappers.h"
#include "internal/pycore_stackref.h"
#include "internal/pycore_tuple.h"
#include <stddef.h>

#define RESOLUTION_CACHE_SIZE 16
#define RESOLUTION_MAX_REGISTRATIONS 32
/* Cache slots: type, implementation, guards, ABC epoch. */
#define CACHE_STRIDE 4

typedef struct {
    PyObject_HEAD
    vectorcallfunc vectorcall;
    PyObject *fallback;
    PyObject *first_name;
    PyObject *registry;     /* dict snapshot for resolvers; published */
    PyObject *table;        /* exact table or NULL; published */
    PyObject *resolver;
    PyObject *cache;        /* resolution table or NULL; published */
    int cache_enabled;
    uint64_t cache_generation;
    uint64_t cache_hits;
    uint64_t cache_misses;
    Py_ssize_t arity;
    unsigned long long version;
    int central_operator;
} GenericObject;

/* Private diagnostics switch: counting cache hits writes shared memory. */
static int dispatch_stats;

/* Immutable open-addressed tables keyed by exact type identity. A table is a
 * tuple of slots of `stride` items whose first `arity` items are the key
 * types; Py_None marks an empty slot. The load factor is at most 1/2, so
 * probing terminates. Keys are compared by address only and owned by the
 * table, so an unowned type pointer read from an operand can be probed. */
static inline size_t
type_hash(PyTypeObject *const *types, Py_ssize_t arity)
{
    uint64_t h = (uintptr_t)types[0];
    if (arity == 2) {
        h = h * UINT64_C(0x9E3779B97F4A7C15) ^ (uintptr_t)types[1];
    }
    h *= UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h >> 32);
}

static PyObject **
table_find(PyObject *table, Py_ssize_t arity, Py_ssize_t stride,
           PyTypeObject *const *types)
{
    PyObject **items = _PyTuple_ITEMS(table);
    size_t mask = (size_t)(PyTuple_GET_SIZE(table) / stride) - 1;
    for (size_t i = type_hash(types, arity) & mask;; i = (i + 1) & mask) {
        PyObject **slot = items + i * stride;
        if (slot[0] == Py_None ||
            (slot[0] == (PyObject *)types[0] &&
             (arity == 1 || slot[1] == (PyObject *)types[1]))) {
            return slot;
        }
    }
}

static PyObject *
table_new(Py_ssize_t count, Py_ssize_t stride)
{
    Py_ssize_t capacity = 8;
    while (capacity < 2 * count) {
        capacity *= 2;
    }
    PyObject *table = PyTuple_New(capacity * stride);
    if (table == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < capacity * stride; i++) {
        PyTuple_SET_ITEM(table, i, Py_None);
    }
    return table;
}

/* Only for unpublished tables; `values` must hold a new key. */
static void
table_insert(PyObject *table, Py_ssize_t arity, Py_ssize_t stride,
             PyObject *const *values)
{
    PyObject **slot = table_find(table, arity, stride,
                                 (PyTypeObject *const *)values);
    assert(slot[0] == Py_None);
    for (Py_ssize_t i = 0; i < stride; i++) {
        slot[i] = Py_NewRef(values[i]);
    }
}

static Py_ssize_t
table_count(PyObject *table, Py_ssize_t stride)
{
    Py_ssize_t count = 0;
    for (Py_ssize_t i = 0; table != NULL && i < PyTuple_GET_SIZE(table); i += stride) {
        count += PyTuple_GET_ITEM(table, i) != Py_None;
    }
    return count;
}

/* Exact table for a private registry dict. */
static PyObject *
build_table(PyObject *registry, Py_ssize_t arity)
{
    PyObject *table = table_new(PyDict_GET_SIZE(registry), arity + 1);
    if (table == NULL) {
        return NULL;
    }
    Py_ssize_t pos = 0;
    PyObject *value;
    while (PyDict_Next(registry, &pos, NULL, &value)) {
        PyObject *types = PyTuple_GET_ITEM(value, 0);
        PyObject *func = PyTuple_GET_ITEM(value, 1);
        PyObject *values[3] = {PyTuple_GET_ITEM(types, 0), func, func};
        if (arity == 2) {
            values[1] = PyTuple_GET_ITEM(types, 1);
        }
        table_insert(table, arity, arity + 1, values);
    }
    return table;
}

/* Implementations are shared by every calling thread. */
static void
enable_deferred(PyObject *obj)
{
    (void)PyUnstable_Object_EnableDeferredRefcount(obj);
}

static inline PyObject *exact_lookup(GenericObject *, PyTypeObject *const *);

/* Tier-2 traces may use this generic's exact selections as constants (see
   _GRAIL_CALL_GENERIC_FOLDED); invalidate them when the table changes. */
static void
invalidate_dependents(GenericObject *self)
{
    _Py_Executors_InvalidateDependency(_PyInterpreterState_GET(), self, 1);
}


int
_PyGrail_ConfigureOperators(PyObject *operators)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->grail_operators != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Grail operators are already configured");
        return -1;
    }
    const char *record_match = Py_GETENV("PYTHON_GRAIL_RECORD_MATCH");
    interp->grail_record_match_off =
        record_match != NULL && strcmp(record_match, "0") == 0;
    const char *native_hooks = Py_GETENV("PYTHON_GRAIL_NATIVE_HOOKS");
    interp->grail_native_hooks_off =
        native_hooks != NULL && strcmp(native_hooks, "0") == 0;
    const char *changed_only = Py_GETENV("PYTHON_GRAIL_REPLACE_CHANGED_ONLY");
    interp->grail_replace_check_all =
        changed_only != NULL && strcmp(changed_only, "0") == 0;
    const char *freelist = Py_GETENV("PYTHON_GRAIL_RECORD_FREELIST");
    interp->grail_record_freelist_off =
        freelist != NULL && strcmp(freelist, "0") == 0;
    interp->grail_operators = Py_NewRef(operators);
    for (int i = 0; i < 4; i++) {
        ((GenericObject *)PyTuple_GET_ITEM(operators, i))->central_operator = i + 1;
    }
    return 0;
}

/* Returns 1 with *result set to the registered implementation's final
   result (NULL on error), or 0 when the exact pair has no registration. */
int
_PyGrail_BinaryOp(PyObject *a, PyObject *b, int index, PyObject **result)
{
    PyObject *operators = _PyInterpreterState_GET()->grail_operators;
    /* Interpreter clearing may run arbitrary finalizers. No central table is
       available once its owning reference has been detached. */
    if (operators == NULL) {
        return 0;
    }
    /* Each type is read once and only compared with owned table keys; a
       racing __class__ assignment selects for either type. */
    PyTypeObject *types[] = {Py_TYPE(a), Py_TYPE(b)};
    GenericObject *op = (GenericObject *)PyTuple_GET_ITEM(operators, index);
    PyObject *func = exact_lookup(op, types);
    if (func == NULL) {
        return 0;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    _PyCStackRef ref;
    _PyThreadState_PushCStackRefNew(tstate, &ref, func);
    PyObject *args[] = {a, b};
    *result = PyObject_Vectorcall(PyStackRef_AsPyObjectBorrow(ref.ref), args, 2, NULL);
    _PyThreadState_PopCStackRef(tstate, &ref);
    return 1;
}

/* The central operator generic for a BINARY_OP oparg (borrowed), or NULL. */
PyObject *
_PyGrail_BinaryOpGeneric(int oparg)
{
    int index;
    switch (oparg) {
        case NB_ADD: case NB_INPLACE_ADD: index = 0; break;
        case NB_SUBTRACT: case NB_INPLACE_SUBTRACT: index = 1; break;
        case NB_MULTIPLY: case NB_INPLACE_MULTIPLY: index = 2; break;
        case NB_TRUE_DIVIDE: case NB_INPLACE_TRUE_DIVIDE: index = 3; break;
        default: return NULL;
    }
    PyObject *operators = _PyInterpreterState_GET()->grail_operators;
    if (operators == NULL) {
        return NULL;
    }
    return PyTuple_GET_ITEM(operators, index);
}

PyObject *
_PyGrail_BinaryOpLookup(PyTypeObject *left, PyTypeObject *right, int oparg)
{
    PyObject *op = _PyGrail_BinaryOpGeneric(oparg);
    if (op == NULL) {
        return NULL;
    }
    PyTypeObject *types[] = {left, right};
    return exact_lookup((GenericObject *)op, types);
}

static PyObject *
dispatch_key(PyObject *const *types, Py_ssize_t arity)
{
    PyObject *key = PyTuple_New(arity);
    if (key == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < arity; i++) {
        if (!PyType_Check(types[i])) {
            Py_DECREF(key);
            PyErr_SetString(PyExc_TypeError, "dispatch keys must be types");
            return NULL;
        }
        /* Identity keys avoid executing metaclass hash/equality under locks.
         * Registry values own the actual types, preventing address reuse. */
        PyObject *id = PyLong_FromVoidPtr(types[i]);
        if (id == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        PyTuple_SET_ITEM(key, i, id);
    }
    return key;
}

static uint64_t
abc_epoch(void)
{
    return _Py_atomic_load_uint64(&_PyInterpreterState_GET()->grail_abc_epoch);
}

/* Plans own every dependency, including metaclasses. Assigning a standard
   CPython type version also versions its bases, whose changes invalidate it.
   Unversionable/custom-MRO types conservatively stay on the cold path. */
static PyObject *
resolution_guards(PyObject *cls, PyObject *snapshot)
{
    Py_ssize_t count = PyDict_GET_SIZE(snapshot);
    if (count > RESOLUTION_MAX_REGISTRATIONS) {
        return Py_NewRef(Py_None);
    }
    PyObject *guards = PyTuple_New(2 * (count + 1));
    if (guards == NULL) {
        return NULL;
    }
    Py_ssize_t pos = 0, index = 0;
    PyObject *value;
    for (;;) {
        PyTypeObject *type = (PyTypeObject *)cls;
        PyTypeObject *meta = (PyTypeObject *)Py_NewRef((PyObject *)Py_TYPE(type));
        PyTypeObject *dependencies[] = {type, meta};
        for (int i = 0; i < 2; i++) {
            PyTypeObject *dep = dependencies[i];
            if (!PyUnstable_Type_AssignVersionTag(dep)) {
                Py_DECREF(meta);
                Py_DECREF(guards);
                return Py_NewRef(Py_None);
            }
            unsigned int version = FT_ATOMIC_LOAD_UINT_RELAXED(dep->tp_version_tag);
            if (version == 0) {
                Py_DECREF(meta);
                Py_DECREF(guards);
                return Py_NewRef(Py_None);
            }
            PyObject *dep_meta = Py_NewRef(
                i == 0 ? (PyObject *)meta : (PyObject *)Py_TYPE(dep));
            PyObject *guard = Py_BuildValue("(OIO)", dep, version, dep_meta);
            Py_DECREF(dep_meta);
            if (guard == NULL) {
                Py_DECREF(meta);
                Py_DECREF(guards);
                return NULL;
            }
            PyTuple_SET_ITEM(guards, index++, guard);
        }
        Py_DECREF(meta);
        if (!PyDict_Next(snapshot, &pos, NULL, &value)) {
            break;
        }
        cls = PyTuple_GET_ITEM(PyTuple_GET_ITEM(value, 0), 0);
    }
    return guards;
}

/* No allocation, Python callbacks, or reference releases. A racing mutation
   can use the old answer; completed mutations invalidate the standard tags.
   No Python-function call specialization is cached: function mutation remains
   visible through ordinary vectorcall of the owning callable. */
static int
resolution_valid(PyObject *guards, uint64_t epoch)
{
    if (epoch != abc_epoch()) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(guards); i++) {
        PyObject *guard = PyTuple_GET_ITEM(guards, i);
        PyTypeObject *type = (PyTypeObject *)PyTuple_GET_ITEM(guard, 0);
        unsigned long version = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(guard, 1));
        if (FT_ATOMIC_LOAD_UINT_RELAXED(type->tp_version_tag) != version ||
            (PyObject *)Py_TYPE(type) != PyTuple_GET_ITEM(guard, 2)) {
            return 0;
        }
    }
    return 1;
}

/* Lock-free; borrowed results are valid until the next quiescent state. */
static inline PyObject *
exact_lookup(GenericObject *self, PyTypeObject *const *types)
{
    PyObject *table = FT_ATOMIC_LOAD_PTR_ACQUIRE(self->table);
    if (table == NULL) {
        return NULL;
    }
    PyObject **slot = table_find(table, self->arity, self->arity + 1, types);
    return slot[0] == Py_None ? NULL : slot[self->arity];
}

static inline PyObject *
cached_lookup(GenericObject *self, PyTypeObject *type, int stats)
{
    PyObject *cache = FT_ATOMIC_LOAD_PTR_ACQUIRE(self->cache);
    if (cache == NULL) {
        return NULL;
    }
    PyObject **slot = table_find(cache, 1, CACHE_STRIDE, &type);
    if (slot[0] == Py_None ||
        !resolution_valid(slot[2], PyLong_AsUnsignedLongLong(slot[3]))) {
        return NULL;
    }
    if (stats && FT_ATOMIC_LOAD_INT_RELAXED(dispatch_stats)) {
        _Py_atomic_add_uint64(&self->cache_hits, 1);
    }
    return slot[1];
}

static inline PyObject *
fast_lookup(GenericObject *self, PyTypeObject *const *types, int resolve)
{
    PyObject *func = exact_lookup(self, types);
    if (func == NULL && resolve && self->resolver != Py_None) {
        func = cached_lookup(self, types[0], 1);
    }
    return func;
}

static void generic_dealloc(GenericObject *self);

/* Implementation selected by an ordinary (non-operator) generic for a first
 * argument, or NULL: not a generic, or a cold resolution would be needed.
 * Lock-free, no errors, allocation or callbacks; the borrowed result is
 * valid until the calling thread's next quiescent state. `stats` counts a
 * resolution-cache hit in the diagnostics (calls, not specializer probes). */
PyObject *
_PyGrail_GenericSelect(PyObject *callable, PyObject *first, int stats)
{
    if (Py_TYPE(callable)->tp_dealloc != (destructor)generic_dealloc) {
        return NULL;
    }
    GenericObject *self = (GenericObject *)callable;
    if (self->central_operator || self->arity != 1) {
        return NULL;
    }
    PyTypeObject *type = Py_TYPE(first);
    PyObject *func = exact_lookup(self, &type);
    if (func == NULL) {
        func = self->resolver == Py_None ? self->fallback :
            cached_lookup(self, type, stats);
    }
    return func;
}

/* For the tier-2 optimizer: the exact-table implementation an ordinary
 * generic selects for `type` (borrowed), or NULL. Unlike
 * _PyGrail_GenericSelect, resolution-cache entries are excluded: only
 * register/override change the exact table, and both invalidate executors
 * that depend on the generic. */
PyObject *
_PyGrail_GenericExactSelect(PyObject *callable, PyTypeObject *type)
{
    if (Py_TYPE(callable)->tp_dealloc != (destructor)generic_dealloc) {
        return NULL;
    }
    GenericObject *self = (GenericObject *)callable;
    if (self->central_operator || self->arity != 1) {
        return NULL;
    }
    return exact_lookup(self, &type);
}

/* For the tier-2 optimizer: the type (borrowed) registered for func in an
 * ordinary generic's exact table when it is the only registration of func
 * and the type cannot be subclassed, else NULL. Only that type selects
 * func: another type would need the registered type in its MRO. */
PyTypeObject *
_PyGrail_GenericSoleExactType(PyObject *callable, PyObject *func)
{
    if (Py_TYPE(callable)->tp_dealloc != (destructor)generic_dealloc) {
        return NULL;
    }
    GenericObject *self = (GenericObject *)callable;
    PyObject *table = self->table;
    if (self->central_operator || self->arity != 1 || table == NULL ||
        func == self->fallback) {
        return NULL;
    }
    PyObject *type = NULL;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(table); i += 2) {
        PyObject *key = PyTuple_GET_ITEM(table, i);
        if (key != Py_None && PyTuple_GET_ITEM(table, i + 1) == func) {
            if (type != NULL) {
                return NULL;
            }
            type = key;
        }
    }
    if (type == NULL || !PyType_Check(type) ||
        PyType_HasFeature((PyTypeObject *)type, Py_TPFLAGS_BASETYPE)) {
        return NULL;
    }
    return (PyTypeObject *)type;
}

/* A copy of `cache` plus one new entry, or NULL (no error) if it is full or
 * already holds the key. Then the caller publishes a fresh one-entry table. */
static PyObject *
cache_with(PyObject *cache, PyObject *const *values, int *error)
{
    Py_ssize_t count = table_count(cache, CACHE_STRIDE);
    if (cache == NULL || count >= RESOLUTION_CACHE_SIZE ||
        table_find(cache, 1, CACHE_STRIDE,
                   (PyTypeObject *const *)values)[0] != Py_None) {
        return NULL;
    }
    PyObject *replacement = table_new(count + 1, CACHE_STRIDE);
    if (replacement == NULL) {
        *error = 1;
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(cache); i += CACHE_STRIDE) {
        if (PyTuple_GET_ITEM(cache, i) != Py_None) {
            table_insert(replacement, 1, CACHE_STRIDE, _PyTuple_ITEMS(cache) + i);
        }
    }
    table_insert(replacement, 1, CACHE_STRIDE, values);
    return replacement;
}

/* Cold path for an exact and cache miss. `type` must be owned by the caller.
 * ABC/metaclass callbacks resolve one owning immutable registry snapshot,
 * outside all locks. Registration during resolution affects later calls.
 * Returns a strong reference, or NULL with an exception. */
static PyObject *
resolve_slow(GenericObject *self, PyObject *type)
{
    uint64_t generation = _Py_atomic_load_uint64_acquire(&self->cache_generation);
    int cache_enabled = FT_ATOMIC_LOAD_INT_RELAXED(self->cache_enabled);
    PyObject *snapshot = Py_NewRef(FT_ATOMIC_LOAD_PTR_ACQUIRE(self->registry));
    if (cache_enabled) {
        _Py_atomic_add_uint64(&self->cache_misses, 1);
    }
    uint64_t epoch = abc_epoch();
    PyObject *guards = !cache_enabled ? Py_NewRef(Py_None) :
        resolution_guards(type, snapshot);
    if (guards == NULL) {
        Py_DECREF(snapshot);
        return NULL;
    }
    PyObject *args[] = {type, snapshot, self->fallback};
    PyObject *func = PyObject_Vectorcall(self->resolver, args, 3, NULL);
    if (func != NULL && guards != Py_None && resolution_valid(guards, epoch)) {
        PyObject *epoch_obj = PyLong_FromUnsignedLongLong(epoch);
        PyObject *one = epoch_obj == NULL ? NULL : table_new(1, CACHE_STRIDE);
        if (one == NULL) {
            Py_CLEAR(func);
        }
        else {
            PyObject *values[CACHE_STRIDE] = {type, func, guards, epoch_obj};
            table_insert(one, 1, CACHE_STRIDE, values);
            enable_deferred(func);
            PyObject *retired = NULL;
            int error = 0;
            Py_BEGIN_CRITICAL_SECTION(self);
            if (self->cache_enabled && self->registry == snapshot &&
                self->cache_generation == generation &&
                resolution_valid(guards, epoch)) {
                PyObject *replacement = cache_with(self->cache, values, &error);
                if (!error) {
                    retired = self->cache;
                    FT_ATOMIC_STORE_PTR_RELEASE(
                        self->cache, replacement ? replacement : Py_NewRef(one));
                }
            }
            Py_END_CRITICAL_SECTION();
            _PyObject_XDecRefDelayed(retired);
            if (error) {
                Py_CLEAR(func);
            }
        }
        Py_XDECREF(one);
        Py_XDECREF(epoch_obj);
    }
    Py_DECREF(guards);
    Py_DECREF(snapshot);
    return func;
}

/* Strong selection, or NULL without an error for a miss without resolver.
 * `types` must be owned by the caller. */
static PyObject *
generic_lookup(GenericObject *self, PyObject *const *types, Py_ssize_t arity,
               int resolve)
{
    if (self->arity != arity) {
        PyErr_SetString(PyExc_TypeError, "wrong dispatch arity");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < arity; i++) {
        if (!PyType_Check(types[i])) {
            PyErr_SetString(PyExc_TypeError, "dispatch keys must be types");
            return NULL;
        }
    }
    PyObject *func = fast_lookup(self, (PyTypeObject *const *)types, resolve);
    if (func != NULL) {
        return Py_NewRef(func);
    }
    if (!resolve || self->resolver == Py_None) {
        return NULL;
    }
    return resolve_slow(self, types[0]);
}

static PyObject *
generic_vectorcall(PyObject *obj, PyObject *const *args, size_t nargsf,
                   PyObject *kwnames)
{
    GenericObject *self = (GenericObject *)obj;
    /* The builtin operator entry point owns exact-pair lookup and fallback.
       In particular, NotImplemented must not escape a direct ops.add call. */
    if (self->central_operator) {
        return PyObject_Vectorcall(self->fallback, args, nargsf, kwnames);
    }
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    PyObject *values[2] = {NULL, NULL};
    PyTypeObject *types[2] = {NULL, NULL};
    for (Py_ssize_t i = 0; i < self->arity && i < nargs; i++) {
        values[i] = args[i];
    }
    if (self->arity == 1 && values[0] == NULL && kwnames != NULL) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            int eq = PyObject_RichCompareBool(PyTuple_GET_ITEM(kwnames, i),
                                             self->first_name, Py_EQ);
            if (eq < 0) {
                return NULL;
            }
            if (eq) {
                values[0] = args[nargs + i];
                break;
            }
        }
    }
    PyThreadState *tstate = _PyThreadState_GET();
    _PyCStackRef ref;
    _PyThreadState_PushCStackRef(tstate, &ref);
    if (values[0] != NULL && (self->arity == 1 || values[1] != NULL)) {
        /* Unowned type reads are only probed (see exact_lookup). */
        for (Py_ssize_t i = 0; i < self->arity; i++) {
            types[i] = Py_TYPE(values[i]);
        }
        PyObject *func = fast_lookup(self, types, 1);
        if (func != NULL) {
            ref.ref = PyStackRef_FromPyObjectNew(func);
        }
        else if (self->resolver != Py_None) {
            /* Own the type before any allocation/detach: another thread can
               change an ordinary operand's __class__ while resolving. */
            PyObject *type = Py_NewRef((PyObject *)Py_TYPE(values[0]));
            func = resolve_slow(self, type);
            Py_DECREF(type);
            if (func == NULL) {
                _PyThreadState_PopCStackRef(tstate, &ref);
                return NULL;
            }
            ref.ref = PyStackRef_FromPyObjectSteal(func);
        }
    }
    if (PyStackRef_IsNull(ref.ref)) {
        ref.ref = PyStackRef_FromPyObjectNew(self->fallback);
    }
    PyObject *result = PyObject_Vectorcall(
        PyStackRef_AsPyObjectBorrow(ref.ref), args, nargsf, kwnames);
    _PyThreadState_PopCStackRef(tstate, &ref);
    return result;
}

static PyObject *
generic_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *fallback, *name, *resolver = Py_None;
    int pair = 0;
    static char *keywords[] = {"fallback", "first_name", "pair", "resolver", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OU|pO:Generic", keywords,
                                     &fallback, &name, &pair, &resolver)) {
        return NULL;
    }
    if (!PyCallable_Check(fallback)) {
        PyErr_SetString(PyExc_TypeError, "fallback must be callable");
        return NULL;
    }
    if (resolver != Py_None && (!PyCallable_Check(resolver) || pair)) {
        PyErr_SetString(PyExc_TypeError, "resolver requires single dispatch and a callable");
        return NULL;
    }
    GenericObject *self = (GenericObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->fallback = Py_NewRef(fallback);
    self->first_name = Py_NewRef(name);
    self->resolver = Py_NewRef(resolver);
    self->arity = pair ? 2 : 1;
    self->vectorcall = generic_vectorcall;
    self->registry = PyDict_New();
    self->cache_enabled = 1;
    if (self->registry == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    /* Generics are shared globals called from every thread. */
    enable_deferred((PyObject *)self);
    enable_deferred(fallback);
    return (PyObject *)self;
}

static PyObject *
generic_register(GenericObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *dispatch, *func;
    int override = 0;
    static char *keywords[] = {"dispatch", "func", "override", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|p:register", keywords,
                                     &dispatch, &func, &override)) {
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "implementation must be callable");
        return NULL;
    }
    if (self->central_operator &&
        _PyInterpreterState_GET()->grail_operators == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Grail operators have been cleared");
        return NULL;
    }
    PyObject *types[2] = {dispatch, NULL};
    if (self->arity == 2) {
        if (!PyTuple_CheckExact(dispatch) || PyTuple_GET_SIZE(dispatch) != 2) {
            PyErr_SetString(PyExc_TypeError, "operator dispatch requires two types");
            return NULL;
        }
        types[0] = PyTuple_GET_ITEM(dispatch, 0);
        types[1] = PyTuple_GET_ITEM(dispatch, 1);
    }
    /* Builtin arithmetic specializations run unguarded, so a pair of two
     * builtin (static) types cannot be registered. */
    if (self->central_operator && PyType_Check(types[0]) &&
        PyType_Check(types[1]) &&
        _PyGrail_IsStaticPair((PyTypeObject *)types[0], (PyTypeObject *)types[1])) {
        PyErr_Format(PyExc_TypeError,
                     "cannot register an operator for two built-in types "
                     "(%s, %s)", ((PyTypeObject *)types[0])->tp_name,
                     ((PyTypeObject *)types[1])->tp_name);
        return NULL;
    }
    PyObject *facade = PyImport_ImportModule("grail");
    PyObject *validated = facade == NULL ? NULL :
        PyObject_CallMethod(facade, "_validate_registration", "OOO",
                            self, dispatch, func);
    Py_XDECREF(facade);
    if (validated == NULL) {
        return NULL;
    }
    Py_DECREF(validated);
    PyObject *key = dispatch_key(types, self->arity);
    if (key == NULL) {
        return NULL;
    }
    PyObject *owned_types = PyTuple_New(self->arity);
    if (owned_types == NULL) {
        Py_DECREF(key);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < self->arity; i++) {
        PyTuple_SET_ITEM(owned_types, i, Py_NewRef(types[i]));
    }
    PyObject *entry = PyTuple_Pack(2, owned_types, func);
    Py_DECREF(owned_types);
    if (entry == NULL) {
        Py_DECREF(key);
        return NULL;
    }
    enable_deferred(func);
    PyObject *old = NULL, *replacement = NULL, *table = NULL;
    PyObject *old_table = NULL, *old_cache = NULL;
    int status = -1;
    Py_BEGIN_CRITICAL_SECTION(self);
    int exists = PyDict_Contains(self->registry, key);
    if (exists >= 0 && (!exists || override)) {
        replacement = PyDict_Copy(self->registry);
        if (replacement != NULL && PyDict_SetItem(replacement, key, entry) == 0 &&
            (table = build_table(replacement, self->arity)) != NULL) {
            /* Readers may see any mix of old and new parts; each is a
               consistent state that a racing call may observe anyway. */
            old = self->registry;
            FT_ATOMIC_STORE_PTR_RELEASE(self->registry, replacement);
            replacement = NULL;
            old_table = self->table;
            FT_ATOMIC_STORE_PTR_RELEASE(self->table, table);
            table = NULL;
            self->version++;
            old_cache = self->cache;
            FT_ATOMIC_STORE_PTR_RELEASE(self->cache, NULL);
            _Py_atomic_add_uint64(&self->cache_generation, 1);
            if (self->central_operator) {
                /* Publish after the registry update and before returning. */
                _Py_atomic_store_int_release(
                    &_PyInterpreterState_GET()->grail_operators_active, 1);
            }
            status = 0;
        }
    }
    else if (exists > 0) {
        PyErr_SetString(PyExc_TypeError,
                        "duplicate implementation; use override");
    }
    Py_END_CRITICAL_SECTION();
    if (status == 0) {
        invalidate_dependents(self);
    }
    /* Lock-free readers may still use retired snapshots and tables. */
    _PyObject_XDecRefDelayed(old);
    _PyObject_XDecRefDelayed(old_table);
    _PyObject_XDecRefDelayed(old_cache);
    Py_XDECREF(replacement);
    Py_DECREF(entry);
    Py_DECREF(key);
    if (status < 0) {
        return NULL;
    }
    return Py_NewRef(func);
}

static PyObject *
generic_dispatch(GenericObject *self, PyObject *dispatch)
{
    PyObject *types[2] = {dispatch, NULL};
    if (self->arity == 2) {
        if (!PyTuple_CheckExact(dispatch) || PyTuple_GET_SIZE(dispatch) != 2) {
            PyErr_SetString(PyExc_TypeError, "expected a pair of types");
            return NULL;
        }
        types[0] = PyTuple_GET_ITEM(dispatch, 0);
        types[1] = PyTuple_GET_ITEM(dispatch, 1);
    }
    PyObject *func = generic_lookup(self, types, self->arity, 1);
    if (func == NULL && !PyErr_Occurred()) {
        return Py_NewRef(self->fallback);
    }
    return func;
}

static int
generic_traverse(GenericObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->fallback);
    Py_VISIT(self->first_name);
    Py_VISIT(self->registry);
    Py_VISIT(self->table);
    Py_VISIT(self->resolver);
    Py_VISIT(self->cache);
    return 0;
}

static int
generic_clear(GenericObject *self)
{
    Py_CLEAR(self->fallback);
    Py_CLEAR(self->first_name);
    Py_CLEAR(self->registry);
    if (self->table != NULL) {
        invalidate_dependents(self);
    }
    Py_CLEAR(self->table);
    Py_CLEAR(self->resolver);
    Py_CLEAR(self->cache);
    return 0;
}

static void
generic_dealloc(GenericObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    generic_clear(self);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *
generic_get_fallback(GenericObject *self, void *closure)
{
    return Py_NewRef(self->fallback);
}

static PyObject *
generic_get_version(GenericObject *self, void *closure)
{
    unsigned long long version;
    Py_BEGIN_CRITICAL_SECTION(self);
    version = self->version;
    Py_END_CRITICAL_SECTION();
    return PyLong_FromUnsignedLongLong(version);
}

static PyObject *
generic_decorator(GenericObject *self, PyObject *args, int override)
{
    PyObject *facade = PyImport_ImportModule("grail");
    if (facade == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_CallMethod(facade, "_register_decorator", "OOO",
                                         self, args, override ? Py_True : Py_False);
    Py_DECREF(facade);
    return result;
}

static PyObject *
generic_impl(GenericObject *self, PyObject *func)
{
    PyObject *args = PyTuple_Pack(1, func);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = generic_decorator(self, args, 0);
    Py_DECREF(args);
    return result;
}

static PyObject *
generic_override(GenericObject *self, PyObject *args)
{
    return generic_decorator(self, args, 1);
}

static PyObject *
generic_get_pair(GenericObject *self, void *closure)
{
    return PyBool_FromLong(self->arity == 2);
}

static PyObject *
generic_get_name(GenericObject *self, void *closure)
{
    return PyObject_GetAttrString(self->fallback, "__name__");
}

/* Private experimental ablation and diagnostics; not a public cache API. */
uint64_t
_PyGrail_GenericGeneration(PyObject *op)
{
    return _Py_atomic_load_uint64_acquire(&((GenericObject *)op)->cache_generation);
}

static PyObject *
generic_set_cache_enabled(GenericObject *self, PyObject *enabled)
{
    int value = PyObject_IsTrue(enabled);
    if (value < 0) {
        return NULL;
    }
    PyObject *retired;
    Py_BEGIN_CRITICAL_SECTION(self);
    retired = self->cache;
    FT_ATOMIC_STORE_PTR_RELEASE(self->cache, NULL);
    FT_ATOMIC_STORE_INT_RELAXED(self->cache_enabled, value);
    _Py_atomic_add_uint64(&self->cache_generation, 1);
    Py_END_CRITICAL_SECTION();
    _PyObject_XDecRefDelayed(retired);
    Py_RETURN_NONE;
}

static PyObject *
generic_cache_info(GenericObject *self, PyObject *Py_UNUSED(ignored))
{
    int enabled;
    Py_ssize_t size;
    unsigned long long hits, misses;
    Py_BEGIN_CRITICAL_SECTION(self);
    enabled = self->cache_enabled;
    size = table_count(self->cache, CACHE_STRIDE);
    hits = _Py_atomic_load_uint64_relaxed(&self->cache_hits);
    misses = _Py_atomic_load_uint64_relaxed(&self->cache_misses);
    Py_END_CRITICAL_SECTION();
    return Py_BuildValue("{s:i,s:n,s:i,s:i,s:K,s:K}",
                        "enabled", enabled, "size", size,
                        "capacity", RESOLUTION_CACHE_SIZE,
                        "max_registrations", RESOLUTION_MAX_REGISTRATIONS,
                        "hits", hits, "misses", misses);
}

static PyMethodDef generic_methods[] = {
    {"register", _PyCFunction_CAST(generic_register), METH_VARARGS | METH_KEYWORDS, NULL},
    {"dispatch", (PyCFunction)generic_dispatch, METH_O, NULL},
    {"_set_cache_enabled", (PyCFunction)generic_set_cache_enabled, METH_O, NULL},
    {"_cache_info", (PyCFunction)generic_cache_info, METH_NOARGS, NULL},
    {"impl", (PyCFunction)generic_impl, METH_O, NULL},
    {"override", (PyCFunction)generic_override, METH_VARARGS, NULL},
    {NULL}
};

static PyGetSetDef generic_getset[] = {
    {"fallback", (getter)generic_get_fallback, NULL, NULL, NULL},
    {"version", (getter)generic_get_version, NULL, NULL, NULL},
    {"pair", (getter)generic_get_pair, NULL, NULL, NULL},
    {"__name__", (getter)generic_get_name, NULL, NULL, NULL},
    {NULL}
};

static PyMemberDef generic_members[] = {
    {"__vectorcalloffset__", Py_T_PYSSIZET, offsetof(GenericObject, vectorcall), Py_READONLY},
    {NULL}
};

/* Private: count cache hits for tests and diagnostics. Off by default, since
   counting writes shared memory on the lock-free read path. */
static PyObject *
set_dispatch_stats(PyObject *module, PyObject *enabled)
{
    int value = PyObject_IsTrue(enabled);
    if (value < 0) {
        return NULL;
    }
    FT_ATOMIC_STORE_INT_RELAXED(dispatch_stats, value);
    Py_RETURN_NONE;
}

static PyMethodDef generic_module_methods[] = {
    {"_set_dispatch_stats", set_dispatch_stats, METH_O, NULL},
    {NULL}
};

int
_PyGrail_InitGenerics(PyObject *module)
{
    if (PyModule_AddFunctions(module, generic_module_methods) < 0) {
        return -1;
    }
    PyType_Slot slots[] = {
        {Py_tp_new, generic_new},
        {Py_tp_dealloc, generic_dealloc},
        {Py_tp_traverse, generic_traverse},
        {Py_tp_clear, generic_clear},
        {Py_tp_call, PyVectorcall_Call},
        {Py_tp_methods, generic_methods},
        {Py_tp_members, generic_members},
        {Py_tp_getset, generic_getset},
        {0, NULL}
    };
    PyType_Spec spec = {
        .name = "_grail.Generic",
        .basicsize = sizeof(GenericObject),
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                 Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_IMMUTABLETYPE,
        .slots = slots,
    };
    GrailState *state = PyModule_GetState(module);
    state->generic_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &spec, NULL);
    if (state->generic_type == NULL) {
        return -1;
    }
    return PyModule_AddObjectRef(module, "Generic", (PyObject *)state->generic_type);
}
