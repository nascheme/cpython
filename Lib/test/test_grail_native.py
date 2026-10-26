"""Tests of the native Grail runtime's internals.

What Grail does is specified by grail-compat's tests (grail-compat/tests),
copied into this directory as test_grail.py, test_grail_atom.py and so on by
grail-compat's tools/sync_to_cpython_grail.py; edit them there, not here.  This file
holds the tests only the native implementation can pass: the generic-dispatch
caches and their ablation switches, operator dispatch through the interpreter
for non-record types, allocation-failure cleanup and structural sharing in the
C containers.
"""
import abc
import functools
import gc
import operator
import sys
import sysconfig
import unittest
import weakref

from test.support import threading_helper
from test.support.script_helper import assert_python_ok
from test.test_grail import Barrier, run_pool

import _grail
import grail
from grail import PVector


def setUpModule():
    # Cache-hit counting is off by default: it writes shared memory on the
    # lock-free dispatch path.
    _grail._set_dispatch_stats(True)


def tearDownModule():
    _grail._set_dispatch_stats(False)


def isolated_registrations(test):
    # Protocol/operator registries intentionally own registered types for the
    # interpreter lifetime. Isolate tests that extend those global registries
    # so refleak repetitions do not accumulate fresh local types/callables.
    @functools.wraps(test)
    def run(self):
        import _interpreters
        ident = _interpreters.create()
        try:
            result = _interpreters.exec(ident, f'''
from test import test_grail_native
case = test_grail_native.{type(self).__name__}({test.__name__!r})
method = getattr(case, {test.__name__!r})
method.__wrapped__(case)
''')
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)
    return run


class RecordNativeTests(unittest.TestCase):
    """The runtime identifies itself."""

    def test_native_and_gil_disabled(self):
        self.assertEqual(grail.backend, "native-phase1")
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())
        self.assertEqual(grail.generic(lambda obj: obj).__class__.__module__, "_grail")

class RecordConformanceTests(unittest.TestCase):
    """Value plans are checked without a recursion guard up to a fixed
    height, and with one above it."""

    def test_union_of_leaves_and_deep_plans(self):
        @grail.record
        class Leaf:
            x: int
        @grail.record
        class Holder:
            items: tuple[Leaf | None, ...]
        self.assertTrue(_grail._is_conforming(Holder((Leaf(1), None))))
        self.assertFalse(_grail._is_conforming(Holder((Leaf(1), 2))))
        self.assertFalse(_grail._is_conforming(Holder((Leaf("1"),))))
        for depth in (31, 32, 33, 40):
            ann = int
            value = 1
            for _ in range(depth):
                ann = tuple[ann | None, ...]
                value = (value, None)
            Deep = grail.record(type("Deep", (), {"__annotations__": {"v": ann}}))
            self.assertTrue(_grail._is_conforming(Deep(value)), depth)
            bad = "x"
            for _ in range(depth):
                bad = (bad,)
            self.assertFalse(_grail._is_conforming(Deep(bad)), depth)


