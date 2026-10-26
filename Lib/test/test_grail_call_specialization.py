"""Tier-one entry into registered operator and generic implementations.

BINARY_OP_GRAIL pushes the registered exact-pair implementation's frame (over
a NotImplemented-handling shim) instead of calling it through PyNumber_* and
vectorcall. CALL_GENERIC dispatches a generic function and enters the
selected implementation's frame. Both read the registry on every execution.
"""
import dis
import sys
import sysconfig
import threading
import traceback
import unittest

from test.support import requires_specialization
from test.support.script_helper import assert_python_ok

# Subprocesses keep the GIL disabled on free-threaded builds.
GIL_ARGS = ("-X", "gil=0") if sysconfig.get_config_var("Py_GIL_DISABLED") else ()

import grail


def opnames(func):
    return {i.opname for i in dis.get_instructions(func, adaptive=True)}


def warm(func, *args):
    for _ in range(1000):
        func(*args)


@requires_specialization
class OperatorEntryTests(unittest.TestCase):
    # Operator registrations last for the interpreter's lifetime, so each
    # test registers local types in a fresh subinterpreter.
    def run_isolated(self, code):
        import _interpreters
        ident = _interpreters.create()
        try:
            result = _interpreters.exec(ident, '''
import dis, gc, os, sys, threading, traceback, grail
from grail import ops
import sysconfig
assert not (sysconfig.get_config_var("Py_GIL_DISABLED") and sys._is_gil_enabled())
inline = os.environ.get("PYTHON_GRAIL_INLINE_OPERATORS") != "0"
def opnames(f):
    return {i.opname for i in dis.get_instructions(f, adaptive=True)}
def warm(f, *args):
    for _ in range(1000):
        f(*args)
def check_inline(f):
    assert ("BINARY_OP_GRAIL" in opnames(f)) == inline, opnames(f)
@grail.record
class V:
    x: float
    y: float
''' + code)
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)

    def test_operators_and_inplace(self):
        self.run_isolated('''
ops.add.register((V, V), lambda a, b: V(a.x + b.x, a.y + b.y))
ops.sub.register((V, V), lambda a, b: V(a.x - b.x, a.y - b.y))
ops.mul.register((V, float), lambda a, k: V(a.x * k, a.y * k))
ops.truediv.register((V, float), lambda a, k: V(a.x / k, a.y / k))
def add(a, b): return a + b
def sub(a, b): return a - b
def mul(a, b): return a * b
def div(a, b): return a / b
def iadd(a, b):
    a += b
    return a
def imul(a, b):
    a *= b
    return a
v = V(1.0, 2.0)
for f, b, expected in [(add, v, V(2.0, 4.0)), (sub, v, V(0.0, 0.0)),
                       (mul, 2.0, V(2.0, 4.0)), (div, 2.0, V(0.5, 1.0)),
                       (iadd, v, V(2.0, 4.0)), (imul, 2.0, V(2.0, 4.0))]:
    warm(f, v, b)
    check_inline(f)
    assert f(v, b) == expected, (f, f(v, b))
# Unregistered and builtin pairs at other sites keep their fast paths.
def fadd(a, b): return a + b
warm(fadd, 1.5, 2.5)
assert "BINARY_OP_ADD_FLOAT" in opnames(fadd)
''')

    def test_not_implemented_is_final(self):
        self.run_isolated('''
calls = []
class P:
    def __init__(self, n): self.n = n
    def __add__(self, other): return ("add", self.n, other.n)
    def __iadd__(self, other): return ("iadd", self.n, other.n)
    def __rsub__(self, other): return ("rsub", other.n, self.n)
def decline(a, b):
    calls.append(None)
    return NotImplemented
ops.add.register((P, P), decline)
ops.sub.register((P, P), decline)
def add(a, b): return a + b
def iadd(a, b):
    a += b
    return a
def sub(a, b): return a - b
p, q = P(1), P(2)
# The implementation's result is final: no __add__/__iadd__/__rsub__ or
# TypeError follows a registered NotImplemented.
for f in (add, iadd, sub):
    warm(f, p, q)
    check_inline(f)
    calls.clear()
    assert f(p, q) is NotImplemented, f(p, q)
    assert len(calls) == 1
''')

    def test_exceptions_and_traceback(self):
        self.run_isolated('''
def bad(a, b):
    raise ValueError("boom")
ops.truediv.register((V, V), bad)
def div(a, b): return a / b
v = V(1.0, 1.0)
for _ in range(1000):
    try:
        div(v, v)
    except ValueError:
        pass
check_inline(div)
try:
    div(v, v)
except ValueError as e:
    names = [fs.name for fs in traceback.extract_tb(e.__traceback__)]
    assert names[-2:] == ["div", "bad"], names
''')

    def test_override_and_shape_change(self):
        self.run_isolated('''
ops.add.register((V, V), lambda a, b: "first")
def add(a, b): return a + b
v = V(0.0, 0.0)
warm(add, v, v)
check_inline(add)
ops.add.override((V, V), lambda a, b: "second")
assert add(v, v) == "second"
def star(*args): return ("star", len(args))
ops.add.override((V, V), star)
assert add(v, v) == ("star", 2)
warm(add, v, v)
assert "BINARY_OP_GRAIL" not in opnames(add)
def two(a, b): return "two"
ops.add.override((V, V), two)
assert add(v, v) == "two"
def add2(a, b): return a + b   # add may still be backing off
warm(add2, v, v)
check_inline(add2)
two.__code__ = (lambda a, b, c=3: "three").__code__
two.__defaults__ = (3,)
assert add2(v, v) == "three"
class Callable:
    def __call__(self, a, b): return "object"
ops.add.override((V, V), Callable())
assert add2(v, v) == "object"
''')

    def test_argument_and_frame_lifetime(self):
        self.run_isolated('''
kept = []
frames = []
def first(a, b):
    kept.append(b)
    frames.append(sys._getframe())
    return a
ops.add.register((V, V), first)
def add(a, b): return a + b
warm(add, V(1.0, 1.0), V(2.0, 2.0))
check_inline(add)
kept.clear(); frames.clear()
for i in range(100):
    a, b = V(float(i), 0.0), V(0.0, float(i))
    assert add(a, b) is a
    del a, b
gc.collect()
assert [b.y for b in kept] == [float(i) for i in range(100)]
assert [f.f_locals["a"].x for f in frames] == [float(i) for i in range(100)]
''')

    def test_recursion_limit(self):
        self.run_isolated('''
def recurse(a, b):
    return a + b
ops.add.register((V, V), recurse)
def add(a, b): return a + b
v = V(0.0, 0.0)
for _ in range(3):
    try:
        add(v, v)
    except RecursionError:
        pass
    else:
        raise AssertionError("expected RecursionError")
''')

    def test_tracing(self):
        self.run_isolated('''
def impl(a, b):
    return "impl"
ops.add.register((V, V), impl)
def add(a, b): return a + b
v = V(0.0, 0.0)
warm(add, v, v)
check_inline(add)
events = []
def tracer(frame, event, arg):
    if event in ("call", "return"):
        events.append((event, frame.f_code.co_name))
    return tracer
sys.settrace(tracer)
try:
    r = add(v, v)
finally:
    sys.settrace(None)
assert r == "impl"
assert ("call", "impl") in events and ("return", "impl") in events, events
''')

    def test_concurrent_override(self):
        self.run_isolated('''
def one(a, b): return 1
def two(a, b): return 2
ops.add.register((V, V), one)
def add(a, b): return a + b
v = V(0.0, 0.0)
warm(add, v, v)
stop = False
bad = []
def worker():
    while not stop:
        r = add(v, v)
        if r not in (1, 2):
            bad.append(r)
threads = [threading.Thread(target=worker) for _ in range(4)]
for t in threads:
    t.start()
for i in range(500):
    ops.add.override((V, V), two if i % 2 else one)
stop = True
for t in threads:
    t.join()
assert not bad, bad[:5]
''')

    def test_ablation_subprocess(self):
        code = '''if 1:
            import _interpreters
            ident = _interpreters.create()
            assert _interpreters.exec(ident, """if 1:
                import dis, grail
                @grail.record
                class V:
                    x: float
                grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x))
                def add(a, b): return a + b
                for _ in range(1000):
                    add(V(1.0), V(2.0))
                names = {i.opname for i in dis.get_instructions(add, adaptive=True)}
                assert "BINARY_OP_GRAIL" not in names, names
                assert add(V(1.0), V(2.0)) == V(3.0)
            """) is None
        '''
        assert_python_ok(*GIL_ARGS, "-c", code,
                         PYTHON_GRAIL_INLINE_OPERATORS="0")


