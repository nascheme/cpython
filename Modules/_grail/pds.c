/* Copied from grail-compat src/grail/_c/ by tools/sync_to_proto.py; do not edit here. */
/* Grail persistent maps and sets over a copy of CPython's immutable HAMT
 * (grail_hamt.c, the trie behind contextvars).
 * Builders and bulk construction update nodes in place while the builder's
 * root exclusively owns them (GrailHamt_AssocInPlace); everything reachable from
 * a published root is immutable.
 */
#include "grail_compat.h"
#include "grail_hamt.h"

/* The prototype builds this file into the interpreter as _grail_pds;
   grail-compat builds it as grail._pds_native. */
#ifdef Py_BUILD_CORE
#  define PDS_MODULE_NAME "_grail_pds"
#  define PDS_MODULE_INIT PyInit__grail_pds
#else
#  define PDS_MODULE_NAME "grail._pds_native"
#  define PDS_MODULE_INIT PyInit__pds_native
#endif

#include <stdint.h>

typedef struct {
    GrailHamtState hamt;         /* must be first: see grail_hamt.h */
    PyTypeObject *map_type;
    PyTypeObject *set_type;
    PyTypeObject *builder_type;
    PyTypeObject *iterator_type;
    PyObject *mapping_abc;
    PyObject *set_abc;
    PyObject *keys_view;         /* collections.abc views with native __iter__ */
    PyObject *items_view;
    PyObject *values_view;
    PyObject *view_bases;        /* (KeysView, ValuesView, ItemsView) */
} PDSState;

typedef struct {
    PyObject_HEAD
    GrailHamtObject *root;
    Py_hash_t hash;              /* -1 until computed; relaxed atomic. */
    int is_set;
} Persistent;

enum { OPEN, SEALED, FINISHED, ABORTED };
typedef struct {
    PyObject_HEAD
    GrailHamtObject *root;
    PyTypeObject *result_type;
    uint64_t owner;
    int is_set;
    int status;
    int busy;
    int entered;
} Builder;

static PDSState *
pds_state(PyObject *self)
{
    return PyType_GetModuleState(Py_TYPE(self));
}

static void persistent_dealloc(PyObject *op);

static PyObject *
wrap_root(PyTypeObject *type, GrailHamtObject *root, int is_set)
{
    /* Steals root, including on failure. */
    if (root == NULL) {
        return NULL;
    }
    Persistent *self = (Persistent *)type->tp_alloc(type, 0);
    if (self == NULL) {
        Py_DECREF(root);
        return NULL;
    }
    self->root = root;
    self->hash = -1;
    self->is_set = is_set;
    return (PyObject *)self;
}

static inline int
is_persistent(PyObject *op)
{
    return Py_TYPE(op)->tp_dealloc == persistent_dealloc;
}

static int
persistent_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(((Persistent *)op)->root);
    return 0;
}

static void
persistent_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    Py_XDECREF(((Persistent *)op)->root);
    type->tp_free(op);
    GRAIL_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static int
assoc_root(GrailHamtObject **root, PyObject *key, PyObject *value)
{
    /* In place when *root is ours alone, as in builders and construction. */
    return GrailHamt_AssocInPlace(root, key, value);
}

static int
without_root(GrailHamtObject **root, PyObject *key)
{
    GrailHamtObject *next = GrailHamt_Without(*root, key);
    if (next == NULL) {
        return -1;
    }
    Py_SETREF(*root, next);
    return 0;
}

/* Entries collected from a source, as owned references. */
typedef struct {
    PyObject **keys;
    PyObject **vals;
    Py_ssize_t n;
    Py_ssize_t cap;
} Entries;

static void
entries_clear(Entries *e)
{
    for (Py_ssize_t i = 0; i < e->n; i++) {
        Py_DECREF(e->keys[i]);
        Py_DECREF(e->vals[i]);
    }
    PyMem_Free(e->keys);
    PyMem_Free(e->vals);
    e->keys = e->vals = NULL;
    e->n = e->cap = 0;
}

