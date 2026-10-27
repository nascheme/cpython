/* Grail unboxed numeric vectors: FloatVector (float64) and IntVector (int64).
 * Each vector is one immutable variable-size object whose items follow the
 * header. There are no per-element objects or references: reads box a new
 * float or int, and the storage is exported read-only through the buffer
 * protocol. Published storage never changes.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "internal/pycore_critical_section.h"  // Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST
#include "internal/pycore_long.h"    // _PyLong_IsCompact()
#include "internal/pycore_object.h"  // _Py_DECREF_TYPE()
#include "internal/pycore_pyatomic_ft_wrappers.h"
#include "internal/pycore_tuple.h"   // _PyTuple_HASH_XXPRIME_1

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { FLOAT, INT };

typedef union {
    double f;
    int64_t i;
} Item;

typedef struct {
    PyObject_VAR_HEAD            /* ob_size is the element count. */
    Py_hash_t hash;              /* -1 until computed; relaxed atomic. */
    int kind;
    Item items[];
} NumVector;

typedef struct {
    PyObject_HEAD
    NumVector *vector;           /* Retained until the iterator is destroyed. */
    Py_ssize_t index;            /* Atomic; advanced by compare-exchange. */
    int reverse;
} NumIterator;

enum { OPEN, SEALED, FINISHED, ABORTED };

typedef struct {
    PyObject_HEAD
    Item *items;                 /* PyMem buffer of cap items, [0, len) used. */
    Py_ssize_t len;
    Py_ssize_t cap;
    PyTypeObject *result_type;
    uint64_t owner;
    int kind;
    int status;
    int busy;
    int entered;
} Builder;

typedef struct {
    PyTypeObject *float_type;
    PyTypeObject *int_type;
    PyTypeObject *iterator_type;
    PyTypeObject *builder_type;
} NumState;

static const char *const kind_format[] = {"d", "q"};

#define TYPE_NAME(op) (_PyType_Name(Py_TYPE(op)))
#define MAX_ITEMS ((PY_SSIZE_T_MAX - (Py_ssize_t)sizeof(NumVector)) / (Py_ssize_t)sizeof(Item))

/* Elements */

static int
unbox(int kind, PyObject *value, Item *out)
{
    if (kind == FLOAT) {
        double d = PyFloat_CheckExact(value) ? PyFloat_AS_DOUBLE(value) : PyFloat_AsDouble(value);
        if (d == -1.0 && PyErr_Occurred()) {
            return -1;
        }
        out->f = d;
        return 0;
    }
    if (PyFloat_Check(value)) {
        PyErr_Format(PyExc_TypeError, "IntVector elements must be integers, not %.200s",
                     Py_TYPE(value)->tp_name);
        return -1;
    }
    return PyLong_AsInt64(value, &out->i);
}

static PyObject *
box(int kind, Item item)
{
    return kind == FLOAT ? PyFloat_FromDouble(item.f) : PyLong_FromInt64(item.i);
}

static int
item_eq(int kind, Item a, Item b)
{
    return kind == FLOAT ? a.f == b.f : a.i == b.i;
}

static Py_uhash_t
item_hash(int kind, Item item)
{
    if (kind == FLOAT) {
        /* A NaN element has no object identity to hash; it is never equal
         * to itself, so any value is consistent with equality. */
        return isnan(item.f) ? 0 : (Py_uhash_t)_Py_HashDouble(NULL, item.f);
    }
    /* hash(int): sign * (|v| mod modulus), with -1 mapped to -2. */
    int64_t v = item.i;
    uint64_t magnitude = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    Py_hash_t h = (Py_hash_t)(magnitude % PyHASH_MODULUS);
    if (v < 0) {
        h = -h;
    }
    return (Py_uhash_t)(h == -1 ? -2 : h);
}

/* Vector allocation and access */

static NumVector *
numvec_alloc(PyTypeObject *type, int kind, Py_ssize_t n)
{
    if (n > MAX_ITEMS) {
        PyErr_NoMemory();
        return NULL;
    }
    NumVector *self = (NumVector *)type->tp_alloc(type, n);
    if (self == NULL) {
        return NULL;
    }
    self->hash = -1;
    self->kind = kind;
    return self;
}

static NumVector *
numvec_from_items(PyTypeObject *type, int kind, const Item *items, Py_ssize_t n)
{
    NumVector *self = numvec_alloc(type, kind, n);
    if (self != NULL && n > 0) {
        memcpy(self->items, items, (size_t)n * sizeof(Item));
    }
    return self;
}

