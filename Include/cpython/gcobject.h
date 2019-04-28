
#ifndef Py_CPYTHON_GCOBJECT_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* buffer interface */
typedef struct gcbufferinfo {
    void *buf;
    PyGCObject *obj;        /* owned reference */
    Py_ssize_t len;
    Py_ssize_t itemsize;  /* This is Py_ssize_t so it can be
                             pointed to by strides in simple case.*/
    int readonly;
    int ndim;
    char *format;
    Py_ssize_t *shape;
    Py_ssize_t *strides;
    Py_ssize_t *suboffsets;
    void *internal;
} PyGC_buffer;

typedef int (*getbuffergcproc)(PyGCObject *, Py_buffer *, int);
typedef void (*releasebuffergcproc)(PyGCObject *, Py_buffer *);

/* PyBUF constants in object.h */

/* End buffer interface */

typedef struct {
    /* Number implementations must check *both*
       arguments for proper type and implement the necessary conversions
       in the slot functions themselves. */

    binarygcfunc gcnb_add;
    binarygcfunc gcnb_subtract;
    binarygcfunc gcnb_multiply;
    binarygcfunc gcnb_remainder;
    binarygcfunc gcnb_divmod;
    ternarygcfunc gcnb_power;
    unarygcfunc gcnb_negative;
    unarygcfunc gcnb_positive;
    unarygcfunc gcnb_absolute;
    gcinquiry gcnb_bool;
    unarygcfunc gcnb_invert;
    binarygcfunc gcnb_lshift;
    binarygcfunc gcnb_rshift;
    binarygcfunc gcnb_and;
    binarygcfunc gcnb_xor;
    binarygcfunc gcnb_or;
    unarygcfunc gcnb_int;
    unarygcfunc gcnb_float;

    binarygcfunc gcnb_inplace_add;
    binarygcfunc gcnb_inplace_subtract;
    binarygcfunc gcnb_inplace_multiply;
    binarygcfunc gcnb_inplace_remainder;
    ternarygcfunc gcnb_inplace_power;
    binarygcfunc gcnb_inplace_lshift;
    binarygcfunc gcnb_inplace_rshift;
    binarygcfunc gcnb_inplace_and;
    binarygcfunc gcnb_inplace_xor;
    binarygcfunc gcnb_inplace_or;

    binarygcfunc gcnb_floor_divide;
    binarygcfunc gcnb_true_divide;
    binarygcfunc gcnb_inplace_floor_divide;
    binarygcfunc gcnb_inplace_true_divide;

    unarygcfunc gcnb_index;

    binarygcfunc gcnb_matrix_multiply;
    binarygcfunc gcnb_inplace_matrix_multiply;
} PyGCNumberMethods;

typedef struct {
    lengcfunc gcsq_length;
    binarygcfunc gcsq_concat;
    ssizearggcfunc gcsq_repeat;
    ssizearggcfunc gcsq_item;
    ssizeobjarggcproc gcsq_ass_item;
    objobjgcproc gcsq_contains;

    binarygcfunc gcsq_inplace_concat;
    ssizearggcfunc gcsq_inplace_repeat;
} PyGCSequenceMethods;

typedef struct {
    lengcfunc gcmp_length;
    binarygcfunc gcmp_subscript;
    objobjarggcproc gcmp_ass_subscript;
} PyGCMappingMethods;

typedef struct {
    unarygcfunc gcam_await;
    unarygcfunc gcam_aiter;
    unarygcfunc gcam_anext;
} PyGCAsyncMethods;

typedef struct {
     getbuffergcproc bf_getbuffer;
     releasebuffergcproc bf_releasebuffer;
} PyGCBufferProcs;

/* We can't provide a full compile-time check that limited-API
   users won't implement tp_print. However, not defining printfunc
   and making tp_print of a different function pointer type
   if Py_LIMITED_API is set should at least cause a warning
   in most cases. */
typedef int (*printgcfunc)(PyGCObject *, FILE *, int);

