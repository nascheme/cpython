/* Copied from grail-compat src/grail/_c/ by tools/sync_to_proto.py; do not edit here. */
/* Grail's copy of CPython's HAMT.

   Vendored from cpython-proto 58b6a084ae0 Python/hamt.c, which is the
   CPython 3.15 branch's file (last upstream change gh-155515, GC-tracked
   iterators) plus the Grail additions GrailHamt_AssocInPlace,
   GrailHamt_FromItems and GrailHamt_IteratorInit/Next.  Changes from that file:
   symbols renamed (_PyHamt_* -> GrailHamt_*); the object and node types are
   heap types created per module (GrailHamtState) instead of static types,
   and the empty node and empty HAMT are per-module objects instead of
   runtime singletons; the Python-level mapping API ("hamt" methods, keys/
   values/items iterators, _PyHamt_Eq) and the Py_DEBUG dump helpers are
   removed.  The tree layout and algorithms are unchanged.

   Copyright (c) 2001 Python Software Foundation; All Rights Reserved.
   Licensed under the Python Software Foundation License Version 2. */

#include "grail_compat.h"
#include "grail_hamt.h"
#include <stddef.h>               // offsetof()

/*
This file provides an implementation of an immutable mapping using the
Hash Array Mapped Trie (or HAMT) datastructure.

This design allows to have:

1. Efficient copy: immutable mappings can be copied by reference,
   making it an O(1) operation.

2. Efficient mutations: due to structural sharing, only a portion of
   the trie needs to be copied when the collection is mutated.  The
   cost of set/delete operations is O(log N).

3. Efficient lookups: O(log N).

(where N is number of key/value items in the immutable mapping.)


HAMT
====

The core idea of HAMT is that the shape of the trie is encoded into the
hashes of keys.

Say we want to store a K/V pair in our mapping.  First, we calculate the
hash of K, let's say it's 19830128, or in binary:

    0b1001011101001010101110000 = 19830128

Now let's partition this bit representation of the hash into blocks of
5 bits each:

    0b00_00000_10010_11101_00101_01011_10000 = 19830128
          (6)   (5)   (4)   (3)   (2)   (1)

Each block of 5 bits represents a number between 0 and 31.  So if we have
a tree that consists of nodes, each of which is an array of 32 pointers,
those 5-bit blocks will encode a position on a single tree level.

For example, storing the key K with hash 19830128, results in the following
tree structure:

                     (array of 32 pointers)
                     +---+ -- +----+----+----+ -- +----+
  root node          | 0 | .. | 15 | 16 | 17 | .. | 31 |   0b10000 = 16 (1)
  (level 1)          +---+ -- +----+----+----+ -- +----+
                                      |
                     +---+ -- +----+----+----+ -- +----+
  a 2nd level node   | 0 | .. | 10 | 11 | 12 | .. | 31 |   0b01011 = 11 (2)
                     +---+ -- +----+----+----+ -- +----+
                                      |
                     +---+ -- +----+----+----+ -- +----+
  a 3rd level node   | 0 | .. | 04 | 05 | 06 | .. | 31 |   0b00101 = 5  (3)
                     +---+ -- +----+----+----+ -- +----+
                                      |
                     +---+ -- +----+----+----+----+
  a 4th level node   | 0 | .. | 04 | 29 | 30 | 31 |        0b11101 = 29 (4)
                     +---+ -- +----+----+----+----+
                                      |
                     +---+ -- +----+----+----+ -- +----+
  a 5th level node   | 0 | .. | 17 | 18 | 19 | .. | 31 |   0b10010 = 18 (5)
                     +---+ -- +----+----+----+ -- +----+
                                      |
                       +--------------+
                       |
                     +---+ -- +----+----+----+ -- +----+
  a 6th level node   | 0 | .. | 15 | 16 | 17 | .. | 31 |   0b00000 = 0  (6)
                     +---+ -- +----+----+----+ -- +----+
                       |
                       V -- our value (or collision)

To rehash: for a K/V pair, the hash of K encodes where in the tree V will
be stored.

To optimize memory footprint and handle hash collisions, our implementation
uses three different types of nodes:

 * A Bitmap node;
 * An Array node;
 * A Collision node.

Because we implement an immutable dictionary, our nodes are also
immutable.  Therefore, when we need to modify a node, we copy it, and
do that modification to the copy.


Array Nodes
-----------

These nodes are very simple.  Essentially they are arrays of 32 pointers
we used to illustrate the high-level idea in the previous section.

We use Array nodes only when we need to store more than 16 pointers
in a single node.

Array nodes do not store key objects or value objects.  They are used
only as an indirection level - their pointers point to other nodes in
the tree.


Bitmap Node
-----------

Allocating a new 32-pointers array for every node of our tree would be
very expensive.  Unless we store millions of keys, most of tree nodes would
be very sparse.

When we have less than 16 elements in a node, we don't want to use the
Array node, that would mean that we waste a lot of memory.  Instead,
we can use bitmap compression and can have just as many pointers
as we need!

Bitmap nodes consist of two fields:

1. An array of pointers.  If a Bitmap node holds N elements, the
   array will be of N pointers.

2. A 32bit integer -- a bitmap field.  If an N-th bit is set in the
   bitmap, it means that the node has an N-th element.

For example, say we need to store a 3 elements sparse array:

   +---+  --  +---+  --  +----+  --  +----+
   | 0 |  ..  | 4 |  ..  | 11 |  ..  | 17 |
   +---+  --  +---+  --  +----+  --  +----+
                |          |           |
                o1         o2          o3

We allocate a three-pointer Bitmap node.  Its bitmap field will be
then set to:

   0b_00100_00010_00000_10000 == (1 << 17) | (1 << 11) | (1 << 4)

To check if our Bitmap node has an I-th element we can do:

   bitmap & (1 << I)


And here's a formula to calculate a position in our pointer array
which would correspond to an I-th element:

   popcount(bitmap & ((1 << I) - 1))


Let's break it down:

 * `popcount` is a function that returns a number of bits set to 1;

 * `((1 << I) - 1)` is a mask to filter the bitmask to contain bits
   set to the *right* of our bit.


So for our 17, 11, and 4 indexes:

 * bitmap & ((1 << 17) - 1) == 0b100000010000 => 2 bits are set => index is 2.

 * bitmap & ((1 << 11) - 1) == 0b10000 => 1 bit is set => index is 1.

 * bitmap & ((1 << 4) - 1) == 0b0 => 0 bits are set => index is 0.


To conclude: Bitmap nodes are just like Array nodes -- they can store
a number of pointers, but use bitmap compression to eliminate unused
pointers.


Bitmap nodes have two pointers for each item:

  +----+----+----+----+  --  +----+----+
  | k1 | v1 | k2 | v2 |  ..  | kN | vN |
  +----+----+----+----+  --  +----+----+

When kI == NULL, vI points to another tree level.

When kI != NULL, the actual key object is stored in kI, and its
value is stored in vI.


Collision Nodes
---------------

Collision nodes are simple arrays of pointers -- two pointers per
key/value.  When there's a hash collision, say for k1/v1 and k2/v2
we have `hash(k1)==hash(k2)`.  Then our collision node will be:

  +----+----+----+----+
  | k1 | v1 | k2 | v2 |
  +----+----+----+----+


Tree Structure
--------------

All nodes are PyObjects.

The `GrailHamtObject` object has a pointer to the root node (h_root),
and has a length field (h_count).

High-level functions accept a GrailHamtObject object and dispatch to
lower-level functions depending on what kind of node h_root points to.


Operations
==========

There are three fundamental operations on an immutable dictionary:

1. "o.assoc(k, v)" will return a new immutable dictionary, that will be
   a copy of "o", but with the "k/v" item set.

   Functions in this file:

        hamt_node_assoc, hamt_node_bitmap_assoc,
        hamt_node_array_assoc, hamt_node_collision_assoc

   `hamt_node_assoc` function accepts a node object, and calls
   other functions depending on its actual type.

2. "o.find(k)" will lookup key "k" in "o".

   Functions:

        hamt_node_find, hamt_node_bitmap_find,
        hamt_node_array_find, hamt_node_collision_find

3. "o.without(k)" will return a new immutable dictionary, that will be
   a copy of "o", buth without the "k" key.

   Functions:

        hamt_node_without, hamt_node_bitmap_without,
        hamt_node_array_without, hamt_node_collision_without


Further Reading
===============

1. http://blog.higher-order.net/2009/09/08/understanding-clojures-persistenthashmap-deftwice.html

2. http://blog.higher-order.net/2010/08/16/assoc-and-clojures-persistenthashmap-part-ii.html

3. Clojure's PersistentHashMap implementation:
   https://github.com/clojure/clojure/blob/master/src/jvm/clojure/lang/PersistentHashMap.java


Debug
=====

The HAMT datatype is accessible for testing purposes under the
`_testinternalcapi` module:

    >>> from _testinternalcapi import hamt
    >>> h = hamt()
    >>> h2 = h.set('a', 2)
    >>> h3 = h2.set('b', 3)
    >>> list(h3)
    ['a', 'b']

When CPython is built in debug mode, a '__dump__()' method is available
to introspect the tree:

    >>> print(h3.__dump__())
    HAMT(len=2):
        BitmapNode(size=4 count=2 bitmap=0b110 id=0x10eb9d9e8):
            'a': 2
            'b': 3
*/


/* Node types are per-module heap types, so identify a node by its
   tp_dealloc, which every module's copy of a type shares. */
static void hamt_node_array_dealloc(PyObject *self);
static void hamt_node_bitmap_dealloc(PyObject *self);
static void hamt_node_collision_dealloc(PyObject *self);

#define IS_ARRAY_NODE(node)     (Py_TYPE(node)->tp_dealloc == hamt_node_array_dealloc)
#define IS_BITMAP_NODE(node)    (Py_TYPE(node)->tp_dealloc == hamt_node_bitmap_dealloc)
#define IS_COLLISION_NODE(node) (Py_TYPE(node)->tp_dealloc == hamt_node_collision_dealloc)

