/* Grail persistent vector baseline: immutable 32-way trie and short tail.
 * Tuple nodes are private implementation objects with real GC-visible ownership
 * edges. No user-visible backing list, manual node refcounts, or mutable caches.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "internal/pycore_long.h"    // _PyLong_IsCompact()
#include "internal/pycore_modsupport.h"  // _PyArg_CheckPositional()
#include "internal/pycore_object.h"  // _Py_DECREF_TYPE()
#include "internal/pycore_pyatomic_ft_wrappers.h"
#include "internal/pycore_tuple.h"   // _PyTuple_HASH_XXPRIME_1

#include <stdint.h>

#define BITS 5
#define WIDTH (1 << BITS)
#define MASK (WIDTH - 1)

typedef struct {
    PyObject_HEAD
    PyObject *root;              /* NULL, or tuple trie; shift 0 is a leaf. */
    PyObject *tail;              /* tuple of 1..32 items, or empty if count 0. */
    Py_ssize_t count;
    Py_hash_t hash;              /* -1 until computed; relaxed atomic. */
    unsigned int shift;
} Vector;

typedef struct {
    PyObject_HEAD
    Vector *vector;
    PyObject *leaf;              /* Borrowed; vector owns its entire lifetime. */
    Py_ssize_t index;
    Py_ssize_t block;
    int reverse;
} VectorIterator;

typedef struct {
    PyTypeObject *vector_type;
    PyTypeObject *iterator_type;
    PyTypeObject *builder_type;
} VectorState;

static PyObject *
vector_wrap(PyTypeObject *type, PyObject *root, PyObject *tail,
            Py_ssize_t count, unsigned int shift)
{
    /* Steal root and tail, including on failure. A NULL root is valid. */
    if (tail == NULL) {
        Py_XDECREF(root);
        return NULL;
    }
    Vector *self = (Vector *)type->tp_alloc(type, 0);
    if (self == NULL) {
        Py_XDECREF(root);
        Py_DECREF(tail);
        return NULL;
    }
    self->root = root;
    self->tail = tail;
    self->count = count;
    self->hash = -1;
    self->shift = shift;
    return (PyObject *)self;
}

static int
vector_traverse(PyObject *op, visitproc visit, void *arg)
{
    Vector *self = (Vector *)op;
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(self->root);
    Py_VISIT(self->tail);
    return 0;
}

static void
vector_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    Py_XDECREF(((Vector *)op)->root);
    Py_XDECREF(((Vector *)op)->tail);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

/* The trie leaf holding index, which must be below the tail. */
static PyObject *
trie_leaf(PyObject *node, unsigned int shift, Py_ssize_t index)
{
    for (; shift != 0; shift -= BITS) {
        node = PyTuple_GET_ITEM(node, ((size_t)index >> shift) & MASK);
    }
    return node;
}

static PyObject *
vector_leaf(Vector *self, Py_ssize_t index)
{
    assert(index >= 0 && index < self->count);
    if (index >= self->count - PyTuple_GET_SIZE(self->tail)) {
        return self->tail;
    }
    return trie_leaf(self->root, self->shift, index);
}

static PyObject *
vector_borrow(Vector *self, Py_ssize_t index)
{
    return PyTuple_GET_ITEM(vector_leaf(self, index), index & MASK);
}

/* Build once from a private immutable snapshot, in O(n), not repeated append.
 * shift is at most 60 on a 64-bit build, so child capacity fits Py_ssize_t. */
static PyObject *
build_tree(PyObject *source, Py_ssize_t start, Py_ssize_t count, unsigned int shift)
{
    if (shift == 0) {
        assert(count == WIDTH);
        return PyTuple_GetSlice(source, start, start + WIDTH);
    }
    Py_ssize_t capacity = (Py_ssize_t)((size_t)1 << shift);
    Py_ssize_t children = 1 + (count - 1) / capacity;
    assert(children <= WIDTH);
    PyObject *node = PyTuple_New(children);
    if (node == NULL) {
        return NULL;
    }
    Py_ssize_t offset = 0;
    for (Py_ssize_t i = 0; i < children; i++) {
        Py_ssize_t size = Py_MIN(capacity, count - offset);
        PyObject *child = build_tree(source, start + offset, size, shift - BITS);
        if (child == NULL) {
            Py_DECREF(node);
            return NULL;
        }
        PyTuple_SET_ITEM(node, i, child);
        offset += size;
    }
    return node;
}

static PyObject *
vector_from_tuple(PyTypeObject *type, PyObject *source)
{
    Py_ssize_t count = PyTuple_GET_SIZE(source);
    Py_ssize_t trunk = count == 0 ? 0 : ((count - 1) >> BITS) << BITS;
    unsigned int shift = 0;
    while ((size_t)(trunk >> BITS) > ((size_t)1 << shift)) {
        shift += BITS;
    }
    PyObject *root = trunk == 0 ? NULL : build_tree(source, 0, trunk, shift);
    if (trunk != 0 && root == NULL) {
        return NULL;
    }
    return vector_wrap(type, root, PyTuple_GetSlice(source, trunk, count), count, shift);
}

static Py_ssize_t
vector_len(PyObject *op)
{
    return ((Vector *)op)->count;
}

