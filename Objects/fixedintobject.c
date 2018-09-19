/* Fixed integer object, stored tagged pointer */

#ifdef WITH_FIXEDINT

#include "taggedptr.h"

/* small integer cache */
#define NSMALLPOSFIXEDINTS 257
#define NSMALLNEGFIXEDINTS 5
static PyObject *small_fixedints[NSMALLNEGFIXEDINTS + NSMALLPOSFIXEDINTS];


static PyObject* _PyLong_FromLongLong(long long ival);

Py_ssize_t _PyFixedInt_Val(PyObject *v)
{
    assert(TAGGED_CHECK(v));
    ssize_t ival = FROM_TAGGED(v);
    return ival;
}

// box v if needed, return new ref
PyObject *
_PyFixedInt_Untag(PyObject *v)
{
    PyObject *a;
    if (_PyFixedInt_Check(v)) {
        ssize_t ival = FROM_TAGGED(v);
        if (-NSMALLNEGFIXEDINTS <= ival && ival < NSMALLPOSFIXEDINTS) {
            a = (PyObject *)small_fixedints[ival + NSMALLNEGFIXEDINTS];
            Py_INCREF(a);
        }
        else {
            a = _PyLong_FromLongLong(ival);
        }
    }
    else {
        a = v;
        Py_INCREF(v);
    }
    return a;
}

// FIXME: cleanup
#define obj_as_long _PyFixedInt_Untag


static ssize_t
fixedint_get_digit(PyLongObject *v, Py_ssize_t i)
{
    size_t ival = Py_ABS(_PyFixedInt_Val((PyObject*)v));
    return (ival >> (PyLong_SHIFT*i)) & PyLong_MASK;
}

PyObject *
_PyFixedInt_New(PyObject *self, PyObject *args)
{
    Py_ssize_t x = 0;

    if (!PyArg_ParseTuple(args, "l", &x))
        return NULL;
    if (!TAGGED_IN_RANGE(x)) {
        PyErr_SetString(PyExc_ValueError,
                        "value will not fit into fixed integer");
        return NULL;
    }
    return AS_TAGGED(x);
}

static PyObject *
fixedint_repr(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    PyObject *result = PyObject_Repr(w);
    Py_XDECREF(w);
    return result;
}

static int
fixedint_format_writer(_PyUnicodeWriter *writer,
                       PyObject *obj,
                       int base, int alternate)
{
    PyObject *w = obj_as_long(obj);
    long result = _PyLong_FormatWriter(writer, w, base, alternate);
    Py_XDECREF(w);
    return result;
}