@requires_specialization
class GenericEntryTests(unittest.TestCase):
    def make(self):
        @grail.record
        class A:
            x: int

        @grail.record
        class B:
            y: int

        @grail.generic
        def area(shape, scale):
            return ("fallback", scale)

        @area.impl
        def _(shape: A, scale):
            return shape.x * scale

        @area.impl
        def _(shape: B, scale):
            if shape.y < 0:
                raise ValueError("negative")
            return -shape.y * scale

        def site(shape, scale):
            return area(shape, scale)
        return A, B, area, site

    def expect_inline(self, func):
        import os
        inline = os.environ.get("PYTHON_GRAIL_INLINE_GENERICS") != "0"
        self.assertEqual("CALL_GENERIC" in opnames(func), inline, opnames(func))

    def test_polymorphic_site(self):
        A, B, area, site = self.make()
        items = [A(2), B(3), 7, "s"]
        for _ in range(300):
            out = [site(s, 10) for s in items]
        self.expect_inline(site)
        self.assertEqual(out, [20, -30, ("fallback", 10), ("fallback", 10)])

    def test_override_and_mutation(self):
        A, B, area, site = self.make()
        warm(site, A(1), 1)
        self.expect_inline(site)
        area.override(A, lambda shape, scale: "new")
        self.assertEqual(site(A(1), 1), "new")
        impl = lambda shape, scale: "lambda"
        area.override(A, impl)
        warm(site, A(1), 1)
        impl.__code__ = (lambda shape, scale, extra: "three").__code__
        with self.assertRaises(TypeError):
            site(A(1), 1)
        class Callable:
            def __call__(self, shape, scale):
                return "object"
        area.override(A, Callable())
        self.assertEqual(site(A(1), 1), "object")

    def test_exceptions_and_traceback(self):
        A, B, area, site = self.make()
        warm(site, B(1), 1)
        try:
            site(B(-1), 1)
        except ValueError as e:
            names = [fs.name for fs in traceback.extract_tb(e.__traceback__)]
        self.assertEqual(names[-2:], ["site", "_"])

    def test_wrong_arity_and_keywords(self):
        A, B, area, site = self.make()
        def one(shape):
            return area(shape)
        warm(site, A(1), 1)
        for _ in range(300):
            with self.assertRaises(TypeError):
                one(A(1))
        self.assertEqual(area(shape=A(2), scale=3), 6)
        self.assertEqual(area(A(2), scale=3), 6)

    def test_resolution_and_invalidation(self):
        class Base:
            pass

        class Other:
            pass

        class Sub(Base):
            pass

        @grail.generic
        def g(o):
            return "fallback"

        @g.impl
        def _(o: Base):
            return "base"

        @g.impl
        def _(o: Other):
            return "other"

        def site(o):
            return g(o)
        warm(site, Sub())
        self.expect_inline(site)
        self.assertEqual(site(Sub()), "base")
        Sub.__bases__ = (Other,)
        self.assertEqual(site(Sub()), "other")

    def test_recursion(self):
        @grail.generic
        def depth(n):
            return 0

        @depth.impl
        def _(n: int):
            return 1 + depth(n - 1) if n else 0
        self.assertEqual(depth(300), 300)
        for _ in range(3):
            with self.assertRaises(RecursionError):
                depth(10 ** 6)

    def test_concurrent_registration(self):
        @grail.generic
        def g(o):
            return "fallback"

        types = [type(f"T{i}", (), {}) for i in range(50)]
        @g.impl
        def _(o: int):
            return "int"

        def site(o):
            return g(o)
        warm(site, 1)
        stop = False
        bad = []

        def worker():
            while not stop:
                if site(1) != "int":
                    bad.append(1)
        threads = [threading.Thread(target=worker) for _ in range(4)]
        for t in threads:
            t.start()
        try:
            for t in types:
                g.register(t, lambda o: "t")
            g.override(int, lambda o: "int")
        finally:
            stop = True
            for t in threads:
                t.join()
        self.assertEqual(bad, [])

    def test_ablation_subprocess(self):
        code = '''if 1:
            import dis, grail
            @grail.generic
            def g(o):
                return 0
            @g.impl
            def _(o: int):
                return o
            def site(o): return g(o)
            for _ in range(1000):
                site(1)
            names = {i.opname for i in dis.get_instructions(site, adaptive=True)}
            assert "CALL_GENERIC" not in names, names
        '''
        assert_python_ok(*GIL_ARGS, "-c", code,
                         PYTHON_GRAIL_INLINE_GENERICS="0")