static int
entries_reserve(Entries *e, Py_ssize_t extra)
{
    if (e->n + extra <= e->cap) {
        return 0;
    }
    Py_ssize_t cap = Py_MAX(e->n + extra, e->cap < 8 ? 8 : e->cap + e->cap / 2);
    PyObject **keys = PyMem_Resize(e->keys, PyObject *, cap);
    if (keys == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    e->keys = keys;
    PyObject **vals = PyMem_Resize(e->vals, PyObject *, cap);
    if (vals == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    e->vals = vals;
    e->cap = cap;
    return 0;
}

/* Append, stealing key and value (also on failure). */
static int
entries_add(Entries *e, PyObject *key, PyObject *value)
{
    if (entries_reserve(e, 1) < 0) {
        Py_DECREF(key);
        Py_DECREF(value);
        return -1;
    }
    e->keys[e->n] = key;
    e->vals[e->n] = value;
    e->n++;
    return 0;
}

/* Collect the entries of an exact dict or frozendict. Only references are
 * taken while the dict is locked, so no Python code runs and the lock is
 * held briefly, unlike inserting (which hashes and compares keys). */
static int
collect_dict(Entries *e, PyObject *dict, int is_set)
{
    int rc = 0;
    Py_BEGIN_CRITICAL_SECTION(dict);
    rc = entries_reserve(e, PyDict_GET_SIZE(dict));
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (rc == 0 && PyDict_Next(dict, &pos, &key, &value)) {
        rc = entries_add(e, Py_NewRef(key), Py_NewRef(is_set ? Py_None : value));
    }
    Py_END_CRITICAL_SECTION();
    return rc;
}

/* Collect the entries of another persistent map or set (keys for a set). */
static int
collect_persistent(Entries *e, Persistent *other, int is_set)
{
    if (entries_reserve(e, GrailHamt_Len(other->root)) < 0) {
        return -1;
    }
    /* other is immutable and kept alive by the caller. */
    GrailHamtIteratorState it;
    PyObject *key, *value;
    GrailHamt_IteratorInit(&it, other->root);
    while (GrailHamt_IteratorNext(&it, &key, &value)) {
        if (entries_add(e, Py_NewRef(key), Py_NewRef(is_set ? Py_None : value)) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
collect_iterable(Entries *e, PyObject *source, int is_set)
{
    PyObject *iterable;
    if (!is_set) {
        /* Snapshot mapping items using its own public protocol. */
        int mapping = PyObject_HasAttrStringWithError(source, "keys");
        if (mapping < 0) {
            return -1;
        }
        iterable = mapping ? PyMapping_Items(source) : Py_NewRef(source);
    }
    else {
        iterable = Py_NewRef(source);
    }
    if (iterable == NULL) {
        return -1;
    }
    PyObject *it = PyObject_GetIter(iterable);
    Py_DECREF(iterable);
    if (it == NULL) {
        return -1;
    }
    PyObject *item;
    int rc = 0;
    while (rc == 0 && (item = PyIter_Next(it)) != NULL) {
        if (is_set) {
            rc = entries_add(e, item, Py_None);
            continue;
        }
        PyObject *pair = PySequence_Tuple(item);
        Py_DECREF(item);
        if (pair == NULL) {
            rc = -1;
        }
        else if (PyTuple_GET_SIZE(pair) != 2) {
            PyErr_SetString(PyExc_ValueError, "map entries must have length two");
            rc = -1;
        }
        else {
            rc = entries_add(e, Py_NewRef(PyTuple_GET_ITEM(pair, 0)),
                             Py_NewRef(PyTuple_GET_ITEM(pair, 1)));
        }
        Py_XDECREF(pair);
    }
    Py_DECREF(it);
    return rc < 0 || PyErr_Occurred() ? -1 : 0;
}

/* Add source's entries to *root. An empty root is built bottom-up in one
 * pass; otherwise entries are inserted in place. */
static int
add_source(GrailHamtObject **root, PyObject *source, int is_set)
{
    int empty = GrailHamt_Len(*root) == 0;
    if (is_persistent(source) && ((Persistent *)source)->is_set == is_set && empty) {
        Py_SETREF(*root, (GrailHamtObject *)Py_NewRef(((Persistent *)source)->root));
        return 0;
    }
    Entries e = {0};
    int rc;
    if (is_persistent(source) && (is_set || !((Persistent *)source)->is_set)) {
        rc = collect_persistent(&e, (Persistent *)source, is_set);
    }
    else if (PyAnyDict_CheckExact(source)) {
        rc = collect_dict(&e, source, is_set);
    }
    else {
        rc = collect_iterable(&e, source, is_set);
    }
    if (rc == 0 && empty) {
        GrailHamtObject *built = GrailHamt_FromItems(GrailHamt_GetState(*root), e.keys, e.vals, e.n);
        if (built == NULL) {
            rc = -1;
        }
        else {
            Py_SETREF(*root, built);
        }
    }
    else if (rc == 0) {
        for (Py_ssize_t i = 0; i < e.n && rc == 0; i++) {
            rc = assoc_root(root, e.keys[i], e.vals[i]);
        }
    }
    entries_clear(&e);
    return rc;
}

static PyObject *
persistent_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *source = NULL;
    static char *keywords[] = {"source", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", keywords, &source)) {
        return NULL;
    }
    PDSState *state = PyType_GetModuleState(type);
    int is_set = type == state->set_type;
    if (source != NULL && Py_IS_TYPE(source, type)) {
        return Py_NewRef(source);
    }
    GrailHamtObject *root = GrailHamt_New(&state->hamt);
    if (root == NULL) {
        return NULL;
    }
    if (source != NULL && add_source(&root, source, is_set) < 0) {
        Py_DECREF(root);
        return NULL;
    }
    return wrap_root(type, root, is_set);
}

static Py_ssize_t
persistent_len(PyObject *op)
{
    return GrailHamt_Len(((Persistent *)op)->root);
}

static int
persistent_contains(PyObject *op, PyObject *key)
{
    PyObject *value;
    return GrailHamt_Find(((Persistent *)op)->root, key, &value);
}

static PyObject *
lookup(GrailHamtObject *root, PyObject *key, PyObject *fallback)
{
    PyObject *value;
    int found = GrailHamt_Find(root, key, &value);
    if (found < 0) {
        return NULL;
    }
    if (found) {
        return Py_NewRef(value);
    }
    if (fallback != NULL) {
        return Py_NewRef(fallback);
    }
    PyErr_SetObject(PyExc_KeyError, key);
    return NULL;
}

static PyObject *
map_subscript(PyObject *op, PyObject *key)
{
    return lookup(((Persistent *)op)->root, key, NULL);
}

static PyObject *
map_get(PyObject *op, PyObject *args)
{
    PyObject *key, *fallback = Py_None;
    if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &fallback)) {
        return NULL;
    }
    return lookup(((Persistent *)op)->root, key, fallback);
}

enum { ITER_KEYS, ITER_VALUES, ITER_ITEMS };

typedef struct {
    PyObject_HEAD
    GrailHamtObject *root;          /* NULL once exhausted. */
    GrailHamtIteratorState state;   /* Borrowed nodes of root. */
    int kind;
} PDSIterator;

static int
iterator_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(((PDSIterator *)op)->root);
    return 0;
}

static void
iterator_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    Py_XDECREF(((PDSIterator *)op)->root);
    type->tp_free(op);
    GRAIL_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static PyObject *
iterator_next(PyObject *op)
{
    PDSIterator *it = (PDSIterator *)op;
    PyObject *key = NULL, *value = NULL, *exhausted = NULL;
    /* Advancement state is shared if the iterator is. Take strong references
     * inside the critical section; allocate and release outside it. */
    int found = 0;
    Py_BEGIN_CRITICAL_SECTION(op);
    if (it->root != NULL) {
        found = GrailHamt_IteratorNext(&it->state, &key, &value);
        if (!found) {
            exhausted = (PyObject *)it->root;
            it->root = NULL;
        }
        /* Only acquire what is returned: shared refcounts are contended. */
        else if (it->kind == ITER_KEYS) {
            Py_INCREF(key);
        }
        else if (it->kind == ITER_VALUES) {
            Py_INCREF(value);
        }
        else {
            Py_INCREF(key);
            Py_INCREF(value);
        }
    }
    Py_END_CRITICAL_SECTION();
    Py_XDECREF(exhausted);
    if (!found) {
        return NULL;
    }
    switch (it->kind) {
    case ITER_KEYS:
        return key;
    case ITER_VALUES:
        return value;
    default:
        return grail_tuple_from_pair_steal(key, value);
    }
}
static PyObject *
new_iterator(PyObject *op, int kind)
{
    PDSState *state = pds_state(op);
    PDSIterator *it = (PDSIterator *)state->iterator_type->tp_alloc(state->iterator_type, 0);
    if (it == NULL) {
        return NULL;
    }
    it->root = (GrailHamtObject *)Py_NewRef(((Persistent *)op)->root);
    GrailHamt_IteratorInit(&it->state, it->root);
    it->kind = kind;
    return (PyObject *)it;
}

static PyObject *
persistent_iter(PyObject *op)
{
    return new_iterator(op, ITER_KEYS);
}
static PyObject *
map_set(PyObject *op, PyObject *args)
{
    PyObject *key, *value;
    if (!PyArg_UnpackTuple(args, "set", 2, 2, &key, &value)) {
        return NULL;
    }
    return wrap_root(Py_TYPE(op), GrailHamt_Assoc(((Persistent *)op)->root, key, value), 0);
}

static PyObject *
set_add(PyObject *op, PyObject *key)
{
    return wrap_root(Py_TYPE(op), GrailHamt_Assoc(((Persistent *)op)->root, key, Py_None), 1);
}

static PyObject *
persistent_remove(PyObject *op, PyObject *key)
{
    Persistent *self = (Persistent *)op;
    if (!self->is_set) {
        int found = persistent_contains(op, key);
        if (found < 0) {
            return NULL;
        }
        if (!found) {
            PyErr_SetObject(PyExc_KeyError, key);
            return NULL;
        }
    }
    return wrap_root(Py_TYPE(op), GrailHamt_Without(self->root, key), self->is_set);
}

/* Merge one entry for update_with into *root (a private builder root). */
static int
merge_entry(GrailHamtObject **root, PyObject *fn, PyObject *key, PyObject *value)
{
    PyObject *current;
    int found = GrailHamt_Find(*root, key, &current);
    if (found < 0) {
        return -1;
    }
    if (!found) {
        return assoc_root(root, key, value);
    }
    /* fn may run arbitrary code; nothing else can reach the private root,
     * but hold current in case fn's side effects drop other references. */
    Py_INCREF(current);
    PyObject *merged = PyObject_CallFunctionObjArgs(fn, current, value, NULL);
    Py_DECREF(current);
    if (merged == NULL) {
        return -1;
    }
    int rc = assoc_root(root, key, merged);
    Py_DECREF(merged);
    return rc;
}

static PyObject *
map_update_with(PyObject *op, PyObject *args)
{
    PyObject *other, *fn;
    if (!PyArg_UnpackTuple(args, "update_with", 2, 2, &other, &fn)) {
        return NULL;
    }
    if (!PyCallable_Check(fn)) {
        PyErr_SetString(PyExc_TypeError, "update_with requires a callable");
        return NULL;
    }
    Persistent *self = (Persistent *)op;
    if (Py_IS_TYPE(other, Py_TYPE(op)) && GrailHamt_Len(self->root) == 0) {
        return Py_NewRef(other);
    }
    GrailHamtObject *root = (GrailHamtObject *)Py_NewRef(self->root);
    int rc = 0;
    if (Py_IS_TYPE(other, Py_TYPE(op))) {
        /* other is immutable and kept alive by the caller. */
        GrailHamtIteratorState it;
        PyObject *key, *value;
        GrailHamt_IteratorInit(&it, ((Persistent *)other)->root);
        while (rc == 0 && GrailHamt_IteratorNext(&it, &key, &value)) {
            rc = merge_entry(&root, fn, key, value);
        }
    }
    else {
        /* Any mapping or iterable of pairs, snapshotted like the constructor. */
        GrailHamtObject *source = GrailHamt_New(GrailHamt_GetState(self->root));
        rc = source == NULL ? -1 : add_source(&source, other, 0);
        if (rc == 0) {
            GrailHamtIteratorState it;
            PyObject *key, *value;
            GrailHamt_IteratorInit(&it, source);
            while (rc == 0 && GrailHamt_IteratorNext(&it, &key, &value)) {
                rc = merge_entry(&root, fn, key, value);
            }
        }
        Py_XDECREF(source);
    }
    if (rc < 0) {
        Py_DECREF(root);
        return NULL;
    }
    return wrap_root(Py_TYPE(op), root, 0);
}

static PyObject *
map_keys(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallOneArg(pds_state(op)->keys_view, op);
}

static PyObject *
map_items(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallOneArg(pds_state(op)->items_view, op);
}

static PyObject *
map_values(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallOneArg(pds_state(op)->values_view, op);
}

static PyObject *
map_dict(PyObject *op)
{
    PyObject *dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }
    /* The map is immutable and op keeps it alive: borrowed entries suffice. */
    GrailHamtIteratorState it;
    PyObject *key, *value;
    GrailHamt_IteratorInit(&it, ((Persistent *)op)->root);
    while (GrailHamt_IteratorNext(&it, &key, &value)) {
        if (PyDict_SetItem(dict, key, value) < 0) {
            Py_DECREF(dict);
            return NULL;
        }
    }
    return dict;
}
static PyObject *
persistent_repr(PyObject *op)
{
    int recursive = Py_ReprEnter(op);
    if (recursive != 0) {
        return recursive < 0 ? NULL : PyUnicode_FromString("...");
    }
    Persistent *self = (Persistent *)op;
    PyObject *contents = self->is_set ? PySequence_List(op) : map_dict(op);
    PyObject *result = contents == NULL ? NULL :
        PyUnicode_FromFormat("%s(%R)", self->is_set ? "PSet" : "PMap", contents);
    Py_XDECREF(contents);
    Py_ReprLeave(op);
    return result;
}

