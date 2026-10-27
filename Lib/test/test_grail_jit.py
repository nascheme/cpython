"""Grail operations in tier-2 traces: correctness under change.

Each loop runs long enough to be traced. Registrations, overrides and code
changes happen while the loop is hot, so a trace that followed a stale
implementation would compute a wrong result. The checks hold on every build;
the trace-content test needs the JIT or the uop interpreter.
"""
import sys
import unittest

import grail

ITERATIONS = 20000
HALF = ITERATIONS // 2


def executor_uops(func):
    import _opcode
    code = func.__code__
    names = set()
    for i in range(0, len(code.co_code), 2):
        try:
            executor = _opcode.get_executor(code, i)
        except (ValueError, RuntimeError):
            continue
        names.update(executor[j][0].rsplit("_r", 1)[0] for j in range(len(executor)))
    return names


def executor_uop_counts(func):
    import _opcode
    from collections import Counter
    code = func.__code__
    counts = Counter()
    for i in range(0, len(code.co_code), 2):
        try:
            executor = _opcode.get_executor(code, i)
        except (ValueError, RuntimeError):
            continue
        counts.update(executor[j][0].rsplit("_r", 1)[0] for j in range(len(executor)))
    return counts


def tier2_enabled():
    jit = getattr(sys, "_jit", None)
    return jit is not None and jit.is_enabled()


class GrailTraceTests(unittest.TestCase):
    def make_types(self):
        @grail.record
        class V:
            x: float
            y: float

        @grail.record
        class W:
            x: float
            y: float

        return V, W

    def test_generic_override_while_hot(self):
        V, _ = self.make_types()
        f = grail.generic(lambda v: -1.0)
        f.register(V, lambda v: v.x)

        def loop(v):
            total = 0.0
            for i in range(ITERATIONS):
                if i == HALF:
                    f.override(V, lambda v: v.y)
                total += f(v)
            return total

        self.assertEqual(loop(V(1.0, 2.0)), HALF * 1.0 + HALF * 2.0)

    def test_generic_polymorphic_site(self):
        V, W = self.make_types()
        f = grail.generic(lambda v: -1.0)
        f.register(V, lambda v: v.x)
        f.register(W, lambda v: v.y)

        def loop(values):
            total = 0.0
            for i in range(ITERATIONS):
                total += f(values[i % 2])
            return total

        self.assertEqual(loop((V(1.0, 0.0), W(0.0, 3.0))), HALF * 1.0 + HALF * 3.0)

    def test_generic_late_registration(self):
        V, W = self.make_types()
        f = grail.generic(lambda v: -1.0)
        f.register(V, lambda v: v.x)

        def loop(v, w):
            total = 0.0
            for i in range(ITERATIONS):
                if i == HALF:
                    f.register(W, lambda v: v.y)
                total += f(v) + (f(w) if i >= HALF else 0.0)
            return total

        self.assertEqual(loop(V(1.0, 0.0), W(0.0, 3.0)), ITERATIONS * 1.0 + HALF * 3.0)

    def test_generic_code_change_while_hot(self):
        V, _ = self.make_types()
        f = grail.generic(lambda v: -1.0)

        def impl(v):
            return v.x

        def other(v):
            return v.y

        f.register(V, impl)

        def loop(v):
            total = 0.0
            for i in range(ITERATIONS):
                if i == HALF:
                    impl.__code__ = other.__code__
                total += f(v)
            return total

        self.assertEqual(loop(V(1.0, 2.0)), HALF * 1.0 + HALF * 2.0)

    def test_operator_override_while_hot(self):
        V, _ = self.make_types()
        grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x, a.y + b.y))

        def loop(a, b):
            total = 0.0
            for i in range(ITERATIONS):
                if i == HALF:
                    grail.ops.add.override((V, V), lambda a, b: V(a.x - b.x, a.y - b.y))
                total += (a + b).x
            return total

        self.assertEqual(loop(V(5.0, 0.0), V(1.0, 0.0)), HALF * 6.0 + HALF * 4.0)

    def test_operator_polymorphic_site(self):
        V, W = self.make_types()
        grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x, 0.0))
        grail.ops.add.register((W, W), lambda a, b: W(a.x * b.x, 0.0))

        def loop(pairs):
            total = 0.0
            for i in range(ITERATIONS):
                a, b = pairs[i % 2]
                total += (a + b).x
            return total

        pairs = ((V(2.0, 0.0), V(3.0, 0.0)), (W(2.0, 0.0), W(3.0, 0.0)))
        self.assertEqual(loop(pairs), HALF * 5.0 + HALF * 6.0)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_trace_contains_grail_uops(self):
        # A closure generic is not a trace constant: dynamic selection.
        V, _ = self.make_types()
        grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x, a.y + b.y))
        norm = grail.generic(lambda v: 0.0)
        norm.register(V, lambda v: v.x * v.y)

        def loop(a, b):
            total = 0.0
            for _ in range(ITERATIONS):
                c = a + b
                total += norm(c)
            return total

        self.assertEqual(loop(V(1.0, 2.0), V(3.0, 4.0)), ITERATIONS * 24.0)
        uops = executor_uops(loop)
        for name in ("_GRAIL_CALL_GENERIC_SELECT_TIER_TWO", "_CALL_RECORD"):
            self.assertIn(name, uops)
        self.assertTrue({"_LOAD_ATTR_RECORD", "_LOAD_ATTR_RECORD_TYPED",
                         "_LOAD_ATTR_RECORD_BORROW",
                         "_LOAD_ATTR_RECORD_TYPED_BORROW"} & uops)
        self.assertTrue({"_GRAIL_BINARY_CHECK_FUNC", "_GRAIL_BINARY_LOAD_IMPL"} & uops)


