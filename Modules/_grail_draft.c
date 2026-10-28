/* grail.mutate drafts: edit an immutable value through thread-confined
 * drafts, then build the new value with grail.finish.
 *
 * Native counterpart of Lib/grail/_mutate.py (copied from grail-compat),
 * with the same behaviour and errors.  A draft of a record keeps one slot
 * per field: NULL until the field is assigned or read as a child draft.  A
 * draft of a PVector keeps the assigned indices in a short array and the
 * appended items in a list; a draft of a PMap keeps a dict of assignments
 * and deletions.  finish() builds bottom-up, caching each draft's result on
 * the draft, so an aliased draft is built once and a draft that reaches
 * itself is an error instead of unbounded recursion.
 *
 * Every operation first checks that the root is open and that the calling
 * thread created it, so a draft's state is only touched by its own thread.
 *
 * The root draft carries the mutate() call's state and is its own root.
 * Child drafts hold a reference to it, and it holds its edits, so finish()
 * and an aborting exit drop the root's edits to break that cycle. Record
 * and PVector drafts are recycled through per-thread freelists, like small
 * records.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "internal/pycore_code.h"    // _PySpecialize_RegisterAssItem()
#include "internal/pycore_grail.h"   // RecordObject, _PyGrail_RecordWithValues()
#include "internal/pycore_freelist.h" // _Py_FREELIST_POP_MEM()
#include "internal/pycore_moduleobject.h" // _PyModule_GetState()
#include "internal/pycore_object.h"  // _PyObject_Init()
#include "internal/pycore_pystate.h" // _PyThreadState_GET()

enum { ROOT_OPEN = _PyGrail_DRAFT_OPEN, ROOT_SEALED = _PyGrail_DRAFT_SEALED,
       ROOT_DONE };
enum { KIND_NONE, KIND_RECORD, KIND_VECTOR, KIND_MAP };

typedef struct _PyGrailDraftState {
    PyTypeObject *record_draft_type;
    PyTypeObject *vector_draft_type;
    PyTypeObject *map_draft_type;
    PyTypeObject *pvector_type;
    PyTypeObject *pmap_type;
    const _PyGrailVectorOps *vops;
    PyObject *deleted;           /* map deletion marker */
    PyObject *str_mutate;
    PyObject *str_finish;
} DraftState;

/* The common head of the three draft types and record drafts are laid out
 * in pycore_grail.h, for the specialized record draft attribute
 * instructions. st points into the module, which outlives its types'
 * objects. A root is the root draft (the head's root-only fields). */
typedef _PyGrailDraft DraftObject;
typedef _PyGrailDraft RootObject;
typedef _PyGrailRecordDraft RecordDraft;

#define VECTOR_DRAFT_FREELIST (Py_grail_records_MAXSAVESIZE + 1)

typedef struct {
    Py_ssize_t index;
    PyObject *value;
    /* value is the child draft of the base's item at index, so finish()
     * compares its result with the child's base instead of the trie. */
    int child;
} VectorChange;

typedef struct {
    DraftObject head;
    Py_ssize_t base_len;
    Py_ssize_t nchanges;
    Py_ssize_t capacity;
    VectorChange *changes;
    VectorChange small[4];
    /* Once there are more changes than small holds: changes[slot[i] - 1] is
     * the change at index i, slot[i] == 0 for none (base_len entries). */
    uint32_t *slot;
    PyObject *tail;              /* list of appended items, or NULL */
} VectorDraft;

/* finish() applies this many changes or more through a transient. */
#define VECTOR_DRAFT_TRANSIENT 4

typedef struct {
    DraftObject head;
    PyObject *changes;           /* dict: key -> value or st->deleted */
} MapDraft;

#define DRAFT(op) ((DraftObject *)(op))

static inline int
is_draft(DraftState *st, PyObject *op)
{
    PyTypeObject *t = Py_TYPE(op);
    return t == st->record_draft_type || t == st->vector_draft_type ||
           t == st->map_draft_type;
}

static inline int
draft_kind(DraftState *st, PyObject *value)
{
    PyTypeObject *t = Py_TYPE(value);
    if (t == st->pvector_type) {
        return KIND_VECTOR;
    }
    if (t == st->pmap_type) {
        return KIND_MAP;
    }
    if (_PyGrail_IsRecordTypeFast(t)) {
        return KIND_RECORD;
    }
    return KIND_NONE;
}

static int
fail(RootObject *root)
{
    PyErr_SetString(PyExc_RuntimeError,
                    root == NULL || root->state != ROOT_OPEN
                        ? "draft is no longer open" : "draft used from another thread");
    return -1;
}

/* root is NULL only in a draft cleared by the garbage collector. */
static inline int
check(RootObject *root)
{
    if (root == NULL || root->state != ROOT_OPEN ||
        root->owner != _PyThreadState_GET()) {
        return fail(root);
    }
    return 0;
}

/* ------------------------------------------------- creating drafts */

/* Initialize the head of a new draft d of base and track it; root as in
 * new_draft. Takes the references to root and base. */
static inline PyObject *
init_draft(DraftState *st, DraftObject *d, RootObject *root, PyObject *base,
           int kind, unsigned int base_version)
{
    d->root = root == NULL ? d : root;
    d->base = base;
    d->built = NULL;
    d->st = st;
    d->owner = root == NULL ? _PyThreadState_GET() : NULL;
    d->open_owner = d->owner;
    d->base_version = base_version;
    d->state = ROOT_OPEN;
    d->building = 0;
    d->kind = (char)kind;
    _PyObject_GC_TRACK(d);
    return (PyObject *)d;
}