typedef struct _gctypeobject {
    PyGCObject_VAR_HEAD
    const char *gctp_name; /* For printing, in format "<module>.<name>" */
    Py_ssize_t gctp_basicsize, gctp_itemsize; /* For allocation */

    /* Methods to implement standard operations */

    gcdestructor gctp_finalizer;
    printgcfunc gctp_print;
    getattrgcfunc gctp_getattr;
    setattrgcfunc gctp_setattr;
    PyGCAsyncMethods *gctp_as_async;
    reprgcfunc gctp_repr;

    /* Method suites for standard classes */

    PyGCNumberMethods *gctp_as_number;
    PyGCSequenceMethods *gctp_as_sequence;
    PyGCMappingMethods *gctp_as_mapping;

    /* More standard operations (here for binary compatibility) */

    hashgcfunc gctp_hash;
    ternarygcfunc gctp_call;
    reprgcfunc gctp_str;
    getattrogcfunc gctp_getattro;
    setattrogcfunc gctp_setattro;

    /* Functions to access object as input/output buffer */
    PyGCBufferProcs *gctp_as_buffer;

    /* Flags to define presence of optional/expanded features */
    unsigned long gctp_flags;

    const char *gctp_doc; /* Documentation string */

    /* Assigned meaning in release 2.0 */
    /* call function for all accessible objects */
    traversegcproc gctp_traverse;

    /* delete references to contained objects */
    gcinquiry gctp_clear;

    /* Assigned meaning in release 2.1 */
    /* rich comparisons */
    richcmpgcfunc gctp_richcompare;

    /* weak reference enabler TODO(thomas@python.org) reimplement with
     * libgc's weakref support? */
    Py_ssize_t gctp_weaklistoffset;

    /* Iterators */
    getitergcfunc gctp_iter;
    iternextgcfunc gctp_iternext;

    /* Attribute descriptor and subclassing stuff */
    struct PyGCMethodDef *gctp_methods;
    struct PyGCMemberDef *gctp_members;
    struct PyGCGetSetDef *gctp_getset;
    struct _gctypeobject *gctp_base;
    PyGCObject *gctp_dict;
    descrgetgcfunc gctp_descr_get;
    descrsetgcfunc gctp_descr_set;
    Py_ssize_t gctp_dictoffset;
    initgcproc gctp_init;
    newgcfunc gctp_new;
    freefunc gctp_free; /* Low-level free-memory routine */
    gcinquiry gctp_is_gc; /* For PyGCObject_IS_GC */
    PyGCObject *gctp_bases;
    PyGCObject *gctp_mro; /* method resolution order */
    PyGCObject *gctp_cache;
    PyGCObject *gctp_subclasses;
    PyGCObject *gctp_weaklist;

    /* Type attribute cache version tag. Added in version 2.6 */
    unsigned int gctp_version_tag;

} PyGCTypeObject;

/* The *real* layout of a type object when allocated on the heap */
typedef struct _gcheaptypeobject {
    /* Note: there's a dependency on the order of these members
       in slotptr() in typeobject.c . */
    PyGCTypeObject gcht_type;
    PyGCAsyncMethods as_async;
    PyGCNumberMethods as_number;
    PyGCMappingMethods as_mapping;
    PyGCSequenceMethods as_sequence; /* as_sequence comes after as_mapping,
                                      so that the mapping wins when both
                                      the mapping and the sequence define
                                      a given operator (e.g. __getitem__).
                                      see add_operators() in typeobject.c . */
    PyGCBufferProcs as_buffer;
    PyGCObject *ht_name, *ht_slots, *ht_qualname;
    struct _dictkeysobject *ht_cached_keys;
    /* here are optional user slots, followed by the members. */
} PyGCHeapTypeObject;

/* access macro to the members which are floating "behind" the object */
#define PyGCHeapType_GET_MEMBERS(etype) \
    ((PyGCMemberDef *)(((char *)etype) + PyGC_TYPE(etype)->gctp_basicsize))

PyAPI_FUNC(const char *) _PyGCType_Name(PyGCTypeObject *);
PyAPI_FUNC(PyGCObject *) _PyGCType_Lookup(PyGCTypeObject *, PyGCObject *);
PyAPI_FUNC(PyGCObject *) _PyGCType_LookupId(PyGCTypeObject *, _Py_Identifier *);
PyAPI_FUNC(PyGCObject *) _PyGCObject_LookupSpecial(PyGCObject *, _Py_Identifier *);
PyAPI_FUNC(PyGCTypeObject *) _PyGCType_CalculateMetaclass(PyGCTypeObject *, PyGCObject *);
PyAPI_FUNC(PyGCObject *) _PyGCType_GetDocFromInternalDoc(const char *, const char *);
PyAPI_FUNC(PyGCObject *) _PyGCType_GetTextSignatureFromInternalDoc(const char *, const char *);

PyAPI_FUNC(int) PyGCObject_Print(PyGCObject *, FILE *, int);
PyAPI_FUNC(void) _PyGCObject_Dump(PyGCObject *);

