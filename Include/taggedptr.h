#ifndef Py_LIMITED_API
#ifndef Py_TAGGEDPTR_H
#define Py_TAGGEDPTR_H
#ifdef __cplusplus
extern "C" {
#endif

typedef ssize_t tagged_ptr;

#define MAX_BITS ((sizeof(tagged_ptr) * 8) - 1)

#define MAX_TAGGED_VALUE (((tagged_ptr)1<<(MAX_BITS-1)) - 1)
#define MIN_TAGGED_VALUE (- ((tagged_ptr)1 << (MAX_BITS-1)))

#define CAN_TAG(v) ((v) >= MIN_TAGGED_VALUE && (v) <= MAX_TAGGED_VALUE)

#define TAG_IT(x) ((PyObject*) (((x) << 1) | 0x01))
#define UNTAG_IT(x) (((tagged_ptr)(x)) >> 1)
#define IS_TAGGED(x) (((tagged_ptr)(x)) & 0x01)

#ifdef __cplusplus
}
#endif
#endif /* !Py_TAGGEDPTR_H */
#endif /* Py_LIMITED_API */
