# Copied from grail-compat tests/ by tools/sync_to_proto.py; do not edit here.
"""Semantic tests for Grail, shared by the native runtime and the shim.

Runs against whichever ``grail`` is importable: the native prototype
(``_grail``) or the pure-Python shim (``grail._core``).  cpython-proto runs
a copy as Lib/test/test_grail.py; tests of its internals (dispatch caches,
operator dispatch for non-record types, which needs interpreter support)
are in its test_grail_native.py.  Native ablation switches are exercised
only on the native runtime.

Runs under pytest and ``python -m unittest`` (no pytest import).
"""
import abc
import concurrent.futures
import copy
import functools
import gc
import io
import operator
import os
import pickle
import struct
import subprocess
import sys
import sysconfig
import threading
import unittest
import weakref

import grail

# The private core: _grail natively, grail._core in the shim.
_grail = grail._core

SHIM = grail.backend == "shim"
NATIVE = not SHIM

# Subprocesses keep the GIL disabled on free-threaded builds.
GIL_ARGS = ("-X", "gil=0") if sysconfig.get_config_var("Py_GIL_DISABLED") else ()

# Generous bound for every wait in threaded tests: a bug fails, never hangs.
TIMEOUT = 120

# copy.replace is new in 3.13; records support __replace__ everywhere.
HAS_COPY_REPLACE = hasattr(copy, "replace")
needs_copy_replace = unittest.skipUnless(HAS_COPY_REPLACE, "copy.replace needs 3.13+")


def copy_replace(obj, /, **changes):
    if HAS_COPY_REPLACE:
        return copy.replace(obj, **changes)
    return type(obj).__replace__(obj, **changes)


class threading_helper:
    @staticmethod
    def requires_working_threading():
        try:
            thread = threading.Thread(target=lambda: None)
            thread.start()
            thread.join()
        except RuntimeError:
            return unittest.skip("requires working threading")
        return lambda test: test


def Barrier(parties):
    return threading.Barrier(parties, timeout=TIMEOUT)


