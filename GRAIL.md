# Grail

Grail adds immutable values and open, type-dispatched functions to CPython,
with the free-threaded build as the primary target.  Values can be shared
between threads without locks, and the interpreter can rely on their
immutability when it specializes code.

Only free-threaded builds (`--disable-gil`) are supported.  The Windows build
has not been integrated.

## What it provides

The `grail` module provides:

- **Records** (`@grail.record`, `grail.field`): immutable classes with fixed,
  boxed fields.  Fields whose annotations are value types (`int`, `float`,
  `str`, `bytes`, `bool`, `None`, other value records, and unions and tuples
  of these) are checked when a record is built.  A record whose fields all
  conform is a *value record*: deeply immutable, structurally compared and
  hashed, shareable without synchronization.  Other fields are ordinary
  references with CPython's usual semantics.  Records support
  `copy.replace`, positional class patterns, copying and pickling.
- **Generic functions** (`@grail.generic`, `.impl`, `.register`,
  `.override`): single dispatch on the first argument with the same MRO and
  ABC rules as `functools.singledispatch`.  Exact-type calls are resolved
  without locks; inherited selections are cached.
- **Operators** (`grail.ops`): registrations for `+ - * /` on exact operand
  type pairs, used by the binary operators, `operator` and the `PyNumber_*`
  C API.  Pairs of two builtin types cannot be registered.  `deriving` on a
  record of floats generates element-wise operators in C.
- **Protocol hooks** (`grail.repr`, `grail.eq`, `grail.hash`): generics that
  define records' `repr`, `==` and `hash`.  The defaults are implemented in
  C, and the default hash of a value record is cached.
- **Persistent collections**: `PMap` and `PSet` (HAMT), `PVector` (trie),
  with builders (transients) for batched construction.
- **Numeric vectors**: `FloatVector` and `IntVector`, flat unboxed storage.
- **Atom**: a synchronized cell holding a value, with `swap`, `reset`,
  compare-and-set and an exact-int `add`.
- Introspection: `record_fields`, `record_values`, `is_value_record`,
  `substitutable`.

## How it fits into CPython

- A record type is an ordinary heap type with `object` as its only base.  Its
  frozen schema (field names, defaults and the compiled value plans) is held
  by a private pointer on `PyHeapTypeObject`, outside the type's namespace.
  Declaration is processed in Python (`Lib/grail/`); construction, field
  access, replacement and dispatch are native and do not read Python
  metadata.
- Each record instance carries a conformance bit, set at construction.
  Value records hold no finalizers or weak references.  Small records are
  recycled through per-thread freelists.
- Generic functions are native objects with lock-free reads of an immutable
  registration table.  Protocol hooks and operator registrations are per
  interpreter.
- The specializing interpreter knows about records and generics:
  `LOAD_ATTR_RECORD` (field loads at fixed offsets), `CALL_RECORD`
  (positional construction), `CALL_GENERIC` (dispatch without a frame),
  `BINARY_OP_GRAIL` and `BINARY_OP_GRAIL_KERNEL` (registered operators),
  `MATCH_CLASS_RECORD` and `UNPACK_SEQUENCE_RECORD` (class patterns).  The
  tier-2 optimizer types field loads from value plans.
- Immutable values loaded from a shared cell by another thread switch to
  deferred reference counting, so readers do not contend on their
  reference counts.  Registrations retire replaced tables through delayed
  reclamation.

## Source map

| Area | Main files |
| --- | --- |
| Records and schema | `Objects/recordobject.c`, `Objects/typeobject.c`, `Include/internal/pycore_grail.h` |
| Generics and central arithmetic | `Objects/genericobject.c`, `Objects/abstract.c`, `Modules/_abc.c` |
| Python facade and hooks | `Lib/grail/`, `Modules/_grail.c` |
| Interpreter specialization | `Python/bytecodes.c`, `Python/specialize.c`, `Python/optimizer_bytecodes.c` |
| Shared-value lifetime | `Objects/object.c`, `Include/internal/pycore_cell.h`, `Include/internal/pycore_object_deferred.h` |
| Collections | `Modules/_grail/` (`PMap`, `PSet`, HAMT), `Modules/_grail_vector.c`, `Modules/_grail_numvec.c` |
| Atom | `Modules/_grail_atom.c` |
| Copy | `Lib/copy.py` |
| Tests | `Lib/test/test_grail*.py` |

After editing `Python/bytecodes.c`, regenerate with `make regen-cases`; the
generated files are not edited by hand.

## Build and test

```sh
mkdir build && cd build
../configure --disable-gil
make -j8
./python -X gil=0 -m test test_grail test_grail_atom test_grail_pds \
    test_grail_vector test_grail_numvec test_grail_hamt test_grail_native \
    test_grail_specialization test_grail_call_specialization \
    test_grail_arithmetic_specialization test_grail_jit
```

`test_grail_jit` needs a build configured with `--enable-experimental-jit`.

## Limits

- Operators are exact-pair only; comparisons and other binary operators are
  not open registries.  There is no way to unregister an implementation, and
  registries keep their types and callables until interpreter shutdown.
- Annotations never coerce values: an `int` passed for a `float` field makes
  the record nonconforming, not an error.  Unsupported or unresolved
  annotations become ordinary fields.
- Hooks are trusted to keep the equality and hash obligations and to be pure
  functions of the value.
- No record syntax, sum types, typed collections or public C API yet.
- `_grail` and the other private modules are implementation details.