/* A new record draft of base; root as in new_draft. */
static inline PyObject *
new_record_draft(DraftState *st, RootObject *root, PyObject *base)
{
    Py_ssize_t n = _PyGrail_RecordFieldCount(base);
    RecordDraft *r = NULL;
    if (n <= Py_grail_records_MAXSAVESIZE) {
        r = _Py_FREELIST_POP_MEM(grail_drafts[n]);
        if (r != NULL) {
            assert(Py_IS_TYPE(r, &_PyGrail_FreeRecord_Type));
            _PyObject_InitVar((PyVarObject *)r, st->record_draft_type, n);
        }
    }
    if (r == NULL) {
        r = PyObject_GC_NewVar(RecordDraft, st->record_draft_type, n);
        if (r == NULL) {
            return NULL;
        }
    }
    r->n = n;
    memset(r->values, 0, n * sizeof(PyObject *));
    PyTypeObject *tp = Py_TYPE(base);
    unsigned int version = tp->tp_version_tag;
    if (version == 0 && PyUnstable_Type_AssignVersionTag(tp)) {
        version = tp->tp_version_tag;
    }
    if (root != NULL) {
        Py_INCREF(root);
    }
    return init_draft(st, &r->head, root, Py_NewRef(base), KIND_RECORD, version);
}

/* A new draft of base: a child of root, or a new root draft when root is
 * NULL. */
static PyObject *
new_draft(DraftState *st, RootObject *root, PyObject *base, int kind)
{
    DraftObject *d;
    if (kind == KIND_RECORD) {
        return new_record_draft(st, root, base);
    }
    else if (kind == KIND_VECTOR) {
        Py_ssize_t len = st->vops->length(base);
        if (len < 0) {
            return NULL;
        }
        VectorDraft *v = _Py_FREELIST_POP_MEM(grail_drafts[VECTOR_DRAFT_FREELIST]);
        if (v != NULL) {
            assert(Py_IS_TYPE(v, &_PyGrail_FreeRecord_Type));
            _PyObject_Init((PyObject *)v, st->vector_draft_type);
        }
        else {
            v = PyObject_GC_New(VectorDraft, st->vector_draft_type);
            if (v == NULL) {
                return NULL;
            }
        }
        v->base_len = len;
        v->nchanges = 0;
        v->capacity = Py_ARRAY_LENGTH(v->small);
        v->changes = v->small;
        v->slot = NULL;
        v->tail = NULL;
        d = &v->head;
    }
    else {
        MapDraft *m = PyObject_GC_New(MapDraft, st->map_draft_type);
        if (m == NULL) {
            return NULL;
        }
        m->changes = NULL;
        d = &m->head;
    }
    if (root != NULL) {
        Py_INCREF(root);
    }
    return init_draft(st, d, root, Py_NewRef(base), kind, 0);
}

/* A new child draft of root for value, a record, PVector or PMap. */
static inline PyObject *
new_child(RootObject *root, PyObject *value, int kind)
{
    return kind == KIND_RECORD ? new_record_draft(root->st, root, value)
                               : new_draft(root->st, root, value, kind);
}

/* 0 if value may be stored in a draft of root, else -1 with ValueError. */
static inline int
storable(RootObject *root, PyObject *value)
{
    if (is_draft(root->st, value) && DRAFT(value)->root != root) {
        PyErr_SetString(PyExc_ValueError, "draft belongs to another mutate()");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------- shared draft methods */

static int
head_traverse(DraftObject *d, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(d));
    if (d->root != d) {
        Py_VISIT(d->root);
    }
    Py_VISIT(d->base);
    Py_VISIT(d->built);
    return 0;
}

static void
head_clear(DraftObject *d)
{
    d->base_version = 0;
    d->open_owner = NULL;
    if (d->root == d) {
        d->root = NULL;
    }
    Py_CLEAR(d->root);
    Py_CLEAR(d->base);
    Py_CLEAR(d->built);
}

static PyObject *
draft_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    RootObject *root = DRAFT(self)->root;
    if (check(root) < 0) {
        return NULL;
    }
    if (root != DRAFT(self)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "only the root draft is a context manager");
        return NULL;
    }
    return Py_NewRef(self);
}

static void drop_edits(DraftObject *root);

static PyObject *
draft_exit(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    RootObject *root = DRAFT(self)->root;
    if (root != NULL && root->state == ROOT_OPEN) {
        if (nargs > 0 && args[0] != Py_None) {
            root->state = ROOT_DONE;         /* aborted: no result */
            root->open_owner = NULL;
            drop_edits(root);
        }
        else {
            root->state = ROOT_SEALED;
            root->open_owner = NULL;
        }
    }
    Py_RETURN_FALSE;
}

static PyObject *
draft_no_copy(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyErr_SetString(PyExc_TypeError, "cannot copy a draft");
    return NULL;
}

static PyObject *
draft_no_pickle(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyErr_SetString(PyExc_TypeError, "cannot pickle a draft");
    return NULL;
}

static PyObject *
draft_repr(PyObject *self)
{
    return PyUnicode_FromFormat("<draft of %R>", DRAFT(self)->base);
}

#define DRAFT_METHODS \
    {"__enter__", draft_enter, METH_NOARGS, NULL}, \
    {"__exit__", _PyCFunction_CAST(draft_exit), METH_FASTCALL, NULL}, \
    {"__copy__", _PyCFunction_CAST(draft_no_copy), METH_FASTCALL, NULL}, \
    {"__deepcopy__", _PyCFunction_CAST(draft_no_copy), METH_FASTCALL, NULL}, \
    {"__reduce_ex__", _PyCFunction_CAST(draft_no_pickle), METH_FASTCALL, NULL}, \
    {"__reduce__", _PyCFunction_CAST(draft_no_pickle), METH_FASTCALL, NULL}