def assert_python_ok(*args, **env):
    environment = dict(os.environ, **env)
    result = subprocess.run([sys.executable, *args], env=environment,
                            capture_output=True, text=True, timeout=TIMEOUT)
    if result.returncode:
        raise AssertionError(f"subprocess failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
    return result


def run_ablation(code, **env):
    # PYTHON_GRAIL_* switches select native implementation paths only.
    if NATIVE:
        assert_python_ok(*GIL_ARGS, "-c", code, **env)


def run_pool(worker, count):
    with concurrent.futures.ThreadPoolExecutor(count) as pool:
        list(pool.map(worker, range(count), timeout=TIMEOUT))


RECORD_FREELIST = """
import gc, threading, grail

def make(name, n):
    ns = {"__module__": __name__,
          "__annotations__": {f"f{i}": float for i in range(n)}}
    return grail.record(type(name, (), ns))

# Records of different types and sizes reuse each other's memory; a parked
# record is untracked and its old type may die.
for rounds in range(3):
    A, B = make("A", 2), make("B", 2)
    Z, W = make("Z", 0), make("W", 12)
    a = [A(1.0, float(i)) for i in range(300)]
    z = [Z() for _ in range(300)]
    w = [W(*map(float, range(12))) for _ in range(10)]
    del a, z, w, A, Z, W
    gc.collect()
    b = [B(float(i), 2.0) for i in range(300)]
    assert all(type(x) is B and x.f0 == i and x.f1 == 2.0
               for i, x in enumerate(b))
    assert not any(type(o).__name__ == "free_record" for o in gc.get_objects())
    del b

# Per-thread freelists are released when threads exit.
C = make("C", 3)
def work():
    keep = []
    for i in range(2000):
        c = C(1.0, 2.0, float(i))
        if i % 7 == 0:
            keep.append(c)
    assert keep[-1].f2 == 1995.0
threads = [threading.Thread(target=work) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()
assert C(1.0, 2.0, 3.0) == C(1.0, 2.0, 3.0)
"""

CONSTRUCTION_PATHS = '''
import grail
_grail = grail._core

@grail.record
class Inner:
    v: float

@grail.record
class R:
    x: float
    inner: Inner
    z: int = 3

def conforming(r):
    return _grail._is_conforming(r)

i = Inner(1.0)
built = [R(1.0, i, 3), R(1.0, i), R(1.0, inner=i), R(x=1.0, inner=i, z=3),
         R.__new__(R, 1.0, i, 3), R(*(1.0, i, 3))]
for r in built:
    assert r == built[0] and r.inner is i and conforming(r), r
for r in (R(1, i, 3), R(1.0, Inner(1), 3), R(1.0, i, 3.0), R(1.0, "s", 3)):
    assert not conforming(r), r
assert R(1, i, 3).x == 1 and type(R(1, i, 3).x) is int
for args, kwargs in [((), {}), ((1.0,), {}), ((1.0, i, 3, 4), {}),
                     ((1.0, i, 3), {"x": 1.0}), ((1.0, i, 3), {"w": 1})]:
    try:
        R(*args, **kwargs)
    except TypeError:
        pass
    else:
        raise AssertionError((args, kwargs))

@grail.record
class Empty:
    pass
assert Empty() == Empty() and conforming(Empty())

@grail.record
class Wide:
    a: int; b: int; c: int; d: int; e: int; f: int; g: int; h: int; i: int
assert Wide(*range(9)) == Wide(0, 1, 2, 3, 4, 5, 6, 7, i=8)
'''


MATCH_PATHS = '''
import grail

@grail.record
class P:
    x: object
    y: object = 0
    z: object = 0

@grail.record
class Q:
    x: object

class Proxy:
    __class__ = P
    x = "proxy"

def outcome(subject):
    match subject:
        case P(a, b, c) if a == "all":
            return ("pos", a, b, c)
        case P(a, z=c, y=b) if a == "mixed":
            return ("mixed", a, b, c)
        case P(z=c) if c == "kw":
            return ("kw", c)
        case P(__class__=c, y=b) if b == "nonfield":
            return ("nonfield", c.__name__)
        case P(a):
            return ("one", a)
        case P():
            return ("bare",)
        case Q(a):
            return ("q", a)
        case _:
            return ("none",)

assert outcome(P("all", 2, 3)) == ("pos", "all", 2, 3)
assert outcome(P("mixed", 2, 3)) == ("mixed", "mixed", 2, 3)
assert outcome(P(1, z="kw")) == ("kw", "kw")
assert outcome(P(1, "nonfield")) == ("nonfield", "P")
assert outcome(P(1)) == ("one", 1)
assert outcome(Proxy()) == ("one", "proxy")
assert outcome(Q(5)) == ("q", 5)
assert outcome(3) == ("none",)

def other(subject):
    match subject:
        case Q(a):
            return ("q", a)
        case object(__class__=c):
            return ("object", c.__name__)
# A record of another record type is not an instance; non-record patterns
# on records take the generic path.
assert other(P(1)) == ("object", "P")
assert other(Q(1)) == ("q", 1)

def error(subject, case):
    try:
        exec("match subject:\\n    " + case + ": pass",
             {"P": P, "subject": subject})
    except TypeError as e:
        message = str(e)  # drop the module name, if any
        prefix = P.__module__ + "."
        return message[len(prefix):] if message.startswith(prefix) else message

for subject in (P(1, 2, 3), Proxy()):
    assert error(subject, "case P(a, b, c, d)") == (
        "P() accepts 3 positional sub-patterns (4 given)")
    assert error(subject, "case P(a, x=b)") == (
        "P() got multiple sub-patterns for attribute 'x'")
    assert error(subject, "case P(missing=a)") is None
for case in ("case P(a, b, x=c)", "case P(a, b, c, y=d)"):
    assert error(P(1, 2, 3), case) == (
        "P() got multiple sub-patterns for attribute %r" % case[-4]), case
'''


REPLACE_CONFORMANCE = """
import copy, grail
_grail = grail._core

@grail.record
class P:
    x: float
    y: float

@grail.record
class R:
    p: P | None
    xs: tuple[P, ...]
    n: int

ok = R(P(1.0, 2.0), (P(3.0, 4.0),), 1)
assert _grail._is_conforming(ok)
for changes, expect in [
    ({}, True),
    ({"n": 2}, True),
    ({"n": 2.0}, False),
    ({"n": 2, "p": None}, True),
    ({"n": 2, "p": P(1, 2.0)}, False),
    ({"xs": (P(1.0, 2.0), P(1.0, "y"))}, False),
    ({"xs": ()}, True),
]:
    for replace in (copy.replace, lambda r, **kw: r.__replace__(**kw)):
        assert _grail._is_conforming(replace(ok, **changes)) is expect, changes
bad = R(P(1.0, 2.0), (P(3.0, 4.0),), "n")
assert not _grail._is_conforming(bad)
assert not _grail._is_conforming(copy.replace(bad, p=None))
assert _grail._is_conforming(copy.replace(bad, n=3))
assert not _grail._is_conforming(copy.replace(bad, n=3, xs=(P(1, 2),)))
"""

REPLACE_PATHS = '''
import collections, copy, grail, types

assert isinstance(copy.replace, types.BuiltinFunctionType) == native
assert copy.replace.__module__ == "copy"

@grail.record
class Wide:
    a: int; b: int; c: int; d: int; e: int; f: int; g: int; h: int; last: int

w = Wide(*range(9))
# A key that is equal to a field name but not the interned object.
y = "".join(["la", "st"])
assert y is not "last"
for replace in (copy.replace, lambda obj, **kw: obj.__replace__(**kw)):
    assert replace(w) == w
    assert replace(w, last=10, a=-1) == Wide(-1, *range(1, 8), 10)
    assert replace(w, **{y: 11}) == Wide(*range(8), 11)
    try:
        replace(w, z=1)
    except TypeError as e:
        assert str(e) == "unexpected record field 'z'", e
    else:
        raise AssertionError
assert Wide(*range(8), **{y: 12}) == Wide(*range(8), 12)
try:
    w.__replace__(1)
except TypeError as e:
    assert str(e) == "__replace__ accepts only keyword fields", e
else:
    raise AssertionError

T = collections.namedtuple("T", "obj b")
assert copy.replace(T(1, 2), obj=5) == T(5, 2)
class NoReplace:
    __replace__ = None
for bad in (3, NoReplace()):
    try:
        copy.replace(bad)
    except TypeError as e:
        assert str(e) == f"replace() does not support {type(bad).__name__} objects", e
    else:
        raise AssertionError
for args in ((), (w, w)):
    try:
        copy.replace(*args)
    except TypeError:
        pass
    else:
        raise AssertionError
'''


HOOK_PATHS = '''
import grail
_grail = grail._core

@grail.record
class V:
    x: float
    y: float = 0.0

@grail.record
class W:
    x: float
    y: float = 0.0

@grail.record
class T:
    names: tuple[str, ...]

@grail.record
class O:
    names: object

class S(str):
    pass

for r in (V(1.0, 2.0), V(1, 2), T(("a", "bc")), O(("a", "bc")), O(S("x"))):
    assert hash(r) == grail.hash(r) == _grail._default_hash(r), r
    assert r == r and not (r != r)
# A conforming tuple field hashes like the equal tuple in an ordinary field,
# and like an equal nonconforming tuple (full-width str hashes included).
assert T(("a", "bc")) == T((S("a"), "bc"))
assert hash(T(("a", "bc"))) == hash(T((S("a"), "bc")))
assert V(1.0) == V(1) and hash(V(1.0)) == hash(V(1))
assert V(1.0) != W(1.0) and not (V(1.0) == W(1.0))
assert V(1.0) != V(2.0) and not (V(1.0) == V(2.0))
assert V(1.0).__eq__(W(1.0)) is NotImplemented
try:
    hash(O([]))
except TypeError:
    pass
else:
    raise AssertionError
d = {V(float(i)): i for i in range(50)}
assert all(d[V(float(i))] == i for i in range(50))
'''


def isolated_registrations(test):
    # Protocol/operator registries intentionally own registered types for the
    # interpreter lifetime. Isolate tests that extend those global registries
    # so refleak repetitions do not accumulate fresh local types/callables.
    # The shim has no per-interpreter state to isolate: run directly.
    if SHIM:
        return test
    @functools.wraps(test)
    def run(self):
        import _interpreters
        ident = _interpreters.create()
        directory, filename = os.path.split(os.path.abspath(__file__))
        module = os.path.splitext(filename)[0]
        try:
            result = _interpreters.exec(ident, f'''
import importlib, sys
sys.path.insert(0, {directory!r})
test_grail = importlib.import_module({module!r})
case = test_grail.{type(self).__name__}({test.__name__!r})
method = getattr(case, {test.__name__!r})
method.__wrapped__(case)
''')
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)
    return run


