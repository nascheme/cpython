/* Boxed, immutable Grail records. All instances remain GC tracked. */
#include "Python.h"
#include "internal/pycore_ceval.h"     // _PyEval_MatchClass()
#include "internal/pycore_code.h"      // _Py_GetBaseCodeUnit()
#include "internal/pycore_interpframe.h"  // _PyFrame_GetCode()
#include "internal/pycore_opcode_metadata.h"  // _PyOpcode_Caches
#include "internal/pycore_floatobject.h"  // _PyFloat_ExactDealloc()
#include "internal/pycore_freelist.h"  // _Py_FREELIST_PUSH()
#include "internal/pycore_grail.h"
#include "internal/pycore_object.h"  // _Py_DECREF_TYPE()
#include "internal/pycore_runtime.h"  // _Py_ID()
#include "internal/pycore_stackref.h"
#include "internal/pycore_tuple.h"     // _PyTuple_HASH_XXPRIME_1
#include "internal/pycore_unicodeobject.h"  // _PyUnicode_InternMortal()
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Heap-type schema: (names, plans, defaults, annotations, eligible, nodes).
 * A plan is None, an exact value type, or (tag, tuple-of-plans).
 * Tags: 1 union, 2 fixed tuple, 3 variable tuple.
 *
 * nodes is a bytes object holding the plans compiled at declaration, so
 * runtime checks read C kinds instead of decoding tuples and ints. Node i
 * (i < number of fields) is field i's plan; a union or tuple node's children
 * are the contiguous nodes [first, first + n). Types are borrowed from the
 * plans, which the schema owns. */
#define PLAN_ANY _PyGrail_PLAN_ANY
#define PLAN_EXACT _PyGrail_PLAN_EXACT
#define PLAN_RECORD _PyGrail_PLAN_RECORD
#define PLAN_UNION _PyGrail_PLAN_UNION
#define PLAN_TUPLE _PyGrail_PLAN_TUPLE
#define PLAN_VARTUPLE _PyGrail_PLAN_VARTUPLE
typedef _PyGrailPlanNode PlanNode;

/* Conformance recursion follows the plan, so checking a plan no taller than
 * this needs no recursion guard. */
#define PLAN_UNGUARDED_HEIGHT 32

static_assert(offsetof(PyBytesObject, ob_sval) % _Alignof(PlanNode) == 0,
              "plan nodes are stored in a bytes object");

static PyObject *record_new(PyTypeObject *, PyObject *, PyObject *);

int
_PyGrail_IsRecordType(PyObject *obj)
{
    return PyType_Check(obj) && ((PyTypeObject *)obj)->tp_new == record_new;
}

static Py_ssize_t
record_count(PyObject *obj)
{
    return (Py_TYPE(obj)->tp_basicsize - offsetof(RecordObject, fields)) /
           sizeof(PyObject *);
}

static PyObject *
schema_part(PyTypeObject *type, int part)
{
    return PyTuple_GET_ITEM(((PyHeapTypeObject *)type)->_ht_grail_schema, part);
}

static inline const PlanNode *
schema_nodes(PyTypeObject *type)
{
    return (const PlanNode *)PyBytes_AS_STRING(schema_part(type, 5));
}

static int
require_record_type(PyObject *obj)
{
    if (!_PyGrail_IsRecordType(obj)) {
        PyErr_SetString(PyExc_TypeError, "expected a Grail record type");
        return -1;
    }
    return 0;
}

static int
require_record(PyObject *obj)
{
    return require_record_type((PyObject *)Py_TYPE(obj));
}

static int
plan_check(PyObject *plan)
{
    if (plan == Py_None) {
        return 0;
    }
    if (PyType_Check(plan)) {
        if (plan == (PyObject *)Py_TYPE(Py_None) ||
            plan == (PyObject *)&PyBool_Type || plan == (PyObject *)&PyLong_Type ||
            plan == (PyObject *)&PyFloat_Type || plan == (PyObject *)&PyUnicode_Type ||
            plan == (PyObject *)&PyBytes_Type) {
            return 1;
        }
        return _PyGrail_IsRecordType(plan) &&
               schema_part((PyTypeObject *)plan, 4) == Py_True;
    }
    if (!PyTuple_CheckExact(plan) || PyTuple_GET_SIZE(plan) != 2 ||
        !PyLong_CheckExact(PyTuple_GET_ITEM(plan, 0)) ||
        !PyTuple_CheckExact(PyTuple_GET_ITEM(plan, 1))) {
        return 0;
    }
    long tag = PyLong_AsLong(PyTuple_GET_ITEM(plan, 0));
    if (tag < 1 || tag > 3) {
        return 0;
    }
    PyObject *children = PyTuple_GET_ITEM(plan, 1);
    Py_ssize_t n = PyTuple_GET_SIZE(children);
    if ((tag == 1 && n == 0) || (tag == 3 && n != 1)) {
        return 0;
    }
    if (Py_EnterRecursiveCall(" while validating a record annotation")) {
        return -1;
    }
    int result = 1;
    for (Py_ssize_t i = 0; i < n; i++) {
        result = plan_check(PyTuple_GET_ITEM(children, i));
        if (result != 1) {
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return result;
}

static int plan_conforms_nested(const PlanNode *, const PlanNode *, PyObject *);

/* 1 if value conforms to leaf plan node p (any, exact or record), else 0. */
static inline int
plan_conforms_leaf(const PlanNode *p, PyObject *value)
{
    if (p->kind == PLAN_ANY) {
        return 1;
    }
    return Py_TYPE(value) == p->type &&
           (p->kind == PLAN_EXACT || ((RecordObject *)value)->conforming);
}

/* 1 if value conforms to plan node p, 0 if not, -1 on error. A union of
 * leaves, such as `T | None`, is checked inline. */
static inline int
plan_conforms(const PlanNode *nodes, const PlanNode *p, PyObject *value)
{
    switch (p->kind) {
    case PLAN_ANY:
    case PLAN_EXACT:
    case PLAN_RECORD:
        return plan_conforms_leaf(p, value);
    case PLAN_UNION:
        if (p->height == 1) {
            const PlanNode *children = nodes + p->first;
            for (uint32_t i = 0; i < p->n; i++) {
                if (plan_conforms_leaf(&children[i], value)) {
                    return 1;
                }
            }
            return 0;
        }
        _Py_FALLTHROUGH;
    default:
        return plan_conforms_nested(nodes, p, value);
    }
}

static int
plan_conforms_nested(const PlanNode *nodes, const PlanNode *p, PyObject *value)
{
    int guarded = p->height > PLAN_UNGUARDED_HEIGHT;
    if (guarded && Py_EnterRecursiveCall(" while checking record conformance")) {
        return -1;
    }
    const PlanNode *children = nodes + p->first;
    int result = 0;
    if (p->kind == PLAN_UNION) {
        for (uint32_t i = 0; i < p->n; i++) {
            result = plan_conforms(nodes, &children[i], value);
            if (result != 0) {
                break;
            }
        }
    }
    else if (p->kind == PLAN_VARTUPLE && p->height <= 2 &&
             (children->height == 0 || children->kind == PLAN_UNION)) {
        /* tuple[T, ...] of a leaf or a union of leaves (`T | None`): the
         * common long cases, without the per-item switch. */
        if (PyTuple_CheckExact(value)) {
            const PlanNode *item = children;
            PyObject **items = ((PyTupleObject *)value)->ob_item;
            Py_ssize_t n = PyTuple_GET_SIZE(value);
            result = 1;
            if (item->kind == PLAN_EXACT) {
                PyTypeObject *type = item->type;
                for (Py_ssize_t i = 0; i < n; i++) {
                    if (Py_TYPE(items[i]) != type) {
                        result = 0;
                        break;
                    }
                }
            }
            else if (item->kind == PLAN_UNION) {
                const PlanNode *alts = nodes + item->first;
                for (Py_ssize_t i = 0; i < n && result; i++) {
                    result = 0;
                    for (uint32_t j = 0; j < item->n; j++) {
                        if (plan_conforms_leaf(&alts[j], items[i])) {
                            result = 1;
                            break;
                        }
                    }
                }
            }
            else {
                for (Py_ssize_t i = 0; i < n; i++) {
                    if (!plan_conforms_leaf(item, items[i])) {
                        result = 0;
                        break;
                    }
                }
            }
        }
    }
    else if (PyTuple_CheckExact(value) &&
             (p->kind == PLAN_VARTUPLE || PyTuple_GET_SIZE(value) == p->n)) {
        int var = p->kind == PLAN_VARTUPLE;
        result = 1;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(value); i++) {
            result = plan_conforms(nodes, &children[var ? 0 : i],
                                   PyTuple_GET_ITEM(value, i));
            if (result != 1) {
                break;
            }
        }
    }
    if (guarded) {
        Py_LeaveRecursiveCall();
    }
    return result;
}

const _PyGrailPlanNode *
_PyGrail_PlanNodes(PyTypeObject *type)
{
    return _PyGrail_IsRecordTypeFast(type) ? schema_nodes(type) : NULL;
}

int
_PyGrail_PlanConforms(const _PyGrailPlanNode *nodes, const _PyGrailPlanNode *p,
                      PyObject *value)
{
    return plan_conforms(nodes, p, value);
}

/* Number of nodes below plan, which plan_check has validated. */
static Py_ssize_t
plan_descendants(PyObject *plan)
{
    if (!PyTuple_Check(plan)) {
        return 0;
    }
    PyObject *children = PyTuple_GET_ITEM(plan, 1);
    Py_ssize_t count = PyTuple_GET_SIZE(children);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(children); i++) {
        count += plan_descendants(PyTuple_GET_ITEM(children, i));
    }
    return count;
}