PyAPI_FUNC(int) _PyGCObject_IsAbstract(PyGCObject *);
PyAPI_FUNC(PyGCObject *) _PyGCObject_GetAttrId(PyGCObject *, struct _Py_Identifier *);
PyAPI_FUNC(int) _PyGCObject_SetAttrId(PyGCObject *, struct _Py_Identifier *, PyGCObject *);
PyAPI_FUNC(int) _PyGCObject_HasAttrId(PyGCObject *, struct _Py_Identifier *);
/* Replacements of PyGCObject_GetAttr() and _PyGCObject_GetAttrId() which
   don't raise AttributeError.

   Return 1 and set *result != NULL if an attribute is found.
   Return 0 and set *result == NULL if an attribute is not found;
   an AttributeError is silenced.
   Return -1 and set *result == NULL if an error other than AttributeError
   is raised.
*/
PyAPI_FUNC(int) _PyGCObject_LookupAttr(PyGCObject *, PyGCObject *, PyGCObject **);
PyAPI_FUNC(int) _PyGCObject_LookupAttrId(PyGCObject *, struct _Py_Identifier *, PyGCObject **);
PyAPI_FUNC(PyGCObject **) _PyGCObject_GetDictPtr(PyGCObject *);
PyAPI_FUNC(PyGCObject *) _PyGCObject_NextNotImplemented(PyGCObject *);

/* Same as PyGCObject_Generic{Get,Set}Attr, but passing the attributes
   dict as the last parameter. */
PyAPI_FUNC(PyGCObject *)
_PyGCObject_GenericGetAttrWithDict(PyGCObject *, PyGCObject *, PyGCObject *, int);
PyAPI_FUNC(int)
_PyGCObject_GenericSetAttrWithDict(PyGCObject *, PyGCObject *,
                                 PyGCObject *, PyGCObject *);

#define PyGCType_HasFeature(t,f)  (((t)->gctp_flags & (f)) != 0)

PyAPI_DATA(PyGCTypeObject) _PyGCNone_Type;
PyAPI_DATA(PyGCTypeObject) _PyGCNotImplemented_Type;

/* Define a set of assertion macros:
   _PyGCObject_ASSERT_FROM(), _PyGCObject_ASSERT_WITH_MSG() and
   _PyGCObject_ASSERT().

   These work like the regular C assert(), in that they will abort the
   process with a message on stderr if the given condition fails to hold,
   but compile away to nothing if NDEBUG is defined.

   However, before aborting, Python will also try to call _PyGCObject_Dump() on
   the given object.  This may be of use when investigating bugs in which a
   particular object is corrupt (e.g. buggy a gctp_visit method in an extension
   module breaking the garbage collector), to help locate the broken objects.

   The WITH_MSG variant allows you to supply an additional message that Python
   will attempt to print to stderr, after the object dump. */
#ifdef NDEBUG
   /* No debugging: compile away the assertions: */
#  define _PyGCObject_ASSERT_FROM(obj, expr, msg, filename, lineno, func) \
    ((void)0)
#else
   /* With debugging: generate checks: */
#  define _PyGCObject_ASSERT_FROM(obj, expr, msg, filename, lineno, func) \
    ((expr) \
      ? (void)(0) \
      : _PyGCObject_AssertFailed((obj), Py_STRINGIFY(expr), \
                                 (msg), (filename), (lineno), (func)))
#endif

#define _PyGCObject_ASSERT_WITH_MSG(obj, expr, msg) \
    _PyGCObject_ASSERT_FROM(obj, expr, msg, __FILE__, __LINE__, __func__)
#define _PyGCObject_ASSERT(obj, expr) \
    _PyGCObject_ASSERT_WITH_MSG(obj, expr, NULL)

#define _PyGCObject_ASSERT_FAILED_MSG(obj, msg) \
    _PyGCObject_AssertFailed((obj), NULL, (msg), __FILE__, __LINE__, __func__)

/* Declare and define _PyGCObject_AssertFailed() even when NDEBUG is defined,
   to avoid causing compiler/linker errors when building extensions without
   NDEBUG against a Python built with NDEBUG defined.

   msg, expr and function can be NULL. */
PyAPI_FUNC(void) _PyGCObject_AssertFailed(
    PyGCObject *obj,
    const char *expr,
    const char *msg,
    const char *file,
    int line,
    const char *function);

/* Check if an object is consistent. For example, ensure that the reference
   counter is greater than or equal to 1, and ensure that ob_type is not NULL.

   Call _PyGCObject_AssertFailed() if the object is inconsistent.

   If check_content is zero, only check header fields: reduce the overhead.

   The function always return 1. The return value is just here to be able to
   write:

   assert(_PyGCObject_CheckConsistency(obj, 1)); */
PyAPI_FUNC(int) _PyGCObject_CheckConsistency(
    PyGCObject *op,
    int check_content);

#ifdef __cplusplus
}
#endif