class RecordTests(unittest.TestCase):
    def point_type(self):
        @grail.record
        class Point:
            x: float
            y: float = 0.0
        return Point

    def test_construction(self):
        Point = self.point_type()
        p = Point(1.5)
        self.assertIs(type(p), Point)
        self.assertIs(p.__class__, Point)
        self.assertEqual((p.x, p.y), (1.5, 0.0))
        self.assertEqual(Point(y=2.0, x=1.0), Point(1.0, 2.0))
        self.assertEqual(Point.__new__(Point, 1.0), Point(1.0))
        for args, kwargs in [((), {}), ((1, 2, 3), {}), ((1,), {"x": 2}),
                             ((), {"z": 2})]:
            with self.subTest(args=args, kwargs=kwargs), self.assertRaises(TypeError):
                Point(*args, **kwargs)

    def test_empty(self):
        @grail.record
        class Empty:
            pass
        self.assertTrue(grail.is_value_record(Empty))
        self.assertTrue(_grail._is_conforming(Empty()))
        self.assertEqual(repr(Empty()), "Empty()")
        self.assertEqual(Empty(), Empty())
        self.assertEqual(grail.record_values(Empty()), {})

    @unittest.skipIf(SHIM and grail._core._freeze is None,
                     "shim cannot harden types without ctypes")
    def test_mutation_bypasses(self):
        Point = self.point_type()
        p = Point(1.0)
        attempts = [lambda: setattr(p, "x", 3), lambda: delattr(p, "x"),
                    lambda: setattr(p, "other", 3),
                    lambda: object.__setattr__(p, "x", 3),
                    lambda: object.__delattr__(p, "x"),
                    lambda: Point.x.__set__(p, 3),
                    lambda: setattr(p, "__class__", object),
                    lambda: object.__setattr__(p, "__class__", object),
                    lambda: setattr(Point, "x", 3),
                    lambda: type.__setattr__(Point, "x", 3),
                    lambda: type("Child", (Point,), {}),
                    lambda: object.__new__(Point),
                    lambda: weakref.ref(p)]
        for attempt in attempts:
            with self.subTest(attempt=attempt), self.assertRaises((TypeError, AttributeError)):
                attempt()
        self.assertFalse(hasattr(p, "__dict__"))
        self.assertEqual(p.x, 1.0)
        self.assertEqual(Point.__bases__, (object,))
        self.assertIs(type(Point), type)

    def test_reject_behavior(self):
        bodies = ["def method(self): pass", "def __init__(self): pass",
                  "def __new__(cls): pass", "def __del__(self): pass",
                  "def __doc__(self): pass",
                  "x = property(lambda self: 1)", "__slots__ = ()",
                  "x = staticmethod(lambda: 1)", "__class__: int"]
        for body in bodies:
            with self.subTest(body=body), self.assertRaises(TypeError):
                exec("@grail.record\nclass Bad:\n " + body, {"grail": grail})
        class Base:
            pass
        with self.assertRaises(TypeError):
            grail.record(type("Bad", (Base,), {}))
        class Meta(type):
            pass
        with self.assertRaises(TypeError):
            grail.record(Meta("Bad", (), {}))

    def test_defaults_and_factories(self):
        calls = []
        def factory(n):
            def run():
                gc.collect()
                calls.append(n)
                return [n]
            return run
        @grail.record
        class R:
            a: list = grail.field(factory=factory(1))
            b: list = grail.field(factory=factory(2))
        x = R()
        self.assertEqual(calls, [1, 2])
        y = R(a=[])
        self.assertEqual(calls, [1, 2, 2])
        self.assertIsNot(x.a, y.a)
        for value in ([], {}, set()):
            with self.assertRaises(ValueError):
                grail.record(type("Bad", (), {"x": value}))
        with self.assertRaises(TypeError):
            grail.field(default=1, factory=list)
        with self.assertRaises(TypeError):
            grail.field(factory=3)

    def test_invalid_module_metadata(self):
        class Bad:
            __module__ = 123
            x: int
        with self.assertRaises(TypeError):
            grail.record(Bad)

    def test_unresolved_annotations_are_conservative(self):
        namespace = {"grail": grail}
        exec("from __future__ import annotations\n"
             "@grail.record\nclass R:\n x: MissingType\n", namespace)
        R = namespace["R"]
        self.assertFalse(grail.is_value_record(R))
        self.assertEqual(R(3).x, 3)
        namespace["MissingType"] = int
        self.assertFalse(grail.is_value_record(R))

    def test_factory_failure(self):
        events = []
        def fail():
            events.append("failed")
            raise RuntimeError("factory")
        @grail.record
        class R:
            x: list = grail.field(factory=list)
            y: object = grail.field(factory=fail)
        for _ in range(10):
            with self.assertRaisesRegex(RuntimeError, "factory"):
                R()
        self.assertEqual(len(events), 10)
        self.assertEqual(R(y=5).y, 5)

    def test_replace(self):
        Point = self.point_type()
        p = Point(1.0)
        q = copy_replace(p, y=2.5)
        self.assertIs(type(q), Point)
        self.assertEqual((p.y, q.y), (0.0, 2.5))
        self.assertTrue(_grail._is_conforming(q))
        self.assertFalse(_grail._is_conforming(copy_replace(p, x="bad")))
        with self.assertRaises(TypeError):
            copy_replace(p, z=1)
        with self.assertRaises(TypeError):
            p.__replace__(1)

    @needs_copy_replace
    def test_replace_paths(self):
        # copy.replace is native unless disabled; both must agree.
        # The shim leaves copy.replace to the standard library.
        exec(REPLACE_PATHS, {"native": NATIVE})
        run_ablation("native = False\n" + REPLACE_PATHS,
                     PYTHON_GRAIL_COPY_REPLACE="0")

    @needs_copy_replace
    def test_replace_conformance(self):
        # A conforming source checks only the replaced fields; any other
        # source checks them all. Both must agree with the full check.
        exec(REPLACE_CONFORMANCE, {})
        run_ablation(REPLACE_CONFORMANCE, PYTHON_GRAIL_REPLACE_CHANGED_ONLY="0")

    def test_replace_does_not_run_defaults(self):
        @grail.record
        class R:
            x: list = grail.field(factory=list)
        r = R()
        self.assertIs(copy_replace(r).x, r.x)

    def test_conformance(self):
        Point = self.point_type()
        self.assertTrue(grail.is_value_record(Point))
        self.assertTrue(_grail._is_conforming(Point(1.0)))
        for value in (1, True, "wrong", []):
            p = Point(value)
            self.assertIs(p.x, value)
            self.assertFalse(_grail._is_conforming(p))
        self.assertEqual(Point(1), Point(1.0))
        self.assertEqual(hash(Point(1)), hash(Point(1.0)))
        @grail.record
        class Ordinary:
            x: object
        self.assertFalse(grail.is_value_record(Ordinary))
        self.assertFalse(_grail._is_conforming(Ordinary(1)))

    def test_record_freelist(self):
        exec(RECORD_FREELIST, {})
        run_ablation(RECORD_FREELIST, PYTHON_GRAIL_RECORD_FREELIST="0")

    def test_construction_paths(self):
        # Positional, keyword, default and __new__ construction must agree;
        # the staged path is also checked with the fast path disabled.
        code = CONSTRUCTION_PATHS
        exec(code, {})
        run_ablation(code, PYTHON_GRAIL_POSITIONAL_CONSTRUCTION="0")

    def test_nested_plans(self):
        Point = self.point_type()
        @grail.record
        class Scene:
            p: Point | None
            fixed: tuple[int, float]
            points: tuple[Point, ...]
        self.assertTrue(grail.is_value_record(Scene))
        r = Scene(Point(1.0), (2, 3.0), (Point(2.0),))
        self.assertTrue(_grail._is_conforming(r))
        for bad in [copy_replace(r, fixed=(True, 3.0)),
                    copy_replace(r, points=(Point(1),)),
                    copy_replace(r, p="bad")]:
            self.assertFalse(_grail._is_conforming(bad))
        self.assertEqual(grail.substitutable(r, copy_replace(r)), True)

    def test_deep_plans(self):
        # Children of nested unions/tuples are laid out after their parents.
        Point = self.point_type()
        @grail.record
        class Deep:
            a: tuple[tuple[int, Point | None], ...] | None
            b: tuple[int | tuple[float, ...], Point]
            c: int
        self.assertTrue(grail.is_value_record(Deep))
        good = Deep(((1, Point(1.0)), (2, None)), ((1.0, 2.0), Point(3.0)), 4)
        self.assertTrue(_grail._is_conforming(good))
        self.assertTrue(_grail._is_conforming(Deep(None, (5, Point(1.0)), 4)))
        for bad in [Deep(((1, Point(1)),), (5, Point(1.0)), 4),
                    Deep(((1, 2),), (5, Point(1.0)), 4),
                    Deep(None, ((1.0, 2),), 4),
                    Deep(None, ((1.0,), Point(1.0)), 4.0),
                    Deep(None, (5, Point(1.0), 6), 4)]:
            self.assertFalse(_grail._is_conforming(bad))
        nan = float("nan")
        x = Deep(((1, Point(nan)),), ((nan,), Point(1.0)), 4)
        y = Deep(((1, Point(nan)),), ((nan,), Point(1.0)), 4)
        self.assertTrue(_grail._is_conforming(x))
        self.assertEqual(x, y)
        self.assertEqual(hash(x), hash(y))

    def test_metadata_snapshot(self):
        class Source:
            x: int
        annotations = Source.__annotations__
        R = grail.record(Source)
        annotations["x"] = list
        self.assertTrue(grail.is_value_record(R))
        self.assertTrue(_grail._is_conforming(R(1)))
        fields = grail.record_fields(R)
        self.assertEqual(fields[0].name, "x")
        self.assertEqual(fields[0].annotation, int)
        self.assertEqual(fields[0].kind, "value")
        values = grail.record_values(R(1))
        values["x"] = 3
        self.assertEqual(grail.record_values(R(1)), {"x": 1})
        for func in (grail.record_fields, grail.is_value_record):
            with self.assertRaises(TypeError):
                func(int)
        with self.assertRaises(TypeError):
            grail.record_values(1)

    def test_ordinary_identity_and_hash(self):
        @grail.record
        class R:
            x: list
        a, b = [], []
        self.assertEqual(R(a), R(b))
        self.assertFalse(grail.substitutable(R(a), R(b)))
        self.assertTrue(grail.substitutable(R(a), R(a)))
        with self.assertRaises(TypeError):
            hash(R(a))

    def test_float_representations(self):
        Point = self.point_type()
        positive, negative = Point(0.0), Point(-0.0)
        self.assertEqual(positive, negative)
        self.assertFalse(grail.substitutable(positive, negative))
        self.assertEqual(hash(positive), hash(negative))
        nan1 = struct.unpack("!d", bytes.fromhex("7ff8000000000042"))[0]
        nan2 = struct.unpack("!d", bytes.fromhex("7ff8000000000042"))[0]
        nan3 = struct.unpack("!d", bytes.fromhex("7ff8000000000043"))[0]
        self.assertIsNot(nan1, nan2)
        self.assertEqual(Point(nan1), Point(nan2))
        self.assertEqual(hash(Point(nan1)), hash(Point(nan2)))
        self.assertTrue(grail.substitutable(Point(nan1), Point(nan2)))
        self.assertNotEqual(Point(nan1), Point(nan3))
        self.assertFalse(grail.substitutable(Point(nan1), Point(nan3)))
        self.assertEqual(Point(nan1), Point(nan1, 0))
        self.assertEqual(hash(Point(nan1)), hash(Point(nan1, 0)))

    def test_hook_paths(self):
        # Native hash/== must agree with calling the default hooks.
        exec(HOOK_PATHS, {})
        run_ablation(HOOK_PATHS, PYTHON_GRAIL_NATIVE_HOOKS="0")

    def test_tuple_nan_hash(self):
        @grail.record
        class R:
            x: tuple[float, ...]
        a = float("nan")
        b = float("nan")
        self.assertEqual(R((a,)), R((b,)))
        self.assertEqual(hash(R((a,))), hash(R((b,))))

    def test_distinct_record_types(self):
        A, B = self.point_type(), self.point_type()
        self.assertNotEqual(A(1.0), B(1.0))
        self.assertFalse(grail.substitutable(A(1.0), B(1.0)))
        self.assertNotEqual(A(1.0), 3)
        with self.assertRaises(TypeError):
            A(1.0) < A(2.0)

    def test_class_pattern(self):
        Point = self.point_type()
        p = Point(1.0, 2.0)
        match p:
            case Point(x, y=other):
                self.assertEqual((x, other), (1.0, 2.0))
            case _:
                self.fail("record did not match")
        with self.assertRaises(TypeError):
            match p:
                case Point(a, b, c):
                    pass

    def test_match_paths(self):
        # Exact records read fields by index; the generic __match_args__
        # path must give the same results and errors.
        exec(MATCH_PATHS, {})
        run_ablation(MATCH_PATHS, PYTHON_GRAIL_RECORD_MATCH="0")

    def test_repr_cycle_and_gc(self):
        @grail.record
        class Node:
            children: list
        class Payload:
            pass
        payload = Payload()
        ref = weakref.ref(payload)
        children = [payload]
        r = Node(children)
        self.assertTrue(gc.is_tracked(r))
        children.append(r)
        self.assertIn("...", repr(r))
        del r, children, payload
        gc.collect()
        self.assertIsNone(ref())

    def test_type_metadata_cycle(self):
        class Default:
            pass
        default = Default()
        @grail.record
        class R:
            x: object = default
        default.owner = R
        type_ref, default_ref = weakref.ref(R), weakref.ref(default)
        self.assertIs(R().x, default)
        del R, default
        gc.collect()
        self.assertIsNone(default_ref())
        self.assertIsNone(type_ref())

    def test_deep_deallocation(self):
        @grail.record
        class Link:
            next: object
        r = None
        for _ in range(20000):
            r = Link(r)
        del r
        gc.collect()

    @isolated_registrations
    def test_hook_registration(self):
        Point = self.point_type()
        @grail.repr.impl
        def custom(p: Point):
            return f"custom({p.x})"
        self.assertEqual(repr(Point(2.0)), "custom(2.0)")
        @grail.hash.impl
        def custom_hash(p: Point):
            return 123
        self.assertEqual(hash(Point(2.0)), 123)
        @grail.eq.impl
        def custom_eq(p: Point, other):
            return isinstance(other, Point)
        self.assertEqual(Point(2.0), Point(3.0))
        self.assertFalse(Point(2.0) != Point(3.0))

    @isolated_registrations
    def test_hash_registration_after_hashing(self):
        # An implementation may cache hashes, but never across a change to
        # the selected hash, including one for a nested value's type.
        Point = self.point_type()
        @grail.record
        class Segment:
            start: Point
            end: Point
        p = Point(2.0)
        s = Segment(p, Point(3.0))
        default = hash(s)
        self.assertEqual(hash(s), default)
        hash(p)
        grail.hash.register(Point, lambda p: 123)
        self.assertEqual(hash(p), 123)
        self.assertEqual(hash(s), hash((123, 123)))
        grail.hash.override(Point, lambda p: 456)
        self.assertEqual(hash(p), 456)
        self.assertEqual(hash(s), hash((456, 456)))
        grail.hash.register(Segment, lambda s: 789)
        self.assertEqual(hash(s), 789)

    def test_private_schema_validation(self):
        class Source:
            pass
        for names, plans, defaults, annotations in [
            (1, (), (), ()),
            (("x",), (), (), ()),
            (("x",), ((9, ()),), ((0, None),), (int,)),
            (("__class__",), (None,), ((0, None),), (int,)),
            (("x",), (int,), ((2, 123),), (int,)),
        ]:
            with self.assertRaises(TypeError):
                _grail._make_record(Source, names, plans, defaults, annotations)