static void
plan_compile(PlanNode *nodes, PlanNode *node, PyObject *plan, uint32_t *next)
{
    *node = (PlanNode){0};
    if (plan == Py_None) {
        node->kind = PLAN_ANY;
        return;
    }
    if (PyType_Check(plan)) {
        node->type = (PyTypeObject *)plan;
        node->kind = _PyGrail_IsRecordType(plan) ? PLAN_RECORD : PLAN_EXACT;
        return;
    }
    long tag = PyLong_AsLong(PyTuple_GET_ITEM(plan, 0));
    PyObject *children = PyTuple_GET_ITEM(plan, 1);
    node->kind = tag == 1 ? PLAN_UNION : tag == 2 ? PLAN_TUPLE : PLAN_VARTUPLE;
    node->n = (uint32_t)PyTuple_GET_SIZE(children);
    node->first = *next;
    *next += node->n;
    for (uint32_t i = 0; i < node->n; i++) {
        PlanNode *child = &nodes[node->first + i];
        plan_compile(nodes, child, PyTuple_GET_ITEM(children, i), next);
        if (child->height >= node->height) {
            node->height = child->height + 1;
        }
    }
    if (node->height == 0) {
        node->height = 1;
    }
}

/* The schema's nodes for validated plans: a bytes object. */
static PyObject *
plans_compile(PyObject *plans)
{
    Py_ssize_t n = PyTuple_GET_SIZE(plans);
    Py_ssize_t total = n;
    for (Py_ssize_t i = 0; i < n; i++) {
        total += plan_descendants(PyTuple_GET_ITEM(plans, i));
    }
    if (total > UINT32_MAX || (size_t)total > PY_SSIZE_T_MAX / sizeof(PlanNode)) {
        return PyErr_NoMemory();
    }
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, total * sizeof(PlanNode));
    if (bytes == NULL) {
        return NULL;
    }
    PlanNode *nodes = (PlanNode *)PyBytes_AS_STRING(bytes);
    uint32_t next = (uint32_t)n;
    for (Py_ssize_t i = 0; i < n; i++) {
        plan_compile(nodes, &nodes[i], PyTuple_GET_ITEM(plans, i), &next);
    }
    assert(next == total);
    return bytes;
}

/* Small records are recycled through a per-thread (per-interpreter with the
 * GIL) freelist per field count, shared by every record type: all records
 * of one size have the same layout, no pre-header and PyObject_GC_Del as
 * tp_free. A parked record is untracked, holds no references and has the
 * placeholder type below, never a record type it no longer owns. */
PyTypeObject _PyGrail_FreeRecord_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "grail.free_record",
    .tp_basicsize = offsetof(RecordObject, fields),
    .tp_itemsize = sizeof(PyObject *),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_free = PyObject_GC_Del,
};

/* Fields are not zeroed: callers fill every field, with no allocation or
 * callback in between, and only then track the record. */
static inline RecordObject *
record_alloc(PyTypeObject *type, Py_ssize_t n)
{
    assert(type->tp_alloc == PyType_GenericAlloc);
    assert(type->tp_free == PyObject_GC_Del);
    if (n <= Py_grail_records_MAXSAVESIZE) {
        RecordObject *self = _Py_FREELIST_POP_MEM(grail_records[n]);
        if (self != NULL) {
            assert(Py_IS_TYPE(self, &_PyGrail_FreeRecord_Type));
            _PyObject_Init((PyObject *)self, type);
            self->hash_tag = 0;
            return self;
        }
    }
    RecordObject *self = PyObject_GC_New(RecordObject, type);
    if (self != NULL) {
        self->hash_tag = 0;
    }
    return self;
}

/* tp_free for a cleared, untracked record whose type reference the caller
 * still holds. */
static inline void
record_free(RecordObject *self, PyTypeObject *type)
{
    Py_ssize_t n = record_count((PyObject *)self);
    assert(_PyType_PreHeaderSize(type) ==
           _PyType_PreHeaderSize(&_PyGrail_FreeRecord_Type));
    if (n <= Py_grail_records_MAXSAVESIZE &&
        !_PyInterpreterState_GET()->grail_record_freelist_off) {
        Py_SET_TYPE(self, &_PyGrail_FreeRecord_Type);
        if (_Py_FREELIST_PUSH(grail_records[n], self,
                              Py_grail_records_MAXFREELIST)) {
            return;
        }
    }
    PyObject_GC_Del(self);
}

/* Allocate and fill a record whose conformance is already known. */
static PyObject *
record_build(PyTypeObject *type, PyObject *const *values, int conforming)
{
    /* No Python callback between allocation and completion. A new instance
     * is fully initialized before any reference to it can escape. */
    Py_ssize_t n = PyTuple_GET_SIZE(schema_part(type, 1));
    RecordObject *self = record_alloc(type, n);
    if (self == NULL) {
        return NULL;
    }
    self->conforming = conforming;
    for (Py_ssize_t i = 0; i < n; i++) {
        self->fields[i] = Py_NewRef(values[i]);
    }
    _PyObject_GC_TRACK(self);
    return (PyObject *)self;
}

static PyObject *
record_finish(PyTypeObject *type, PyObject *const *values)
{
    Py_ssize_t n = PyTuple_GET_SIZE(schema_part(type, 1));
    const PlanNode *nodes = schema_nodes(type);
    int conforming = 1;
    for (Py_ssize_t i = 0; i < n; i++) {
        int ok = plan_conforms(nodes, &nodes[i], values[i]);
        if (ok < 0) {
            return NULL;
        }
        if (!ok) {
            conforming = 0;
        }
    }
    return record_build(type, values, conforming);
}

/* CALL_RECORD: a positional call supplying every field of an exact record
 * type. Steals the argument references on success only, so the caller's
 * stack is intact on error. Same order as record_finish: no callback can see
 * the record before it is complete. */
PyObject *
_PyGrail_RecordFromStackRefsStealOnSuccess(PyTypeObject *type,
                                           _PyStackRef *args, Py_ssize_t n)
{
    return _PyGrail_RecordFromStackRefsCheckedStealOnSuccess(type, args, n,
                                                             UINT64_MAX);
}

/* Tier-2 _CALL_RECORD_CHECKED: the optimizer has shown that the fields whose
 * bit is clear in check conform (values built or loaded in the same trace),
 * so only the others are checked. Leaf plans are checked inline. */
PyObject *
_PyGrail_RecordFromStackRefsCheckedStealOnSuccess(PyTypeObject *type,
                                                  _PyStackRef *args, Py_ssize_t n,
                                                  uint64_t check)
{
    assert(PyTuple_GET_SIZE(schema_part(type, 1)) == n);
    const PlanNode *nodes = schema_nodes(type);
    int conforming = 1;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (i < 64 && !(check & ((uint64_t)1 << i))) {
            continue;
        }
        const PlanNode *p = &nodes[i];
        PyObject *value = PyStackRef_AsPyObjectBorrow(args[i]);
        int ok = p->kind <= PLAN_RECORD ? plan_conforms_leaf(p, value) :
                 plan_conforms(nodes, p, value);
        if (ok < 0) {
            return NULL;
        }
        conforming &= ok;
    }
    RecordObject *self = record_alloc(type, n);
    if (self == NULL) {
        return NULL;
    }
    self->conforming = conforming;
    for (Py_ssize_t i = 0; i < n; i++) {
        self->fields[i] = PyStackRef_AsPyObjectSteal(args[i]);
    }
    _PyObject_GC_TRACK(self);
    return (PyObject *)self;
}

/* Index of the field called name, n if there is none, or -1 on error.
 * Field names are interned at declaration and keyword names usually are, so
 * a pointer pass finds them; the comparison pass covers other strings. */
static Py_ssize_t
field_index(PyObject *names, PyObject *name)
{
    Py_ssize_t n = PyTuple_GET_SIZE(names);
    for (Py_ssize_t i = 0; i < n; i++) {
        if (PyTuple_GET_ITEM(names, i) == name) {
            return i;
        }
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        int eq = PyObject_RichCompareBool(name, PyTuple_GET_ITEM(names, i), Py_EQ);
        if (eq != 0) {
            return eq < 0 ? -1 : i;
        }
    }
    return n;
}

/* Class pattern with a record type (MATCH_CLASS). Record types are final
 * frozen types with the standard metaclass, member descriptors for exactly
 * the fields and __match_args__ equal to the field names. A record of
 * another type is therefore not an instance (its __class__ cannot be
 * overridden), positional sub-pattern i of an exact match is field i and a
 * keyword naming a field reads that field.
 * Returns 1 with *result set (the attribute tuple, NULL for no match, or NULL
 * with an error), or 0 when the generic path must run: a non-record type or
 * subject, too many positional sub-patterns, a keyword that is not a field or
 * repeats a positional field, or a cleared record. The generic path then
 * reports any error. */
int
_PyGrail_MatchRecord(PyObject *subject, PyObject *type, Py_ssize_t nargs,
                     PyObject *kwargs, PyObject **result)
{
    if (!_PyGrail_IsRecordType(type)) {
        return 0;
    }
    if (Py_TYPE(subject) != (PyTypeObject *)type) {
        if (!_PyGrail_IsRecordType((PyObject *)Py_TYPE(subject))) {
            return 0;
        }
        *result = NULL;
        return 1;
    }
    Py_ssize_t n = record_count(subject);
    Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwargs);
    if (nargs > n) {
        return 0;
    }
    PyObject *attrs = PyTuple_New(nargs + nkwargs);
    if (attrs == NULL) {
        *result = NULL;
        return 1;
    }
    PyObject **fields = ((RecordObject *)subject)->fields;
    for (Py_ssize_t i = 0; i < nargs; i++) {
        if (fields[i] == NULL) {
            goto generic;
        }
        PyTuple_SET_ITEM(attrs, i, Py_NewRef(fields[i]));
    }
    PyObject *names = schema_part((PyTypeObject *)type, 0);
    for (Py_ssize_t k = 0; k < nkwargs; k++) {
        Py_ssize_t i = field_index(names, PyTuple_GET_ITEM(kwargs, k));
        if (i < 0) {
            Py_DECREF(attrs);
            *result = NULL;
            return 1;
        }
        if (i < nargs || i == n || fields[i] == NULL) {
            goto generic;
        }
        PyTuple_SET_ITEM(attrs, nargs + k, Py_NewRef(fields[i]));
    }
    *result = attrs;
    return 1;