static PyObject *
vector_item(PyObject *op, Py_ssize_t index)
{
    Vector *self = (Vector *)op;
    if (index < 0 || index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "PVector index out of range");
        return NULL;
    }
    return Py_NewRef(vector_borrow(self, index));
}

/* A PVector index as a Py_ssize_t (not yet normalized); a small exact int
 * skips the __index__ protocol. */
static inline int
index_value(PyObject *key, Py_ssize_t *index)
{
    if (PyLong_CheckExact(key) && _PyLong_IsCompact((PyLongObject *)key)) {
        *index = _PyLong_CompactValue((PyLongObject *)key);
        return 0;
    }
    if (!PyIndex_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "PVector indices must be integers");
        return -1;
    }
    *index = PyNumber_AsSsize_t(key, PyExc_IndexError);
    return *index == -1 && PyErr_Occurred() ? -1 : 0;
}

static int
normalize_index(Vector *self, PyObject *key, Py_ssize_t *index)
{
    if (index_value(key, index) < 0) {
        return -1;
    }
    if (*index < 0) {
        *index += self->count;
    }
    if (*index < 0 || *index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "PVector index out of range");
        return -1;
    }
    return 0;
}

static PyObject *
vector_subscript(PyObject *op, PyObject *key)
{
    Vector *self = (Vector *)op;
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
            return NULL;
        }
        Py_ssize_t count = PySlice_AdjustIndices(self->count, &start, &stop, step);
        if (start == 0 && step == 1 && count == self->count) {
            return Py_NewRef(op);
        }
        PyObject *snapshot = PyTuple_New(count);
        if (snapshot == NULL) {
            return NULL;
        }
        PyObject *leaf = NULL;
        Py_ssize_t block = -1, index = start;
        for (Py_ssize_t i = 0; i < count; i++) {
            if ((index >> BITS) != block) {
                leaf = vector_leaf(self, index);
                block = index >> BITS;
            }
            PyTuple_SET_ITEM(snapshot, i, Py_NewRef(PyTuple_GET_ITEM(leaf, index & MASK)));
            /* Avoid overflow on the unused position after the last item. */
            if (i + 1 < count) {
                index += step;
            }
        }
        PyObject *result = vector_from_tuple(Py_TYPE(op), snapshot);
        Py_DECREF(snapshot);
        return result;
    }
    Py_ssize_t index;
    return normalize_index(self, key, &index) < 0 ? NULL : vector_item(op, index);
}

/* Copy an immutable node, optionally replacing/adding its final slot. */
static PyObject *
copy_node(PyObject *node, Py_ssize_t slot, PyObject *value)
{
    Py_ssize_t size = PyTuple_GET_SIZE(node);
    assert(slot >= 0 && slot <= size);
    PyObject *result = PyTuple_New(size + (slot == size));
    if (result == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(result); i++) {
        PyTuple_SET_ITEM(result, i, Py_NewRef(i == slot ? value : PyTuple_GET_ITEM(node, i)));
    }
    return result;
}

static PyObject *
set_path(PyObject *node, unsigned int shift, Py_ssize_t index, PyObject *value)
{
    Py_ssize_t slot = ((size_t)index >> shift) & MASK;
    if (shift == 0) {
        return copy_node(node, slot, value);
    }
    PyObject *child = set_path(PyTuple_GET_ITEM(node, slot), shift - BITS, index, value);
    if (child == NULL) {
        return NULL;
    }
    PyObject *result = copy_node(node, slot, child);
    Py_DECREF(child);
    return result;
}

static PyObject *
vector_set(PyObject *op, PyObject *const *args, Py_ssize_t nargs)
{
    Vector *self = (Vector *)op;
    if (!_PyArg_CheckPositional("set", nargs, 2, 2)) {
        return NULL;
    }
    PyObject *key = args[0], *value = args[1];
    Py_ssize_t index;
    if (normalize_index(self, key, &index) < 0) {
        return NULL;
    }
    if (index >= self->count - PyTuple_GET_SIZE(self->tail)) {
        return vector_wrap(Py_TYPE(op), Py_XNewRef(self->root),
                           copy_node(self->tail, index & MASK, value), self->count, self->shift);
    }
    PyObject *root = set_path(self->root, self->shift, index, value);
    if (root == NULL) {
        return NULL;
    }
    return vector_wrap(Py_TYPE(op), root, Py_NewRef(self->tail), self->count, self->shift);
}