static void draft_dealloc(PyObject *op);
static PyObject *build_draft(PyObject *draft);

/* The finished value for a draft (borrowed from the draft, which caches
 * it), or value itself when it is not a draft. Drafts are the only objects
 * whose type has draft_dealloc. */
static inline PyObject *
build(PyObject *value)
{
    return Py_TYPE(value)->tp_dealloc == draft_dealloc ? build_draft(value)
                                                       : value;
}

/* ------------------------------------------------------- record drafts */

static int
record_draft_traverse(PyObject *op, visitproc visit, void *arg)
{
    RecordDraft *r = (RecordDraft *)op;
    for (Py_ssize_t i = 0; i < r->n; i++) {
        Py_VISIT(r->values[i]);
    }
    return head_traverse(&r->head, visit, arg);
}

static int
record_draft_clear(PyObject *op)
{
    RecordDraft *r = (RecordDraft *)op;
    for (Py_ssize_t i = 0; i < r->n; i++) {
        Py_CLEAR(r->values[i]);
    }
    head_clear(&r->head);
    return 0;
}

static int vector_draft_clear(PyObject *op);
static int map_draft_clear(PyObject *op);

static void
draft_dealloc(PyObject *op)
{
    PyTypeObject *tp = Py_TYPE(op);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
    int slot = -1;
    int kind = DRAFT(op)->kind;
    if (kind == KIND_RECORD) {
        (void)record_draft_clear(op);
        Py_ssize_t n = ((RecordDraft *)op)->n;
        slot = n <= Py_grail_records_MAXSAVESIZE ? (int)n : -1;
    }
    else if (kind == KIND_VECTOR) {
        (void)vector_draft_clear(op);
        slot = VECTOR_DRAFT_FREELIST;
    }
    else {
        (void)map_draft_clear(op);
    }
    /* Drafts have no pre-header and PyObject_GC_Del as tp_free, like
     * records; a parked draft has the placeholder type. */
    if (slot >= 0) {
        assert(_PyType_PreHeaderSize(tp) ==
               _PyType_PreHeaderSize(&_PyGrail_FreeRecord_Type));
        Py_SET_TYPE(op, &_PyGrail_FreeRecord_Type);
        if (!_Py_FREELIST_PUSH(grail_drafts[slot], op, Py_grail_drafts_MAXFREELIST)) {
            PyObject_GC_Del(op);
        }
    }
    else {
        tp->tp_free(op);
    }
    /* Pairs with the per-thread type reference taken at allocation, as for
     * records; Py_DECREF would use the type's shared refcount. */
    _Py_DECREF_TYPE(tp);
}

static int
no_field(PyObject *base, PyObject *name)
{
    PyErr_Format(PyExc_AttributeError, "'%s' record has no field %R",
                 _PyType_Name(Py_TYPE(base)), name);
    return -1;
}

/* Field i of an open record draft, creating the child draft of a
 * draftable field on first use (LOAD_ATTR_RECORD_DRAFT's slow path). */
static PyObject *
record_draft_load(PyObject *self, Py_ssize_t i)
{
    RecordDraft *r = (RecordDraft *)self;
    RootObject *root = r->head.root;
    PyObject *value = r->values[i];
    if (value != NULL) {
        return Py_NewRef(value);
    }
    PyObject *field = ((RecordObject *)r->head.base)->fields[i];
    int kind = draft_kind(root->st, field);
    if (!kind) {
        return Py_NewRef(field);
    }
    value = new_child(root, field, kind);
    if (value != NULL) {
        r->values[i] = Py_NewRef(value);
    }
    return value;
}

static PyObject *
record_draft_getattro(PyObject *self, PyObject *name)
{
    RecordDraft *r = (RecordDraft *)self;
    PyObject *base = r->head.base;
    if (base == NULL) {
        return PyObject_GenericGetAttr(self, name);
    }
    Py_ssize_t i = _PyGrail_FieldIndex(Py_TYPE(base), name);
    if (i < 0) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        /* Dunders (__class__, __enter__, ...) come from the draft type. */
        if (PyUnicode_Check(name) && PyUnicode_GET_LENGTH(name) > 4 &&
            PyUnicode_READ_CHAR(name, 0) == '_' &&
            PyUnicode_READ_CHAR(name, 1) == '_') {
            return PyObject_GenericGetAttr(self, name);
        }
        if (check(r->head.root) < 0) {
            return NULL;
        }
        no_field(base, name);
        return NULL;
    }
    if (check(r->head.root) < 0) {
        return NULL;
    }
    return record_draft_load(self, i);
}

static int
record_draft_setattro(PyObject *self, PyObject *name, PyObject *value)
{
    RecordDraft *r = (RecordDraft *)self;
    RootObject *root = r->head.root;
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "cannot delete a record field");
        return -1;
    }
    if (check(root) < 0) {
        return -1;
    }
    PyObject *base = r->head.base;
    Py_ssize_t i = _PyGrail_FieldIndex(Py_TYPE(base), name);
    if (i < 0) {
        return PyErr_Occurred() ? -1 : no_field(base, name);
    }
    if (storable(root, value) < 0) {
        return -1;
    }
    Py_XSETREF(r->values[i], Py_NewRef(value));
    return 0;
}