# Public, module-resolvable fixtures for both pickle engines.
_pickle_factory_calls = []


def _pickle_factory():
    _pickle_factory_calls.append(1)
    return []


@grail.record
class PickleRecord:
    first: object
    second: object = grail.field(factory=_pickle_factory)


@grail.record
class PickleValue:
    values: tuple[int, ...]


class PickleNamespace:
    @grail.record
    class Nested:
        value: int


class PickleObserver:
    def __setstate__(self, state):
        self.__dict__.update(state)
        # The ordinary object's state can be incomplete during reconstruction;
        # its record owner must already have every immutable field installed.
        assert self.owner.first is self
        assert self.owner.second == 42
        assert tuple(grail.record_values(self.owner)) == ("first", "second")
        try:
            self.owner.first = None
        except AttributeError:
            pass
        else:
            raise AssertionError("restored record is mutable")
        self.observed = True


_pickle_restore_refs = []


class PickleTracked:
    def __new__(cls):
        obj = super().__new__(cls)
        _pickle_restore_refs.append(weakref.ref(obj))
        return obj


def _pickle_fail():
    raise RuntimeError("restore failed")


class PickleFailure:
    def __reduce__(self):
        return _pickle_fail, ()


class CopyPickleTests(unittest.TestCase):
    def roundtrips(self, obj):
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            for pickler in (pickle.Pickler, pickle._Pickler):
                stream = io.BytesIO()
                pickler(stream, protocol=protocol).dump(obj)
                for unpickler in (pickle.Unpickler, pickle._Unpickler):
                    with self.subTest(protocol=protocol, pickler=pickler,
                                      unpickler=unpickler):
                        yield unpickler(io.BytesIO(stream.getvalue())).load()

    def test_shallow_copy(self):
        for record in (PickleValue((1, 2)), PickleValue([]),
                       PickleRecord([], [])):
            self.assertIs(copy.copy(record), record)
            self.assertIs(record.__copy__(), record)

    def test_deepcopy_conformance_not_declaration(self):
        good = PickleValue((1, 2))
        self.assertTrue(grail.is_value_record(PickleValue))
        self.assertIs(copy.deepcopy(good), good)
        bad = PickleValue([[]])
        result = copy.deepcopy([good, bad, bad])
        self.assertIs(result[0], good)
        self.assertIs(result[1], result[2])
        self.assertIsNot(result[1], bad)
        self.assertIsNot(result[1].values, bad.values)
        self.assertIsNot(result[1].values[0], bad.values[0])
        # A conforming nested record does not make its ordinary owner a value.
        root = PickleRecord(good, [bad, good])
        cloned = copy.deepcopy(root)
        self.assertIs(cloned.first, good)
        self.assertIs(cloned.second[1], good)
        self.assertIsNot(cloned.second[0], bad)
        with self.assertRaises(TypeError):
            root.__deepcopy__([])

    def test_deepcopy_is_self(self):
        @grail.record
        class Value:
            values: tuple[int, ...]
        @grail.record
        class Loose:
            values: object
        self.assertTrue(_grail._deepcopy_is_self(Value((1,))))
        self.assertFalse(_grail._deepcopy_is_self(Loose([])))
        for obj in (1, [], (1,), Value, object()):
            self.assertFalse(_grail._deepcopy_is_self(obj))

    def test_deepcopy_nested_value(self):
        @grail.record
        class Outer:
            values: tuple[PickleValue, ...]
        obj = Outer((PickleValue((1,)),))
        self.assertTrue(_grail._is_conforming(obj))
        self.assertIs(copy.deepcopy(obj), obj)
        bad = Outer((PickleValue([]),))
        self.assertFalse(_grail._is_conforming(bad))
        cloned = copy.deepcopy(bad)
        self.assertIsNot(cloned, bad)
        self.assertIsNot(cloned.values[0].values, bad.values[0].values)

    def test_cycles_and_aliases(self):
        for nonconforming in (False, True):
            children = []
            record = (PickleValue(children) if nonconforming else
                      PickleRecord(children, children))
            children.extend([record, record, (record, children)])
            for root in (record, children):
                with self.subTest(nonconforming=nonconforming,
                                  record_root=root is record):
                    copies = [copy.deepcopy(root), *self.roundtrips(root)]
                    for cloned in copies:
                        r = cloned if root is record else cloned[0]
                        c = r.values if nonconforming else r.first
                        self.assertIsNot(r, record)
                        self.assertIsNot(c, children)
                        self.assertIs(c[0], r)
                        self.assertIs(c[1], r)
                        self.assertIs(c[2][0], r)
                        self.assertIs(c[2][1], c)
                        if not nonconforming:
                            self.assertIs(r.second, c)
                        if root is children:
                            self.assertIs(cloned, c)
                        with self.assertRaises((TypeError, AttributeError)):
                            object.__setattr__(r, "values" if nonconforming
                                               else "first", None)

    def test_mixed_graph(self):
        value = PickleValue((1, 2))
        ordinary = []
        record = PickleRecord((ordinary, value), ordinary)
        ordinary.extend([record, value])
        root = [record, ordinary, value, record]
        for cloned in [copy.deepcopy(root), *self.roundtrips(root)]:
            self.assertIs(cloned[0], cloned[3])
            self.assertIs(cloned[0].first[0], cloned[1])
            self.assertIs(cloned[0].second, cloned[1])
            self.assertIs(cloned[1][0], cloned[0])
            self.assertIs(cloned[1][1], cloned[2])
            self.assertIs(cloned[0].first[1], cloned[2])
            self.assertEqual(cloned[2], value)

    def test_defaults_not_replayed(self):
        obj = PickleRecord(1)
        calls = len(_pickle_factory_calls)
        for cloned in [copy.deepcopy(obj), *self.roundtrips(obj)]:
            self.assertEqual(cloned.first, 1)
            self.assertEqual(cloned.second, [])
            self.assertIsNot(cloned.second, obj.second)
        self.assertEqual(len(_pickle_factory_calls), calls)

    def test_public_type_resolution(self):
        for obj in (PickleNamespace.Nested(42), PickleValue((1, 2))):
            for cloned in self.roundtrips(obj):
                self.assertIs(type(cloned), type(obj))
                self.assertEqual(cloned, obj)
        @grail.record
        class Local:
            value: int
        for pickler in (pickle.Pickler, pickle._Pickler):
            with self.assertRaises((pickle.PicklingError, AttributeError)):
                pickler(io.BytesIO()).dump(Local(1))
        original = globals()["PickleValue"]
        try:
            globals()["PickleValue"] = int
            for pickler in (pickle.Pickler, pickle._Pickler):
                with self.assertRaises(pickle.PicklingError):
                    pickler(io.BytesIO()).dump(original((1,)))
        finally:
            globals()["PickleValue"] = original

    def test_reducer_and_malformed_reconstruction(self):
        obj = PickleRecord(1, 2)
        for reduction in (obj.__reduce__(), obj.__reduce_ex__(5)):
            constructor, args = reduction
            self.assertIs(constructor, PickleRecord)
            self.assertEqual(args, (1, 2))
            self.assertEqual(constructor(*args), obj)
        class Bad:
            def __reduce__(self):
                return PickleRecord, (1, 2, 3)
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            data = pickle.dumps(Bad(), protocol)
            for unpickler in (pickle.Unpickler, pickle._Unpickler):
                with self.assertRaises(TypeError):
                    unpickler(io.BytesIO(data)).load()

    def test_state_callbacks_observe_complete_record(self):
        observer = PickleObserver()
        record = PickleRecord(observer, 42)
        observer.owner = record
        for cloned in [copy.deepcopy(record), *self.roundtrips(record)]:
            self.assertTrue(cloned.first.observed)
            self.assertIs(cloned.first.owner, cloned)
            self.assertEqual(cloned.second, 42)

    def test_pickle_failure_cleanup(self):
        obj = PickleRecord(PickleTracked(), PickleFailure())
        _pickle_restore_refs.clear()
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            for pickler in (pickle.Pickler, pickle._Pickler):
                stream = io.BytesIO()
                pickler(stream, protocol=protocol).dump(obj)
                for unpickler in (pickle.Unpickler, pickle._Unpickler):
                    with self.assertRaisesRegex(RuntimeError, "restore failed"):
                        unpickler(io.BytesIO(stream.getvalue())).load()
        gc.collect()
        self.assertTrue(_pickle_restore_refs)
        self.assertTrue(all(ref() is None for ref in _pickle_restore_refs))
        _pickle_restore_refs.clear()

    def test_recursive_callback_observes_only_complete_record(self):
        seen = []
        class Callback:
            def __deepcopy__(self, memo):
                # Re-enter once; the mutable ancestor terminates recursion.
                if not seen:
                    seen.append(None)
                    result = copy.deepcopy(record, memo)
                    self_test.assertEqual(tuple(grail.record_values(result)),
                                          ("first", "second"))
                    self_test.assertIs(result.second, 42)
                    with self_test.assertRaises(AttributeError):
                        result.first = None
                    seen[0] = result
                return "copied"
        self_test = self
        children = []
        record = PickleRecord(children, 42)
        children.append(Callback())
        cloned = copy.deepcopy(record)
        self.assertIs(cloned, seen[0])
        self.assertEqual(cloned.first, ["copied"])

    def test_copy_failure_cleanup(self):
        refs = []
        class Payload:
            def __deepcopy__(self, memo):
                result = Payload()
                refs.append(weakref.ref(result))
                return result
        class Failure:
            def __deepcopy__(self, memo):
                gc.collect()
                raise RuntimeError("copy failed")
        obj = PickleRecord(Payload(), Failure())
        for _ in range(3):
            with self.assertRaisesRegex(RuntimeError, "copy failed"):
                copy.deepcopy(obj)
        gc.collect()
        self.assertTrue(all(ref() is None for ref in refs))
        self.assertIsInstance(obj.first, Payload)

    @threading_helper.requires_working_threading()
    def test_concurrent_copy(self):
        value = PickleValue((1, 2))
        record = PickleRecord([value], value)
        def worker(_):
            for _ in range(100):
                cloned = copy.deepcopy(record)
                self.assertIs(cloned.first[0], value)
                self.assertIs(cloned.second, value)
                self.assertIsNot(cloned.first, record.first)
        run_pool(worker, 4)


