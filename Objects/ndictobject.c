/* Namespace dictionary object implementation.
*/

/* TODO

*/

#include "Python.h"
#include "structmember.h"
#include "ndictobject.h"
//#include <stddef.h>

/* PyNDictObject */
struct _ndictobject {
    PyDictObject nd_dict;        /* the underlying dict */
    PyObject *nd_namespace;      /* namespace object holding dict */
    //PyObject *nd_weakreflist;    /* holds weakrefs to the ndict */
};


/* ----------------------------------------------
 * NamespaceDict methods
 */

/* __sizeof__() */

/* NamespaceDict.__sizeof__() does not have a docstring. */
PyDoc_STRVAR(ndict_sizeof__doc__, "");

static PyObject *
ndict_sizeof(PyNDictObject *nd)
{
    Py_ssize_t res = _PyDict_SizeOf((PyDictObject *)nd);
    res += sizeof(PyObject *);
    return PyLong_FromSsize_t(res);
}

PyDoc_STRVAR(ndict_copy__doc__, "nd.copy() -> a copy of nd");

static PyObject *
ndict_copy(PyNDictObject *nd)
{
    PyObject *nd_copy;

    if (PyNDict_CheckExact(nd))
        nd_copy = _PyNDict_New(nd->nd_namespace);
    else {
        nd_copy = PyObject_CallFunctionObjArgs((PyObject *)Py_TYPE(nd), nd,
                                               NULL);
        if (nd_copy != NULL)
            ((PyNDictObject *)nd_copy)->nd_namespace = nd->nd_namespace;
    }
    return nd_copy;
}

/* tp_methods */

static PyMethodDef ndict_methods[] = {

    /* overridden dict methods */
    {"__sizeof__",      (PyCFunction)ndict_sizeof,      METH_NOARGS,
     ndict_sizeof__doc__},
    {"copy",            (PyCFunction)ndict_copy,        METH_NOARGS,
     ndict_copy__doc__},

    {NULL,              NULL}   /* sentinel */
};


/* ----------------------------------------------
 * NamespaceDict type slot methods
 */

/* tp_dealloc */

static void
ndict_dealloc(PyNDictObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->nd_namespace);
    PyDict_Type.tp_dealloc((PyObject *)self);
}

/* tp_doc */

PyDoc_STRVAR(ndict_doc,
        "Dictionary that keeps reference to containing namespace");

/* tp_traverse */

static int
ndict_traverse(PyNDictObject *nd, visitproc visit, void *arg)
{
    Py_VISIT(nd->nd_namespace);
    return PyDict_Type.tp_traverse((PyObject *)nd, visit, arg);
}

/* tp_clear */

static int
ndict_tp_clear(PyNDictObject *nd)
{
    Py_CLEAR(nd->nd_namespace);
    PyDict_Clear((PyObject *)nd);
    return 0;
}

/* PyNDict_Type */

PyTypeObject PyNDict_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "collections.NamespaceDict",                /* tp_name */
    sizeof(PyNDictObject),                      /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)ndict_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    ndict_doc,                                  /* tp_doc */
    (traverseproc)ndict_traverse,               /* tp_traverse */
    (inquiry)ndict_tp_clear,                    /* tp_clear */
    0,             /* tp_richcompare */
    0,    /* tp_weaklistoffset */
    0,                    /* tp_iter */
    0,                                          /* tp_iternext */
    ndict_methods,                              /* tp_methods */
    0,                                          /* tp_members */
    0,                               /* tp_getset */
    &PyDict_Type,                               /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,      /* tp_dictoffset */
    0,                       /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    0,                         /* tp_new */
    0,                                          /* tp_free */
};


/* ----------------------------------------------
 * the public NamespaceDict API
 */

PyObject *
_PyNDict_New(PyObject *namespace) {
    PyObject *nd = PyDict_Type.tp_new(&PyNDict_Type, NULL, NULL);
    if (nd != NULL) {
        ((PyNDictObject *)nd)->nd_namespace = namespace;
        Py_XINCREF(namespace);
    }
    return nd;
}

PyObject *
_PyNDict_GetNamespace(PyObject *nd) {
    if (PyNDict_Check(nd))
        return ((PyNDictObject *)nd)->nd_namespace;
    return NULL;
}

int
_PyNDict_SetNamespace(PyObject *nd, PyObject *ns) {
    if (PyNDict_Check(nd)) {
        ((PyNDictObject *)nd)->nd_namespace = ns;
        Py_XINCREF(ns);
        return 1;
    }
    return 0;
}
