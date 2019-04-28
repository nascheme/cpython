
#include "Python.h"

#include "gcobject.h"

/* A PyObject that's a proxy for a PyGCObject. */

typedef struct {
    PyObject_HEAD
    PyGCObject *real;
} PyGCProxyObject;

PyTypeObject PyGCRef_Type;

#define PyGCRef_CheckExact(o) (Py_TYPE(o) == &PyGCRef_Type)

static PyObject *
PyGCProxyObject_New(PyGCObject *real)
{
    PyGCProxyObject *self = PyObject_MALLOC(sizeof(PyGCProxyObject));
    PyObject_INIT(self, &PyGCRef_Type);
    self->real = real;
    return &self->ob_base;
}

PyObject *
PyObject_FromPyGCObject(PyGCObject *gcob)
{
    if (gcob->gcob_ref == NULL)
        gcob->gcob_ref = PyGCRef_New(gcob);
    return gcob->gcob_ref;
}

PyGCObject *
PyGCObject_FromPyObject(PyObject *ob)
{
    if (PyGCRef_CheckExact(ob))
        return ((PyGCProxyObject *)ob)->real;
    return new_gcobref(ob);
}

static PyObject *
proxy_repr(PyObject *op)
{
    PyGCProxyObject *proxy = (PyGCProxyObject *)op;
    return PyObject_FromPyGCObject(PyGCObject_Repr(proxy->real));
}

static void
proxy_dealloc(PyObject* op)
{
    Py_TYPE(op)->tp_free(op);
}