static void
numvec_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static Py_ssize_t
numvec_len(PyObject *op)
{
    return Py_SIZE(op);
}

static PyObject *
numvec_item(PyObject *op, Py_ssize_t index)
{
    NumVector *self = (NumVector *)op;
    if (index < 0 || index >= Py_SIZE(self)) {
        PyErr_Format(PyExc_IndexError, "%s index out of range", TYPE_NAME(op));
        return NULL;
    }
    return box(self->kind, self->items[index]);
}

static int
normalize_index(PyObject *key, Py_ssize_t count, const char *name, Py_ssize_t *index)
{
    if (PyLong_CheckExact(key) && _PyLong_IsCompact((PyLongObject *)key)) {
        /* A small exact int skips the __index__ protocol. */
        *index = _PyLong_CompactValue((PyLongObject *)key);
    }
    else if (!PyIndex_Check(key)) {
        PyErr_Format(PyExc_TypeError, "%s indices must be integers or slices, not %.200s",
                     name, Py_TYPE(key)->tp_name);
        return -1;
    }
    else {
        *index = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (*index == -1 && PyErr_Occurred()) {
            return -1;
        }
    }
    if (*index < 0) {
        *index += count;
    }
    if (*index < 0 || *index >= count) {
        PyErr_Format(PyExc_IndexError, "%s index out of range", name);
        return -1;
    }
    return 0;
}

static PyObject *
numvec_subscript(PyObject *op, PyObject *key)
{
    NumVector *self = (NumVector *)op;
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
            return NULL;
        }
        Py_ssize_t n = PySlice_AdjustIndices(Py_SIZE(self), &start, &stop, step);
        if (step == 1) {
            if (n == Py_SIZE(self)) {
                return Py_NewRef(op);
            }
            return (PyObject *)numvec_from_items(Py_TYPE(op), self->kind, self->items + start, n);
        }
        NumVector *result = numvec_alloc(Py_TYPE(op), self->kind, n);
        if (result != NULL) {
            for (Py_ssize_t i = 0, j = start; i < n; i++, j += step) {
                result->items[i] = self->items[j];
            }
        }
        return (PyObject *)result;
    }
    Py_ssize_t index;
    if (normalize_index(key, Py_SIZE(self), TYPE_NAME(op), &index) < 0) {
        return NULL;
    }
    return box(self->kind, self->items[index]);
}

/* Persistent updates: each copies the flat buffer, O(n). */

static PyObject *
numvec_set(PyObject *op, PyObject *args)
{
    NumVector *self = (NumVector *)op;
    PyObject *key, *value;
    if (!PyArg_UnpackTuple(args, "set", 2, 2, &key, &value)) {
        return NULL;
    }
    Item item;
    Py_ssize_t index;
    if (unbox(self->kind, value, &item) < 0 ||
        normalize_index(key, Py_SIZE(self), TYPE_NAME(op), &index) < 0) {
        return NULL;
    }
    NumVector *result = numvec_from_items(Py_TYPE(op), self->kind, self->items, Py_SIZE(self));
    if (result != NULL) {
        result->items[index] = item;
    }
    return (PyObject *)result;
}

static PyObject *
numvec_append(PyObject *op, PyObject *value)
{
    NumVector *self = (NumVector *)op;
    Item item;
    if (unbox(self->kind, value, &item) < 0) {
        return NULL;
    }
    Py_ssize_t n = Py_SIZE(self);
    NumVector *result = numvec_alloc(Py_TYPE(op), self->kind, n + 1);
    if (result != NULL) {
        if (n > 0) {
            memcpy(result->items, self->items, (size_t)n * sizeof(Item));
        }
        result->items[n] = item;
    }
    return (PyObject *)result;
}

static PyObject *
numvec_pop(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    NumVector *self = (NumVector *)op;
    if (Py_SIZE(self) == 0) {
        PyErr_Format(PyExc_IndexError, "pop from empty %s", TYPE_NAME(op));
        return NULL;
    }
    return (PyObject *)numvec_from_items(Py_TYPE(op), self->kind, self->items, Py_SIZE(self) - 1);
}