/* The module state that owns a HAMT object or node. */
#define HAMT_STATE(op) \
    ((GrailHamtState *)PyType_GetModuleState(Py_TYPE(op)))


/* Return type for 'find' (lookup a key) functions.

   * F_ERROR - an error occurred;
   * F_NOT_FOUND - the key was not found;
   * F_FOUND - the key was found.
*/
typedef enum {F_ERROR, F_NOT_FOUND, F_FOUND} hamt_find_t;


/* Return type for 'without' (delete a key) functions.

   * W_ERROR - an error occurred;
   * W_NOT_FOUND - the key was not found: there's nothing to delete;
   * W_EMPTY - the key was found: the node/tree would be empty
     if the key is deleted;
   * W_NEWNODE - the key was found: a new node/tree is returned
     without that key.
*/
typedef enum {W_ERROR, W_NOT_FOUND, W_EMPTY, W_NEWNODE} hamt_without_t;


/* Low-level iterator protocol type.

   * I_ITEM - a new item has been yielded;
   * I_END - the whole tree was visited (similar to StopIteration).
*/
typedef enum {I_ITEM, I_END} hamt_iter_t;


#define HAMT_ARRAY_NODE_SIZE 32


typedef struct {
    PyObject_VAR_HEAD
    uint32_t b_bitmap;
    PyObject *b_array[1];
} GrailHamtNode_Bitmap;


typedef struct {
    PyObject_HEAD
    GrailHamtNode *a_array[HAMT_ARRAY_NODE_SIZE];
    Py_ssize_t a_count;
} GrailHamtNode_Array;

#define GrailHamtNode_Array_CAST(op)      ((GrailHamtNode_Array *)(op))


typedef struct {
    PyObject_VAR_HEAD
    int32_t c_hash;
    PyObject *c_array[1];
} GrailHamtNode_Collision;

#define GrailHamtNode_Collision_CAST(op)  ((GrailHamtNode_Collision *)(op))


static GrailHamtObject *
hamt_alloc(GrailHamtState *st);

static GrailHamtNode *
hamt_node_assoc(GrailHamtNode *node,
                uint32_t shift, int32_t hash,
                PyObject *key, PyObject *val, int* added_leaf, int mut);

static hamt_without_t
hamt_node_without(GrailHamtNode *node,
                  uint32_t shift, int32_t hash,
                  PyObject *key,
                  GrailHamtNode **new_node);

static hamt_find_t
hamt_node_find(GrailHamtNode *node,
               uint32_t shift, int32_t hash,
               PyObject *key, PyObject **val);


static GrailHamtNode *
hamt_node_array_new(GrailHamtState *st, Py_ssize_t);

static GrailHamtNode *
hamt_node_collision_new(GrailHamtState *st, int32_t hash, Py_ssize_t size);

static inline Py_ssize_t
hamt_node_collision_count(GrailHamtNode_Collision *node);


#ifdef Py_DEBUG
static void
_hamt_node_array_validate(void *obj_raw)
{
    PyObject *obj = _PyObject_CAST(obj_raw);
    assert(IS_ARRAY_NODE(obj));
    GrailHamtNode_Array *node = (GrailHamtNode_Array*)obj;
    Py_ssize_t i = 0, count = 0;
    for (; i < HAMT_ARRAY_NODE_SIZE; i++) {
        if (node->a_array[i] != NULL) {
            count++;
        }
    }
    assert(count == node->a_count);
}

#define VALIDATE_ARRAY_NODE(NODE) \
    do { _hamt_node_array_validate(NODE); } while (0);
#else
#define VALIDATE_ARRAY_NODE(NODE)
#endif


/* Returns -1 on error */
static inline int32_t
hamt_hash(PyObject *o)
{
    Py_hash_t hash = PyObject_Hash(o);

#if SIZEOF_PY_HASH_T <= 4
    return hash;
#else
    if (hash == -1) {
        /* exception */
        return -1;
    }

    /* While it's somewhat suboptimal to reduce Python's 64 bit hash to
       32 bits via XOR, it seems that the resulting hash function
       is good enough (this is also how Long type is hashed in Java.)
       Storing 10, 100, 1000 Python strings results in a relatively
       shallow and uniform tree structure.

       Also it's worth noting that it would be possible to adapt the tree
       structure to 64 bit hashes, but that would increase memory pressure
       and provide little to no performance benefits for collections with
       fewer than billions of key/value pairs.

       Important: do not change this hash reducing function. There are many
       tests that need an exact tree shape to cover all code paths and
       we do that by specifying concrete values for test data's `__hash__`.
       If this function is changed most of the regression tests would
       become useless.
    */
    int32_t xored = (int32_t)(hash & 0xffffffffl) ^ (int32_t)(hash >> 32);
    return xored == -1 ? -2 : xored;
#endif
}

static inline uint32_t
hamt_mask(int32_t hash, uint32_t shift)
{
    return (((uint32_t)hash >> shift) & 0x01f);
}

static inline uint32_t
hamt_bitpos(int32_t hash, uint32_t shift)
{
    return (uint32_t)1 << hamt_mask(hash, shift);
}

static inline uint32_t
hamt_bitindex(uint32_t bitmap, uint32_t bit)
{
    return (uint32_t)grail_popcount32(bitmap & (bit - 1));
}


/////////////////////////////////// Dump Helpers
/////////////////////////////////// Bitmap Node

#define GrailHamtNode_Bitmap_CAST(op)     ((GrailHamtNode_Bitmap *)(op))


static GrailHamtNode *
hamt_node_bitmap_new(GrailHamtState *st, Py_ssize_t size)
{
    /* Create a new bitmap node of size 'size' */

    GrailHamtNode_Bitmap *node;
    Py_ssize_t i;

    if (size == 0) {
        /* Since bitmap nodes are immutable, we can cache the instance
           for size=0 and reuse it whenever we need an empty bitmap node.
        */
        return (GrailHamtNode *)Py_NewRef(st->empty_bitmap);
    }

    assert(size >= 0);
    assert(size % 2 == 0);

    /* No freelist; allocate a new bitmap node */
    node = PyObject_GC_NewVar(
        GrailHamtNode_Bitmap, st->bitmap_type, size);
    if (node == NULL) {
        return NULL;
    }

    Py_SET_SIZE(node, size);

    for (i = 0; i < size; i++) {
        node->b_array[i] = NULL;
    }

    node->b_bitmap = 0;

    PyObject_GC_Track(node);

    return (GrailHamtNode *)node;
}

static inline Py_ssize_t
hamt_node_bitmap_count(GrailHamtNode_Bitmap *node)
{
    return Py_SIZE(node) / 2;
}

static GrailHamtNode_Bitmap *
hamt_node_bitmap_clone(GrailHamtNode_Bitmap *node)
{
    GrailHamtState *st = HAMT_STATE(node);
    /* Clone a bitmap node; return a new one with the same child notes. */

    GrailHamtNode_Bitmap *clone;
    Py_ssize_t i;

    clone = (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, Py_SIZE(node));
    if (clone == NULL) {
        return NULL;
    }

    for (i = 0; i < Py_SIZE(node); i++) {
        clone->b_array[i] = Py_XNewRef(node->b_array[i]);
    }

    clone->b_bitmap = node->b_bitmap;
    return clone;
}

static GrailHamtNode_Bitmap *
hamt_node_bitmap_clone_without(GrailHamtNode_Bitmap *o, uint32_t bit)
{
    GrailHamtState *st = HAMT_STATE(o);
    assert(bit & o->b_bitmap);
    assert(hamt_node_bitmap_count(o) > 1);

    GrailHamtNode_Bitmap *new = (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, 
        Py_SIZE(o) - 2);
    if (new == NULL) {
        return NULL;
    }

    uint32_t idx = hamt_bitindex(o->b_bitmap, bit);
    uint32_t key_idx = 2 * idx;
    uint32_t val_idx = key_idx + 1;
    uint32_t i;

    for (i = 0; i < key_idx; i++) {
        new->b_array[i] = Py_XNewRef(o->b_array[i]);
    }

    assert(Py_SIZE(o) >= 0 && Py_SIZE(o) <= 32);
    for (i = val_idx + 1; i < (uint32_t)Py_SIZE(o); i++) {
        new->b_array[i - 2] = Py_XNewRef(o->b_array[i]);
    }

    new->b_bitmap = o->b_bitmap & ~bit;
    return new;
}

static GrailHamtNode *
hamt_node_new_bitmap_or_collision(GrailHamtState *st, uint32_t shift,
                                  PyObject *key1, PyObject *val1,
                                  int32_t key2_hash,
                                  PyObject *key2, PyObject *val2)
{
    /* Helper method.  Creates a new node for key1/val and key2/val2
       pairs.

       If key1 hash is equal to the hash of key2, a Collision node
       will be created.  If they are not equal, a Bitmap node is
       created.
    */

    int32_t key1_hash = hamt_hash(key1);
    if (key1_hash == -1) {
        return NULL;
    }

    if (key1_hash == key2_hash) {
        GrailHamtNode_Collision *n;
        n = (GrailHamtNode_Collision *)hamt_node_collision_new(st, key1_hash, 4);
        if (n == NULL) {
            return NULL;
        }

        n->c_array[0] = Py_NewRef(key1);
        n->c_array[1] = Py_NewRef(val1);

        n->c_array[2] = Py_NewRef(key2);
        n->c_array[3] = Py_NewRef(val2);

        return (GrailHamtNode *)n;
    }
    else {
        int added_leaf = 0;
        GrailHamtNode *n = hamt_node_bitmap_new(st, 0);
        if (n == NULL) {
            return NULL;
        }

        GrailHamtNode *n2 = hamt_node_assoc(
            n, shift, key1_hash, key1, val1, &added_leaf, 0);
        Py_DECREF(n);
        if (n2 == NULL) {
            return NULL;
        }

        n = hamt_node_assoc(n2, shift, key2_hash, key2, val2, &added_leaf, 0);
        Py_DECREF(n2);
        if (n == NULL) {
            return NULL;
        }

        return n;
    }
}

