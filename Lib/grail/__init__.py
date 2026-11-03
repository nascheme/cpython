"""Experimental Grail facade for the CPython 3.15 free-threaded prototype.

Native boxed records with copy/pickle support, MRO/ABC generic dispatch,
and numeric record operators. Native persistent PMap/PSet/PVector, unboxed
FloatVector/IntVector, synchronized Atom cells, the thread Pool and the
mutate/finish drafts live in separate modules. No record field/call fast paths.
Inherited dispatch uses a bounded guarded native resolution cache.
"""

import _thread
import abc as _abc
import annotationlib as _annotationlib
import builtins as _builtins
import inspect as _inspect
import operator as _operator
import types as _types
import typing as _typing
from collections import namedtuple as _namedtuple

import _grail as _core
from _grail_pds import PMap, PSet
from _grail_atom import Atom
from _grail_vector import PVector
from _grail_numvec import FloatVector, IntVector
from _grail_pool import Pool
from _grail_draft import mutate, finish, original
from ._parallel import pmap, parallel_reduce

backend = _core.backend
__all__ = ["record", "field", "replace", "generic", "generic_impl", "generic_register",
           "generic_override", "generic_dispatch", "generic_fallback",
           "generic_version", "is_generic", "ops", "repr", "eq", "hash",
           "record_fields", "record_values", "is_record", "is_value_record",
           "conforms", "substitutable", "Record", "MISSING",
           "PMap", "PSet", "PVector", "FloatVector", "IntVector", "Atom",
           "Pool", "pool", "pmap", "parallel_reduce",
           "mutate", "finish", "original"]

class _Missing:
    __slots__ = ()

    def __repr__(self):
        return "MISSING"


MISSING = _Missing()  # no default or factory; no annotation
_Field = _namedtuple("_Field", "default factory")
FieldInfo = _namedtuple("FieldInfo", "name annotation kind default factory")


def field(*, default=MISSING, factory=MISSING):
    if default is not MISSING and factory is not MISSING:
        raise TypeError("specify a default or a factory, not both")
    if factory is not MISSING and not callable(factory):
        raise TypeError("field factory must be callable")
    return _Field(default, factory)


_TypeAliasType = _typing.TypeAliasType


def _value_plan(annotation, aliases=()):
    if annotation is None:
        annotation = type(None)
    if any(annotation is t for t in (type(None), bool, int, float, str, bytes)):
        return annotation
    if _core._is_record_type(annotation):
        return annotation if is_value_record(annotation) else None
    origin = _typing.get_origin(annotation)
    args = _typing.get_args(annotation)
    alias = origin if isinstance(origin, _TypeAliasType) else annotation
    if isinstance(alias, _TypeAliasType):
        if alias in aliases:
            return None  # a recursive alias has no finite plan
        try:
            value = alias.__value__
            if alias is origin:
                value = value[args]
        except (NameError, TypeError):
            return None  # unresolved, or a parameter the value does not use
        return _value_plan(value, aliases + (alias,))
    if origin is _typing.Annotated:
        return _value_plan(args[0], aliases)
    if origin is _typing.Literal:
        # Conformance is by type: membership in the literal is not checked.
        members = tuple(dict.fromkeys(type(value) for value in args))
        children = tuple(_value_plan(t, aliases) for t in members)
        if not all(t is not None for t in children):
            return None
        return children[0] if len(children) == 1 else (1, children)
    if origin in (_types.UnionType, _typing.Union):
        children = tuple(_value_plan(t, aliases) for t in args)
        return (1, children) if all(t is not None for t in children) else None
    if origin is tuple:
        if len(args) == 2 and args[1] is Ellipsis:
            child = _value_plan(args[0], aliases)
            return (3, (child,)) if child is not None else None
        if not args and annotation != tuple[()]:
            return None
        children = tuple(_value_plan(t, aliases) for t in args)
        return (2, children) if all(t is not None for t in children) else None
    return None


def _check_default(value):
    # Match dataclasses' unhashable-default approximation without calling
    # user hash functions. Known mutable cells will need explicit rejection
    # when atoms become part of the native implementation.
    if type(value).__hash__ is None:
        raise ValueError("mutable record default; use field(factory=...)")


