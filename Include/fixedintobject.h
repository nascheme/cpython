/* fixed integer stored as tagged pointer */

PyAPI_DATA(PyTypeObject) PyLong_Type;
typedef struct _longobject PyLongObject;
/* return true if object is tagged pointer containing fixed int */
PyAPI_FUNC(PyObject *) _PyFixedInt_Add(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyFixedInt_Subtract(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyFixedInt_Multiply(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyFixedInt_Untag(PyObject *);
PyAPI_FUNC(Py_ssize_t) _PyFixedInt_Val(PyObject *);