/* 1 if every key of a is in b. Both are immutable and kept alive. */
static int
keys_subset(Persistent *a, Persistent *b)
{
    if (GrailHamt_Len(a->root) > GrailHamt_Len(b->root)) {
        return 0;
    }
    GrailHamtIteratorState it;
    PyObject *key, *value, *found;
    GrailHamt_IteratorInit(&it, a->root);
    while (GrailHamt_IteratorNext(&it, &key, &value)) {
        int rc = GrailHamt_Find(b->root, key, &found);
        if (rc <= 0) {
            return rc;
        }
    }
    return 1;
}

/* Map equality without reference traffic. Both maps are immutable and kept
 * alive by the caller, so their keys and values stay alive through any
 * comparison callback; identical values need no comparison at all. */
static int
map_equal(Persistent *a, Persistent *b)
{
    if (a->root == b->root) {
        return 1;
    }
    if (GrailHamt_Len(a->root) != GrailHamt_Len(b->root)) {
        return 0;
    }
    GrailHamtIteratorState it;
    PyObject *key, *value, *other;
    GrailHamt_IteratorInit(&it, a->root);
    while (GrailHamt_IteratorNext(&it, &key, &value)) {
        int rc = GrailHamt_Find(b->root, key, &other);
        if (rc > 0 && value != other) {
            rc = PyObject_RichCompareBool(value, other, Py_EQ);
        }
        if (rc <= 0) {
            return rc;
        }
    }
    return 1;
}