static PyObject *
new_path(unsigned int shift, PyObject *leaf)
{
    if (shift == 0) {
        return Py_NewRef(leaf);
    }
    PyObject *child = new_path(shift - BITS, leaf);
    if (child == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(1, child);
    Py_DECREF(child);
    return result;
}

static PyObject *
push_leaf(PyObject *node, unsigned int shift, Py_ssize_t index, PyObject *leaf)
{
    assert(shift > 0);
    Py_ssize_t slot = ((size_t)index >> shift) & MASK;
    PyObject *child;
    if (slot == PyTuple_GET_SIZE(node)) {
        child = new_path(shift - BITS, leaf);
    }
    else {
        child = push_leaf(PyTuple_GET_ITEM(node, slot), shift - BITS, index, leaf);
    }
    if (child == NULL) {
        return NULL;
    }
    PyObject *result = copy_node(node, slot, child);
    Py_DECREF(child);
    return result;
}

static PyObject *
vector_append(PyObject *op, PyObject *value)
{
    Vector *self = (Vector *)op;
    if (self->count == PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "PVector is too large");
        return NULL;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(self->tail);
    if (size < WIDTH) {
        return vector_wrap(Py_TYPE(op), Py_XNewRef(self->root),
                           copy_node(self->tail, size, value), self->count + 1, self->shift);
    }
    PyObject *root;
    unsigned int shift = self->shift;
    Py_ssize_t trunk = self->count - size;
    if (self->root == NULL) {
        root = Py_NewRef(self->tail);
    }
    else if ((size_t)(trunk >> BITS) == ((size_t)1 << shift)) {
        PyObject *path = new_path(shift, self->tail);
        if (path == NULL) {
            return NULL;
        }
        root = PyTuple_Pack(2, self->root, path);
        Py_DECREF(path);
        shift += BITS;
    }
    else {
        root = push_leaf(self->root, shift, trunk, self->tail);
    }
    if (root == NULL) {
        return NULL;
    }
    return vector_wrap(Py_TYPE(op), root, PyTuple_Pack(1, value), self->count + 1, shift);
}

/* NULL with no exception means the entire subtree was removed. */
static PyObject *
pop_leaf(PyObject *node, unsigned int shift, Py_ssize_t index)
{
    if (shift == 0) {
        return NULL;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(node);
    Py_ssize_t slot = ((size_t)index >> shift) & MASK;
    assert(slot == size - 1);
    PyObject *child = pop_leaf(PyTuple_GET_ITEM(node, slot), shift - BITS, index);
    if (child == NULL) {
        if (PyErr_Occurred() || size == 1) {
            return NULL;
        }
        return PyTuple_GetSlice(node, 0, size - 1);
    }
    PyObject *result = copy_node(node, slot, child);
    Py_DECREF(child);
    return result;
}

static PyObject *
vector_pop(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Vector *self = (Vector *)op;
    if (self->count == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from empty PVector");
        return NULL;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(self->tail);
    if (size > 1 || self->count == 1) {
        return vector_wrap(Py_TYPE(op), Py_XNewRef(self->root),
                           PyTuple_GetSlice(self->tail, 0, size - 1), self->count - 1, self->shift);
    }
    PyObject *tail = Py_NewRef(vector_leaf(self, self->count - 2));
    PyObject *root = pop_leaf(self->root, self->shift, self->count - 2);
    if (root == NULL && PyErr_Occurred()) {
        Py_DECREF(tail);
        return NULL;
    }
    unsigned int shift = self->shift;
    while (root != NULL && shift > 0 && PyTuple_GET_SIZE(root) == 1) {
        PyObject *child = Py_NewRef(PyTuple_GET_ITEM(root, 0));
        Py_SETREF(root, child);
        shift -= BITS;
    }
    if (root == NULL) {
        shift = 0;
    }
    return vector_wrap(Py_TYPE(op), root, tail, self->count - 1, shift);
}

/* Transient vectors.
 *
 * A builder owns a trie root that may share nodes with persistent vectors, and
 * keeps its tail in a C array. A node is edited in place only when the
 * builder's whole path to it is exclusively owned: the node and each ancestor
 * is uniquely referenced by the current thread. A uniquely referenced node
 * below a shared one is still reachable through every owner of that ancestor.
 * Otherwise the node is copied once and the copy, owned by the builder, is
 * edited from then on. finish() publishes the root and drops the builder's
 * reference, so published nodes are never edited.
 *
 * Nodes are private tuples. An in-place store resets the tuple's cached hash
 * and retracks it if the collector had untracked it, so tuple invariants hold
 * even for nodes reached through gc introspection.
 */
enum { OPEN, SEALED, FINISHED, ABORTED };

typedef struct {
    PyObject_HEAD
    PyObject *root;              /* NULL, or tuple trie as in Vector. */
    PyObject *tail[WIDTH];       /* Owned items [0, tail_len). */
    Py_ssize_t count;
    PyTypeObject *result_type;
    uint64_t owner;
    unsigned int shift;
    int tail_len;                /* 1..32 when count > 0, else 0. */
    int status;
    int busy;
    int entered;
} Builder;

static inline int
node_is_mine(int mut, PyObject *node)
{
    return mut && _PyObject_IsUniquelyReferenced(node);
}

/* Replace item i of an exclusively owned node; steals value. */
static void
node_store(PyObject *node, Py_ssize_t i, PyObject *value)
{
    PyObject *old = PyTuple_GET_ITEM(node, i);
    PyTuple_SET_ITEM(node, i, value);
    FT_ATOMIC_STORE_SSIZE_RELAXED(((PyTupleObject *)node)->ob_hash, -1);
    if (!PyObject_GC_IsTracked(node)) {
        PyObject_GC_Track(node);
    }
    Py_XDECREF(old);
}

/* Make *slot exclusively owned, copying it if needed. mut says whether the
 * slot itself lies in exclusively owned storage. */
static int
own_node(PyObject **slot, int mut)
{
    if (node_is_mine(mut, *slot)) {
        return 0;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(*slot);
    PyObject *copy = PyTuple_New(size);
    if (copy == NULL) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PyTuple_SET_ITEM(copy, i, Py_NewRef(PyTuple_GET_ITEM(*slot, i)));
    }
    Py_SETREF(*slot, copy);
    return 0;
}

static inline PyObject **
node_slot(PyObject *node, Py_ssize_t i)
{
    return &((PyTupleObject *)node)->ob_item[i];
}

static int
builder_set_path(PyObject **slot, unsigned int shift, Py_ssize_t index, PyObject *value)
{
    /* The builder's own root field is exclusively owned storage. */
    for (;;) {
        if (own_node(slot, 1) < 0) {
            return -1;
        }
        Py_ssize_t i = ((size_t)index >> shift) & MASK;
        if (shift == 0) {
            node_store(*slot, i, Py_NewRef(value));
            return 0;
        }
        slot = node_slot(*slot, i);
        shift -= BITS;
    }
}

/* Append a full leaf at trunk position index (stealing leaf). */
static int
builder_push_leaf(PyObject **slot, unsigned int shift, Py_ssize_t index, PyObject *leaf)
{
    for (;;) {
        assert(shift > 0);
        Py_ssize_t i = ((size_t)index >> shift) & MASK;
        Py_ssize_t size = PyTuple_GET_SIZE(*slot);
        if (i == size) {
            PyObject *path = new_path(shift - BITS, leaf);
            Py_DECREF(leaf);
            if (path == NULL) {
                return -1;
            }
            /* Tuples cannot grow safely in place: a failed _PyTuple_Resize
             * would free the node. Copy into a node one slot larger. */
            PyObject *grown = copy_node(*slot, size, path);
            Py_DECREF(path);
            if (grown == NULL) {
                return -1;
            }
            Py_SETREF(*slot, grown);
            return 0;
        }
        /* Only nodes along the path to an existing child are edited. */
        if (own_node(slot, 1) < 0) {
            Py_DECREF(leaf);
            return -1;
        }
        slot = node_slot(*slot, i);
        shift -= BITS;
    }
}

static PyObject *
builder_wrap_tail(Builder *self)
{
    PyObject *tail = PyTuple_New(self->tail_len);
    if (tail == NULL) {
        return NULL;
    }
    for (int i = 0; i < self->tail_len; i++) {
        PyTuple_SET_ITEM(tail, i, self->tail[i]);
        self->tail[i] = NULL;
    }
    self->tail_len = 0;
    return tail;
}

/* Append, stealing value (also on failure). */
static int
builder_append_steal(Builder *self, PyObject *value)
{
    if (self->count == PY_SSIZE_T_MAX) {
        Py_DECREF(value);
        PyErr_SetString(PyExc_OverflowError, "PVector is too large");
        return -1;
    }
    if (self->tail_len == WIDTH) {
        Py_ssize_t trunk = self->count - WIDTH;
        PyObject *leaf = builder_wrap_tail(self);
        if (leaf == NULL) {
            Py_DECREF(value);
            return -1;
        }
        if (self->root == NULL) {
            self->root = leaf;
        }
        else if ((size_t)(trunk >> BITS) == ((size_t)1 << self->shift)) {
            PyObject *path = new_path(self->shift, leaf);
            Py_DECREF(leaf);
            PyObject *root = path == NULL ? NULL : PyTuple_Pack(2, self->root, path);
            Py_XDECREF(path);
            if (root == NULL) {
                Py_DECREF(value);
                return -1;
            }
            Py_SETREF(self->root, root);
            self->shift += BITS;
        }
        else if (builder_push_leaf(&self->root, self->shift, trunk, leaf) < 0) {
            Py_DECREF(value);
            return -1;
        }
    }
    self->tail[self->tail_len++] = value;
    self->count++;
    return 0;
}

static int
builder_append_item(Builder *self, PyObject *value)
{
    return builder_append_steal(self, Py_NewRef(value));
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

static int
builder_traverse(PyObject *op, visitproc visit, void *arg)
{
    Builder *self = (Builder *)op;
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(self->root);
    for (int i = 0; i < self->tail_len; i++) {
        Py_VISIT(self->tail[i]);
    }
    Py_VISIT(self->result_type);
    return 0;
}

static int
builder_clear(PyObject *op)
{
    Builder *self = (Builder *)op;
    Py_CLEAR(self->root);
    while (self->tail_len > 0) {
        self->tail_len--;
        Py_CLEAR(self->tail[self->tail_len]);
    }
    self->count = 0;
    self->shift = 0;
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
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static Builder *
builder_new(Vector *source)
{
    VectorState *state = PyType_GetModuleState(Py_TYPE(source));
    Builder *self = (Builder *)state->builder_type->tp_alloc(state->builder_type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->root = Py_XNewRef(source->root);
    self->tail_len = (int)PyTuple_GET_SIZE(source->tail);
    for (int i = 0; i < self->tail_len; i++) {
        self->tail[i] = Py_NewRef(PyTuple_GET_ITEM(source->tail, i));
    }
    self->count = source->count;
    self->shift = source->shift;
    self->result_type = (PyTypeObject *)Py_NewRef(Py_TYPE(source));
    self->owner = PyThreadState_GetID(PyThreadState_Get());
    return self;
}

static PyObject *
vector_mutate(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    return (PyObject *)builder_new((Vector *)op);
}

/* Publish the builder's contents as a vector and empty the builder. */
static PyObject *
builder_publish(Builder *self)
{
    PyObject *tail = builder_wrap_tail(self);
    if (tail == NULL) {
        return NULL;
    }
    PyObject *root = self->root;
    self->root = NULL;
    PyObject *result = vector_wrap(self->result_type, root, tail, self->count, self->shift);
    self->count = 0;
    self->shift = 0;
    return result;
}

static int
builder_normalize_index(Builder *self, PyObject *key, Py_ssize_t *index)
{
    if (index_value(key, index) < 0) {
        return -1;
    }
    if (*index < 0) {
        *index += self->count;
    }
    if (*index < 0 || *index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "PVector index out of range");
        return -1;
    }
    return 0;
}

static Py_ssize_t
builder_len(PyObject *op)
{
    Builder *self = (Builder *)op;
    return builder_check(self, 0) < 0 ? -1 : self->count;
}

static PyObject *
builder_borrow(Builder *self, Py_ssize_t index)
{
    Py_ssize_t trunk = self->count - self->tail_len;
    if (index >= trunk) {
        return self->tail[index - trunk];
    }
    return PyTuple_GET_ITEM(trie_leaf(self->root, self->shift, index), index & MASK);
}

static PyObject *
builder_subscript(PyObject *op, PyObject *key)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    self->busy = 1;
    Py_ssize_t index;
    PyObject *result = builder_normalize_index(self, key, &index) < 0 ? NULL :
                       Py_NewRef(builder_borrow(self, index));
    self->busy = 0;
    return result;
}

static int
builder_assign(PyObject *op, PyObject *key, PyObject *value)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return -1;
    }
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "PVector transients only remove with pop()");
        return -1;
    }
    self->busy = 1;
    Py_ssize_t index;
    int rc = builder_normalize_index(self, key, &index);
    if (rc == 0) {
        Py_ssize_t trunk = self->count - self->tail_len;
        if (index >= trunk) {
            Py_SETREF(self->tail[index - trunk], Py_NewRef(value));
        }
        else {
            rc = builder_set_path(&self->root, self->shift, index, value);
        }
    }
    self->busy = 0;
    return rc;
}