static PyObject *
numvec_concat(PyObject *op, PyObject *other)
{
    if (!Py_IS_TYPE(other, Py_TYPE(op))) {
        /* sq_concat must raise, not return NotImplemented: the sequence
         * fallback in PyNumber_Add returns this slot's result directly. */
        PyErr_Format(PyExc_TypeError, "cannot concatenate %s and %.200s",
                     TYPE_NAME(op), Py_TYPE(other)->tp_name);
        return NULL;
    }
    NumVector *a = (NumVector *)op, *b = (NumVector *)other;
    if (Py_SIZE(b) == 0) {
        return Py_NewRef(op);
    }
    if (Py_SIZE(a) == 0) {
        return Py_NewRef(other);
    }
    if (Py_SIZE(b) > MAX_ITEMS - Py_SIZE(a)) {
        return PyErr_NoMemory();
    }
    NumVector *result = numvec_alloc(Py_TYPE(op), a->kind, Py_SIZE(a) + Py_SIZE(b));
    if (result != NULL) {
        memcpy(result->items, a->items, (size_t)Py_SIZE(a) * sizeof(Item));
        memcpy(result->items + Py_SIZE(a), b->items, (size_t)Py_SIZE(b) * sizeof(Item));
    }
    return (PyObject *)result;
}