static PyObject *
set_compare_native(Persistent *a, Persistent *b, int cmp)
{
    Py_ssize_t na = GrailHamt_Len(a->root), nb = GrailHamt_Len(b->root);
    int result;
    switch (cmp) {
    case Py_EQ:
    case Py_NE:
        result = na == nb && (a == b || keys_subset(a, b));
        break;
    case Py_LE:
        result = keys_subset(a, b);
        break;
    case Py_LT:
        result = na < nb && keys_subset(a, b);
        break;
    case Py_GE:
        result = keys_subset(b, a);
        break;
    default:
        result = na > nb && keys_subset(b, a);
        break;
    }
    if (result < 0) {
        return NULL;
    }
    return PyBool_FromLong(cmp == Py_NE ? !result : result);
}

static PyObject *
persistent_compare_impl(PyObject *op, PyObject *other, int cmp)
{
    Persistent *self = (Persistent *)op;
    if (!self->is_set && cmp != Py_EQ && cmp != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (Py_IS_TYPE(other, Py_TYPE(op))) {
        Persistent *that = (Persistent *)other;
        if (self->is_set) {
            return set_compare_native(self, that, cmp);
        }
        /* Equal maps have equal hashes; compare cached ones when present. */
        Py_hash_t h1 = GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(self->hash);
        Py_hash_t h2 = GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(that->hash);
        int equal = (h1 != -1 && h2 != -1 && h1 != h2) ? 0 : map_equal(self, that);
        return equal < 0 ? NULL : PyBool_FromLong(cmp == Py_EQ ? equal : !equal);
    }
    PDSState *state = pds_state(op);
    int supported = self->is_set ? PyAnySet_CheckExact(other) : PyAnyDict_CheckExact(other);
    if (!supported) {
        supported = PyObject_IsInstance(other, self->is_set ? state->set_abc : state->mapping_abc);
        if (supported < 0) {
            return NULL;
        }
        if (!supported) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    PyObject *left = self->is_set ? PyFrozenSet_New(op) : map_dict(op);
    PyObject *right = self->is_set ? PyFrozenSet_New(other) : PyObject_CallOneArg((PyObject *)&PyDict_Type, other);
    PyObject *result = left != NULL && right != NULL ? PyObject_RichCompare(left, right, cmp) : NULL;
    Py_XDECREF(left);
    Py_XDECREF(right);
    return result;
}
static PyObject *
persistent_compare(PyObject *op, PyObject *other, int cmp)
{
    if (Py_EnterRecursiveCall(" while comparing persistent containers")) {
        return NULL;
    }
    PyObject *result = persistent_compare_impl(op, other, cmp);
    Py_LeaveRecursiveCall();
    return result;
}

static Py_uhash_t
shuffle_bits(Py_uhash_t h)
{
    return ((h ^ 89869747UL) ^ (h << 16)) * 3644798167UL;
}

/* hash((key, value)) given hash(key), as frozendict computes it. */
static Py_hash_t
pair_hash(Py_hash_t key_hash, PyObject *value)
{
    Py_uhash_t acc = GRAIL_HASH_XXPRIME_5;
    Py_uhash_t lane = key_hash;
    acc += lane * GRAIL_HASH_XXPRIME_2;
    acc = GRAIL_HASH_XXROTATE(acc);
    acc *= GRAIL_HASH_XXPRIME_1;
    lane = PyObject_Hash(value);
    if (lane == (Py_uhash_t)-1) {
        return -1;
    }
    acc += lane * GRAIL_HASH_XXPRIME_2;
    acc = GRAIL_HASH_XXROTATE(acc);
    acc *= GRAIL_HASH_XXPRIME_1;
    acc += 2 ^ (GRAIL_HASH_XXPRIME_5 ^ 3527539UL);
    if (acc == (Py_uhash_t)-1) {
        return 1546275796;
    }
    return acc;
}

/* Equal to hash(frozenset(s)) for a set and hash(frozendict(m)) for a map,
 * without building either. */
static Py_hash_t
persistent_hash_impl(PyObject *op)
{
    Persistent *self = (Persistent *)op;
    /* The collection is immutable and op keeps it alive. */
    GrailHamtIteratorState it;
    PyObject *key, *value;
    Py_uhash_t hash = 0;
    GrailHamt_IteratorInit(&it, self->root);
    while (GrailHamt_IteratorNext(&it, &key, &value)) {
        Py_hash_t h = PyObject_Hash(key);
        if (h != -1 && !self->is_set) {
            h = pair_hash(h, value);
        }
        if (h == -1) {
            return -1;
        }
        hash ^= shuffle_bits(h);
    }
    hash ^= ((Py_uhash_t)GrailHamt_Len(self->root) + 1) * 1927868237UL;
    hash ^= (hash >> 11) ^ (hash >> 25);
    hash = hash * 69069U + 907133923UL;
    if (hash == (Py_uhash_t)-1) {
        hash = 590923713UL;
    }
    return (Py_hash_t)hash;
}
static Py_hash_t
persistent_hash(PyObject *op)
{
    Persistent *self = (Persistent *)op;
    /* Racing threads compute the same value; any of them may store it. */
    Py_hash_t result = GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(self->hash);
    if (result != -1) {
        return result;
    }
    if (Py_EnterRecursiveCall(" while hashing a persistent container")) {
        return -1;
    }
    result = persistent_hash_impl(op);
    Py_LeaveRecursiveCall();
    if (result != -1) {
        GRAIL_ATOMIC_STORE_SSIZE_RELAXED(self->hash, result);
    }
    return result;
}

static PyObject *
persistent_or(PyObject *left, PyObject *right)
{
    /* Slots also receive reflected calls with a foreign left operand. */
    int reflected = !is_persistent(left);
    PyObject *native = reflected ? right : left;
    if (!is_persistent(native)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    Persistent *self = (Persistent *)native;
    PyTypeObject *type = Py_TYPE(native);
    PyObject *foreign = reflected ? left : right;
    if (!self->is_set && !Py_IS_TYPE(foreign, type) && !PyAnyDict_CheckExact(foreign)) {
        int mapping = PyObject_IsInstance(foreign, pds_state(native)->mapping_abc);
        if (mapping < 0) {
            return NULL;
        }
        if (!mapping) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    if (!reflected && Py_IS_TYPE(right, type)) {
        /* Right-biased union of two native collections. */
        if (GrailHamt_Len(((Persistent *)right)->root) == 0) {
            return Py_NewRef(left);
        }
        if (GrailHamt_Len(self->root) == 0) {
            return Py_NewRef(right);
        }
    }
    GrailHamtObject *root = reflected ? GrailHamt_New(GrailHamt_GetState(self->root)) : (GrailHamtObject *)Py_NewRef(self->root);
    if (root == NULL) {
        return NULL;
    }
    if ((reflected && add_source(&root, left, self->is_set) < 0) ||
        add_source(&root, right, self->is_set) < 0) {
        Py_DECREF(root);
        return NULL;
    }
    return wrap_root(type, root, self->is_set);
}
/* &, - and ^ of two PSets, by membership tests against the tries. */
static PyObject *
set_algebra_native(Persistent *a, Persistent *b, int operation)
{
    PyTypeObject *type = Py_TYPE(a);
    Py_ssize_t na = GrailHamt_Len(a->root), nb = GrailHamt_Len(b->root);
    GrailHamtObject *root;
    Persistent *scan, *probe;
    int keep_found;
    if (operation == 0) {
        /* Intersection: scan the smaller set, as frozenset does. */
        if (na == 0 || (nb != 0 && a->root == b->root)) {
            return Py_NewRef(a);
        }
        root = GrailHamt_New(GrailHamt_GetState(a->root));
        scan = na <= nb ? a : b;
        probe = na <= nb ? b : a;
        keep_found = 1;
    }
    else if (operation == 1 && na <= nb) {
        /* Difference: rebuild from a small left operand. */
        root = GrailHamt_New(GrailHamt_GetState(a->root));
        scan = a;
        probe = b;
        keep_found = 0;
    }
    else {
        /* Difference from a large left operand, or symmetric difference:
         * edit a copy of a for each element of b. */
        if (nb == 0) {
            return Py_NewRef(a);
        }
        root = (GrailHamtObject *)Py_NewRef(a->root);
        GrailHamtIteratorState it;
        PyObject *key, *unused, *found;
        GrailHamt_IteratorInit(&it, b->root);
        while (GrailHamt_IteratorNext(&it, &key, &unused)) {
            int rc = GrailHamt_Find(a->root, key, &found);
            if (rc > 0) {
                rc = without_root(&root, key);
            }
            else if (rc == 0 && operation == 2) {
                rc = assoc_root(&root, key, Py_None);
            }
            if (rc < 0) {
                Py_DECREF(root);
                return NULL;
            }
        }
        return wrap_root(type, root, 1);
    }
    if (root == NULL) {
        return NULL;
    }
    GrailHamtIteratorState it;
    PyObject *key, *unused, *found;
    GrailHamt_IteratorInit(&it, scan->root);
    while (GrailHamt_IteratorNext(&it, &key, &unused)) {
        int rc = GrailHamt_Find(probe->root, key, &found);
        if (rc >= 0 && rc == keep_found) {
            rc = assoc_root(&root, key, Py_None);
        }
        if (rc < 0) {
            Py_DECREF(root);
            return NULL;
        }
    }
    return wrap_root(type, root, 1);
}

static PyObject *
set_algebra(PyObject *left, PyObject *right, int operation)
{
    PyObject *native = is_persistent(left) ? left : right;
    if (!is_persistent(native) || !((Persistent *)native)->is_set) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (Py_IS_TYPE(left, Py_TYPE(right))) {
        return set_algebra_native((Persistent *)left, (Persistent *)right, operation);
    }
    PyObject *a = PyFrozenSet_New(left);
    PyObject *b = PyFrozenSet_New(right);
    PyObject *result = NULL;
    if (a != NULL && b != NULL) {
        PyObject *temporary = operation == 0 ? PyNumber_And(a, b) :
                              operation == 1 ? PyNumber_Subtract(a, b) : PyNumber_Xor(a, b);
        if (temporary != NULL) {
            result = PyObject_CallOneArg((PyObject *)Py_TYPE(native), temporary);
            Py_DECREF(temporary);
        }
    }
    Py_XDECREF(a);
    Py_XDECREF(b);
    return result;
}
static PyObject *set_and(PyObject *a, PyObject *b) { return set_algebra(a, b, 0); }
static PyObject *set_subtract(PyObject *a, PyObject *b) { return set_algebra(a, b, 1); }
static PyObject *set_xor(PyObject *a, PyObject *b) { return set_algebra(a, b, 2); }

static PyObject *
persistent_copy(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return Py_NewRef(op);
}

/* True if copy.deepcopy(value) returns value itself: exact atomic builtins
 * (copy._deepcopy_atomic) and exact tuples of them (copy._deepcopy_tuple).
 * Checking these natively avoids a Python-level walk of every shared tuple,
 * which copy.deepcopy never memoizes because it is its own copy. */
static int
deepcopy_is_identity(PyObject *value, int depth)
{
    PyTypeObject *type = Py_TYPE(value);
    if (value == Py_None || value == Py_Ellipsis || value == Py_NotImplemented ||
        type == &PyBool_Type || type == &PyLong_Type || type == &PyFloat_Type ||
        type == &PyComplex_Type || type == &PyUnicode_Type || type == &PyBytes_Type) {
        return 1;
    }
    if (type != &PyTuple_Type || depth <= 0) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(value); i++) {
        if (!deepcopy_is_identity(PyTuple_GET_ITEM(value, i), depth - 1)) {
            return 0;
        }
    }
    return 1;
}

/* Deep-copy one child, importing copy.deepcopy on first need.  Sets
 * *changed when the copy is a different object. */
static PyObject *
deepcopy_item(PyObject **deepcopy, PyObject *value, PyObject *memo, int *changed)
{
    if (deepcopy_is_identity(value, 16)) {
        return Py_NewRef(value);
    }
    if (*deepcopy == NULL) {
        *deepcopy = PyImport_ImportModuleAttrString("copy", "deepcopy");
        if (*deepcopy == NULL) {
            return NULL;
        }
    }
    PyObject *result = PyObject_CallFunctionObjArgs(*deepcopy, value, memo, NULL);
    if (result != NULL && result != value) {
        *changed = 1;
    }
    return result;
}

/* A mutable descendant may recursively finish copying op first and install
 * the result in the memo.  Return 1 with *result set if so. */
static int
deepcopy_memoized(PyObject *op, PyObject *memo, PyObject **result)
{
    PyObject *key = PyLong_FromVoidPtr(op);
    if (key == NULL) {
        return -1;
    }
    int found = PyDict_GetItemRef(memo, key, result);
    Py_DECREF(key);
    return found;
}

static PyObject *
persistent_deepcopy(PyObject *op, PyObject *memo)
{
    if (!PyDict_Check(memo)) {
        PyErr_SetString(PyExc_TypeError, "deepcopy memo must be a dict");
        return NULL;
    }
    int is_set = ((Persistent *)op)->is_set;
    PyObject *contents = is_set ? PySequence_List(op) : map_dict(op);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *deepcopy = NULL, *result = NULL;
    PyObject *copied = is_set ? PyList_New(0) : PyDict_New();
    int changed = 0;
    if (copied == NULL) {
        goto done;
    }
    if (is_set) {
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(contents); i++) {
            PyObject *item = deepcopy_item(&deepcopy, PyList_GET_ITEM(contents, i), memo, &changed);
            int rc = item == NULL ? -1 : PyList_Append(copied, item);
            Py_XDECREF(item);
            if (rc < 0) {
                goto done;
            }
        }
    }
    else {
        /* Keys before values, in order, as copy._deepcopy_dict does. */
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        while (PyDict_Next(contents, &pos, &key, &value)) {
            PyObject *k = deepcopy_item(&deepcopy, key, memo, &changed);
            PyObject *v = k == NULL ? NULL : deepcopy_item(&deepcopy, value, memo, &changed);
            int rc = v == NULL ? -1 : PyDict_SetItem(copied, k, v);
            Py_XDECREF(k);
            Py_XDECREF(v);
            if (rc < 0) {
                goto done;
            }
        }
    }
    if (deepcopy_memoized(op, memo, &result) == 0) {
        /* Unchanged children: like a tuple, the collection is its own copy. */
        result = changed ? PyObject_CallOneArg((PyObject *)Py_TYPE(op), copied) : Py_NewRef(op);
    }
done:
    Py_XDECREF(deepcopy);
    Py_XDECREF(copied);
    Py_DECREF(contents);
    return result;
}

static PyObject *
persistent_reduce(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyObject *contents = ((Persistent *)op)->is_set ? PySequence_List(op) : map_dict(op);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, contents);
    Py_DECREF(contents);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(2, Py_TYPE(op), args);
    Py_DECREF(args);
    return result;
}

static int
builder_traverse(PyObject *op, visitproc visit, void *arg)
{
    Builder *self = (Builder *)op;
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(self->root);
    Py_VISIT(self->result_type);
    return 0;
}

static int
builder_clear(PyObject *op)
{
    Builder *self = (Builder *)op;
    Py_CLEAR(self->root);
    Py_CLEAR(self->result_type);
    return 0;
}

static void
builder_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    builder_clear(op);
    type->tp_free(op);
    GRAIL_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static PyObject *
persistent_mutate(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PDSState *state = pds_state(op);
    Builder *builder = (Builder *)state->builder_type->tp_alloc(state->builder_type, 0);
    if (builder == NULL) {
        return NULL;
    }
    builder->root = (GrailHamtObject *)Py_NewRef(((Persistent *)op)->root);
    builder->result_type = (PyTypeObject *)Py_NewRef(Py_TYPE(op));
    builder->is_set = ((Persistent *)op)->is_set;
    builder->owner = PyThreadState_GetID(PyThreadState_Get());
    return (PyObject *)builder;
}

static int
builder_check(Builder *self, int allow_sealed)
{
    if (self->owner != PyThreadState_GetID(PyThreadState_Get())) {
        PyErr_SetString(PyExc_RuntimeError, "transient used from another thread");
        return -1;
    }
    if (self->busy) {
        PyErr_SetString(PyExc_RuntimeError, "reentrant transient operation");
        return -1;
    }
    if (self->status != OPEN && !(allow_sealed && self->status == SEALED)) {
        PyErr_SetString(PyExc_RuntimeError, "transient is no longer open");
        return -1;
    }
    return 0;
}

static PyObject *
builder_subscript(PyObject *op, PyObject *key)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    if (self->is_set) {
        PyErr_SetString(PyExc_TypeError, "set transients are not subscriptable");
        return NULL;
    }
    self->busy = 1;
    PyObject *value = lookup(self->root, key, NULL);
    self->busy = 0;
    return value;
}