/* In-place ("transient") assoc support.

   A node may be updated in place only when the caller's whole path to it is
   exclusively owned: 'mut' says the parent is, and the node itself must be
   uniquely referenced by the current thread.  A uniquely referenced node
   below a shared parent is still reachable through every owner of that
   parent, so 'mut' is never set below a shared node.  Every in-place change
   is a single slot replacement, so the tree stays valid at each point where
   key comparison or a released value can run Python code. */
static inline int
hamt_node_is_mutable(int mut, PyObject *node)
{
    return mut && PyUnstable_Object_IsUniquelyReferenced(node);
}

static GrailHamtNode_Bitmap *
hamt_node_bitmap_clone_or_self(GrailHamtNode_Bitmap *node, int mine)
{
    if (mine) {
        return (GrailHamtNode_Bitmap *)Py_NewRef(node);
    }
    return hamt_node_bitmap_clone(node);
}

static GrailHamtNode *
hamt_node_bitmap_assoc(GrailHamtNode_Bitmap *self,
                       uint32_t shift, int32_t hash,
                       PyObject *key, PyObject *val, int* added_leaf, int mut)
{
    GrailHamtState *st = HAMT_STATE(self);
    /* assoc operation for bitmap nodes.

       Return: a new node, or self if key/val already is in the
       collection.

       'added_leaf' is later used in 'GrailHamt_Assoc' to determine if
       `hamt.set(key, val)` increased the size of the collection.
    */

    uint32_t bit = hamt_bitpos(hash, shift);
    uint32_t idx = hamt_bitindex(self->b_bitmap, bit);
    int mine = hamt_node_is_mutable(mut, (PyObject *)self);

    /* Bitmap node layout:

    +------+------+------+------+  ---  +------+------+
    | key1 | val1 | key2 | val2 |  ...  | keyN | valN |
    +------+------+------+------+  ---  +------+------+
    where `N < Py_SIZE(node)`.

    The `node->b_bitmap` field is a bitmap.  For a given
    `(shift, hash)` pair we can determine:

     - If this node has the corresponding key/val slots.
     - The index of key/val slots.
    */

    if (self->b_bitmap & bit) {
        /* The key is set in this node */

        uint32_t key_idx = 2 * idx;
        uint32_t val_idx = key_idx + 1;

        assert(val_idx < (size_t)Py_SIZE(self));

        PyObject *key_or_null = self->b_array[key_idx];
        PyObject *val_or_node = self->b_array[val_idx];

        if (key_or_null == NULL) {
            /* key is NULL.  This means that we have a few keys
               that have the same (hash, shift) pair. */

            assert(val_or_node != NULL);

            GrailHamtNode *sub_node = hamt_node_assoc(
                (GrailHamtNode *)val_or_node,
                shift + 5, hash, key, val, added_leaf, mine);
            if (sub_node == NULL) {
                return NULL;
            }

            if (val_or_node == (PyObject *)sub_node) {
                Py_DECREF(sub_node);
                return (GrailHamtNode *)Py_NewRef(self);
            }

            GrailHamtNode_Bitmap *ret = hamt_node_bitmap_clone_or_self(self, mine);
            if (ret == NULL) {
                Py_DECREF(sub_node);
                return NULL;
            }
            Py_SETREF(ret->b_array[val_idx], (PyObject*)sub_node);
            return (GrailHamtNode *)ret;
        }

        assert(key != NULL);
        /* key is not NULL.  This means that we have only one other
           key in this collection that matches our hash for this shift. */

        int comp_err = PyObject_RichCompareBool(key, key_or_null, Py_EQ);
        if (comp_err < 0) {  /* exception in __eq__ */
            return NULL;
        }
        if (comp_err == 1) {  /* key == key_or_null */
            if (val == val_or_node) {
                /* we already have the same key/val pair; return self. */
                return (GrailHamtNode *)Py_NewRef(self);
            }

            /* We're setting a new value for the key we had before.
               Make a new bitmap node with a replaced value, and return it. */
            GrailHamtNode_Bitmap *ret = hamt_node_bitmap_clone_or_self(self, mine);
            if (ret == NULL) {
                return NULL;
            }
            Py_SETREF(ret->b_array[val_idx], Py_NewRef(val));
            return (GrailHamtNode *)ret;
        }

        /* It's a new key, and it has the same index as *one* another key.
           We have a collision.  We need to create a new node which will
           combine the existing key and the key we're adding.

           `hamt_node_new_bitmap_or_collision` will either create a new
           Collision node if the keys have identical hashes, or
           a new Bitmap node.
        */
        GrailHamtNode *sub_node = hamt_node_new_bitmap_or_collision(st, 
            shift + 5,
            key_or_null, val_or_node,  /* existing key/val */
            hash,
            key, val  /* new key/val */
        );
        if (sub_node == NULL) {
            return NULL;
        }

        GrailHamtNode_Bitmap *ret = hamt_node_bitmap_clone_or_self(self, mine);
        if (ret == NULL) {
            Py_DECREF(sub_node);
            return NULL;
        }
        Py_SETREF(ret->b_array[key_idx], NULL);
        Py_SETREF(ret->b_array[val_idx], (PyObject *)sub_node);

        *added_leaf = 1;
        return (GrailHamtNode *)ret;
    }
    else {
        /* There was no key before with the same (shift,hash). */

        uint32_t n = (uint32_t)grail_popcount32(self->b_bitmap);

        if (n >= 16) {
            /* When we have a situation where we want to store more
               than 16 nodes at one level of the tree, we no longer
               want to use the Bitmap node with bitmap encoding.

               Instead we start using an Array node, which has
               simpler (faster) implementation at the expense of
               having preallocated 32 pointers for its keys/values
               pairs.

               Small hamt objects (<30 keys) usually don't have any
               Array nodes at all.  Between ~30 and ~400 keys hamt
               objects usually have one Array node, and usually it's
               a root node.
            */

            uint32_t jdx = hamt_mask(hash, shift);
            /* 'jdx' is the index of where the new key should be added
               in the new Array node we're about to create. */

            GrailHamtNode *empty = NULL;
            GrailHamtNode_Array *new_node = NULL;
            GrailHamtNode *res = NULL;

            /* Create a new Array node. */
            new_node = (GrailHamtNode_Array *)hamt_node_array_new(st, n + 1);
            if (new_node == NULL) {
                goto fin;
            }

            /* Create an empty bitmap node for the next
               hamt_node_assoc call. */
            empty = hamt_node_bitmap_new(st, 0);
            if (empty == NULL) {
                goto fin;
            }

            /* Make a new bitmap node for the key/val we're adding.
               Set that bitmap node to new-array-node[jdx]. */
            new_node->a_array[jdx] = hamt_node_assoc(
                empty, shift + 5, hash, key, val, added_leaf, 0);
            if (new_node->a_array[jdx] == NULL) {
                goto fin;
            }

            /* Copy existing key/value pairs from the current Bitmap
               node to the new Array node we've just created. */
            Py_ssize_t i, j;
            for (i = 0, j = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
                if (((self->b_bitmap >> i) & 1) != 0) {
                    /* Ensure we don't accidentally override `jdx` element
                       we set few lines above.
                    */
                    assert(new_node->a_array[i] == NULL);

                    if (self->b_array[j] == NULL) {
                        new_node->a_array[i] =
                            (GrailHamtNode *)Py_NewRef(self->b_array[j + 1]);
                    }
                    else {
                        int32_t rehash = hamt_hash(self->b_array[j]);
                        if (rehash == -1) {
                            goto fin;
                        }

                        new_node->a_array[i] = hamt_node_assoc(
                            empty, shift + 5,
                            rehash,
                            self->b_array[j],
                            self->b_array[j + 1],
                            added_leaf, 0);

                        if (new_node->a_array[i] == NULL) {
                            goto fin;
                        }
                    }
                    j += 2;
                }
            }

            VALIDATE_ARRAY_NODE(new_node)

            /* That's it! */
            res = (GrailHamtNode *)new_node;

        fin:
            Py_XDECREF(empty);
            if (res == NULL) {
                Py_XDECREF(new_node);
            }
            return res;
        }
        else {
            /* We have less than 16 keys at this level; let's just
               create a new bitmap node out of this node with the
               new key/val pair added. */

            uint32_t key_idx = 2 * idx;
            uint32_t val_idx = key_idx + 1;
            uint32_t i;

            *added_leaf = 1;

            /* Allocate new Bitmap node which can have one more key/val
               pair in addition to what we have already. */
            GrailHamtNode_Bitmap *new_node =
                (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, 2 * (n + 1));
            if (new_node == NULL) {
                return NULL;
            }

            /* Copy all keys/values that will be before the new key/value
               we are adding. */
            for (i = 0; i < key_idx; i++) {
                new_node->b_array[i] = Py_XNewRef(self->b_array[i]);
            }

            /* Set the new key/value to the new Bitmap node. */
            new_node->b_array[key_idx] = Py_NewRef(key);
            new_node->b_array[val_idx] = Py_NewRef(val);

            /* Copy all keys/values that will be after the new key/value
               we are adding. */
            assert(Py_SIZE(self) >= 0 && Py_SIZE(self) <= 32);
            for (i = key_idx; i < (uint32_t)Py_SIZE(self); i++) {
                new_node->b_array[i + 2] = Py_XNewRef(self->b_array[i]);
            }

            new_node->b_bitmap = self->b_bitmap | bit;
            return (GrailHamtNode *)new_node;
        }
    }
}