class GenericTests(unittest.TestCase):
    def test_exact_dispatch_and_fallback(self):
        @grail.generic
        def render(obj, suffix="!", *, upper=False):
            return "fallback" + suffix
        @render.impl
        def _(obj: int, suffix="!", *, upper=False):
            return str(obj) + suffix
        self.assertEqual(render(4), "4!")
        self.assertEqual(render(obj=5, suffix="?"), "5?")
        self.assertEqual(render("x"), "fallback!")
        self.assertEqual(render.__name__, "render")
        self.assertIs(render.dispatch(int), _)
        self.assertIs(render.dispatch(str), render.fallback)
        self.assertEqual(render.version, 1)
        with self.assertRaises(TypeError):
            render()
        with self.assertRaises(TypeError):
            render(1, obj=2)

    def test_override_and_duplicate(self):
        @grail.generic
        def f(obj):
            return 0
        f.register(int, lambda obj: 1)
        with self.assertRaises(TypeError):
            f.register(int, lambda obj: 2)
        self.assertEqual(f(1), 1)
        f.override(int, lambda obj: 3)
        self.assertEqual(f(1), 3)
        @f.override
        def _(obj: int):
            return 4
        self.assertEqual(f(1), 4)
        self.assertEqual(f.version, 3)

    def test_registration_validation(self):
        @grail.generic
        def f(obj, n=1):
            return n
        with self.assertRaises(TypeError):
            f.register(int, lambda obj: 1)
        with self.assertRaises(TypeError):
            f.register(123, lambda obj, n=1: 1)
        with self.assertRaises(TypeError):
            f.impl(lambda obj, n=1: 1)
        with self.assertRaises(TypeError):
            grail.generic(lambda *, obj: obj)

    def test_mro_dispatch(self):
        class Base:
            pass
        class Child(Base):
            pass
        @grail.generic
        def f(obj):
            return "fallback"
        base_impl = lambda obj: "base"
        f.register(Base, base_impl)
        self.assertEqual(f(Base()), "base")
        self.assertEqual(f(Child()), "base")
        self.assertEqual(f(obj=Child()), "base")
        self.assertIs(f.dispatch(Child), base_impl)
        f.register(Child, lambda obj: "child")
        self.assertEqual(f(Child()), "child")
        f.override(Base, lambda obj: "new base")
        self.assertEqual(f(Base()), "new base")
        self.assertEqual(f(Child()), "child")

    def test_matches_singledispatch(self):
        from collections.abc import Container, Iterable, Sized
        class A:
            pass
        class B(A):
            pass
        class C(B):
            pass
        class Virtual:
            pass
        Sized.register(Virtual)
        class Structural:
            def __iter__(self):
                return iter(())
        fallback = lambda obj: "fallback"
        f = grail.generic(fallback)
        reference = functools.singledispatch(fallback)
        for cls, label in ((object, "object"), (A, "A"), (B, "B"),
                           (Container, "container"), (Iterable, "iterable"),
                           (Sized, "sized")):
            def make_impl(label):
                return lambda obj: label
            impl = make_impl(label)
            f.register(cls, impl)
            reference.register(cls, impl)
            for candidate in (A, B, C, Virtual, Structural, int, str, list):
                with self.subTest(registration=cls, candidate=candidate):
                    try:
                        expected = reference.dispatch(candidate)
                    except RuntimeError:
                        with self.assertRaisesRegex(RuntimeError, "Ambiguous dispatch"):
                            f.dispatch(candidate)
                    else:
                        self.assertIs(f.dispatch(candidate), expected)
                        self.assertEqual(f(candidate()), reference(candidate()))

    def test_abc_changes_and_ambiguity(self):
        class A(abc.ABC):
            pass
        class B(abc.ABC):
            pass
        class T:
            pass
        @grail.generic
        def f(obj):
            return "fallback"
        f.register(A, lambda obj: "A")
        f.register(B, lambda obj: "B")
        self.assertEqual(f(T()), "fallback")
        A.register(T)
        self.assertEqual(f(T()), "A")
        f.override(A, lambda obj: "new A")
        self.assertEqual(f(T()), "new A")
        B.register(T)
        with self.assertRaisesRegex(RuntimeError, "Ambiguous dispatch"):
            f(T())
        f.register(T, lambda obj: "exact")
        self.assertEqual(f(T()), "exact")
        self.assertEqual(f(obj=T()), "exact")

    def test_mro_changes(self):
        class A:
            pass
        class B:
            pass
        class T(A):
            pass
        class Child(T):
            pass
        @grail.generic
        def f(obj):
            return "fallback"
        f.register(A, lambda obj: "A")
        f.register(B, lambda obj: "B")
        instance = Child()
        self.assertEqual(f(instance), "A")
        T.__bases__ = (B,)
        self.assertEqual(f(instance), "B")
        f.override(B, lambda obj: "new B")
        self.assertEqual(f(instance), "new B")

    def test_reentrant_abc_resolution_uses_snapshot(self):
        class T:
            pass
        @grail.generic
        def f(obj):
            return "fallback"
        class A(abc.ABC):
            @classmethod
            def __subclasshook__(cls, candidate):
                if candidate is T:
                    f.override(A, lambda obj: "new ABC")
                    f.register(T, lambda obj: "new exact")
                    return True
                return NotImplemented
        f.register(A, lambda obj: "old ABC")
        self.assertEqual(f(T()), "old ABC")
        self.assertEqual(f(T()), "new exact")
        self.assertEqual(f.version, 3)

    def test_function_mutation(self):
        class A:
            pass
        class B(A):
            pass
        @grail.generic
        def f(obj):
            return 0
        def impl(obj):
            return 1
        f.register(A, impl)
        self.assertEqual(f(B()), 1)
        impl.__code__ = (lambda obj: 2).__code__
        self.assertEqual(f(B()), 2)

    def test_no_metaclass_hash_callbacks(self):
        class Meta(type):
            def __hash__(self):
                raise AssertionError("must use type identity")
        class T(metaclass=Meta):
            pass
        @grail.generic
        def f(obj):
            return 0
        f.register(T, lambda obj: 3)
        self.assertEqual(f(T()), 3)

    def test_reentrant_registration(self):
        @grail.generic
        def f(obj):
            f.register(int, lambda obj: 2)
            return 1
        self.assertEqual(f(0), 1)
        self.assertEqual(f(0), 2)

    def test_retired_callable_can_register(self):
        @grail.generic
        def f(obj):
            return 0
        class Impl:
            def __call__(self, obj):
                return 1
            def __del__(self):
                f.override(int, lambda obj: 3)
        f.register(int, Impl())
        self.assertEqual(f(1), 1)
        f.override(int, lambda obj: 2)
        gc.collect()
        self.assertEqual(f(1), 3)

    def test_generic_is_read_only(self):
        @grail.generic
        def f(obj):
            return obj
        for name in ("fallback", "version", "pair", "__name__", "registry"):
            with self.assertRaises((AttributeError, TypeError)):
                setattr(f, name, None)

    @threading_helper.requires_working_threading()
    def test_cache_concurrent_instance_class_replacement(self):
        class A:
            pass
        class B:
            pass
        class Initial(A):
            pass
        value = Initial()
        f = grail.generic(lambda obj: -1)
        f.register(A, lambda obj: 0)
        f.register(B, lambda obj: 1)
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            for i in range(150):
                if index == 0:
                    cls = type("Transient", (A if i % 2 else B,), {})
                    value.__class__ = cls
                    del cls
                    if i % 10 == 0:
                        gc.collect()
                else:
                    self.assertIn(f(value), (0, 1))
                    self.assertIn(f(obj=value), (0, 1))
        run_pool(worker, 4)
        self.assertIn(f(value), (0, 1))

    @threading_helper.requires_working_threading()
    def test_lock_free_reads_during_registration(self):
        # Readers borrow published tables without locks while a writer grows
        # the exact table and retires overridden implementations.
        class A:
            pass
        types = [type(f"T{i}", (A,), {}) for i in range(40)]
        values = [cls() for cls in types]
        f = grail.generic(lambda obj: -1)
        f.register(A, lambda obj: -2)
        pair = _grail.Generic(lambda a, b: -1, "a", pair=True)
        def single(i):
            return lambda obj: i
        def double(i):
            return lambda a, b: i
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            if index == 0:
                for i, cls in enumerate(types):
                    f.register(cls, single(i))
                    pair.register((cls, cls), double(i))
                for round in range(50):
                    cls = types[round % len(types)]
                    f.override(cls, single(types.index(cls)))
                    if round % 10 == 0:
                        gc.collect()
                return
            for _ in range(20):
                for i, value in enumerate(values):
                    self.assertIn(f(value), (i, -2))
                    self.assertIn(f.dispatch(types[i])(value), (i, -2))
                    self.assertIn(pair(value, value), (i, -1))
        run_pool(worker, 4)
        for i, value in enumerate(values):
            self.assertEqual(f(value), i)
            self.assertEqual(pair(value, value), i)
        self.assertEqual(pair(values[0], values[1]), -1)

    def test_four_types(self):
        @grail.generic
        def f(obj):
            return "fallback"
        for cls in (int, float, str, bytes):
            def impl(obj):
                return type(obj).__name__
            f.register(cls, impl)
        self.assertEqual([f(x) for x in (1, 1.0, "a", b"a")],
                         ["int", "float", "str", "bytes"])

    def test_gc_cycle(self):
        class Payload:
            pass
        payload = Payload()
        ref = weakref.ref(payload)
        def make():
            value = payload
            @grail.generic
            def f(obj):
                return f, value
            return f
        f = make()
        del f, payload
        gc.collect()
        self.assertIsNone(ref())


