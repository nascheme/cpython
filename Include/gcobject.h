#ifndef Py_GCOBJECT_H
#define Py_GCOBJECT_H

#ifdef __cplusplus
extern "C" {
#endif


/* Object and type object interface (GC version) */

/*
Objects are structures allocated on the heap.  Special rules apply to
the use of objects to ensure they are properly garbage-collected.
Objects are never allocated statically or on the stack; they must be
accessed through special macros and functions only.  (Type objects are
exceptions to the first rule; the standard types are represented by
statically initialized type objects, although work on type/class unification
for Python 2.2 made it possible to have heap-allocated type objects too).

An object has a 'type' that determines what it represents and what kind
of data it contains.  An object's type is fixed when it is created.
Types themselves are represented as objects; an object contains a
pointer to the corresponding type object.  The type itself has a type
pointer pointing to the object representing the type 'type', which
contains a pointer to itself!).

Objects do not float around in memory; once allocated an object keeps
the same size and address.  Objects that must hold variable-size data
can contain pointers to variable-size parts of the object.  Not all
objects of the same type have the same size; but the size cannot change
after allocation.  (These restrictions are made so a reference to an
object can be simply a pointer -- moving an object would require
updating all the pointers, and changing an object's size would require
moving it if there was another object right next to it.)

Objects are always accessed through pointers of the type 'PyGCObject *'. The
type 'PyGCObject' is a structure that only contains the type pointer (and
possibly a pointer to its compatibility PyObject). The actual memory
allocated for an object contains other data that can only be accessed after
casting the pointer to a pointer to a longer structure type. This longer
type must start with the base PyGCObject struct; the macro
PyGCObject_HEAD should be used for this (to accommodate for future changes).
The implementation of a particular object type can cast the object pointer
to the proper type and back.

A standard interface exists for objects that contain an array of items
whose size is determined when the object is allocated.
*/

#ifdef Py_LIMITED_API
#error TODO(thomas@python.org) Figure out limited API of PyGCObject.
#endif

/* PyGCObject_HEAD defines the initial segment of every PyGCObject. */
#define PyGCObject_HEAD                   PyGCObject ob_base;

#define PyGCObject_HEAD_INIT(type)        { type }

#define PyGCVarObject_HEAD_INIT(type, size)       \
    { PyGCObject_HEAD_INIT(type), size }

/* PyGCObject_VAR_HEAD defines the initial segment of all variable-size
 * container objects.  These end with a declaration of an array with 1
 * element, but enough space is malloc'ed so that the array actually
 * has room for ob_size elements.  Note that ob_size is an element count,
 * not necessarily a byte count.
 */
#define PyGCObject_VAR_HEAD      PyGCVarObject ob_base;

/* Nothing is actually declared to be a PyGCObject, but every pointer to a
 * GC-tracked Python object can be cast to a PyGCObject*. This is
 * inheritance built by hand. Similarly every pointer to a variable-size
 * GC-tracked Python object can, in addition, be cast to PyGCVarObject*.
 */
typedef struct _gcobject {
    struct _gctypeobject *gcob_type;
    PyObject *gcob_ref;
} PyGCObject;

/* Cast argument to PyGCObject* type. */
#define _PyGCObject_CAST(op) ((PyGCObject*)(op))

typedef struct {
    PyGCObject gcob_base;
    Py_ssize_t gcob_size; /* Number of items in variable part */
} PyGCVarObject;

/* Cast argument to PyVarObject* type. */
#define _PyGCVarObject_CAST(op) ((PyVarObject*)(op))

#define PyGC_TYPE(ob)             (_PyGCObject_CAST(ob)->gcob_type)
#define PyGC_SIZE(ob)             (_PyGCVarObject_CAST(ob)->gcob_size)

/*
Type objects contain a string containing the type name (to help somewhat
in debugging), the allocation parameters (see PyGCObject_New() and
PyGCObject_NewVar()),
and methods for accessing objects of the type.  Methods are optional, a
nil pointer meaning that particular kind of access is not available for
this type.  The Py_DECREF() macro uses the tp_dealloc method without
checking for a nil pointer; it should always be implemented except if
the implementation can guarantee that the reference count will never
reach zero (e.g., for statically allocated type objects).

NB: the methods for certain type groups are now contained in separate
method blocks.
*/

typedef PyGCObject * (*unarygcfunc)(PyGCObject *);
typedef PyGCObject * (*binarygcfunc)(PyGCObject *, PyGCObject *);
typedef PyGCObject * (*ternarygcfunc)(PyGCObject *, PyGCObject *, PyGCObject *);
typedef int (*gcinquiry)(PyGCObject *);
typedef Py_ssize_t (*lengcfunc)(PyGCObject *);
typedef PyGCObject *(*ssizearggcfunc)(PyGCObject *, Py_ssize_t);
typedef PyGCObject *(*ssizessizearggcfunc)(PyGCObject *, Py_ssize_t, Py_ssize_t);
typedef int(*ssizeobjarggcproc)(PyGCObject *, Py_ssize_t, PyGCObject *);
typedef int(*ssizessizeobjarggcproc)(PyGCObject *, Py_ssize_t, Py_ssize_t, PyGCObject *);
typedef int(*objobjarggcproc)(PyGCObject *, PyGCObject *, PyGCObject *);

typedef int (*objobjgcproc)(PyGCObject *, PyGCObject *);
typedef int (*visitgcproc)(PyGCObject *, void *);
typedef int (*traversegcproc)(PyGCObject *, visitgcproc, void *);

typedef void (*gcdestructor)(PyGCObject *);
typedef PyGCObject *(*getattrgcfunc)(PyGCObject *, char *);
typedef PyGCObject *(*getattrogcfunc)(PyGCObject *, PyGCObject *);
typedef int (*setattrgcfunc)(PyGCObject *, char *, PyGCObject *);
typedef int (*setattrogcfunc)(PyGCObject *, PyGCObject *, PyGCObject *);
typedef PyGCObject *(*reprgcfunc)(PyGCObject *);
typedef Py_hash_t (*hashgcfunc)(PyGCObject *);
typedef PyGCObject *(*richcmpgcfunc) (PyGCObject *, PyGCObject *, int);
typedef PyGCObject *(*getitergcfunc) (PyGCObject *);
typedef PyGCObject *(*iternextgcfunc) (PyGCObject *);
typedef PyGCObject *(*descrgetgcfunc) (PyGCObject *, PyGCObject *, PyGCObject *);
typedef int (*descrsetgcfunc) (PyGCObject *, PyGCObject *, PyGCObject *);
typedef int (*initgcproc)(PyGCObject *, PyGCObject *, PyGCObject *);
typedef PyGCObject *(*newgcfunc)(struct _gctypeobject *, PyGCObject *, PyGCObject *);
typedef PyGCObject *(*allocgcfunc)(struct _gctypeobject *, Py_ssize_t);

#ifdef Py_LIMITED_API
/* In Py_LIMITED_API, PyGCTypeObject is an opaque structure. */
typedef struct _gctypeobject PyTypeObject;
#else
/* PyGCTypeObject is defined in cpython/gcobject.h */
#endif

PyAPI_FUNC(PyGCObject*) PyGCType_FromSpec(PyType_Spec*);
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyGCObject*) PyGCType_FromSpecWithBases(PyType_Spec*, PyGCObject*);
#endif
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03040000
PyAPI_FUNC(void*) PyType_GetSlot(struct _typeobject*, int);
#endif