static PyObject *
builder_append(PyObject *op, PyObject *value)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    self->busy = 1;
    int rc = builder_append_item(self, value);
    self->busy = 0;
    if (rc < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
builder_extend(PyObject *op, PyObject *iterable)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    self->busy = 1;
    PyObject *it = PyObject_GetIter(iterable);
    if (it == NULL) {
        self->busy = 0;
        return NULL;
    }
    PyObject *item;
    int rc = 0;
    /* Iteration may run code that uses this builder; busy rejects it. */
    while ((item = PyIter_Next(it)) != NULL) {
        if ((rc = builder_append_steal(self, item)) < 0) {
            break;
        }
    }
    Py_DECREF(it);
    self->busy = 0;
    if (rc < 0 || PyErr_Occurred()) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
builder_pop(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return NULL;
    }
    if (self->count == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from empty PVector");
        return NULL;
    }
    if (self->tail_len == 1 && self->count > 1) {
        /* Refill the tail from the last trie leaf before taking its item. */
        Py_ssize_t last = self->count - 2;
        PyObject *leaf = trie_leaf(self->root, self->shift, last);
        PyObject *root = pop_leaf(self->root, self->shift, last);
        if (root == NULL && PyErr_Occurred()) {
            return NULL;
        }
        PyObject *removed = self->tail[0];
        for (int i = 0; i < WIDTH; i++) {
            self->tail[i] = Py_NewRef(PyTuple_GET_ITEM(leaf, i));
        }
        self->tail_len = WIDTH;
        self->count--;
        unsigned int shift = self->shift;
        while (root != NULL && shift > 0 && PyTuple_GET_SIZE(root) == 1) {
            Py_SETREF(root, Py_NewRef(PyTuple_GET_ITEM(root, 0)));
            shift -= BITS;
        }
        /* Release the old root only after the leaf items are owned. */
        Py_SETREF(self->root, root);
        self->shift = root == NULL ? 0 : shift;
        return removed;
    }
    self->count--;
    return self->tail[--self->tail_len];
}