static int
builder_assign(PyObject *op, PyObject *key, PyObject *value)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return -1;
    }
    if (self->is_set) {
        PyErr_SetString(PyExc_TypeError, "use add/discard on a set transient");
        return -1;
    }
    self->busy = 1;
    int rc = -1;
    if (value != NULL) {
        rc = assoc_root(&self->root, key, value);
    }
    else {
        PyObject *unused;
        int found = GrailHamt_Find(self->root, key, &unused);
        if (found == 0) {
            PyErr_SetObject(PyExc_KeyError, key);
        }
        else if (found > 0) {
            rc = without_root(&self->root, key);
        }
    }
    self->busy = 0;
    return rc;
}

static Py_ssize_t
builder_len(PyObject *op)
{
    Builder *self = (Builder *)op;
    return builder_check(self, 0) < 0 ? -1 : GrailHamt_Len(self->root);
}

static int
builder_contains(PyObject *op, PyObject *key)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return -1;
    }
    self->busy = 1;
    PyObject *unused;
    int found = GrailHamt_Find(self->root, key, &unused);
    self->busy = 0;
    return found;
}

static PyObject *
builder_set_change(PyObject *op, PyObject *key, int remove)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    if (!self->is_set) {
        PyErr_SetString(PyExc_TypeError, "add/discard require a set transient");
        return NULL;
    }
    self->busy = 1;
    int rc = remove ? without_root(&self->root, key) : assoc_root(&self->root, key, Py_None);
    self->busy = 0;
    if (rc < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *builder_add(PyObject *op, PyObject *key) { return builder_set_change(op, key, 0); }
static PyObject *builder_discard(PyObject *op, PyObject *key) { return builder_set_change(op, key, 1); }

static PyObject *
builder_finish(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 1) < 0) {
        return NULL;
    }
    self->busy = 1;
    PyObject *result = wrap_root(self->result_type, (GrailHamtObject *)Py_NewRef(self->root), self->is_set);
    if (result == NULL) {
        self->busy = 0;
        return NULL;
    }
    self->status = FINISHED;
    Py_CLEAR(self->root);
    self->busy = 0;
    return result;
}