/* Transients */

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
builder_reserve(Builder *self, Py_ssize_t extra)
{
    if (extra <= self->cap - self->len) {
        return 0;
    }
    if (extra > MAX_ITEMS - self->len) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t need = self->len + extra;
    Py_ssize_t cap = self->cap < 8 ? 8 : self->cap;
    while (cap < need) {
        cap = cap > MAX_ITEMS / 2 ? MAX_ITEMS : cap * 2;
    }
    Item *items = PyMem_Realloc(self->items, (size_t)cap * sizeof(Item));
    if (items == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    self->items = items;
    self->cap = cap;
    return 0;
}

static int
builder_push(Builder *self, Item item)
{
    if (self->len == self->cap && builder_reserve(self, 1) < 0) {
        return -1;
    }
    self->items[self->len++] = item;
    return 0;
}

static void
builder_release(Builder *self)
{
    PyMem_Free(self->items);
    self->items = NULL;
    self->len = 0;
    self->cap = 0;
}

static void
builder_dealloc(PyObject *op)
{
    Builder *self = (Builder *)op;
    PyTypeObject *type = Py_TYPE(op);
    builder_release(self);
    Py_XDECREF(self->result_type);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static Builder *
builder_new(PyTypeObject *result_type, int kind, const Item *items, Py_ssize_t n)
{
    NumState *state = PyType_GetModuleState(result_type);
    Builder *self = (Builder *)state->builder_type->tp_alloc(state->builder_type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->result_type = (PyTypeObject *)Py_NewRef(result_type);
    self->kind = kind;
    self->owner = PyThreadState_GetID(PyThreadState_Get());
    if (n > 0) {
        if (builder_reserve(self, n) < 0) {
            Py_DECREF(self);
            return NULL;
        }
        memcpy(self->items, items, (size_t)n * sizeof(Item));
        self->len = n;
    }
    return self;
}

/* Copy a one-dimensional exporter whose format is this kind's exactly.
 * Returns 1 if copied, 0 if the buffer does not match, -1 on error. */
static int
builder_extend_buffer(Builder *self, PyObject *source)
{
    Py_buffer view;
    if (PyObject_GetBuffer(source, &view, PyBUF_RECORDS_RO) < 0) {
        return -1;
    }
    const char *format = view.format;
    if (format[0] == '@' || format[0] == '=') {
        format++;
    }
    int matches = view.ndim == 1 && view.itemsize == (Py_ssize_t)sizeof(Item) &&
                  (strcmp(format, kind_format[self->kind]) == 0 ||
                   (self->kind == INT && sizeof(long) == 8 && strcmp(format, "l") == 0));
    int rc = 0;
    if (matches) {
        Py_ssize_t n = view.shape[0], stride = view.strides[0];
        rc = builder_reserve(self, n);
        if (rc == 0) {
            const char *p = view.buf;
            for (Py_ssize_t i = 0; i < n; i++, p += stride) {
                memcpy(&self->items[self->len + i], p, sizeof(Item));
            }
            self->len += n;
            rc = 1;
        }
    }
    PyBuffer_Release(&view);
    return rc;
}

/* Append exact floats and ints of a tuple or list without calling user code.
 * Returns 1 if every item was taken, 0 to fall back to iteration (nothing
 * appended), -1 on error. */
static int
builder_extend_fast(Builder *self, PyObject *source)
{
    Py_ssize_t start = self->len;
    int rc = 1;
    Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(source);
    PyObject **items = PySequence_Fast_ITEMS(source);
    Py_ssize_t n = PySequence_Fast_GET_SIZE(source);
    if (builder_reserve(self, n) < 0) {
        rc = -1;
    }
    for (Py_ssize_t i = 0; rc == 1 && i < n; i++) {
        PyObject *value = items[i];
        Item *out = &self->items[self->len];
        if (PyFloat_CheckExact(value) && self->kind == FLOAT) {
            out->f = PyFloat_AS_DOUBLE(value);
        }
        else if (PyLong_CheckExact(value)) {
            /* Integer-to-double conversion and int64 overflow checks
             * call no user code. */
            Item item;
            if (unbox(self->kind, value, &item) < 0) {
                rc = -1;
                break;
            }
            *out = item;
        }
        else {
            rc = 0;
            break;
        }
        self->len++;
    }
    Py_END_CRITICAL_SECTION_SEQUENCE_FAST();
    if (rc <= 0) {
        self->len = start;
    }
    return rc;
}

/* Append every item of source; on failure the builder is unchanged. */
static int
builder_extend_any(Builder *self, PyObject *source)
{
    Py_ssize_t start = self->len;
    int rc;
    if (PyObject_CheckBuffer(source)) {
        rc = builder_extend_buffer(self, source);
        if (rc != 0) {
            return rc < 0 ? -1 : 0;
        }
    }
    if (PyTuple_CheckExact(source) || PyList_CheckExact(source)) {
        rc = builder_extend_fast(self, source);
        if (rc != 0) {
            return rc < 0 ? -1 : 0;
        }
    }
    if (Py_TYPE(source) == self->result_type) {
        NumVector *v = (NumVector *)source;
        if (builder_reserve(self, Py_SIZE(v)) < 0) {
            return -1;
        }
        memcpy(self->items + self->len, v->items, (size_t)Py_SIZE(v) * sizeof(Item));
        self->len += Py_SIZE(v);
        return 0;
    }
    PyObject *it = PyObject_GetIter(source);
    if (it == NULL) {
        return -1;
    }
    PyObject *value;
    rc = 0;
    while ((value = PyIter_Next(it)) != NULL) {
        Item item;
        rc = unbox(self->kind, value, &item);
        Py_DECREF(value);
        if (rc < 0 || (rc = builder_push(self, item)) < 0) {
            break;
        }
    }
    Py_DECREF(it);
    if (rc < 0 || PyErr_Occurred()) {
        self->len = start;
        return -1;
    }
    return 0;
}

static PyObject *
builder_publish(Builder *self)
{
    NumVector *result = numvec_from_items(self->result_type, self->kind, self->items, self->len);
    if (result != NULL) {
        builder_release(self);
    }
    return (PyObject *)result;
}

static PyObject *
numvec_mutate(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    NumVector *self = (NumVector *)op;
    return (PyObject *)builder_new(Py_TYPE(op), self->kind, self->items, Py_SIZE(self));
}

static Py_ssize_t
builder_len(PyObject *op)
{
    Builder *self = (Builder *)op;
    return builder_check(self, 0) < 0 ? -1 : self->len;
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
    int rc = normalize_index(key, self->len, _PyType_Name(self->result_type), &index);
    self->busy = 0;
    return rc < 0 ? NULL : box(self->kind, self->items[index]);
}

static int
builder_assign(PyObject *op, PyObject *key, PyObject *value)
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 0) < 0) {
        return -1;
    }
    if (value == NULL) {
        PyErr_Format(PyExc_TypeError, "%s transients only remove with pop()",
                     _PyType_Name(self->result_type));
        return -1;
    }
    self->busy = 1;
    Item item;
    Py_ssize_t index;
    int rc = unbox(self->kind, value, &item);
    if (rc == 0) {
        rc = normalize_index(key, self->len, _PyType_Name(self->result_type), &index);
    }
    self->busy = 0;
    if (rc == 0) {
        self->items[index] = item;
    }
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
    Item item;
    int rc = unbox(self->kind, value, &item);
    self->busy = 0;
    if (rc < 0 || builder_push(self, item) < 0) {
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
    /* Iteration may run code that uses this builder; busy rejects it. */
    self->busy = 1;
    int rc = builder_extend_any(self, iterable);
    self->busy = 0;
    if (rc < 0) {
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
    if (self->len == 0) {
        PyErr_Format(PyExc_IndexError, "pop from empty %s", _PyType_Name(self->result_type));
        return NULL;
    }
    PyObject *result = box(self->kind, self->items[self->len - 1]);
    if (result != NULL) {
        self->len--;
    }
    return result;
}

static PyObject *
builder_finish(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    Builder *self = (Builder *)op;
    if (builder_check(self, 1) < 0) {
        return NULL;
    }
    PyObject *result = builder_publish(self);
    if (result != NULL) {
        self->status = FINISHED;
    }
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
            builder_release(self);
        }
    }
    Py_RETURN_FALSE;
}