static PyObject *
record_draft_build(RecordDraft *r)
{
    PyObject *base = r->head.base;
    PyObject *const *fields = ((RecordObject *)base)->fields;
    Py_ssize_t n = r->n;
    PyObject *small[16];
    PyObject **values = NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *v = r->values[i];
        if (v == NULL) {
            continue;
        }
        v = build(v);            /* borrowed */
        if (v == NULL) {
            goto error;
        }
        if (v == fields[i]) {
            continue;
        }
        if (values == NULL) {
            values = n <= (Py_ssize_t)Py_ARRAY_LENGTH(small) ? small :
                     PyMem_Malloc(n * sizeof(PyObject *));
            if (values == NULL) {
                PyErr_NoMemory();
                return NULL;
            }
            memcpy(values, fields, n * sizeof(PyObject *));
        }
        values[i] = v;
    }
    if (values == NULL) {
        return Py_NewRef(base);
    }
    PyObject *result = _PyGrail_RecordWithValues(base, values);
    if (values != small) {
        PyMem_Free(values);
    }
    return result;
error:
    if (values != NULL && values != small) {
        PyMem_Free(values);
    }
    return NULL;
}

static PyMethodDef record_draft_methods[] = {
    DRAFT_METHODS,
    {NULL}
};
static PyType_Slot record_draft_slots[] = {
    {Py_tp_dealloc, draft_dealloc},
    {Py_tp_traverse, record_draft_traverse},
    {Py_tp_clear, record_draft_clear},
    {Py_tp_getattro, record_draft_getattro},
    {Py_tp_setattro, record_draft_setattro},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_repr, draft_repr},
    {Py_tp_methods, record_draft_methods},
    {0}
};
static PyType_Spec record_draft_spec = {
    "grail._RecordDraft", offsetof(RecordDraft, values), sizeof(PyObject *),
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE |
    Py_TPFLAGS_DISALLOW_INSTANTIATION,
    record_draft_slots
};

/* ------------------------------------------------------- vector drafts */

static int
vector_draft_traverse(PyObject *op, visitproc visit, void *arg)
{
    VectorDraft *v = (VectorDraft *)op;
    for (Py_ssize_t i = 0; i < v->nchanges; i++) {
        Py_VISIT(v->changes[i].value);
    }
    Py_VISIT(v->tail);
    return head_traverse(&v->head, visit, arg);
}

static int
vector_draft_clear(PyObject *op)
{
    VectorDraft *v = (VectorDraft *)op;
    Py_ssize_t n = v->nchanges;
    v->nchanges = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_DECREF(v->changes[i].value);
    }
    if (v->changes != v->small) {
        PyMem_Free(v->changes);
        v->changes = v->small;
        v->capacity = Py_ARRAY_LENGTH(v->small);
    }
    PyMem_Free(v->slot);
    v->slot = NULL;
    Py_CLEAR(v->tail);
    head_clear(&v->head);
    return 0;
}

static inline Py_ssize_t
vector_draft_len(VectorDraft *v)
{
    return v->base_len + (v->tail ? PyList_GET_SIZE(v->tail) : 0);
}

/* The normalized index, or -1 with an exception. */
static Py_ssize_t
vector_draft_index(VectorDraft *v, PyObject *key)
{
    if (!PyIndex_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "draft indices must be integers");
        return -1;
    }
    Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
        return -1;
    }
    Py_ssize_t n = vector_draft_len(v);
    if (i < 0) {
        i += n;
    }
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "draft index out of range");
        return -1;
    }
    return i;
}

static VectorChange *
vector_draft_find(VectorDraft *v, Py_ssize_t i)
{
    if (v->slot != NULL) {
        uint32_t k = v->slot[i];
        return k ? &v->changes[k - 1] : NULL;
    }
    for (Py_ssize_t k = 0; k < v->nchanges; k++) {
        if (v->changes[k].index == i) {
            return &v->changes[k];
        }
    }
    return NULL;
}

/* Store a new reference to value at base index i; child as in
 * VectorChange. */
static int
vector_draft_put(VectorDraft *v, Py_ssize_t i, PyObject *value, int child)
{
    VectorChange *c = vector_draft_find(v, i);
    if (c != NULL) {
        c->child = child;
        Py_SETREF(c->value, Py_NewRef(value));
        return 0;
    }
    if (v->nchanges == v->capacity) {
        if (v->slot == NULL && v->base_len <= UINT32_MAX) {
            v->slot = PyMem_Calloc(v->base_len, sizeof(uint32_t));
            if (v->slot == NULL) {
                PyErr_NoMemory();
                return -1;
            }
            for (Py_ssize_t k = 0; k < v->nchanges; k++) {
                v->slot[v->changes[k].index] = (uint32_t)(k + 1);
            }
        }
        Py_ssize_t cap = v->capacity * 2;
        VectorChange *grown = PyMem_Malloc(cap * sizeof(VectorChange));
        if (grown == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        memcpy(grown, v->changes, v->nchanges * sizeof(VectorChange));
        if (v->changes != v->small) {
            PyMem_Free(v->changes);
        }
        v->changes = grown;
        v->capacity = cap;
    }
    v->changes[v->nchanges].index = i;
    v->changes[v->nchanges].value = Py_NewRef(value);
    v->changes[v->nchanges].child = child;
    v->nchanges++;
    if (v->slot != NULL) {
        v->slot[i] = (uint32_t)v->nchanges;
    }
    return 0;
}

static Py_ssize_t
vector_draft_length(PyObject *self)
{
    VectorDraft *v = (VectorDraft *)self;
    if (check(v->head.root) < 0) {
        return -1;
    }
    return vector_draft_len(v);
}

/* Item 0 <= i < len of an open draft, drafting a draftable item. */
static PyObject *
vector_draft_get(VectorDraft *v, Py_ssize_t i)
{
    RootObject *root = v->head.root;
    if (i >= v->base_len) {
        PyObject **slot = &PyList_GET_ITEM(v->tail, i - v->base_len);
        if (!is_draft(root->st, *slot)) {
            int kind = draft_kind(root->st, *slot);
            if (kind) {
                PyObject *d = new_child(root, *slot, kind);
                if (d == NULL) {
                    return NULL;
                }
                Py_SETREF(*slot, d);
            }
        }
        return Py_NewRef(*slot);
    }
    VectorChange *c = vector_draft_find(v, i);
    if (c != NULL) {
        return Py_NewRef(c->value);
    }
    PyObject *item = root->st->vops->borrow(v->head.base, i);
    int kind = draft_kind(root->st, item);
    if (!kind) {
        return Py_NewRef(item);
    }
    PyObject *d = new_child(root, item, kind);
    if (d == NULL || vector_draft_put(v, i, d, 1) < 0) {
        Py_XDECREF(d);
        return NULL;
    }
    return d;
}

static PyObject *
vector_draft_subscript(PyObject *self, PyObject *key)
{
    VectorDraft *v = (VectorDraft *)self;
    if (check(v->head.root) < 0) {
        return NULL;
    }
    Py_ssize_t i = vector_draft_index(v, key);
    if (i < 0) {
        return NULL;
    }
    return vector_draft_get(v, i);
}

/* draft[i] for a small exact int i (BINARY_OP_EXTEND, registered with the
 * specializer): the same as vector_draft_subscript without the index
 * protocol. */
static PyObject *
vector_draft_item(PyObject *self, Py_ssize_t i)
{
    VectorDraft *v = (VectorDraft *)self;
    if (check(v->head.root) < 0) {
        return NULL;
    }
    Py_ssize_t n = vector_draft_len(v);
    if (i < 0) {
        i += n;
    }
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "draft index out of range");
        return NULL;
    }
    return vector_draft_get(v, i);
}