class RecordHashCacheTests(unittest.TestCase):
    """Conforming value records cache their default hash, tagged with the
    grail.hash registration generation."""

    def nested_types(self, calls):
        @grail.record
        class Inner:
            x: int
        @grail.record
        class Outer:
            inner: Inner
            n: int
        grail.hash.register(Inner, lambda r: calls.append(r) or r.x)
        return Inner, Outer

    @isolated_registrations
    def test_cached_and_invalidated(self):
        calls = []
        Inner, Outer = self.nested_types(calls)
        o = Outer(Inner(5), 1)
        h = hash(o)
        self.assertEqual(h, hash((5, 1)))
        self.assertEqual(hash(o), h)
        self.assertEqual(len(calls), 1)
        # Any grail.hash registration invalidates every cached hash.
        @grail.record
        class Other:
            x: int
        grail.hash.register(Other, lambda r: 0)
        self.assertEqual(hash(o), h)
        self.assertEqual(len(calls), 2)
        self.assertEqual(hash(o), h)
        self.assertEqual(len(calls), 2)
        # Nonconforming instances are not cached.
        n = Outer(Inner(5), "1")
        hash(n)
        hash(n)
        self.assertEqual(len(calls), 4)

    def test_native_hooks_off_disables_cache(self):
        code = """if 1:
            import grail
            calls = []
            @grail.record
            class Inner:
                x: int
            @grail.record
            class Outer:
                inner: Inner
            grail.hash.register(Inner, lambda r: calls.append(r) or r.x)
            o = Outer(Inner(5))
            assert hash(o) == hash(o) == hash((5,))
            assert len(calls) == 2, calls
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_NATIVE_HOOKS="0")

    @threading_helper.requires_working_threading()
    @isolated_registrations
    def test_concurrent_hash_and_registration(self):
        @grail.record
        class Point:
            x: int
        points = [Point(i) for i in range(200)]
        expected = [hash((i,)) for i in range(200)]
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            for _ in range(50):
                if index:
                    for p, h in zip(points, expected):
                        self.assertEqual(hash(p), h)
                else:
                    @grail.record
                    class Other:
                        x: int
                    grail.hash.register(Other, lambda r: 0)
        run_pool(worker, 4)


class GenericCacheTests(unittest.TestCase):
    """Inherited-dispatch caches of generic functions."""

    def test_native_resolution_cache_and_ablation(self):
        class Base:
            pass
        class Child(Base):
            pass
        class Missing:
            pass
        calls = []
        def resolver(cls, snapshot, fallback):
            calls.append(cls)
            return grail._resolve_generic(cls, snapshot, fallback)
        fallback = lambda obj: "fallback"
        f = _grail.Generic(fallback, "obj", resolver=resolver)
        implementation = lambda obj: "base"
        f.register(Base, implementation)
        self.assertEqual(f(Base()), "base")
        self.assertEqual(calls, [])  # exact hits never use the resolver/cache
        for _ in range(5):
            self.assertEqual(f(obj=Child()), "base")
            self.assertIs(f.dispatch(Child), implementation)
            self.assertEqual(f(Missing()), "fallback")
        self.assertEqual(calls, [Child, Missing])
        info = f._cache_info()
        self.assertEqual(info["size"], 2)
        self.assertEqual(info["hits"], 13)
        self.assertEqual(info["misses"], 2)
        f._set_cache_enabled(False)
        for _ in range(3):
            self.assertEqual(f(Child()), "base")
        self.assertEqual(calls, [Child, Missing, Child, Child, Child])
        self.assertEqual(f._cache_info()["size"], 0)
        f._set_cache_enabled(True)
        self.assertEqual(f(Child()), "base")
        self.assertEqual(f(Child()), "base")
        self.assertEqual(len(calls), 6)

    def test_cache_registration_and_negative_invalidation(self):
        class A:
            pass
        class B(A):
            pass
        class Missing:
            pass
        f = grail.generic(lambda obj: -1)
        self.assertEqual(f(B()), -1)
        self.assertEqual(f(Missing()), -1)
        self.assertEqual(f._cache_info()["size"], 2)
        impl = lambda obj: 1
        ref = weakref.ref(impl)
        f.register(A, impl)
        self.assertEqual(f._cache_info()["size"], 0)
        self.assertEqual(f(B()), 1)
        self.assertEqual(f(B()), 1)
        self.assertGreater(f._cache_info()["hits"], 0)
        del impl
        f.override(A, lambda obj: 2)
        gc.collect()
        self.assertIsNone(ref())
        self.assertEqual(f(B()), 2)
        f.register(Missing, lambda obj: 3)
        self.assertEqual(f(Missing()), 3)
        f.register(B, lambda obj: 4)
        self.assertEqual(f(B()), 4)

    def test_cache_type_and_metaclass_guards(self):
        class Meta(type):
            def __subclasscheck__(cls, candidate):
                return candidate is T
        class Registered(metaclass=Meta):
            pass
        class T:
            pass
        f = grail.generic(lambda obj: "fallback")
        f.register(Registered, lambda obj: "registered")
        self.assertEqual(f(T()), "registered")
        self.assertEqual(f(T()), "registered")
        self.assertEqual(f._cache_info()["hits"], 1)
        Meta.__subclasscheck__ = lambda cls, candidate: False
        self.assertEqual(f(T()), "fallback")
        self.assertEqual(f(T()), "fallback")
        self.assertEqual(f._cache_info()["hits"], 2)

        class A:
            pass
        class B:
            pass
        class Child(A):
            pass
        g = grail.generic(lambda obj: 0)
        g.register(A, lambda obj: 1)
        g.register(B, lambda obj: 2)
        for _ in range(2):
            self.assertEqual(g(Child()), 1)
        Child.__bases__ = (B,)
        self.assertEqual(g(Child()), 2)
        # Even ordinary attributes invalidate type versions conservatively.
        misses = g._cache_info()["misses"]
        Child.marker = True
        self.assertEqual(g(Child()), 2)
        self.assertEqual(g._cache_info()["misses"], misses + 1)

    def test_cache_abc_invalidation(self):
        class A(abc.ABC):
            pass
        class B(abc.ABC):
            pass
        class T:
            pass
        f = grail.generic(lambda obj: "fallback")
        f.register(A, lambda obj: "A")
        f.register(B, lambda obj: "B")
        for _ in range(3):
            self.assertEqual(f(T()), "fallback")
        self.assertEqual(f._cache_info()["hits"], 2)
        A.register(T)
        for _ in range(3):
            self.assertEqual(f(T()), "A")
        self.assertEqual(f._cache_info()["hits"], 4)
        B.register(T)
        for _ in range(2):
            with self.assertRaisesRegex(RuntimeError, "Ambiguous dispatch"):
                f(T())
        # Errors are never published as resolutions.
        f._set_cache_enabled(False)
        with self.assertRaisesRegex(RuntimeError, "Ambiguous dispatch"):
            f(T())

    def test_cache_matches_uncached_through_mutations(self):
        class A:
            pass
        class B:
            pass
        class V(abc.ABC):
            pass
        class W(abc.ABC):
            pass
        class T(A):
            pass
        class Child(T):
            pass
        class Missing:
            pass
        fallback = lambda obj: -1
        cached = grail.generic(fallback)
        oracle = grail.generic(fallback)
        oracle._set_cache_enabled(False)
        for cls, label in ((A, 0), (B, 1), (V, 2), (W, 3)):
            def make(label):
                return lambda obj: label
            implementation = make(label)
            cached.register(cls, implementation)
            oracle.register(cls, implementation)
        for step in range(20):
            T.__bases__ = (A if step % 2 else B,)
            if step == 5:
                V.register(Missing)
            if step == 10:
                W.register(Missing)
            if step % 3 == 0:
                implementation = lambda obj: step
                cached.override(A, implementation)
                oracle.override(A, implementation)
            for _ in range(2):
                for cls in (A, B, T, Child, Missing, object):
                    try:
                        expected = oracle.dispatch(cls)
                    except RuntimeError as error:
                        with self.assertRaisesRegex(RuntimeError, "Ambiguous dispatch"):
                            cached.dispatch(cls)
                        self.assertIn("Ambiguous dispatch", str(error))
                    else:
                        self.assertIs(cached.dispatch(cls), expected)
                        self.assertEqual(cached(cls()), oracle(cls()))
        self.assertGreater(cached._cache_info()["hits"], 0)
        self.assertEqual(oracle._cache_info()["size"], 0)

    def test_cache_limits_and_type_lifetime(self):
        f = grail.generic(lambda obj: "fallback")
        refs = []
        for i in range(100):
            cls = type(f"Candidate{i}", (), {})
            refs.append(weakref.ref(cls))
            self.assertIs(f.dispatch(cls), f.fallback)
            self.assertLessEqual(f._cache_info()["size"],
                                 f._cache_info()["capacity"])
        del cls
        gc.collect()
        self.assertLessEqual(sum(ref() is not None for ref in refs), 16)
        f._set_cache_enabled(False)
        gc.collect()
        self.assertTrue(all(ref() is None for ref in refs))
        # Large registries deliberately use the uncached correctness path.
        for _ in range(f._cache_info()["max_registrations"] + 1):
            f.register(type("Registered", (), {}), lambda obj: 0)
        f._set_cache_enabled(True)
        self.assertEqual(f(object()), "fallback")
        self.assertEqual(f(object()), "fallback")
        self.assertEqual(f._cache_info()["size"], 0)

    def test_cache_unversionable_type(self):
        class Meta(type):
            def mro(cls):
                return super().mro()
        class T(metaclass=Meta):
            pass
        f = grail.generic(lambda obj: 0)
        self.assertEqual(f(T()), 0)
        self.assertEqual(f(T()), 0)
        self.assertEqual(f._cache_info()["size"], 0)
        self.assertEqual(f._cache_info()["hits"], 0)

    def test_cache_reentrant_publication(self):
        class A:
            pass
        class B(A):
            pass
        calls = 0
        def resolver(cls, snapshot, fallback):
            nonlocal calls
            calls += 1
            result = grail._resolve_generic(cls, snapshot, fallback)
            if calls == 1:
                f.override(A, lambda obj: 2)
            elif calls == 2:
                f._set_cache_enabled(False)
                f._set_cache_enabled(True)
            return result
        f = _grail.Generic(lambda obj: 0, "obj", resolver=resolver)
        f.register(A, lambda obj: 1)
        self.assertEqual(f(B()), 1)  # owning old registry snapshot
        self.assertEqual(f._cache_info()["size"], 0)
        self.assertEqual(f(B()), 2)
        self.assertEqual(f._cache_info()["size"], 0)  # ablation changed generation
        self.assertEqual(f(B()), 2)
        self.assertEqual(f(B()), 2)
        self.assertEqual(calls, 3)

    def test_cache_type_change_during_resolution(self):
        class A:
            pass
        class B:
            pass
        class T(A):
            pass
        calls = 0
        def resolver(cls, snapshot, fallback):
            nonlocal calls
            calls += 1
            result = grail._resolve_generic(cls, snapshot, fallback)
            if calls == 1:
                T.__bases__ = (B,)
            return result
        f = _grail.Generic(lambda obj: 0, "obj", resolver=resolver)
        f.register(A, lambda obj: 1)
        f.register(B, lambda obj: 2)
        self.assertEqual(f(T()), 1)
        self.assertEqual(f._cache_info()["size"], 0)
        self.assertEqual(f(T()), 2)
        self.assertEqual(f(T()), 2)
        self.assertEqual(calls, 2)

    def test_cache_abc_change_during_resolution(self):
        class A(abc.ABC):
            pass
        class T:
            pass
        calls = 0
        def resolver(cls, snapshot, fallback):
            nonlocal calls
            calls += 1
            result = grail._resolve_generic(cls, snapshot, fallback)
            if calls == 1:
                A.register(T)
            return result
        f = _grail.Generic(lambda obj: 0, "obj", resolver=resolver)
        f.register(A, lambda obj: 1)
        self.assertEqual(f(T()), 0)
        self.assertEqual(f._cache_info()["size"], 0)
        self.assertEqual(f(T()), 1)
        self.assertEqual(f(T()), 1)
        self.assertEqual(calls, 2)

    def test_cached_callable_mutation_and_retirement(self):
        class A:
            pass
        class B(A):
            pass
        f = grail.generic(lambda obj: -1)
        def implementation(obj, result=1):
            return result
        # General generics preserve call shape, including defaults.
        g = grail.generic(lambda obj, result=1: -1)
        g.register(A, implementation)
        self.assertEqual(g(B()), 1)
        implementation.__defaults__ = (2,)
        self.assertEqual(g(B()), 2)
        self.assertEqual(g._cache_info()["hits"], 1)
        implementation.__code__ = (lambda obj, result=1: -result).__code__
        self.assertEqual(g(B()), -2)
        g._set_cache_enabled(False)
        self.assertEqual(g(B()), -2)

        events = []
        old = lambda obj: 1
        ref = weakref.ref(old, lambda ref: events.append(f(B())))
        f.register(A, old)
        self.assertEqual(f(B()), 1)
        del old
        f.override(A, lambda obj: 2)
        gc.collect()
        self.assertIsNone(ref())
        self.assertEqual(events, [2])

    @threading_helper.requires_working_threading()
    def test_cache_concurrent_type_changes_and_ablation(self):
        class A:
            pass
        class B:
            pass
        class T(A):
            pass
        value = T()
        f = grail.generic(lambda obj: -1)
        f.register(A, lambda obj: 0)
        f.register(B, lambda obj: 1)
        self.assertEqual(f(value), 0)
        self.assertEqual(f(value), 0)
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            for i in range(300):
                if index == 0:
                    expected = i % 2
                    T.__bases__ = (A if expected == 0 else B,)
                    self.assertEqual(f(value), expected)
                elif index == 1:
                    f._set_cache_enabled(i % 2 == 0)
                else:
                    self.assertIn(f(value), (0, 1))
                    self.assertIn(f.dispatch(T)(value), (0, 1))
        run_pool(worker, 4)
        T.__bases__ = (A,)
        f._set_cache_enabled(True)
        self.assertEqual(f(value), 0)
        self.assertEqual(f(value), 0)
        self.assertGreater(f._cache_info()["hits"], 0)

    def test_cache_gc_cycle(self):
        def make():
            class T:
                pass
            f = grail.generic(lambda obj: 0)
            T.generic = f
            f(T())
            self.assertEqual(f._cache_info()["size"], 1)
            return weakref.ref(T)
        ref = make()
        gc.collect()
        self.assertIsNone(ref())

    def test_cache_subinterpreter_isolation(self):
        import _interpreters
        class T:
            pass
        f = grail.generic(lambda obj: 0)
        self.assertEqual(f(T()), 0)
        ident = _interpreters.create()
        try:
            result = _interpreters.exec(ident, """