# Module-level generics and records: constants in traces, so dispatch folds.
MODULE = """
import grail

@grail.record
class V:
    x: float
    y: float

@grail.record
class W:
    x: float
    y: float

grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x, a.y + b.y))
norm = grail.generic(lambda v: -1.0)
norm.register(V, lambda v: v.x * v.y)

def loop(n, a, b, switch=None, action=None):
    total = 0.0
    for i in range(n):
        if i == switch:
            action()
        total += norm(a + b)
    return total

def loop_known(n, x, y):
    # Operand types are known in the trace (record constructions).
    total = 0.0
    for i in range(n):
        total += norm(V(x, y) + V(y, x))
    return total
"""


def load_module():
    namespace = {"__name__": "grail_jit_module"}
    exec(MODULE, namespace)
    return namespace


class FoldedDispatchTests(unittest.TestCase):
    def test_generic_override_while_hot(self):
        m = load_module()
        V, norm = m["V"], m["norm"]
        result = m["loop"](ITERATIONS, V(1.0, 2.0), V(3.0, 4.0), HALF,
                           lambda: norm.override(V, lambda v: v.x + v.y))
        self.assertEqual(result, HALF * 24.0 + HALF * 10.0)

    def test_operator_override_while_hot(self):
        m = load_module()
        V = m["V"]
        result = m["loop"](ITERATIONS, V(1.0, 2.0), V(3.0, 4.0), HALF,
                           lambda: grail.ops.add.override(
                               (V, V), lambda a, b: V(a.x - b.x, a.y - b.y)))
        self.assertEqual(result, HALF * 24.0 + HALF * 4.0)

    def test_generic_rebound_while_hot(self):
        m = load_module()
        V = m["V"]
        other = grail.generic(lambda v: 7.0)

        def rebind():
            m["norm"] = other

        result = m["loop"](ITERATIONS, V(1.0, 2.0), V(3.0, 4.0), HALF, rebind)
        self.assertEqual(result, HALF * 24.0 + HALF * 7.0)

    def test_other_record_type(self):
        m = load_module()
        V, W, norm = m["V"], m["W"], m["norm"]
        grail.ops.add.register((W, W), lambda a, b: W(a.x * b.x, a.y * b.y))
        norm.register(W, lambda v: v.x - v.y)
        loop = m["loop"]
        self.assertEqual(loop(ITERATIONS, V(1.0, 2.0), V(3.0, 4.0)), ITERATIONS * 24.0)
        self.assertEqual(loop(ITERATIONS, W(1.0, 2.0), W(3.0, 4.0)), ITERATIONS * -5.0)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_folded_uops(self):
        m = load_module()
        self.assertEqual(m["loop_known"](ITERATIONS, 1.0, 2.0), ITERATIONS * 9.0)
        uops = executor_uops(m["loop_known"])
        self.assertIn("_GRAIL_CALL_GENERIC_FOLDED", uops)
        # The element-wise implementation is fused (see ElementwiseFusionTests).
        self.assertTrue({"_GRAIL_BINARY_LOAD_IMPL", "_GRAIL_ELEMENTWISE_FLOAT"} & uops)
        self.assertNotIn("_GRAIL_CALL_GENERIC_SELECT_TIER_TWO", uops)
        self.assertNotIn("_GRAIL_BINARY_CHECK_FUNC", uops)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_fold_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_module, executor_uops
            m = load_module()
            assert m["loop_known"](20000, 1.0, 2.0) == 20000 * 9.0
            uops = executor_uops(m["loop_known"])
            assert "_GRAIL_CALL_GENERIC_SELECT_TIER_TWO" in uops, uops
            assert "_GRAIL_BINARY_CHECK_FUNC" in uops, uops
            assert "_GRAIL_CALL_GENERIC_FOLDED" not in uops, uops
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_FOLD_DISPATCH="0")