static int
vector_draft_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
    VectorDraft *v = (VectorDraft *)self;
    RootObject *root = v->head.root;
    if (check(root) < 0) {
        return -1;
    }
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete draft items");
        return -1;
    }
    Py_ssize_t i = vector_draft_index(v, key);
    if (i < 0 || storable(root, value) < 0) {
        return -1;
    }
    if (i >= v->base_len) {
        return PyList_SetItem(v->tail, i - v->base_len, Py_NewRef(value));
    }
    return vector_draft_put(v, i, value, 0);
}

/* draft[i] = value for an int i (_STORE_SUBSCR_SSIZE_ITEM, registered
 * with the optimizer): vector_draft_ass_subscript without the index
 * protocol. */
static int
vector_draft_ass_item(PyObject *self, Py_ssize_t i, PyObject *value)
{
    VectorDraft *v = (VectorDraft *)self;
    RootObject *root = v->head.root;
    if (check(root) < 0) {
        return -1;
    }
    Py_ssize_t n = vector_draft_len(v);
    if (i < 0) {
        i += n;
    }
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "draft index out of range");
        return -1;
    }
    if (storable(root, value) < 0) {
        return -1;
    }
    if (i >= v->base_len) {
        return PyList_SetItem(v->tail, i - v->base_len, Py_NewRef(value));
    }
    return vector_draft_put(v, i, value, 0);
}

static int
vector_draft_tail(VectorDraft *v)
{
    if (v->tail == NULL) {
        v->tail = PyList_New(0);
    }
    return v->tail == NULL ? -1 : 0;
}