static hamt_without_t
hamt_node_bitmap_without(GrailHamtNode_Bitmap *self,
                         uint32_t shift, int32_t hash,
                         PyObject *key,
                         GrailHamtNode **new_node)
{
    uint32_t bit = hamt_bitpos(hash, shift);
    if ((self->b_bitmap & bit) == 0) {
        return W_NOT_FOUND;
    }

    uint32_t idx = hamt_bitindex(self->b_bitmap, bit);

    uint32_t key_idx = 2 * idx;
    uint32_t val_idx = key_idx + 1;

    PyObject *key_or_null = self->b_array[key_idx];
    PyObject *val_or_node = self->b_array[val_idx];

    if (key_or_null == NULL) {
        /* key == NULL means that 'value' is another tree node. */

        GrailHamtNode *sub_node = NULL;

        hamt_without_t res = hamt_node_without(
            (GrailHamtNode *)val_or_node,
            shift + 5, hash, key, &sub_node);

        switch (res) {
            case W_EMPTY:
                /* It's impossible for us to receive a W_EMPTY here:

                    - Array nodes are converted to Bitmap nodes when
                      we delete 16th item from them;

                    - Collision nodes are converted to Bitmap when
                      there is one item in them;

                    - Bitmap node's without() inlines single-item
                      sub-nodes.

                   So in no situation we can have a single-item
                   Bitmap child of another Bitmap node.
                */
                Py_UNREACHABLE();

            case W_NEWNODE: {
                assert(sub_node != NULL);

                if (IS_BITMAP_NODE(sub_node)) {
                    GrailHamtNode_Bitmap *sub_tree = (GrailHamtNode_Bitmap *)sub_node;
                    if (hamt_node_bitmap_count(sub_tree) == 1 &&
                            sub_tree->b_array[0] != NULL)
                    {
                        /* A bitmap node with one key/value pair.  Just
                           merge it into this node.

                           Note that we don't inline Bitmap nodes that
                           have a NULL key -- those nodes point to another
                           tree level, and we cannot simply move tree levels
                           up or down.
                        */

                        GrailHamtNode_Bitmap *clone = hamt_node_bitmap_clone(self);
                        if (clone == NULL) {
                            Py_DECREF(sub_node);
                            return W_ERROR;
                        }

                        PyObject *key = sub_tree->b_array[0];
                        PyObject *val = sub_tree->b_array[1];

                        Py_XSETREF(clone->b_array[key_idx], Py_NewRef(key));
                        Py_SETREF(clone->b_array[val_idx], Py_NewRef(val));

                        Py_DECREF(sub_tree);

                        *new_node = (GrailHamtNode *)clone;
                        return W_NEWNODE;
                    }
                }

#ifdef Py_DEBUG
                /* Ensure that Collision.without implementation
                   converts to Bitmap nodes itself.
                */
                if (IS_COLLISION_NODE(sub_node)) {
                    assert(hamt_node_collision_count(
                            (GrailHamtNode_Collision*)sub_node) > 1);
                }
#endif

                GrailHamtNode_Bitmap *clone = hamt_node_bitmap_clone(self);
                if (clone == NULL) {
                    Py_DECREF(sub_node);
                    return W_ERROR;
                }

                Py_SETREF(clone->b_array[val_idx],
                          (PyObject *)sub_node);  /* borrow */

                *new_node = (GrailHamtNode *)clone;
                return W_NEWNODE;
            }

            case W_ERROR:
            case W_NOT_FOUND:
                assert(sub_node == NULL);
                return res;

            default:
                Py_UNREACHABLE();
        }
    }
    else {
        /* We have a regular key/value pair */

        int cmp = PyObject_RichCompareBool(key_or_null, key, Py_EQ);
        if (cmp < 0) {
            return W_ERROR;
        }
        if (cmp == 0) {
            return W_NOT_FOUND;
        }

        if (hamt_node_bitmap_count(self) == 1) {
            return W_EMPTY;
        }

        *new_node = (GrailHamtNode *)
            hamt_node_bitmap_clone_without(self, bit);
        if (*new_node == NULL) {
            return W_ERROR;
        }

        return W_NEWNODE;
    }
}

static hamt_find_t
hamt_node_bitmap_find(GrailHamtNode_Bitmap *self,
                      uint32_t shift, int32_t hash,
                      PyObject *key, PyObject **val)
{
    /* Lookup a key in a Bitmap node. */

    uint32_t bit = hamt_bitpos(hash, shift);
    uint32_t idx;
    uint32_t key_idx;
    uint32_t val_idx;
    PyObject *key_or_null;
    PyObject *val_or_node;
    int comp_err;

    if ((self->b_bitmap & bit) == 0) {
        return F_NOT_FOUND;
    }

    idx = hamt_bitindex(self->b_bitmap, bit);
    key_idx = idx * 2;
    val_idx = key_idx + 1;

    assert(val_idx < (size_t)Py_SIZE(self));

    key_or_null = self->b_array[key_idx];
    val_or_node = self->b_array[val_idx];

    if (key_or_null == NULL) {
        /* There are a few keys that have the same hash at the current shift
           that match our key.  Dispatch the lookup further down the tree. */
        assert(val_or_node != NULL);
        return hamt_node_find((GrailHamtNode *)val_or_node,
                              shift + 5, hash, key, val);
    }

    /* We have only one key -- a potential match.  Let's compare if the
       key we are looking at is equal to the key we are looking for. */
    assert(key != NULL);
    comp_err = PyObject_RichCompareBool(key, key_or_null, Py_EQ);
    if (comp_err < 0) {  /* exception in __eq__ */
        return F_ERROR;
    }
    if (comp_err == 1) {  /* key == key_or_null */
        *val = val_or_node;
        return F_FOUND;
    }

    return F_NOT_FOUND;
}

static int
hamt_node_bitmap_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    /* Bitmap's tp_traverse */
    GrailHamtNode_Bitmap *self = GrailHamtNode_Bitmap_CAST(op);
    for (Py_ssize_t i = Py_SIZE(self); --i >= 0;) {
        Py_VISIT(self->b_array[i]);
    }
    return 0;
}

static void
hamt_node_bitmap_dealloc(PyObject *self)
{
    /* Bitmap's tp_dealloc */

    GrailHamtNode_Bitmap *node = GrailHamtNode_Bitmap_CAST(self);
    Py_ssize_t i, len = Py_SIZE(self);

    PyObject_GC_UnTrack(self);

    if (len > 0) {
        i = len;
        while (--i >= 0) {
            Py_XDECREF(node->b_array[i]);
        }
    }

    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    GRAIL_DECREF_TYPE(tp);
}



/////////////////////////////////// Collision Node


static GrailHamtNode *
hamt_node_collision_new(GrailHamtState *st, int32_t hash, Py_ssize_t size)
{
    /* Create a new Collision node. */

    GrailHamtNode_Collision *node;
    Py_ssize_t i;

    assert(size >= 4);
    assert(size % 2 == 0);

    node = PyObject_GC_NewVar(
        GrailHamtNode_Collision, st->collision_type, size);
    if (node == NULL) {
        return NULL;
    }

    for (i = 0; i < size; i++) {
        node->c_array[i] = NULL;
    }

    Py_SET_SIZE(node, size);
    node->c_hash = hash;

    PyObject_GC_Track(node);

    return (GrailHamtNode *)node;
}

static hamt_find_t
hamt_node_collision_find_index(GrailHamtNode_Collision *self, PyObject *key,
                               Py_ssize_t *idx)
{
    /* Lookup `key` in the Collision node `self`.  Set the index of the
       found key to 'idx'. */

    Py_ssize_t i;
    PyObject *el;

    for (i = 0; i < Py_SIZE(self); i += 2) {
        el = self->c_array[i];

        assert(el != NULL);
        int cmp = PyObject_RichCompareBool(key, el, Py_EQ);
        if (cmp < 0) {
            return F_ERROR;
        }
        if (cmp == 1) {
            *idx = i;
            return F_FOUND;
        }
    }

    return F_NOT_FOUND;
}

static GrailHamtNode *
hamt_node_collision_assoc(GrailHamtNode_Collision *self,
                          uint32_t shift, int32_t hash,
                          PyObject *key, PyObject *val, int* added_leaf, int mut)
{
    GrailHamtState *st = HAMT_STATE(self);
    /* Set a new key to this level (currently a Collision node)
       of the tree. */

    if (hash == self->c_hash) {
        /* The hash of the 'key' we are adding matches the hash of
           other keys in this Collision node. */

        Py_ssize_t key_idx = -1;
        hamt_find_t found;
        GrailHamtNode_Collision *new_node;
        Py_ssize_t i;

        /* Let's try to lookup the new 'key', maybe we already have it. */
        found = hamt_node_collision_find_index(self, key, &key_idx);
        switch (found) {
            case F_ERROR:
                /* Exception. */
                return NULL;

            case F_NOT_FOUND:
                /* This is a totally new key.  Clone the current node,
                   add a new key/value to the cloned node. */

                new_node = (GrailHamtNode_Collision *)hamt_node_collision_new(st, 
                    self->c_hash, Py_SIZE(self) + 2);
                if (new_node == NULL) {
                    return NULL;
                }

                for (i = 0; i < Py_SIZE(self); i++) {
                    new_node->c_array[i] = Py_NewRef(self->c_array[i]);
                }

                new_node->c_array[i] = Py_NewRef(key);
                new_node->c_array[i + 1] = Py_NewRef(val);

                *added_leaf = 1;
                return (GrailHamtNode *)new_node;

            case F_FOUND:
                /* There's a key which is equal to the key we are adding. */

                assert(key_idx >= 0);
                assert(key_idx < Py_SIZE(self));
                Py_ssize_t val_idx = key_idx + 1;

                if (self->c_array[val_idx] == val) {
                    /* We're setting a key/value pair that's already set. */
                    return (GrailHamtNode *)Py_NewRef(self);
                }

                if (hamt_node_is_mutable(mut, (PyObject *)self)) {
                    Py_SETREF(self->c_array[val_idx], Py_NewRef(val));
                    return (GrailHamtNode *)Py_NewRef(self);
                }

                /* We need to replace old value for the key
                   with a new value.  Create a new Collision node.*/
                new_node = (GrailHamtNode_Collision *)hamt_node_collision_new(st, 
                    self->c_hash, Py_SIZE(self));
                if (new_node == NULL) {
                    return NULL;
                }

                /* Copy all elements of the old node to the new one. */
                for (i = 0; i < Py_SIZE(self); i++) {
                    new_node->c_array[i] = Py_NewRef(self->c_array[i]);
                }

                /* Replace the old value with the new value for the our key. */
                Py_SETREF(new_node->c_array[val_idx], Py_NewRef(val));

                return (GrailHamtNode *)new_node;

            default:
                Py_UNREACHABLE();
        }
    }
    else {
        /* The hash of the new key is different from the hash that
           all keys of this Collision node have.

           Create a Bitmap node inplace with two children:
           key/value pair that we're adding, and the Collision node
           we're replacing on this tree level.
        */

        GrailHamtNode_Bitmap *new_node;
        GrailHamtNode *assoc_res;

        new_node = (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, 2);
        if (new_node == NULL) {
            return NULL;
        }
        new_node->b_bitmap = hamt_bitpos(self->c_hash, shift);
        new_node->b_array[1] = Py_NewRef(self);

        assoc_res = hamt_node_bitmap_assoc(
            new_node, shift, hash, key, val, added_leaf, 0);
        Py_DECREF(new_node);
        return assoc_res;
    }
}