generic:
    Py_DECREF(attrs);
    return 0;
}

/* A class pattern with positional sub-patterns only compiles to
 *   MATCH_CLASS n; COPY 1; POP_JUMP_IF_NONE; NOT_TAKEN; UNPACK_SEQUENCE n
 * (the jump made from COPY/IS_OP/POP_JUMP_IF_FALSE by the flow graph). At such
 * a site MATCH_CLASS_RECORD pushes the matched record itself instead of an
 * attribute tuple, and the UNPACK_SEQUENCE reads its leading fields. A long
 * jump or field count adds EXTENDED_ARG; those sites are not specialized.
 * Base code units ignore specialization, instrumentation and executors. */
int
_PyGrail_IsRecordMatchSite(PyCodeObject *code, Py_ssize_t match, int nargs)
{
    static const struct { int at, opcode, arg; } layout[] = {
        {0, MATCH_CLASS, -1}, {2, COPY, 1}, {3, POP_JUMP_IF_NONE, -2},
        {5, NOT_TAKEN, -2}, {_PyGrail_MATCH_TO_UNPACK, UNPACK_SEQUENCE, -1},
    };
    assert(_PyOpcode_Caches[MATCH_CLASS] == 1);
    assert(_PyOpcode_Caches[POP_JUMP_IF_NONE] == 1);
    if (match < 0 || match + _PyGrail_MATCH_TO_UNPACK >= Py_SIZE(code)) {
        return 0;
    }
    for (size_t k = 0; k < Py_ARRAY_LENGTH(layout); k++) {
        _Py_CODEUNIT unit = _Py_GetBaseCodeUnit(code, (int)match + layout[k].at);
        int arg = layout[k].arg == -1 ? nargs : layout[k].arg;
        if (unit.op.code != layout[k].opcode || (arg >= 0 && unit.op.arg != arg)) {
            return 0;
        }
    }
    return 1;
}

/* MATCH_CLASS_RECORD: a record subject against a record type with only
 * positional sub-patterns (names is the empty tuple), at a site laid out as
 * above, as the instruction's guard and specializer ensure. On a match with
 * every field present it returns the subject; the UNPACK_SEQUENCE then takes
 * its first nargs fields. Otherwise the result is _PyEval_MatchClass's (an
 * attribute tuple, None, or an error for too many sub-patterns). */
PyObject *
_PyGrail_MatchRecordPositional(PyThreadState *tstate, PyObject *subject,
                               PyObject *type, Py_ssize_t nargs, PyObject *names)
{
    assert(_PyGrail_IsRecordType(type));
    assert(_PyGrail_IsRecordType((PyObject *)Py_TYPE(subject)));
    assert(PyTuple_GET_SIZE(names) == 0);
    if (Py_TYPE(subject) != (PyTypeObject *)type) {
        return NULL;
    }
    if (nargs <= record_count(subject)) {
        PyObject **fields = ((RecordObject *)subject)->fields;
        Py_ssize_t i = 0;
        while (i < nargs && fields[i] != NULL) {
            i++;
        }
        if (i == nargs) {
            return Py_NewRef(subject);
        }
    }
    return _PyEval_MatchClass(tstate, subject, type, nargs, names);
}

/* Generic UNPACK_SEQUENCE of a record: only a MATCH_CLASS_RECORD result at a
 * record match site is unpackable (generic MATCH_CLASS always produces a
 * tuple there, so this is that result). Writes the first n fields below sp,
 * field 0 on top, as _PyEval_UnpackIterableStackRef does, and returns 1;
 * returns 0 (ordinary unpacking, which raises) anywhere else. */
int
_PyGrail_UnpackMatchedRecord(_PyInterpreterFrame *frame, PyObject *record,
                             int n, _PyStackRef *sp)
{
    PyCodeObject *code = _PyFrame_GetCode(frame);
    Py_ssize_t unpack = frame->instr_ptr - _PyFrame_GetBytecode(frame);
    if (!_PyGrail_IsRecordMatchSite(code, unpack - _PyGrail_MATCH_TO_UNPACK, n)) {
        return 0;
    }
    assert(n <= record_count(record));
    PyObject **fields = ((RecordObject *)record)->fields;
    for (int i = 0; i < n; i++) {
        assert(fields[i] != NULL);
        *--sp = PyStackRef_FromPyObjectNew(fields[i]);
    }
    return 1;
}

/* The exact type of the field at byte offset `offset` of a conforming
 * instance of record type `type`, or NULL when its plan does not name one
 * (no plan or a union). Tier-2 traces type a field load with it after
 * checking the instance's conformance bit; plans are frozen. Borrowed. */
PyTypeObject *
_PyGrail_ConformingFieldType(PyTypeObject *type, Py_ssize_t offset)
{
    if (!_PyGrail_IsRecordTypeFast(type)) {
        return NULL;
    }
    Py_ssize_t i = (offset - (Py_ssize_t)offsetof(RecordObject, fields)) /
                   (Py_ssize_t)sizeof(PyObject *);
    if (i < 0 || i >= PyTuple_GET_SIZE(schema_part(type, 1))) {
        return NULL;
    }
    const PlanNode *p = &schema_nodes(type)[i];
    switch (p->kind) {
    case PLAN_EXACT:
    case PLAN_RECORD:
        return p->type;
    case PLAN_TUPLE:
    case PLAN_VARTUPLE:
        return &PyTuple_Type;
    default:
        return NULL;
    }
}

/* Element-wise arithmetic kernels (grail.record(deriving=...)) for record
 * types whose fields are all float. A kernel is an ordinary registered
 * exact-pair implementation of one central operator:
 *   kind 0: (R, R)        field op field   (+, -)
 *   kind 1: (R, scalar)   field op scalar  (*, /; scalar is an exact float/int)
 *   kind 2: (scalar, R)   scalar op field  (*)
 * When every record operand conforms (all fields are exact floats), the
 * result's fields are computed on doubles and the result is conforming.
 * Otherwise, and for division by zero, each field uses the ordinary operator
 * (PyNumber_*) and the result is built like a constructor call, so results
 * and errors match a Python implementation that does the same per field. */
typedef struct {
    PyObject_HEAD
    PyTypeObject *record_type;
    int op;
    int kind;
    vectorcallfunc vectorcall;
} KernelObject;

enum { KERNEL_ADD, KERNEL_SUB, KERNEL_MUL, KERNEL_DIV };

static inline double
kernel_double_op(int op, double x, double y)
{
    switch (op) {
        case KERNEL_ADD: return x + y;
        case KERNEL_SUB: return x - y;
        case KERNEL_MUL: return x * y;
        default: return x / y;
    }
}

static PyObject *
kernel_number_op(int op, PyObject *x, PyObject *y)
{
    switch (op) {
        case KERNEL_ADD: return PyNumber_Add(x, y);
        case KERNEL_SUB: return PyNumber_Subtract(x, y);
        case KERNEL_MUL: return PyNumber_Multiply(x, y);
        default: return PyNumber_TrueDivide(x, y);
    }
}

/* The kernel is read once on entry: callers may only borrow it, and the
 * allocations below can run arbitrary code. The record operand keeps the
 * record type alive. */
PyObject *
_PyGrail_KernelApply(PyObject *kernel, PyObject *a, PyObject *b)
{
    KernelObject *k = (KernelObject *)kernel;
    PyTypeObject *type = k->record_type;
    int op = k->op, kind = k->kind;
    PyObject *rec = kind == 2 ? b : a;
    PyObject *other = kind == 2 ? a : b;
    if (type == NULL || Py_TYPE(rec) != type ||
        (kind == 0 ? Py_TYPE(other) != type :
         !PyFloat_CheckExact(other) && !PyLong_CheckExact(other))) {
        PyErr_Format(PyExc_TypeError,
                     "element-wise kernel does not support '%.100s' and '%.100s'",
                     Py_TYPE(a)->tp_name, Py_TYPE(b)->tp_name);
        return NULL;
    }
    PyObject **x = ((RecordObject *)rec)->fields;
    PyObject **y = kind == 0 ? ((RecordObject *)other)->fields : NULL;
    int fast = ((RecordObject *)rec)->conforming &&
               (kind != 0 || ((RecordObject *)other)->conforming);
    double scalar = 0.0;
    if (fast && kind != 0) {
        if (PyFloat_CheckExact(other)) {
            scalar = PyFloat_AS_DOUBLE(other);
        }
        else {
            scalar = PyLong_AsDouble(other);
            if (scalar == -1.0 && PyErr_Occurred()) {
                return NULL;
            }
        }
        /* The ordinary operator raises ZeroDivisionError. */
        fast = !(op == KERNEL_DIV && scalar == 0.0);
    }
    Py_ssize_t n = record_count(rec);
    PyObject *small[8];
    PyObject **values = small;
    if (n > (Py_ssize_t)Py_ARRAY_LENGTH(small)) {
        values = PyMem_Malloc(n * sizeof(PyObject *));
        if (values == NULL) {
            return PyErr_NoMemory();
        }
    }
    PyObject *result = NULL;
    Py_ssize_t filled;
    for (filled = 0; filled < n; filled++) {
        PyObject *value;
        if (fast) {
            double field = PyFloat_AS_DOUBLE(x[filled]);
            double partner = y != NULL ? PyFloat_AS_DOUBLE(y[filled]) : scalar;
            value = PyFloat_FromDouble(kind == 2 ?
                kernel_double_op(op, partner, field) :
                kernel_double_op(op, field, partner));
        }
        else if (kind == 2) {
            value = kernel_number_op(op, other, x[filled]);
        }
        else {
            value = kernel_number_op(op, x[filled], y != NULL ? y[filled] : other);
        }
        if (value == NULL) {
            goto done;
        }
        values[filled] = value;
    }
    if (fast) {
        /* Every field is a new exact float: conforming by construction. */
        RecordObject *self = record_alloc(type, n);
        if (self != NULL) {
            self->conforming = 1;
            for (Py_ssize_t i = 0; i < n; i++) {
                self->fields[i] = values[i];
            }
            filled = 0;
            _PyObject_GC_TRACK(self);
            result = (PyObject *)self;
        }
    }
    else {
        result = record_finish(type, values);
    }
done:
    for (Py_ssize_t i = 0; i < filled; i++) {
        Py_DECREF(values[i]);
    }
    if (values != small) {
        PyMem_Free(values);
    }
    return result;
}