/* Construction */

static PyObject *
numvec_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *source = NULL;
    static char *keywords[] = {"source", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", keywords, &source)) {
        return NULL;
    }
    if (source != NULL && Py_IS_TYPE(source, type)) {
        return Py_NewRef(source);
    }
    NumState *state = PyType_GetModuleState(type);
    int kind = type == state->float_type ? FLOAT : INT;
    if (source == NULL) {
        return (PyObject *)numvec_alloc(type, kind, 0);
    }
    Builder *builder = builder_new(type, kind, NULL, 0);
    if (builder == NULL) {
        return NULL;
    }
    PyObject *result = NULL;
    if (builder_extend_any(builder, source) == 0) {
        result = builder_publish(builder);
    }
    Py_DECREF(builder);
    return result;
}

/* Iteration: the vector never changes, so only the position is shared. */

static void
iterator_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    Py_XDECREF(((NumIterator *)op)->vector);
    type->tp_free(op);
    _Py_DECREF_TYPE(type);  // pairs with tp_alloc's per-thread type reference
}

static PyObject *
iterator_next(PyObject *op)
{
    NumIterator *self = (NumIterator *)op;
    NumVector *v = self->vector;
#ifndef Py_GIL_DISABLED
    /* The GIL serializes advancing the position. */
    Py_ssize_t index = self->index;
    if (index < 0 || index >= Py_SIZE(v)) {
        return NULL;
    }
    self->index = self->reverse ? index - 1 : index + 1;
    return box(v->kind, v->items[index]);
#else
    Py_ssize_t index = _Py_atomic_load_ssize_relaxed(&self->index);
    for (;;) {
        if (index < 0 || index >= Py_SIZE(v)) {
            return NULL;
        }
        Py_ssize_t next = self->reverse ? index - 1 : index + 1;
        if (_Py_atomic_compare_exchange_ssize(&self->index, &index, next)) {
            return box(v->kind, v->items[index]);
        }
    }
#endif
}

static PyObject *
iterator_length_hint(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    NumIterator *self = (NumIterator *)op;
    Py_ssize_t index = _Py_atomic_load_ssize_relaxed(&self->index);
    Py_ssize_t n = Py_SIZE(self->vector);
    Py_ssize_t left = index < 0 || index >= n ? 0 : self->reverse ? index + 1 : n - index;
    return PyLong_FromSsize_t(left);
}

static PyObject *
numvec_iterator(PyObject *op, int reverse)
{
    NumState *state = PyType_GetModuleState(Py_TYPE(op));
    NumIterator *it = (NumIterator *)state->iterator_type->tp_alloc(state->iterator_type, 0);
    if (it == NULL) {
        return NULL;
    }
    it->vector = (NumVector *)Py_NewRef(op);
    it->index = reverse ? Py_SIZE(op) - 1 : 0;
    it->reverse = reverse;
    return (PyObject *)it;
}

static PyObject *numvec_iter(PyObject *op) { return numvec_iterator(op, 0); }
static PyObject *numvec_reversed(PyObject *op, PyObject *Py_UNUSED(ignored)) { return numvec_iterator(op, 1); }

/* Search */

/* Find value in [start, stop): its index, -1 if absent, -2 on error. Exact
 * floats (FloatVector) and exact int64 ints (IntVector) compare unboxed;
 * other values compare against each boxed element, as for a tuple. */