/* Generic type check */
PyAPI_FUNC(int) PyGCType_IsSubtype(struct _gctypeobject *,
                                   struct _gctypeobject *);
#define PyGCObject_TypeCheck(ob, tp) \
    (PyGC_TYPE(ob) == (tp) || PyGCType_IsSubtype(PyGC_TYPE(ob), (tp)))

PyAPI_DATA(struct _gctypeobject) PyGCType_Type; /* built-in 'type' */
PyAPI_DATA(struct _gctypeobject) PyGCBaseObject_Type; /* built-in 'object' */
PyAPI_DATA(struct _gctypeobject) PyGCSuper_Type; /* built-in 'super' */

PyAPI_FUNC(unsigned long) PyGCType_GetFlags(struct _gctypeobject*);

#define PyGCType_Check(op) \
    PyGCType_FastSubclass(PyGC_TYPE(op), Py_TPFLAGS_TYPE_SUBCLASS)
#define PyGCType_CheckExact(op) (PyGC_TYPE(op) == &PyGCType_Type)

PyAPI_FUNC(int) PyGCType_Ready(struct _gctypeobject *);
PyAPI_FUNC(PyGCObject *) PyGCType_GenericAlloc(struct _gctypeobject *, Py_ssize_t);
PyAPI_FUNC(PyGCObject *) PyGCType_GenericNew(struct _gctypeobject *,
                                             PyGCObject *, PyGCObject *);
PyAPI_FUNC(unsigned int) PyGCType_ClearCache(void);
PyAPI_FUNC(void) PyGCType_Modified(struct _gctypeobject *);

