#ifndef Py_LIMITED_API
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

typedef ssize_t tagged_ptr;

/* number of bits to use for the tagged value */
#define TAGGED_MAX_BITS ((sizeof(tagged_ptr) * 8) - 1)

/* min/max signed value that can be represented */
#define TAGGED_MAX_VALUE (((tagged_ptr)1<<(TAGGED_MAX_BITS-1)) - 1)
#define TAGGED_MIN_VALUE (- ((tagged_ptr)1 << (TAGGED_MAX_BITS-1)))

/* return true if value can be storedas tagged */
#define TAGGED_IN_RANGE(v) ((v) >= TAGGED_MIN_VALUE && (v) <= TAGGED_MAX_VALUE)

/* shift value and add tag */
#define AS_TAGGED(x) ((PyObject*) (((x) << 1) | 0x01))

/* remove tag and unshift */
#define FROM_TAGGED(x) (((tagged_ptr)(x)) >> 1)

/* return true if value is a tagged value */
#define TAGGED_CHECK(x) (((tagged_ptr)(x)) & 0x01)

#ifdef __cplusplus
}
#endif
#endif /* !Py_TAGGEDPTR_H */
#endif /* Py_LIMITED_API */