static inline Py_ssize_t
hamt_node_collision_count(GrailHamtNode_Collision *node)
{
    return Py_SIZE(node) / 2;
}

static hamt_without_t
hamt_node_collision_without(GrailHamtNode_Collision *self,
                            uint32_t shift, int32_t hash,
                            PyObject *key,
                            GrailHamtNode **new_node)
{
    GrailHamtState *st = HAMT_STATE(self);
    if (hash != self->c_hash) {
        return W_NOT_FOUND;
    }

    Py_ssize_t key_idx = -1;
    hamt_find_t found = hamt_node_collision_find_index(self, key, &key_idx);

    switch (found) {
        case F_ERROR:
            return W_ERROR;

        case F_NOT_FOUND:
            return W_NOT_FOUND;

        case F_FOUND:
            assert(key_idx >= 0);
            assert(key_idx < Py_SIZE(self));

            Py_ssize_t new_count = hamt_node_collision_count(self) - 1;

            if (new_count == 0) {
                /* The node has only one key/value pair and it's for the
                   key we're trying to delete.  So a new node will be empty
                   after the removal.
                */
                return W_EMPTY;
            }

            if (new_count == 1) {
                /* The node has two keys, and after deletion the
                   new Collision node would have one.  Collision nodes
                   with one key shouldn't exist, so convert it to a
                   Bitmap node.
                */
                GrailHamtNode_Bitmap *node = (GrailHamtNode_Bitmap *)
                    hamt_node_bitmap_new(st, 2);
                if (node == NULL) {
                    return W_ERROR;
                }

                if (key_idx == 0) {
                    node->b_array[0] = Py_NewRef(self->c_array[2]);
                    node->b_array[1] = Py_NewRef(self->c_array[3]);
                }
                else {
                    assert(key_idx == 2);
                    node->b_array[0] = Py_NewRef(self->c_array[0]);
                    node->b_array[1] = Py_NewRef(self->c_array[1]);
                }

                node->b_bitmap = hamt_bitpos(hash, shift);

                *new_node = (GrailHamtNode *)node;
                return W_NEWNODE;
            }

            /* Allocate a new Collision node with capacity for one
               less key/value pair */
            GrailHamtNode_Collision *new = (GrailHamtNode_Collision *)
                hamt_node_collision_new(st, 
                    self->c_hash, Py_SIZE(self) - 2);
            if (new == NULL) {
                return W_ERROR;
            }

            /* Copy all other keys from `self` to `new` */
            Py_ssize_t i;
            for (i = 0; i < key_idx; i++) {
                new->c_array[i] = Py_NewRef(self->c_array[i]);
            }
            for (i = key_idx + 2; i < Py_SIZE(self); i++) {
                new->c_array[i - 2] = Py_NewRef(self->c_array[i]);
            }

            *new_node = (GrailHamtNode*)new;
            return W_NEWNODE;

        default:
            Py_UNREACHABLE();
    }
}

static hamt_find_t
hamt_node_collision_find(GrailHamtNode_Collision *self,
                         uint32_t shift, int32_t hash,
                         PyObject *key, PyObject **val)
{
    /* Lookup `key` in the Collision node `self`.  Set the value
       for the found key to 'val'. */

    Py_ssize_t idx = -1;
    hamt_find_t res;

    res = hamt_node_collision_find_index(self, key, &idx);
    if (res == F_ERROR || res == F_NOT_FOUND) {
        return res;
    }

    assert(idx >= 0);
    assert(idx + 1 < Py_SIZE(self));

    *val = self->c_array[idx + 1];
    assert(*val != NULL);

    return F_FOUND;
}


static int
hamt_node_collision_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    /* Collision's tp_traverse */
    GrailHamtNode_Collision *self = GrailHamtNode_Collision_CAST(op);
    for (Py_ssize_t i = Py_SIZE(self); --i >= 0; ) {
        Py_VISIT(self->c_array[i]);
    }
    return 0;
}

static void
hamt_node_collision_dealloc(PyObject *self)
{
    /* Collision's tp_dealloc */
    Py_ssize_t len = Py_SIZE(self);
    PyObject_GC_UnTrack(self);
    if (len > 0) {
        GrailHamtNode_Collision *node = GrailHamtNode_Collision_CAST(self);
        while (--len >= 0) {
            Py_XDECREF(node->c_array[len]);
        }
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    GRAIL_DECREF_TYPE(tp);
}



/////////////////////////////////// Array Node


static GrailHamtNode *
hamt_node_array_new(GrailHamtState *st, Py_ssize_t count)
{
    Py_ssize_t i;

    GrailHamtNode_Array *node = PyObject_GC_New(
        GrailHamtNode_Array, st->array_type);
    if (node == NULL) {
        return NULL;
    }

    for (i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
        node->a_array[i] = NULL;
    }

    node->a_count = count;

    PyObject_GC_Track(node);
    return (GrailHamtNode *)node;
}

static GrailHamtNode_Array *
hamt_node_array_clone(GrailHamtNode_Array *node)
{
    GrailHamtState *st = HAMT_STATE(node);
    GrailHamtNode_Array *clone;
    Py_ssize_t i;

    VALIDATE_ARRAY_NODE(node)

    /* Create a new Array node. */
    clone = (GrailHamtNode_Array *)hamt_node_array_new(st, node->a_count);
    if (clone == NULL) {
        return NULL;
    }

    /* Copy all elements from the current Array node to the new one. */
    for (i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
        clone->a_array[i] = (GrailHamtNode*)Py_XNewRef(node->a_array[i]);
    }

    VALIDATE_ARRAY_NODE(clone)
    return clone;
}

static GrailHamtNode *
hamt_node_array_assoc(GrailHamtNode_Array *self,
                      uint32_t shift, int32_t hash,
                      PyObject *key, PyObject *val, int* added_leaf, int mut)
{
    GrailHamtState *st = HAMT_STATE(self);
    /* Set a new key to this level (currently a Collision node)
       of the tree.

       Array nodes don't store values, they can only point to
       other nodes.  They are simple arrays of 32 BaseNode pointers/
     */

    uint32_t idx = hamt_mask(hash, shift);
    GrailHamtNode *node = self->a_array[idx];
    GrailHamtNode *child_node;
    GrailHamtNode_Array *new_node;
    Py_ssize_t i;

    if (node == NULL) {
        /* There's no child node for the given hash.  Create a new
           Bitmap node for this key. */

        GrailHamtNode_Bitmap *empty = NULL;

        /* Get an empty Bitmap node to work with. */
        empty = (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, 0);
        if (empty == NULL) {
            return NULL;
        }

        /* Set key/val to the newly created empty Bitmap, thus
           creating a new Bitmap node with our key/value pair. */
        child_node = hamt_node_bitmap_assoc(
            empty,
            shift + 5, hash, key, val, added_leaf, 0);
        Py_DECREF(empty);
        if (child_node == NULL) {
            return NULL;
        }

        if (hamt_node_is_mutable(mut, (PyObject *)self)) {
            self->a_array[idx] = child_node;  /* borrow */
            self->a_count++;
            VALIDATE_ARRAY_NODE(self)
            return (GrailHamtNode *)Py_NewRef(self);
        }

        /* Create a new Array node. */
        new_node = (GrailHamtNode_Array *)hamt_node_array_new(st, self->a_count + 1);
        if (new_node == NULL) {
            Py_DECREF(child_node);
            return NULL;
        }

        /* Copy all elements from the current Array node to the
           new one. */
        for (i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
            new_node->a_array[i] = (GrailHamtNode*)Py_XNewRef(self->a_array[i]);
        }

        assert(new_node->a_array[idx] == NULL);
        new_node->a_array[idx] = child_node;  /* borrow */
        VALIDATE_ARRAY_NODE(new_node)
    }
    else {
        /* There's a child node for the given hash.
           Set the key to it./ */
        int mine = hamt_node_is_mutable(mut, (PyObject *)self);
        child_node = hamt_node_assoc(
            node, shift + 5, hash, key, val, added_leaf, mine);
        if (child_node == NULL) {
            return NULL;
        }
        else if (child_node == node) {
            /* Unchanged, or updated in place. */
            Py_DECREF(child_node);
            return (GrailHamtNode *)Py_NewRef(self);
        }

        new_node = mine ? (GrailHamtNode_Array *)Py_NewRef(self)
                        : hamt_node_array_clone(self);
        if (new_node == NULL) {
            Py_DECREF(child_node);
            return NULL;
        }

        Py_SETREF(new_node->a_array[idx], child_node);  /* borrow */
        VALIDATE_ARRAY_NODE(new_node)
    }

    return (GrailHamtNode *)new_node;
}

static hamt_without_t
hamt_node_array_without(GrailHamtNode_Array *self,
                        uint32_t shift, int32_t hash,
                        PyObject *key,
                        GrailHamtNode **new_node)
{
    GrailHamtState *st = HAMT_STATE(self);
    uint32_t idx = hamt_mask(hash, shift);
    GrailHamtNode *node = self->a_array[idx];

    if (node == NULL) {
        return W_NOT_FOUND;
    }

    GrailHamtNode *sub_node = NULL;
    hamt_without_t res = hamt_node_without(
        (GrailHamtNode *)node,
        shift + 5, hash, key, &sub_node);

    switch (res) {
        case W_NOT_FOUND:
        case W_ERROR:
            assert(sub_node == NULL);
            return res;

        case W_NEWNODE: {
            /* We need to replace a node at the `idx` index.
               Clone this node and replace.
            */
            assert(sub_node != NULL);

            GrailHamtNode_Array *clone = hamt_node_array_clone(self);
            if (clone == NULL) {
                Py_DECREF(sub_node);
                return W_ERROR;
            }

            Py_SETREF(clone->a_array[idx], sub_node);  /* borrow */
            *new_node = (GrailHamtNode*)clone;  /* borrow */
            return W_NEWNODE;
        }

        case W_EMPTY: {
            assert(sub_node == NULL);
            /* We need to remove a node at the `idx` index.
               Calculate the size of the replacement Array node.
            */
            Py_ssize_t new_count = self->a_count - 1;

            if (new_count == 0) {
                return W_EMPTY;
            }

            if (new_count >= 16) {
                /* We convert Bitmap nodes to Array nodes, when a
                   Bitmap node needs to store more than 15 key/value
                   pairs.  So we will create a new Array node if we
                   the number of key/values after deletion is still
                   greater than 15.
                */

                GrailHamtNode_Array *new = hamt_node_array_clone(self);
                if (new == NULL) {
                    return W_ERROR;
                }
                new->a_count = new_count;
                Py_CLEAR(new->a_array[idx]);

                *new_node = (GrailHamtNode*)new;  /* borrow */
                return W_NEWNODE;
            }

            /* New Array node would have less than 16 key/value
               pairs.  We need to create a replacement Bitmap node. */

            Py_ssize_t bitmap_size = new_count * 2;
            uint32_t bitmap = 0;

            GrailHamtNode_Bitmap *new = (GrailHamtNode_Bitmap *)
                hamt_node_bitmap_new(st, bitmap_size);
            if (new == NULL) {
                return W_ERROR;
            }

            Py_ssize_t new_i = 0;
            for (uint32_t i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
                if (i == idx) {
                    /* Skip the node we are deleting. */
                    continue;
                }

                GrailHamtNode *node = self->a_array[i];
                if (node == NULL) {
                    /* Skip any missing nodes. */
                    continue;
                }

                bitmap |= 1U << i;

                if (IS_BITMAP_NODE(node)) {
                    GrailHamtNode_Bitmap *child = (GrailHamtNode_Bitmap *)node;

                    if (hamt_node_bitmap_count(child) == 1 &&
                            child->b_array[0] != NULL)
                    {
                        /* node is a Bitmap with one key/value pair, just
                           merge it into the new Bitmap node we're building.

                           Note that we don't inline Bitmap nodes that
                           have a NULL key -- those nodes point to another
                           tree level, and we cannot simply move tree levels
                           up or down.
                        */
                        PyObject *key = child->b_array[0];
                        PyObject *val = child->b_array[1];

                        new->b_array[new_i] = Py_NewRef(key);
                        new->b_array[new_i + 1] = Py_NewRef(val);
                    }
                    else {
                        new->b_array[new_i] = NULL;
                        new->b_array[new_i + 1] = Py_NewRef(node);
                    }
                }
                else {

#ifdef Py_DEBUG
                    if (IS_COLLISION_NODE(node)) {
                        Py_ssize_t child_count = hamt_node_collision_count(
                            (GrailHamtNode_Collision*)node);
                        assert(child_count > 1);
                    }
                    else if (IS_ARRAY_NODE(node)) {
                        assert(((GrailHamtNode_Array*)node)->a_count >= 16);
                    }
#endif

                    /* Just copy the node into our new Bitmap */
                    new->b_array[new_i] = NULL;
                    new->b_array[new_i + 1] = Py_NewRef(node);
                }

                new_i += 2;
            }

            new->b_bitmap = bitmap;
            *new_node = (GrailHamtNode*)new;  /* borrow */
            return W_NEWNODE;
        }

        default:
            Py_UNREACHABLE();
    }
}

static hamt_find_t
hamt_node_array_find(GrailHamtNode_Array *self,
                     uint32_t shift, int32_t hash,
                     PyObject *key, PyObject **val)
{
    /* Lookup `key` in the Array node `self`.  Set the value
       for the found key to 'val'. */

    uint32_t idx = hamt_mask(hash, shift);
    GrailHamtNode *node;

    node = self->a_array[idx];
    if (node == NULL) {
        return F_NOT_FOUND;
    }

    /* Dispatch to the generic hamt_node_find */
    return hamt_node_find(node, shift + 5, hash, key, val);
}

static int
hamt_node_array_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(op));
    /* Array's tp_traverse */
    GrailHamtNode_Array *self = GrailHamtNode_Array_CAST(op);
    for (Py_ssize_t i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
        Py_VISIT(self->a_array[i]);
    }
    return 0;
}