static PyObject *
builder_finish(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 1) < 0) {
        return NULL;
    }
    self->busy = 1;
    PyObject *result = builder_publish(self);
    self->busy = 0;
    if (result == NULL) {
        return NULL;
    }
    self->status = FINISHED;
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
            PyTypeObject *type = self->result_type;
            self->result_type = NULL;
            builder_clear(op);
            self->result_type = type;
        }
    }
    Py_RETURN_FALSE;
}

static PyObject *
vector_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *source = NULL;
    static char *keywords[] = {"source", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O:PVector", keywords, &source)) {
        return NULL;
    }
    if (source != NULL && Py_IS_TYPE(source, type)) {
        return Py_NewRef(source);
    }
    if (source == NULL || PyTuple_CheckExact(source)) {
        PyObject *tuple = source == NULL ? PyTuple_New(0) : Py_NewRef(source);
        PyObject *result = tuple == NULL ? NULL : vector_from_tuple(type, tuple);
        Py_XDECREF(tuple);
        return result;
    }
    /* Append each item once, rather than snapshotting to a tuple and then
     * copying into leaves: acquiring shared items' references is the main
     * cost when threads build from shared data. */
    VectorState *state = PyType_GetModuleState(type);
    Builder *builder = (Builder *)state->builder_type->tp_alloc(state->builder_type, 0);
    if (builder == NULL) {
        return NULL;
    }
    builder->result_type = (PyTypeObject *)Py_NewRef(type);
    PyObject *result = NULL;
    PyObject *it = PyObject_GetIter(source);
    if (it != NULL) {
        PyObject *item;
        int rc = 0;
        while ((item = PyIter_Next(it)) != NULL) {
            if ((rc = builder_append_steal(builder, item)) < 0) {
                break;
            }
        }
        Py_DECREF(it);
        if (rc == 0 && !PyErr_Occurred()) {
            result = builder_publish(builder);
        }
    }
    Py_DECREF(builder);
    return result;
}