/* Number of fields of a record type whose plans are all exactly float, or
 * -1. Plans are frozen, so the tier-2 optimizer can rely on the answer. */
Py_ssize_t
_PyGrail_FloatRecordFieldCount(PyTypeObject *type)
{
    if (!_PyGrail_IsRecordTypeFast(type)) {
        return -1;
    }
    PyObject *plans = schema_part(type, 1);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(plans); i++) {
        if (PyTuple_GET_ITEM(plans, i) != (PyObject *)&PyFloat_Type) {
            return -1;
        }
    }
    return PyTuple_GET_SIZE(plans);
}

/* Tier 2: an inlined operator implementation the optimizer recognized as
 * element-wise float arithmetic on a record type whose fields are all float
 * (the kinds of the kernels above; scalars are exact floats). The trace
 * guarantees the operand types and that every record operand conforms, so
 * each field is computed on doubles exactly as the Python implementation
 * does. reuse, if not NULL, is a record operand whose only reference the
 * caller owns: its fields are overwritten (in place for floats nothing
 * else references) and it is returned, borrowed. Otherwise the result is a
 * new conforming record. */
PyObject *
_PyGrail_ElementwiseFloat(PyTypeObject *type, int op, int kind,
                          PyObject *a, PyObject *b, PyObject *reuse)
{
    assert(Py_IS_TYPE(kind == 2 ? b : a, type));
    assert(kind == 0 ? Py_IS_TYPE(b, type) :
           PyFloat_CheckExact(kind == 1 ? b : a));
    RecordObject *rec = (RecordObject *)(kind == 2 ? b : a);
    PyObject **x = rec->fields;
    PyObject **y = kind == 0 ? ((RecordObject *)b)->fields : NULL;
    double scalar = kind == 1 ? PyFloat_AS_DOUBLE(b) :
                    kind == 2 ? PyFloat_AS_DOUBLE(a) : 0.0;
    Py_ssize_t n = record_count((PyObject *)rec);
    assert(rec->conforming && (y == NULL || ((RecordObject *)b)->conforming));
    if (reuse != NULL) {
        assert(reuse == a || reuse == b);
        assert(Py_IS_TYPE(reuse, type) && _PyObject_IsUniquelyReferenced(reuse));
        RecordObject *self = (RecordObject *)reuse;
        for (Py_ssize_t i = 0; i < n; i++) {
            double field = PyFloat_AS_DOUBLE(x[i]);
            double partner = y != NULL ? PyFloat_AS_DOUBLE(y[i]) : scalar;
            double value = kind == 2 ? kernel_double_op(op, partner, field) :
                                       kernel_double_op(op, field, partner);
            PyObject *old = self->fields[i];
            if (_PyObject_IsUniquelyReferenced(old)) {
                ((PyFloatObject *)old)->ob_fval = value;
                continue;
            }
            PyObject *f = PyFloat_FromDouble(value);
            if (f == NULL) {
                /* Fields 0..i-1 changed, but nothing else can see them. */
                return NULL;
            }
            self->fields[i] = f;
            Py_DECREF(old);  /* referenced elsewhere: not freed */
        }
        self->hash_tag = 0;
        return reuse;
    }
    RecordObject *self = record_alloc(type, n);
    if (self == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        double field = PyFloat_AS_DOUBLE(x[i]);
        double partner = y != NULL ? PyFloat_AS_DOUBLE(y[i]) : scalar;
        PyObject *f = PyFloat_FromDouble(kind == 2 ?
            kernel_double_op(op, partner, field) :
            kernel_double_op(op, field, partner));
        if (f == NULL) {
            for (Py_ssize_t j = 0; j < i; j++) {
                Py_DECREF(self->fields[j]);
            }
            record_free(self, type);
            _Py_DECREF_TYPE(type);
            return NULL;
        }
        self->fields[i] = f;
    }
    /* Every field is a new exact float: conforming by construction. */
    self->conforming = 1;
    _PyObject_GC_TRACK(self);
    return (PyObject *)self;
}

static PyMutex float_programs_mutex;
static const _PyGrailFloatProgram *float_programs[512];
static int float_programs_count;

/* A persistent copy of program (types referenced), or NULL when the table
 * is full. The optimizer passes it to _GRAIL_FLOAT_PROGRAM as a constant. */
const _PyGrailFloatProgram *
_PyGrail_InternFloatProgram(const _PyGrailFloatProgram *program)
{
    _PyGrailFloatProgram key;
    memset(&key, 0, sizeof(key));
    memcpy(key.types, program->types, sizeof(key.types));
    key.nargs = program->nargs;
    key.len = program->len;
    memcpy(key.code, program->code, program->len);
    const _PyGrailFloatProgram *result = NULL;
    PyMutex_Lock(&float_programs_mutex);
    for (int i = 0; i < float_programs_count; i++) {
        if (memcmp(float_programs[i], &key, sizeof(key)) == 0) {
            result = float_programs[i];
            goto done;
        }
    }
    if (float_programs_count < (int)Py_ARRAY_LENGTH(float_programs)) {
        _PyGrailFloatProgram *copy = PyMem_RawMalloc(sizeof(key));
        if (copy != NULL) {
            *copy = key;
            for (int i = 0; i < key.nargs; i++) {
                Py_INCREF(key.types[i]);
            }
            float_programs[float_programs_count++] = copy;
            result = copy;
        }
    }
done:
    PyMutex_Unlock(&float_programs_mutex);
    return result;
}

/* The arguments have the program's exact types and record arguments
 * conform, so every field it loads is an exact float. */
int
_PyGrail_FloatProgramArgsOk(const _PyGrailFloatProgram *program,
                            _PyStackRef *args)
{
    for (int i = 0; i < program->nargs; i++) {
        PyObject *arg = PyStackRef_AsPyObjectBorrow(args[i]);
        PyTypeObject *type = program->types[i];
        if (Py_TYPE(arg) != type ||
            (type != &PyFloat_Type && !((RecordObject *)arg)->conforming)) {
            return 0;
        }
    }
    return 1;
}

/* One float operation per program step, in program order, so the result is
 * bit for bit what the Python function computes. */
double
_PyGrail_FloatProgramEval(const _PyGrailFloatProgram *program,
                          _PyStackRef *args)
{
    double stack[_PyGrail_FLOAT_PROGRAM_MAX];
    int sp = 0;
    for (int i = 0; i < program->len; i++) {
        uint8_t c = program->code[i];
        switch (c >> 6) {
            case 0: {
                PyObject *arg = PyStackRef_AsPyObjectBorrow(args[(c >> 4) & 3]);
                int field = c & 15;
                stack[sp++] = PyFloat_AS_DOUBLE(field == _PyGrail_FLOAT_SCALAR ? arg :
                                                ((RecordObject *)arg)->fields[field]);
                break;
            }
            case 1:
                sp--;
                stack[sp - 1] = stack[sp - 1] + stack[sp];
                break;
            case 2:
                sp--;
                stack[sp - 1] = stack[sp - 1] - stack[sp];
                break;
            default:
                sp--;
                stack[sp - 1] = stack[sp - 1] * stack[sp];
                break;
        }
    }
    assert(sp == 1);
    return stack[0];
}

void
_PyGrail_KernelDealloc(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    Py_CLEAR(((KernelObject *)self)->record_type);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static int
kernel_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(((KernelObject *)self)->record_type);
    return 0;
}

static int
kernel_clear(PyObject *self)
{
    Py_CLEAR(((KernelObject *)self)->record_type);
    return 0;
}

static PyObject *
kernel_vectorcall(PyObject *self, PyObject *const *args, size_t nargsf,
                  PyObject *kwnames)
{
    if (kwnames != NULL || PyVectorcall_NARGS(nargsf) != 2) {
        PyErr_SetString(PyExc_TypeError,
                        "element-wise kernel takes exactly two positional arguments");
        return NULL;
    }
    return _PyGrail_KernelApply(self, args[0], args[1]);
}

static PyObject *
kernel_repr(PyObject *self)
{
    KernelObject *k = (KernelObject *)self;
    static const char symbols[] = "+-*/";
    const char *name = k->record_type == NULL ? "?" : k->record_type->tp_name;
    const char *lhs = k->kind == 2 ? "scalar" : name;
    const char *rhs = k->kind == 1 ? "scalar" : name;
    return PyUnicode_FromFormat("<grail element-wise kernel %s %c %s>",
                                lhs, symbols[k->op], rhs);
}

static PyMemberDef kernel_members[] = {
    {"__vectorcalloffset__", Py_T_PYSSIZET, offsetof(KernelObject, vectorcall),
     Py_READONLY},
    {NULL}
};

int
_PyGrail_InitKernels(PyObject *module)
{
    PyType_Slot slots[] = {
        {Py_tp_dealloc, _PyGrail_KernelDealloc},
        {Py_tp_traverse, kernel_traverse},
        {Py_tp_clear, kernel_clear},
        {Py_tp_call, PyVectorcall_Call},
        {Py_tp_repr, kernel_repr},
        {Py_tp_members, kernel_members},
        {0, NULL}
    };
    PyType_Spec spec = {
        .name = "_grail.Kernel",
        .basicsize = sizeof(KernelObject),
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                 Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_IMMUTABLETYPE |
                 Py_TPFLAGS_DISALLOW_INSTANTIATION,
        .slots = slots,
    };
    GrailState *state = PyModule_GetState(module);
    state->kernel_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &spec, NULL);
    return state->kernel_type == NULL ? -1 : 0;
}