static void
hamt_node_array_dealloc(PyObject *self)
{
    /* Array's tp_dealloc */
    PyObject_GC_UnTrack(self);
    GrailHamtNode_Array *obj = GrailHamtNode_Array_CAST(self);
    for (Py_ssize_t i = 0; i < HAMT_ARRAY_NODE_SIZE; i++) {
        Py_XDECREF(obj->a_array[i]);
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    GRAIL_DECREF_TYPE(tp);
}



/////////////////////////////////// Node Dispatch


static GrailHamtNode *
hamt_node_assoc(GrailHamtNode *node,
                uint32_t shift, int32_t hash,
                PyObject *key, PyObject *val, int* added_leaf, int mut)
{
    /* Set key/value to the 'node' starting with the given shift/hash.
       Return a new node, or the same node if key/value already
       set.

       added_leaf will be set to 1 if key/value wasn't in the
       tree before.

       This method automatically dispatches to the suitable
       hamt_node_{nodetype}_assoc method.
    */

    if (IS_BITMAP_NODE(node)) {
        return hamt_node_bitmap_assoc(
            (GrailHamtNode_Bitmap *)node,
            shift, hash, key, val, added_leaf, mut);
    }
    else if (IS_ARRAY_NODE(node)) {
        return hamt_node_array_assoc(
            (GrailHamtNode_Array *)node,
            shift, hash, key, val, added_leaf, mut);
    }
    else {
        assert(IS_COLLISION_NODE(node));
        return hamt_node_collision_assoc(
            (GrailHamtNode_Collision *)node,
            shift, hash, key, val, added_leaf, mut);
    }
}

static hamt_without_t
hamt_node_without(GrailHamtNode *node,
                  uint32_t shift, int32_t hash,
                  PyObject *key,
                  GrailHamtNode **new_node)
{
    if (IS_BITMAP_NODE(node)) {
        return hamt_node_bitmap_without(
            (GrailHamtNode_Bitmap *)node,
            shift, hash, key,
            new_node);
    }
    else if (IS_ARRAY_NODE(node)) {
        return hamt_node_array_without(
            (GrailHamtNode_Array *)node,
            shift, hash, key,
            new_node);
    }
    else {
        assert(IS_COLLISION_NODE(node));
        return hamt_node_collision_without(
            (GrailHamtNode_Collision *)node,
            shift, hash, key,
            new_node);
    }
}

static hamt_find_t
hamt_node_find(GrailHamtNode *node,
               uint32_t shift, int32_t hash,
               PyObject *key, PyObject **val)
{
    /* Find the key in the node starting with the given shift/hash.

       If a value is found, the result will be set to F_FOUND, and
       *val will point to the found value object.

       If a value wasn't found, the result will be set to F_NOT_FOUND.

       If an exception occurs during the call, the result will be F_ERROR.

       This method automatically dispatches to the suitable
       hamt_node_{nodetype}_find method.
    */

    if (IS_BITMAP_NODE(node)) {
        return hamt_node_bitmap_find(
            (GrailHamtNode_Bitmap *)node,
            shift, hash, key, val);

    }
    else if (IS_ARRAY_NODE(node)) {
        return hamt_node_array_find(
            (GrailHamtNode_Array *)node,
            shift, hash, key, val);
    }
    else {
        assert(IS_COLLISION_NODE(node));
        return hamt_node_collision_find(
            (GrailHamtNode_Collision *)node,
            shift, hash, key, val);
    }
}



/////////////////////////////////// Iterators: Machinery


static hamt_iter_t
hamt_iterator_next(GrailHamtIteratorState *iter, PyObject **key, PyObject **val);


static void
hamt_iterator_init(GrailHamtIteratorState *iter, GrailHamtNode *root)
{
    for (uint32_t i = 0; i < GRAIL_HAMT_MAX_TREE_DEPTH; i++) {
        iter->i_nodes[i] = NULL;
        iter->i_pos[i] = 0;
    }

    iter->i_level = 0;

    /* Note: we don't incref/decref nodes in i_nodes. */
    iter->i_nodes[0] = root;
}

static hamt_iter_t
hamt_iterator_bitmap_next(GrailHamtIteratorState *iter,
                          PyObject **key, PyObject **val)
{
    int8_t level = iter->i_level;

    GrailHamtNode_Bitmap *node = (GrailHamtNode_Bitmap *)(iter->i_nodes[level]);
    Py_ssize_t pos = iter->i_pos[level];

    if (pos + 1 >= Py_SIZE(node)) {
#ifdef Py_DEBUG
        assert(iter->i_level >= 0);
        iter->i_nodes[iter->i_level] = NULL;
#endif
        iter->i_level--;
        return hamt_iterator_next(iter, key, val);
    }

    if (node->b_array[pos] == NULL) {
        iter->i_pos[level] = pos + 2;

        int8_t next_level = level + 1;
        assert(next_level < GRAIL_HAMT_MAX_TREE_DEPTH);
        iter->i_level = next_level;
        iter->i_pos[next_level] = 0;
        iter->i_nodes[next_level] = (GrailHamtNode *)
            node->b_array[pos + 1];

        return hamt_iterator_next(iter, key, val);
    }

    *key = node->b_array[pos];
    *val = node->b_array[pos + 1];
    iter->i_pos[level] = pos + 2;
    return I_ITEM;
}

static hamt_iter_t
hamt_iterator_collision_next(GrailHamtIteratorState *iter,
                             PyObject **key, PyObject **val)
{
    int8_t level = iter->i_level;

    GrailHamtNode_Collision *node = (GrailHamtNode_Collision *)(iter->i_nodes[level]);
    Py_ssize_t pos = iter->i_pos[level];

    if (pos + 1 >= Py_SIZE(node)) {
#ifdef Py_DEBUG
        assert(iter->i_level >= 0);
        iter->i_nodes[iter->i_level] = NULL;
#endif
        iter->i_level--;
        return hamt_iterator_next(iter, key, val);
    }

    *key = node->c_array[pos];
    *val = node->c_array[pos + 1];
    iter->i_pos[level] = pos + 2;
    return I_ITEM;
}

static hamt_iter_t
hamt_iterator_array_next(GrailHamtIteratorState *iter,
                         PyObject **key, PyObject **val)
{
    int8_t level = iter->i_level;

    GrailHamtNode_Array *node = (GrailHamtNode_Array *)(iter->i_nodes[level]);
    Py_ssize_t pos = iter->i_pos[level];

    if (pos >= HAMT_ARRAY_NODE_SIZE) {
#ifdef Py_DEBUG
        assert(iter->i_level >= 0);
        iter->i_nodes[iter->i_level] = NULL;
#endif
        iter->i_level--;
        return hamt_iterator_next(iter, key, val);
    }

    for (Py_ssize_t i = pos; i < HAMT_ARRAY_NODE_SIZE; i++) {
        if (node->a_array[i] != NULL) {
            iter->i_pos[level] = i + 1;

            int8_t next_level = level + 1;
            assert(next_level < GRAIL_HAMT_MAX_TREE_DEPTH);
            iter->i_pos[next_level] = 0;
            iter->i_nodes[next_level] = node->a_array[i];
            iter->i_level = next_level;

            return hamt_iterator_next(iter, key, val);
        }
    }

#ifdef Py_DEBUG
        assert(iter->i_level >= 0);
        iter->i_nodes[iter->i_level] = NULL;
#endif

    iter->i_level--;
    return hamt_iterator_next(iter, key, val);
}

static hamt_iter_t
hamt_iterator_next(GrailHamtIteratorState *iter, PyObject **key, PyObject **val)
{
    if (iter->i_level < 0) {
        return I_END;
    }

    assert(iter->i_level < GRAIL_HAMT_MAX_TREE_DEPTH);

    GrailHamtNode *current = iter->i_nodes[iter->i_level];

    if (IS_BITMAP_NODE(current)) {
        return hamt_iterator_bitmap_next(iter, key, val);
    }
    else if (IS_ARRAY_NODE(current)) {
        return hamt_iterator_array_next(iter, key, val);
    }
    else {
        assert(IS_COLLISION_NODE(current));
        return hamt_iterator_collision_next(iter, key, val);
    }
}


/////////////////////////////////// HAMT high-level functions


GrailHamtObject *
GrailHamt_Assoc(GrailHamtObject *o, PyObject *key, PyObject *val)
{
    GrailHamtState *st = HAMT_STATE(o);
    int32_t key_hash;
    int added_leaf = 0;
    GrailHamtNode *new_root;
    GrailHamtObject *new_o;

    key_hash = hamt_hash(key);
    if (key_hash == -1) {
        return NULL;
    }

    new_root = hamt_node_assoc(
        (GrailHamtNode *)(o->h_root),
        0, key_hash, key, val, &added_leaf, 0);
    if (new_root == NULL) {
        return NULL;
    }

    if (new_root == o->h_root) {
        Py_DECREF(new_root);
        return (GrailHamtObject*)Py_NewRef(o);
    }

    new_o = hamt_alloc(st);
    if (new_o == NULL) {
        Py_DECREF(new_root);
        return NULL;
    }

    new_o->h_root = new_root;  /* borrow */
    new_o->h_count = added_leaf ? o->h_count + 1 : o->h_count;

    return new_o;
}

/* Bulk construction.

   GrailHamt_FromItems builds a HAMT bottom-up.  Entries are sorted once by
   their hash chunks in level order, so every node's entries are contiguous
   and each node is allocated at its final size.  The result has the shape
   that inserting the same entries one at a time can produce: a slot with one
   entry holds it inline; a slot whose entries all have the same hash holds a
   collision node; any other slot holds a sub-node; a node with more than 16
   slots is an array node, whose children are always nodes. */

typedef struct {
    uint32_t order;     /* hash chunks, level 0 most significant */
    int32_t hash;
    PyObject *key;      /* borrowed from the caller */
    PyObject *val;
} hamt_bulk_entry;

static uint32_t
hamt_bulk_order(int32_t hash)
{
    uint32_t order = 0;
    for (uint32_t shift = 0; shift < 30; shift += 5) {
        order |= hamt_mask(hash, shift) << (27 - shift);
    }
    return order | hamt_mask(hash, 30);
}

static int
hamt_bulk_sort(hamt_bulk_entry *e, Py_ssize_t n)
{
    /* Stable LSD radix sort, so equal keys keep their input order. */
    hamt_bulk_entry *tmp = PyMem_New(hamt_bulk_entry, n);
    if (tmp == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    hamt_bulk_entry *src = e, *dst = tmp;
    for (int pass = 0; pass < 4; pass++) {
        Py_ssize_t counts[257] = {0};
        int bits = pass * 8;
        for (Py_ssize_t i = 0; i < n; i++) {
            counts[((src[i].order >> bits) & 0xff) + 1]++;
        }
        for (int b = 0; b < 256; b++) {
            counts[b + 1] += counts[b];
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            dst[counts[(src[i].order >> bits) & 0xff]++] = src[i];
        }
        hamt_bulk_entry *swap = src;
        src = dst;
        dst = swap;
    }
    /* An even number of passes leaves the result in e. */
    assert(src == e);
    PyMem_Free(tmp);
    return 0;
}

static GrailHamtNode *
hamt_bulk_node(GrailHamtState *st, hamt_bulk_entry *e, Py_ssize_t n, uint32_t shift,
               Py_ssize_t *count);

/* Fill one slot of a bitmap node from entries that share its chunk. */
static int
hamt_bulk_slot(GrailHamtState *st, hamt_bulk_entry *e, Py_ssize_t n, uint32_t shift,
               PyObject **key_slot, PyObject **val_slot, Py_ssize_t *count)
{
    int same = 1;
    for (Py_ssize_t i = 1; i < n && same; i++) {
        same = e[i].hash == e[0].hash;
    }
    if (!same) {
        assert(shift < 30);
        GrailHamtNode *sub = hamt_bulk_node(st, e, n, shift + 5, count);
        if (sub == NULL) {
            return -1;
        }
        *val_slot = (PyObject *)sub;
        return 0;
    }

    /* Equal hashes: merge equal keys as repeated assoc would, keeping the
       first key object and the last value. */
    Py_ssize_t kept = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_ssize_t j = 0;
        for (; j < kept; j++) {
            int eq = PyObject_RichCompareBool(e[i].key, e[j].key, Py_EQ);
            if (eq < 0) {
                return -1;
            }
            if (eq) {
                e[j].val = e[i].val;
                break;
            }
        }
        if (j == kept) {
            e[kept++] = e[i];
        }
    }
    *count += kept;
    if (kept == 1) {
        *key_slot = Py_NewRef(e[0].key);
        *val_slot = Py_NewRef(e[0].val);
        return 0;
    }
    GrailHamtNode_Collision *node = (GrailHamtNode_Collision *)
        hamt_node_collision_new(st, e[0].hash, 2 * kept);
    if (node == NULL) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < kept; i++) {
        node->c_array[2 * i] = Py_NewRef(e[i].key);
        node->c_array[2 * i + 1] = Py_NewRef(e[i].val);
    }
    *val_slot = (PyObject *)node;
    return 0;
}