static int
iterator_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(((VectorIterator *)op)->vector);
    /* leaf is borrowed, not an additional owned GC edge. */
    return 0;
}

static void
iterator_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    Py_XDECREF(((VectorIterator *)op)->vector);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static PyObject *
iterator_next(PyObject *op)
{
    VectorIterator *self = (VectorIterator *)op;
    PyObject *result = NULL;
    /* No callbacks, allocations, or last-reference decrefs under this lock. */
    Py_BEGIN_CRITICAL_SECTION(op);
    Py_ssize_t index = self->index;
    if (index >= 0 && index < self->vector->count) {
        if (self->leaf == NULL || self->block != (index >> BITS)) {
            self->leaf = vector_leaf(self->vector, index);
            self->block = index >> BITS;
        }
        result = Py_NewRef(PyTuple_GET_ITEM(self->leaf, index & MASK));
        self->index += self->reverse ? -1 : 1;
    }
    Py_END_CRITICAL_SECTION();
    return result;
}

static PyObject *
vector_iterator(PyObject *op, int reverse)
{
    VectorState *state = PyType_GetModuleState(Py_TYPE(op));
    VectorIterator *it = (VectorIterator *)state->iterator_type->tp_alloc(state->iterator_type, 0);
    if (it == NULL) {
        return NULL;
    }
    it->vector = (Vector *)Py_NewRef(op);
    it->index = reverse ? it->vector->count - 1 : 0;
    it->block = -1;
    it->reverse = reverse;
    return (PyObject *)it;
}

static PyObject *vector_iter(PyObject *op) { return vector_iterator(op, 0); }
static PyObject *vector_reversed(PyObject *op, PyObject *Py_UNUSED(ignored)) { return vector_iterator(op, 1); }

static int
vector_contains(PyObject *op, PyObject *value)
{
    Vector *self = (Vector *)op;
    for (Py_ssize_t i = 0; i < self->count; i++) {
        int equal = PyObject_RichCompareBool(vector_borrow(self, i), value, Py_EQ);
        if (equal != 0) {
            return equal;
        }
    }
    return 0;
}

static PyObject *
vector_count(PyObject *op, PyObject *value)
{
    Vector *self = (Vector *)op;
    Py_ssize_t count = 0;
    for (Py_ssize_t i = 0; i < self->count; i++) {
        int equal = PyObject_RichCompareBool(vector_borrow(self, i), value, Py_EQ);
        if (equal < 0) {
            return NULL;
        }
        count += equal;
    }
    return PyLong_FromSsize_t(count);
}

static PyObject *
vector_index(PyObject *op, PyObject *args)
{
    Vector *self = (Vector *)op;
    PyObject *value;
    Py_ssize_t start = 0, stop = PY_SSIZE_T_MAX;
    if (!PyArg_ParseTuple(args, "O|O&O&:index", &value,
                          _PyEval_SliceIndexNotNone, &start, _PyEval_SliceIndexNotNone, &stop)) {
        return NULL;
    }
    if (start < 0) {
        start = Py_MAX(0, start + self->count);
    }
    stop = stop < 0 ? Py_MAX(0, stop + self->count) : Py_MIN(stop, self->count);
    for (Py_ssize_t i = start; i < stop; i++) {
        int equal = PyObject_RichCompareBool(vector_borrow(self, i), value, Py_EQ);
        if (equal < 0) {
            return NULL;
        }
        if (equal) {
            return PyLong_FromSsize_t(i);
        }
    }
    PyErr_SetString(PyExc_ValueError, "value is not in PVector");
    return NULL;
}

static PyObject *
vector_compare(PyObject *op, PyObject *other, int cmp)
{
    if (!Py_IS_TYPE(other, Py_TYPE(op))) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    Vector *a = (Vector *)op, *b = (Vector *)other;
    if ((cmp == Py_EQ || cmp == Py_NE) && a->count != b->count) {
        return PyBool_FromLong(cmp == Py_NE);
    }
    if (Py_EnterRecursiveCall(" while comparing PVectors")) {
        return NULL;
    }
    PyObject *result = NULL;
    Py_ssize_t count = Py_MIN(a->count, b->count);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *x = vector_borrow(a, i), *y = vector_borrow(b, i);
        int equal = PyObject_RichCompareBool(x, y, Py_EQ);
        if (equal < 0) {
            goto done;
        }
        if (!equal) {
            result = cmp == Py_EQ || cmp == Py_NE ? PyBool_FromLong(cmp == Py_NE) : PyObject_RichCompare(x, y, cmp);
            goto done;
        }
    }
    switch (cmp) {
        case Py_EQ: result = PyBool_FromLong(a->count == b->count); break;
        case Py_NE: result = PyBool_FromLong(a->count != b->count); break;
        case Py_LT: result = PyBool_FromLong(a->count < b->count); break;
        case Py_LE: result = PyBool_FromLong(a->count <= b->count); break;
        case Py_GT: result = PyBool_FromLong(a->count > b->count); break;
        case Py_GE: result = PyBool_FromLong(a->count >= b->count); break;
    }