# Record fields typed from their plans (_LOAD_ATTR_RECORD_TYPED).
FIELDS = """
import grail

@grail.record
class V:
    x: float
    y: float

@grail.record
class W:
    x: float
    y: float

@grail.record
class R:
    p: V
    q: V

grail.ops.add.register((V, V), lambda a, b: V(a.x + b.x, a.y + b.y))
grail.ops.add.register((W, W), lambda a, b: W(a.x * b.x, a.y * b.y))

def loop(rs):
    total = 0.0
    n = len(rs)
    for i in range(20000):
        r = rs[i % n]
        s = r.p + r.q
        total += s.x * r.p.y
    return total
"""


def load_fields():
    namespace = {"__name__": "grail_jit_fields"}
    exec(FIELDS, namespace)
    return namespace


class TypedFieldTests(unittest.TestCase):
    def test_nonconforming_instances(self):
        m = load_fields()
        V, W, R, loop = m["V"], m["W"], m["R"], m["loop"]
        good = R(V(1.0, 2.0), V(3.0, 4.0))
        self.assertEqual(loop([good]), ITERATIONS * 8.0)
        # Nonconforming instances (an int field, a W where V is declared)
        # exit the typed loads and dispatch normally.
        ints = R(V(1, 2.0), V(3.0, 4.0))
        other = R(W(1.0, 2.0), W(3.0, 4.0))
        self.assertEqual(loop([good, ints]), HALF * 8.0 + HALF * 8.0)
        self.assertEqual(loop([good, other]), HALF * 8.0 + HALF * 6.0)
        self.assertEqual(loop([other]), ITERATIONS * 6.0)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_typed_uops(self):
        m = load_fields()
        V, R = m["V"], m["R"]
        self.assertEqual(m["loop"]([R(V(1.0, 2.0), V(3.0, 4.0))]), ITERATIONS * 8.0)
        uops = executor_uops(m["loop"])
        self.assertTrue({"_LOAD_ATTR_RECORD_TYPED", "_LOAD_ATTR_RECORD_TYPED_BORROW"} & uops)
        self.assertTrue({"_GRAIL_BINARY_LOAD_IMPL", "_GRAIL_ELEMENTWISE_FLOAT"} & uops)
        self.assertNotIn("_GRAIL_BINARY_CHECK_FUNC", uops)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_typed_fields_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_fields, executor_uops
            m = load_fields()
            V, R = m["V"], m["R"]
            assert m["loop"]([R(V(1.0, 2.0), V(3.0, 4.0))]) == 20000 * 8.0
            uops = executor_uops(m["loop"])
            assert "_LOAD_ATTR_RECORD" in uops, uops
            assert "_LOAD_ATTR_RECORD_TYPED" not in uops, uops
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_TYPED_FIELDS="0")


