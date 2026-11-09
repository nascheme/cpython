
/* Map C struct members to Python object attributes */

#include "Python.h"
#include "pycore_abstract.h"      // _PyNumber_Index()
#include "pycore_descrobject.h"   // _PyMember_GetOffset()
#include "pycore_long.h"          // _PyLong_IsNegative()
#include "pycore_object.h"        // _Py_TryIncrefCompare(), FT_ATOMIC_*()
#include "pycore_critical_section.h"

#include <string.h>               // memcpy()

// Scalar members are read without a lock.  In the free-threaded build they
// are stored with release and loaded with acquire ordering, so a reader that
// observes a new value also observes writes made before it was stored.
// Members are accessed by size so that each C type maps to an atomic of the
// same width.
static inline void
member_load_acquire(void *dst, const void *addr, size_t size)
{
#ifdef Py_GIL_DISABLED
    switch (size) {
    case 1: {
        uint8_t v = _Py_atomic_load_uint8_acquire((const uint8_t *)addr);
        memcpy(dst, &v, 1);
        return;
    }
    case 2: {
        uint16_t v = _Py_atomic_load_uint16_acquire((const uint16_t *)addr);
        memcpy(dst, &v, 2);
        return;
    }
    case 4: {
        uint32_t v = _Py_atomic_load_uint32_acquire((const uint32_t *)addr);
        memcpy(dst, &v, 4);
        return;
    }
    case 8: {
        uint64_t v = _Py_atomic_load_uint64_acquire((const uint64_t *)addr);
        memcpy(dst, &v, 8);
        return;
    }
    }
    Py_UNREACHABLE();
#else
    memcpy(dst, addr, size);
#endif
}

static inline void
member_store_release(void *addr, const void *src, size_t size)
{
#ifdef Py_GIL_DISABLED
    switch (size) {
    case 1: {
        uint8_t v;
        memcpy(&v, src, 1);
        _Py_atomic_store_uint8_release((uint8_t *)addr, v);
        return;
    }
    case 2: {
        uint16_t v;
        memcpy(&v, src, 2);
        _Py_atomic_store_uint16_release((uint16_t *)addr, v);
        return;
    }
    case 4: {
        uint32_t v;
        memcpy(&v, src, 4);
        _Py_atomic_store_uint32_release((uint32_t *)addr, v);
        return;
    }
    case 8: {
        uint64_t v;
        memcpy(&v, src, 8);
        _Py_atomic_store_uint64_release((uint64_t *)addr, v);
        return;
    }
    }
    Py_UNREACHABLE();
#else
    memcpy(addr, src, size);
#endif
}

// Declare `type name` and load it from `addr` with acquire ordering.
#define LOAD_MEMBER(type, name, addr) \
    type name; \
    member_load_acquire(&name, (addr), sizeof(type))

// Store `value`, converted to `type`, to `addr` with release ordering.
#define STORE_MEMBER(type, addr, value) \
    do { \
        type _member_value = (type)(value); \
        member_store_release((addr), &_member_value, sizeof(type)); \
    } while (0)

static inline PyObject *
member_get_object(const char *addr, const char *obj_addr, PyMemberDef *l)
{
    PyObject *v = FT_ATOMIC_LOAD_PTR(*(PyObject **) addr);
    if (v == NULL) {
        PyErr_Format(PyExc_AttributeError,
                     "'%T' object has no attribute '%s'",
                     (PyObject *)obj_addr, l->name);
    }
    return v;
}

void *
_PyMember_GetOffset(PyObject *obj, PyMemberDef *mp)
{
    unsigned char *addr = (unsigned char *)obj + mp->offset;
    if (mp->flags & _Py_AFTER_ITEMS) {
        PyTypeObject *type = Py_TYPE(obj);
        addr += _Py_SIZE_ROUND_UP(Py_SIZE(obj) * type->tp_itemsize, SIZEOF_VOID_P);
    }
    return addr;
}