done:
    Py_LeaveRecursiveCall();
    return result;
}

static Py_hash_t
vector_hash_impl(Vector *self)
{
    /* Equal to hash(tuple(self)), streamed over the leaves. */
    Py_uhash_t acc = _PyTuple_HASH_XXPRIME_5;
    PyObject *leaf = NULL;
    for (Py_ssize_t i = 0; i < self->count; i++) {
        if ((i & MASK) == 0) {
            leaf = vector_leaf(self, i);
        }
        Py_uhash_t lane = PyObject_Hash(PyTuple_GET_ITEM(leaf, i & MASK));
        if (lane == (Py_uhash_t)-1) {
            return -1;
        }
        acc += lane * _PyTuple_HASH_XXPRIME_2;
        acc = _PyTuple_HASH_XXROTATE(acc);
        acc *= _PyTuple_HASH_XXPRIME_1;
    }
    acc += self->count ^ (_PyTuple_HASH_XXPRIME_5 ^ 3527539UL);
    if (acc == (Py_uhash_t)-1) {
        return 1546275796;
    }
    return acc;
}

static Py_hash_t
vector_hash(PyObject *op)
{
    Vector *self = (Vector *)op;
    /* Racing threads compute the same value; any of them may store it. */
    Py_hash_t hash = FT_ATOMIC_LOAD_SSIZE_RELAXED(self->hash);
    if (hash != -1) {
        return hash;
    }
    if (Py_EnterRecursiveCall(" while hashing a PVector")) {
        return -1;
    }
    hash = vector_hash_impl(self);
    Py_LeaveRecursiveCall();
    if (hash != -1) {
        FT_ATOMIC_STORE_SSIZE_RELAXED(self->hash, hash);
    }
    return hash;
}

static PyObject *
vector_concat(PyObject *op, PyObject *other)
{
    if (!Py_IS_TYPE(other, Py_TYPE(op))) {
        /* sq_concat must raise, not return NotImplemented: the sequence
         * fallback in PyNumber_Add returns this slot's result directly. */
        PyErr_Format(PyExc_TypeError, "cannot concatenate PVector and %.200s", Py_TYPE(other)->tp_name);
        return NULL;
    }
    Vector *self = (Vector *)op, *right = (Vector *)other;
    if (right->count > PY_SSIZE_T_MAX - self->count) {
        PyErr_SetString(PyExc_OverflowError, "concatenated PVector is too large");
        return NULL;
    }
    if (right->count == 0) {
        return Py_NewRef(op);
    }
    if (self->count == 0) {
        return Py_NewRef(other);
    }
    /* Append right's items to a builder over self: O(len(right)), sharing
     * all of self's trie. */
    Builder *builder = builder_new(self);
    if (builder == NULL) {
        return NULL;
    }
    PyObject *result = NULL;
    PyObject *leaf = NULL;
    for (Py_ssize_t i = 0; i < right->count; i++) {
        if ((i & MASK) == 0) {
            leaf = vector_leaf(right, i);
        }
        if (builder_append_item(builder, PyTuple_GET_ITEM(leaf, i & MASK)) < 0) {
            goto done;
        }
    }
    result = builder_publish(builder);
done:
    Py_DECREF(builder);
    return result;
}

static PyObject *
vector_repr(PyObject *op)
{
    int recursive = Py_ReprEnter(op);
    if (recursive != 0) {
        return recursive < 0 ? NULL : PyUnicode_FromString("...");
    }
    PyObject *contents = PySequence_List(op);
    PyObject *result = contents == NULL ? NULL : PyUnicode_FromFormat("PVector(%R)", contents);
    Py_XDECREF(contents);
    Py_ReprLeave(op);
    return result;
}

static PyObject *vector_copy(PyObject *op, PyObject *Py_UNUSED(ignored)) { return Py_NewRef(op); }

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
vector_deepcopy(PyObject *op, PyObject *memo)
{
    if (!PyDict_Check(memo)) {
        PyErr_SetString(PyExc_TypeError, "deepcopy memo must be a dict");
        return NULL;
    }
    PyObject *contents = PySequence_Tuple(op);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *deepcopy = NULL, *result = NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(contents);
    PyObject *copied = PyTuple_New(n);
    int changed = 0;
    if (copied == NULL) {
        goto done;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = deepcopy_item(&deepcopy, PyTuple_GET_ITEM(contents, i), memo, &changed);
        if (item == NULL) {
            goto done;
        }
        PyTuple_SET_ITEM(copied, i, item);
    }
    if (deepcopy_memoized(op, memo, &result) == 0) {
        /* Unchanged children: like a tuple, the vector is its own copy. */
        result = changed ? vector_from_tuple(Py_TYPE(op), copied) : Py_NewRef(op);
    }
done:
    Py_XDECREF(deepcopy);
    Py_XDECREF(copied);
    Py_DECREF(contents);
    return result;
}