static PyObject *
fixedint_format(PyObject *v, int base)
{
    PyObject *w = obj_as_long(v);
    PyObject *result = _PyLong_Format(w, base);
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

static const unsigned char fixedint_bit_table[32] = {
    0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5
};

static int
bits_in_ulong(unsigned long long d)
{
    int d_bits = 0;
    while (d >= 32) {
        d_bits += 6;
        d >>= 6;
    }
    d_bits += (int)fixedint_bit_table[d];
    return d_bits;
}

static size_t
fixedint_numbits(PyObject *v)
{
    if (_PyFixedInt_Check(v)) {
        ssize_t ival = FROM_TAGGED(v);
        return bits_in_ulong(Py_ABS(ival));
    }
    PyObject *w = obj_as_long(v);
    size_t result = _PyLong_NumBits(w);
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
    long long result;
    if (_PyFixedInt_Check(v)) {
        result = FROM_TAGGED(v);
    }
    else {
        PyObject *w = obj_as_long(v);
        result = PyLong_AsLongLong(w);
        Py_XDECREF(w);
    }
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
    if (TAGGED_CHECK(v) && TAGGED_CHECK(w)) {
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

enum {OP_ADD, OP_SUB, OP_MUL};

/* slow version of binop */
static PyObject *
fixedint_binop(PyObject *v, PyObject *w, int op)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv;
    switch (op) {
        case OP_ADD:
            rv = PyNumber_Add(a, b);
            break;
        case OP_SUB:
            rv = PyNumber_Add(a, b);
            break;
        case OP_MUL:
            rv = PyNumber_Multiply(a, b);
            break;
    }
    Py_DECREF(a);
    Py_DECREF(b);
    return rv;
}

/* used by PyNumber_Add */
PyObject *
_PyFixedInt_Add(PyObject *v, PyObject *w)
{
    long a, b, x;
    a = FROM_TAGGED(v);
    b = FROM_TAGGED(w);
    /* casts in the line below avoid undefined behaviour on overflow */
    x = (long)((unsigned long)a + b);
    if (((x^a) >= 0 || (x^b) >= 0) && TAGGED_IN_RANGE(x)) {
        return AS_TAGGED(x);
    }
    return fixedint_binop(v, w, OP_ADD);
}

/* used by PyNumber_Subtract */
PyObject *
_PyFixedInt_Subtract(PyObject *v, PyObject *w)
{
    long a, b, x;
    a = FROM_TAGGED(v);
    b = FROM_TAGGED(w);
    /* casts in the line below avoid undefined behaviour on overflow */
    x = (long)((unsigned long)a - b);
    if ((x^a) >= 0 || (x^~b) >= 0)
        return AS_TAGGED(x);
    return fixedint_binop(v, w, OP_SUB);
}

/* used by PyNumber_Multiply */
PyObject *
_PyFixedInt_Multiply(PyObject *v, PyObject *w)
{
    long a, b;
    long longprod;                      /* a*b in native long arithmetic */
    double doubled_longprod;            /* (double)longprod */
    double doubleprod;                  /* (double)a * (double)b */

    a = FROM_TAGGED(v);
    b = FROM_TAGGED(w);
    /* casts in the next line avoid undefined behaviour on overflow */
    longprod = (long)((unsigned long)a * b);
    doubleprod = (double)a * (double)b;
    doubled_longprod = (double)longprod;

    /* Fast path for normal case:  small multiplicands, and no info
       is lost in either method. */
    if (doubled_longprod == doubleprod) {
        if (TAGGED_IN_RANGE(longprod)) {
            return AS_TAGGED(longprod);
        }
    }
    return fixedint_binop(v, w, OP_MUL);
}

static PyObject *
fixedint_add(PyObject *v, PyObject *w)
{
    if (TAGGED_CHECK(v) && TAGGED_CHECK(w)) {
        return _PyFixedInt_Add(v, w);
    }
    return fixedint_binop(v, w, OP_ADD);
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
    PyObject *rv = PyLong_Type.tp_as_number->nb_multiply(a, b);
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
    double rv = _PyLong_Frexp((PyLongObject *)w, e);
    Py_XDECREF(w);
    return rv;
}

static double
fixedint_as_double(PyObject *v)
{
    PyObject *w = obj_as_long(v);
    double rv = PyLong_AsDouble(w);
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

static int long_divrem(PyLongObject *a, PyLongObject *b,
                       PyLongObject **pdiv, PyLongObject **prem);

static int
fixedint_divrem(PyObject *v, PyObject *w,
                PyLongObject **pdiv, PyLongObject **prem)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    int rv = long_divrem((PyLongObject*)a, (PyLongObject*)b, pdiv, prem);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

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
    return FROM_TAGGED(v) != 0;

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

static PyObject *
fixedint_gcd(PyObject *v, PyObject *w)
{
    PyObject *a = obj_as_long(v);
    PyObject *b = obj_as_long(w);
    PyObject *rv = _PyLong_GCD(a, b);
    Py_XDECREF(a);
    Py_XDECREF(b);
    return rv;
}

static int
fixedint_init_small_cache(void)
{
    PyObject *v;
    for (int ival = -NSMALLNEGFIXEDINTS; ival < NSMALLPOSFIXEDINTS; ival++) {
        v = (PyObject *)_PyLong_FromLongLong(ival);
        if (v == NULL) {
            return 0;
        }
        small_fixedints[ival + NSMALLNEGFIXEDINTS] = v;
    }
    return 1;
}

static void
fixedint_fini_small_cache(void)
{
    PyObject *v;
    for (int ival = -NSMALLNEGFIXEDINTS; ival < NSMALLPOSFIXEDINTS; ival++) {
        v = small_fixedints[ival + NSMALLNEGFIXEDINTS];
        Py_XDECREF(v);
    }
}

#endif /* WITH_FIXEDINT */