static GrailHamtNode *
hamt_bulk_node(GrailHamtState *st, hamt_bulk_entry *e, Py_ssize_t n, uint32_t shift,
               Py_ssize_t *count)
{
    assert(n >= 1 && shift <= 30);
    Py_ssize_t slots = 1;
    for (Py_ssize_t i = 1; i < n; i++) {
        slots += hamt_mask(e[i].hash, shift) != hamt_mask(e[i - 1].hash, shift);
    }

    if (slots > 16) {
        GrailHamtNode_Array *node = (GrailHamtNode_Array *)hamt_node_array_new(st, slots);
        if (node == NULL) {
            return NULL;
        }
        for (Py_ssize_t a = 0, b; a < n; a = b) {
            uint32_t idx = hamt_mask(e[a].hash, shift);
            for (b = a + 1; b < n && hamt_mask(e[b].hash, shift) == idx; b++) {
            }
            GrailHamtNode *child = hamt_bulk_node(st, e + a, b - a, shift + 5, count);
            if (child == NULL) {
                Py_DECREF(node);
                return NULL;
            }
            node->a_array[idx] = child;
        }
        VALIDATE_ARRAY_NODE(node)
        return (GrailHamtNode *)node;
    }

    GrailHamtNode_Bitmap *node = (GrailHamtNode_Bitmap *)hamt_node_bitmap_new(st, 2 * slots);
    if (node == NULL) {
        return NULL;
    }
    Py_ssize_t j = 0;
    for (Py_ssize_t a = 0, b; a < n; a = b) {
        uint32_t idx = hamt_mask(e[a].hash, shift);
        for (b = a + 1; b < n && hamt_mask(e[b].hash, shift) == idx; b++) {
        }
        node->b_bitmap |= (uint32_t)1 << idx;
        if (b - a == 1) {
            node->b_array[j] = Py_NewRef(e[a].key);
            node->b_array[j + 1] = Py_NewRef(e[a].val);
            (*count)++;
        }
        else if (hamt_bulk_slot(st, e + a, b - a, shift,
                                &node->b_array[j], &node->b_array[j + 1],
                                count) < 0) {
            Py_DECREF(node);
            return NULL;
        }
        j += 2;
    }
    return (GrailHamtNode *)node;
}