# Borrowed record field loads (_LOAD_ATTR_RECORD*_BORROW). Each loop has
# exactly two field loads; the comments say which may be borrowed.
BORROW = """
import grail

@grail.record
class V:
    x: float
    y: float

@grail.record
class U:
    x: float | int
    y: float | int

def dot(vs):            # both
    t = 0.0
    for v in vs:
        t += v.x * v.y
    return t

def dot_untyped(us):    # both, untyped
    t = 0.0
    for u in us:
        t += u.x * u.y
    return t

def stored(vs):         # neither: the fields are stored as locals
    t = 0.0
    for v in vs:
        x = v.x
        y = v.y
        t += x * y
    return t

def rebound(vs, w):     # only w.y: v is rebound while v.x is on the stack
    t = 0.0
    for v in vs:
        t += v.x * (v := w).y
    return t

def branch(vs, flag):   # only v.y: a jump between v.x and its pop
    t = 0.0
    for v in vs:
        t += v.x * (2.0 if flag else v.y)
    return t
"""


REPEAT = 100   # calls of 100 iterations, enough to trace the loops


def load_borrow():
    namespace = {"__name__": "grail_jit_borrow"}
    exec(BORROW, namespace)
    return namespace


class BorrowedFieldTests(unittest.TestCase):
    def check(self, func, args, expected, borrowed):
        for _ in range(REPEAT):
            self.assertEqual(func(*args), expected)
        if tier2_enabled():
            counts = executor_uop_counts(func)
            loads = sum(counts[name] for name in (
                "_LOAD_ATTR_RECORD", "_LOAD_ATTR_RECORD_TYPED",
                "_LOAD_ATTR_RECORD_BORROW", "_LOAD_ATTR_RECORD_TYPED_BORROW"))
            self.assertEqual(loads, 2, counts)
            self.assertEqual(counts["_LOAD_ATTR_RECORD_BORROW"] +
                             counts["_LOAD_ATTR_RECORD_TYPED_BORROW"],
                             borrowed, counts)

    def test_borrowed_loads(self):
        m = load_borrow()
        V, U = m["V"], m["U"]
        x, y = float("1.5"), float("2.5")
        before = sys.getrefcount(x), sys.getrefcount(y)
        vs = [V(x, y)] * 100
        self.check(m["dot"], (vs,), 100 * 3.75, 2)
        if tier2_enabled():
            # v conforms after the first typed load, so the second needs
            # no exit.
            counts = executor_uop_counts(m["dot"])
            self.assertEqual(counts["_LOAD_ATTR_RECORD_TYPED_BORROW"], 1, counts)
            self.assertEqual(counts["_LOAD_ATTR_RECORD_BORROW"], 1, counts)
        us = [U(x, y)] * 100
        self.check(m["dot_untyped"], (us,), 100 * 3.75, 2)
        del vs, us
        self.assertEqual((sys.getrefcount(x), sys.getrefcount(y)), before)

    def test_owning_loads(self):
        m = load_borrow()
        V = m["V"]
        vs = [V(1.5, 2.5)] * 100
        w = V(3.0, 4.0)
        self.check(m["stored"], (vs,), 100 * 3.75, 0)
        self.check(m["rebound"], (vs, w), 100 * 6.0, 1)
        self.check(m["branch"], (vs, False), 100 * 3.75, 1)

    def test_rebound_owner_is_freed(self):
        # The record behind v.x dies when v is rebound, so that load must
        # own its result (a debug build checks the reference counts).
        m = load_borrow()
        V = m["V"]
        w = V(3.0, 4.0)
        expected = sum(float(i) * 4.0 for i in range(100))
        for _ in range(REPEAT):
            vs = [V(float(i), 2.0) for i in range(100)]
            self.assertEqual(m["rebound"](iter(vs), w), expected)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_borrow_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_borrow, executor_uop_counts
            m = load_borrow()
            vs = [m["V"](1.5, 2.5)] * 100
            for _ in range({REPEAT}):
                assert m["dot"](vs) == 375.0
            counts = executor_uop_counts(m["dot"])
            assert counts["_LOAD_ATTR_RECORD_TYPED"] + counts["_LOAD_ATTR_RECORD"] == 2, counts
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_BORROW_FIELDS="0")