class OperatorTests(unittest.TestCase):
    @isolated_registrations
    def test_numeric_record_pairs(self):
        @grail.record
        class Vec:
            x: float
        @grail.ops.add.impl
        def add(a: Vec, b: Vec):
            return Vec(a.x + b.x)
        @grail.ops.mul.impl
        def mul(a: Vec, b: float):
            return Vec(a.x * b)
        @grail.ops.mul.impl
        def rmul(a: float, b: Vec):
            return Vec(a * b.x)
        @grail.ops.neg.impl
        def neg(a: Vec):
            return Vec(-a.x)
        @grail.ops.abs.impl
        def absolute(a: Vec):
            return abs(a.x)
        self.assertEqual(Vec(1.0) + Vec(2.0), Vec(3.0))
        self.assertEqual(operator.add(Vec(1.0), Vec(2.0)), Vec(3.0))
        self.assertEqual(grail.ops.add(Vec(1.0), Vec(2.0)), Vec(3.0))
        self.assertEqual(Vec(2.0) * 3.0, Vec(6.0))
        self.assertEqual(3.0 * Vec(2.0), Vec(6.0))
        self.assertEqual(-Vec(2.0), Vec(-2.0))
        self.assertEqual(abs(Vec(-2.0)), 2.0)
        with self.assertRaises(TypeError):
            Vec(1.0) + 2
        with self.assertRaises(TypeError):
            Vec(1.0) * 2
        self.assertEqual(1 + 2, 3)
        self.assertEqual(grail.ops.add(1, 2), 3)
        with self.assertRaises(TypeError):
            grail.ops.neg.register(int, lambda obj: 0)


