/* ixed integer object, stored tagged pointer */

//#include "Python.h"

#ifdef WITH_FIXEDINT

#include "taggedptr.h"

#if 0
typedef struct _fixedintobject PyFixedIntObject;

typedef struct {
    PyObject_HEAD
} PyFixedIntObject;
#endif

// box v if needed, return new ref
static PyObject *
obj_as_long(PyObject *v)
{
    PyObject *a;
    if (IS_TAGGED(v)) {
        ssize_t ival = UNTAG_IT(v);
        a = PyLong_FromLongLong(ival);
    }
    else {
        a = v;
        Py_INCREF(v);
    }
    return a;
}


PyObject *
_PyFixedInt_New(PyObject *self, PyObject *args)
{
    Py_ssize_t x = 0;

    if (!PyArg_ParseTuple(args, "l", &x))
        return NULL;
    if (!CAN_TAG(x)) {
        PyErr_SetString(PyExc_ValueError,
                        "value will not fit into fixed integer");
        return NULL;
    }
    return TAG_IT(x);
}

static PyObject *
fixedint_repr(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    PyObject *result = PyObject_Repr(w);
    Py_XDECREF(w);
    return result;
}

static long
fixedint_as_long_and_overflow(PyObject *v, int *overflow)
{
    PyObject *w = obj_as_long(v);
    long result = PyLong_AsLongAndOverflow(w, overflow);
    Py_XDECREF(w);
    return result;
}

static Py_ssize_t
fixedint_as_ssize_t(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    Py_ssize_t result = PyLong_AsSsize_t(w);
    Py_XDECREF(w);
    return result;
}

static size_t
fixedint_as_size_t(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    size_t result = PyLong_AsSize_t(w);
    Py_XDECREF(w);
    return result;
}

static unsigned long
fixedint_as_unsignedlong(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    unsigned long result = PyLong_AsUnsignedLongLong(w);
    Py_XDECREF(w);
    return result;
}