static PyObject *
vector_draft_append(PyObject *self, PyObject *value)
{
    VectorDraft *v = (VectorDraft *)self;
    RootObject *root = v->head.root;
    if (check(root) < 0 || storable(root, value) < 0 || vector_draft_tail(v) < 0 ||
        PyList_Append(v->tail, value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
vector_draft_extend(PyObject *self, PyObject *iterable)
{
    VectorDraft *v = (VectorDraft *)self;
    RootObject *root = v->head.root;
    if (check(root) < 0 || vector_draft_tail(v) < 0) {
        return NULL;
    }
    PyObject *it = PyObject_GetIter(iterable);
    if (it == NULL) {
        return NULL;
    }
    PyObject *item;
    while ((item = PyIter_Next(it)) != NULL) {
        if (storable(root, item) < 0 || PyList_Append(v->tail, item) < 0) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
        Py_DECREF(item);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) {
        return NULL;
    }
    Py_RETURN_NONE;
}

/* The base item that change c replaces (borrowed). */
static inline PyObject *
vector_draft_old(const _PyGrailVectorOps *ops, PyObject *base, VectorChange *c)
{
    return c->child ? DRAFT(c->value)->base : ops->borrow(base, c->index);
}

/* Few changes are applied with set and append; more through a transient,
 * which copies each trie path once. */
static PyObject *
vector_draft_build(VectorDraft *v)
{
    DraftState *st = v->head.root->st;
    const _PyGrailVectorOps *ops = st->vops;
    PyObject *base = v->head.base;
    Py_ssize_t ntail = v->tail ? PyList_GET_SIZE(v->tail) : 0;
    if (v->nchanges + ntail < VECTOR_DRAFT_TRANSIENT) {
        Py_ssize_t indices[VECTOR_DRAFT_TRANSIENT];
        PyObject *values[VECTOR_DRAFT_TRANSIENT];
        Py_ssize_t n = 0;
        for (Py_ssize_t k = 0; k < v->nchanges; k++) {
            PyObject *value = build(v->changes[k].value);
            if (value == NULL) {
                return NULL;
            }
            Py_ssize_t index = v->changes[k].index;
            if (value != vector_draft_old(ops, base, &v->changes[k])) {
                indices[n] = index;
                values[n++] = value;
            }
        }
        PyObject *result = n ? ops->set_many(base, n, indices, values)
                             : Py_NewRef(base);
        for (Py_ssize_t k = 0; k < ntail && result != NULL; k++) {
            PyObject *value = build(PyList_GET_ITEM(v->tail, k));
            if (value == NULL) {
                Py_DECREF(result);
                return NULL;
            }
            Py_SETREF(result, ops->append(result, value));
        }
        return result;
    }
    PyObject *builder = NULL, *result = NULL;
    int changed = ntail > 0;
    for (Py_ssize_t k = 0; k < v->nchanges; k++) {
        PyObject *value = build(v->changes[k].value);
        if (value == NULL) {
            goto done;
        }
        Py_ssize_t index = v->changes[k].index;
        if (value == vector_draft_old(ops, base, &v->changes[k])) {
            continue;
        }
        if (builder == NULL && (builder = ops->builder(base)) == NULL) {
            goto done;
        }
        if (ops->builder_set(builder, index, value) < 0) {
            goto done;
        }
        changed = 1;
    }
    if (!changed) {
        return Py_NewRef(base);  /* every change was its own value */
    }
    if (builder == NULL && (builder = ops->builder(base)) == NULL) {
        goto done;
    }
    for (Py_ssize_t k = 0; k < ntail; k++) {
        PyObject *value = build(PyList_GET_ITEM(v->tail, k));
        if (value == NULL || ops->builder_append(builder, value) < 0) {
            goto done;
        }
    }
    result = ops->builder_finish(builder);
done:
    Py_XDECREF(builder);
    return result;
}

static PyMethodDef vector_draft_methods[] = {
    DRAFT_METHODS,
    {"append", vector_draft_append, METH_O, NULL},
    {"extend", vector_draft_extend, METH_O, NULL},
    {NULL}
};
static PyType_Slot vector_draft_slots[] = {
    {Py_tp_dealloc, draft_dealloc},
    {Py_tp_traverse, vector_draft_traverse},
    {Py_tp_clear, vector_draft_clear},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_repr, draft_repr},
    {Py_tp_methods, vector_draft_methods},
    {Py_mp_length, vector_draft_length},
    {Py_mp_subscript, vector_draft_subscript},
    {Py_mp_ass_subscript, vector_draft_ass_subscript},
    {Py_sq_length, vector_draft_length},
    {0}
};
static PyType_Spec vector_draft_spec = {
    "grail._VectorDraft", sizeof(VectorDraft), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE |
    Py_TPFLAGS_DISALLOW_INSTANTIATION,
    vector_draft_slots
};

/* ---------------------------------------------------------- map drafts */

static int
map_draft_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(((MapDraft *)op)->changes);
    return head_traverse(&((MapDraft *)op)->head, visit, arg);
}

static int
map_draft_clear(PyObject *op)
{
    Py_CLEAR(((MapDraft *)op)->changes);
    head_clear(&((MapDraft *)op)->head);
    return 0;
}

/* 1 with *value (a new reference), 0 when absent, -1 on error. */
static int
map_draft_lookup(MapDraft *m, PyObject *key, PyObject **value)
{
    RootObject *root = m->head.root;
    *value = NULL;
    if (check(root) < 0) {
        return -1;
    }
    if (m->changes != NULL) {
        int found = PyDict_GetItemRef(m->changes, key, value);
        if (found < 0) {
            return -1;
        }
        if (found) {
            if (*value == root->st->deleted) {
                Py_CLEAR(*value);
                return 0;
            }
            return 1;
        }
    }
    int found = PyMapping_GetOptionalItem(m->head.base, key, value);
    if (found <= 0) {
        return found;
    }
    int kind = draft_kind(root->st, *value);
    if (!kind) {
        return 1;
    }
    PyObject *d = new_child(root, *value, kind);
    Py_SETREF(*value, d);
    if (d == NULL) {
        return -1;
    }
    if (m->changes == NULL && (m->changes = PyDict_New()) == NULL) {
        Py_CLEAR(*value);
        return -1;
    }
    if (PyDict_SetItem(m->changes, key, d) < 0) {
        Py_CLEAR(*value);
        return -1;
    }
    return 1;
}

static PyObject *
map_draft_subscript(PyObject *self, PyObject *key)
{
    PyObject *value;
    int found = map_draft_lookup((MapDraft *)self, key, &value);
    if (found == 0) {
        PyErr_SetObject(PyExc_KeyError, key);
    }
    return value;
}

static PyObject *
map_draft_get(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "get() takes 1 or 2 arguments");
        return NULL;
    }
    PyObject *value;
    int found = map_draft_lookup((MapDraft *)self, args[0], &value);
    if (found == 0) {
        return Py_NewRef(nargs == 2 ? args[1] : Py_None);
    }
    return value;
}

static int
map_draft_contains(PyObject *self, PyObject *key)
{
    PyObject *value;
    int found = map_draft_lookup((MapDraft *)self, key, &value);
    Py_XDECREF(value);
    return found;
}

static int
map_draft_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
    MapDraft *m = (MapDraft *)self;
    RootObject *root = m->head.root;
    if (value == NULL) {
        PyObject *old;
        int found = map_draft_lookup(m, key, &old);
        Py_XDECREF(old);
        if (found <= 0) {
            if (found == 0) {
                PyErr_SetObject(PyExc_KeyError, key);
            }
            return -1;
        }
        value = root->st->deleted;
    }
    else if (check(root) < 0 || storable(root, value) < 0) {
        return -1;
    }
    if (m->changes == NULL && (m->changes = PyDict_New()) == NULL) {
        return -1;
    }
    return PyDict_SetItem(m->changes, key, value);
}