@grail.record
class RV:
    x: float
    y: object


@grail.record
class RW:
    x: float
    y: float = 0.0


@requires_specialization
class RecordCallTests(unittest.TestCase):
    def test_specializes_and_conforms(self):
        import _grail
        def site(a, b):
            return RV(a, b)
        warm(site, 1.0, 2)
        self.assertIn("CALL_RECORD", opnames(site))
        r = site(1.0, "s")
        self.assertEqual(r, RV(1.0, "s"))
        self.assertTrue(_grail._is_conforming(RV(1.0, 2)) is False)  # object field
        def wsite(a, b):
            return RW(a, b)
        warm(wsite, 1.0, 2.0)
        self.assertIn("CALL_RECORD", opnames(wsite))
        self.assertTrue(_grail._is_conforming(wsite(1.0, 2.0)))
        self.assertFalse(_grail._is_conforming(wsite(1, 2.0)))
        self.assertIs(type(wsite(1, 2.0).x), int)

    def test_argument_ownership(self):
        def site(a, b):
            return RV(a, b)
        warm(site, 1.0, 2)
        values = [object() for _ in range(3)]
        before = [sys.getrefcount(v) for v in values]
        records = [site(1.0, v) for v in values]
        for v, r in zip(values, records):
            self.assertIs(r.y, v)
        del records, r, v
        self.assertEqual([sys.getrefcount(v) for v in values], before)
        # Freshly built arguments are owned only by the record.
        r = site(1.0, [1, 2])
        self.assertEqual(sys.getrefcount(r.y), 2)

    def test_polymorphic_and_deopt(self):
        def site(t, *args):
            return t(*args)
        def fixed(t, a, b):
            return t(a, b)
        warm(fixed, RV, 1.0, 2)
        self.assertIn("CALL_RECORD", opnames(fixed))
        self.assertEqual(fixed(RW, 1.0, 2.0), RW(1.0, 2.0))
        self.assertEqual(fixed(lambda a, b: (a, b), 1, 2), (1, 2))
        self.assertEqual(fixed(RV, 1.0, 2), RV(1.0, 2))
        # Too few, defaults, keywords: ordinary errors and results.
        def short(a):
            return RW(a)
        warm(short, 1.0)
        self.assertEqual(short(1.0), RW(1.0, 0.0))
        self.assertNotIn("CALL_RECORD", opnames(short))
        def too_many():
            return RV(1, 2, 3)
        for _ in range(100):
            with self.assertRaises(TypeError):
                too_many()
        def kw(a):
            return RV(a, y=2)
        warm(kw, 1.0)
        self.assertEqual(kw(1.0), RV(1.0, 2))

    def test_rebound_name(self):
        ns = {"T": RV}
        exec("def site(a, b): return T(a, b)", ns)
        site = ns["site"]
        warm(site, 1.0, 2)
        self.assertIn("CALL_RECORD", opnames(site))
        ns["T"] = RW
        self.assertEqual(site(1.0, 2.0), RW(1.0, 2.0))
        ns["T"] = dict.fromkeys
        self.assertEqual(site("a", 1), {"a": 1})

    def test_tuple_plan(self):
        @grail.record
        class Deep:
            t: tuple[int, ...]
        def site(t):
            return Deep(t)
        warm(site, (1,))
        self.assertIn("CALL_RECORD", opnames(site))
        nested = (1,)
        self.assertEqual(site(nested).t, nested)

    def test_ablation_subprocess(self):
        code = '''if 1:
            import dis, grail
            @grail.record
            class R:
                x: float
            def site(a): return R(a)
            for _ in range(1000):
                site(1.0)
            names = {i.opname for i in dis.get_instructions(site, adaptive=True)}
            assert "CALL_RECORD" not in names, names
            assert "CALL_BUILTIN_CLASS" in names, names
        '''
        assert_python_ok(*GIL_ARGS, "-c", code,
                         PYTHON_GRAIL_RECORD_CALLS="0")