static PyObject *proxy_nb_add(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_add == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_add(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_subtract(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_subtract == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_subtract(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_multiply(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_multiply == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_multiply(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_remainder(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_remainder == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_remainder(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_divmod(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_divmod == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_divmod(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_power(PyObject *a, PyObject *b, PyObject *c)
{
  PyGCObject *real_a, *proxy_b, *proxy_c, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_power == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  proxy_c = PyGCObject_FromPyObject(c);
  if (proxy_c == NULL)
    return NULL;
  result = m->gcnb_power(real_a, proxy_b, proxy_c);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_negative(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_negative == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_negative(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_positive(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_positive == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_positive(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_absolute(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_absolute == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_absolute(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static int proxy_nb_bool(PyObject *a)
{
  PyGCObject *real_a;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_bool == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  return m->gcnb_bool(real_a);
};

static PyObject *proxy_nb_invert(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_invert == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_invert(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_lshift(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_lshift == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_lshift(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_rshift(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_rshift == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_rshift(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_and(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_and == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_and(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_xor(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_xor == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_xor(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_or(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_or == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_or(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_int(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_int == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_int(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_float(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_float == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_float(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PyObject *proxy_nb_inplace_add(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_add == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_add(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_subtract(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_subtract == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_subtract(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_multiply(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_multiply == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_multiply(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_remainder(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_remainder == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_remainder(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_power(PyObject *a, PyObject *b, PyObject *c)
{
  PyGCObject *real_a, *proxy_b, *proxy_c, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_power == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  proxy_c = PyGCObject_FromPyObject(c);
  if (proxy_c == NULL)
    return NULL;
  result = m->gcnb_inplace_power(real_a, proxy_b, proxy_c);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_lshift(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_lshift == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_lshift(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_rshift(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_rshift == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_rshift(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_and(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_and == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_and(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_xor(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_xor == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_xor(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_or(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_or == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_or(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PyObject *proxy_nb_floor_divide(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_floor_divide == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_floor_divide(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_true_divide(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_true_divide == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_true_divide(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_floor_divide(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_floor_divide == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_floor_divide(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_true_divide(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_true_divide == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_true_divide(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PyObject *proxy_nb_index(PyObject *a)
{
  PyGCObject *real_a, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_index == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcnb_index(real_a);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PyObject *proxy_nb_matrix_multiply(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_matrix_multiply == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_matrix_multiply(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_nb_inplace_matrix_multiply(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCNumberMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_number;
  if (m == NULL || m->gcnb_inplace_matrix_multiply == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcnb_inplace_matrix_multiply(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PyNumberMethods proxy_as_number = {
    proxy_nb_add,
    proxy_nb_subtract,
    proxy_nb_multiply,
    proxy_nb_remainder,
    proxy_nb_divmod,
    proxy_nb_power,
    proxy_nb_negative,
    proxy_nb_positive,
    proxy_nb_absolute,
    proxy_nb_bool,
    proxy_nb_invert,
    proxy_nb_lshift,
    proxy_nb_rshift,
    proxy_nb_and,
    proxy_nb_xor,
    proxy_nb_or,
    proxy_nb_int,
    0,
    proxy_nb_float,
    proxy_nb_inplace_add,
    proxy_nb_inplace_subtract,
    proxy_nb_inplace_multiply,
    proxy_nb_inplace_remainder,
    proxy_nb_inplace_power,
    proxy_nb_inplace_lshift,
    proxy_nb_inplace_rshift,
    proxy_nb_inplace_and,
    proxy_nb_inplace_xor,
    proxy_nb_inplace_or,
    proxy_nb_floor_divide,
    proxy_nb_true_divide,
    proxy_nb_inplace_floor_divide,
    proxy_nb_inplace_true_divide,
    proxy_nb_index,
    proxy_nb_matrix_multiply,
    proxy_nb_inplace_matrix_multiply,
};


static Py_ssize_t proxy_sq_length(PyObject *a)
{
  PyGCObject *real_a;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_length == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  return m->gcsq_length(real_a);
};

static PyObject *proxy_sq_concat(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_concat == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcsq_concat(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_sq_repeat(PyObject *a, Py_ssize_t n)
{
  PyGCObject *real_a, *result;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_repeat == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcsq_repeat(real_a, n);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_sq_item(PyObject *a, Py_ssize_t n)
{
  PyGCObject *real_a, *result;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_item == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcsq_item(real_a, n);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static int proxy_sq_ass_item(PyObject *a, Py_ssize_t n, PyObject *b)
{
  PyGCObject *real_a, *proxy_b;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_ass_item == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return -1;
  return m->gcsq_ass_item(real_a, n, proxy_b);
}


static int proxy_sq_contains(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_contains == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return -1;
  return m->gcsq_contains(real_a, proxy_b);
}


static PyObject *proxy_sq_inplace_concat(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_inplace_concat == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcsq_inplace_concat(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};

static PyObject *proxy_sq_inplace_repeat(PyObject *a, Py_ssize_t n)
{
  PyGCObject *real_a, *result;
  PyGCSequenceMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_sequence;
  if (m == NULL || m->gcsq_inplace_repeat == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  result = m->gcsq_inplace_repeat(real_a, n);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static PySequenceMethods proxy_as_sequence = {
    proxy_sq_length,
    proxy_sq_concat,
    proxy_sq_repeat,
    proxy_sq_item,
    0,
    proxy_sq_ass_item,
    0,
    proxy_sq_contains,
    proxy_sq_inplace_concat,
    proxy_sq_inplace_repeat,
};


static Py_ssize_t proxy_mp_length(PyObject *a)
{
  PyGCObject *real_a;
  PyGCMappingMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_mapping;
  if (m == NULL || m->gcmp_length == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  return m->gcmp_length(real_a);
};

static PyObject *proxy_mp_subscript(PyObject *a, PyObject *b)
{
  PyGCObject *real_a, *proxy_b, *result;
  PyGCMappingMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_mapping;
  if (m == NULL || m->gcmp_subscript == NULL)
    Py_RETURN_NOTIMPLEMENTED;
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return NULL;
  result = m->gcmp_subscript(real_a, proxy_b);
  if (result == NULL)
    return NULL;
  return PyObject_FromPyGCObject(result);
};


static int proxy_mp_ass_subscript(PyObject *a, PyObject *b, PyObject *c)
{
  PyGCObject *real_a, *proxy_b, *proxy_c;
  PyGCMappingMethods *m;
  assert(PyGCRef_CheckExact(a));
  real_a = ((PyGCProxyObject *)a)->proxy_real;
  m = PyGC_TYPE(real_a)->gctp_as_mapping;
  if (m == NULL || m->gcmp_ass_subscript == NULL) {
    PyErr_SetString(PyExc_TypeError, "unsupported by proxy");
    return -1;
  }
  proxy_b = PyGCObject_FromPyObject(b);
  if (proxy_b == NULL)
    return -1;
  proxy_c = PyGCObject_FromPyObject(c);
  if (proxy_c == NULL)
    return -1;
  return m->gcmp_ass_subscript(real_a, proxy_b, proxy_c);
}

static PyMappingMethods proxy_as_mapping = {
    proxy_mp_length,
    proxy_mp_subscript,
    proxy_mp_ass_subscript,
};


/* TODO(thomas@python.org): tp_as_buffer, tp_as_async */

#undef DEFINE_PROXY_SSIZEARGFUNC
#undef DEFINE_PROXY_LENFUNC
#undef DEFINE_PROXY_INQUIRYFUNC
#undef DEFINE_PROXY_TERNARYFUNC
#undef DEFINE_PROXY_UNARYFUNC
#undef DEFINE_PROXY_BINARYFUNC


PyTypeObject PyGCRef_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "PyGCRef_Type",
    sizeof(PyGCProxyObject),
    0,                  /*tp_itemsize*/
    proxy_dealloc,        /*tp_dealloc*/
    0,                  /*tp_print*/
    0,                  /*tp_getattr*/
    0,                  /*tp_setattr*/
    0,                  /*tp_reserved*/
    proxy_repr,           /*tp_repr*/
    &proxy_as_number,     /*tp_as_number*/
    &proxy_as_sequence,   /*tp_as_sequence*/
    &proxy_as_mapping,    /*tp_as_mapping*/
    0,                  /*tp_hash */
    0,                  /*tp_call */
    0,                  /*tp_str */
    0,                  /*tp_getattro */
    0,                  /*tp_setattro */
    0,                  /*tp_as_buffer */
    Py_TPFLAGS_DEFAULT, /*tp_flags */
    0,                  /*tp_doc */
    0,                  /*tp_traverse */
    0,                  /*tp_clear */
    0,                  /*tp_richcompare */
    0,                  /*tp_weaklistoffset */
    0,                  /*tp_iter */
    0,                  /*tp_iternext */
    0,                  /*tp_methods */
    0,                  /*tp_members */
    0,                  /*tp_getset */
    0,                  /*tp_base */
    0,                  /*tp_dict */
    0,                  /*tp_descr_get */
    0,                  /*tp_descr_set */
    0,                  /*tp_dictoffset */
    0,                  /*tp_init */
    0,                  /*tp_alloc */
    0,                  /*tp_new */
};