static Py_ssize_t
map_draft_length(PyObject *self)
{
    MapDraft *m = (MapDraft *)self;
    RootObject *root = m->head.root;
    if (check(root) < 0) {
        return -1;
    }
    Py_ssize_t n = PyObject_Length(m->head.base);  /* a PMap */
    if (n < 0 || m->changes == NULL) {
        return n;
    }
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(m->changes, &pos, &key, &value)) {
        int in_base = PySequence_Contains(m->head.base, key);
        if (in_base < 0) {
            return -1;
        }
        if (value == root->st->deleted) {
            n -= in_base;
        }
        else if (!in_base) {
            n++;
        }
    }
    return n;
}

static PyObject *
map_draft_build(MapDraft *m)
{
    DraftState *st = m->head.root->st;
    PyObject *base = m->head.base;
    if (m->changes == NULL) {
        return Py_NewRef(base);
    }
    PyObject *edits = PyList_New(0);
    if (edits == NULL) {
        return NULL;
    }
    Py_ssize_t pos = 0;
    PyObject *key, *value, *transient = NULL, *result = NULL;
    while (PyDict_Next(m->changes, &pos, &key, &value)) {
        PyObject *old;
        if (PyMapping_GetOptionalItem(base, key, &old) < 0) {
            goto done;
        }
        if (value != st->deleted) {
            value = build(value);
            if (value == NULL) {
                Py_XDECREF(old);
                goto done;
            }
        }
        int changed = value == st->deleted ? old != NULL : value != old;
        Py_XDECREF(old);
        if (changed) {
            PyObject *pair = PyTuple_Pack(2, key, value);
            if (pair == NULL || PyList_Append(edits, pair) < 0) {
                Py_XDECREF(pair);
                goto done;
            }
            Py_DECREF(pair);
        }
    }
    if (PyList_GET_SIZE(edits) == 0) {
        result = Py_NewRef(base);
        goto done;
    }
    transient = PyObject_CallMethodNoArgs(base, st->str_mutate);
    if (transient == NULL) {
        goto done;
    }
    for (Py_ssize_t k = 0; k < PyList_GET_SIZE(edits); k++) {
        PyObject *pair = PyList_GET_ITEM(edits, k);
        PyObject *ek = PyTuple_GET_ITEM(pair, 0), *ev = PyTuple_GET_ITEM(pair, 1);
        int rc = ev == st->deleted ? PyObject_DelItem(transient, ek)
                                   : PyObject_SetItem(transient, ek, ev);
        if (rc < 0) {
            goto done;
        }
    }
    result = PyObject_CallMethodNoArgs(transient, st->str_finish);
done:
    Py_XDECREF(transient);
    Py_DECREF(edits);
    return result;
}

static PyMethodDef map_draft_methods[] = {
    DRAFT_METHODS,
    {"get", _PyCFunction_CAST(map_draft_get), METH_FASTCALL, NULL},
    {NULL}
};
static PyType_Slot map_draft_slots[] = {
    {Py_tp_dealloc, draft_dealloc},
    {Py_tp_traverse, map_draft_traverse},
    {Py_tp_clear, map_draft_clear},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_repr, draft_repr},
    {Py_tp_methods, map_draft_methods},
    {Py_mp_length, map_draft_length},
    {Py_mp_subscript, map_draft_subscript},
    {Py_mp_ass_subscript, map_draft_ass_subscript},
    {Py_sq_contains, map_draft_contains},
    {0}
};
static PyType_Spec map_draft_spec = {
    "grail._MapDraft", sizeof(MapDraft), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE |
    Py_TPFLAGS_DISALLOW_INSTANTIATION,
    map_draft_slots
};

/* ------------------------------------------------------------ building */

/* build() of a draft. */
static PyObject *
build_draft(PyObject *value)
{
    DraftObject *d = DRAFT(value);
    if (d->built != NULL) {
        return d->built;
    }
    if (d->building) {
        PyErr_SetString(PyExc_ValueError, "draft contains itself");
        return NULL;
    }
    d->building = 1;
    PyObject *result;
    if (d->kind == KIND_RECORD) {
        result = record_draft_build((RecordDraft *)d);
    }
    else if (d->kind == KIND_VECTOR) {
        result = vector_draft_build((VectorDraft *)d);
    }
    else {
        result = map_draft_build((MapDraft *)d);
    }
    d->building = 0;
    d->built = result;
    return result;
}

/* ------------------------------------------------------ module functions */

static inline DraftState *
get_state(PyObject *module)
{
    return (DraftState *)_PyModule_GetState(module);
}

static PyObject *
grail_mutate(PyObject *module, PyObject *value)
{
    DraftState *st = get_state(module);
    if (is_draft(st, value)) {
        PyErr_SetString(PyExc_TypeError, "mutate() requires a value, not a draft");
        return NULL;
    }
    int kind = draft_kind(st, value);
    if (!kind) {
        PyErr_SetString(PyExc_TypeError,
                        "mutate() requires a Grail record, PVector or PMap");
        return NULL;
    }
    return new_draft(st, NULL, value, kind);
}

/* Release a root draft's edits, and with them its child drafts, which
 * refer back to it. Only finish() and an aborting exit call this: the
 * draft is closed, so nothing reads the edits again. */
static void
drop_edits(DraftObject *root)
{
    if (root->kind == KIND_RECORD) {
        RecordDraft *r = (RecordDraft *)root;
        for (Py_ssize_t i = 0; i < r->n; i++) {
            Py_CLEAR(r->values[i]);
        }
    }
    else if (root->kind == KIND_VECTOR) {
        VectorDraft *v = (VectorDraft *)root;
        Py_ssize_t n = v->nchanges;
        v->nchanges = 0;
        for (Py_ssize_t i = 0; i < n; i++) {
            Py_DECREF(v->changes[i].value);
        }
        PyMem_Free(v->slot);
        v->slot = NULL;
        Py_CLEAR(v->tail);
    }
    else {
        Py_CLEAR(((MapDraft *)root)->changes);
    }
}