/* _grail._kernel(record_type, op, kind): op 0-3 is + - * /. */
PyObject *
_PyGrail_MakeKernel(PyObject *module, PyObject *args)
{
    PyObject *type;
    int op, kind;
    if (!PyArg_ParseTuple(args, "Oii:_kernel", &type, &op, &kind) ||
        require_record_type(type) < 0) {
        return NULL;
    }
    if (op < KERNEL_ADD || op > KERNEL_DIV || kind < 0 || kind > 2 ||
        (kind == 0) != (op == KERNEL_ADD || op == KERNEL_SUB) ||
        (kind == 2 && op != KERNEL_MUL)) {
        PyErr_SetString(PyExc_ValueError, "unsupported element-wise operation");
        return NULL;
    }
    PyObject *plans = schema_part((PyTypeObject *)type, 1);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(plans); i++) {
        if (PyTuple_GET_ITEM(plans, i) != (PyObject *)&PyFloat_Type) {
            PyErr_Format(PyExc_TypeError,
                         "element-wise arithmetic needs every field of %s "
                         "to be annotated float", ((PyTypeObject *)type)->tp_name);
            return NULL;
        }
    }
    GrailState *state = PyModule_GetState(module);
    KernelObject *k = PyObject_GC_New(KernelObject, state->kernel_type);
    if (k == NULL) {
        return NULL;
    }
    k->record_type = (PyTypeObject *)Py_NewRef(type);
    k->op = op;
    k->kind = kind;
    k->vectorcall = kernel_vectorcall;
    PyObject_GC_Track(k);
    return (PyObject *)k;
}

PyObject *
_PyGrail_IsKernelPy(PyObject *module, PyObject *obj)
{
    return PyBool_FromLong(_PyGrail_IsKernel(obj));
}

static PyObject *record_vectorcall_general(PyObject *, PyObject *const *,
                                           size_t, PyObject *);

/* Every field passed positionally: no staging buffer, defaults or extra
 * references. record_finish borrows the arguments, and nothing can call back
 * before the record is complete. */
static PyObject *
record_vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf,
                  PyObject *kwnames)
{
    PyTypeObject *type = (PyTypeObject *)callable;
    if (kwnames != NULL ||
        PyVectorcall_NARGS(nargsf) != PyTuple_GET_SIZE(schema_part(type, 1))) {
        return record_vectorcall_general(callable, args, nargsf, kwnames);
    }
    return record_finish(type, args);
}

static PyObject *
record_vectorcall_general(PyObject *callable, PyObject *const *args,
                          size_t nargsf, PyObject *kwnames)
{
    PyTypeObject *type = (PyTypeObject *)callable;
    PyObject *names = schema_part(type, 0);
    PyObject *defaults = schema_part(type, 2);
    Py_ssize_t n = PyTuple_GET_SIZE(names);
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs > n) {
        PyErr_Format(PyExc_TypeError, "%s expects at most %zd positional arguments",
                     type->tp_name, n);
        return NULL;
    }
    PyObject *small[8] = {NULL};
    PyObject **values = small;
    if (n > (Py_ssize_t)Py_ARRAY_LENGTH(small)) {
        values = PyMem_Calloc(n, sizeof(PyObject *));
        if (values == NULL) {
            return PyErr_NoMemory();
        }
    }
    PyObject *result = NULL;
    for (Py_ssize_t i = 0; i < nargs; i++) {
        values[i] = Py_NewRef(args[i]);
    }
    if (kwnames != NULL) {
        for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(kwnames); k++) {
            PyObject *key = PyTuple_GET_ITEM(kwnames, k);
            Py_ssize_t i = field_index(names, key);
            if (i < 0) {
                goto done;
            }
            if (i == n) {
                PyErr_Format(PyExc_TypeError, "unexpected record field %R", key);
                goto done;
            }
            if (values[i] != NULL) {
                PyErr_Format(PyExc_TypeError, "multiple values for field %R", key);
                goto done;
            }
            values[i] = Py_NewRef(args[nargs + k]);
        }
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        if (values[i] != NULL) {
            continue;
        }
        PyObject *entry = PyTuple_GET_ITEM(defaults, i);
        long kind = PyLong_AsLong(PyTuple_GET_ITEM(entry, 0));
        PyObject *value = PyTuple_GET_ITEM(entry, 1);
        if (kind == 0) {
            PyErr_Format(PyExc_TypeError, "missing required record field %R",
                         PyTuple_GET_ITEM(names, i));
            goto done;
        }
        values[i] = kind == 2 ? PyObject_CallNoArgs(value) : Py_NewRef(value);
        if (values[i] == NULL) {
            goto done;
        }
    }
    result = record_finish(type, values);
done:
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_XDECREF(values[i]);
    }
    if (values != small) {
        PyMem_Free(values);
    }
    return result;
}

static PyObject *
record_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    /* Also covers direct R.__new__(R, ...) calls, without exposing staging. */
    return PyVectorcall_Call((PyObject *)type, args, kwargs);
}

static int
record_traverse(RecordObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    for (Py_ssize_t i = 0; i < record_count((PyObject *)self); i++) {
        Py_VISIT(self->fields[i]);
    }
    return 0;
}

static int
record_clear(RecordObject *self)
{
    for (Py_ssize_t i = 0; i < record_count((PyObject *)self); i++) {
        Py_CLEAR(self->fields[i]);
    }
    return 0;
}

static void
record_dealloc(RecordObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_BEGIN(self, record_dealloc);
    /* As record_clear, but float fields (the common numeric case) are
     * released without the generic deallocation dispatch. Fields are NULL
     * after tp_clear. */
    for (Py_ssize_t i = 0; i < record_count((PyObject *)self); i++) {
        PyObject *field = self->fields[i];
        if (field == NULL) {
            continue;
        }
        if (PyFloat_CheckExact(field)) {
            _Py_DECREF_SPECIALIZED(field, _PyFloat_ExactDealloc);
        }
        else {
            Py_DECREF(field);
        }
    }
    record_free(self, type);
    /* Pairs with the per-thread type reference taken at allocation; a plain
     * Py_DECREF would contend on the shared type refcount across threads. */
    _Py_DECREF_TYPE(type);
    Py_TRASHCAN_END;
}

static int
record_setattr(PyObject *self, PyObject *name, PyObject *value)
{
    PyErr_SetString(PyExc_AttributeError, "record fields are immutable");
    return -1;
}

static int record_substitutable(PyObject *, PyObject *);