static Py_ssize_t
numvec_find(NumVector *self, PyObject *value, Py_ssize_t start, Py_ssize_t stop, Py_ssize_t *count)
{
    int kind = self->kind;
    Item target;
    int fast = 0;
    if (kind == FLOAT && PyFloat_CheckExact(value)) {
        target.f = PyFloat_AS_DOUBLE(value);
        fast = 1;
    }
    else if (kind == INT && PyLong_CheckExact(value)) {
        int overflow;
        long long v = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (overflow) {
            return -1;  /* No int64 element equals it. */
        }
        if (v == -1 && PyErr_Occurred()) {
            return -2;
        }
        target.i = v;
        fast = 1;
    }
    Py_ssize_t found = -1;
    for (Py_ssize_t i = start; i < stop && i < Py_SIZE(self); i++) {
        int equal;
        if (fast) {
            equal = item_eq(kind, self->items[i], target);
        }
        else {
            PyObject *element = box(kind, self->items[i]);
            if (element == NULL) {
                return -2;
            }
            equal = PyObject_RichCompareBool(element, value, Py_EQ);
            Py_DECREF(element);
            if (equal < 0) {
                return -2;
            }
        }
        if (equal) {
            if (count == NULL) {
                return i;
            }
            (*count)++;
            found = i;
        }
    }
    return found;
}

static int
numvec_contains(PyObject *op, PyObject *value)
{
    Py_ssize_t i = numvec_find((NumVector *)op, value, 0, Py_SIZE(op), NULL);
    return i == -2 ? -1 : i >= 0;
}

static PyObject *
numvec_count(PyObject *op, PyObject *value)
{
    Py_ssize_t count = 0;
    if (numvec_find((NumVector *)op, value, 0, Py_SIZE(op), &count) == -2) {
        return NULL;
    }
    return PyLong_FromSsize_t(count);
}

static PyObject *
numvec_index(PyObject *op, PyObject *args)
{
    PyObject *value;
    Py_ssize_t start = 0, stop = PY_SSIZE_T_MAX, n = Py_SIZE(op);
    if (!PyArg_ParseTuple(args, "O|nn:index", &value, &start, &stop)) {
        return NULL;
    }
    if (start < 0) {
        start = Py_MAX(start + n, 0);
    }
    if (stop < 0) {
        stop = Py_MAX(stop + n, 0);
    }
    Py_ssize_t i = numvec_find((NumVector *)op, value, start, stop, NULL);
    if (i == -2) {
        return NULL;
    }
    if (i == -1) {
        PyErr_Format(PyExc_ValueError, "%s.index(x): x not in vector", TYPE_NAME(op));
        return NULL;
    }
    return PyLong_FromSsize_t(i);
}

/* Comparison and hashing */

static int
item_cmp(int kind, Item a, Item b, int cmp)
{
    if (kind == FLOAT) {
        switch (cmp) {
            case Py_LT: return a.f < b.f;
            case Py_LE: return a.f <= b.f;
            case Py_GT: return a.f > b.f;
            default: return a.f >= b.f;
        }
    }
    switch (cmp) {
        case Py_LT: return a.i < b.i;
        case Py_LE: return a.i <= b.i;
        case Py_GT: return a.i > b.i;
        default: return a.i >= b.i;
    }
}

static PyObject *
numvec_compare(PyObject *op, PyObject *other, int cmp)
{
    if (!Py_IS_TYPE(other, Py_TYPE(op))) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    NumVector *a = (NumVector *)op, *b = (NumVector *)other;
    Py_ssize_t na = Py_SIZE(a), nb = Py_SIZE(b);
    if ((cmp == Py_EQ || cmp == Py_NE) && na != nb) {
        return PyBool_FromLong(cmp == Py_NE);
    }
    /* Elements compare by value: NaN is unequal to itself, unlike the
     * identity shortcut a tuple applies to one shared NaN object. */
    Py_ssize_t n = Py_MIN(na, nb);
    for (Py_ssize_t i = 0; i < n; i++) {
        if (!item_eq(a->kind, a->items[i], b->items[i])) {
            if (cmp == Py_EQ || cmp == Py_NE) {
                return PyBool_FromLong(cmp == Py_NE);
            }
            return PyBool_FromLong(item_cmp(a->kind, a->items[i], b->items[i], cmp));
        }
    }
    Py_RETURN_RICHCOMPARE(na, nb, cmp);
}

static Py_hash_t
numvec_hash(PyObject *op)
{
    NumVector *self = (NumVector *)op;
    /* Racing threads compute the same value; any of them may store it. */
    Py_hash_t hash = FT_ATOMIC_LOAD_SSIZE_RELAXED(self->hash);
    if (hash != -1) {
        return hash;
    }
    /* Equal to hash(tuple(self)) for NaN-free contents. */
    Py_ssize_t n = Py_SIZE(self);
    Py_uhash_t acc = _PyTuple_HASH_XXPRIME_5;
    for (Py_ssize_t i = 0; i < n; i++) {
        acc += item_hash(self->kind, self->items[i]) * _PyTuple_HASH_XXPRIME_2;
        acc = _PyTuple_HASH_XXROTATE(acc);
        acc *= _PyTuple_HASH_XXPRIME_1;
    }
    acc += n ^ (_PyTuple_HASH_XXPRIME_5 ^ 3527539UL);
    hash = acc == (Py_uhash_t)-1 ? 1546275796 : (Py_hash_t)acc;
    FT_ATOMIC_STORE_SSIZE_RELAXED(self->hash, hash);
    return hash;
}