static PyObject *
builder_enter(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    if (self->entered) {
        PyErr_SetString(PyExc_RuntimeError, "transient context already entered");
        return NULL;
    }
    self->entered = 1;
    return Py_NewRef(op);
}

static PyObject *
builder_exit(PyObject *op, PyObject *args)
{
    Builder *self = (Builder *)op;
    PyObject *exc, *value, *tb;
    if (!PyArg_UnpackTuple(args, "__exit__", 3, 3, &exc, &value, &tb)) {
        return NULL;
    }
    if (self->owner != PyThreadState_GetID(PyThreadState_Get()) || self->busy || !self->entered) {
        PyErr_SetString(PyExc_RuntimeError, "invalid transient context exit");
        return NULL;
    }
    self->entered = 0;
    if (self->status == OPEN) {
        self->status = exc == Py_None ? SEALED : ABORTED;
        if (self->status == ABORTED) {
            Py_CLEAR(self->root);
        }
    }
    Py_RETURN_FALSE;
}

static PyMethodDef map_methods[] = {
    {"get", map_get, METH_VARARGS, NULL},
    {"set", map_set, METH_VARARGS, NULL},
    {"delete", persistent_remove, METH_O, NULL},
    {"update_with", map_update_with, METH_VARARGS,
     "update_with(other, fn) -> map with every key of both; where a key is in\n"
     "both, its value is fn(self[k], other[k]). fn should be pure."},
    {"keys", map_keys, METH_NOARGS, NULL},
    {"items", map_items, METH_NOARGS, NULL},
    {"values", map_values, METH_NOARGS, NULL},
    {"mutate", persistent_mutate, METH_NOARGS, NULL},
    {"__copy__", persistent_copy, METH_NOARGS, NULL},
    {"__deepcopy__", persistent_deepcopy, METH_O, NULL},
    {"__reduce__", persistent_reduce, METH_NOARGS, NULL},
    {"__class_getitem__", Py_GenericAlias, METH_O | METH_CLASS, NULL},
    {NULL}
};