/* p is NULL when the values have no plan to compare under. */
static int
value_substitutable(const PlanNode *nodes, const PlanNode *p,
                    PyObject *a, PyObject *b)
{
    if (a == b) {
        return 1;
    }
    if (p == NULL || p->kind == PLAN_ANY || Py_TYPE(a) != Py_TYPE(b)) {
        return 0;
    }
    if (p->kind == PLAN_EXACT || p->kind == PLAN_RECORD) {
        if (Py_TYPE(a) != p->type) {
            return 0;
        }
        if (p->kind == PLAN_RECORD) {
            return record_substitutable(a, b);
        }
        if (PyFloat_CheckExact(a)) {
            double x = PyFloat_AS_DOUBLE(a), y = PyFloat_AS_DOUBLE(b);
            return memcmp(&x, &y, sizeof(double)) == 0;
        }
        return PyObject_RichCompareBool(a, b, Py_EQ);
    }
    const PlanNode *children = nodes + p->first;
    if (p->kind == PLAN_UNION) {
        for (uint32_t i = 0; i < p->n; i++) {
            int ok = plan_conforms(nodes, &children[i], a);
            if (ok < 0) {
                return -1;
            }
            if (ok) {
                return value_substitutable(nodes, &children[i], a, b);
            }
        }
        return 0;
    }
    if (!PyTuple_CheckExact(a) || PyTuple_GET_SIZE(a) != PyTuple_GET_SIZE(b)) {
        return 0;
    }
    if (Py_EnterRecursiveCall(" while comparing record values")) {
        return -1;
    }
    int var = p->kind == PLAN_VARTUPLE;
    int result = 1;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(a); i++) {
        result = value_substitutable(nodes, &children[var ? 0 : i],
                                     PyTuple_GET_ITEM(a, i), PyTuple_GET_ITEM(b, i));
        if (result != 1) {
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return result;
}

static int
record_substitutable(PyObject *a, PyObject *b)
{
    if (a == b) {
        return 1;
    }
    if (Py_TYPE(a) != Py_TYPE(b)) {
        return 0;
    }
    if (Py_EnterRecursiveCall(" while comparing records")) {
        return -1;
    }
    RecordObject *x = (RecordObject *)a, *y = (RecordObject *)b;
    const PlanNode *nodes = schema_nodes(Py_TYPE(a));
    int both = x->conforming && y->conforming;
    int result = 1;
    for (Py_ssize_t i = 0; i < record_count(a); i++) {
        result = value_substitutable(nodes, both ? &nodes[i] : NULL,
                                     x->fields[i], y->fields[i]);
        if (result != 1) {
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return result;
}

PyObject *
_PyGrail_Substitutable(PyObject *module, PyObject *args)
{
    PyObject *a, *b;
    if (!PyArg_ParseTuple(args, "OO:substitutable", &a, &b)) {
        return NULL;
    }
    if (require_record(a) < 0 || require_record(b) < 0) {
        return NULL;
    }
    int result = record_substitutable(a, b);
    return result < 0 ? NULL : PyBool_FromLong(result);
}

/* Default record equality: 1, 0, -1 on error, or 2 for NotImplemented. */
static int
record_default_eq(PyObject *a, PyObject *b)
{
    if (Py_TYPE(a) != Py_TYPE(b)) {
        return 2;
    }
    if (Py_EnterRecursiveCall(" in record equality")) {
        return -1;
    }
    RecordObject *x = (RecordObject *)a, *y = (RecordObject *)b;
    const PlanNode *nodes = schema_nodes(Py_TYPE(a));
    int both = x->conforming && y->conforming;
    int result = 1;
    for (Py_ssize_t i = 0; i < record_count(a); i++) {
        result = value_substitutable(nodes, both ? &nodes[i] : NULL,
                                     x->fields[i], y->fields[i]);
        if (result == 0) {
            result = PyObject_RichCompareBool(x->fields[i], y->fields[i], Py_EQ);
        }
        if (result != 1) {
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return result;
}

PyObject *
_PyGrail_DefaultEq(PyObject *module, PyObject *args)
{
    PyObject *a, *b;
    if (!PyArg_ParseTuple(args, "OO:_default_eq", &a, &b) || require_record(a) < 0) {
        return NULL;
    }
    int result = record_default_eq(a, b);
    if (result == 2) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    return result < 0 ? NULL : PyBool_FromLong(result);
}

PyObject *
_PyGrail_DefaultRepr(PyObject *module, PyObject *obj)
{
    if (!_PyGrail_IsRecordType((PyObject *)Py_TYPE(obj))) {
        return PyObject_Repr(obj);
    }
    int entered = Py_ReprEnter(obj);
    if (entered != 0) {
        return entered < 0 ? NULL : PyUnicode_FromString("...");
    }
    RecordObject *self = (RecordObject *)obj;
    PyObject *names = schema_part(Py_TYPE(obj), 0);
    PyObject *parts = PyList_New(record_count(obj));
    PyObject *result = NULL;
    if (parts == NULL) {
        goto done;
    }
    for (Py_ssize_t i = 0; i < record_count(obj); i++) {
        PyObject *part = PyUnicode_FromFormat("%U=%R", PyTuple_GET_ITEM(names, i), self->fields[i]);
        if (part == NULL) {
            goto done;
        }
        PyList_SET_ITEM(parts, i, part);
    }
    PyObject *sep = PyUnicode_FromString(", ");
    PyObject *joined = sep == NULL ? NULL : PyUnicode_Join(sep, parts);
    Py_XDECREF(sep);
    if (joined != NULL) {
        PyObject *name = PyType_GetName(Py_TYPE(obj));
        if (name != NULL) {
            result = PyUnicode_FromFormat("%U(%U)", name, joined);
            Py_DECREF(name);
        }
        Py_DECREF(joined);
    }
done:
    Py_XDECREF(parts);
    Py_ReprLeave(obj);
    return result;
}

/* Hashes combine like tuple_hash over the lane hashes, without building
 * tuples or ints: a conforming tuple field then hashes exactly like the equal
 * tuple stored in an ordinary field. */
static inline Py_uhash_t
hash_lane(Py_uhash_t acc, Py_hash_t lane)
{
    acc += (Py_uhash_t)lane * _PyTuple_HASH_XXPRIME_2;
    acc = _PyTuple_HASH_XXROTATE(acc);
    return acc * _PyTuple_HASH_XXPRIME_1;
}

static inline Py_hash_t
hash_finish(Py_uhash_t acc, Py_ssize_t len)
{
    acc += len ^ (_PyTuple_HASH_XXPRIME_5 ^ 3527539UL);
    return acc == (Py_uhash_t)-1 ? 1546275796 : (Py_hash_t)acc;
}

/* Hash value fields according to their representation where Python's hash
 * is identity-dependent (NaNs). Never cache a hash or call a hook under locks. */
static Py_hash_t
value_hash(const PlanNode *nodes, const PlanNode *p, PyObject *value)
{
    if (p == NULL || p->kind == PLAN_ANY) {
        return PyObject_Hash(value);
    }
    if (PyFloat_CheckExact(value) && isnan(PyFloat_AS_DOUBLE(value))) {
        double bits = PyFloat_AS_DOUBLE(value);
        return Py_HashBuffer(&bits, sizeof(bits));
    }
    const PlanNode *children = nodes + p->first;
    if (p->kind == PLAN_UNION) {
        for (uint32_t i = 0; i < p->n; i++) {
            int ok = plan_conforms(nodes, &children[i], value);
            if (ok < 0) {
                return -1;
            }
            if (ok) {
                return value_hash(nodes, &children[i], value);
            }
        }
    }
    else if (p->kind == PLAN_TUPLE || p->kind == PLAN_VARTUPLE) {
        if (Py_EnterRecursiveCall(" in record hashing")) {
            return -1;
        }
        int var = p->kind == PLAN_VARTUPLE;
        Py_ssize_t len = PyTuple_GET_SIZE(value);
        Py_uhash_t acc = _PyTuple_HASH_XXPRIME_5;
        Py_hash_t result = 0;
        for (Py_ssize_t i = 0; i < len; i++) {
            Py_hash_t hash = value_hash(nodes, &children[var ? 0 : i],
                                        PyTuple_GET_ITEM(value, i));
            if (hash == -1) {
                result = -1;
                break;
            }
            acc = hash_lane(acc, hash);
        }
        Py_LeaveRecursiveCall();
        return result == -1 ? -1 : hash_finish(acc, len);
    }
    return PyObject_Hash(value);
}

static Py_hash_t
record_default_hash(PyObject *obj)
{
    if (Py_EnterRecursiveCall(" in record hashing")) {
        return -1;
    }
    RecordObject *self = (RecordObject *)obj;
    const PlanNode *nodes = schema_nodes(Py_TYPE(obj));
    Py_ssize_t n = record_count(obj);
    Py_uhash_t acc = _PyTuple_HASH_XXPRIME_5;
    Py_hash_t result = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        int ok = plan_conforms(nodes, &nodes[i], self->fields[i]);
        if (ok < 0) {
            result = -1;
            break;
        }
        /* A mismatch in another field must not change the hash of a
         * substitutable NaN/tuple here: conforming and nonconforming
         * instances can still compare equal. */
        Py_hash_t hash = value_hash(nodes, ok ? &nodes[i] : NULL, self->fields[i]);
        if (hash == -1) {
            result = -1;
            break;
        }
        acc = hash_lane(acc, hash);
    }
    Py_LeaveRecursiveCall();
    return result == -1 ? -1 : hash_finish(acc, n);
}

PyObject *
_PyGrail_DefaultHash(PyObject *module, PyObject *obj)
{
    if (require_record(obj) < 0) {
        return NULL;
    }
    Py_hash_t hash = record_default_hash(obj);
    return hash == -1 ? NULL : PyLong_FromSsize_t(hash);
}

static GrailState *
record_state(PyObject *obj)
{
    return PyType_GetModuleState(Py_TYPE(obj));
}

/* True when the protocol hook would run its native default for obj, so the
 * slot can compute it directly instead of calling the hook. Lock-free; the
 * borrowed implementation is only compared. PYTHON_GRAIL_NATIVE_HOOKS=0
 * always calls the hook. */
static int
hook_is_default(PyObject *hook, PyObject *obj, PyCFunction impl)
{
    if (_PyInterpreterState_GET()->grail_native_hooks_off) {
        return 0;
    }
    PyObject *func = _PyGrail_GenericSelect(hook, obj, 0);
    return func != NULL && PyCFunction_Check(func) &&
           PyCFunction_GET_FUNCTION(func) == impl;
}

static PyObject *
record_repr(PyObject *obj)
{
    GrailState *state = record_state(obj);
    return PyObject_CallOneArg(state->repr_hook, obj);
}

static PyObject *
record_richcompare(PyObject *a, PyObject *b, int op)
{
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    GrailState *state = record_state(a);
    if (hook_is_default(state->eq_hook, a, (PyCFunction)_PyGrail_DefaultEq)) {
        int result = record_default_eq(a, b);
        if (result == 2) {
            Py_RETURN_NOTIMPLEMENTED;
        }
        return result < 0 ? NULL : PyBool_FromLong(result == (op == Py_EQ));
    }
    PyObject *args[] = {a, b};
    PyObject *result = PyObject_Vectorcall(state->eq_hook, args, 2, NULL);
    if (result == NULL || result == Py_NotImplemented || op == Py_EQ) {
        return result;
    }
    int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth < 0 ? NULL : PyBool_FromLong(!truth);
}

/* Hash caching. A conforming value record holds only values, and the
 * default hash of a value depends only on the value and on which hash
 * implementation each record type selects (hooks are trusted to be pure
 * functions of the value, as for == and hash obligations generally). Hash
 * hooks are registered only for record types, which are exact, so that
 * selection changes only with a grail.hash registration: the cached default
 * hash is tagged with the hook's generation. It is bumped after the change
 * is published, so a hash computed under a newer table can only carry an
 * older, stale tag.
 * Writers serialize on the tag (HASH_TAG_BUSY), and a reader that sees the
 * current tag (acquire) sees the hash stored before it.
 * PYTHON_GRAIL_NATIVE_HOOKS=0 disables the cache with the native default. */
#define HASH_TAG_BUSY UINT32_MAX

static inline uint32_t
hash_epoch(GrailState *state)
{
    uint64_t epoch = _PyGrail_GenericGeneration(state->hash_hook);
    uint32_t tag = (uint32_t)epoch + 1;
    /* After 2**32 registrations, stop caching rather than reuse a tag. */
    return epoch >= HASH_TAG_BUSY - 1 ? 0 : tag;
}

static void
record_cache_hash(RecordObject *self, GrailState *state, uint32_t tag,
                  Py_hash_t hash)
{
    uint32_t old = _Py_atomic_load_uint32_relaxed(&self->hash_tag);
    if (old == HASH_TAG_BUSY || hash_epoch(state) != tag ||
        !_Py_atomic_compare_exchange_uint32(&self->hash_tag, &old, HASH_TAG_BUSY)) {
        return;
    }
    _Py_atomic_store_ssize_relaxed(&self->hash, hash);
    _Py_atomic_store_uint32_release(&self->hash_tag, tag);
}

static Py_hash_t
record_hash(PyObject *obj)
{
    GrailState *state = record_state(obj);
    RecordObject *self = (RecordObject *)obj;
    uint32_t tag = 0;
    if (_PyGrail_IsConformingValueRecord(obj) &&
        !_PyInterpreterState_GET()->grail_native_hooks_off) {
        tag = hash_epoch(state);
        if (tag != 0 && _Py_atomic_load_uint32_acquire(&self->hash_tag) == tag) {
            return _Py_atomic_load_ssize_relaxed(&self->hash);
        }
    }
    if (hook_is_default(state->hash_hook, obj, _PyGrail_DefaultHash)) {
        Py_hash_t hash = record_default_hash(obj);
        if (tag != 0 && hash != -1) {
            record_cache_hash(self, state, tag, hash);
        }
        return hash;
    }
    PyObject *result = PyObject_CallOneArg(state->hash_hook, obj);
    if (result == NULL) {
        return -1;
    }
    Py_hash_t hash = PyLong_AsSsize_t(result);
    if (hash == -1 && PyErr_ExceptionMatches(PyExc_OverflowError)) {
        PyErr_Clear();
        hash = PyObject_Hash(result);
    }
    Py_DECREF(result);
    return hash == -1 && !PyErr_Occurred() ? -2 : hash;
}

static PyObject *
record_binary(PyObject *a, PyObject *b, int index)
{
    /* Exact-pair lookup now belongs to the abstract arithmetic entry points.
       Slots must not retry a registration that already returned NotImplemented. */
    Py_RETURN_NOTIMPLEMENTED;
}

static PyObject *record_add(PyObject *a, PyObject *b) { return record_binary(a, b, 0); }
static PyObject *record_sub(PyObject *a, PyObject *b) { return record_binary(a, b, 1); }
static PyObject *record_mul(PyObject *a, PyObject *b) { return record_binary(a, b, 2); }
static PyObject *record_div(PyObject *a, PyObject *b) { return record_binary(a, b, 3); }

static PyObject *
record_unary(PyObject *obj, int index)
{
    GrailState *state = record_state(obj);
    return PyObject_CallOneArg(PyTuple_GET_ITEM(state->operators, index), obj);
}
static PyObject *record_neg(PyObject *obj) { return record_unary(obj, 4); }
static PyObject *record_abs(PyObject *obj) { return record_unary(obj, 5); }

/* __replace__ and copy.replace: the arguments are borrowed for the call, so
 * record_build can read them in place. Conformance of a value against a
 * frozen plan never changes (values are immutable and a record's bit is
 * fixed), so when the source conforms only the replaced fields are checked.
 * PYTHON_GRAIL_REPLACE_CHANGED_ONLY=0 checks every field. */
static PyObject *
record_replace_impl(PyObject *obj, PyObject *const *args, PyObject *kwnames)
{
    PyTypeObject *type = Py_TYPE(obj);
    PyObject *names = schema_part(type, 0);
    const PlanNode *nodes = schema_nodes(type);
    int source_conforms = ((RecordObject *)obj)->conforming &&
                          !_PyInterpreterState_GET()->grail_replace_check_all;
    int conforming = 1;
    Py_ssize_t n = record_count(obj);
    PyObject *small[8];
    PyObject **values = small;
    if (n > (Py_ssize_t)Py_ARRAY_LENGTH(small)) {
        values = PyMem_Malloc(n * sizeof(PyObject *));
        if (values == NULL) {
            return PyErr_NoMemory();
        }
    }
    PyObject *result = NULL;
    memcpy(values, ((RecordObject *)obj)->fields, n * sizeof(PyObject *));
    Py_ssize_t nkwargs = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t k = 0; k < nkwargs; k++) {
        PyObject *key = PyTuple_GET_ITEM(kwnames, k);
        Py_ssize_t i = field_index(names, key);
        if (i < 0) {
            goto done;
        }
        if (i == n) {
            PyErr_Format(PyExc_TypeError, "unexpected record field %R", key);
            goto done;
        }
        values[i] = args[k];
        if (source_conforms && conforming) {
            int ok = plan_conforms(nodes, &nodes[i], args[k]);
            if (ok < 0) {
                goto done;
            }
            conforming = ok;
        }
    }
    result = source_conforms ? record_build(type, values, conforming) :
                               record_finish(type, values);
done:
    if (values != small) {
        PyMem_Free(values);
    }
    return result;
}

static PyObject *
record_replace(PyObject *obj, PyObject *const *args, Py_ssize_t nargs,
               PyObject *kwnames)
{
    if (nargs) {
        PyErr_SetString(PyExc_TypeError, "__replace__ accepts only keyword fields");
        return NULL;
    }
    return record_replace_impl(obj, args, kwnames);
}

/* copy.replace(obj, /, **changes) without a Python frame or kwargs dict. A
 * record's __class__ is its own type and its __replace__ is record_replace,
 * so records are replaced directly; other objects follow Lib/copy.py. */
static PyObject *
copy_replace(PyObject *module, PyObject *const *args, Py_ssize_t nargs,
             PyObject *kwnames)
{
    if (nargs != 1) {
        PyErr_Format(PyExc_TypeError,
                     "replace() takes exactly one positional argument (%zd given)",
                     nargs);
        return NULL;
    }
    PyObject *obj = args[0];
    if (_PyGrail_IsRecordType((PyObject *)Py_TYPE(obj))) {
        return record_replace_impl(obj, args + 1, kwnames);
    }
    PyObject *cls = PyObject_GetAttr(obj, &_Py_ID(__class__));
    if (cls == NULL) {
        return NULL;
    }
    PyObject *func;
    if (PyObject_GetOptionalAttr(cls, &_Py_ID(__replace__), &func) < 0) {
        Py_DECREF(cls);
        return NULL;
    }
    if (func == NULL || func == Py_None) {
        Py_XDECREF(func);
        PyObject *name = PyObject_GetAttr(cls, &_Py_ID(__name__));
        if (name != NULL) {
            PyErr_Format(PyExc_TypeError, "replace() does not support %S objects",
                         name);
            Py_DECREF(name);
        }
        Py_DECREF(cls);
        return NULL;
    }
    Py_DECREF(cls);
    PyObject *result = PyObject_Vectorcall(func, args, 1, kwnames);
    Py_DECREF(func);
    return result;
}

PyDoc_STRVAR(copy_replace__doc__,
"replace($module, obj, /, **changes)\n"
"--\n"
"\n"
"Return a new object replacing specified fields with new values.\n"
"\n"
"This is especially useful for immutable objects, like named tuples or\n"
"frozen dataclasses.");

static PyMethodDef copy_replace_def = {
    "replace", _PyCFunction_CAST(copy_replace), METH_FASTCALL | METH_KEYWORDS,
    copy_replace__doc__
};

/* grail.replace(record, /, **changes): copy.replace for Grail records only. */
static PyObject *
grail_replace(PyObject *module, PyObject *const *args, Py_ssize_t nargs,
              PyObject *kwnames)
{
    if (nargs != 1) {
        PyErr_Format(PyExc_TypeError,
                     "replace() takes exactly one positional argument (%zd given)",
                     nargs);
        return NULL;
    }
    if (!_PyGrail_IsRecordType((PyObject *)Py_TYPE(args[0]))) {
        PyErr_SetString(PyExc_TypeError, "replace() requires a Grail record");
        return NULL;
    }
    return record_replace_impl(args[0], args + 1, kwnames);
}

PyDoc_STRVAR(grail_replace__doc__,
"replace($module, obj, /, **changes)\n"
"--\n"
"\n"
"Return a Grail record with the supplied fields replaced.\n"
"\n"
"Unchanged fields are shared, and defaults and factories are not replayed.");

static PyMethodDef grail_replace_def = {
    "replace", _PyCFunction_CAST(grail_replace), METH_FASTCALL | METH_KEYWORDS,
    grail_replace__doc__
};

/* Lib/copy.py uses _grail.replace, which reports module "copy", when present.
 * PYTHON_GRAIL_COPY_REPLACE=0 leaves it out, so copy.replace stays Python. */
int
_PyGrail_AddCopyReplace(PyObject *module)
{
    PyObject *grail_modname = PyUnicode_FromString("grail");
    if (grail_modname == NULL) {
        return -1;
    }
    PyObject *record_replace_func = PyCFunction_NewEx(&grail_replace_def, module,
                                                      grail_modname);
    Py_DECREF(grail_modname);
    if (PyModule_Add(module, "replace_record", record_replace_func) < 0) {
        return -1;
    }
    const char *env = Py_GETENV("PYTHON_GRAIL_COPY_REPLACE");
    if (env != NULL && strcmp(env, "0") == 0) {
        return 0;
    }
    PyObject *modname = PyUnicode_FromString("copy");
    if (modname == NULL) {
        return -1;
    }
    PyObject *func = PyCFunction_NewEx(&copy_replace_def, module, modname);
    Py_DECREF(modname);
    return PyModule_Add(module, "replace", func);
}

static PyObject *
record_copy(PyObject *obj, PyObject *Py_UNUSED(ignored))
{
    return Py_NewRef(obj);
}

static PyObject *
record_deepcopy(PyObject *obj, PyObject *memo)
{
    if (!PyDict_Check(memo)) {
        PyErr_SetString(PyExc_TypeError, "deepcopy memo must be a dict");
        return NULL;
    }
    if (schema_part(Py_TYPE(obj), 4) == Py_True &&
        ((RecordObject *)obj)->conforming) {
        return Py_NewRef(obj);
    }
    PyObject *facade = PyImport_ImportModule("grail");
    PyObject *result = facade == NULL ? NULL :
        PyObject_CallMethod(facade, "_deepcopy_record", "OO", obj, memo);
    Py_XDECREF(facade);
    return result;
}

static PyObject *
record_reduce(PyObject *obj, PyObject *Py_UNUSED(ignored))
{
    PyObject *values = PyTuple_New(record_count(obj));
    if (values == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < record_count(obj); i++) {
        PyTuple_SET_ITEM(values, i, Py_NewRef(((RecordObject *)obj)->fields[i]));
    }
    /* Pickle memoizes mutable descendants before descending back to this
     * record. Its recursive REDUCE handling then keeps the first completed
     * record, just as recursive tuple pickling does. No shell or setter. */
    PyObject *result = PyTuple_Pack(2, (PyObject *)Py_TYPE(obj), values);
    Py_DECREF(values);
    return result;
}

static PyMethodDef record_methods[] = {
    {"__copy__", record_copy, METH_NOARGS, NULL},
    {"__deepcopy__", record_deepcopy, METH_O, NULL},
    {"__reduce__", record_reduce, METH_NOARGS, NULL},
    {"__reduce_ex__", record_reduce, METH_O, NULL},
    {"__replace__", _PyCFunction_CAST(record_replace), METH_FASTCALL | METH_KEYWORDS, NULL},
    {NULL}
};

static PyObject *make_record_type(PyObject *, PyObject *, PyObject *,
                                  PyObject *, PyObject *, PyObject *, int);

PyObject *
_PyGrail_MakeRecord(PyObject *module, PyObject *args)
{
    PyObject *source, *names, *plans, *defaults, *annotations;
    if (!PyArg_ParseTuple(args, "OOOOO:_make_record", &source, &names, &plans,
                         &defaults, &annotations)) {
        return NULL;
    }
    if (!Py_IS_TYPE(source, &PyType_Type)) {
        PyErr_SetString(PyExc_TypeError, "records require the standard type metaclass");
        return NULL;
    }
    PyObject *parts[] = {names, plans, defaults, annotations};
    for (int i = 0; i < 4; i++) {
        if (!PyTuple_CheckExact(parts[i]) ||
            PyTuple_GET_SIZE(parts[i]) != PyTuple_GET_SIZE(names)) {
            PyErr_SetString(PyExc_TypeError, "record schema requires equal-sized tuples");
            return NULL;
        }
    }
    Py_ssize_t n = PyTuple_GET_SIZE(names);
    if ((size_t)n > (INT_MAX - offsetof(RecordObject, fields)) / sizeof(PyObject *)) {
        return PyErr_NoMemory();
    }
    int eligible = 1;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *name = PyTuple_GET_ITEM(names, i);
        if (!PyUnicode_CheckExact(name) || !PyUnicode_IsIdentifier(name)) {
            PyErr_SetString(PyExc_TypeError, "record fields must be identifiers");
            return NULL;
        }
        const char *s = PyUnicode_AsUTF8(name);
        if (s == NULL) {
            return NULL;
        }
        if (s[0] == '_' && s[1] == '_') {
            PyErr_SetString(PyExc_TypeError, "dunder record fields are reserved");
            return NULL;
        }
        for (Py_ssize_t j = 0; j < i; j++) {
            int eq = PyObject_RichCompareBool(name, PyTuple_GET_ITEM(names, j), Py_EQ);
            if (eq != 0) {
                if (eq > 0) {
                    PyErr_SetString(PyExc_TypeError, "duplicate record field");
                }
                return NULL;
            }
        }
        PyObject *plan = PyTuple_GET_ITEM(plans, i);
        int ok = plan_check(plan);
        if (ok < 0 || PyErr_Occurred()) {
            return NULL;
        }
        if (plan != Py_None && !ok) {
            PyErr_SetString(PyExc_TypeError, "invalid native value plan");
            return NULL;
        }
        eligible &= plan != Py_None;
        PyObject *entry = PyTuple_GET_ITEM(defaults, i);
        if (!PyTuple_CheckExact(entry) || PyTuple_GET_SIZE(entry) != 2 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(entry, 0))) {
            PyErr_SetString(PyExc_TypeError, "invalid record default");
            return NULL;
        }
        long kind = PyLong_AsLong(PyTuple_GET_ITEM(entry, 0));
        if (kind < 0 || kind > 2 ||
            (kind == 2 && !PyCallable_Check(PyTuple_GET_ITEM(entry, 1)))) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_TypeError, "invalid record default kind/factory");
            }
            return NULL;
        }
    }
    /* Interned names let keyword lookups (field_index) match by pointer. */
    PyObject *interned = PyTuple_New(n);
    if (interned == NULL) {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *name = Py_NewRef(PyTuple_GET_ITEM(names, i));
        _PyUnicode_InternMortal(interp, &name);
        PyTuple_SET_ITEM(interned, i, name);
    }
    PyObject *result = make_record_type(module, source, interned, plans,
                                        defaults, annotations, eligible);
    Py_DECREF(interned);
    return result;
}