static unsigned long
fixedint_as_unsignedlongmask(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    unsigned long result = PyLong_AsUnsignedLongMask(w);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_sign(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    unsigned long result = _PyLong_Sign(w);
    Py_XDECREF(w);
    return result;
}

static size_t
fixedint_numbits(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    size_t result = _PyLong_Sign(w);
    Py_XDECREF(w);
    return result;
}

static void *
fixedint_as_voidptr(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    void *result = PyLong_AsVoidPtr(w);
    Py_XDECREF(w);
    return result;
}

static long long
fixedint_as_longlong(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    long long result = PyLong_AsLongLong(w);
    Py_XDECREF(w);
    return result;
}

static unsigned long long
fixedint_as_unsignedlonglong(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    unsigned long long result = PyLong_AsUnsignedLongLong(w);
    Py_XDECREF(w);
    return result;
}

static unsigned long long
fixedint_as_unsignedlonglongmask(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    unsigned long long result = PyLong_AsUnsignedLongLongMask(w);
    Py_XDECREF(w);
    return result;
}

static long long
fixedint_as_longlong_and_overflow(PyObject *v, int *overflow)
{
    PyObject *w = obj_as_long(v);
    long long result = PyLong_AsLongLongAndOverflow(w, overflow);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_unsignedshort_converter(PyObject *v, void *ptr)
{
    PyObject *w = obj_as_long(v);
    int result = _PyLong_UnsignedShort_Converter(w, ptr);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_unsignedint_converter(PyObject *v, void *ptr)
{
    PyObject *w = obj_as_long(v);
    int result = _PyLong_UnsignedInt_Converter(w, ptr);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_unsignedlong_converter(PyObject *v, void *ptr)
{
    PyObject *w = obj_as_long(v);
    int result = _PyLong_UnsignedLong_Converter(w, ptr);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_unsignedlonglong_converter(PyObject *v, void *ptr)
{
    PyObject *w = obj_as_long(v);
    int result = _PyLong_UnsignedLongLong_Converter(w, ptr);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_size_t_converter(PyObject *v, void *ptr)
{
    PyObject *w = obj_as_long(v);
    int result = _PyLong_Size_t_Converter(w, ptr);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_as_bytearray(PyObject* v,
                      unsigned char* bytes, size_t n,
                      int little_endian, int is_signed)
{
    PyObject *w = obj_as_long(v);
    size_t result = _PyLong_AsByteArray((PyLongObject *)w, bytes, n,
                                        little_endian, is_signed);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_compare(PyObject *a, PyObject *b)
{
    ssize_t i = (ssize_t)a;
    ssize_t j = (ssize_t)b;
    return (i < j) ? -1 : (i > j) ? 1 : 0;
}

static PyObject *
fixedint_richcompare(PyObject *v, PyObject *w, int op)
{
    int result;
    if (IS_TAGGED(v) && IS_TAGGED(w)) {
        result = fixedint_compare(v, w);
        Py_RETURN_RICHCOMPARE(result, 0, op);
    }
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_richcompare(a, b, op);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

static PyObject *
fixedint_add_slow(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyNumber_Add(a, b);
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

/* used by BINARY_ADD */
PyObject *
_PyFixedInt_Add(PyObject *v, PyObject *w)
{
    long a, b, x;
    a = UNTAG_IT(v);
    b = UNTAG_IT(w);
    /* casts in the line below avoid undefined behaviour on overflow */
    x = (long)((unsigned long)a + b);
    if (((x^a) >= 0 || (x^b) >= 0) && CAN_TAG(x)) {
        return TAG_IT(x);
    }
    return fixedint_add_slow(v, w);
}

static PyObject *
fixedint_add(PyObject *v, PyObject *w)
{
    if (IS_TAGGED(v) && IS_TAGGED(w)) {
        return _PyFixedInt_Add(v, w);
    }
    return fixedint_add_slow(v, w);
}

static PyObject *
fixedint_sub(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_subtract(a, b);
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

static PyObject *
fixedint_mul(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_remainder(a, b);
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

static PyObject *
fixedint_mod(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_remainder(a, b);
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

static PyObject *
fixedint_divmod(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_divmod(a, b);
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

static PyObject *
fixedint_pow(PyObject *v, PyObject *w, PyObject *z)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *c = obj_as_long(z);
    PyObject *rv = PyLong_Type.tp_as_number->nb_power(a, b, c);
    Py_DECREF(a);
    Py_DECREF(b);
    Py_DECREF(c);
    return rv;
}

static PyObject *long_bitwise(PyLongObject *a, char op, PyLongObject *b);

static PyObject *
fixedint_bitwise(PyObject *v, char op, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = long_bitwise((PyLongObject *)a, op, (PyLongObject *)b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

static double
fixedint_frexp(PyObject *v, Py_ssize_t *e)
{
    PyObject *w = obj_as_long(v);
    double rv = _PyLong_Frexp((PyLongObject *)v, e);
    Py_XDECREF(w);
    return rv;
}

static double
fixedint_as_double(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    double rv = PyLong_AsDouble(v);
    Py_XDECREF(w);
    return rv;
}

static PyObject *
fixedint_div(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_floor_divide(a, b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

static PyObject *
fixedint_true_divide(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_true_divide(a, b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

#if 0
static PyObject *
fixedint_index(PyObject *v)
{
    return obj_as_long(v);
}
#endif

static Py_hash_t
fixedint_hash(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    Py_hash_t rv = PyObject_Hash(w);
    Py_XDECREF(w);
    return rv;
}

static PyObject *
fixedint_invert(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    PyObject *rv = PyLong_Type.tp_as_number->nb_invert(w);
    Py_XDECREF(w);
    return rv;
}

static PyObject *
fixedint_neg(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    PyObject *rv = PyLong_Type.tp_as_number->nb_negative(w);
    Py_XDECREF(w);
    return rv;
}

static PyObject *
fixedint_abs(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    PyObject *rv = PyLong_Type.tp_as_number->nb_absolute(w);
    Py_XDECREF(w);
    return rv;
}

static int
fixedint_bool(PyObject *v)
{
    return UNTAG_IT(v) != 0;

}

static PyObject *
fixedint_rshift(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_rshift(a, b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

static PyObject *
fixedint_lshift(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = PyLong_Type.tp_as_number->nb_lshift(a, b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

#endif /* WITH_FIXEDINT */