KERNEL_PATHS = '''
import dis, grail, math, operator
_grail = grail._core
from grail import _python_kernel

@grail.record(deriving="+-*/")
class V:
    x: float
    y: float
    z: float

@grail.record(deriving="+-*/")
class Wide:
    a: float; b: float; c: float; d: float; e: float; f: float; g: float
    h: float; i: float; j: float

@grail.record(deriving="+*")
class Empty:
    pass

def outcome(f, a, b):
    try:
        r = f(a, b)
    except Exception as e:
        return ("raise", type(e), str(e))
    values = tuple(_grail._record_values(r).values())
    return (type(r), _grail._is_conforming(r),
            tuple(v.hex() if type(v) is float else repr(v) for v in values))

specials = [0.0, -0.0, 1.5, -2.25, 1e308, -1e308, 5e-324, math.inf, -math.inf,
            math.nan]
records = [V(1.0, 2.0, 3.0), V(-0.0, math.inf, math.nan), V(1e308, -1e308, 5e-324),
           V(1, 2, 3), V(1.0, 2, 3.5), V("a", "b", "c"), V(True, 2.0, None)]
records += [V(x, y, z) for x, y, z in zip(specials, reversed(specials), specials[3:])]
scalars = specials + [0, 1, -3, 2**53 + 1, 2**1100]
binary = {"+": operator.add, "-": operator.sub, "*": operator.mul,
          "/": operator.truediv}
checked = 0
for symbol, op in binary.items():
    index = "+-*/".index(symbol)
    if index < 2:
        cases = [(0, a, b) for a in records for b in records]
    else:
        cases = [(1, a, k) for a in records for k in scalars]
        if index == 2:
            cases += [(2, k, a) for a in records for k in scalars]
    for kind, a, b in cases:
        reference = _python_kernel(V, index, kind)
        expected = outcome(reference, a, b)
        for call in (op, getattr(grail.ops, ("add", "sub", "mul", "truediv")[index])):
            assert outcome(call, a, b) == expected, (symbol, a, b, outcome(call, a, b), expected)
        checked += 1
assert checked > 300, checked

# Results of conforming operands are conforming; others are recomputed.
assert _grail._is_conforming(V(1.0, 2.0, 3.0) + V(1.0, 2.0, 3.0))
assert not _grail._is_conforming(V(1, 2, 3) + V(1, 2, 3))
w = Wide(*map(float, range(10)))
assert w + w == Wide(*(2.0 * i for i in range(10)))
assert 2 * w == w * 2.0 == w + w and (w / 2) * 2 == w
assert Empty() + Empty() == Empty() and Empty() * 3 == Empty()

# Warmed sites specialize to the kernel, binary and in-place; overrides
# and other pairs still take effect at those sites.
def sub(a, b): return a - b
def isub(a, b):
    a -= b
    return a
def mul(a, b): return a * b
a, b = V(1.0, 2.0, 3.0), V(0.5, 0.5, 0.5)
for f, x, y in ((sub, a, b), (isub, a, b), (mul, a, 2.0)):
    for _ in range(1000):
        f(x, y)
    if grail.backend != "shim":
        names = {i.opname for i in dis.get_instructions(f, adaptive=True)}
        assert KERNEL_OPCODE in names, (f, names)
assert sub(a, b) == V(0.5, 1.5, 2.5) and isub(a, b) == V(0.5, 1.5, 2.5)
assert mul(a, 2) == V(2.0, 4.0, 6.0) and mul(2, a) == V(2.0, 4.0, 6.0)
grail.ops.sub.override((V, V), lambda p, q: "override")
assert sub(a, b) == "override" and a - b == "override"

# Direct kernel calls check their operands.
kernel = grail.ops.add.dispatch((V, V))
assert _grail._is_kernel(kernel) == KERNELS
if KERNELS:
    assert kernel(a, b) == V(1.5, 2.5, 3.5)
    for args in ((a, 1.0), (1.0, a), (a, Wide(*range(10)))):
        try:
            kernel(*args)
        except TypeError:
            pass
        else:
            raise AssertionError(args)

# Registration rules.
for bad in ("%", "++", ("+", "+")):
    try:
        grail.record(deriving=bad)(type("B", (), {"__annotations__": {"x": float}}))
    except ValueError:
        pass
    else:
        raise AssertionError(bad)
for annotation in (int, object, float | None):
    try:
        @grail.record(deriving="+")
        class Bad:
            x: annotation
    except TypeError:
        pass
    else:
        raise AssertionError(annotation)
'''


