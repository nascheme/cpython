#ifdef _Py_TIER2

/*
 * This file contains the support code for CPython's uops optimizer.
 * It also performs some simple optimizations.
 * It performs a traditional data-flow analysis[1] over the trace of uops.
 * Using the information gained, it chooses to emit, or skip certain instructions
 * if possible.
 *
 * [1] For information on data-flow analysis, please see
 * https://clang.llvm.org/docs/DataFlowAnalysisIntro.html
 *
 * */
#include "Python.h"
#include "opcode.h"
#include "pycore_dict.h"
#include "pycore_interp.h"
#include "pycore_opcode_metadata.h"
#include "pycore_opcode_utils.h"
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_*
#include "pycore_tstate.h"        // _PyThreadStateImpl
#include "pycore_uop_metadata.h"
#include "pycore_long.h"
#include "pycore_interpframe.h"  // _PyFrame_GetCode
#include "pycore_optimizer.h"
#include "pycore_object.h"
#include "pycore_function.h"
#include "pycore_grail.h"         // _PyGrail_IsPositionalRecordCall()
#include "pycore_uop_ids.h"
#include "pycore_range.h"
#include "pycore_unicodeobject.h"
#include "pycore_ceval.h"
#include "pycore_floatobject.h"
#include "pycore_setobject.h"
#include "pycore_typeobject.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef Py_DEBUG
    extern const char *_PyUOpName(int index);
    extern void _PyUOpPrint(const _PyUOpInstruction *uop);
    extern void _PyUOpSymPrint(JitOptRef ref);
    static const char *const DEBUG_ENV = "PYTHON_OPT_DEBUG";
    static inline int get_lltrace(void) {
        char *uop_debug = Py_GETENV(DEBUG_ENV);
        int lltrace = 0;
        if (uop_debug != NULL && *uop_debug >= '0') {
            lltrace = *uop_debug - '0';  // TODO: Parse an int and all that
        }
        return lltrace;
    }
    #define DPRINTF(level, ...) \
    if (get_lltrace() >= (level)) { printf(__VA_ARGS__); }

static void
dump_abstract_stack(_Py_UOpsAbstractFrame *frame, JitOptRef *stack_pointer)
{
    printf("    locals=[");
    for (int i = 0 ; i < frame->locals_len; i++) {
        if (i > 0) {
            printf(", ");
        }
        _PyUOpSymPrint(frame->locals[i]);
    }
    printf("]\n");
    if (stack_pointer < frame->stack) {
        printf("    stack=%d\n", (int)(stack_pointer - frame->stack));
    }
    else {
        printf("    stack=[");
        for (JitOptRef *ptr = frame->stack; ptr < stack_pointer; ptr++) {
            if (ptr != frame->stack) {
                printf(", ");
            }
            _PyUOpSymPrint(*ptr);
        }
        printf("]\n");
    }
    fflush(stdout);
}

static void
dump_uop(JitOptContext *ctx, const char *label, int index,
              const _PyUOpInstruction *instr, JitOptRef *stack_pointer)
{
    if (get_lltrace() >= 3) {
        printf("%4d %s: ", index, label);
        _PyUOpPrint(instr);
        printf("\n");
        if (get_lltrace() >= 5 && ctx->frame->code != ((PyCodeObject *)&_Py_InitCleanup) &&
            ctx->frame->code != ((PyCodeObject *)&_Py_BinaryOpCleanup)) {
            dump_abstract_stack(ctx->frame, stack_pointer);
        }
    }
}

static void
dump_uops(JitOptContext *ctx, const char *label,
          _PyUOpInstruction *start, JitOptRef *stack_pointer)
{
    int current_len = uop_buffer_length(&ctx->out_buffer);
    int added_count = (int)(ctx->out_buffer.next - start);
    for (int j = 0; j < added_count; j++) {
        dump_uop(ctx, label, current_len - added_count + j, &start[j], stack_pointer);
    }
}

#define DUMP_UOP dump_uop
#define DUMP_UOPS dump_uops

#else
    #define DPRINTF(level, ...)
    #define DUMP_UOP(ctx, label, index, instr, stack_pointer)
    #define DUMP_UOPS(ctx, label, start, stack_pointer)
#endif

static int
get_mutations(PyObject* dict) {
    assert(PyDict_CheckExact(dict));
    PyDictObject *d = (PyDictObject *)dict;
    uint64_t tag = FT_ATOMIC_LOAD_UINT64_RELAXED(d->_ma_watcher_tag);
    return (tag >> DICT_MAX_WATCHERS) & ((1 << DICT_WATCHED_MUTATION_BITS) - 1);
}

static void
increment_mutations(PyObject* dict) {
    assert(PyDict_CheckExact(dict));
    PyDictObject *d = (PyDictObject *)dict;
    FT_ATOMIC_ADD_UINT64(d->_ma_watcher_tag, 1ULL << DICT_MAX_WATCHERS);
}

/* The first two dict watcher IDs are reserved for CPython,
 * so we don't need to check that they haven't been used */
#define BUILTINS_WATCHER_ID 0
#define GLOBALS_WATCHER_ID  1
#define TYPE_WATCHER_ID  0

static int
globals_watcher_callback(PyDict_WatchEvent event, PyObject* dict,
                         PyObject* key, PyObject* new_value)
{
    RARE_EVENT_STAT_INC(watched_globals_modification);
    assert(get_mutations(dict) < _Py_MAX_ALLOWED_GLOBALS_MODIFICATIONS);
    _Py_Executors_InvalidateDependency(_PyInterpreterState_GET(), dict, 1);
    increment_mutations(dict);
    PyDict_Unwatch(GLOBALS_WATCHER_ID, dict);
    return 0;
}

static int
type_watcher_callback(PyTypeObject* type)
{
    _Py_Executors_InvalidateDependency(_PyInterpreterState_GET(), type, 1);
    PyType_Unwatch(TYPE_WATCHER_ID, (PyObject *)type);
    return 0;
}

static int
_setup_optimizer_watchers(void *Py_UNUSED(arg))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    FT_ATOMIC_STORE_PTR_RELEASE(
        interp->dict_state.watchers[GLOBALS_WATCHER_ID],
        globals_watcher_callback);
    interp->type_watchers[TYPE_WATCHER_ID] = type_watcher_callback;
    return 0;
}

static void
watch_type(PyTypeObject *type, _PyBloomFilter *filter)
{
    if (_Py_IsImmortal(type) && (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE)) {
        return;
    }
    PyType_Watch(TYPE_WATCHER_ID, (PyObject *)type);
    _Py_BloomFilter_Add(filter, type);
}

static PyObject *
convert_global_to_const(_PyUOpInstruction *inst, PyObject *obj)
{
    assert(inst->opcode == _LOAD_GLOBAL_MODULE || inst->opcode == _LOAD_GLOBAL_BUILTINS || inst->opcode == _LOAD_ATTR_MODULE);
    assert(PyDict_CheckExact(obj));
    PyDictObject *dict = (PyDictObject *)obj;
    assert(dict->ma_keys->dk_kind == DICT_KEYS_UNICODE);
    PyDictUnicodeEntry *entries = DK_UNICODE_ENTRIES(dict->ma_keys);
    int64_t index = inst->operand1;
    assert(index <= UINT16_MAX);
    if ((int)index >= dict->ma_keys->dk_nentries) {
        return NULL;
    }
    PyDictKeysObject *keys = dict->ma_keys;
    if (keys->dk_version != inst->operand0) {
        return NULL;
    }
    PyObject *res = entries[index].me_value;
    if (res == NULL) {
        return NULL;
    }
    if (_Py_IsImmortal(res)) {
        inst->opcode = _LOAD_CONST_INLINE_BORROW;
    } else {
        inst->opcode = _LOAD_CONST_INLINE;
    }
    inst->operand0 = (uint64_t)res;
    return res;
}

static bool
incorrect_keys(PyObject *obj, uint32_t version)
{
    if (!PyDict_CheckExact(obj)) {
        return true;
    }
    PyDictObject *dict = (PyDictObject *)obj;
    return dict->ma_keys->dk_version != version;
}


#define STACK_LEVEL()     ((int)(stack_pointer - ctx->frame->stack))
#define STACK_SIZE()      ((int)(ctx->frame->stack_len))

static inline int
is_terminator_uop(const _PyUOpInstruction *uop)
{
    int opcode = uop->opcode;
    return (
        opcode == _EXIT_TRACE ||
        opcode == _JUMP_TO_TOP ||
        opcode == _DYNAMIC_EXIT ||
        opcode == _DEOPT
    );
}

#define CURRENT_FRAME_IS_SHIM() \
    (ctx->frame->code == ((PyCodeObject *)&_Py_InitCleanup) || \
     ctx->frame->code == ((PyCodeObject *)&_Py_BinaryOpCleanup))

#define GETLOCAL(idx)          ((ctx->frame->locals[idx]))

#define REPLACE_OP(INST, OP, ARG, OPERAND)    \
    (INST)->opcode = OP;            \
    (INST)->oparg = ARG;            \
    (INST)->operand0 = OPERAND;

#define ADD_OP(OP, ARG, OPERAND) add_op(ctx, this_instr, (OP), (ARG), (OPERAND))

static inline void
add_op(JitOptContext *ctx, _PyUOpInstruction *this_instr,
       uint16_t opcode, uint16_t oparg, uintptr_t operand0)
{
    _PyUOpInstruction *out = ctx->out_buffer.next;
    assert(out < ctx->out_buffer.end);
    out->opcode = (opcode);
    out->format = this_instr->format;
    out->oparg = (oparg);
    out->target = this_instr->target;
    out->operand0 = (operand0);
    out->operand1 = this_instr->operand1;
#ifdef Py_STATS
    out->fitness = this_instr->fitness;
#endif
    ctx->out_buffer.next++;
}

/* Shortened forms for convenience, used in optimizer_bytecodes.c */
#define sym_is_not_null _Py_uop_sym_is_not_null
#define sym_is_const _Py_uop_sym_is_const
#define sym_is_safe_const _Py_uop_sym_is_safe_const
#define sym_is_not_container _Py_uop_sym_is_not_container
#define sym_get_const _Py_uop_sym_get_const
#define sym_new_const_steal _Py_uop_sym_new_const_steal
#define sym_get_const_as_stackref _Py_uop_sym_get_const_as_stackref
#define sym_new_unknown _Py_uop_sym_new_unknown
#define sym_new_not_null _Py_uop_sym_new_not_null
#define sym_new_type _Py_uop_sym_new_type
#define sym_is_null _Py_uop_sym_is_null
#define sym_new_const _Py_uop_sym_new_const
#define sym_new_null _Py_uop_sym_new_null
#define sym_has_type _Py_uop_sym_has_type
#define sym_get_type _Py_uop_sym_get_type
#define sym_get_probable_type _Py_uop_sym_get_probable_type
#define sym_matches_type _Py_uop_sym_matches_type
#define sym_matches_type_version _Py_uop_sym_matches_type_version
#define sym_get_type_version _Py_uop_sym_get_type_version
#define sym_set_null(SYM) _Py_uop_sym_set_null(ctx, SYM)
#define sym_set_non_null(SYM) _Py_uop_sym_set_non_null(ctx, SYM)
#define sym_set_type(SYM, TYPE) _Py_uop_sym_set_type(ctx, SYM, TYPE)
#define sym_set_type_version(SYM, VERSION) _Py_uop_sym_set_type_version(ctx, SYM, VERSION)
#define sym_set_const(SYM, CNST) _Py_uop_sym_set_const(ctx, SYM, CNST)
#define sym_set_compact_int(SYM) _Py_uop_sym_set_compact_int(ctx, SYM)
#define sym_is_bottom _Py_uop_sym_is_bottom
#define sym_truthiness _Py_uop_sym_truthiness
#define frame_new _Py_uop_frame_new
#define frame_new_from_symbol _Py_uop_frame_new_from_symbol
#define frame_pop _Py_uop_frame_pop
#define sym_new_tuple _Py_uop_sym_new_tuple
#define sym_tuple_getitem _Py_uop_sym_tuple_getitem
#define sym_tuple_length _Py_uop_sym_tuple_length
#define sym_is_immortal _Py_uop_symbol_is_immortal
#define sym_is_compact_int _Py_uop_sym_is_compact_int
#define sym_new_compact_int _Py_uop_sym_new_compact_int
#define sym_new_truthiness _Py_uop_sym_new_truthiness
#define sym_new_predicate _Py_uop_sym_new_predicate
#define sym_apply_predicate_narrowing _Py_uop_sym_apply_predicate_narrowing
#define sym_set_recorded_type(SYM, TYPE) _Py_uop_sym_set_recorded_type(ctx, SYM, TYPE)
#define sym_set_recorded_value(SYM, VAL) _Py_uop_sym_set_recorded_value(ctx, SYM, VAL)
#define sym_set_recorded_gen_func(SYM, VAL) _Py_uop_sym_set_recorded_gen_func(ctx, SYM, VAL)
#define sym_get_probable_func_code _Py_uop_sym_get_probable_func_code
#define sym_get_probable_value _Py_uop_sym_get_probable_value
#define sym_set_stack_depth(DEPTH, SP) _Py_uop_sym_set_stack_depth(ctx, DEPTH, SP)

/* Comparison oparg masks */
#define COMPARE_LT_MASK 2
#define COMPARE_GT_MASK 4
#define COMPARE_EQ_MASK 8

#define JUMP_TO_LABEL(label) goto label;

static int
check_stack_bounds(JitOptContext *ctx, JitOptRef *stack_pointer, int offset, int opcode)
{
    int stack_level = (int)(stack_pointer + (offset) - ctx->frame->stack);
    int should_check = !CURRENT_FRAME_IS_SHIM() ||
        (opcode == _RETURN_VALUE) ||
        (opcode == _RETURN_GENERATOR) ||
        (opcode == _YIELD_VALUE);
    if (should_check && (stack_level < 0 || stack_level > STACK_SIZE() + MAX_CACHED_REGISTER)) {
        ctx->contradiction = true;
        ctx->done = true;
        return 1;
    }
    return 0;
}

