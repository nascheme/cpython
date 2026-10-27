/* Copied from grail-compat src/grail/_c/ by tools/sync_to_cpython_grail.py; do not edit here. */
/* Portability layer for Grail's native modules.
 *
 * The same sources build two ways:
 *   - in grail-compat, as an ordinary extension against CPython 3.14+;
 *   - in cpython-grail, as a core module (Py_BUILD_CORE), where the
 *     internal fast paths below are used instead.
 */
#ifndef GRAIL_COMPAT_H
#define GRAIL_COMPAT_H

#include "Python.h"

#if PY_VERSION_HEX < 0x030E0000
#  error "Grail's native modules require CPython 3.14 or newer"
#endif

#ifdef Py_BUILD_CORE
#  include "internal/pycore_object.h"                // _Py_DECREF_TYPE()
#  include "internal/pycore_pyatomic_ft_wrappers.h"  // FT_ATOMIC_*
#  include "internal/pycore_tuple.h"                 // _PyTuple_Recycle()
#endif

#include <stdint.h>


/* Release the reference a heap-type instance holds on its type.  On
   free-threaded core builds tp_alloc takes a per-thread type reference,
   which _Py_DECREF_TYPE pairs with. */
#ifdef Py_BUILD_CORE
#  define GRAIL_DECREF_TYPE(tp) _Py_DECREF_TYPE(tp)
#else
#  define GRAIL_DECREF_TYPE(tp) Py_DECREF(tp)
#endif


/* Relaxed atomic Py_ssize_t access, for fields read and written without a
   lock on free-threaded builds (e.g. a cached hash). */
#if defined(Py_BUILD_CORE)
#  define GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(v) FT_ATOMIC_LOAD_SSIZE_RELAXED(v)
#  define GRAIL_ATOMIC_STORE_SSIZE_RELAXED(v, x) FT_ATOMIC_STORE_SSIZE_RELAXED(v, x)
#elif defined(Py_GIL_DISABLED)
   /* cpython/pyatomic.h is part of Python.h on free-threaded builds. */
#  define GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(v) _Py_atomic_load_ssize_relaxed(&(v))
#  define GRAIL_ATOMIC_STORE_SSIZE_RELAXED(v, x) _Py_atomic_store_ssize_relaxed(&(v), (x))
#else
#  define GRAIL_ATOMIC_LOAD_SSIZE_RELAXED(v) (v)
#  define GRAIL_ATOMIC_STORE_SSIZE_RELAXED(v, x) ((v) = (x))
#endif


/* Constants of tuple's xxHash-based hash (Objects/tupleobject.c), so that
   hash(PMap) equals hash(frozendict) and hash(PSet) equals hash(frozenset). */
#if SIZEOF_PY_UHASH_T > 4
#  define GRAIL_HASH_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#  define GRAIL_HASH_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#  define GRAIL_HASH_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#  define GRAIL_HASH_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#  define GRAIL_HASH_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#  define GRAIL_HASH_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#  define GRAIL_HASH_XXPRIME_5 ((Py_uhash_t)374761393UL)
#  define GRAIL_HASH_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif


/* dict or frozendict (3.15+), exactly. */
#ifndef PyAnyDict_CheckExact
#  define PyAnyDict_CheckExact(op) PyDict_CheckExact(op)
#endif


/* Population count of a 32-bit word. */
#if defined(__GNUC__) || defined(__clang__)
#  define grail_popcount32(x) __builtin_popcount((uint32_t)(x))
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#  include <intrin.h>
#  define grail_popcount32(x) ((int)__popcnt((unsigned int)(x)))
#else
static inline int
grail_popcount32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555U);
    x = (x & 0x33333333U) + ((x >> 2) & 0x33333333U);
    return (int)((((x + (x >> 4)) & 0x0F0F0F0FU) * 0x01010101U) >> 24);
}
#endif


/* (a, b), stealing both references; NULL (and both released) on error. */
/* Before reusing a tuple nothing else references: reset its hash cache
   and re-track it with the garbage collector. */
static inline void
grail_tuple_recycle(PyObject *op)
{
#ifdef Py_BUILD_CORE
    _PyTuple_Recycle(op);
#else
    ((PyTupleObject *)op)->ob_hash = -1;
    if (!PyObject_GC_IsTracked(op)) {
        PyObject_GC_Track(op);
    }
#endif
}

static inline PyObject *
grail_tuple_from_pair_steal(PyObject *a, PyObject *b)
{
    PyObject *t = PyTuple_New(2);
    if (t == NULL) {
        Py_DECREF(a);
        Py_DECREF(b);
        return NULL;
    }
    PyTuple_SET_ITEM(t, 0, a);
    PyTuple_SET_ITEM(t, 1, b);
    return t;
}

#endif /* GRAIL_COMPAT_H */