import abc, grail
class A(abc.ABC): pass
class T: pass
f = grail.generic(lambda obj: 0)
f.register(A, lambda obj: 1)
assert f(T()) == 0
assert f(T()) == 0
assert f._cache_info()['hits'] == 1
A.register(T)
assert f(T()) == 1
f._set_cache_enabled(False)
assert f(T()) == 1
""")
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)
        self.assertEqual(f(T()), 0)
        self.assertEqual(f._cache_info()["hits"], 1)

class OperatorNativeTests(unittest.TestCase):
    """Registered operators for non-record operand types, dispatched by the VM."""

    @isolated_registrations
    def test_not_implemented(self):
        @grail.record
        class R:
            x: int
        # A registered result is final: NotImplemented is not a decline.
        grail.ops.add.register((R, R), lambda a, b: NotImplemented)
        r = R(1)
        for call in (operator.add, operator.iadd, grail.ops.add):
            self.assertIs(call(r, R(2)), NotImplemented)
        r += R(2)
        self.assertIs(r, NotImplemented)
        grail.ops.add.register((R, object), lambda a, b: "ordinary partner")
        self.assertEqual(R(1) + object(), "ordinary partner")

    @isolated_registrations
    def test_warmed_builtin_arithmetic_and_c_api(self):
        import _testcapi

        def add(a, b): return a + b
        def sub(a, b): return a - b
        def mul(a, b): return a * b
        def div(a, b): return a / b
        def iadd(a, b):
            a += b
            return a
        def isub(a, b):
            a -= b
            return a
        def imul(a, b):
            a *= b
            return a
        def idiv(a, b):
            a /= b
            return a

        operations = [
            (grail.ops.add, add, iadd, operator.add, operator.iadd,
             _testcapi.number_add, _testcapi.number_inplaceadd),
            (grail.ops.sub, sub, isub, operator.sub, operator.isub,
             _testcapi.number_subtract, _testcapi.number_inplacesubtract),
            (grail.ops.mul, mul, imul, operator.mul, operator.imul,
             _testcapi.number_multiply, _testcapi.number_inplacemultiply),
            (grail.ops.truediv, div, idiv, operator.truediv, operator.itruediv,
             _testcapi.number_truedivide, _testcapi.number_inplacetruedivide),
        ]
        class MyInt(int): pass
        class MyFloat(float): pass
        # Sites warmed on builtin pairs; the registered pairs differ only in
        # a heap subclass, which no builtin kernel accepts.
        from types import FunctionType
        pairs = [((12345, 6789), (MyInt(12345), 6789)),
                 ((12345.5, 6789.5), (12345.5, MyFloat(6789.5))),
                 ((12345, 6789.5), (MyInt(12345), 6789.5))]
        cases = []
        for generic, binary, inplace, *other_calls in operations:
            for builtin, heap in pairs:
                binary = FunctionType(binary.__code__.replace(), globals())
                inplace = FunctionType(inplace.__code__.replace(), globals())
                for _ in range(1000):
                    binary(*builtin)
                    inplace(*builtin)
                cases.append((generic, builtin, heap,
                              [binary, inplace, *other_calls]))
        marker = object()
        for generic, builtin, heap, calls in cases:
            expected = calls[2](*builtin)
            generic.register(tuple(map(type, heap)), lambda a, b: marker)
            for call in [generic, *calls]:
                self.assertIs(call(*heap), marker)
                self.assertEqual(call(*builtin), expected)
            with self.assertRaisesRegex(TypeError, "two built-in types"):
                generic.register(tuple(map(type, builtin)), lambda a, b: marker)
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    @isolated_registrations
    def test_warmed_unicode_and_sequences(self):
        def add(a, b): return a + b
        def inplace(a, b):
            a += b
            return a
        for _ in range(1000):
            self.assertEqual(add("grail-left", "grail-right"),
                             "grail-leftgrail-right")
            inplace("grail-left", "grail-right")
        for pair in ((str, str), (list, list), (list, int), (tuple, tuple)):
            with self.assertRaisesRegex(TypeError, "two built-in types"):
                grail.ops.add.register(pair, lambda a, b: None)
        self.assertEqual(add("grail-left", "grail-right"),
                         "grail-leftgrail-right")
        class L(list):
            pass
        marker = object()
        left, right = L([1]), [2]
        grail.ops.add.register((L, list), lambda a, b: marker)
        for call in (add, inplace, operator.add, operator.iadd, grail.ops.add):
            self.assertIs(call(left, right), marker)
        self.assertEqual(left, [1])  # exact registration precedes mutation
        grail.ops.mul.register((L, int), lambda a, b: marker)
        self.assertIs(left * 2, marker)
        self.assertIs(operator.imul(left, 2), marker)
        self.assertEqual(left, [1])
        # No synthesized commutative pair.
        self.assertEqual(2 * left, [1, 1])
        self.assertEqual(right + left, [2, 1])

    @isolated_registrations
    def test_fallback_order_and_single_attempt(self):
        events = []
        class Base:
            def __add__(self, other):
                events.append("add")
                return "base"
            def __iadd__(self, other):
                events.append("iadd")
                return NotImplemented
        class Child(Base):
            def __radd__(self, other):
                events.append("radd")
                return "reflected"
        a, b = Base(), Child()
        self.assertEqual(a + b, "reflected")
        self.assertEqual(events, ["radd"])
        events.clear()
        # A registered NotImplemented is final: no reflected or in-place
        # fallback runs.
        def decline(a, b):
            events.append("registered")
            return NotImplemented
        grail.ops.add.register((Base, Child), decline)
        for call in (operator.add, grail.ops.add, operator.iadd):
            events.clear()
            self.assertIs(call(a, b), NotImplemented)
            self.assertEqual(events, ["registered"])
        grail.ops.add.override((Base, Child), lambda a, b: "exact")
        events.clear()
        self.assertEqual(a + b, "exact")
        self.assertEqual(operator.iadd(a, b), "exact")
        self.assertEqual(events, [])
        # Pair lookup has neither MRO nor ABC matching.
        grail.ops.add.register((Base, Base), lambda a, b: "not inherited")
        self.assertEqual(Child() + Child(), "base")

    @isolated_registrations
    def test_reentrant_operator_and_failure(self):
        class T:
            def __add__(self, other):
                return "fallback"
        def first(a, b):
            grail.ops.add.override((T, T), lambda a, b: "next")
            gc.collect()
            return NotImplemented
        grail.ops.add.register((T, T), first)
        a, b = T(), T()
        self.assertIs(a + b, NotImplemented)
        self.assertEqual(a + b, "next")
        def fail(a, b):
            raise ValueError("operator failed")
        grail.ops.add.override((T, T), fail)
        for call in (operator.add, operator.iadd, grail.ops.add):
            with self.assertRaisesRegex(ValueError, "operator failed"):
                call(a, b)
        with self.assertRaises(TypeError):
            grail.ops.add.register((T, T), fail)
        with self.assertRaises(TypeError):
            grail.ops.add.register((T, 1), fail)
        # Failed registration leaves the previous callable intact.
        with self.assertRaisesRegex(ValueError, "operator failed"):
            a + b

    @isolated_registrations
    @threading_helper.requires_working_threading()
    def test_concurrent_operator_publication(self):
        class T:
            def __add__(self, other):
                return -1
        a, b = T(), T()
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            for _ in range(300):
                if index == 0:
                    if grail.ops.add.version == 0:
                        grail.ops.add.register((T, T), lambda a, b: 0)
                    else:
                        grail.ops.add.override((T, T), lambda a, b: 0)
                elif index == 1:
                    self.assertIn(operator.iadd(a, b), (-1, 0))
                else:
                    self.assertIn(a + b, (-1, 0))
        run_pool(worker, 4)
        self.assertEqual(a + b, 0)

class AtomNativeTests(unittest.TestCase):
    """Allocation failures in Atom."""

    def test_add_allocation_failure(self):
        try:
            import _testcapi
        except ImportError:
            self.skipTest("requires _testcapi")
        if not hasattr(_testcapi, "set_nomemory"):
            self.skipTest("requires _testcapi.set_nomemory")
        # Allocator hooks are process-global; never install in the test runner.
        assert_python_ok("-c", '''
import _testcapi
from grail import Atom
old = 2**200
for delta in (1, -1, 2**400):
    a = Atom(old)
    add = a.add
    failed = False
    try:
        _testcapi.set_nomemory(0, 1)
        add(delta)
    except MemoryError:
        failed = True
    finally:
        _testcapi.remove_mem_hooks()
    assert failed
    assert a.value is old
    assert add(delta) == old + delta
''')

class PVectorNativeTests(unittest.TestCase):
    """PVector internals."""

    def test_structural_sharing(self):
        # Implementation-level ownership test, not a public layout contract.
        def nodes(v):
            return [x for x in gc.get_referents(v) if type(x) is tuple]
        a = PVector(range(2000))
        root, tail = nodes(a)
        b = a.set(0, "changed")
        newroot, newtail = nodes(b)
        self.assertIs(newtail, tail)
        self.assertIsNot(root, newroot)
        self.assertIs(root[1], newroot[1])
        self.assertIsNot(root[0], newroot[0])
        for b in (a.set(-1, "changed"), a.append(2000), a.pop()):
            self.assertIs(nodes(b)[0], root)
        full = PVector(range(64))
        oldroot, oldtail = nodes(full)
        promoted, _ = nodes(full.append(64))
        self.assertIs(promoted[0], oldroot)
        self.assertIs(promoted[1], oldtail)

    def test_allocation_failure_cleanup(self):
        # Allocator hooks are process-global: isolate them from the test runner.
        code = '''
import _testcapi
from grail import PVector

small = PVector(range(100))
full = PVector(range(1056))
deep = PVector(range(33000))
models = [(small, list(small)), (full, list(full)), (deep, list(deep))]
source = tuple(range(1057))

def build(v):
    with v.mutate() as t:
        t[5] = -5
        t.extend(range(70))
        t[-1] = -1
        t.pop()
        t.pop()
    return t.finish()

operations = [
    lambda: build(small),
    lambda: build(deep),
    lambda: deep + small,
    lambda: small.append(100),
    lambda: full.append(1056),
    lambda: deep.set(0, -1),
    lambda: full.append(1056).pop(),
    lambda: deep[::-2],
    lambda: small + full,
    lambda: PVector(source),
]
for operation in operations:
    expected = operation()
    failures = 0
    for position in range(40):
        failed = False
        result = None
        try:
            _testcapi.set_nomemory(position, position + 1)
            result = operation()
        except MemoryError:
            failed = True
        finally:
            _testcapi.remove_mem_hooks()
        failures += failed
        if not failed:
            assert result == expected
        for original, model in models:
            assert list(original) == model
    assert failures > 0

# A builder stays consistent after an operation fails.
for position in range(60):
    t = deep.mutate()
    try:
        _testcapi.set_nomemory(position, position + 1)
        for i in range(0, 33000, 997):
            t[i] = -i
        for i in range(100):
            t.append(i)
        for i in range(40):
            t.pop()
    except MemoryError:
        pass
    finally:
        _testcapi.remove_mem_hooks()
    seen = [t[i] for i in range(len(t))]
    assert seen == list(t.finish())
    assert list(deep) == list(range(33000))
'''
        assert_python_ok("-c", code)

class NumericVectorNativeTests(unittest.TestCase):
    """FloatVector and IntVector internals."""

    def test_allocation_failure_cleanup(self):
        # Allocator hooks are process-global: isolate them from the test runner.
        code = '''
import _testcapi
from grail import FloatVector, IntVector

small = FloatVector(range(10))
large = IntVector(range(5000))
models = [(small, list(small)), (large, list(large))]

def build(v):
    with v.mutate() as t:
        t[5] = -5
        t.extend(range(70))
        t[-1] = -1
        t.pop()
    return t.finish()

operations = [
    lambda: build(small),
    lambda: build(large),
    lambda: large + large,
    lambda: small.append(1),
    lambda: large.set(0, -1),
    lambda: large.pop(),
    lambda: large[::-2],
    lambda: FloatVector(list(range(100))),
    lambda: IntVector(iter(range(100))),
    lambda: small.tolist(),
    lambda: repr(small),
    lambda: list(reversed(small)),
]
for operation in operations:
    expected = operation()
    failures = 0
    for position in range(40):
        failed = False
        result = None
        try:
            _testcapi.set_nomemory(position, position + 1)
            result = operation()
        except MemoryError:
            failed = True
        finally:
            _testcapi.remove_mem_hooks()
        failures += failed
        if not failed:
            assert result == expected
        for original, model in models:
            assert list(original) == model
    assert failures > 0

# A builder stays consistent after an operation fails.
for position in range(40):
    t = small.mutate()
    try:
        _testcapi.set_nomemory(position, position + 1)
        for i in range(500):
            t.append(i)
        t.extend(range(500))
        t.extend(list(range(500)))
    except MemoryError:
        pass
    finally:
        _testcapi.remove_mem_hooks()
    seen = [t[i] for i in range(len(t))]
    assert seen == list(t.finish())
'''
        assert_python_ok("-c", code)

if __name__ == "__main__":
    unittest.main()