static PyObject *
vector_reduce(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyObject *contents = PySequence_Tuple(op);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, contents);
    Py_DECREF(contents);
    PyObject *result = args == NULL ? NULL : PyTuple_Pack(2, Py_TYPE(op), args);
    Py_XDECREF(args);
    return result;
}

static PyMethodDef vector_methods[] = {
    {"set", _PyCFunction_CAST(vector_set), METH_FASTCALL, "Return a vector with an existing index replaced."},
    {"append", vector_append, METH_O, "Return a vector with one appended element."},
    {"pop", vector_pop, METH_NOARGS, "Return the vector without its last element."},
    {"mutate", vector_mutate, METH_NOARGS, "Return a transient builder starting from this vector."},
    {"count", vector_count, METH_O, NULL},
    {"index", vector_index, METH_VARARGS, NULL},
    {"__reversed__", vector_reversed, METH_NOARGS, NULL},
    {"__copy__", vector_copy, METH_NOARGS, NULL},
    {"__deepcopy__", vector_deepcopy, METH_O, NULL},
    {"__reduce__", vector_reduce, METH_NOARGS, NULL},
    {"__class_getitem__", Py_GenericAlias, METH_O | METH_CLASS, NULL},
    {NULL}
};
static PyType_Slot vector_slots[] = {
    {Py_tp_new, vector_new},
    {Py_tp_dealloc, vector_dealloc},
    {Py_tp_traverse, vector_traverse},
    {Py_tp_repr, vector_repr},
    {Py_tp_hash, vector_hash},
    {Py_tp_richcompare, vector_compare},
    {Py_tp_iter, vector_iter},
    {Py_tp_methods, vector_methods},
    {Py_sq_length, vector_len},
    {Py_sq_item, vector_item},
    {Py_sq_contains, vector_contains},
    {Py_sq_concat, vector_concat},
    {Py_mp_length, vector_len},
    {Py_mp_subscript, vector_subscript},
    {0}
};
static PyMethodDef builder_methods[] = {
    {"append", builder_append, METH_O, "Append an item."},
    {"extend", builder_extend, METH_O, "Append the items of an iterable."},
    {"pop", builder_pop, METH_NOARGS, "Remove and return the last item."},
    {"finish", builder_finish, METH_NOARGS, "Return the built PVector; the transient is then unusable."},
    {"__enter__", builder_enter, METH_NOARGS, NULL},
    {"__exit__", builder_exit, METH_VARARGS, NULL},
    {NULL}
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
    {0}
};
static PyType_Slot iterator_slots[] = {
    {Py_tp_dealloc, iterator_dealloc},
    {Py_tp_traverse, iterator_traverse},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, iterator_next},
    {0}
};
#define CLOSED_GC (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE)
static PyType_Spec vector_spec = {"grail.PVector", sizeof(Vector), 0, CLOSED_GC | Py_TPFLAGS_SEQUENCE, vector_slots};
static PyType_Spec iterator_spec = {"_grail_vector._Iterator", sizeof(VectorIterator), 0,
    CLOSED_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION, iterator_slots};
static PyType_Spec builder_spec = {"_grail_vector._Transient", sizeof(Builder), 0,
    CLOSED_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION, builder_slots};

static int
module_traverse(PyObject *module, visitproc visit, void *arg)
{
    VectorState *state = PyModule_GetState(module);
    Py_VISIT(state->vector_type);
    Py_VISIT(state->iterator_type);
    Py_VISIT(state->builder_type);
    return 0;
}

static int
module_clear(PyObject *module)
{
    VectorState *state = PyModule_GetState(module);
    Py_CLEAR(state->vector_type);
    Py_CLEAR(state->iterator_type);
    Py_CLEAR(state->builder_type);
    return 0;
}

static int
module_exec(PyObject *module)
{
    VectorState *state = PyModule_GetState(module);
    state->vector_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &vector_spec, NULL);
    state->iterator_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &iterator_spec, NULL);
    state->builder_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &builder_spec, NULL);
    if (state->vector_type == NULL || state->iterator_type == NULL || state->builder_type == NULL) {
        return -1;
    }
    PyObject *sequence = PyImport_ImportModuleAttrString("collections.abc", "Sequence");
    if (sequence == NULL) {
        return -1;
    }
    PyObject *registered = PyObject_CallMethod(sequence, "register", "O", state->vector_type);
    Py_DECREF(sequence);
    if (registered == NULL) {
        return -1;
    }
    Py_DECREF(registered);
    return PyModule_AddObjectRef(module, "PVector", (PyObject *)state->vector_type);
}

PyABIInfo_VAR(vector_abi);
static PyModuleDef_Slot module_slots[] = {
    {Py_mod_abi, &vector_abi},
    {Py_mod_exec, module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};
static struct PyModuleDef vector_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail_vector",
    .m_size = sizeof(VectorState),
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
};
PyMODINIT_FUNC
PyInit__grail_vector(void)
{
    return PyModuleDef_Init(&vector_module);
}