static PyMethodDef set_methods[] = {
    {"add", set_add, METH_O, NULL},
    {"discard", persistent_remove, METH_O, NULL},
    {"mutate", persistent_mutate, METH_NOARGS, NULL},
    {"__copy__", persistent_copy, METH_NOARGS, NULL},
    {"__deepcopy__", persistent_deepcopy, METH_O, NULL},
    {"__reduce__", persistent_reduce, METH_NOARGS, NULL},
    {"__class_getitem__", Py_GenericAlias, METH_O | METH_CLASS, NULL},
    {NULL}
};

static PyMethodDef builder_methods[] = {
    {"add", builder_add, METH_O, NULL},
    {"discard", builder_discard, METH_O, NULL},
    {"finish", builder_finish, METH_NOARGS, NULL},
    {"__enter__", builder_enter, METH_NOARGS, NULL},
    {"__exit__", builder_exit, METH_VARARGS, NULL},
    {NULL}
};

#define COMMON_SLOTS \
    {Py_tp_new, persistent_new}, \
    {Py_tp_dealloc, persistent_dealloc}, \
    {Py_tp_traverse, persistent_traverse}, \
    {Py_tp_repr, persistent_repr}, \
    {Py_tp_iter, persistent_iter}, \
    {Py_tp_hash, persistent_hash}, \
    {Py_tp_richcompare, persistent_compare}, \
    {Py_sq_contains, persistent_contains}, \
    {Py_nb_or, persistent_or}

static PyType_Slot map_slots[] = {
    COMMON_SLOTS,
    {Py_tp_methods, map_methods},
    {Py_mp_length, persistent_len},
    {Py_mp_subscript, map_subscript},
    {0}
};
static PyType_Slot set_slots[] = {
    COMMON_SLOTS,
    {Py_tp_methods, set_methods},
    {Py_sq_length, persistent_len},
    {Py_nb_and, set_and},
    {Py_nb_subtract, set_subtract},
    {Py_nb_xor, set_xor},
    {0}
};
static PyType_Slot builder_slots[] = {
    {Py_tp_dealloc, builder_dealloc},
    {Py_tp_traverse, builder_traverse},
    {Py_tp_clear, builder_clear},
    {Py_tp_methods, builder_methods},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_mp_length, builder_len},
    {Py_mp_subscript, builder_subscript},
    {Py_mp_ass_subscript, builder_assign},
    {Py_sq_contains, builder_contains},
    {0}
};

static PyType_Slot iterator_slots[] = {
    {Py_tp_dealloc, iterator_dealloc},
    {Py_tp_traverse, iterator_traverse},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, iterator_next},
    {0}
};

#define PERSISTENT_FLAGS (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE)
static PyType_Spec map_spec = {"grail.PMap", sizeof(Persistent), 0, PERSISTENT_FLAGS | Py_TPFLAGS_MAPPING, map_slots};
static PyType_Spec set_spec = {"grail.PSet", sizeof(Persistent), 0, PERSISTENT_FLAGS, set_slots};
static PyType_Spec iterator_spec = {PDS_MODULE_NAME "._Iterator", sizeof(PDSIterator), 0,
    PERSISTENT_FLAGS | Py_TPFLAGS_DISALLOW_INSTANTIATION, iterator_slots};
static PyType_Spec builder_spec = {PDS_MODULE_NAME "._Transient", sizeof(Builder), 0,
    PERSISTENT_FLAGS | Py_TPFLAGS_DISALLOW_INSTANTIATION, builder_slots};

static int
pds_traverse(PyObject *module, visitproc visit, void *arg)
{
    PDSState *state = PyModule_GetState(module);
    if (GrailHamt_TraverseState(&state->hamt, visit, arg) < 0) {
        return -1;
    }
    Py_VISIT(state->map_type);
    Py_VISIT(state->set_type);
    Py_VISIT(state->builder_type);
    Py_VISIT(state->iterator_type);
    Py_VISIT(state->mapping_abc);
    Py_VISIT(state->set_abc);
    Py_VISIT(state->keys_view);
    Py_VISIT(state->items_view);
    Py_VISIT(state->values_view);
    Py_VISIT(state->view_bases);
    return 0;
}

