#ifndef Py_NDICTOBJECT_H
#define Py_NDICTOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ndictobject PyNDictObject;

PyAPI_DATA(PyTypeObject) PyNDict_Type;

#define PyNDict_Check(op) PyObject_TypeCheck(op, &PyNDict_Type)
#define PyNDict_CheckExact(op) (Py_TYPE(op) == &PyNDict_Type)

PyAPI_FUNC(PyObject *) _PyNDict_New(PyObject *ns);
PyAPI_FUNC(PyObject *) _PyNDict_GetNamespace(PyObject *nd);
PyAPI_FUNC(int) _PyNDict_SetNamespace(PyObject *nd, PyObject *ns);

#ifdef __cplusplus
}
#endif
#endif /* !Py_NDICTOBJECT_H */