static PyObject *
grail_finish(PyObject *module, PyObject *draft)
{
    DraftState *st = get_state(module);
    if (!is_draft(st, draft)) {
        PyErr_SetString(PyExc_TypeError, "finish() requires a draft from mutate()");
        return NULL;
    }
    RootObject *root = DRAFT(draft)->root;
    if (root != DRAFT(draft)) {
        PyErr_SetString(PyExc_RuntimeError, root == NULL ? "draft is no longer open"
                                                         : "finish() requires the root draft");
        return NULL;
    }
    if (root->owner != _PyThreadState_GET()) {
        PyErr_SetString(PyExc_RuntimeError, "draft used from another thread");
        return NULL;
    }
    if (root->built == NULL) {
        if (root->state == ROOT_DONE) {
            PyErr_SetString(PyExc_RuntimeError, "draft was aborted");
            return NULL;
        }
        root->state = ROOT_DONE;
        root->open_owner = NULL;
        if (build(draft) == NULL) {
            drop_edits(root);
            return NULL;
        }
        drop_edits(root);
    }
    return Py_NewRef(root->built);
}

static PyObject *
grail_original(PyObject *module, PyObject *draft)
{
    if (!is_draft(get_state(module), draft)) {
        PyErr_SetString(PyExc_TypeError, "original() requires a draft from mutate()");
        return NULL;
    }
    return Py_NewRef(DRAFT(draft)->base);
}

PyDoc_STRVAR(mutate_doc,
"mutate($module, value, /)\n--\n\n"
"Return a draft of a record, PVector or PMap; see finish().");
PyDoc_STRVAR(finish_doc,
"finish($module, draft, /)\n--\n\n"
"Return the value a root draft describes and close the draft.\n\n"
"Calling it again returns the same value. An aborted draft has no value.");
PyDoc_STRVAR(original_doc,
"original($module, draft, /)\n--\n\n"
"The value a draft was made from (its edits are not applied).");

static PyMethodDef draft_module_methods[] = {
    {"mutate", grail_mutate, METH_O, mutate_doc},
    {"finish", grail_finish, METH_O, finish_doc},
    {"original", grail_original, METH_O, original_doc},
    {NULL}
};

static int
draft_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    DraftState *st = get_state(module);
    Py_VISIT(st->record_draft_type);
    Py_VISIT(st->vector_draft_type);
    Py_VISIT(st->map_draft_type);
    Py_VISIT(st->pvector_type);
    Py_VISIT(st->pmap_type);
    Py_VISIT(st->deleted);
    return 0;
}

static int
draft_module_clear(PyObject *module)
{
    DraftState *st = get_state(module);
    Py_CLEAR(st->record_draft_type);
    Py_CLEAR(st->vector_draft_type);
    Py_CLEAR(st->map_draft_type);
    Py_CLEAR(st->pvector_type);
    Py_CLEAR(st->pmap_type);
    Py_CLEAR(st->deleted);
    Py_CLEAR(st->str_mutate);
    Py_CLEAR(st->str_finish);
    return 0;
}

static void
draft_module_free(void *module)
{
    (void)draft_module_clear((PyObject *)module);
}

static PyTypeObject *
import_type(const char *module, const char *name)
{
    PyObject *obj = PyImport_ImportModuleAttrString(module, name);
    if (obj != NULL && !PyType_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s.%s is not a type", module, name);
        Py_CLEAR(obj);
    }
    return (PyTypeObject *)obj;
}

static int
draft_exec(PyObject *module)
{
    DraftState *st = get_state(module);
#define TYPE(field, spec) \
    if ((st->field = (PyTypeObject *)PyType_FromModuleAndSpec( \
             module, &spec, NULL)) == NULL) { \
        return -1; \
    }
    TYPE(record_draft_type, record_draft_spec)
    TYPE(vector_draft_type, vector_draft_spec)
    TYPE(map_draft_type, map_draft_spec)
#undef TYPE
    if ((st->pvector_type = import_type("_grail_vector", "PVector")) == NULL ||
        (st->pmap_type = import_type("_grail_pds", "PMap")) == NULL) {
        return -1;
    }
    if ((st->deleted = PyBaseObject_Type.tp_alloc(&PyBaseObject_Type, 0)) == NULL ||
        (st->str_mutate = PyUnicode_InternFromString("mutate")) == NULL ||
        (st->str_finish = PyUnicode_InternFromString("finish")) == NULL) {
        return -1;
    }
    if ((st->vops = _PyGrail_GetVectorOps()) == NULL) {
        PyErr_SetString(PyExc_ImportError, "_grail_vector registered no operations");
        return -1;
    }
    _PyGrail_RegisterRecordDraft(record_draft_getattro, draft_dealloc,
                                 record_draft_load);
    _PyGrail_RegisterDraftSubscript(vector_draft_subscript, vector_draft_item);
    _PySpecialize_RegisterAssItem(vector_draft_ass_subscript, vector_draft_ass_item);
    _PyGrail_RegisterDraftWith(draft_enter, (void *)draft_exit);
    return 0;
}

PyABIInfo_VAR(draft_abi);
static PyModuleDef_Slot draft_module_slots[] = {
    {Py_mod_abi, &draft_abi},
    {Py_mod_exec, draft_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};
static struct PyModuleDef draft_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail_draft",
    .m_size = sizeof(DraftState),
    .m_methods = draft_module_methods,
    .m_slots = draft_module_slots,
    .m_traverse = draft_module_traverse,
    .m_clear = draft_module_clear,
    .m_free = draft_module_free,
};
PyMODINIT_FUNC
PyInit__grail_draft(void)
{
    return PyModuleDef_Init(&draft_module);
}