# Element-wise operator implementations fused into _GRAIL_ELEMENTWISE_FLOAT.
FUSE = """
import grail
from grail import ops

@grail.record
class V:
    x: float
    y: float

@grail.record
class W:
    x: float
    y: float

@grail.record
class P:  # not all float
    x: float
    n: int

@ops.add.impl
def _(a: V, b: V) -> V:
    return V(a.x + b.x, a.y + b.y)

@ops.sub.impl
def _(a: V, b: V) -> V:
    return V(a.x - b.x, a.y - b.y)

@ops.mul.impl
def _(a: V, k: float) -> V:
    return V(a.x * k, a.y * k)

@ops.mul.impl
def _(k: float, a: V) -> V:
    return V(k * a.x, k * a.y)

# Shapes that must not be fused.
@ops.add.impl
def _(a: W, b: W) -> W:
    return W(a.x + b.x, a.y - b.y)      # different ops

@ops.sub.impl
def _(a: W, b: W) -> W:
    return W(a.x - b.y, a.y - b.x)      # crossed fields

@ops.mul.impl
def _(a: W, k: float) -> W:
    return W(a.x * k * k, a.y * k)      # nested arithmetic

def ops_loop(n, a, b, k):
    sx = sy = 0.0
    for i in range(n):
        s = (a + b) - b * k + k * a
        sx += s.x
        sy += s.y
    return sx, sy

def w_loop(n, a, b, k):
    sx = sy = 0.0
    for i in range(n):
        s = (a + b) - b * k
        sx += s.x
        sy += s.y
    return sx, sy

def chain(n, a, b, k):
    # Every intermediate is a unique temporary, so results are reused.
    for i in range(n):
        a = (a + b * k) - b * k
    return a

def from_list(n, vs, b):
    # Operand types come from a list: guarded, not known.
    t = 0.0
    for i in range(n):
        t += (vs[i % len(vs)] + b).x
    return t

def build(n, x, y, b):
    # V(x, y) is a unique temporary whose floats x and y are shared.
    t = 0.0
    for i in range(n):
        t += (V(x, y) + b).x
    return t

def keep(n, a, b, out):
    for i in range(n):
        s = a + b
        out.append(s)
        a = s + b
    return a
"""


def load_fuse():
    namespace = {"__name__": "grail_jit_fuse"}
    exec(FUSE, namespace)
    return namespace


def ops_loop_expected(n, ax, ay, bx, by, k):
    sx = sy = 0.0
    for _ in range(n):
        sx += ((ax + bx) - bx * k) + k * ax
        sy += ((ay + by) - by * k) + k * ay
    return sx, sy