class KernelTests(unittest.TestCase):
    @isolated_registrations
    def test_kernel_paths(self):
        # Kernels must match their Python reference bit for bit, including
        # errors, NaNs, signed zeros and nonconforming operands.
        exec(KERNEL_PATHS, {"KERNELS": True,
                            "KERNEL_OPCODE": "BINARY_OP_GRAIL_KERNEL"})

    def test_kernel_ablation(self):
        assert_python_ok(*GIL_ARGS, "-c",
                         "KERNELS = False\nKERNEL_OPCODE = 'BINARY_OP_GRAIL'\n"
                         + KERNEL_PATHS, PYTHON_GRAIL_KERNELS="0")


class ThreadTests(unittest.TestCase):
    @threading_helper.requires_working_threading()
    def test_shared_record(self):
        @grail.record
        class R:
            x: int
            y: tuple[int, ...]
        record = R(42, (1, 2, 3))
        def read():
            for _ in range(5000):
                self.assertEqual(record.x, 42)
                self.assertEqual(copy_replace(record, x=43).x, 43)
                self.assertEqual(hash(record), hash(R(42, (1, 2, 3))))
        run_pool(lambda _: read(), 4)
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    @threading_helper.requires_working_threading()
    def test_concurrent_registration(self):
        @grail.generic
        def f(obj):
            return -1
        f.register(int, lambda obj: 0)
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            for _ in range(500):
                if index < 2:
                    result = index
                    f.override(int, lambda obj: result)
                else:
                    self.assertIn(f(0), (0, 1))
        run_pool(worker, 4)
        self.assertEqual(f.version, 1001)
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    @threading_helper.requires_working_threading()
    def test_concurrent_inherited_and_abc_dispatch(self):
        class A(abc.ABC):
            pass
        class B:
            pass
        class T(B):
            pass
        class Virtual:
            pass
        @grail.generic
        def f(obj):
            return -1
        f.register(A, lambda obj: 10)
        f.register(B, lambda obj: 0)
        self.assertEqual(f(Virtual()), -1)
        barrier = Barrier(4)
        def worker(index):
            barrier.wait()
            if index == 0:
                A.register(Virtual)
            for _ in range(200):
                if index < 2:
                    f.override(B, lambda obj: index)
                else:
                    self.assertIn(f(T()), (0, 1))
                    self.assertIn(f(Virtual()), (-1, 10))
        run_pool(worker, 4)
        self.assertEqual(f(Virtual()), 10)
        self.assertIn(f(T()), (0, 1))

    @unittest.skipUnless(sys.version_info >= (3, 13), "needs _interpreters")
    def test_subinterpreter(self):
        import _interpreters
        ident = _interpreters.create()
        try:
            result = _interpreters.exec(ident, """
import sys, grail
import sysconfig
assert not (sysconfig.get_config_var("Py_GIL_DISABLED") and sys._is_gil_enabled())
@grail.record
class Point:
    x: int
assert Point(3).x == 3
assert repr(Point(3)) == 'Point(x=3)'
import copy, pickle
value = Point(3)
assert copy.copy(value) is value
assert copy.deepcopy(value) is value
assert pickle.loads(pickle.dumps(value)) == value
@grail.generic
def f(obj):
    return 0
class Base:
    pass
class Child(Base):
    pass
f.register(Base, lambda obj: 1)
assert f(Child()) == 1
""")
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)


if __name__ == "__main__":
    unittest.main()
