/*[clinic input]
preserve
[clinic start generated code]*/

PyDoc_STRVAR(_contextvars_copy_context__doc__,
"copy_context($module, /)\n"
"--\n"
"\n");

#define _CONTEXTVARS_COPY_CONTEXT_METHODDEF    \
    {"copy_context", (PyCFunction)_contextvars_copy_context, METH_NOARGS, _contextvars_copy_context__doc__},

static PyObject *
_contextvars_copy_context_impl(PyObject *module);

static PyObject *
_contextvars_copy_context(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return _contextvars_copy_context_impl(module);
}

PyDoc_STRVAR(_contextvars__empty_context__doc__,
"_empty_context($module, /)\n"
"--\n"
"\n");

#define _CONTEXTVARS__EMPTY_CONTEXT_METHODDEF    \
    {"_empty_context", (PyCFunction)_contextvars__empty_context, METH_NOARGS, _contextvars__empty_context__doc__},

static PyObject *
_contextvars__empty_context_impl(PyObject *module);

static PyObject *
_contextvars__empty_context(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return _contextvars__empty_context_impl(module);
}
/*[clinic end generated code: output=77860971c96ec652 input=a9049054013a1b77]*/