static PyObject *
make_record_type(PyObject *module, PyObject *source, PyObject *names,
                 PyObject *plans, PyObject *defaults, PyObject *annotations,
                 int eligible)
{
    Py_ssize_t n = PyTuple_GET_SIZE(names);
    GrailState *state = PyModule_GetState(module);
    int configured;
    Py_BEGIN_CRITICAL_SECTION(module);
    configured = state->operators != NULL;
    Py_END_CRITICAL_SECTION();
    if (!configured) {
        PyErr_SetString(PyExc_RuntimeError, "import grail before creating records");
        return NULL;
    }
    PyObject *name = PyObject_GetAttrString(source, "__name__");
    PyObject *modname = PyObject_GetAttrString(source, "__module__");
    if (name != NULL && modname != NULL &&
        (!PyUnicode_Check(name) || !PyUnicode_Check(modname))) {
        PyErr_SetString(PyExc_TypeError, "record name and module must be strings");
        Py_CLEAR(name);
    }
    PyObject *qualified = name != NULL && modname != NULL ?
                          PyUnicode_FromFormat("%U.%U", modname, name) : NULL;
    Py_XDECREF(name);
    Py_XDECREF(modname);
    if (qualified == NULL) {
        return NULL;
    }
    PyMemberDef *members = PyMem_Calloc(n + 1, sizeof(PyMemberDef));
    if (members == NULL) {
        Py_DECREF(qualified);
        return PyErr_NoMemory();
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        members[i].name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(names, i));
        members[i].type = Py_T_OBJECT_EX;
        members[i].offset = offsetof(RecordObject, fields) + i * sizeof(PyObject *);
        members[i].flags = Py_READONLY;
    }
    PyType_Slot slots[] = {
        {Py_tp_new, record_new},
        {Py_tp_dealloc, record_dealloc},
        {Py_tp_traverse, record_traverse},
        {Py_tp_clear, record_clear},
        {Py_tp_setattro, record_setattr},
        {Py_tp_members, members},
        {Py_tp_methods, record_methods},
        {Py_tp_repr, record_repr},
        {Py_tp_richcompare, record_richcompare},
        {Py_tp_hash, record_hash},
        {Py_nb_add, record_add},
        {Py_nb_subtract, record_sub},
        {Py_nb_multiply, record_mul},
        {Py_nb_true_divide, record_div},
        {Py_nb_negative, record_neg},
        {Py_nb_absolute, record_abs},
        {0, NULL}
    };
    PyType_Spec spec = {
        .name = PyUnicode_AsUTF8(qualified),
        .basicsize = (int)(offsetof(RecordObject, fields) + n * sizeof(PyObject *)),
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
        .slots = slots,
    };
    PyObject *nodes = plans_compile(plans);
    PyObject *schema = nodes == NULL ? NULL :
        PyTuple_Pack(6, names, plans, defaults, annotations,
                     eligible ? Py_True : Py_False, nodes);
    Py_XDECREF(nodes);
    PyObject *result = NULL;
    if (schema != NULL && spec.name != NULL) {
        result = PyType_FromModuleAndSpec(module, &spec, NULL);
    }
    PyMem_Free(members);
    Py_DECREF(qualified);
    if (result == NULL) {
        Py_XDECREF(schema);
        return NULL;
    }
    ((PyHeapTypeObject *)result)->_ht_grail_schema = schema;
    /* Build metadata before freezing/publication. */
    PyObject *qualname = PyObject_GetAttrString(source, "__qualname__");
    PyObject *doc = PyObject_GetAttrString(source, "__doc__");
    if (qualname == NULL || doc == NULL ||
        PyObject_SetAttrString(result, "__qualname__", qualname) < 0 ||
        PyObject_SetAttrString(result, "__doc__", doc) < 0 ||
        PyObject_SetAttrString(result, "__match_args__", names) < 0) {
        Py_XDECREF(qualname);
        Py_XDECREF(doc);
        Py_DECREF(result);
        return NULL;
    }
    Py_DECREF(qualname);
    Py_DECREF(doc);
    /* PYTHON_GRAIL_POSITIONAL_CONSTRUCTION=0: always stage arguments. */
    const char *positional = Py_GETENV("PYTHON_GRAIL_POSITIONAL_CONSTRUCTION");
    ((PyTypeObject *)result)->tp_vectorcall =
        positional != NULL && strcmp(positional, "0") == 0 ?
        record_vectorcall_general : record_vectorcall;
    if (PyType_Freeze((PyTypeObject *)result) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

PyObject *
_PyGrail_RecordInfo(PyObject *module, PyObject *type)
{
    if (require_record_type(type) < 0) {
        return NULL;
    }
    PyObject *schema = ((PyHeapTypeObject *)type)->_ht_grail_schema;
    PyObject *fields = PyTuple_GetSlice(schema, 0, 4);
    if (fields == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(2, fields, PyTuple_GET_ITEM(schema, 4));
    Py_DECREF(fields);
    return result;
}

PyObject *
_PyGrail_RecordValues(PyObject *module, PyObject *obj)
{
    if (require_record(obj) < 0) {
        return NULL;
    }
    RecordObject *self = (RecordObject *)obj;
    PyObject *result = PyDict_New();
    if (result == NULL) {
        return NULL;
    }
    PyObject *names = schema_part(Py_TYPE(obj), 0);
    for (Py_ssize_t i = 0; i < record_count(obj); i++) {
        if (PyDict_SetItem(result, PyTuple_GET_ITEM(names, i), self->fields[i]) < 0) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/* Non-raising form of record_deepcopy's shortcut, for copy.deepcopy.  The
 * argument is borrowed and the result immortal, so threads deep-copying one
 * shared record do not contend on its refcount via a bound __deepcopy__. */
PyObject *
_PyGrail_DeepcopyIsSelf(PyObject *module, PyObject *obj)
{
    return PyBool_FromLong(_PyGrail_IsRecordType((PyObject *)Py_TYPE(obj)) &&
                           schema_part(Py_TYPE(obj), 4) == Py_True &&
                           ((RecordObject *)obj)->conforming);
}

PyObject *
_PyGrail_IsConforming(PyObject *module, PyObject *obj)
{
    if (require_record(obj) < 0) {
        return NULL;
    }
    return PyBool_FromLong(schema_part(Py_TYPE(obj), 4) == Py_True &&
                           ((RecordObject *)obj)->conforming);
}