PyObject *
PyMember_GetOne(const char *obj_addr, PyMemberDef *l)
{
    PyObject *v;
    if (l->flags & Py_RELATIVE_OFFSET) {
        PyErr_SetString(
            PyExc_SystemError,
            "PyMember_GetOne used with Py_RELATIVE_OFFSET");
        return NULL;
    }

    const void *addr = _PyMember_GetOffset((PyObject *)obj_addr, l);
    switch (l->type) {
    case Py_T_BOOL: {
        LOAD_MEMBER(char, x, addr);
        v = PyBool_FromLong(x);
        break;
    }
    case Py_T_BYTE: {
        LOAD_MEMBER(char, x, addr);
        v = PyLong_FromLong(x);
        break;
    }
    case Py_T_UBYTE: {
        LOAD_MEMBER(unsigned char, x, addr);
        v = PyLong_FromUnsignedLong(x);
        break;
    }
    case Py_T_SHORT: {
        LOAD_MEMBER(short, x, addr);
        v = PyLong_FromLong(x);
        break;
    }
    case Py_T_USHORT: {
        LOAD_MEMBER(unsigned short, x, addr);
        v = PyLong_FromUnsignedLong(x);
        break;
    }
    case Py_T_INT: {
        LOAD_MEMBER(int, x, addr);
        v = PyLong_FromLong(x);
        break;
    }
    case Py_T_UINT: {
        LOAD_MEMBER(unsigned int, x, addr);
        v = PyLong_FromUnsignedLong(x);
        break;
    }
    case Py_T_LONG: {
        LOAD_MEMBER(long, x, addr);
        v = PyLong_FromLong(x);
        break;
    }
    case Py_T_ULONG: {
        LOAD_MEMBER(unsigned long, x, addr);
        v = PyLong_FromUnsignedLong(x);
        break;
    }
    case Py_T_PYSSIZET: {
        LOAD_MEMBER(Py_ssize_t, x, addr);
        v = PyLong_FromSsize_t(x);
        break;
    }
    case Py_T_FLOAT: {
        LOAD_MEMBER(float, x, addr);
        v = PyFloat_FromDouble((double)x);
        break;
    }
    case Py_T_DOUBLE: {
        LOAD_MEMBER(double, x, addr);
        v = PyFloat_FromDouble(x);
        break;
    }
    case Py_T_STRING:
        if (*(char**)addr == NULL) {
            v = Py_NewRef(Py_None);
        }
        else
            v = PyUnicode_FromString(*(char**)addr);
        break;
    case Py_T_STRING_INPLACE:
        v = PyUnicode_FromString((char*)addr);
        break;
    case Py_T_CHAR: {
        LOAD_MEMBER(char, x, addr);
        v = PyUnicode_FromStringAndSize(&x, 1);
        break;
    }
    case _Py_T_OBJECT:
        v = FT_ATOMIC_LOAD_PTR(*(PyObject **) addr);
        if (v != NULL) {
#ifdef Py_GIL_DISABLED
            if (!_Py_TryIncrefCompare((PyObject **) addr, v)) {
                Py_BEGIN_CRITICAL_SECTION((PyObject *) obj_addr);
                v = FT_ATOMIC_LOAD_PTR(*(PyObject **) addr);
                Py_XINCREF(v);
                Py_END_CRITICAL_SECTION();
            }
#else
            Py_INCREF(v);
#endif
        }
        if (v == NULL) {
            v = Py_None;
        }
        break;
    case Py_T_OBJECT_EX:
        v = member_get_object(addr, obj_addr, l);
#ifndef Py_GIL_DISABLED
        Py_XINCREF(v);
#else
        if (v != NULL) {
            if (!_Py_TryIncrefCompare((PyObject **) addr, v)) {
                Py_BEGIN_CRITICAL_SECTION((PyObject *) obj_addr);
                v = member_get_object(addr, obj_addr, l);
                Py_XINCREF(v);
                Py_END_CRITICAL_SECTION();
            }
        }
#endif
        break;
    case Py_T_LONGLONG: {
        LOAD_MEMBER(long long, x, addr);
        v = PyLong_FromLongLong(x);
        break;
    }
    case Py_T_ULONGLONG: {
        LOAD_MEMBER(unsigned long long, x, addr);
        v = PyLong_FromUnsignedLongLong(x);
        break;
    }
    case _Py_T_NONE:
        // doesn't require free-threading code path
        v = Py_NewRef(Py_None);
        break;
    default:
        PyErr_SetString(PyExc_SystemError, "bad memberdescr type");
        v = NULL;
    }
    return v;
}