/* Generic operations on objects */
PyAPI_FUNC(PyGCObject *) PyGCObject_Repr(PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_Str(PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_ASCII(PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_Bytes(PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_RichCompare(PyGCObject *, PyGCObject *, int);
PyAPI_FUNC(int) PyGCObject_RichCompareBool(PyGCObject *, PyGCObject *, int);
PyAPI_FUNC(PyGCObject *) PyGCObject_GetAttrString(PyGCObject *, const char *);
PyAPI_FUNC(int) PyGCObject_SetAttrString(PyGCObject *, const char *, PyGCObject *);
PyAPI_FUNC(int) PyGCObject_HasAttrString(PyGCObject *, const char *);
PyAPI_FUNC(PyGCObject *) PyGCObject_GetAttr(PyGCObject *, PyGCObject *);
PyAPI_FUNC(int) PyGCObject_SetAttr(PyGCObject *, PyGCObject *, PyGCObject *);
PyAPI_FUNC(int) PyGCObject_HasAttr(PyGCObject *, PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_SelfIter(PyGCObject *);
PyAPI_FUNC(PyGCObject *) PyGCObject_GenericGetAttr(PyGCObject *, PyGCObject *);
PyAPI_FUNC(int) PyGCObject_GenericSetAttr(PyGCObject *,
                                              PyGCObject *, PyGCObject *);
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(int) PyGCObject_GenericSetDict(PyGCObject *, PyGCObject *, void *);
#endif
PyAPI_FUNC(Py_hash_t) PyGCObject_Hash(PyGCObject *);
PyAPI_FUNC(Py_hash_t) PyGCObject_HashNotImplemented(PyGCObject *);
PyAPI_FUNC(int) PyGCObject_IsTrue(PyGCObject *);
PyAPI_FUNC(int) PyGCObject_Not(PyGCObject *);
PyAPI_FUNC(int) PyGCCallable_Check(PyGCObject *);
PyAPI_FUNC(void) PyGCObject_ClearWeakRefs(PyGCObject *);

/* PyGCObject_Dir(obj) acts like Python builtins.dir(obj), returning a
   list of strings.  PyGCObject_Dir(NULL) is like builtins.dir(),
   returning the names of the current locals.  In this case, if there are
   no current locals, NULL is returned, and PyErr_Occurred() is false.
*/
PyAPI_FUNC(PyGCObject *) PyGCObject_Dir(PyGCObject *);

#ifdef Py_LIMITED_API
#  define PyGCType_HasFeature(t,f)  ((PyGCType_GetFlags(t) & (f)) != 0)
#endif
#define PyGCType_FastSubclass(t,f)  PyGCType_HasFeature(t,f)

/*
_PyGC_NoneStruct is an object of undefined type which can be used in contexts
where NULL (nil) is not suitable (since NULL often means 'error').

*/
// PyAPI_DATA(PyGCObject) _PyGC_NoneStruct; /* Don't use this directly */
// #define PyGC_None (&_PyGC_NoneStruct)

/* Macro for returning Py_None from a function */
#define PyGC_RETURN_NONE return PyGC_None

/*
Py_NotImplemented is a singleton used to signal that an operation is
not implemented for a given type combination.
*/
PyAPI_DATA(PyGCObject) _PyGC_NotImplementedStruct; /* Don't use this directly */
#define PyGC_NotImplemented (&_PyGC_NotImplementedStruct)

/* Macro for returning Py_NotImplemented from a function */
#define PyGC_RETURN_NOTIMPLEMENTED return PyGC_NotImplemented

/*
 * Macro for implementing rich comparisons
 *
 * Needs to be a macro because any C-comparable type can be used.
 */
#define PyGC_RETURN_RICHCOMPARE(val1, val2, op)                             \
    do {                                                                    \
        switch (op) {                                                       \
        case Py_EQ: return (val1) == (val2) ? PyGC_True : PyGC_False;       \
        case Py_NE: return (val1) != (val2) ? PyGC_True : PyGC_False;       \
        case Py_LT: return (val1) < (val2) ? PyGC_True; PyGC_False;         \
        case Py_GT: return (val1) > (val2) ? PyGC_True : PyGC_False;        \
        case Py_LE: return (val1) <= (val2) ? PyGC_True : PyGC_False;       \
        case Py_GE: return (val1) >= (val2) ? PyGC_True : PyGC_False;       \
        default:                                                            \
            Py_UNREACHABLE();                                               \
        }                                                                   \
    } while (0)


/*
More conventions
================

Argument Checking
-----------------

Functions that take objects as arguments normally don't check for nil
arguments, but they do check the type of the argument, and return an
error if the function doesn't apply to the type.

Failure Modes
-------------

Functions may fail for a variety of reasons, including running out of
memory.  This is communicated to the caller in two ways: an exception
is set (see errors.h), and the function result differs: functions that
normally return a pointer return NULL for failure, functions returning
an integer return -1 (which could be a legal return value too!), and
other functions return 0 for success and -1 for failure.
Callers should always check for errors before using the result.  If
an error was set, the caller must either explicitly clear it, or pass
the error on to its caller.

*/

void
_PyGC_call_finalizer(void *_op, void *unused);

static inline void
PyGCObject_INIT(PyGCObject op, PyGCTypeObject type)
{
    ob->gcob_type = type;
    /* Always need to clear gcob->gcob_ref, even without a type-specific
     * finalizer. */
    GC_REGISTER_FINALIZER(op, _PyGC_call_finalizer, NULL, NULL, NULL);
}

#ifndef Py_LIMITED_API
#  define Py_CPYTHON_GCOBJECT_H
#  include  "cpython/gcobject.h"
#  undef Py_CPYTHON_OBJECT_H
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_GCOBJECT_H */