/* Buffer protocol: read-only, one dimension, C-contiguous. */

static const Py_ssize_t item_stride = sizeof(Item);

static int
numvec_getbuffer(PyObject *op, Py_buffer *view, int flags)
{
    NumVector *self = (NumVector *)op;
    if (flags & PyBUF_WRITABLE) {
        PyErr_Format(PyExc_BufferError, "%s is not writable", TYPE_NAME(op));
        view->obj = NULL;
        return -1;
    }
    view->buf = self->items;
    view->obj = Py_NewRef(op);
    view->len = Py_SIZE(self) * (Py_ssize_t)sizeof(Item);
    view->readonly = 1;
    view->itemsize = sizeof(Item);
    view->format = (flags & PyBUF_FORMAT) ? (char *)kind_format[self->kind] : NULL;
    view->ndim = 1;
    /* The shape is the immutable element count stored in the object. */
    view->shape = (flags & PyBUF_ND) ? &((PyVarObject *)op)->ob_size : NULL;
    view->strides = (flags & PyBUF_STRIDES) == PyBUF_STRIDES ? (Py_ssize_t *)&item_stride : NULL;
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

/* Representation, copy and pickle */

static PyObject *
numvec_tolist(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    NumVector *self = (NumVector *)op;
    PyObject *list = PyList_New(Py_SIZE(self));
    if (list == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        PyObject *item = box(self->kind, self->items[i]);
        if (item == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, item);
    }
    return list;
}

static PyObject *
numvec_repr(PyObject *op)
{
    PyObject *contents = numvec_tolist(op, NULL);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *result = PyUnicode_FromFormat("%s(%R)", TYPE_NAME(op), contents);
    Py_DECREF(contents);
    return result;
}

/* Elements are numbers, so a vector is its own shallow and deep copy. */
static PyObject *numvec_copy(PyObject *op, PyObject *Py_UNUSED(ignored)) { return Py_NewRef(op); }
static PyObject *numvec_deepcopy(PyObject *op, PyObject *Py_UNUSED(memo)) { return Py_NewRef(op); }

static PyObject *
numvec_reduce(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyObject *contents = numvec_tolist(op, NULL);
    if (contents == NULL) {
        return NULL;
    }
    PyObject *result = Py_BuildValue("O(N)", Py_TYPE(op), contents);
    return result;
}

static PyMethodDef numvec_methods[] = {
    {"set", numvec_set, METH_VARARGS, "Return a vector with an existing index replaced."},
    {"append", numvec_append, METH_O, "Return a vector with one appended element."},
    {"pop", numvec_pop, METH_NOARGS, "Return the vector without its last element."},
    {"mutate", numvec_mutate, METH_NOARGS, "Return a transient builder starting from this vector."},
    {"tolist", numvec_tolist, METH_NOARGS, "Return the elements as a list."},
    {"count", numvec_count, METH_O, NULL},
    {"index", numvec_index, METH_VARARGS, NULL},
    {"__reversed__", numvec_reversed, METH_NOARGS, NULL},
    {"__copy__", numvec_copy, METH_NOARGS, NULL},
    {"__deepcopy__", numvec_deepcopy, METH_O, NULL},
    {"__reduce__", numvec_reduce, METH_NOARGS, NULL},
    {NULL}
};
static PyType_Slot numvec_slots[] = {
    {Py_tp_new, numvec_new},
    {Py_tp_dealloc, numvec_dealloc},
    {Py_tp_repr, numvec_repr},
    {Py_tp_hash, numvec_hash},
    {Py_tp_richcompare, numvec_compare},
    {Py_tp_iter, numvec_iter},
    {Py_tp_methods, numvec_methods},
    {Py_sq_length, numvec_len},
    {Py_sq_item, numvec_item},
    {Py_sq_contains, numvec_contains},
    {Py_sq_concat, numvec_concat},
    {Py_mp_length, numvec_len},
    {Py_mp_subscript, numvec_subscript},
    {Py_bf_getbuffer, numvec_getbuffer},
    {0}
};
static PyMethodDef builder_methods[] = {
    {"append", builder_append, METH_O, "Append an element."},
    {"extend", builder_extend, METH_O, "Append the elements of an iterable."},
    {"pop", builder_pop, METH_NOARGS, "Remove and return the last element."},
    {"finish", builder_finish, METH_NOARGS, "Return the built vector; the transient is then unusable."},
    {"__enter__", builder_enter, METH_NOARGS, NULL},
    {"__exit__", builder_exit, METH_VARARGS, NULL},
    {NULL}
};
static PyType_Slot builder_slots[] = {
    {Py_tp_dealloc, builder_dealloc},
    {Py_tp_methods, builder_methods},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_mp_length, builder_len},
    {Py_mp_subscript, builder_subscript},
    {Py_mp_ass_subscript, builder_assign},
    {0}
};
static PyMethodDef iterator_methods[] = {
    {"__length_hint__", iterator_length_hint, METH_NOARGS, NULL},
    {NULL}
};
static PyType_Slot iterator_slots[] = {
    {Py_tp_dealloc, iterator_dealloc},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, iterator_next},
    {Py_tp_methods, iterator_methods},
    {0}
};
/* None of these objects holds a reference that could form a cycle. */
#define CLOSED (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE)
static PyType_Spec float_spec = {"grail.FloatVector", sizeof(NumVector), sizeof(Item),
    CLOSED | Py_TPFLAGS_SEQUENCE, numvec_slots};
