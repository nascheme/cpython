/* Fixed integer object, stored tagged pointer */

#include "Python.h"
#include <stdbool.h>

/* cribbed from pyjion */
typedef ssize_t tagged_ptr;

#define MAX_BITS ((sizeof(tagged_ptr) * 8) - 1)

#define MAX_TAGGED_VALUE (((tagged_ptr)1<<(MAX_BITS-1)) - 1)
#define MIN_TAGGED_VALUE (- ((tagged_ptr)1 << (MAX_BITS-1)))

static bool
can_tag(tagged_ptr value) {
    return value >= MIN_TAGGED_VALUE && value <= MAX_TAGGED_VALUE;
}

#define TAG_IT(x) ((PyObject*) (((x) << 1) | 0x01))
#define UNTAG_IT(x) (((tagged_ptr)(x)) >> 1)
#define IS_TAGGED(x) (((tagged_ptr)(x)) & 0x01)

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
        a = PyLong_FromLongLong(UNTAG_IT(v));
    }
    else {
        a = v;
        Py_INCREF(v);
    }
    return a;
}


static PyObject *
fixedint_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    Py_ssize_t x = 0;

    if (!PyArg_ParseTuple(args, "|l", &x))
        return NULL;
    assert(type == &PyFixedInt_Type);
    if (!can_tag(x)) {
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
    Py_DECREF(w);
    return result;
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
    if (((x^a) >= 0 || (x^b) >= 0) && can_tag(x)) {
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
fixedint_index(PyObject *v)
{
    return obj_as_long(v);
}

static void
fixedint_dealloc(PyObject *op)
{
    // noop
}

static PyNumberMethods fixedint_as_number = {
    fixedint_add,          /* nb_add */
    0,          /* nb_subtract */
    0,          /* nb_multiply */
    0,          /* nb_remainder */
    0,       /* nb_divmod */
    0,          /* nb_power */
    0, /* nb_negative */
    0,        /* nb_positive */
    0, /* nb_absolute */
    0, /* nb_bool */
    0,                  /* nb_invert */
    0,                  /* nb_lshift */
    0,                  /* nb_rshift */
    0,                  /* nb_and */
    0,                  /* nb_xor */
    0,                  /* nb_or */
    0, /* nb_int */
    0,                  /* nb_reserved */
    0,        /* nb_int */
    0,                  /* nb_inplace_add */
    0,                  /* nb_inplace_subtract */
    0,                  /* nb_inplace_multiply */
    0,                  /* nb_inplace_remainder */
    0,                  /* nb_inplace_power */
    0,                  /* nb_inplace_lshift */
    0,                  /* nb_inplace_rshift */
    0,                  /* nb_inplace_and */
    0,                  /* nb_inplace_xor */
    0,                  /* nb_inplace_or */
    0,    		/* nb_floor_divide */
    0,          	/* nb_true_divide */
    0,                  /* nb_inplace_floor_divide */
    0,                  /* nb_inplace_true_divide */
    fixedint_index,     /* nb_index */
};


PyTypeObject PyFixedInt_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "fixedint",
    sizeof(tagged_ptr),
    0,
    (destructor)fixedint_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)fixedint_repr,                       /* tp_repr */
    &fixedint_as_number,                           /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                       /* tp_hash */
    0,                                          /* tp_call */
    (reprfunc)fixedint_repr,                       /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,   /* tp_flags */
    0,                           /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                              /* tp_methods */
    0,                                          /* tp_members */
    0,                               /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    fixedint_new,                                  /* tp_new */
};