#define CHECK_STACK_BOUNDS(offset) \
    if (check_stack_bounds(ctx, stack_pointer, offset, opcode)) { \
        break; \
    } \

static int
optimize_to_bool(
    _PyUOpInstruction *this_instr,
    JitOptContext *ctx,
    JitOptRef value,
    JitOptRef *result_ptr,
    uint16_t prefix, uint16_t suffix)
{
    if (sym_matches_type(value, &PyBool_Type)) {
        ADD_OP(_NOP, 0, 0);
        *result_ptr = value;
        return 1;
    }
    int truthiness = sym_truthiness(ctx, value);
    if (truthiness >= 0) {
        PyObject *load = truthiness ? Py_True : Py_False;
        if (prefix != _NOP) {
            ADD_OP(prefix, 0, 0);
        }
        ADD_OP(_LOAD_CONST_INLINE_BORROW, 0, (uintptr_t)load);
        if (suffix != _NOP) {
            ADD_OP(suffix, 2, 0);
        }
        *result_ptr = sym_new_const(ctx, load);
        return 1;
    }
    return 0;
}

/* For COMPARE_OP: true if both operands are records of one type, known or
 * as recorded while tracing (then guarded here). */
static bool
grail_same_record_operands(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                           JitOptRef left, JitOptRef right)
{
    PyTypeObject *lt = sym_has_type(left) ? sym_get_type(left)
                                          : sym_get_probable_type(left);
    PyTypeObject *rt = sym_has_type(right) ? sym_get_type(right)
                                           : sym_get_probable_type(right);
    if (lt == NULL || lt != rt || !_PyGrail_IsRecordTypeFast(lt)) {
        return false;
    }
    if ((!sym_has_type(left) || !sym_has_type(right)) &&
        lt->tp_version_tag == 0 && !PyUnstable_Type_AssignVersionTag(lt)) {
        return false;
    }
    _PyBloomFilter *dependencies = ctx->dependencies;
    if (!sym_has_type(left)) {
        ADD_OP(_GUARD_NOS_OBJ_TYPE_VERSION, 0, lt->tp_version_tag);
        sym_set_type(left, lt);
        watch_type(lt, dependencies);
    }
    if (!sym_has_type(right)) {
        ADD_OP(_GUARD_TYPE_VERSION, 0, lt->tp_version_tag);
        sym_set_type(right, lt);
        sym_set_type_version(right, lt->tp_version_tag);
        watch_type(lt, dependencies);
    }
    return true;
}

/* For COMPARE_OP: true if one operand is an exact float and the other a
 * compact int, known or as recorded while tracing (then guarded here).
 * Mixed float/int comparisons do not specialize in tier 1. */
static bool
float_int_operands(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                   JitOptRef left, JitOptRef right)
{
    PyTypeObject *lt = sym_has_type(left) ? sym_get_type(left)
                                          : sym_get_probable_type(left);
    PyTypeObject *rt = sym_has_type(right) ? sym_get_type(right)
                                           : sym_get_probable_type(right);
    bool float_left = lt == &PyFloat_Type && rt == &PyLong_Type;
    if (!float_left && !(lt == &PyLong_Type && rt == &PyFloat_Type)) {
        return false;
    }
    if (float_left) {
        if (!sym_has_type(left)) {
            ADD_OP(_GUARD_NOS_FLOAT, 0, 0);
            sym_set_type(left, &PyFloat_Type);
        }
        if (!sym_is_compact_int(right)) {
            ADD_OP(_GUARD_TOS_INT, 0, 0);
            sym_set_compact_int(right);
        }
    }
    else {
        if (!sym_is_compact_int(left)) {
            ADD_OP(_GUARD_NOS_INT, 0, 0);
            sym_set_compact_int(left);
        }
        if (!sym_has_type(right)) {
            ADD_OP(_GUARD_TOS_FLOAT, 0, 0);
            sym_set_type(right, &PyFloat_Type);
        }
    }
    return true;
}

static void
optimize_dict_known_hash(
    JitOptContext *ctx, _PyBloomFilter *dependencies, _PyUOpInstruction *this_instr,
    PyObject *sub, uint16_t opcode)
{
    if (PyUnicode_CheckExact(sub) || PyLong_CheckExact(sub) || PyBytes_CheckExact(sub)
            || PyFloat_CheckExact(sub) || PyComplex_CheckExact(sub)) {
        // PyObject_Hash can't fail on these types
        ADD_OP(opcode, 0, PyObject_Hash(sub));
    }
    else if (PyTuple_CheckExact(sub)) {
        // only use known hash variant when hash of tuple is already computed
        // since computing it can call arbitrary code
        Py_hash_t hash = ((PyTupleObject *)sub)->ob_hash;
        if (hash != -1) {
            ADD_OP(opcode, 0, hash);
        }
    }
    else if (Py_TYPE(sub)->tp_hash == PyBaseObject_Type.tp_hash) {
        // for user-defined objects which don't override tp_hash
        Py_hash_t hash = PyObject_Hash(sub);
        ADD_OP(opcode, 0, hash);
        watch_type(Py_TYPE(sub), dependencies);
    }
}

static void
eliminate_pop_guard(_PyUOpInstruction *this_instr, JitOptContext *ctx, bool exit)
{
    ADD_OP(_POP_TOP, 0, 0);
    if (exit) {
        REPLACE_OP((this_instr+1), _EXIT_TRACE, 0, 0);
        this_instr[1].target = this_instr->target;
    }
}

static JitOptRef
lookup_attr(JitOptContext *ctx, _PyBloomFilter *dependencies, _PyUOpInstruction *this_instr,
            PyTypeObject *type, PyObject *name,
            uint16_t prefix, uint16_t suffix)
{
    // The cached value may be dead, so we need to do the lookup again... :(
    if (type && PyType_Check(type)) {
        PyObject *lookup = _PyType_Lookup(type, name);
        if (lookup) {
            bool immortal = _Py_IsImmortal(lookup) || (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE);
            if (prefix != _NOP) {
                ADD_OP(prefix, 0, 0);
            }
            ADD_OP(immortal ? _LOAD_CONST_INLINE_BORROW : _LOAD_CONST_INLINE,
                   0, (uintptr_t)lookup);
            if (suffix != _NOP) {
                ADD_OP(suffix, 2, 0);
            }
            if ((type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) == 0) {
                watch_type(type, dependencies);
            }
            return sym_new_const(ctx, lookup);
        }
    }
    return sym_new_not_null(ctx);
}

/* Grail value conformance in a trace.
 *
 * Records and tuples are immutable and plans are frozen, so two kinds of
 * facts hold for the rest of a trace once established:
 *  - grail_conforming: records whose conformance bit is known to be set
 *    (they passed a typed field load, or were built from proven values);
 *  - grail_plans: values known to conform to a plan node, such as a field of
 *    a conforming record, an item or slice of a conforming tuple, or a
 *    concatenation of those.
 * grail_conforms() combines them with known types, constants and symbolic
 * tuples. A _CALL_RECORD then checks only the arguments it cannot prove
 * (_CALL_RECORD_CHECKED), and a record built from proven arguments is known
 * to conform. PYTHON_GRAIL_JIT_KNOWN_CONFORMING=0 disables the plan facts and
 * _CALL_RECORD_CHECKED (fresh process per setting). */

#define GRAIL_PLAN_DEPTH 8

typedef _PyGrailPlanNode GrailPlan;

static int
grail_plan_facts(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_KNOWN_CONFORMING");
    return value == NULL || strcmp(value, "0") != 0;
}

static bool
grail_in_conforming_set(JitOptContext *ctx, JitOptSymbol *sym)
{
    for (int i = 0; i < ctx->grail_conforming_count; i++) {
        if (ctx->grail_conforming[i] == sym) {
            return true;
        }
    }
    return false;
}

/* True if plan alternative a cannot describe a value of exact type type. */
static bool
grail_plan_excludes(const GrailPlan *a, PyTypeObject *type)
{
    switch (a->kind) {
    case _PyGrail_PLAN_EXACT:
    case _PyGrail_PLAN_RECORD:
        return a->type != type;
    case _PyGrail_PLAN_TUPLE:
    case _PyGrail_PLAN_VARTUPLE:
        return type != &PyTuple_Type;
    default:
        return false;
    }
}

/* True if every value conforming to plan h of hn (and of exact type type,
 * when not NULL) conforms to plan w of wn. Structural: the two plans may
 * belong to different record types. */