static PyType_Spec int_spec = {"grail.IntVector", sizeof(NumVector), sizeof(Item),
    CLOSED | Py_TPFLAGS_SEQUENCE, numvec_slots};
static PyType_Spec iterator_spec = {"_grail_numvec._Iterator", sizeof(NumIterator), 0,
    CLOSED | Py_TPFLAGS_DISALLOW_INSTANTIATION, iterator_slots};
static PyType_Spec builder_spec = {"_grail_numvec._Transient", sizeof(Builder), 0,
    CLOSED | Py_TPFLAGS_DISALLOW_INSTANTIATION, builder_slots};

static int
module_traverse(PyObject *module, visitproc visit, void *arg)
{
    NumState *state = PyModule_GetState(module);
    Py_VISIT(state->float_type);
    Py_VISIT(state->int_type);
    Py_VISIT(state->iterator_type);
    Py_VISIT(state->builder_type);
    return 0;
}

static int
module_clear(PyObject *module)
{
    NumState *state = PyModule_GetState(module);
    Py_CLEAR(state->float_type);
    Py_CLEAR(state->int_type);
    Py_CLEAR(state->iterator_type);
    Py_CLEAR(state->builder_type);
    return 0;
}

static int
module_exec(PyObject *module)
{
    NumState *state = PyModule_GetState(module);
    state->float_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &float_spec, NULL);
    state->int_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &int_spec, NULL);
    state->iterator_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &iterator_spec, NULL);
    state->builder_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &builder_spec, NULL);
    if (state->float_type == NULL || state->int_type == NULL ||
        state->iterator_type == NULL || state->builder_type == NULL) {
        return -1;
    }
    PyObject *sequence = PyImport_ImportModuleAttrString("collections.abc", "Sequence");
    if (sequence == NULL) {
        return -1;
    }
    PyTypeObject *types[] = {state->float_type, state->int_type};
    for (int i = 0; i < 2; i++) {
        PyObject *registered = PyObject_CallMethod(sequence, "register", "O", types[i]);
        if (registered == NULL) {
            Py_DECREF(sequence);
            return -1;
        }
        Py_DECREF(registered);
    }
    Py_DECREF(sequence);
    if (PyModule_AddObjectRef(module, "FloatVector", (PyObject *)state->float_type) < 0) {
        return -1;
    }
    return PyModule_AddObjectRef(module, "IntVector", (PyObject *)state->int_type);
}

PyABIInfo_VAR(numvec_abi);
static PyModuleDef_Slot module_slots[] = {
    {Py_mod_abi, &numvec_abi},
    {Py_mod_exec, module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};
static struct PyModuleDef numvec_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail_numvec",
    .m_size = sizeof(NumState),
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
};
PyMODINIT_FUNC
PyInit__grail_numvec(void)
{
    return PyModuleDef_Init(&numvec_module);
}