@_typing.dataclass_transform(field_specifiers=(field,), frozen_default=True)
def record(cls=None, /, *, deriving=()):
    """Declare a record type.

    deriving names element-wise operators (a sequence such as "+-*/" or
    ("+", "-")) for a record whose fields are all
    annotated float: "+" and "-" for two records, "*" for a record and a
    scalar (float or int) in either order, "/" for a record divided by a
    scalar. They are registered in grail.ops like any implementation.
    """
    if cls is None:
        return lambda cls: record(cls, deriving=deriving)
    deriving = _check_deriving(deriving)
    result = _make_record(cls)
    if deriving:
        _derive(result, deriving)
    return result


def _make_record(cls):
    if type(cls) is not type or cls.__bases__ != (object,):
        raise TypeError("records require the standard metaclass and only object as base")
    namespace = vars(cls)
    allowed = {"__module__", "__qualname__", "__doc__", "__annotations__",
               "__annotate_func__", "__annotate__", "__firstlineno__",
               "__static_attributes__", "__dict__", "__weakref__",
               "__annotations_cache__"}
    for name, value in namespace.items():
        if name in allowed:
            if (name in ("__module__", "__doc__") and value is not None
                    and not isinstance(value, str)):
                raise TypeError(f"record metadata must be text: {name!r}")
            if name in ("__dict__", "__weakref__"):
                if (not isinstance(value, _types.GetSetDescriptorType)
                        or value.__objclass__ not in (cls, object)):
                    raise TypeError(f"record may not define {name!r}")
            continue
        if name.startswith("__") or callable(value) or hasattr(type(value), "__get__"):
            raise TypeError(f"record body may contain only fields: {name!r}")
    try:
        annotations = _annotationlib.get_annotations(cls, eval_str=True)
    except (NameError, TypeError):
        # Unresolved references remain ordinary fields. Plans never change
        # after type creation, even if the referenced name is later defined.
        annotations = _annotationlib.get_annotations(cls, format=_annotationlib.Format.STRING)
    names = list(annotations)
    names.extend(name for name in namespace if name not in allowed and name not in annotations)
    defaults = []
    for name in names:
        if name.startswith("__"):
            raise TypeError("dunder record fields are reserved")
        value = namespace.get(name, MISSING)
        factory = MISSING
        if isinstance(value, _Field):
            value, factory = value
        if factory is not MISSING:
            defaults.append((2, factory))
        elif value is MISSING:
            defaults.append((0, None))
        else:
            _check_default(value)
            defaults.append((1, value))
    annotations = tuple(annotations.get(name, MISSING) for name in names)
    plans = tuple(_value_plan(t) for t in annotations)
    return _core._make_record(cls, tuple(names), plans, tuple(defaults), annotations)


_DERIVABLE = {"+": 0, "-": 1, "*": 2, "/": 3}


def _check_deriving(deriving):
    deriving = tuple(deriving)  # a string is a sequence of operator characters
    for op in deriving:
        if op not in _DERIVABLE:
            raise ValueError(f"cannot derive {op!r}; expected one of + - * /")
    if len(set(deriving)) != len(deriving):
        raise ValueError("duplicate derived operator")
    return deriving


def _kernels_enabled():
    import os, sys
    return (sys.flags.ignore_environment
            or os.environ.get("PYTHON_GRAIL_KERNELS") != "0")


def _python_kernel(record_type, op, kind):
    # The same computation as a Python function: the kernel's reference
    # semantics, and the implementation when PYTHON_GRAIL_KERNELS=0.
    names = [info.name for info in record_fields(record_type)]
    symbol = "+-*/"[op]
    if kind == 0:
        parts = [f"a.{n} {symbol} b.{n}" for n in names]
    elif kind == 1:
        parts = [f"a.{n} {symbol} b" for n in names]
    else:
        parts = [f"a {symbol} b.{n}" for n in names]
    ns = {"R": record_type}
    exec(f"def element_wise(a, b): return R({', '.join(parts)})", ns)
    return ns["element_wise"]


