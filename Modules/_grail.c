/* Experimental Grail core. Only the free-threaded build is supported. */
#include "Python.h"
#include "internal/pycore_grail.h"

static PyObject *
grail_configure(PyObject *module, PyObject *args)
{
    PyObject *repr, *eq, *hash, *operators;
    if (!PyArg_ParseTuple(args, "OOOO:_configure", &repr, &eq, &hash, &operators)) {
        return NULL;
    }
    GrailState *state = PyModule_GetState(module);
    if (!Py_IS_TYPE(repr, state->generic_type) ||
        !Py_IS_TYPE(eq, state->generic_type) ||
        !Py_IS_TYPE(hash, state->generic_type) ||
        !PyTuple_CheckExact(operators) || PyTuple_GET_SIZE(operators) != 6) {
        PyErr_SetString(PyExc_TypeError, "invalid Grail hooks");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < 6; i++) {
        if (!Py_IS_TYPE(PyTuple_GET_ITEM(operators, i), state->generic_type)) {
            PyErr_SetString(PyExc_TypeError, "operators must be native generics");
            return NULL;
        }
    }
    /* Called once by the facade during import, before record publication. */
    int configured;
    Py_BEGIN_CRITICAL_SECTION(module);
    configured = state->operators != NULL;
    if (!configured && _PyGrail_ConfigureOperators(operators) < 0) {
        configured = -1;
    }
    if (!configured) {
        state->repr_hook = Py_NewRef(repr);
        state->eq_hook = Py_NewRef(eq);
        state->hash_hook = Py_NewRef(hash);
        state->operators = Py_NewRef(operators);
    }
    Py_END_CRITICAL_SECTION();
    if (configured < 0) {
        return NULL;
    }
    if (configured) {
        PyErr_SetString(PyExc_RuntimeError, "Grail hooks are already configured");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
grail_is_record_type(PyObject *module, PyObject *obj)
{
    return PyBool_FromLong(_PyGrail_IsRecordType(obj));
}

static PyMethodDef grail_methods[] = {
    {"_configure", grail_configure, METH_VARARGS, NULL},
    {"_make_record", _PyGrail_MakeRecord, METH_VARARGS, NULL},
    {"_record_info", _PyGrail_RecordInfo, METH_O, NULL},
    {"_record_values", _PyGrail_RecordValues, METH_O, NULL},
    {"_is_record_type", grail_is_record_type, METH_O, NULL},
    {"_is_conforming", _PyGrail_IsConforming, METH_O, NULL},
    {"_deepcopy_is_self", _PyGrail_DeepcopyIsSelf, METH_O, NULL},
    {"_kernel", _PyGrail_MakeKernel, METH_VARARGS, NULL},
    {"_is_kernel", _PyGrail_IsKernelPy, METH_O, NULL},
    {"substitutable", _PyGrail_Substitutable, METH_VARARGS, NULL},
    {"_default_repr", _PyGrail_DefaultRepr, METH_O, NULL},
    {"_default_eq", _PyGrail_DefaultEq, METH_VARARGS, NULL},
    {"_default_hash", _PyGrail_DefaultHash, METH_O, NULL},
    {NULL}
};

static int
grail_traverse(PyObject *module, visitproc visit, void *arg)
{
    GrailState *state = PyModule_GetState(module);
    Py_VISIT(state->generic_type);
    Py_VISIT(state->repr_hook);
    Py_VISIT(state->eq_hook);
    Py_VISIT(state->hash_hook);
    Py_VISIT(state->operators);
    Py_VISIT(state->kernel_type);
    return 0;
}

static int
grail_clear(PyObject *module)
{
    GrailState *state = PyModule_GetState(module);
    Py_CLEAR(state->generic_type);
    Py_CLEAR(state->repr_hook);
    Py_CLEAR(state->eq_hook);
    Py_CLEAR(state->hash_hook);
    Py_CLEAR(state->operators);
    Py_CLEAR(state->kernel_type);
    return 0;
}

static int
grail_exec(PyObject *module)
{
    if (_PyGrail_InitGenerics(module) < 0) {
        return -1;
    }
    if (_PyGrail_InitKernels(module) < 0) {
        return -1;
    }
    if (_PyGrail_AddCopyReplace(module) < 0) {
        return -1;
    }
    return PyModule_AddStringConstant(module, "backend", "native-phase1");
}

PyABIInfo_VAR(grail_abi);

static PyModuleDef_Slot grail_slots[] = {
    {Py_mod_abi, &grail_abi},
    {Py_mod_exec, grail_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef grail_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_grail",
    .m_size = sizeof(GrailState),
    .m_methods = grail_methods,
    .m_slots = grail_slots,
    .m_traverse = grail_traverse,
    .m_clear = grail_clear,
};

PyMODINIT_FUNC
PyInit__grail(void)
{
    return PyModuleDef_Init(&grail_module);
}