@requires_specialization
class RecordMatchTests(unittest.TestCase):
    def test_specializes(self):
        def site(r):
            match r:
                case RV(a, b):
                    return "RV", a, b
                case RW(a):
                    return "RW", a
                case RW():
                    return "never"
            return None
        warm(site, RV(1.0, 2))
        warm(site, RW(3.0, 4.0))
        self.assertIn("MATCH_CLASS_RECORD", opnames(site))
        self.assertEqual(site(RV(1.0, "s")), ("RV", 1.0, "s"))
        self.assertEqual(site(RW(5.0)), ("RW", 5.0))
        # A non-record subject leaves the specialized path.
        self.assertIsNone(site(5))
        self.assertIsNone(site(RV))
        self.assertEqual(site(RV(1, 2)), ("RV", 1, 2))

    def test_fields_and_ownership(self):
        def site(r):
            match r:
                case RV(x, y):
                    return y
        warm(site, RV(1.0, 2))
        value = object()
        before = sys.getrefcount(value)
        r = RV(1.0, value)
        for _ in range(100):
            self.assertIs(site(r), value)
        del r
        self.assertEqual(sys.getrefcount(value), before)

    def test_errors_and_keywords(self):
        def too_many(r):
            match r:
                case RV(a, b, c):
                    return c
        for _ in range(100):
            with self.assertRaisesRegex(TypeError, "accepts 2 positional sub-patterns"):
                too_many(RV(1.0, 2))
        # A record of another type is still no match.
        self.assertIsNone(too_many(RW(1.0)))
        def kw(r):
            match r:
                case RV(y=y):
                    return y
        warm(kw, RV(1.0, 2))
        self.assertNotIn("MATCH_CLASS_RECORD", opnames(kw))
        self.assertEqual(kw(RV(1.0, 3)), 3)

    def test_rebound_name(self):
        class Plain:
            __match_args__ = ("x", "y")
            def __init__(self, x, y):
                self.x, self.y = x, y
        ns = {"T": RV}
        exec("def site(r):\n"
             "    match r:\n"
             "        case T(a, b):\n"
             "            return a, b\n", ns)
        site = ns["site"]
        warm(site, RV(1.0, 2))
        self.assertIn("MATCH_CLASS_RECORD", opnames(site))
        ns["T"] = RW
        self.assertEqual(site(RW(1.0, 2.0)), (1.0, 2.0))
        self.assertIsNone(site(RV(1.0, 2)))
        ns["T"] = Plain
        self.assertEqual(site(Plain(3, 4)), (3, 4))
        self.assertIsNone(site(RV(1.0, 2)))
        ns["T"] = 5
        with self.assertRaisesRegex(TypeError, "class pattern must refer to a class"):
            site(RV(1.0, 2))

    def test_unpack_without_tuple(self):
        def site(r):
            match r:
                case RV(a, b):
                    return a, b
                case RW(a):
                    return a
        warm(site, RV(1.0, 2))
        warm(site, RW(3.0, 4.0))
        names = opnames(site)
        self.assertIn("MATCH_CLASS_RECORD", names)
        self.assertIn("UNPACK_SEQUENCE_RECORD", names)
        self.assertEqual(site(RV(5.0, "y")), (5.0, "y"))
        self.assertEqual(site(RW(6.0)), 6.0)
        self.assertIsNone(site(7))

    def test_plain_unpack_still_fails(self):
        # Records are not iterable outside class patterns, warm or not.
        def unpack(r):
            a, b = r
            return a, b
        def both(r):
            match r:
                case RV(a, b):
                    pass
            x, y = r
            return x, y
        for f in (unpack, both):
            for _ in range(200):
                with self.assertRaisesRegex(TypeError, "cannot unpack"):
                    f(RV(1.0, 2))
        self.assertNotIn("UNPACK_SEQUENCE_RECORD", opnames(unpack))
        unpacks = [i.opname for i in dis.get_instructions(both, adaptive=True)
                   if i.opname.startswith("UNPACK_SEQUENCE")]
        self.assertEqual(unpacks, ["UNPACK_SEQUENCE_RECORD", "UNPACK_SEQUENCE"])

    def test_tracing_matched_site(self):
        def site(r):
            match r:
                case RV(a, b):
                    return a, b
        warm(site, RV(1.0, 2))
        events = []
        def tracer(frame, event, arg):
            if frame.f_code is site.__code__:
                events.append(event)
            return tracer
        sys.settrace(tracer)
        try:
            self.assertEqual(site(RV(1.0, 2)), (1.0, 2))
        finally:
            sys.settrace(None)
        self.assertIn("line", events)
        self.assertEqual(site(RV(3.0, 4)), (3.0, 4))
        # sys.monitoring instruments every opcode of the site.
        mon = sys.monitoring
        tool = mon.DEBUGGER_ID
        mon.use_tool_id(tool, "test")
        seen = []
        try:
            mon.register_callback(tool, mon.events.INSTRUCTION,
                                  lambda code, off: seen.append(off))
            mon.set_local_events(tool, site.__code__, mon.events.INSTRUCTION |
                                 mon.events.BRANCH_LEFT | mon.events.BRANCH_RIGHT)
            for _ in range(50):
                self.assertEqual(site(RV(1.0, 2)), (1.0, 2))
                self.assertIsNone(site(RW(1.0)))
        finally:
            mon.set_local_events(tool, site.__code__, 0)
            mon.register_callback(tool, mon.events.INSTRUCTION, None)
            mon.free_tool_id(tool)
        self.assertTrue(seen)
        self.assertEqual(site(RV(5.0, 6)), (5.0, 6))

    def test_ablation_subprocess(self):
        code = '''if 1:
            import dis, grail
            @grail.record
            class R:
                x: float
            def site(r):
                match r:
                    case R(x):
                        return x
            for _ in range(1000):
                assert site(R(1.0)) == 1.0
            names = {i.opname for i in dis.get_instructions(site, adaptive=True)}
            assert "MATCH_CLASS_RECORD" not in names, names
            assert "MATCH_CLASS" in names, names
        '''
        assert_python_ok(*GIL_ARGS, "-c", code,
                         PYTHON_GRAIL_RECORD_MATCH="0")
        assert_python_ok(*GIL_ARGS, "-c", code,
                         PYTHON_GRAIL_RECORD_MATCH_OP="0")


if __name__ == "__main__":
    unittest.main()
