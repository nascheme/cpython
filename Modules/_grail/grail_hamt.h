/* Copied from grail-compat src/grail/_c/ by tools/sync_to_cpython_grail.py; do not edit here. */
/* Grail's copy of CPython's HAMT (Python/hamt.c), the immutable mapping
 * behind contextvars, used by PMap and PSet.  See grail_hamt.c for the
 * provenance and the differences from upstream.
 */
#ifndef GRAIL_HAMT_H
#define GRAIL_HAMT_H

#include "Python.h"
#include <stdint.h>

/*
HAMT tree is shaped by hashes of keys. Every group of 5 bits of a hash denotes
the exact position of the key in one level of the tree. Since we're using
32 bit hashes, we can have at most 7 such levels. Although if there are
two distinct keys with equal hashes, they will have to occupy the same
cell in the 7th level of the tree -- so we'd put them in a "collision" node.
Which brings the total possible tree depth to 8. Read more about the actual
layout of the HAMT tree in `grail_hamt.c`.

This constant is used to define a datastucture for storing iteration state.
*/
#define GRAIL_HAMT_MAX_TREE_DEPTH 8


/* Abstract tree node. */
typedef struct {
    PyObject_HEAD
} GrailHamtNode;


/* An HAMT immutable mapping collection. */
typedef struct {
    PyObject_HEAD
    GrailHamtNode *h_root;
    PyObject *h_weakreflist;
    Py_ssize_t h_count;
} GrailHamtObject;


/* Per-module state: the HAMT object and node types, and the shared empty
   bitmap node and empty HAMT.  The owning module keeps this struct as the
   FIRST member of its module state, so the state can be found from any HAMT
   object or node with PyType_GetModuleState(Py_TYPE(obj)). */
typedef struct {
    PyTypeObject *hamt_type;
    PyTypeObject *bitmap_type;
    PyTypeObject *array_type;
    PyTypeObject *collision_type;
    GrailHamtNode *empty_bitmap;
    GrailHamtObject *empty;
} GrailHamtState;

/* The state that owns a HAMT object. */
static inline GrailHamtState *
GrailHamt_GetState(GrailHamtObject *o)
{
    return (GrailHamtState *)PyType_GetModuleState(Py_TYPE(o));
}

/* Create the types and singletons in module exec.  Return 0 or -1. */
int GrailHamt_InitState(PyObject *module, GrailHamtState *st);
int GrailHamt_TraverseState(GrailHamtState *st, visitproc visit, void *arg);
void GrailHamt_ClearState(GrailHamtState *st);


/* A struct to hold the state of depth-first traverse of the tree.

   HAMT is an immutable collection.  Iterators will hold a strong reference
   to it, and every node in the HAMT has strong references to its children.

   So for iterators, we can implement zero allocations and zero reference
   inc/dec depth-first iteration.

   - i_nodes: an array of seven pointers to tree nodes
   - i_level: the current node in i_nodes
   - i_pos: an array of positions within nodes in i_nodes.
*/
typedef struct {
    GrailHamtNode *i_nodes[GRAIL_HAMT_MAX_TREE_DEPTH];
    Py_ssize_t i_pos[GRAIL_HAMT_MAX_TREE_DEPTH];
    int8_t i_level;
} GrailHamtIteratorState;


/* Return a new reference to the empty HAMT. */
GrailHamtObject * GrailHamt_New(GrailHamtState *st);

/* Return a new collection based on "o", but with an additional
   key/val pair. */
GrailHamtObject * GrailHamt_Assoc(GrailHamtObject *o, PyObject *key, PyObject *val);

/* Return a new collection based on "o", but without "key". */
GrailHamtObject * GrailHamt_Without(GrailHamtObject *o, PyObject *key);


/* Set key/val in *o for a builder.  If the caller's reference in *o is the
   only one, nodes that only it reaches are updated in place; otherwise *o
   is replaced by a new HAMT as GrailHamt_Assoc would return.

   Return 0 on success, -1 on error (*o unchanged). */
int GrailHamt_AssocInPlace(GrailHamtObject **o, PyObject *key, PyObject *val);


/* Build a HAMT from n keys and values in O(n) node allocations.  Of equal
   keys the last one wins.  The arrays must stay alive and unchanged during
   the call, which may run __hash__ and __eq__.

   Return a new reference, or NULL with an exception set. */
GrailHamtObject * GrailHamt_FromItems(GrailHamtState *st,
                                      PyObject *const *keys,
                                      PyObject *const *vals, Py_ssize_t n);


/* Iterate over the items of o without allocating, yielding borrowed
   references.  o must stay alive and unchanged during the iteration.

   GrailHamt_IteratorNext returns 1 for an item and 0 at the end. */
void GrailHamt_IteratorInit(GrailHamtIteratorState *iter, GrailHamtObject *o);
int GrailHamt_IteratorNext(GrailHamtIteratorState *iter,
                           PyObject **key, PyObject **val);

/* Find "key" in the "o" collection.

   Return:
   - -1: An error occurred.
   - 0: "key" wasn't found in "o".
   - 1: "key" is in "o"; "*val" is set to its value (a borrowed ref).
*/
int GrailHamt_Find(GrailHamtObject *o, PyObject *key, PyObject **val);


/* Return the size of "o"; equivalent of "len(o)". */
Py_ssize_t GrailHamt_Len(GrailHamtObject *o);

#endif /* GRAIL_HAMT_H */