GrailHamtObject *
GrailHamt_FromItems(GrailHamtState *st, PyObject *const *keys, PyObject *const *vals, Py_ssize_t n)
{
    if (n == 0) {
        return GrailHamt_New(st);
    }
    hamt_bulk_entry *e = PyMem_New(hamt_bulk_entry, n);
    if (e == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        int32_t hash = hamt_hash(keys[i]);
        if (hash == -1) {
            PyMem_Free(e);
            return NULL;
        }
        e[i].order = hamt_bulk_order(hash);
        e[i].hash = hash;
        e[i].key = keys[i];
        e[i].val = vals[i];
    }
    GrailHamtObject *o = NULL;
    Py_ssize_t count = 0;
    if (hamt_bulk_sort(e, n) == 0) {
        GrailHamtNode *root = hamt_bulk_node(st, e, n, 0, &count);
        if (root != NULL) {
            o = hamt_alloc(st);
            if (o == NULL) {
                Py_DECREF(root);
            }
            else {
                o->h_root = root;
                o->h_count = count;
            }
        }
    }
    PyMem_Free(e);
    return o;
}

int
GrailHamt_AssocInPlace(GrailHamtObject **po, PyObject *key, PyObject *val)
{
    GrailHamtState *st = HAMT_STATE(*po);
    GrailHamtObject *o = *po;
    int added_leaf = 0;
    int32_t key_hash = hamt_hash(key);
    if (key_hash == -1) {
        return -1;
    }

    /* The caller's reference in *po is the only one: nothing else can
       observe the tree, so exclusively owned nodes may change in place. */
    int mut = PyUnstable_Object_IsUniquelyReferenced((PyObject *)o);
    GrailHamtNode *new_root = hamt_node_assoc(
        (GrailHamtNode *)(o->h_root),
        0, key_hash, key, val, &added_leaf, mut);
    if (new_root == NULL) {
        return -1;
    }

    if (mut) {
        if (new_root == o->h_root) {
            Py_DECREF(new_root);
        }
        else {
            Py_SETREF(o->h_root, new_root);
        }
        if (added_leaf) {
            o->h_count++;
        }
        return 0;
    }

    if (new_root == o->h_root) {
        Py_DECREF(new_root);
        return 0;
    }
    GrailHamtObject *new_o = hamt_alloc(st);
    if (new_o == NULL) {
        Py_DECREF(new_root);
        return -1;
    }
    new_o->h_root = new_root;  /* borrow */
    new_o->h_count = added_leaf ? o->h_count + 1 : o->h_count;
    Py_SETREF(*po, new_o);
    return 0;
}

void
GrailHamt_IteratorInit(GrailHamtIteratorState *iter, GrailHamtObject *o)
{
    hamt_iterator_init(iter, o->h_root);
}

int
GrailHamt_IteratorNext(GrailHamtIteratorState *iter,
                     PyObject **key, PyObject **val)
{
    return hamt_iterator_next(iter, key, val) == I_ITEM;
}

GrailHamtObject *
GrailHamt_Without(GrailHamtObject *o, PyObject *key)
{
    GrailHamtState *st = HAMT_STATE(o);
    int32_t key_hash = hamt_hash(key);
    if (key_hash == -1) {
        return NULL;
    }

    GrailHamtNode *new_root = NULL;

    hamt_without_t res = hamt_node_without(
        (GrailHamtNode *)(o->h_root),
        0, key_hash, key,
        &new_root);

    switch (res) {
        case W_ERROR:
            return NULL;
        case W_EMPTY:
            return GrailHamt_New(st);
        case W_NOT_FOUND:
            return (GrailHamtObject*)Py_NewRef(o);
        case W_NEWNODE: {
            assert(new_root != NULL);

            GrailHamtObject *new_o = hamt_alloc(st);
            if (new_o == NULL) {
                Py_DECREF(new_root);
                return NULL;
            }

            new_o->h_root = new_root;  /* borrow */
            new_o->h_count = o->h_count - 1;
            assert(new_o->h_count >= 0);
            return new_o;
        }
        default:
            Py_UNREACHABLE();
    }
}

static hamt_find_t
hamt_find(GrailHamtObject *o, PyObject *key, PyObject **val)
{
    if (o->h_count == 0) {
        return F_NOT_FOUND;
    }

    int32_t key_hash = hamt_hash(key);
    if (key_hash == -1) {
        return F_ERROR;
    }

    return hamt_node_find(o->h_root, 0, key_hash, key, val);
}


int
GrailHamt_Find(GrailHamtObject *o, PyObject *key, PyObject **val)
{
    hamt_find_t res = hamt_find(o, key, val);
    switch (res) {
        case F_ERROR:
            return -1;
        case F_NOT_FOUND:
            return 0;
        case F_FOUND:
            return 1;
        default:
            Py_UNREACHABLE();
    }
}



Py_ssize_t
GrailHamt_Len(GrailHamtObject *o)
{
    return o->h_count;
}


static GrailHamtObject *
hamt_alloc(GrailHamtState *st)
{
    GrailHamtObject *o;
    o = PyObject_GC_New(GrailHamtObject, st->hamt_type);
    if (o == NULL) {
        return NULL;
    }
    o->h_count = 0;
    o->h_root = NULL;
    o->h_weakreflist = NULL;
    PyObject_GC_Track(o);
    return o;
}

GrailHamtObject *
GrailHamt_New(GrailHamtState *st)
{
    /* HAMT is an immutable object so we can easily cache an
       empty instance. */
    return (GrailHamtObject *)Py_NewRef(st->empty);
}


/////////////////////////////////// GrailHamt_Type

#define GrailHamtObject_CAST(op)      ((GrailHamtObject *)(op))

static int
hamt_tp_clear(PyObject *op)
{
    GrailHamtObject *self = GrailHamtObject_CAST(op);
    Py_CLEAR(self->h_root);
    return 0;
}

static int
hamt_tp_traverse(PyObject *op, visitproc visit, void *arg)
{
    GrailHamtObject *self = GrailHamtObject_CAST(op);
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(self->h_root);
    return 0;
}

static void
hamt_tp_dealloc(PyObject *self)
{
    GrailHamtObject *obj = GrailHamtObject_CAST(self);
    PyObject_GC_UnTrack(self);
    if (obj->h_weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }
    (void)hamt_tp_clear(self);
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    GRAIL_DECREF_TYPE(tp);
}

#define NODE_FLAGS (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | \
                    Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION)

static PyMemberDef hamt_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET,
     offsetof(GrailHamtObject, h_weakreflist), Py_READONLY},
    {NULL}
};

static PyType_Slot hamt_slots[] = {
    {Py_tp_dealloc, hamt_tp_dealloc},
    {Py_tp_traverse, hamt_tp_traverse},
    {Py_tp_clear, hamt_tp_clear},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_members, hamt_members},
    {0, NULL}
};

static PyType_Slot array_node_slots[] = {
    {Py_tp_dealloc, hamt_node_array_dealloc},
    {Py_tp_traverse, hamt_node_array_traverse},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {0, NULL}
};

static PyType_Slot bitmap_node_slots[] = {
    {Py_tp_dealloc, hamt_node_bitmap_dealloc},
    {Py_tp_traverse, hamt_node_bitmap_traverse},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {0, NULL}
};

static PyType_Slot collision_node_slots[] = {
    {Py_tp_dealloc, hamt_node_collision_dealloc},
    {Py_tp_traverse, hamt_node_collision_traverse},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {0, NULL}
};

static PyType_Spec hamt_spec = {
    "grail._hamt", sizeof(GrailHamtObject), 0, NODE_FLAGS, hamt_slots};
static PyType_Spec array_node_spec = {
    "grail._hamt_array_node", sizeof(GrailHamtNode_Array), 0,
    NODE_FLAGS, array_node_slots};
static PyType_Spec bitmap_node_spec = {
    "grail._hamt_bitmap_node",
    sizeof(GrailHamtNode_Bitmap) - sizeof(PyObject *), sizeof(PyObject *),
    NODE_FLAGS, bitmap_node_slots};
static PyType_Spec collision_node_spec = {
    "grail._hamt_collision_node",
    sizeof(GrailHamtNode_Collision) - sizeof(PyObject *), sizeof(PyObject *),
    NODE_FLAGS, collision_node_slots};

int
GrailHamt_InitState(PyObject *module, GrailHamtState *st)
{
#define NEW_TYPE(field, spec) \
    st->field = (PyTypeObject *)PyType_FromModuleAndSpec(module, &spec, NULL); \
    if (st->field == NULL) { \
        return -1; \
    }
    NEW_TYPE(hamt_type, hamt_spec);
    NEW_TYPE(array_type, array_node_spec);
    NEW_TYPE(bitmap_type, bitmap_node_spec);
    NEW_TYPE(collision_type, collision_node_spec);
#undef NEW_TYPE

    GrailHamtNode_Bitmap *node = PyObject_GC_NewVar(
        GrailHamtNode_Bitmap, st->bitmap_type, 0);
    if (node == NULL) {
        return -1;
    }
    node->b_bitmap = 0;
    PyObject_GC_Track(node);
    st->empty_bitmap = (GrailHamtNode *)node;

    st->empty = hamt_alloc(st);
    if (st->empty == NULL) {
        return -1;
    }
    st->empty->h_root = (GrailHamtNode *)Py_NewRef(st->empty_bitmap);
    return 0;
}

int
GrailHamt_TraverseState(GrailHamtState *st, visitproc visit, void *arg)
{
    Py_VISIT(st->hamt_type);
    Py_VISIT(st->array_type);
    Py_VISIT(st->bitmap_type);
    Py_VISIT(st->collision_type);
    Py_VISIT(st->empty_bitmap);
    Py_VISIT(st->empty);
    return 0;
}

void
GrailHamt_ClearState(GrailHamtState *st)
{
    Py_CLEAR(st->empty);
    Py_CLEAR(st->empty_bitmap);
    Py_CLEAR(st->hamt_type);
    Py_CLEAR(st->array_type);
    Py_CLEAR(st->bitmap_type);
    Py_CLEAR(st->collision_type);
}