static int
pds_clear(PyObject *module)
{
    PDSState *state = PyModule_GetState(module);
    GrailHamt_ClearState(&state->hamt);
    Py_CLEAR(state->map_type);
    Py_CLEAR(state->set_type);
    Py_CLEAR(state->builder_type);
    Py_CLEAR(state->iterator_type);
    Py_CLEAR(state->mapping_abc);
    Py_CLEAR(state->set_abc);
    Py_CLEAR(state->keys_view);
    Py_CLEAR(state->items_view);
    Py_CLEAR(state->values_view);
    Py_CLEAR(state->view_bases);
    return 0;
}

/* __iter__ for the mapping views: iterate the trie directly rather than
 * through the Python-level Mapping mixins (one lookup per item). */
static PyObject *
view_iter(PyObject *module, PyObject *view, int kind)
{
    PDSState *state = PyModule_GetState(module);
    PyObject *mapping = PyObject_GetAttrString(view, "_mapping");
    if (mapping == NULL) {
        return NULL;
    }
    PyObject *result;
    if (Py_IS_TYPE(mapping, state->map_type)) {
        result = new_iterator(mapping, kind);
    }
    else {
        /* A view constructed by hand over some other mapping. */
        PyObject *base_iter = PyObject_GetAttrString(
            PyTuple_GET_ITEM(state->view_bases, kind), "__iter__");
        result = base_iter == NULL ? NULL : PyObject_CallOneArg(base_iter, view);
        Py_XDECREF(base_iter);
    }
    Py_DECREF(mapping);
    return result;
}

static PyObject *keys_view_iter(PyObject *m, PyObject *v) { return view_iter(m, v, ITER_KEYS); }
static PyObject *values_view_iter(PyObject *m, PyObject *v) { return view_iter(m, v, ITER_VALUES); }
static PyObject *items_view_iter(PyObject *m, PyObject *v) { return view_iter(m, v, ITER_ITEMS); }

static PyMethodDef view_iter_defs[] = {
    {"__iter__", keys_view_iter, METH_O, NULL},
    {"__iter__", values_view_iter, METH_O, NULL},
    {"__iter__", items_view_iter, METH_O, NULL},
};

/* class <name>(<collections.abc view>): __slots__ = (); __iter__ = native */
static PyObject *
make_view(PyObject *module, int kind, const char *name)
{
    PDSState *state = PyModule_GetState(module);
    PyObject *base = PyTuple_GET_ITEM(state->view_bases, kind);
    PyObject *func = PyCFunction_NewEx(&view_iter_defs[kind], module, NULL);
    PyObject *method = func == NULL ? NULL : PyInstanceMethod_New(func);
    Py_XDECREF(func);
    if (method == NULL) {
        return NULL;
    }
    PyObject *slots = PyTuple_New(0);
    PyObject *ns = slots == NULL ? NULL :
        Py_BuildValue("{sOsOss}", "__iter__", method, "__slots__", slots, "__module__", "grail");
    Py_DECREF(method);
    Py_XDECREF(slots);
    if (ns == NULL) {
        return NULL;
    }
    PyObject *cls = PyObject_CallFunction((PyObject *)Py_TYPE(base), "s(O)O", name, base, ns);
    Py_DECREF(ns);
    return cls;
}

static int
pds_exec(PyObject *module)
{
    PDSState *state = PyModule_GetState(module);
    if (GrailHamt_InitState(module, &state->hamt) < 0) {
        return -1;
    }
    state->map_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &map_spec, NULL);
    state->set_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &set_spec, NULL);
    state->builder_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &builder_spec, NULL);
    state->iterator_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &iterator_spec, NULL);
    if (state->map_type == NULL || state->set_type == NULL || state->builder_type == NULL || state->iterator_type == NULL) {
        return -1;
    }
    PyObject *abc = PyImport_ImportModule("collections.abc");
    if (abc == NULL) {
        return -1;
    }
    state->mapping_abc = PyObject_GetAttrString(abc, "Mapping");
    state->set_abc = PyObject_GetAttrString(abc, "Set");
    PyObject *keys = PyObject_GetAttrString(abc, "KeysView");
    PyObject *values = PyObject_GetAttrString(abc, "ValuesView");
    PyObject *items = PyObject_GetAttrString(abc, "ItemsView");
    Py_DECREF(abc);
    if (keys != NULL && values != NULL && items != NULL) {
        state->view_bases = PyTuple_Pack(3, keys, values, items);
    }
    Py_XDECREF(keys);
    Py_XDECREF(values);
    Py_XDECREF(items);
    if (state->mapping_abc == NULL || state->set_abc == NULL || state->view_bases == NULL) {
        return -1;
    }
    state->keys_view = make_view(module, ITER_KEYS, "PMapKeysView");
    state->values_view = make_view(module, ITER_VALUES, "PMapValuesView");
    state->items_view = make_view(module, ITER_ITEMS, "PMapItemsView");
    if (state->keys_view == NULL || state->values_view == NULL || state->items_view == NULL) {
        return -1;
    }
    PyObject *registered = PyObject_CallMethod(state->mapping_abc, "register", "O", state->map_type);
    if (registered == NULL) {
        return -1;
    }
    Py_DECREF(registered);
    registered = PyObject_CallMethod(state->set_abc, "register", "O", state->set_type);
    if (registered == NULL) {
        return -1;
    }
    Py_DECREF(registered);
    if (PyModule_AddObjectRef(module, "PMap", (PyObject *)state->map_type) < 0 ||
        PyModule_AddObjectRef(module, "PSet", (PyObject *)state->set_type) < 0) {
        return -1;
    }
    return PyModule_AddStringConstant(module, "backend", "cpython-hamt-transient");
}

#ifdef Py_mod_abi
PyABIInfo_VAR(pds_abi);
#endif
static PyModuleDef_Slot pds_slots[] = {
#ifdef Py_mod_abi
    {Py_mod_abi, &pds_abi},
#endif
    {Py_mod_exec, pds_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};
static struct PyModuleDef pds_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = PDS_MODULE_NAME,
    .m_size = sizeof(PDSState),
    .m_slots = pds_slots,
    .m_traverse = pds_traverse,
    .m_clear = pds_clear,
};
PyMODINIT_FUNC
PDS_MODULE_INIT(void)
{
    return PyModuleDef_Init(&pds_module);
}