class ElementwiseFusionTests(unittest.TestCase):
    def test_results(self):
        m = load_fuse()
        V, W = m["V"], m["W"]
        self.assertEqual(m["ops_loop"](ITERATIONS, V(1.5, 2.25), V(0.5, -3.0), 1.25),
                         ops_loop_expected(ITERATIONS, 1.5, 2.25, 0.5, -3.0, 1.25))
        # W's implementations are evaluated as written.
        a, b = W(1.5, 2.25), W(0.5, -3.0)
        s = W(a.x + b.x, a.y - b.y)
        t = W(b.x * 1.25 * 1.25, b.y * 1.25)
        u = W(s.x - t.y, s.y - t.x)
        self.assertEqual(m["w_loop"](ITERATIONS, a, b, 1.25),
                         (ITERATIONS * u.x, ITERATIONS * u.y))

    def test_chain_reuses_temporaries(self):
        m = load_fuse()
        V = m["V"]
        a, b = V(1.0, 2.0), V(0.25, 0.5)
        r = m["chain"](ITERATIONS, a, b, 2.0)
        x, y = 1.0, 2.0
        for _ in range(ITERATIONS):
            x = (x + 0.25 * 2.0) - 0.25 * 2.0
            y = (y + 0.5 * 2.0) - 0.5 * 2.0
        self.assertEqual((r.x, r.y), (x, y))
        self.assertEqual((a.x, a.y, b.x, b.y), (1.0, 2.0, 0.25, 0.5))
        self.assertEqual(r, V(x, y))
        self.assertEqual(hash(r), hash(V(x, y)))

    def test_shared_values_unchanged(self):
        m = load_fuse()
        V = m["V"]
        x, y = float("1.5"), float("2.5")
        b = V(0.25, 0.5)
        self.assertEqual(m["build"](ITERATIONS, x, y, b), ITERATIONS * 1.75)
        self.assertEqual((x, y), (1.5, 2.5))
        out = []
        a = V(1.0, 1.0)
        r = m["keep"](100, a, b, out)
        self.assertEqual(a, V(1.0, 1.0))
        self.assertEqual(out, [V(1.0 + 0.25 * (2 * i + 1), 1.0 + 0.5 * (2 * i + 1))
                               for i in range(100)])
        self.assertEqual(r, V(1.0 + 0.25 * 200, 1.0 + 0.5 * 200))
        for _ in range(REPEAT):
            out.clear()
            m["keep"](100, a, b, out)
        self.assertEqual(out[-1], V(1.0 + 0.25 * 199, 1.0 + 0.5 * 199))

    def test_polymorphic_and_nonconforming(self):
        m = load_fuse()
        V, W = m["V"], m["W"]
        b = V(1.0, 2.0)
        self.assertEqual(m["from_list"](ITERATIONS, [V(1.0, 0.0)], b), ITERATIONS * 2.0)
        # An int field: nonconforming, so the fused uop exits.
        self.assertEqual(m["from_list"](ITERATIONS, [V(1.0, 0.0), V(3, 0.0)], b),
                         HALF * 2.0 + HALF * 4.0)
        self.assertEqual(m["from_list"](ITERATIONS, [V(3, 0.0)], b), ITERATIONS * 4.0)
        # The other operand a W: its own implementation, or no implementation.
        with self.assertRaises(TypeError):
            m["from_list"](ITERATIONS, [V(1.0, 0.0), W(1.0, 0.0)], b)
        bw = W(1.0, 2.0)
        self.assertEqual(m["from_list"](ITERATIONS, [W(1.0, 0.0)], bw), ITERATIONS * 2.0)

    def test_global_rebound_while_hot(self):
        m = load_fuse()
        V, W = m["V"], m["W"]

        def loop(n, a, b):
            types = set()
            for i in range(n):
                if i == HALF:
                    m["V"] = W  # the implementations now build W
                types.add(type(a + b))
            return types

        self.assertEqual(loop(ITERATIONS, V(1.0, 2.0), V(3.0, 4.0)), {V, W})

    def test_override_while_hot(self):
        m = load_fuse()
        V = m["V"]

        def loop(n, a, b):
            t = 0.0
            for i in range(n):
                if i == HALF:
                    grail.ops.add.override((V, V), lambda a, b: V(a.x * b.x, a.y * b.y))
                t += (a + b).x
            return t

        self.assertEqual(loop(ITERATIONS, V(2.0, 1.0), V(3.0, 1.0)),
                         HALF * 5.0 + HALF * 6.0)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_fused_uops(self):
        m = load_fuse()
        V, W = m["V"], m["W"]
        m["ops_loop"](ITERATIONS, V(1.5, 2.25), V(0.5, -3.0), 1.25)
        counts = executor_uop_counts(m["ops_loop"])
        self.assertEqual(counts["_GRAIL_ELEMENTWISE_FLOAT"], 5, counts)
        self.assertEqual(counts["_PUSH_FRAME"], 0, counts)
        m["w_loop"](ITERATIONS, W(1.5, 2.25), W(0.5, -3.0), 1.25)
        counts = executor_uop_counts(m["w_loop"])
        self.assertEqual(counts["_GRAIL_ELEMENTWISE_FLOAT"], 0, counts)
        self.assertEqual(counts["_PUSH_FRAME"], 3, counts)
        # Fresh implementations: after the warmup above, the shared + could
        # get its own function-entry trace, where this loop's trace would stop.
        m = load_fuse()
        V = m["V"]
        m["from_list"](ITERATIONS, [V(1.0, 0.0)], V(1.0, 2.0))
        counts = executor_uop_counts(m["from_list"])
        self.assertEqual(counts["_GRAIL_ELEMENTWISE_FLOAT"], 1, counts)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_fuse_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_fuse, executor_uop_counts
            m = load_fuse()
            V = m["V"]
            m["ops_loop"](20000, V(1.5, 2.25), V(0.5, -3.0), 1.25)
            counts = executor_uop_counts(m["ops_loop"])
            assert counts["_GRAIL_ELEMENTWISE_FLOAT"] == 0, counts
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_FUSE_ELEMENTWISE="0")


if __name__ == "__main__":
    unittest.main()