def _derive(record_type, deriving):
    plans = _core._record_info(record_type)[0][1]
    if any(plan is not float for plan in plans):
        raise TypeError(f"deriving needs every field of {record_type.__name__} "
                        "to be annotated float")
    make = _core._kernel if _kernels_enabled() else _python_kernel
    for symbol in deriving:
        op = _DERIVABLE[symbol]
        generic = (ops.add, ops.sub, ops.mul, ops.truediv)[op]
        if op < 2:
            pairs = [((record_type, record_type), 0)]
        else:
            pairs = [((record_type, scalar), 1) for scalar in (float, int)]
            if op == 2:
                pairs += [((scalar, record_type), 2) for scalar in (float, int)]
        for pair, kind in pairs:
            generic.register(pair, make(record_type, op, kind))


replace = _core.replace_record


def record_fields(record_type):
    (names, plans, defaults, annotations), _ = _core._record_info(record_type)
    return tuple(FieldInfo(name, annotation, "value" if plan is not None else "ordinary",
                           value if kind == 1 else MISSING,
                           value if kind == 2 else MISSING)
                 for name, plan, (kind, value), annotation in
                 zip(names, plans, defaults, annotations))


def record_values(obj):
    return _core._record_values(obj)


def is_record(obj):
    """Whether obj is a record type; never raises."""
    return _core._is_record_type(obj)


def is_value_record(record_type):
    return _core._record_info(record_type)[1]


def conforms(obj):
    """Whether a record instance is a value record: its type is a value
    record type and every field holds a value of its declared type."""
    return _core._is_conforming(obj)


class Record(_abc.ABC):
    """Virtual base of every record type, for dispatch on "any record"."""
    __slots__ = ()

    def __new__(cls, *args, **kwargs):
        raise TypeError("Record cannot be instantiated")

    def __init_subclass__(cls, **kwargs):
        raise TypeError("Record cannot be subclassed")

    @classmethod
    def __subclasshook__(cls, candidate):
        return _core._is_record_type(candidate) or NotImplemented


def _deepcopy_record(obj, memo):
    # Immutable reconstruction, like copy._deepcopy_tuple: never memoize a
    # partial record. A cycle necessarily passes through an ordinary mutable
    # object, whose memo entry terminates recursive copying. A recursive call
    # can complete this record first; prefer its memoized result in that case.
    from copy import deepcopy
    values = tuple(deepcopy(value, memo)
                   for value in _core._record_values(obj).values())
    key = id(obj)
    if key in memo:
        return memo[key]
    result = type(obj)(*values)
    memo[key] = result
    return result


substitutable = _core.substitutable


def _resolve_generic(cls, snapshot, fallback):
    # functools is the ordinary-class semantic oracle, not the shim. The
    # snapshot owns all types/callables through ABC/metaclass callbacks and
    # reentrant registration. No native registry lock is held here.
    from functools import _find_impl
    registry = {types[0]: func for types, func in snapshot.values()}
    implementation = _find_impl(cls, registry)
    return fallback if implementation is None else implementation


def generic(fallback):
    signature = _inspect.signature(fallback)
    params = tuple(signature.parameters.values())
    if not params or params[0].kind not in (params[0].POSITIONAL_ONLY,
                                            params[0].POSITIONAL_OR_KEYWORD):
        raise TypeError("generic needs a positional first parameter")
    return _core.Generic(fallback, params[0].name, resolver=_resolve_generic)


