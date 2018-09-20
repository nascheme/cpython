#ifndef Py_TAGGEDPTR_H
#define Py_TAGGEDPTR_H
#ifdef __cplusplus
extern "C" {
#endif

/* Support for storing values in tagged pointers. Due to memory alignment
   requirements, low order bits of pointers are always zero.  Tags abuse
   that fact to store non-pointer data (like fixed integers) in pointer
   data types.  The trick is that you must always be careful to check and
   untag before trying to de-reference the pointer.
   */

typedef ssize_t _Py_tagged_ptr;

/* number of bits used for tags */
#define _Py_TAGGED_BITS 1

/* mask to get only tag bits */
#define _Py_TAG_MASK ((1<<_Py_TAGGED_BITS)-1)

/* number of bits available for the tagged value */
#define _Py_TAGGED_MAX_BITS ((sizeof(_Py_tagged_ptr) * 8) - _Py_TAGGED_BITS)

/* min/max signed value that can be represented */
#define _Py_TAGGED_MAX_VALUE (((_Py_tagged_ptr)1<<(_Py_TAGGED_MAX_BITS-1)) - 1)
#define _Py_TAGGED_MIN_VALUE (- ((_Py_tagged_ptr)1 << (_Py_TAGGED_MAX_BITS-1)))

/* return true if value can be storedas tagged */
#define _Py_TAGGED_IN_RANGE(v) ((v) >= _Py_TAGGED_MIN_VALUE && (v) <= _Py_TAGGED_MAX_VALUE)

/* shift value 'x' and add tag 't' */
#define _Py_AS_TAGGED(x, t) ((PyObject*) (((x) << _Py_TAGGED_BITS) | (t)))

/* remove tag and unshift */
#define _Py_FROM_TAGGED(x) (((_Py_tagged_ptr)(x)) >> _Py_TAGGED_BITS)

/* return true if value is a tagged value */
#define _Py_IS_TAGGED(x) (((_Py_tagged_ptr)(x)) & 1)

#define _Py_GET_TAG(x) (((_Py_tagged_ptr)(x)) & _Py_TAG_MASK)

/* return true if value is tagged with tag 't' */
#define _Py_HAS_TAG(x, t) (_Py_GET_TAG(x) == (t))

/* define the tags used */
#define _PyFixedInt_Tag 0x01
#define _PyFixedInt_Check(v) (_Py_HAS_TAG(v, _PyFixedInt_Tag))
#if 0
typedef struct _longobject PyLongObject;
PyAPI_FUNC(Py_ssize_t) _PyFixedInt_Val(PyObject *);
#endif



#ifdef __cplusplus
}
#endif
#endif /* !Py_TAGGEDPTR_H */