#define WARN(msg)                                               \
    do {                                                        \
    if (PyErr_WarnEx(PyExc_RuntimeWarning, msg, 1) < 0)         \
        return -1;                                              \
    } while (0)

int
PyMember_SetOne(char *addr, PyMemberDef *l, PyObject *v)
{
    PyObject *oldv;
    if (l->flags & Py_RELATIVE_OFFSET) {
        PyErr_SetString(
            PyExc_SystemError,
            "PyMember_SetOne used with Py_RELATIVE_OFFSET");
        return -1;
    }

    PyObject *obj = (PyObject *)addr;
    addr = _PyMember_GetOffset(obj, l);

    if ((l->flags & Py_READONLY))
    {
        PyErr_SetString(PyExc_AttributeError, "readonly attribute");
        return -1;
    }
    if (v == NULL && l->type != Py_T_OBJECT_EX && l->type != _Py_T_OBJECT) {
        PyErr_SetString(PyExc_TypeError,
                        "can't delete numeric/char attribute");
        return -1;
    }
    switch (l->type) {
    case Py_T_BOOL:{
        if (!PyBool_Check(v)) {
            PyErr_SetString(PyExc_TypeError,
                            "attribute value type must be bool");
            return -1;
        }
        if (v == Py_True)
            STORE_MEMBER(char, addr, 1);
        else
            STORE_MEMBER(char, addr, 0);
        break;
        }
    case Py_T_BYTE:{
        long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(char, addr, long_val);
        /* XXX: For compatibility, only warn about truncations
           for now. */
        if ((long_val > CHAR_MAX) || (long_val < CHAR_MIN))
            WARN("Truncation of value to char");
        break;
        }
    case Py_T_UBYTE:{
        long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(unsigned char, addr, long_val);
        if ((long_val > UCHAR_MAX) || (long_val < 0))
            WARN("Truncation of value to unsigned char");
        break;
        }
    case Py_T_SHORT:{
        long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(short, addr, long_val);
        if ((long_val > SHRT_MAX) || (long_val < SHRT_MIN))
            WARN("Truncation of value to short");
        break;
        }
    case Py_T_USHORT:{
        long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(unsigned short, addr, long_val);
        if ((long_val > USHRT_MAX) || (long_val < 0))
            WARN("Truncation of value to unsigned short");
        break;
        }
    case Py_T_INT:{
        long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(int, addr, long_val);
        if ((long_val > INT_MAX) || (long_val < INT_MIN))
            WARN("Truncation of value to int");
        break;
        }
    case Py_T_UINT: {
        /* XXX: For compatibility, accept negative int values
           as well. */
        v = _PyNumber_Index(v);
        if (v == NULL) {
            return -1;
        }
        if (_PyLong_IsNegative((PyLongObject *)v)) {
            long long_val = PyLong_AsLong(v);
            Py_DECREF(v);
            if (long_val == -1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned int, addr, (unsigned long)long_val);
            WARN("Writing negative value into unsigned field");
        }
        else {
            unsigned long ulong_val = PyLong_AsUnsignedLong(v);
            Py_DECREF(v);
            if (ulong_val == (unsigned long)-1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned int, addr, ulong_val);
            if (ulong_val > UINT_MAX) {
                WARN("Truncation of value to unsigned int");
            }
        }
        break;
    }
    case Py_T_LONG: {
        const long long_val = PyLong_AsLong(v);
        if ((long_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(long, addr, long_val);
        break;
    }
    case Py_T_ULONG: {
        /* XXX: For compatibility, accept negative int values
           as well. */
        v = _PyNumber_Index(v);
        if (v == NULL) {
            return -1;
        }
        if (_PyLong_IsNegative((PyLongObject *)v)) {
            long long_val = PyLong_AsLong(v);
            Py_DECREF(v);
            if (long_val == -1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned long, addr, long_val);
            WARN("Writing negative value into unsigned field");
        }
        else {
            unsigned long ulong_val = PyLong_AsUnsignedLong(v);
            Py_DECREF(v);
            if (ulong_val == (unsigned long)-1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned long, addr, ulong_val);
        }
        break;
    }
    case Py_T_PYSSIZET: {
        const Py_ssize_t ssize_val = PyLong_AsSsize_t(v);
        if ((ssize_val == (Py_ssize_t)-1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(Py_ssize_t, addr, ssize_val);
        break;
    }
    case Py_T_FLOAT:{
        double double_val = PyFloat_AsDouble(v);
        if ((double_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(float, addr, double_val);
        break;
        }
    case Py_T_DOUBLE: {
        const double double_val = PyFloat_AsDouble(v);
        if ((double_val == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(double, addr, double_val);
        break;
    }
    case _Py_T_OBJECT:
    case Py_T_OBJECT_EX:
        Py_BEGIN_CRITICAL_SECTION(obj);
        oldv = *(PyObject **)addr;
        FT_ATOMIC_STORE_PTR_RELEASE(*(PyObject **)addr, Py_XNewRef(v));
        Py_END_CRITICAL_SECTION();
        if (v == NULL && oldv == NULL && l->type == Py_T_OBJECT_EX) {
            // Raise an exception when attempting to delete an already deleted
            // attribute.
            // Differently from Py_T_OBJECT_EX, _Py_T_OBJECT does not raise an
            // exception here (PyMember_GetOne will return Py_None instead of
            // NULL).
            PyErr_SetString(PyExc_AttributeError, l->name);
            return -1;
        }
        Py_XDECREF(oldv);
        break;
    case Py_T_CHAR: {
        const char *string;
        Py_ssize_t len;

        string = PyUnicode_AsUTF8AndSize(v, &len);
        if (string == NULL || len != 1) {
            PyErr_BadArgument();
            return -1;
        }
        STORE_MEMBER(char, addr, string[0]);
        break;
        }
    case Py_T_STRING:
    case Py_T_STRING_INPLACE:
        PyErr_SetString(PyExc_TypeError, "readonly attribute");
        return -1;
    case Py_T_LONGLONG:{
        long long value = PyLong_AsLongLong(v);
        if ((value == -1) && PyErr_Occurred())
            return -1;
        STORE_MEMBER(long long, addr, value);
        break;
        }
    case Py_T_ULONGLONG: {
        v = _PyNumber_Index(v);
        if (v == NULL) {
            return -1;
        }
        if (_PyLong_IsNegative((PyLongObject *)v)) {
            long long_val = PyLong_AsLong(v);
            Py_DECREF(v);
            if (long_val == -1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned long long, addr, (long long)long_val);
            WARN("Writing negative value into unsigned field");
        }
        else {
            unsigned long long ulonglong_val = PyLong_AsUnsignedLongLong(v);
            Py_DECREF(v);
            if (ulonglong_val == (unsigned long long)-1 && PyErr_Occurred()) {
                return -1;
            }
            STORE_MEMBER(unsigned long long, addr, ulonglong_val);
        }
        break;
    }
    default:
        PyErr_Format(PyExc_SystemError,
                     "bad memberdescr type for %s", l->name);
        return -1;
    }
    return 0;
}