def _validate_registration(g, dispatch, func):
    if _core._is_kernel(func):
        if not g.pair:
            raise TypeError("element-wise kernels implement binary operators")
        return  # validated when the kernel was made
    actual = _inspect.signature(func)
    if any(g is hook for hook in (repr, eq, hash, ops.neg, ops.abs)):
        if not _core._is_record_type(dispatch):
            raise TypeError("phase-one builtin protocol hooks require a record type")
    if g.pair or any(g is hook for hook in (repr, eq, hash, ops.neg, ops.abs)):
        actual.bind(*([None] * (2 if g.pair or g is eq else 1)))
        return
    try:
        expected = _inspect.signature(g.fallback)
    except ValueError:
        # Native protocol defaults have no Python signature. Their slots
        # supply fixed arguments; reject incompatible implementations now.
        count = 2 if g.pair or g is eq else 1
        actual.bind(*([None] * count))
        return
    def shape(signature):
        return tuple((p.name, p.kind, p.default is not p.empty)
                     for p in signature.parameters.values())
    # The dispatch argument is positional, so its name is not part of the shape.
    first = next(iter(actual.parameters.values()), None)
    if (first is None
            or first.kind not in (first.POSITIONAL_ONLY, first.POSITIONAL_OR_KEYWORD)
            or shape(actual)[1:] != shape(expected)[1:]):
        raise TypeError("implementation must preserve the generic call shape")


def _register_decorator(g, args, override):
    if len(args) == 2:
        dispatch, func = args
    elif len(args) == 1:
        func = args[0]
        params = tuple(_inspect.signature(func).parameters.values())
        count = 2 if g.pair else 1
        if len(params) < count:
            raise TypeError("implementation needs annotated dispatch parameters")
        hints = _annotationlib.get_annotations(func, eval_str=True)
        try:
            types = tuple(hints[p.name] for p in params[:count])
        except KeyError:
            raise TypeError("implementation needs annotated dispatch parameters") from None
        dispatch = types if g.pair else types[0]
    else:
        raise TypeError("expected an implementation, or a type and implementation")
    return g.register(dispatch, func, override=override)


def is_generic(value, /):
    return isinstance(value, _core.Generic)


def _operation(operation):
    if not isinstance(operation, _core.Generic):
        raise TypeError("expected a generic function, not "
                        f"{type(operation).__name__}")
    return operation


# Module-level spellings of the Generic methods. The stub types generic()
# as returning its fallback unchanged, so a type checker sees only these.
def generic_impl(operation, /):
    return _operation(operation).impl


def generic_register(operation, dispatch, func, /):
    return _operation(operation).register(dispatch, func)


def generic_override(operation, /, *args):
    g = _operation(operation)
    if not args:
        def decorator(func, /):
            return g.override(func)
        return decorator
    if len(args) != 2:
        raise TypeError("generic_override takes a generic function, optionally "
                        "followed by a dispatch key and an implementation")
    return g.override(*args)


def generic_dispatch(operation, dispatch, /):
    return _operation(operation).dispatch(dispatch)


def generic_fallback(operation, /):
    return _operation(operation).fallback


def generic_version(operation, /):
    return _operation(operation).version


# Native fallbacks execute generated C record behavior. Their hooks can be
# extended without altering any record type or its descriptors.
repr = _core.Generic(_core._default_repr, "obj")
eq = _core.Generic(_core._default_eq, "a")
hash = _core.Generic(_core._default_hash, "obj")


_Ops = _namedtuple("_Ops", "add sub mul truediv neg abs")
ops = _Ops(*(_core.Generic(getattr(_operator, name), "a", pair=True)
             for name in ("add", "sub", "mul", "truediv")),
           _core.Generic(_operator.neg, "obj"),
           _core.Generic(_builtins.abs, "obj"))
_core._configure(repr, eq, hash,
                 (ops.add, ops.sub, ops.mul, ops.truediv, ops.neg, ops.abs))


_shared_pool = None
_shared_pool_lock = _thread.allocate_lock()


def pool():
    """Return the shared Pool, started on first use with one worker per CPU.

    It is shut down at exit. A child process made by fork() starts its own.
    """
    global _shared_pool
    with _shared_pool_lock:
        if _shared_pool is None:
            import atexit
            import os
            _shared_pool = Pool(os.process_cpu_count() or 1)
            atexit.register(_shared_pool.shutdown)
            if hasattr(os, "register_at_fork"):
                os.register_at_fork(after_in_child=_forget_pool)
        return _shared_pool


def _forget_pool():
    global _shared_pool, _shared_pool_lock
    _shared_pool = None
    _shared_pool_lock = _thread.allocate_lock()