static bool
grail_plan_implies(const GrailPlan *hn, const GrailPlan *h, PyTypeObject *type,
                   const GrailPlan *wn, const GrailPlan *w, int depth)
{
    if (w->kind == _PyGrail_PLAN_ANY) {
        return true;
    }
    if (depth > GRAIL_PLAN_DEPTH) {
        return false;
    }
    if (h->kind == _PyGrail_PLAN_UNION) {
        bool any = false;
        for (uint32_t k = 0; k < h->n; k++) {
            const GrailPlan *a = &hn[h->first + k];
            if (type != NULL && grail_plan_excludes(a, type)) {
                continue;
            }
            if (!grail_plan_implies(hn, a, type, wn, w, depth + 1)) {
                return false;
            }
            any = true;
        }
        return any;
    }
    switch (w->kind) {
    case _PyGrail_PLAN_EXACT:
    case _PyGrail_PLAN_RECORD:
        return h->kind == w->kind && h->type == w->type;
    case _PyGrail_PLAN_UNION:
        for (uint32_t k = 0; k < w->n; k++) {
            if (grail_plan_implies(hn, h, type, wn, &wn[w->first + k], depth + 1)) {
                return true;
            }
        }
        return false;
    case _PyGrail_PLAN_VARTUPLE: {
        const GrailPlan *wc = &wn[w->first];
        if (h->kind == _PyGrail_PLAN_VARTUPLE) {
            return grail_plan_implies(hn, &hn[h->first], NULL, wn, wc, depth + 1);
        }
        if (h->kind != _PyGrail_PLAN_TUPLE) {
            return false;
        }
        for (uint32_t k = 0; k < h->n; k++) {
            if (!grail_plan_implies(hn, &hn[h->first + k], NULL, wn, wc, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case _PyGrail_PLAN_TUPLE:
        if (h->kind != _PyGrail_PLAN_TUPLE || h->n != w->n) {
            return false;
        }
        for (uint32_t k = 0; k < h->n; k++) {
            if (!grail_plan_implies(hn, &hn[h->first + k], NULL,
                                    wn, &wn[w->first + k], depth + 1)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

/* True if a plan fact about ref implies plan w of wn. */
static bool
grail_fact_implies(JitOptContext *ctx, JitOptRef ref, const GrailPlan *wn,
                   const GrailPlan *w)
{
    JitOptSymbol *sym = PyJitRef_Unwrap(ref);
    PyTypeObject *type = NULL;
    for (int i = 0; i < ctx->grail_plan_count; i++) {
        if (ctx->grail_plans[i].value != sym) {
            continue;
        }
        if (type == NULL) {
            type = sym_get_type(ref);
        }
        if (grail_plan_implies(ctx->grail_plans[i].nodes, ctx->grail_plans[i].node,
                               type, wn, w, 0)) {
            return true;
        }
    }
    return false;
}

/* Grail records known to conform, so that typed field loads from them need
 * no exit. A conforming record stays conforming (its fields are immutable
 * values), so this holds for the rest of the trace. */
static bool
grail_known_conforming(JitOptContext *ctx, JitOptRef ref)
{
    if (grail_in_conforming_set(ctx, PyJitRef_Unwrap(ref))) {
        return true;
    }
    PyTypeObject *type = sym_get_type(ref);
    if (type == NULL || ctx->grail_plan_count == 0) {
        return false;
    }
    GrailPlan record = {.type = type, .kind = _PyGrail_PLAN_RECORD};
    return grail_fact_implies(ctx, ref, &record, &record);
}

static void
grail_set_conforming(JitOptContext *ctx, JitOptRef ref)
{
    if (!grail_in_conforming_set(ctx, PyJitRef_Unwrap(ref)) &&
        ctx->grail_conforming_count < GRAIL_MAX_CONFORMING) {
        ctx->grail_conforming[ctx->grail_conforming_count++] = PyJitRef_Unwrap(ref);
    }
}

/* Record that ref conforms to plan node p of nodes. A leaf plan also gives
 * the value's exact type. */
static void
grail_add_plan_fact(JitOptContext *ctx, JitOptRef ref, const GrailPlan *nodes,
                    const GrailPlan *p)
{
    if (p->kind == _PyGrail_PLAN_ANY || !grail_plan_facts()) {
        return;
    }
    if (p->kind == _PyGrail_PLAN_EXACT || p->kind == _PyGrail_PLAN_RECORD) {
        PyTypeObject *type = sym_get_type(ref);
        if (type != NULL && type != p->type) {
            return;  // unreachable code: keep what is known
        }
        if (type == NULL) {
            sym_set_type(ref, p->type);
        }
        if (p->kind == _PyGrail_PLAN_RECORD) {
            grail_set_conforming(ctx, ref);
        }
        return;
    }
    JitOptSymbol *sym = PyJitRef_Unwrap(ref);
    for (int i = 0; i < ctx->grail_plan_count; i++) {
        if (ctx->grail_plans[i].value == sym && ctx->grail_plans[i].node == p) {
            return;
        }
    }
    if (ctx->grail_plan_count < GRAIL_MAX_PLAN_FACTS) {
        int n = ctx->grail_plan_count++;
        ctx->grail_plans[n].value = sym;
        ctx->grail_plans[n].nodes = nodes;
        ctx->grail_plans[n].node = p;
    }
}

/* True if the value of ref is known to conform to plan w of wn. */
static bool
grail_conforms(JitOptContext *ctx, JitOptRef ref, const GrailPlan *wn,
               const GrailPlan *w, int depth)
{
    if (w->kind == _PyGrail_PLAN_ANY) {
        return true;
    }
    if (depth > GRAIL_PLAN_DEPTH) {
        return false;
    }
    if (sym_is_const(ctx, ref)) {
        PyObject *value = sym_get_const(ctx, ref);
        if (value != NULL) {
            int res = _PyGrail_PlanConforms(wn, w, value);
            if (res < 0) {
                PyErr_Clear();
            }
            return res == 1;
        }
    }
    if (grail_fact_implies(ctx, ref, wn, w)) {
        return true;
    }
    PyTypeObject *type = sym_get_type(ref);
    switch (w->kind) {
    case _PyGrail_PLAN_EXACT:
        return type == w->type;
    case _PyGrail_PLAN_RECORD:
        return type == w->type && grail_in_conforming_set(ctx, PyJitRef_Unwrap(ref));
    case _PyGrail_PLAN_UNION:
        for (uint32_t k = 0; k < w->n; k++) {
            if (grail_conforms(ctx, ref, wn, &wn[w->first + k], depth + 1)) {
                return true;
            }
        }
        return false;
    case _PyGrail_PLAN_TUPLE:
    case _PyGrail_PLAN_VARTUPLE: {
        Py_ssize_t length = sym_tuple_length(ref);
        if (length < 0 ||
            (w->kind == _PyGrail_PLAN_TUPLE && length != (Py_ssize_t)w->n)) {
            return false;
        }
        for (Py_ssize_t k = 0; k < length; k++) {
            const GrailPlan *c = &wn[w->first +
                                     (w->kind == _PyGrail_PLAN_TUPLE ? k : 0)];
            if (!grail_conforms(ctx, sym_tuple_getitem(ctx, ref, (int)k), wn, c,
                                depth + 1)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

/* Facts for item (index, or -1 if not constant) of a tuple, or for a slice
 * of it (slice true), from the tuple's plan facts. */
static void
grail_tuple_part_facts(JitOptContext *ctx, JitOptRef tuple, JitOptRef res,
                       long index, bool slice)
{
    JitOptSymbol *sym = PyJitRef_Unwrap(tuple);
    int n = ctx->grail_plan_count;
    for (int i = 0; i < n; i++) {
        if (ctx->grail_plans[i].value != sym) {
            continue;
        }
        const GrailPlan *nodes = ctx->grail_plans[i].nodes;
        const GrailPlan *p = ctx->grail_plans[i].node;
        if (p->kind == _PyGrail_PLAN_VARTUPLE) {
            grail_add_plan_fact(ctx, res, nodes, slice ? p : &nodes[p->first]);
        }
        else if (p->kind == _PyGrail_PLAN_TUPLE && !slice &&
                 index >= 0 && index < (long)p->n) {
            grail_add_plan_fact(ctx, res, nodes, &nodes[p->first + index]);
        }
    }
}

/* The concatenation of a tuple and a part known to conform to the same
 * variable-tuple plan conforms to it too. */
static void
grail_tuple_concat_facts(JitOptContext *ctx, JitOptRef tuple, JitOptRef part,
                         JitOptRef res)
{
    JitOptSymbol *sym = PyJitRef_Unwrap(tuple);
    int n = ctx->grail_plan_count;
    for (int i = 0; i < n; i++) {
        if (ctx->grail_plans[i].value != sym) {
            continue;
        }
        const GrailPlan *nodes = ctx->grail_plans[i].nodes;
        const GrailPlan *p = ctx->grail_plans[i].node;
        if (p->kind == _PyGrail_PLAN_VARTUPLE &&
            grail_conforms(ctx, part, nodes, p, 0)) {
            grail_add_plan_fact(ctx, res, nodes, p);
        }
    }
}

/* A field load from a record known to conform: the value conforms to the
 * field's plan. */
static void
grail_field_facts(JitOptContext *ctx, JitOptRef owner, Py_ssize_t offset,
                  JitOptRef attr)
{
    PyTypeObject *type = sym_get_type(owner);
    const GrailPlan *nodes = type != NULL ? _PyGrail_PlanNodes(type) : NULL;
    if (nodes == NULL || !grail_known_conforming(ctx, owner)) {
        return;
    }
    Py_ssize_t i = (offset - (Py_ssize_t)offsetof(RecordObject, fields)) /
                   (Py_ssize_t)sizeof(PyObject *);
    Py_ssize_t n = (type->tp_basicsize - (Py_ssize_t)offsetof(RecordObject, fields)) /
                   (Py_ssize_t)sizeof(PyObject *);
    if (i >= 0 && i < n) {
        grail_add_plan_fact(ctx, attr, nodes, &nodes[i]);
    }
}

/* _CALL_RECORD of a record type: check only the arguments not known to
 * conform. */
static void
grail_record_call(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                  PyTypeObject *type, JitOptRef *args, int nargs, JitOptRef res)
{
    const GrailPlan *nodes = _PyGrail_PlanNodes(type);
    if (nodes == NULL || nargs > 64 || !grail_plan_facts()) {
        return;
    }
    uint64_t check = 0;
    for (int i = 0; i < nargs; i++) {
        if (!grail_conforms(ctx, args[i], nodes, &nodes[i], 0)) {
            check |= (uint64_t)1 << i;
        }
    }
    uint64_t all = nargs == 64 ? UINT64_MAX : ((uint64_t)1 << nargs) - 1;
    if (check != all) {
        REPLACE_OP(this_instr, _CALL_RECORD_CHECKED, nargs, check);
    }
    if (check == 0) {
        grail_set_conforming(ctx, res);
    }
}

/* The result of a typed load (field_type is from the field's plan): owner
 * conforms once the load is reached. A record-typed field of a conforming
 * record conforms too. */
static JitOptRef
grail_typed_field(JitOptContext *ctx, JitOptRef owner, PyTypeObject *field_type,
                  _PyBloomFilter *dependencies)
{
    grail_set_conforming(ctx, owner);
    JitOptRef attr = sym_new_type(ctx, field_type);
    if (_PyGrail_IsRecordTypeFast(field_type)) {
        grail_set_conforming(ctx, attr);
        if (field_type->tp_version_tag != 0) {
            sym_set_type_version(attr, field_type->tp_version_tag);
            watch_type(field_type, dependencies);
        }
    }
    return attr;
}

/* Borrowed Grail record field loads.
 *
 * A record's fields are never replaced, so a field lives as long as its
 * record. A field load can then push a borrowed reference, and the pop that
 * consumes it becomes _POP_TOP_NOP, if a local of the current frame holds
 * the record from the load until the pop. These are the lifetime rules of
 * LOAD_FAST_BORROW (see optimize_load_fast() in flowgraph.c): the borrowed
 * value is consumed before its supporting local is killed, and is not
 * stored as a local.
 *
 * The load is emitted in its owning form and remembered. The decision is
 * made at the pop: if nothing between the load and the pop could kill the
 * local, copy the value, or change frames, the load is rewritten to its
 * borrowed form. Code between the two must also contain no bytecode jump,
 * so that a side exit in between continues in tier 1 on the same
 * straight-line bytecode, which pops the value too.
 *
 * PYTHON_GRAIL_JIT_BORROW_FIELDS=0 disables it (fresh process per setting). */
static int
grail_borrow_fields(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_BORROW_FIELDS");
    return value == NULL || strcmp(value, "0") != 0;
}

/* Remember the record field load just processed (not yet copied to the
 * output buffer) if a local of the current frame holds its owner. */
static void
grail_borrow_candidate(JitOptContext *ctx, JitOptRef attr, JitOptRef owner,
                       uint16_t borrow_op)
{
    if (ctx->grail_borrow_count >= GRAIL_MAX_BORROWS || !grail_borrow_fields()) {
        return;
    }
    JitOptSymbol *owner_sym = PyJitRef_Unwrap(owner);
    _Py_UOpsAbstractFrame *frame = ctx->frame;
    for (int i = 0; i < frame->locals_len; i++) {
        if (PyJitRef_Unwrap(frame->locals[i]) == owner_sym) {
            int n = ctx->grail_borrow_count++;
            ctx->grail_borrows[n].field = PyJitRef_Unwrap(attr);
            ctx->grail_borrows[n].owner = owner_sym;
            ctx->grail_borrows[n].local = i;
            ctx->grail_borrows[n].load =
                (int)(ctx->out_buffer.next - ctx->out_buffer.start);
            ctx->grail_borrows[n].borrow_op = borrow_op;
            return;
        }
    }
}

/* True if popping value can be a no-op because it is a remembered field
 * load, which is then rewritten to its borrowed form. */
static bool
grail_borrow_pop(JitOptContext *ctx, JitOptRef value)
{
    if (ctx->grail_borrow_count == 0 || PyJitRef_IsBorrowed(value)) {
        return false;
    }
    JitOptSymbol *sym = PyJitRef_Unwrap(value);
    for (int n = 0; n < ctx->grail_borrow_count; n++) {
        if (ctx->grail_borrows[n].field != sym) {
            continue;
        }
        bool ok = PyJitRef_Unwrap(ctx->frame->locals[ctx->grail_borrows[n].local]) ==
                  ctx->grail_borrows[n].owner;
        if (ok) {
            _PyUOpInstruction *load = ctx->out_buffer.start + ctx->grail_borrows[n].load;
            assert(load->opcode == _LOAD_ATTR_RECORD ||
                   load->opcode == _LOAD_ATTR_RECORD_TYPED);
            load->opcode = ctx->grail_borrows[n].borrow_op;
        }
        ctx->grail_borrows[n] = ctx->grail_borrows[--ctx->grail_borrow_count];
        return ok;
    }
    return false;
}

/* Forget all remembered loads if this uop could kill a local, copy a stack
 * value or come from a bytecode jump, or if it changed frames. */
static void
grail_borrow_barrier(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                     _Py_UOpsAbstractFrame *frame_before)
{
    int opcode = this_instr->opcode;
    bool barrier = ctx->frame != frame_before;
    switch (opcode) {
        case _SWAP_FAST: case _SWAP_FAST_0: case _SWAP_FAST_1:
        case _SWAP_FAST_2: case _SWAP_FAST_3: case _SWAP_FAST_4:
        case _SWAP_FAST_5: case _SWAP_FAST_6: case _SWAP_FAST_7:
        case _DELETE_FAST:
        case _LOAD_FAST_AND_CLEAR:
        case _MAKE_CELL:
        case _COPY: case _COPY_1: case _COPY_2: case _COPY_3:
            barrier = true;
            break;
        default:
            if (is_terminator_uop(this_instr)) {
                barrier = true;
            }
            break;
    }
    if (!barrier && !CURRENT_FRAME_IS_SHIM()) {
        PyCodeObject *code = ctx->frame->code;
        if (this_instr->format == UOP_FORMAT_TARGET &&
            this_instr->target < (uint32_t)Py_SIZE(code)) {
            int op = _PyOpcode_Deopt[_PyCode_CODE(code)[this_instr->target].op.code];
            barrier = OPCODE_HAS_JUMP(op);
        }
        else {
            barrier = true;
        }
    }
    if (barrier) {
        ctx->grail_borrow_count = 0;
    }
}

static void
optimize_pop_top(JitOptContext *ctx, _PyUOpInstruction *this_instr, JitOptRef value)
{
    PyTypeObject *typ = sym_get_type(value);
    if (grail_borrow_pop(ctx, value)) {
        ADD_OP(_POP_TOP_NOP, 0, 0);
    }
    else if (PyJitRef_IsBorrowed(value) ||
        sym_is_immortal(PyJitRef_Unwrap(value)) ||
        sym_is_null(value)) {
        ADD_OP(_POP_TOP_NOP, 0, 0);
    }
    else if (typ == &PyLong_Type) {
        ADD_OP(_POP_TOP_INT, 0, 0);
    }
    else if (typ == &PyFloat_Type) {
        ADD_OP(_POP_TOP_FLOAT, 0, 0);
    }
    else if (typ == &PyUnicode_Type) {
        ADD_OP(_POP_TOP_UNICODE, 0, 0);
    }
    else {
        ADD_OP(_POP_TOP, 0, 0);
    }
}

/* Look up name via super (normal case from supercheck where
   su_obj_type = Py_TYPE(obj)). */
static JitOptRef
lookup_super_attr(JitOptContext *ctx, _PyBloomFilter *dependencies,
                  _PyUOpInstruction *this_instr,
                  PyTypeObject *su_type, PyTypeObject *obj_type,
                  PyObject *name,
                  uint16_t immortal, uint16_t mortal, uint16_t suffix)
{
    if (su_type == NULL || obj_type == NULL) {
        return sym_new_not_null(ctx);
    }
    /* Normal case: obj_type must be a subtype of su_type */
    if (!PyType_IsSubtype(obj_type, su_type)) {
        return sym_new_not_null(ctx);
    }
    PyObject *lookup = _PySuper_LookupDescr(su_type, obj_type, name);
    if (lookup == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        return sym_new_not_null(ctx);
    }
    if ((Py_TYPE(lookup)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR) == 0) {
        Py_DECREF(lookup);
        return sym_new_not_null(ctx);
    }
    int opcode = mortal;
    if (_Py_IsImmortal(lookup) || (obj_type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE)) {
        opcode = immortal;
    }
    ADD_OP(_SWAP, 3, 0);
    ADD_OP(_POP_TOP, 0, 0);
    ADD_OP(_POP_TOP, 0, 0);
    ADD_OP(opcode, 0, (uintptr_t)lookup);
    if (suffix != _NOP) {
        ADD_OP(suffix, 2, 0);
    }
    // if obj_type is immutable, then all its superclasses are immutable
    if ((obj_type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) == 0) {
        watch_type(su_type, dependencies);
        watch_type(obj_type, dependencies);
    }
    return sym_new_const_steal(ctx, lookup);
}

static
PyCodeObject *
get_current_code_object(JitOptContext *ctx)
{
    return (PyCodeObject *)ctx->frame->code;
}

static PyObject *
get_co_name(JitOptContext *ctx, int index)
{
    return PyTuple_GET_ITEM(get_current_code_object(ctx)->co_names, index);
}

static int
get_test_bit_for_bools(void) {
#ifdef Py_STACKREF_DEBUG
    uintptr_t false_bits = _Py_STACKREF_FALSE_INDEX;
    uintptr_t true_bits = _Py_STACKREF_TRUE_INDEX;
#else
    uintptr_t false_bits = (uintptr_t)&_Py_FalseStruct;
    uintptr_t true_bits = (uintptr_t)&_Py_TrueStruct;
#endif
    for (int i = 4; i < 8; i++) {
        if ((true_bits ^ false_bits) & (uintptr_t)(1 << i)) {
            return i;
        }
    }
    return 0;
}

static int
test_bit_set_in_true(int bit) {
#ifdef Py_STACKREF_DEBUG
    uintptr_t true_bits = _Py_STACKREF_TRUE_INDEX;
#else
    uintptr_t true_bits = (uintptr_t)&_Py_TrueStruct;
#endif
    assert((true_bits ^ ((uintptr_t)&_Py_FalseStruct)) & (uintptr_t)(1 << bit));
    return true_bits & (uintptr_t)(1 << bit);
}

#ifdef Py_DEBUG
void
_Py_opt_assert_within_stack_bounds(
    _Py_UOpsAbstractFrame *frame, JitOptRef *stack_pointer,
    const char *filename, int lineno
) {
    if (frame->code == ((PyCodeObject *)&_Py_InitCleanup) ||
        frame->code == ((PyCodeObject *)&_Py_BinaryOpCleanup)) {
        return;
    }
    int level = (int)(stack_pointer - frame->stack);
    if (level < 0) {
        printf("Stack underflow (depth = %d) at %s:%d\n", level, filename, lineno);
        fflush(stdout);
        abort();
    }
    int size = (int)(frame->stack_len) + MAX_CACHED_REGISTER;
    if (level > size) {
        printf("Stack overflow (depth = %d) at %s:%d\n", level, filename, lineno);
        fflush(stdout);
        abort();
    }
}
#endif

#ifdef Py_DEBUG
#define ASSERT_WITHIN_STACK_BOUNDS(F, L) _Py_opt_assert_within_stack_bounds(ctx->frame, stack_pointer, (F), (L))
#else
#define ASSERT_WITHIN_STACK_BOUNDS(F, L) (void)0
#endif

/* Fold Grail generic and operator dispatch into constants guarded by
 * executor invalidation. PYTHON_GRAIL_JIT_FOLD_DISPATCH=0 disables it, as an
 * ablation (use a fresh process for each setting). */
static int
grail_fold_dispatch(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_FOLD_DISPATCH");
    return value == NULL || strcmp(value, "0") != 0;
}

/* Type record field loads from their plans when the record's type is known
 * (_LOAD_ATTR_RECORD_TYPED). PYTHON_GRAIL_JIT_TYPED_FIELDS=0 disables it. */
static int
grail_typed_fields(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_TYPED_FIELDS");
    return value == NULL || strcmp(value, "0") != 0;
}

/* Fused element-wise float arithmetic.
 *
 * A registered operator implementation such as
 *
 *     def _(a: Vec3, b: Vec3) -> Vec3:
 *         return Vec3(a.x - b.x, a.y - b.y, a.z - b.z)
 *
 * on a record type whose fields are all float is traced as a pushed frame
 * with field loads, float operations, CALL_RECORD and a return. When the
 * inlined body is exactly that shape (one of the kernel kinds: record op
 * record, record op float, float op record, the same op on every field in
 * field order), the call, the body and the return are replaced by
 * _GRAIL_ELEMENTWISE_FLOAT, which computes on doubles and reuses a record
 * operand nobody else references. The selection and the callee's global are
 * constants under executor invalidation, as for folded dispatch; the uop
 * checks the operand types, which the trace may only have observed.
 *
 * PYTHON_GRAIL_JIT_FUSE_ELEMENTWISE=0 disables it (fresh process per
 * setting). */
static int
grail_fuse_elementwise_enabled(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_FUSE_ELEMENTWISE");
    return value == NULL || strcmp(value, "0") != 0;
}

/* Abstract values of the callee's stack. An EW_EXPR is a float computed
 * from the arguments, as a postfix _PyGrailFloatProgram fragment. */
enum { EW_OTHER, EW_NULL, EW_ARG, EW_EXPR, EW_CONST, EW_RESULT };

typedef struct {
    int tag;
    int arg;                /* EW_ARG */
    PyObject *obj;          /* EW_CONST; EW_RESULT: the record type */
    int len;                /* EW_EXPR */
    uint8_t code[_PyGrail_FLOAT_PROGRAM_MAX];
} GrailEwValue;

#define GRAIL_EW_STACK 16
#define EW_FAIL() do { DPRINTF(2, "grail fuse: no (line %d)\n", __LINE__); return 0; } while (0)

#define EW_LOAD(ARG, FIELD) ((uint8_t)(((ARG) << 4) | (FIELD)))
#define EW_OP(OP) ((uint8_t)(((OP) + 1) << 6))   /* OP 0-2: + - * */

/* The element-wise shape of a CALL_RECORD argument i: field i of argument
 * 0 or 1 combined with field i of the other or with it as a scalar.
 * Returns the kind (0-2) and sets *op, or returns -1. */
static int
grail_ew_kind(const GrailEwValue *v, int i, int *op)
{
    if (v->tag != EW_EXPR || v->len != 3 || (v->code[2] >> 6) == 0) {
        return -1;
    }
    *op = (v->code[2] >> 6) - 1;
    uint8_t x = v->code[0], y = v->code[1];
    if (x == EW_LOAD(0, i) && y == EW_LOAD(1, i)) {
        return 0;
    }
    if (x == EW_LOAD(0, i) && y == EW_LOAD(1, _PyGrail_FLOAT_SCALAR)) {
        return 1;
    }
    if (x == EW_LOAD(0, _PyGrail_FLOAT_SCALAR) && y == EW_LOAD(1, i)) {
        return 2;
    }
    return -1;
}

/* Interpret an inlined callee abstractly, from the first uop after its
 * _PUSH_FRAME up to its return. Arguments have the exact types in types[]
 * (records need all-float fields to be loaded from); a NULL entry is
 * learned from the callee's recorded types and float guards (the fused uops
 * check argument types). On success, *result is
 * the returned value (EW_EXPR, or EW_RESULT for a record built from
 * EW_EXPR fields: its fields are checked to be element-wise) and *end is
 * the first trace entry after the return. */
static int
grail_scan_callee(_PyUOpInstruction *p, _PyUOpInstruction *trace_end,
                  PyObject *globals, PyTypeObject **types, int nargs,
                  GrailEwValue *result, _PyUOpInstruction **end)
{
    GrailEwValue stack[GRAIL_EW_STACK];
    int sp = 0;
#define EW_PUSH(V) do { if (sp >= GRAIL_EW_STACK) EW_FAIL(); stack[sp++] = (V); } while (0)
#define EW_NEED(N) do { if (sp < (N)) EW_FAIL(); } while (0)
    for (;; p++) {
        if (p >= trace_end) {
            EW_FAIL();
        }
        GrailEwValue v;
        v.tag = EW_OTHER;
        v.len = 0;
        int opcode = p->opcode;
        switch (opcode) {
            case _CHECK_VALIDITY:
            case _SET_IP:
            case _NOP:
            case _TIER2_RESUME_CHECK:
            case _MAKE_HEAP_SAFE:
            case _RECORD_CODE:
            case _GUARD_IP__PUSH_FRAME:
            case _GUARD_CODE_VERSION__PUSH_FRAME:
                continue;
            case _RECORD_TOS_TYPE:
                if (sp > 0 && stack[sp - 1].tag == EW_ARG &&
                    types[stack[sp - 1].arg] == NULL) {
                    types[stack[sp - 1].arg] = (PyTypeObject *)p->operand0;
                }
                continue;
            case _LOAD_FAST:
            case _LOAD_FAST_BORROW:
                if (p->oparg >= nargs) {
                    EW_FAIL();
                }
                v.tag = EW_ARG;
                v.arg = p->oparg;
                EW_PUSH(v);
                continue;
            case _PUSH_NULL:
                v.tag = EW_NULL;
                EW_PUSH(v);
                continue;
            case _PUSH_NULL_CONDITIONAL:
                if (p->oparg & 1) {
                    v.tag = EW_NULL;
                    EW_PUSH(v);
                }
                continue;
            case _LOAD_GLOBAL_MODULE: {
                if (incorrect_keys(globals, (uint32_t)p->operand0) ||
                    get_mutations(globals) >= _Py_MAX_ALLOWED_GLOBALS_MODIFICATIONS) {
                    EW_FAIL();
                }
                _PyUOpInstruction copy = *p;
                v.obj = convert_global_to_const(&copy, globals);
                if (v.obj == NULL) {
                    EW_FAIL();
                }
                v.tag = EW_CONST;
                EW_PUSH(v);  /* _PUSH_NULL_CONDITIONAL follows */
                continue;
            }
            case _GUARD_TYPE_VERSION: {
                EW_NEED(1);
                GrailEwValue *g = &stack[sp - 1];
                if (g->tag != EW_ARG || types[g->arg] == NULL ||
                    types[g->arg]->tp_version_tag != (uint32_t)p->operand0) {
                    EW_FAIL();
                }
                continue;
            }
            case _GUARD_TOS_FLOAT:
            case _GUARD_NOS_FLOAT: {
                EW_NEED(2);
                GrailEwValue *g = &stack[sp - (opcode == _GUARD_TOS_FLOAT ? 1 : 2)];
                if (g->tag == EW_ARG && types[g->arg] == NULL) {
                    types[g->arg] = &PyFloat_Type;
                }
                if (g->tag != EW_EXPR &&
                    !(g->tag == EW_ARG && types[g->arg] == &PyFloat_Type)) {
                    EW_FAIL();
                }
                continue;
            }
            case _LOAD_ATTR_RECORD:
            case _LOAD_ATTR_RECORD_TYPED:
            case _LOAD_ATTR_RECORD_BORROW:
            case _LOAD_ATTR_RECORD_TYPED_BORROW: {
                EW_NEED(1);
                GrailEwValue owner = stack[sp - 1];
                Py_ssize_t offset = (Py_ssize_t)p->operand0 -
                                    (Py_ssize_t)offsetof(RecordObject, fields);
                if (owner.tag != EW_ARG || offset < 0 ||
                    offset % (Py_ssize_t)sizeof(PyObject *) != 0) {
                    EW_FAIL();
                }
                Py_ssize_t field = offset / (Py_ssize_t)sizeof(PyObject *);
                /* A field of a conforming all-float record is a float. */
                if (field >= _PyGrail_FLOAT_SCALAR || types[owner.arg] == NULL ||
                    field >= _PyGrail_FloatRecordFieldCount(types[owner.arg])) {
                    EW_FAIL();
                }
                v.tag = EW_EXPR;
                v.len = 1;
                v.code[0] = EW_LOAD(owner.arg, (int)field);
                stack[sp - 1] = v;
                EW_PUSH(owner);
                continue;
            }
            case _POP_TOP:
            case _POP_TOP_NOP:
            case _POP_TOP_FLOAT:
                EW_NEED(1);
                sp--;
                continue;
            case _BINARY_OP_ADD_FLOAT:
            case _BINARY_OP_SUBTRACT_FLOAT:
            case _BINARY_OP_MULTIPLY_FLOAT: {
                EW_NEED(2);
                GrailEwValue x = stack[sp - 2], y = stack[sp - 1];
                v.tag = EW_EXPR;
                GrailEwValue *operands[2] = {&x, &y};
                for (int k = 0; k < 2; k++) {
                    GrailEwValue *o = operands[k];
                    if (o->tag == EW_ARG && types[o->arg] == &PyFloat_Type) {
                        if (v.len + 1 > _PyGrail_FLOAT_PROGRAM_MAX) {
                            EW_FAIL();
                        }
                        v.code[v.len++] = EW_LOAD(o->arg, _PyGrail_FLOAT_SCALAR);
                    }
                    else if (o->tag == EW_EXPR) {
                        if (v.len + o->len > _PyGrail_FLOAT_PROGRAM_MAX) {
                            EW_FAIL();
                        }
                        memcpy(v.code + v.len, o->code, o->len);
                        v.len += o->len;
                    }
                    else {
                        EW_FAIL();
                    }
                }
                if (v.len + 1 > _PyGrail_FLOAT_PROGRAM_MAX) {
                    EW_FAIL();
                }
                v.code[v.len++] = EW_OP(opcode == _BINARY_OP_ADD_FLOAT ? 0 :
                                        opcode == _BINARY_OP_SUBTRACT_FLOAT ? 1 : 2);
                stack[sp - 2] = v;
                stack[sp - 1] = x;
                EW_PUSH(y);
                continue;
            }
            case _GUARD_CALLABLE_RECORD:
                continue;  /* checked at _CALL_RECORD */
            case _CALL_RECORD: {
                int n = p->oparg;
                EW_NEED(n + 2);
                GrailEwValue callable = stack[sp - n - 2];
                if (n == 0 || callable.tag != EW_CONST ||
                    stack[sp - n - 1].tag != EW_NULL ||
                    !PyType_Check(callable.obj) ||
                    _PyGrail_FloatRecordFieldCount((PyTypeObject *)callable.obj) != n ||
                    !_PyGrail_IsPositionalRecordCall(callable.obj, n)) {
                    EW_FAIL();
                }
                /* Only element-wise records: the same op and kind on every
                 * field. */
                int op = -1, kind = -1;
                for (int i = 0; i < n; i++) {
                    int o;
                    int k = grail_ew_kind(&stack[sp - n + i], i, &o);
                    if (k < 0 || (kind >= 0 && (k != kind || o != op))) {
                        EW_FAIL();
                    }
                    kind = k;
                    op = o;
                }
                sp -= n + 2;
                v.tag = EW_RESULT;
                v.obj = callable.obj;
                v.arg = op | (kind << 2);
                EW_PUSH(v);
                EW_PUSH(callable);
                continue;
            }
            case _RETURN_VALUE:
                break;
            default:
                DPRINTF(2, "grail fuse: unhandled %s\n", _PyOpcode_uop_name[opcode]);
                EW_FAIL();
        }
        break;  /* _RETURN_VALUE */
    }
#undef EW_PUSH
#undef EW_NEED
    if (sp != 1 || (stack[0].tag != EW_RESULT && stack[0].tag != EW_EXPR)) {
        EW_FAIL();
    }
    *result = stack[0];
    /* Skip the return's own checks: the caller continues after them. */
    p++;
    while (p < trace_end && (p->opcode == _RECORD_CODE ||
                             p->opcode == _GUARD_IP_RETURN_VALUE ||
                             p->opcode == _GUARD_CODE_VERSION_RETURN_VALUE)) {
        p++;
    }
    if (p >= trace_end) {
        EW_FAIL();
    }
    *end = p;
    return 1;
}

/* The frame push needs no recursion check any more. */
static void
grail_drop_recursion_check(JitOptContext *ctx)
{
    if (ctx->out_buffer.next > ctx->out_buffer.start &&
        ctx->out_buffer.next[-1].opcode == _CHECK_RECURSION_REMAINING) {
        ctx->out_buffer.next--;
    }
}

/* The exact type of an operand: known, or as observed while tracing (the
 * fused uops check it). */
static PyTypeObject *
grail_operand_type(JitOptRef ref)
{
    PyTypeObject *type = sym_get_type(ref);
    return type != NULL ? type : sym_get_probable_type(ref);
}

/* Returns the number of trace entries consumed (from this_instr, the
 * _GRAIL_BINARY_CHECK_FUNC) and emits the fused form, or returns 0 and emits
 * nothing. */
static int
grail_fuse_elementwise(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                       _PyUOpInstruction *trace_end, JitOptRef **stack_pointer_p,
                       _PyBloomFilter *dependencies)
{
    JitOptRef *stack_pointer = *stack_pointer_p;
    if (!grail_fuse_elementwise_enabled() || !grail_fold_dispatch() ||
        STACK_LEVEL() < 2) {
        EW_FAIL();
    }
    JitOptRef left = stack_pointer[-2];
    JitOptRef right = stack_pointer[-1];
    PyTypeObject *types[2] = {grail_operand_type(left), grail_operand_type(right)};
    if (types[0] == NULL || types[1] == NULL) {
        EW_FAIL();
    }
    PyObject *impl = _PyGrail_BinaryOpLookup(types[0], types[1], this_instr->oparg);
    if (impl == NULL || !PyFunction_Check(impl) ||
        ((PyFunctionObject *)impl)->func_version != this_instr->operand0) {
        EW_FAIL();
    }
    PyObject *globals = ((PyFunctionObject *)impl)->func_globals;
    _PyUOpInstruction *p = this_instr + 1;
    if (p + 1 >= trace_end || p[0].opcode != _GRAIL_BINARY_INIT_CALL ||
        p[1].opcode != _PUSH_FRAME) {
        EW_FAIL();
    }
    GrailEwValue result;
    _PyUOpInstruction *end;
    if (!grail_scan_callee(p + 2, trace_end, globals, types, 2, &result, &end) ||
        result.tag != EW_RESULT) {
        EW_FAIL();
    }
    PyTypeObject *record_type = (PyTypeObject *)result.obj;
    int ew_oparg = result.arg;
    int kind = ew_oparg >> 2;
    /* The operand types the shape needs: kind 0 (R, R), 1 (R, float),
     * 2 (float, R). */
    PyTypeObject *want[2] = {kind == 2 ? &PyFloat_Type : record_type,
                             kind == 1 ? &PyFloat_Type : record_type};
    if (types[0] != want[0] || types[1] != want[1]) {
        EW_FAIL();
    }

    grail_drop_recursion_check(ctx);
    /* The uop checks the operand types itself. */
    sym_set_type(left, types[0]);
    sym_set_type(right, types[1]);
    ADD_OP(_GRAIL_ELEMENTWISE_FLOAT, ew_oparg, (uintptr_t)record_type);
    _Py_BloomFilter_Add(dependencies, _PyGrail_BinaryOpGeneric(this_instr->oparg));
    _Py_BloomFilter_Add(dependencies, impl);
    PyDict_Watch(GLOBALS_WATCHER_ID, globals);
    _Py_BloomFilter_Add(dependencies, globals);
    watch_type(record_type, dependencies);
    JitOptRef res = sym_new_type(ctx, record_type);
    grail_set_conforming(ctx, res);
    /* Pop r, then l, as the POP_TOPs after the uop do. */
    optimize_pop_top(ctx, this_instr, right);
    optimize_pop_top(ctx, this_instr, left);
    stack_pointer[-2] = res;
    stack_pointer -= 1;
    *stack_pointer_p = stack_pointer;
    return (int)(end - this_instr);
}

/* A call of a Python function (CALL_PY_EXACT_ARGS, from its
 * _CHECK_FUNCTION_VERSION) whose inlined body returns a float expression
 * over float fields of record arguments and float arguments, such as
 *
 *     def dot(a: Vec3, b: Vec3) -> float:
 *         return a.x * b.x + a.y * b.y + a.z * b.z
 *
 * becomes _GRAIL_FLOAT_PROGRAM: no frame, the expression evaluated on
 * doubles in the same order. The function is the trace's (its version is
 * checked, or it is a constant under invalidation). Same return convention
 * as grail_fuse_elementwise. Disabled with the element-wise fusion. */
static int
grail_fuse_float_call(JitOptContext *ctx, _PyUOpInstruction *this_instr,
                      _PyUOpInstruction *trace_end, JitOptRef **stack_pointer_p,
                      _PyBloomFilter *dependencies)
{
    JitOptRef *stack_pointer = *stack_pointer_p;
    int nargs = this_instr->oparg;
    if (!grail_fuse_elementwise_enabled() || nargs < 1 ||
        nargs > _PyGrail_FLOAT_PROGRAM_ARGS || STACK_LEVEL() < nargs + 2) {
        EW_FAIL();
    }
    JitOptRef callable = stack_pointer[-nargs - 2];
    JitOptRef self_or_null = stack_pointer[-nargs - 1];
    if (!sym_is_null(self_or_null)) {
        EW_FAIL();
    }
    PyObject *func = sym_get_const(ctx, callable);
    bool known = func != NULL;
    if (func == NULL) {
        func = sym_get_probable_value(callable);
    }
    if (func == NULL || !PyFunction_Check(func) ||
        ((PyFunctionObject *)func)->func_version != (uint32_t)this_instr->operand0) {
        EW_FAIL();
    }
    PyCodeObject *code = (PyCodeObject *)PyFunction_GET_CODE(func);
    if (code->co_argcount != nargs || code->co_nlocalsplus != nargs ||
        (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))) {
        EW_FAIL();
    }
    _PyGrailFloatProgram program;
    memset(&program, 0, sizeof(program));
    program.nargs = (uint8_t)nargs;
    for (int i = 0; i < nargs; i++) {
        program.types[i] = grail_operand_type(stack_pointer[-nargs + i]);
    }
    /* The call's own checks, then the frame push. */
    _PyUOpInstruction *p = this_instr + 1;
    for (;; p++) {
        if (p >= trace_end) {
            EW_FAIL();
        }
        int op = p->opcode;
        if (op == _PUSH_FRAME) {
            break;
        }
        if (op != _CHECK_FUNCTION_EXACT_ARGS && op != _CHECK_STACK_SPACE &&
            op != _CHECK_RECURSION_REMAINING && op != _INIT_CALL_PY_EXACT_ARGS &&
            op != _SAVE_RETURN_OFFSET) {
            EW_FAIL();
        }
    }
    GrailEwValue result;
    _PyUOpInstruction *end;
    if (!grail_scan_callee(p + 1, trace_end, ((PyFunctionObject *)func)->func_globals,
                           program.types, nargs, &result, &end) ||
        result.tag != EW_EXPR) {
        EW_FAIL();
    }
    for (int i = 0; i < nargs; i++) {
        PyTypeObject *type = program.types[i];
        if (type == NULL ||
            (type != &PyFloat_Type && _PyGrail_FloatRecordFieldCount(type) < 0)) {
            EW_FAIL();
        }
    }
    program.len = (uint8_t)result.len;
    memcpy(program.code, result.code, result.len);
    const _PyGrailFloatProgram *interned = _PyGrail_InternFloatProgram(&program);
    if (interned == NULL) {
        EW_FAIL();
    }

    if (!known) {
        ADD_OP(_CHECK_FUNCTION_VERSION, nargs, this_instr->operand0);
    }
    ADD_OP(_GRAIL_FLOAT_PROGRAM, nargs, (uintptr_t)interned);
    _Py_BloomFilter_Add(dependencies, func);
    PyObject *globals = ((PyFunctionObject *)func)->func_globals;
    PyDict_Watch(GLOBALS_WATCHER_ID, globals);
    _Py_BloomFilter_Add(dependencies, globals);
    for (int i = 0; i < nargs; i++) {
        sym_set_type(stack_pointer[-nargs + i], program.types[i]);
    }
    stack_pointer -= nargs + 1;
    stack_pointer[-1] = PyJitRef_MakeUnique(sym_new_type(ctx, &PyFloat_Type));
    *stack_pointer_p = stack_pointer;
    return (int)(end - this_instr);
}

/* >0 (length) for success, 0 for not ready, clears all possible errors. */
/* Virtual objects.
 *
 * A Grail record built by _CALL_RECORD[_CHECKED] from a constant record
 * type, or a tuple built by _BUILD_TUPLE, is kept virtual when the whole of
 * its life in the trace, typically from the function that builds it,
 * through inlined returns and calls, to the field loads or unpacking that
 * consume it, needs no real object:
 *
 *  - every uop that takes it as an input is one that knows virtual objects:
 *    field loads, unpacking, pops, local stores and loads, copies, swaps,
 *    inlined returns, argument passing to an inlined call, guards that do
 *    not read it, and type version guards that its exact type passes;
 *  - no barrier runs while it is live: a uop that may run arbitrary code or
 *    inspect frames (calls, decrefs of unknown objects...). The exceptions
 *    are decrefs of values known to be safe, returns from frames whose
 *    locals are all safe, uops that escape only to raise, and calls of
 *    math functions and a few builtins on exact floats and ints;
 *  - it is dead before the trace ends (no loop-carried objects yet) and is
 *    not returned out of the trace's root frame.
 *
 * Otherwise it escapes, which here means it stays an ordinary allocation
 * for its whole life: nothing is materialized on the main path.
 *
 * Tier-2 frames are real interpreter frames and uops address the physical
 * stack, so the lowering keeps the stack shape: every slot that holds a
 * virtual object holds the immortal _PyVO_Placeholder, and its fields are
 * owned by the thread's vo_shadow from a slot base the optimizer assigns.
 * _VO_NEW_RECORD and _VO_NEW_TUPLE move the arguments there, _VO_LOAD_FIELD
 * and _VO_UNPACK read them, pops become _POP_TOP_NOP, and _VO_RELEASE drops
 * the fields after the uop at which the object dies (no abstract location
 * holds it any more). A virtual record always conforms: _VO_NEW_RECORD
 * deopts before consuming anything when the arguments do not.
 *
 * Exits: before each uop that can leave the trace while virtual objects
 * are live, the optimizer records where they are (frame, local or stack
 * index) and inserts a _VO_EXIT_INFO marker with a _PyVORecovery.
 * prepare_for_execution() gives that uop private exit and error stubs whose
 * first uop rebuilds each object once and stores it into every slot that
 * holds it (see _PyVO_Recover() in optimizer.c), so tier 1 and other
 * executors only ever see real objects.
 *
 * The analysis runs alongside the abstract interpreter: it already models
 * inlined frames, locals and stacks, and a symbol identifies one
 * allocation. The rewrite runs once the trace is complete, when it is known
 * which candidates escaped. PYTHON_GRAIL_JIT_VIRTUAL_OBJECTS=0 disables it
 * (fresh process per setting). */

enum {
    VO_USE_NEW,         // the construction
    VO_USE_LOAD,        // a field load
    VO_USE_POP,         // a pop: becomes _POP_TOP_NOP
    VO_USE_NOP,         // a guard it passes: removed
    VO_USE_UNPACK,      // unpacking of a tuple
};

#define VO_MAX_FIELDS 16
#define VO_MAX_INPUTS 32
#define VO_MAX_PRE_LOCS 64

static int
vo_enabled(void)
{
    const char *value = Py_GETENV("PYTHON_GRAIL_JIT_VIRTUAL_OBJECTS");
    return value == NULL || strcmp(value, "0") != 0;
}

/* The state of the abstract interpreter before a uop. */
typedef struct {
    uint16_t opcode;
    int oparg;
    int npop;
    int depth;                  // ctx->curr_frame_depth
    bool any_live;
    JitOptRef inputs[VO_MAX_INPUTS];   // bottom first
    int input_cand[VO_MAX_INPUTS];     // candidate index, or -1
    bool inputs_overflow;
    JitOptRef *caller_locals;   // _RETURN_VALUE: the caller's locals
    bool returning_safe;        // _RETURN_VALUE: the frame's locals are safe
    int nlocs;
    _PyVOLoc locs[VO_MAX_PRE_LOCS];
    bool locs_overflow;
    bool pending_locs;          // a candidate is in a frame not yet pushed
} VOPre;

static inline bool
vo_live(const _PyVOCandidate *c)
{
    return !c->escaped && c->death < 0;
}

/* The live candidate whose symbol ref refers to, or -1. */
static int
vo_find(JitOptContext *ctx, JitOptRef ref)
{
    JitOptSymbol *sym = PyJitRef_Unwrap(ref);
    if (sym == NULL) {
        return -1;
    }
    for (int i = 0; i < ctx->vo.ncands; i++) {
        _PyVOCandidate *c = &ctx->vo.cands[i];
        if (c->sym == sym && vo_live(c)) {
            return i;
        }
    }
    return -1;
}

static void
vo_free_slots(JitOptContext *ctx, _PyVOCandidate *c)
{
    uint64_t mask = (c->n == 64 ? UINT64_MAX : (((uint64_t)1 << c->n) - 1)) << c->base;
    ctx->vo.shadow_used &= ~mask;
}

static void
vo_escape(JitOptContext *ctx, int i, const char *why)
{
    _PyVOCandidate *c = &ctx->vo.cands[i];
    if (!vo_live(c)) {
        return;
    }
    DPRINTF(2, "vo: candidate %d (created at %d) escapes: %s\n", i, c->create, why);
    c->escaped = true;
    vo_free_slots(ctx, c);
}

static void
vo_escape_all(JitOptContext *ctx, const char *why)
{
    for (int i = 0; i < ctx->vo.ncands; i++) {
        vo_escape(ctx, i, why);
    }
}

static bool
vo_any_live(JitOptContext *ctx)
{
    for (int i = 0; i < ctx->vo.ncands; i++) {
        if (vo_live(&ctx->vo.cands[i])) {
            return true;
        }
    }
    return false;
}

/* Releasing a reference to a value of this exact type runs no code. */
static bool
vo_safe_scalar_type(PyTypeObject *type)
{
    return type == &PyFloat_Type || type == &PyLong_Type ||
           type == &PyBool_Type || type == &PyUnicode_Type ||
           type == &PyBytes_Type || type == &PyComplex_Type ||
           type == Py_TYPE(Py_None);
}

/* Releasing a conforming record of this type runs no code: every plan is
 * an exact safe scalar, such a record, or a union of those. */
static bool
vo_record_safe(PyTypeObject *type, int depth)
{
    const GrailPlan *nodes = _PyGrail_PlanNodes(type);
    if (nodes == NULL || depth > 4) {
        return false;
    }
    Py_ssize_t n = (type->tp_basicsize - (Py_ssize_t)offsetof(RecordObject, fields)) /
                   (Py_ssize_t)sizeof(PyObject *);
    for (Py_ssize_t i = 0; i < n; i++) {
        const GrailPlan *p = &nodes[i];
        const GrailPlan *alts = p;
        uint32_t nalts = 1;
        if (p->kind == _PyGrail_PLAN_UNION && p->height == 1) {
            alts = nodes + p->first;
            nalts = p->n;
        }
        for (uint32_t k = 0; k < nalts; k++) {
            const GrailPlan *a = &alts[k];
            if (a->kind == _PyGrail_PLAN_EXACT && vo_safe_scalar_type(a->type)) {
                continue;
            }
            if (a->kind == _PyGrail_PLAN_RECORD && vo_record_safe(a->type, depth + 1)) {
                continue;
            }
            return false;
        }
    }
    return true;
}

/* Dropping a reference to ref runs no code. */
static bool
vo_decref_safe(JitOptContext *ctx, JitOptRef ref)
{
    if (sym_is_null(ref) || sym_is_const(ctx, ref) || vo_find(ctx, ref) >= 0) {
        return true;
    }
    PyTypeObject *type = sym_get_type(ref);
    if (type == NULL) {
        return false;
    }
    if (vo_safe_scalar_type(type)) {
        return true;
    }
    return _PyGrail_IsRecordTypeFast(type) && grail_known_conforming(ctx, ref) &&
           vo_record_safe(type, 0);
}

/* The conformance checks of _CALL_RECORD_CHECKED (bits of check) run no
 * code and cannot fail: leaf plans and unions of leaves. */
static bool
vo_record_checks_pure(PyTypeObject *type, int n, uint64_t check)
{
    const GrailPlan *nodes = _PyGrail_PlanNodes(type);
    if (nodes == NULL) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (!(check & ((uint64_t)1 << i))) {
            continue;
        }
        const GrailPlan *p = &nodes[i];
        if (!(p->height == 0 ||
              (p->kind == _PyGrail_PLAN_UNION && p->height == 1))) {
            return false;
        }
    }
    return true;
}

/* A call of a math function, or of abs/min/max, with exact float and int
 * arguments: it runs no Python code and only escapes to raise. */
static bool
vo_pure_builtin_call(JitOptContext *ctx, const VOPre *pre)
{
    if (pre->inputs_overflow || pre->npop < 3) {
        return false;
    }
    PyObject *callable = sym_get_const(ctx, pre->inputs[0]);
    if (callable == NULL || !PyCFunction_CheckExact(callable) ||
        !sym_is_null(pre->inputs[1])) {
        return false;
    }
    PyObject *module = ((PyCFunctionObject *)callable)->m_module;
    const char *name = ((PyCFunctionObject *)callable)->m_ml->ml_name;
    if (module == NULL || !PyUnicode_Check(module)) {
        return false;
    }
    if (!_PyUnicode_EqualToASCIIString(module, "math") &&
        !(_PyUnicode_EqualToASCIIString(module, "builtins") &&
          (strcmp(name, "abs") == 0 || strcmp(name, "min") == 0 ||
           strcmp(name, "max") == 0))) {
        return false;
    }
    for (int i = 2; i < pre->npop; i++) {
        PyTypeObject *type = sym_get_type(pre->inputs[i]);
        if (type != &PyFloat_Type && type != &PyLong_Type) {
            return false;
        }
    }
    return true;
}

static bool
vo_is_pop(int opcode)
{
    return opcode == _POP_TOP || opcode == _POP_TOP_NOP ||
           opcode == _POP_TOP_INT || opcode == _POP_TOP_FLOAT ||
           opcode == _POP_TOP_UNICODE;
}

static bool
vo_is_record_load(int opcode)
{
    return opcode == _LOAD_ATTR_RECORD || opcode == _LOAD_ATTR_RECORD_TYPED ||
           opcode == _LOAD_ATTR_RECORD_BORROW ||
           opcode == _LOAD_ATTR_RECORD_TYPED_BORROW;
}

static bool
vo_is_tuple_unpack(int opcode)
{
    return opcode == _UNPACK_SEQUENCE_TWO_TUPLE ||
           opcode == _UNPACK_SEQUENCE_TUPLE ||
           opcode == _UNPACK_SEQUENCE_UNIQUE_TWO_TUPLE ||
           opcode == _UNPACK_SEQUENCE_UNIQUE_THREE_TUPLE ||
           opcode == _UNPACK_SEQUENCE_UNIQUE_TUPLE;
}

/* Uops that only move references between locals and the stack. */
static bool
vo_is_move(int opcode)
{
    switch (opcode) {
        case _LOAD_FAST:
        case _LOAD_FAST_BORROW:
        case _LOAD_FAST_CHECK:
        case _LOAD_FAST_AND_CLEAR:
        case _SWAP_FAST:
        case _COPY:
        case _SWAP:
        case _MAKE_HEAP_SAFE:
            return true;
        default:
            return false;
    }
}

/* Call guards whose inputs from position 2 on are `unused`. */
static bool
vo_is_call_guard(int opcode)
{
    return opcode == _CHECK_FUNCTION_VERSION ||
           opcode == _CHECK_FUNCTION_EXACT_ARGS ||
           opcode == _CHECK_STACK_SPACE;
}

#define VO_EXIT_FLAGS (HAS_EXIT_FLAG | HAS_DEOPT_FLAG | HAS_PERIODIC_FLAG | HAS_ERROR_FLAG)

/* Scan the abstract frames for the locations of live candidates. With
 * pre != NULL, record them; return whether cand (if >= 0) was found. Frames
 * 1..depth are the trace's frames (frames[0] is the root); a frame
 * initialized for a call but not yet pushed is scanned too. */
static bool
vo_scan(JitOptContext *ctx, JitOptRef *stack_pointer, VOPre *pre, int cand)
{
    bool found = false;
    int depth = ctx->curr_frame_depth;
    int nframes = depth + (ctx->vo.pending_frame ? 1 : 0);
    for (int d = 0; d < nframes && d < MAX_ABSTRACT_FRAME_DEPTH; d++) {
        _Py_UOpsAbstractFrame *f = &ctx->frames[d];
        JitOptRef *sp = d == depth - 1 ? stack_pointer :
                        d == depth ? f->stack : f->stack_pointer;
        int nlocals = f->locals_len;
        int nstack = (int)(sp - f->stack);
        for (int k = 0; k < nlocals + nstack; k++) {
            bool is_stack = k >= nlocals;
            JitOptRef ref = is_stack ? f->stack[k - nlocals] : f->locals[k];
            int c = vo_find(ctx, ref);
            if (c < 0) {
                continue;
            }
            if (c == cand) {
                found = true;
            }
            if (pre != NULL) {
                if (d == depth) {
                    // A frame not yet pushed cannot be rebuilt at an exit.
                    pre->pending_locs = true;
                    continue;
                }
                if (pre->nlocs >= VO_MAX_PRE_LOCS) {
                    pre->locs_overflow = true;
                    continue;
                }
                _PyVOLoc *loc = &pre->locs[pre->nlocs++];
                loc->cand = (uint8_t)c;
                loc->depth = (uint8_t)(d + 1);
                loc->is_stack = is_stack;
                loc->index = (uint16_t)(is_stack ? k - nlocals : k);
            }
        }
    }
    return found;
}

/* Before the abstract interpreter runs this_instr. */
static void
vo_before(JitOptContext *ctx, _PyUOpInstruction *this_instr,
          JitOptRef *stack_pointer, VOPre *pre)
{
    pre->opcode = this_instr->opcode;
    pre->oparg = this_instr->oparg;
    pre->depth = ctx->curr_frame_depth;
    pre->any_live = vo_any_live(ctx);
    pre->npop = 0;
    pre->inputs_overflow = false;
    pre->nlocs = 0;
    pre->locs_overflow = false;
    pre->pending_locs = false;
    pre->caller_locals = NULL;
    pre->returning_safe = false;
    int opcode = this_instr->opcode;
    if (opcode == _CALL_RECORD || opcode == _CALL_RECORD_CHECKED ||
        opcode == _BUILD_TUPLE || pre->any_live) {
        pre->npop = _PyUop_num_inputs(opcode, this_instr->oparg);
        if (pre->npop < 0) {
            pre->npop = 0;
        }
        int n = pre->npop;
        if (n > VO_MAX_INPUTS) {
            pre->inputs_overflow = true;
        }
        for (int i = 0; i < n; i++) {
            JitOptRef ref = stack_pointer[i - n];
            int c = vo_find(ctx, ref);
            if (i >= VO_MAX_INPUTS) {
                if (c >= 0) {
                    vo_escape(ctx, c, "too many inputs");
                }
                continue;
            }
            pre->inputs[i] = ref;
            pre->input_cand[i] = c;
        }
    }
    if (!pre->any_live) {
        return;
    }
    vo_scan(ctx, stack_pointer, pre, -1);
    if (opcode == _RETURN_VALUE) {
        _Py_UOpsAbstractFrame *f = ctx->frame;
        pre->returning_safe = true;
        for (int k = 0; k < f->locals_len; k++) {
            if (!vo_decref_safe(ctx, f->locals[k])) {
                pre->returning_safe = false;
                break;
            }
        }
        if (ctx->curr_frame_depth >= 2) {
            pre->caller_locals = ctx->frames[ctx->curr_frame_depth - 2].locals;
        }
    }
}

static void
vo_add_use(JitOptContext *ctx, int out, int cand, int kind, int field)
{
    if (ctx->vo.nuses >= VO_MAX_USES) {
        vo_escape(ctx, cand, "too many uses");
        return;
    }
    _PyVOUse *u = &ctx->vo.uses[ctx->vo.nuses++];
    u->out = out;
    u->cand = (uint8_t)cand;
    u->kind = (uint8_t)kind;
    u->field = (uint8_t)field;
}

/* Try to make the construction just emitted (out_start[0]) a candidate. */
static void
vo_new_candidate(JitOptContext *ctx, const VOPre *pre,
                 _PyUOpInstruction *out_start, JitOptRef *stack_pointer)
{
    _PyVOState *vo = &ctx->vo;
    int opcode = out_start->opcode;
    PyTypeObject *type = NULL;
    int n = pre->oparg;
    JitOptRef res;
    if (opcode == _CALL_RECORD || opcode == _CALL_RECORD_CHECKED) {
        PyObject *callable = sym_get_const(ctx, pre->inputs[0]);
        if (callable == NULL || !PyType_Check(callable) ||
            !_PyGrail_IsRecordTypeFast((PyTypeObject *)callable)) {
            return;
        }
        type = (PyTypeObject *)callable;
        uint64_t check = opcode == _CALL_RECORD_CHECKED ? out_start->operand0 : UINT64_MAX;
        if (n > VO_MAX_FIELDS || !vo_record_checks_pure(type, n, check)) {
            return;
        }
        res = stack_pointer[-2];
    }
    else if (opcode == _BUILD_TUPLE) {
        if (n < 1 || n > VO_MAX_FIELDS) {
            return;
        }
        res = stack_pointer[-1];
    }
    else {
        return;
    }
    if (vo->ncands >= VO_MAX_CANDIDATES) {
        return;
    }
    for (int i = 0; i < pre->npop; i++) {
        if (pre->input_cand[i] >= 0) {
            // Nested virtual objects are not supported yet.
            return;
        }
    }
    int base = -1;
    uint64_t mask = ((uint64_t)1 << n) - 1;
    for (int b = 0; b + n <= _Py_VO_SHADOW_SIZE; b++) {
        if (!(vo->shadow_used & (mask << b))) {
            base = b;
            break;
        }
    }
    if (base < 0) {
        return;
    }
    bool safe = true;
    if (type != NULL) {
        safe = vo_record_safe(type, 0);
    }
    else {
        for (int i = 0; i < n; i++) {
            if (!vo_decref_safe(ctx, pre->inputs[i])) {
                safe = false;
            }
        }
    }
    int i = vo->ncands++;
    _PyVOCandidate *c = &vo->cands[i];
    c->sym = PyJitRef_Unwrap(res);
    c->type = type;
    c->create = (int)(out_start - ctx->out_buffer.start);
    c->death = -1;
    c->n = (uint8_t)n;
    c->base = (uint8_t)base;
    c->escaped = false;
    c->safe_release = safe;
    vo->shadow_used |= mask << base;
    vo_add_use(ctx, c->create, i, VO_USE_NEW, 0);
    DPRINTF(2, "vo: candidate %d at %d, %d fields, base %d\n", i, c->create, n, base);
}

/* Record the locations of the live candidates (from pre) for the output
 * uops [start, end). Consecutive equal snapshots are merged. */
static void
vo_snapshot(JitOptContext *ctx, const VOPre *pre, int start, int end)
{
    _PyVOState *vo = &ctx->vo;
    if (start == end) {
        return;
    }
    int nlive = 0;
    for (int k = 0; k < pre->nlocs; k++) {
        nlive += vo_live(&vo->cands[pre->locs[k].cand]);
    }
    if (vo->nsnaps > 0) {
        _PyVOSnapshot *last = &vo->snaps[vo->nsnaps - 1];
        if (last->out_end == start && last->depth == pre->depth &&
            last->nlocs == nlive) {
            bool same = true;
            int j = 0;
            for (int k = 0; k < pre->nlocs && same; k++) {
                const _PyVOLoc *a = &pre->locs[k];
                if (!vo_live(&vo->cands[a->cand])) {
                    continue;
                }
                const _PyVOLoc *b = &vo->locs[last->first_loc + j++];
                same = a->cand == b->cand && a->depth == b->depth &&
                       a->is_stack == b->is_stack && a->index == b->index;
            }
            if (same) {
                last->out_end = end;
                return;
            }
        }
    }
    if (vo->nsnaps >= VO_MAX_SNAPSHOTS || vo->nlocs + nlive > VO_MAX_LOCS) {
        vo_escape_all(ctx, "too many exits");
        return;
    }
    _PyVOSnapshot *snap = &vo->snaps[vo->nsnaps++];
    snap->out_start = start;
    snap->out_end = end;
    snap->depth = pre->depth;
    snap->first_loc = vo->nlocs;
    snap->nlocs = nlive;
    for (int k = 0; k < pre->nlocs; k++) {
        if (vo_live(&vo->cands[pre->locs[k].cand])) {
            vo->locs[vo->nlocs++] = pre->locs[k];
        }
    }
}

/* After the abstract interpreter ran pre's uop, emitting
 * [out_start, out_end). */
static void
vo_after(JitOptContext *ctx, const VOPre *pre, _PyUOpInstruction *out_start,
         _PyUOpInstruction *out_end, JitOptRef *stack_pointer, int fused)
{
    _PyVOState *vo = &ctx->vo;
    int opcode = pre->opcode;
    int nout = (int)(out_end - out_start);
    int out = (int)(out_start - ctx->out_buffer.start);
    if (ctx->done) {
        vo_escape_all(ctx, "optimizer stopped");
        return;
    }
    if (pre->any_live) {
        if (fused) {
            vo_escape_all(ctx, "fused call");
            return;
        }
        /* Inputs: is each candidate input consumed by a uop that knows
         * virtual objects? */
        for (int i = 0; i < pre->npop && i < VO_MAX_INPUTS; i++) {
            int c = pre->input_cand[i];
            if (c < 0 || !vo_live(&vo->cands[c])) {
                continue;
            }
            _PyVOCandidate *cand = &vo->cands[c];
            int op = nout == 1 ? out_start->opcode : -1;
            if (vo_is_record_load(opcode)) {
                if (cand->type != NULL && vo_is_record_load(op)) {
                    int field = (int)(((Py_ssize_t)out_start->operand0 -
                                       (Py_ssize_t)offsetof(RecordObject, fields)) /
                                      (Py_ssize_t)sizeof(PyObject *));
                    if (field >= 0 && field < cand->n) {
                        vo_add_use(ctx, out, c, VO_USE_LOAD, field);
                        continue;
                    }
                }
            }
            else if (vo_is_pop(opcode)) {
                if (nout == 0 || op == _NOP) {
                    continue;
                }
                if (vo_is_pop(op)) {
                    vo_add_use(ctx, out, c, VO_USE_POP, 0);
                    continue;
                }
            }
            else if (vo_is_tuple_unpack(opcode) || opcode == _UNPACK_SEQUENCE) {
                if (cand->type == NULL && cand->n == pre->oparg &&
                    (vo_is_tuple_unpack(op) || op == _UNPACK_SEQUENCE)) {
                    vo_add_use(ctx, out, c, VO_USE_UNPACK, 0);
                    continue;
                }
            }
            else if (opcode == _GUARD_TYPE_VERSION) {
                if (nout == 0 || op == _NOP) {
                    continue;
                }
                if (cand->type != NULL && op == _GUARD_TYPE_VERSION &&
                    cand->type->tp_version_tag != 0 &&
                    cand->type->tp_version_tag == (uint32_t)out_start->operand0) {
                    vo_add_use(ctx, out, c, VO_USE_NOP, 0);
                    continue;
                }
            }
            else if (opcode == _GUARD_TOS_TUPLE || opcode == _GUARD_NOS_TUPLE) {
                // The guard checks input 0; input 1 of _GUARD_NOS_TUPLE is
                // unused.
                if (nout == 0 || op == _NOP || i == 1) {
                    continue;
                }
                if (cand->type == NULL && op == opcode) {
                    vo_add_use(ctx, out, c, VO_USE_NOP, 0);
                    continue;
                }
            }
            else if (vo_is_move(opcode)) {
                if (nout == 1 && op == opcode) {
                    continue;
                }
            }
            else if (opcode == _RETURN_VALUE) {
                if (pre->caller_locals != NULL &&
                    ctx->curr_frame_depth == pre->depth - 1 &&
                    ctx->frame->locals == pre->caller_locals) {
                    continue;
                }
            }
            else if (opcode == _INIT_CALL_PY_EXACT_ARGS || vo_is_call_guard(opcode)) {
                if (i >= 2) {
                    continue;
                }
            }
            else if (_PyUop_Flags[opcode] & HAS_RECORDS_VALUE_FLAG) {
                continue;
            }
            vo_escape(ctx, c, _PyOpcode_uop_name[opcode]);
        }
        /* Uops that read or write locals other than by moving them. */
        if ((_PyUop_Flags[opcode] & HAS_LOCAL_FLAG) && !vo_is_move(opcode) &&
            opcode != _DELETE_FAST) {
            for (int k = 0; k < pre->nlocs; k++) {
                if (!pre->locs[k].is_stack && pre->locs[k].depth == pre->depth) {
                    vo_escape(ctx, pre->locs[k].cand, "local uop");
                }
            }
        }
        /* Barriers. */
        for (_PyUOpInstruction *p = out_start; p < out_end; p++) {
            int op = p->opcode;
            if (!(_PyUop_Flags[op] & HAS_ESCAPES_FLAG)) {
                continue;
            }
            bool exempt = false;
            if (vo_is_pop(op)) {
                exempt = vo_is_pop(opcode) && nout == 1 && pre->npop == 1 &&
                         vo_decref_safe(ctx, pre->inputs[0]);
            }
            else if (op == _RETURN_VALUE) {
                exempt = pre->returning_safe;
            }
            else if (op == _CALL_BUILTIN_O || op == _CALL_BUILTIN_FAST) {
                exempt = vo_pure_builtin_call(ctx, pre);
            }
            else if (op == _BINARY_OP_TRUEDIV_FLOAT || op == _BINARY_OP_EXTEND) {
                exempt = true;
            }
            else if (op == _CALL_RECORD || op == _CALL_RECORD_CHECKED) {
                PyObject *callable = pre->npop > 0 && !pre->inputs_overflow ?
                    sym_get_const(ctx, pre->inputs[0]) : NULL;
                exempt = callable != NULL && PyType_Check(callable) &&
                         vo_record_checks_pure((PyTypeObject *)callable, p->oparg,
                                               op == _CALL_RECORD_CHECKED ?
                                               p->operand0 : UINT64_MAX);
            }
            if (!exempt) {
                vo_escape_all(ctx, _PyOpcode_uop_name[op]);
                break;
            }
        }
        /* Ends of the trace. */
        for (_PyUOpInstruction *p = out_start; p < out_end; p++) {
            if (is_terminator_uop(p) || p->opcode == _YIELD_VALUE ||
                p->opcode == _RETURN_GENERATOR) {
                vo_escape_all(ctx, "end of trace");
            }
        }
        /* Exits: remember where the live candidates are. This is done for
         * every uop, not just the ones that can exit: the optimizer can turn
         * an earlier _NOP into a guard (_GUARD_GLOBALS_VERSION). */
        if (vo_any_live(ctx)) {
            bool exits = false;
            for (_PyUOpInstruction *p = out_start; p < out_end; p++) {
                if (_PyUop_Flags[p->opcode] & VO_EXIT_FLAGS) {
                    exits = true;
                }
            }
            if (pre->locs_overflow || (pre->pending_locs && exits)) {
                vo_escape_all(ctx, "locations");
            }
            else if (!pre->pending_locs) {
                vo_snapshot(ctx, pre, out, out + nout);
            }
        }
    }
    /* A new candidate. */
    if ((opcode == _CALL_RECORD || opcode == _CALL_RECORD_CHECKED ||
         opcode == _BUILD_TUPLE) && nout == 1 && !pre->inputs_overflow) {
        vo_new_candidate(ctx, pre, out_start, stack_pointer);
    }
    if (opcode == _INIT_CALL_PY_EXACT_ARGS) {
        vo->pending_frame = true;
    }
    else if (opcode == _PUSH_FRAME) {
        vo->pending_frame = false;
    }
    if (!pre->any_live) {
        return;
    }
    /* Deaths. */
    bool may_die = vo_is_pop(opcode) || vo_is_tuple_unpack(opcode) ||
                   opcode == _UNPACK_SEQUENCE || opcode == _RETURN_VALUE ||
                   opcode == _DELETE_FAST;
    for (int i = 0; i < vo->ncands; i++) {
        _PyVOCandidate *c = &vo->cands[i];
        if (!vo_live(c) || c->create >= out) {
            continue;
        }
        if (vo_scan(ctx, stack_pointer, NULL, i)) {
            continue;
        }
        if (!may_die) {
            // Lost track of it: keep it real.
            vo_escape(ctx, i, "lost");
            continue;
        }
        c->death = out + nout;
        vo_free_slots(ctx, c);
        DPRINTF(2, "vo: candidate %d dies before %d\n", i, c->death);
        if (!c->safe_release) {
            vo_escape_all(ctx, "unsafe release");
        }
    }
}

/* Rewrite the optimized trace for the candidates that did not escape:
 * constructions, uses, releases and exit markers. Returns the new length
 * (length if nothing changes). */
static int
vo_rewrite(_PyThreadStateImpl *tstate, JitOptContext *ctx,
           _PyUOpInstruction *buffer, int length)
{
    _PyVOState *vo = &ctx->vo;
    _PyJitTracerState *tracer = tstate->jit_tracer_state;
    if (tracer->vo_arena != NULL) {
        PyMem_Free(tracer->vo_arena);
        tracer->vo_arena = NULL;
    }
    int nvirtual = 0;
    for (int i = 0; i < vo->ncands; i++) {
        _PyVOCandidate *c = &vo->cands[i];
        if (!c->escaped) {
            assert(c->death >= 0);
            nvirtual++;
        }
    }
    if (vo->failed || nvirtual == 0) {
        return length;
    }
    /* Recovery descriptors: one per snapshot with virtual objects. */
    size_t arena_size = 0;
    int nmarkers = 0;
    for (int s = 0; s < vo->nsnaps; s++) {
        _PyVOSnapshot *snap = &vo->snaps[s];
        uint32_t seen = 0;
        int nobjs = 0, nlocs = 0;
        for (int k = 0; k < snap->nlocs; k++) {
            _PyVOLoc *loc = &vo->locs[snap->first_loc + k];
            if (vo->cands[loc->cand].escaped) {
                continue;
            }
            nlocs++;
            if (!(seen & (1u << loc->cand))) {
                seen |= 1u << loc->cand;
                nobjs++;
            }
        }
        if (nlocs == 0) {
            continue;
        }
        size_t size = sizeof(_PyVORecovery) + nobjs * sizeof(_PyVORecoveryObject) +
                      nlocs * sizeof(_PyVORecoveryLoc);
        arena_size += (size + 7) & ~(size_t)7;
        for (int i = snap->out_start; i < snap->out_end; i++) {
            if (_PyUop_Flags[buffer[i].opcode] & VO_EXIT_FLAGS) {
                nmarkers++;
            }
        }
    }
    if (length + nvirtual + nmarkers > UOP_MAX_TRACE_LENGTH - 8) {
        DPRINTF(1, "vo: trace too long\n");
        return length;
    }
    char *arena = NULL;
    if (arena_size > 0) {
        arena = PyMem_Malloc(arena_size);
        if (arena == NULL) {
            return length;
        }
    }
    _PyUOpInstruction *copy = PyMem_Malloc(length * sizeof(_PyUOpInstruction));
    int16_t *use_of = PyMem_Malloc(length * sizeof(int16_t));
    const _PyVORecovery **info_of = PyMem_Calloc(length, sizeof(_PyVORecovery *));
    if (copy == NULL || use_of == NULL || info_of == NULL) {
        PyMem_Free(arena);
        PyMem_Free(copy);
        PyMem_Free(use_of);
        PyMem_Free(info_of);
        return length;
    }
    memcpy(copy, buffer, length * sizeof(_PyUOpInstruction));
    for (int i = 0; i < length; i++) {
        use_of[i] = -1;
    }
    for (int u = 0; u < vo->nuses; u++) {
        _PyVOUse *use = &vo->uses[u];
        if (!vo->cands[use->cand].escaped) {
            assert(use_of[use->out] < 0);
            use_of[use->out] = (int16_t)u;
        }
    }
    char *next = arena;
    for (int s = 0; s < vo->nsnaps; s++) {
        _PyVOSnapshot *snap = &vo->snaps[s];
        int obj_of[VO_MAX_CANDIDATES];
        for (int i = 0; i < VO_MAX_CANDIDATES; i++) {
            obj_of[i] = -1;
        }
        bool any = false;
        for (int k = 0; k < snap->nlocs; k++) {
            any |= !vo->cands[vo->locs[snap->first_loc + k].cand].escaped;
        }
        if (!any) {
            continue;
        }
        _PyVORecovery *rec = (_PyVORecovery *)next;
        rec->nobjs = 0;
        rec->nlocs = 0;
        // Objects first, then locations (after the final object count).
        for (int k = 0; k < snap->nlocs; k++) {
            _PyVOLoc *loc = &vo->locs[snap->first_loc + k];
            _PyVOCandidate *c = &vo->cands[loc->cand];
            if (c->escaped || obj_of[loc->cand] >= 0) {
                continue;
            }
            obj_of[loc->cand] = rec->nobjs;
            _PyVORecoveryObject *obj = &rec->objs[rec->nobjs++];
            obj->type = c->type;
            obj->base = c->base;
            obj->n = c->n;
        }
        _PyVORecoveryLoc *locs = (_PyVORecoveryLoc *)(rec->objs + rec->nobjs);
        for (int k = 0; k < snap->nlocs; k++) {
            _PyVOLoc *loc = &vo->locs[snap->first_loc + k];
            if (vo->cands[loc->cand].escaped) {
                continue;
            }
            _PyVORecoveryLoc *l = &locs[rec->nlocs++];
            l->obj = (uint8_t)obj_of[loc->cand];
            l->frame_up = (uint8_t)(snap->depth - loc->depth);
            l->is_stack = loc->is_stack;
            l->index = loc->index;
        }
        size_t size = (char *)(locs + rec->nlocs) - next;
        next += (size + 7) & ~(size_t)7;
        for (int i = snap->out_start; i < snap->out_end; i++) {
            info_of[i] = rec;
        }
    }
    assert(next == arena + arena_size);
    int out = 0;
    for (int i = 0; i < length; i++) {
        _PyUOpInstruction inst = copy[i];
        for (int k = 0; k < vo->ncands; k++) {
            _PyVOCandidate *c = &vo->cands[k];
            if (!c->escaped && c->death == i) {
                _PyUOpInstruction *r = &buffer[out++];
                *r = inst;
                r->opcode = _VO_RELEASE;
                r->oparg = c->n;
                r->operand0 = c->base;
                r->operand1 = 0;
            }
        }
        int u = use_of[i];
        if (u >= 0) {
            _PyVOUse *use = &vo->uses[u];
            _PyVOCandidate *c = &vo->cands[use->cand];
            switch (use->kind) {
                case VO_USE_NEW:
                    if (c->type != NULL) {
                        if (inst.opcode == _CALL_RECORD) {
                            inst.operand0 = UINT64_MAX;
                        }
                        inst.opcode = _VO_NEW_RECORD;
                        inst.operand1 = c->base;
                    }
                    else {
                        assert(inst.opcode == _BUILD_TUPLE);
                        inst.opcode = _VO_NEW_TUPLE;
                        inst.operand0 = c->base;
                    }
                    break;
                case VO_USE_LOAD:
                    inst.opcode = (inst.opcode == _LOAD_ATTR_RECORD_BORROW ||
                                   inst.opcode == _LOAD_ATTR_RECORD_TYPED_BORROW) ?
                                  _VO_LOAD_FIELD_BORROW : _VO_LOAD_FIELD;
                    inst.operand0 = c->base + use->field;
                    break;
                case VO_USE_POP:
                    if (vo_is_pop(inst.opcode)) {
                        inst.opcode = _POP_TOP_NOP;
                    }
                    break;
                case VO_USE_NOP:
                    inst.opcode = _NOP;
                    break;
                case VO_USE_UNPACK:
                    inst.opcode = _VO_UNPACK;
                    inst.operand0 = c->base;
                    break;
            }
        }
        if (info_of[i] != NULL && (_PyUop_Flags[inst.opcode] & VO_EXIT_FLAGS)) {
            _PyUOpInstruction *m = &buffer[out++];
            *m = inst;
            m->opcode = _VO_EXIT_INFO;
            m->oparg = 0;
            m->operand0 = (uintptr_t)info_of[i];
            m->operand1 = 0;
        }
        buffer[out++] = inst;
        assert(out <= UOP_MAX_TRACE_LENGTH);
    }
    PyMem_Free(copy);
    PyMem_Free(use_of);
    PyMem_Free(info_of);
    tracer->vo_arena = arena;
    DPRINTF(1, "vo: %d virtual objects, %d -> %d uops\n", nvirtual, length, out);
    return out;
}

static int
optimize_uops(
    _PyThreadStateImpl *tstate,
    _PyUOpInstruction *trace,
    int trace_len,
    int curr_stacklen,
    _PyUOpInstruction *output,
    _PyBloomFilter *dependencies
)
{
    assert(!PyErr_Occurred());
    assert(tstate->jit_tracer_state != NULL);
    PyFunctionObject *func = tstate->jit_tracer_state->initial_state.func;

    JitOptContext *ctx = &tstate->jit_tracer_state->opt_context;
    uint32_t opcode = UINT16_MAX;

    uop_buffer_init(&ctx->out_buffer, output, UOP_MAX_TRACE_LENGTH);

    // Make sure that watchers are set up
    PyInterpreterState *interp = _PyInterpreterState_GET();
    _PyOnceFlag_CallOnce(&interp->dict_state.watcher_setup_once,
                         _setup_optimizer_watchers, NULL);

    _Py_uop_abstractcontext_init(ctx, dependencies);
    memset(&ctx->vo, 0, offsetof(_PyVOState, cands));
    ctx->vo.failed = !vo_enabled();
    _Py_UOpsAbstractFrame *frame = _Py_uop_frame_new(ctx, (PyCodeObject *)func->func_code, NULL, 0);
    if (frame == NULL) {
        return 0;
    }
    frame->func = func;
    ctx->curr_frame_depth++;
    ctx->frame = frame;
    _Py_uop_sym_set_stack_depth(ctx, curr_stacklen, frame->stack_pointer);

    _PyUOpInstruction *this_instr = NULL;
    JitOptRef *stack_pointer = ctx->frame->stack_pointer;

    for (int i = 0; i < trace_len; i++) {
        this_instr = &trace[i];
        if (ctx->done) {
            // Don't do any more optimization, but
            // we still need to reach a terminator for corrctness.
            *(ctx->out_buffer.next++) = *this_instr;
            if (is_terminator_uop(this_instr)) {
                break;
            }
            continue;
        }

        int oparg = this_instr->oparg;
        opcode = this_instr->opcode;

        if (!CURRENT_FRAME_IS_SHIM()) {
            stack_pointer = ctx->frame->stack_pointer;
        }

        DUMP_UOP(ctx, "abs", (int)(this_instr - trace), this_instr, stack_pointer);

        _PyUOpInstruction *out_ptr = ctx->out_buffer.next;
        _Py_UOpsAbstractFrame *frame_before = ctx->frame;
        VOPre vo_pre;
        bool vo_active = !ctx->vo.failed && !CURRENT_FRAME_IS_SHIM();
        if (vo_active) {
            vo_before(ctx, this_instr, stack_pointer, &vo_pre);
        }
        else if (!ctx->vo.failed) {
            vo_escape_all(ctx, "shim frame");
        }

        int fused = 0;
        if (opcode == _GRAIL_BINARY_CHECK_FUNC) {
            fused = grail_fuse_elementwise(ctx, this_instr, trace + trace_len,
                                           &stack_pointer, dependencies);
        }
        else if (opcode == _CHECK_FUNCTION_VERSION) {
            fused = grail_fuse_float_call(ctx, this_instr, trace + trace_len,
                                          &stack_pointer, dependencies);
        }
        if (fused) {
            i += fused - 1;
        }
        else switch (opcode) {

#include "optimizer_cases.c.h"

            default:
                DPRINTF(1, "\nUnknown opcode in abstract interpreter\n");
                Py_UNREACHABLE();
        }
        if (ctx->grail_borrow_count) {
            grail_borrow_barrier(ctx, this_instr, frame_before);
        }
        // If no ADD_OP was called during this iteration, copy the original instruction
        if (ctx->out_buffer.next == out_ptr) {
            *(ctx->out_buffer.next++) = *this_instr;
        }
        if (vo_active) {
            vo_after(ctx, &vo_pre, out_ptr, ctx->out_buffer.next,
                     stack_pointer, fused);
        }
        assert(ctx->frame != NULL);
        DUMP_UOPS(ctx, "out", out_ptr, stack_pointer);
        if (!CURRENT_FRAME_IS_SHIM() && !ctx->done) {
            DPRINTF(3, " stack_level %d\n", STACK_LEVEL());
            ctx->frame->stack_pointer = stack_pointer;
            assert(STACK_LEVEL() >= 0);
        }
    }
    if (ctx->out_of_space) {
        DPRINTF(3, "\n");
        DPRINTF(1, "Out of space in abstract interpreter\n");
    }
    if (ctx->contradiction) {
        // Attempted to push a "bottom" (contradiction) symbol onto the stack.
        // This means that the abstract interpreter has optimized to trace
        // to an unreachable estate.
        // We *could* generate an _EXIT_TRACE or _FATAL_ERROR here, but hitting
        // bottom usually indicates an optimizer bug, so we are probably better off
        // retrying later.
        DPRINTF(3, "\n");
        DPRINTF(1, "Hit bottom in abstract interpreter\n");
        _Py_uop_abstractcontext_fini(ctx);
        OPT_STAT_INC(optimizer_contradiction);
        return 0;
    }

    /* Either reached the end or cannot optimize further, but there
     * would be no benefit in retrying later */
    _Py_uop_abstractcontext_fini(ctx);
    // Check that the trace ends with a proper terminator
    if (uop_buffer_length(&ctx->out_buffer) > 0) {
        assert(is_terminator_uop(uop_buffer_last(&ctx->out_buffer)));
    }

    return uop_buffer_length(&ctx->out_buffer);

error:
    DPRINTF(3, "\n");
    DPRINTF(1, "Encountered error in abstract interpreter\n");
    if (opcode <= MAX_UOP_ID) {
        OPT_ERROR_IN_OPCODE(opcode);
    }
    _Py_uop_abstractcontext_fini(ctx);

    assert(PyErr_Occurred());
    PyErr_Clear();

    return 0;

}

const uint16_t op_without_push[MAX_UOP_ID + 1] = {
    [_COPY] = _NOP,
    [_LOAD_CONST_INLINE] = _NOP,
    [_LOAD_CONST_INLINE_BORROW] = _NOP,
    [_LOAD_FAST] = _NOP,
    [_LOAD_FAST_BORROW] = _NOP,
    [_LOAD_SMALL_INT] = _NOP,
    [_PUSH_NULL] = _NOP,
};

const bool op_skip[MAX_UOP_ID + 1] = {
    [_NOP] = true,
    [_CHECK_VALIDITY] = true,
    [_CHECK_PERIODIC] = true,
    [_SET_IP] = true,
};

const uint16_t op_without_pop[MAX_UOP_ID + 1] = {
    [_POP_TOP] = _NOP,
    [_POP_TOP_NOP] = _NOP,
    [_POP_TOP_INT] = _NOP,
    [_POP_TOP_FLOAT] = _NOP,
    [_POP_TOP_UNICODE] = _NOP,
};


static int
remove_unneeded_uops(_PyUOpInstruction *buffer, int buffer_size)
{
    /* Remove _SET_IP and _CHECK_VALIDITY where possible.
     * _SET_IP is needed if the following instruction escapes or
     * could error. _CHECK_VALIDITY is needed if the previous
     * instruction could have escaped. */
    int last_set_ip = -1;
    bool may_have_escaped = true;
    for (int pc = 0; pc < buffer_size; pc++) {
        int opcode = buffer[pc].opcode;
        switch (opcode) {
            case _START_EXECUTOR:
                may_have_escaped = false;
                break;
            case _SET_IP:
                buffer[pc].opcode = _NOP;
                last_set_ip = pc;
                break;
            case _CHECK_VALIDITY:
                if (may_have_escaped) {
                    may_have_escaped = false;
                }
                else {
                    buffer[pc].opcode = _NOP;
                    if (pc > 0 && buffer[pc - 1].opcode == _VO_EXIT_INFO) {
                        // Its virtual-object exit marker (vo_rewrite()).
                        buffer[pc - 1].opcode = _NOP;
                    }
                }
                break;
            case _EXIT_TRACE:
            default:
            {
                // Cancel out pushes and pops, repeatedly. So:
                //     _LOAD_FAST + _POP_TOP + _POP_TOP + _LOAD_CONST_INLINE_BORROW + _POP_TOP
                // ...becomes:
                //     _NOP + _NOP + _POP_TOP + _NOP + _NOP
                while (op_without_pop[opcode]) {
                    _PyUOpInstruction *last = &buffer[pc - 1];
                    while (op_skip[last->opcode]) {
                        last--;
                    }
                    if (op_without_push[last->opcode] && op_without_pop[opcode]) {
                        last->opcode = op_without_push[last->opcode];
                        opcode = buffer[pc].opcode = op_without_pop[opcode];
                        if (op_without_pop[last->opcode]) {
                            opcode = last->opcode;
                            pc = (int)(last - buffer);
                        }
                    }
                    else {
                        break;
                    }
                }
                /* _PUSH_FRAME doesn't escape or error, but it
                 * does need the IP for the return address */
                bool needs_ip = (opcode == _PUSH_FRAME || opcode == _YIELD_VALUE || opcode == _DYNAMIC_EXIT || opcode == _EXIT_TRACE);
                if (_PyUop_Flags[opcode] & HAS_ESCAPES_FLAG) {
                    needs_ip = true;
                    may_have_escaped = true;
                }
                if (needs_ip && last_set_ip >= 0) {
                    assert(buffer[last_set_ip].opcode == _NOP);
                    buffer[last_set_ip].opcode = _SET_IP;
                    last_set_ip = -1;
                }
                if (opcode == _EXIT_TRACE) {
                    return pc + 1;
                }
                break;
            }
            case _JUMP_TO_TOP:
            case _DYNAMIC_EXIT:
            case _DEOPT:
                return pc + 1;
        }
    }
    Py_UNREACHABLE();
}

//  0 - failure, no error raised, just fall back to Tier 1
// -1 - failure, and raise error
//  > 0 - length of optimized trace
int
_Py_uop_analyze_and_optimize(
    _PyThreadStateImpl *tstate,
    _PyUOpInstruction *buffer,
    int length,
    int curr_stacklen,
    _PyUOpInstruction *output,
    _PyBloomFilter *dependencies
)
{
    OPT_STAT_INC(optimizer_attempts);

    length = optimize_uops(
        tstate, buffer, length, curr_stacklen, output, dependencies);

    if (length == 0) {
        return length;
    }

    assert(length > 0);

    length = vo_rewrite(tstate, &tstate->jit_tracer_state->opt_context,
                        output, length);
    length = remove_unneeded_uops(output, length);
    assert(length > 0);

    OPT_STAT_INC(optimizer_successes);
    return length;
}

#endif /* _Py_TIER2 */
