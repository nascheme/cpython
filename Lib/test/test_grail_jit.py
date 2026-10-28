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


SPECULATE = """
import grail

@grail.record
class A:
    x: int

@grail.record
class B:
    x: int

@grail.record
class C:
    x: int

@grail.generic
def g(v):
    raise TypeError

@g.impl
def _(v: A):
    return 1

def shared(v):
    return 100 if type(v) is B else 2000

g.register(B, shared)
g.register(C, shared)

def loop(items):
    t = 0
    for v in items:
        t += g(v)
    return t
"""


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

    def test_operator_sole_pair_speculation(self):
        ns = {}
        exec("""if 1:
            import grail

            @grail.record
            class M:
                amount: int
                currency: str

            @grail.record
            class N:
                amount: int

            @grail.ops.add.impl
            def _(a: M, b: M):
                return M(a.amount + b.amount, a.currency)

            def shared(a, b):
                return N(a.amount + b.amount + 1000)

            grail.ops.add.register((N, N), shared)
            grail.ops.add.register((N, M), shared)

            def total(start, items):
                acc = start
                for v in items:
                    acc = acc + v
                return acc
            """, ns)
        M, N, total = ns["M"], ns["N"], ns["total"]
        ms = [M(i, "EUR") for i in range(100)]
        for _ in range(REPEAT):
            self.assertEqual(total(M(0, "EUR"), ms), M(4950, "EUR"))
        if tier2_enabled():
            self.assertIn("_GRAIL_BINARY_LOAD_IMPL", executor_uops(total))
        ns_ = [N(i) for i in range(100)]
        for _ in range(REPEAT):
            self.assertEqual(total(N(0), ns_), N(4950 + 100 * 1000))
            self.assertEqual(total(N(0), [N(1), M(2, "x")] * 3),
                             N(3 + 6 + 6000))

    def test_sole_registration_speculation(self):
        # An argument of unknown type: the fold guards on the only type
        # registered for the traced implementation, unless the
        # implementation is shared by several types.
        ns = {}
        exec(SPECULATE, ns)
        A, B, C, loop = ns["A"], ns["B"], ns["C"], ns["loop"]
        for _ in range(REPEAT):
            self.assertEqual(loop([A(0)] * 100), 100)
        if tier2_enabled():
            uops = executor_uops(loop)
            self.assertIn("_GRAIL_CALL_GENERIC_FOLDED", uops)
            self.assertNotIn("_GRAIL_CALL_GENERIC_SELECT_TIER_TWO", uops)
        for _ in range(REPEAT):
            self.assertEqual(loop([A(0), B(0), C(0)] * 100), 100 * 2101)
        ns = {}
        exec(SPECULATE, ns)
        B, C, loop = ns["B"], ns["C"], ns["loop"]
        for _ in range(REPEAT):
            self.assertEqual(loop([B(0)] * 100), 100 * 100)
        if tier2_enabled():
            self.assertNotIn("_GRAIL_CALL_GENERIC_FOLDED", executor_uops(loop))
        for _ in range(REPEAT):
            self.assertEqual(loop([B(0), C(0)] * 100), 100 * 2100)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_folded_uops(self):
        m = load_module()
        self.assertEqual(m["loop_known"](ITERATIONS, 1.0, 2.0), ITERATIONS * 9.0)
        uops = executor_uops(m["loop_known"])
        self.assertIn("_GRAIL_CALL_GENERIC_FOLDED", uops)
        # The element-wise implementation is fused (see ElementwiseFusionTests).
        self.assertTrue({"_GRAIL_BINARY_LOAD_IMPL", "_GRAIL_ELEMENTWISE_FLOAT",
                         "_VO_ELEMENTWISE_FLOAT"} & uops)
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
        self.assertTrue({"_GRAIL_BINARY_LOAD_IMPL", "_GRAIL_ELEMENTWISE_FLOAT",
                         "_VO_ELEMENTWISE_FLOAT"} & uops)
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
        # With virtual temporaries (VirtualObjectTests) in their form for
        # virtual operands and results.
        self.assertEqual(counts["_GRAIL_ELEMENTWISE_FLOAT"] +
                         counts["_VO_ELEMENTWISE_FLOAT"], 5, counts)
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
        self.assertEqual(counts["_GRAIL_ELEMENTWISE_FLOAT"] +
                         counts["_VO_ELEMENTWISE_FLOAT"], 1, counts)

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



# Calls of float-expression functions fused into _GRAIL_FLOAT_PROGRAM.
FLOAT_CALLS = """
import grail

@grail.record
class V:
    x: float
    y: float
    z: float

def dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z

def mixed(a, k, b):
    return (a.x - k) * b.y - k * (a.z + b.x)

def products(a, k, b):
    return a.x * b.y - a.y * b.x + k * a.z - b.z * k

def products_loop(n, a, k, b):
    t = 0.0
    for i in range(n):
        t += products(a, k, b)
    return t

def local(a, b):
    t = a.x * b.x      # a local: not fused
    return t + a.y

def divide(a, b):
    return a.x / b.x   # not specialized: not fused

def dot_loop(n, a, b):
    t = 0.0
    for i in range(n):
        t += dot(a, b)
    return t

def mixed_loop(n, a, k, b):
    t = 0.0
    for i in range(n):
        t += mixed(a, k, b)
    return t

def other_loop(n, a, b):
    t = 0.0
    for i in range(n):
        t += local(a, b) + divide(a, b)
    return t

def switch_loop(n, a, b, switch, action):
    t = 0.0
    for i in range(n):
        if i == switch:
            action()
        t += dot(a, b)
    return t

def list_loop(n, vs, b):
    t = 0.0
    for i in range(n):
        v = vs[i % len(vs)]
        t += dot(v, b)
    return t
"""


def load_float_calls():
    namespace = {"__name__": "grail_jit_float_calls"}
    exec(FLOAT_CALLS, namespace)
    return namespace


class FloatCallFusionTests(unittest.TestCase):
    def test_results(self):
        m = load_float_calls()
        V = m["V"]
        a, b = V(1.5, -2.25, 0.1), V(0.3, 7.0, -1e300)
        d = a.x * b.x + a.y * b.y + a.z * b.z
        self.assertEqual(m["dot_loop"](ITERATIONS, a, b), sum_repeat(d, ITERATIONS))
        k = 0.7
        e = (a.x - k) * b.y - k * (a.z + b.x)
        self.assertEqual(m["mixed_loop"](ITERATIONS, a, k, b), sum_repeat(e, ITERATIONS))
        f = (a.x * b.x + a.y) + a.x / b.x
        self.assertEqual(m["other_loop"](ITERATIONS, a, b), sum_repeat(f, ITERATIONS))
        # A sum of products, evaluated left to right (1e300 cancels).
        g = a.x * b.y - a.y * b.x + k * a.z - b.z * k
        self.assertEqual(m["products"](a, k, b), g)
        self.assertEqual(m["products_loop"](ITERATIONS, a, k, b), sum_repeat(g, ITERATIONS))
        c = V(1e300, 3.0, -1e300)
        g = c.x * b.y - c.y * b.x + k * c.z - b.z * k
        self.assertEqual(m["products_loop"](ITERATIONS, c, k, b), sum_repeat(g, ITERATIONS))

    def test_nonconforming_and_types(self):
        m = load_float_calls()
        V = m["V"]
        b = V(1.0, 2.0, 3.0)
        good, ints = V(1.0, 1.0, 1.0), V(1, 1.0, 1.0)
        self.assertEqual(m["list_loop"](ITERATIONS, [good], b), ITERATIONS * 6.0)
        self.assertEqual(m["list_loop"](ITERATIONS, [good, ints], b), ITERATIONS * 6.0)
        self.assertEqual(type(m["dot"](ints, b)), float)
        # A non-record argument with the same attributes.
        class P:
            x = y = z = 1.0
        self.assertEqual(m["list_loop"](ITERATIONS, [good, P()], b), ITERATIONS * 6.0)

    def test_code_change_and_rebind_while_hot(self):
        m = load_float_calls()
        V = m["V"]
        a, b = V(1.0, 2.0, 3.0), V(1.0, 1.0, 1.0)

        def change_code():
            m["dot"].__code__ = (lambda a, b: a.x - b.x).__code__

        self.assertEqual(m["switch_loop"](ITERATIONS, a, b, HALF, change_code),
                         HALF * 6.0 + HALF * 0.0)
        m = load_float_calls()
        V = m["V"]
        a, b = V(1.0, 2.0, 3.0), V(1.0, 1.0, 1.0)

        def rebind():
            m["dot"] = lambda a, b: a.z * b.z

        self.assertEqual(m["switch_loop"](ITERATIONS, a, b, HALF, rebind),
                         HALF * 6.0 + HALF * 3.0)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_fused_uops(self):
        m = load_float_calls()
        V = m["V"]
        a, b = V(1.5, -2.25, 0.1), V(0.3, 7.0, 2.0)
        m["dot_loop"](ITERATIONS, a, b)
        counts = executor_uop_counts(m["dot_loop"])
        self.assertEqual(counts["_GRAIL_FLOAT_PROGRAM"], 1, counts)
        self.assertEqual(counts["_PUSH_FRAME"], 0, counts)
        m["mixed_loop"](ITERATIONS, a, 0.5, b)
        counts = executor_uop_counts(m["mixed_loop"])
        self.assertEqual(counts["_GRAIL_FLOAT_PROGRAM"], 1, counts)
        m["other_loop"](ITERATIONS, a, b)
        counts = executor_uop_counts(m["other_loop"])
        self.assertEqual(counts["_GRAIL_FLOAT_PROGRAM"], 0, counts)
        m["products_loop"](ITERATIONS, a, 0.5, b)
        counts = executor_uop_counts(m["products_loop"])
        self.assertEqual(counts["_GRAIL_FLOAT_PROGRAM"], 1, counts)


# Records rebuilt from values known to conform (_CALL_RECORD_CHECKED).
CONFORMING = """
import grail

@grail.record
class Item:
    q: tuple[int, ...]
    n: int
    flag: bool

@grail.record
class Table:
    items: tuple[Item | None, ...]
    count: int

def put(t, i, v):
    return t[:i] + (v,) + t[i + 1:]

def step(tab, i):
    items = tab.items
    it = items[i]
    new = Item(it.q[1:] + (i,), it.n + 1, not it.flag)
    return Table(put(items, i, new), tab.count + 1)

def loop(tab, n):
    for k in range(n):
        tab = step(tab, k % 3)
    return tab
"""


def load_conforming():
    namespace = {"__name__": "grail_jit_conforming"}
    exec(CONFORMING, namespace)
    return namespace


class KnownConformingTests(unittest.TestCase):
    def check_table(self, m, tab):
        import _grail
        Item, Table = m["Item"], m["Table"]
        # The conformance bits match a fresh construction's.
        for it in tab.items[:3]:
            self.assertEqual(_grail._is_conforming(it),
                             _grail._is_conforming(Item(it.q, it.n, it.flag)))
        self.assertEqual(_grail._is_conforming(tab),
                         _grail._is_conforming(Table(tab.items, tab.count)))

    def test_results(self):
        m = load_conforming()
        Item, Table = m["Item"], m["Table"]
        start = Table(tuple(Item((0, 0), 0, False) for _ in range(3)) + (None,), 0)
        tab = m["loop"](start, ITERATIONS)
        self.assertEqual(tab.count, ITERATIONS)
        self.assertEqual([it.n for it in tab.items[:3]], [6667, 6667, 6666])
        self.check_table(m, tab)
        import _grail
        self.assertTrue(_grail._is_conforming(tab))
        # A nonconforming item (a str in q, an int flag) leaves the table
        # nonconforming until it is replaced by conforming values.
        for bad in (Item(("x", 0), 0, False), Item((0, 0), 0, 1)):
            for n in (1, 2, 3, 4, 7):
                with self.subTest(bad=bad, n=n):
                    start = Table((bad, Item((0, 0), 0, False),
                                   Item((0, 0), 0, False), None), 0)
                    tab = m["loop"](start, n)
                    self.check_table(m, tab)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_checked_uops(self):
        m = load_conforming()
        Item, Table = m["Item"], m["Table"]
        m["loop"](Table(tuple(Item((0, 0), 0, False) for _ in range(3)), 0),
                  ITERATIONS)
        counts = executor_uop_counts(m["loop"])
        self.assertGreaterEqual(counts["_CALL_RECORD_CHECKED"], 2, counts)
        self.assertEqual(counts["_CALL_RECORD"], 0, counts)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_known_conforming_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_conforming, executor_uops
            m = load_conforming()
            Item, Table = m["Item"], m["Table"]
            start = Table(tuple(Item((0, 0), 0, False) for _ in range(3)), 0)
            assert m["loop"](start, 20000).count == 20000
            uops = executor_uops(m["loop"])
            assert "_CALL_RECORD" in uops, uops
            assert "_CALL_RECORD_CHECKED" not in uops, uops
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_KNOWN_CONFORMING="0")


MATCH_SOURCE = """
import grail

@grail.record
class A:
    x: int

@grail.record
class B:
    x: int
    y: int

@grail.record
class C:
    pass

def total(items):
    t = 0
    for it in items:
        match it:
            case A(x):
                t += x
            case B(x, y):
                t += x * y
            case C():
                t += 100
            case _:
                t += 1000
    return t
"""


def load_match():
    namespace = {}
    exec(MATCH_SOURCE, namespace)
    return namespace


class MatchFoldTests(unittest.TestCase):
    """A class pattern on a record subject of a type known in the trace
    folds to its outcome: one type guard per match statement."""

    def test_homogeneous_then_mixed(self):
        m = load_match()
        A, B, C, total = m["A"], m["B"], m["C"], m["total"]
        bs = [B(i, 2) for i in range(100)]
        for _ in range(REPEAT):
            self.assertEqual(total(bs), 9900)
        if tier2_enabled():
            uops = executor_uop_counts(total)
            self.assertEqual(uops["_MATCH_CLASS_RECORD_NONE"], 1, uops)
            self.assertEqual(uops["_MATCH_CLASS_RECORD_SELF"], 1, uops)
            self.assertEqual(uops["_GUARD_MATCH_CLASS_RECORD"], 0, uops)
        mixed = [A(3), B(2, 5), C(), 7, "s", B(1, 1), A(-1)] * 20
        for _ in range(REPEAT):
            self.assertEqual(total(mixed), 20 * (3 + 10 + 100 + 1000 + 1000 + 1 - 1))
        for _ in range(REPEAT):
            self.assertEqual(total(bs), 9900)

class SubscriptTests(unittest.TestCase):
    """PVector and PVector draft [int] subscripts in traces call the item
    function directly once the container type is guarded."""

    def test_vector_and_draft(self):
        ns = {}
        exec("""if 1:
            def total(v, idx):
                t = 0
                for i in idx:
                    t += v[i]
                return t
            """, ns)
        total = ns["total"]
        v = grail.PVector(range(100))
        idx = list(range(-100, 100))
        for _ in range(REPEAT):
            self.assertEqual(total(v, idx), 2 * sum(range(100)))
        if tier2_enabled():
            self.assertIn("_BINARY_OP_SUBSCR_SSIZE_ITEM", executor_uops(total))
        with self.assertRaises(IndexError):
            total(v, [0, 100])
        with grail.mutate(v) as d:
            for _ in range(REPEAT):
                self.assertEqual(total(d, idx), 2 * sum(range(100)))
            with self.assertRaises(IndexError):
                total(d, [5, -101])
        with self.assertRaises(RuntimeError):
            total(d, [0])
        self.assertEqual(total(list(range(100)), idx), 2 * sum(range(100)))
        self.assertEqual(total(v, [True, 2]), 3)


class DraftStoreTests(unittest.TestCase):
    def test_draft_item_assignment(self):
        ns = {}
        exec("""if 1:
            def fill(d, idx, value):
                for i in idx:
                    d[i] = value + i
            """, ns)
        fill = ns["fill"]
        v = grail.PVector(range(100))
        idx = list(range(-100, 100))
        for _ in range(REPEAT):
            with grail.mutate(v) as d:
                fill(d, idx, 1000)
            r = grail.finish(d)
            self.assertEqual(list(r), [1000 + i for i in range(100)])
        if tier2_enabled():
            self.assertIn("_STORE_SUBSCR_SSIZE_ITEM", executor_uops(fill))
        self.assertEqual(list(v), list(range(100)))
        with grail.mutate(v) as d:
            with self.assertRaises(IndexError):
                fill(d, [0, 100], 0)
            with grail.mutate(v) as other:
                pass
            with self.assertRaises(ValueError):
                d[0] = grail.mutate(v)
        with self.assertRaises(RuntimeError):
            fill(d, [0], 0)
        lst = list(range(100))
        fill(lst, idx, 0)
        self.assertEqual(lst, list(range(100)))


RECORD_EQ_SOURCE = """
import grail

@grail.record
class K:
    v: int
    f: float

@grail.record
class Box:
    k: K
    s: str

def count(pairs, n, switch=-1, action=None):
    eq = ne = 0
    for i in range(n):
        if i == switch:
            action()
        for a, b in pairs:
            eq += a == b
            ne += a != b
    return eq, ne
"""


class RecordEqualityJitTests(unittest.TestCase):
    def test_equality(self):
        ns = {}
        exec(RECORD_EQ_SOURCE, ns)
        K, Box, count = ns["K"], ns["Box"], ns["count"]
        nan = float("nan")
        n1 = K(1, nan)
        pairs = [(K(1, 2.0), K(1, 2.0)), (K(1, 2.0), K(2, 2.0)), (n1, n1),
                 (K(1, nan), K(1, nan)), (K(1, 0.0), K(1, -0.0)),
                 (K(True, 2.0), K(1, 2.0))]
        expected = (sum(a == b for a, b in pairs), sum(a != b for a, b in pairs))
        for _ in range(REPEAT):
            self.assertEqual(count(pairs, 50), (50 * expected[0], 50 * expected[1]))
        boxes = [(Box(K(1, 1.0), "a"), Box(K(1, 1.0), "a")),
                 (Box(K(1, 1.0), "a"), Box(K(1, 1.5), "a"))]
        for _ in range(REPEAT):
            self.assertEqual(count(boxes, 50), (50, 50))
        if tier2_enabled():
            self.assertIn("_COMPARE_OP_RECORD", executor_uops(count))

    def test_hook_registered_while_hot(self):
        ns = {}
        exec(RECORD_EQ_SOURCE, ns)
        K, count = ns["K"], ns["count"]
        pairs = [(K(1, 2.0), K(3, 4.0))]

        def register():
            @grail.eq.impl
            def _(a: K, b):
                return True

        self.assertEqual(count(pairs, ITERATIONS, HALF, register),
                         (HALF, HALF))


class DraftGuardTests(unittest.TestCase):
    def test_closed_draft_in_trace(self):
        ns = {}
        exec("""if 1:
            import grail

            @grail.record
            class P:
                x: int
                y: int

            def bump(d, n):
                for _ in range(n):
                    d.x = d.x + d.y
                return d.x
            """, ns)
        P, bump = ns["P"], ns["bump"]
        for _ in range(REPEAT):
            with grail.mutate(P(0, 1)) as d:
                self.assertEqual(bump(d, 100), 100)
            self.assertEqual(grail.finish(d), P(100, 1))
            # Sealed by the with statement, then finished: no more edits.
            with self.assertRaises(RuntimeError):
                bump(d, 1)
        with grail.mutate(P(0, 1)) as d:
            pass
        with self.assertRaises(RuntimeError):
            bump(d, 1)


# Virtual objects: records and tuples built in one function and consumed by
# its caller or callee in the same trace need no allocation. Each case also
# checks the result, so the checks hold on every build.
VIRTUAL = """
import grail

@grail.record
class P:
    x: int
    y: int

@grail.record
class F:
    x: float
    y: float

def make(i):
    return P(i, i + 1)

def total(p):
    s = p.x
    return s + p.y

def chain(n):
    t = 0
    for i in range(n):
        t += total(make(i))
    return t

def pair(i):
    return i, i * 2

def pairs(n):
    t = 0
    for i in range(n):
        a, b = pair(i)
        t += a + b
    return t

def keep(i, out):
    p = make(i)
    out.append(p)
    return p.x

def escaping(n, out):
    t = 0
    for i in range(n):
        t += keep(i, out)
    return t

def check(p, i, limit):
    q = p
    if i >= limit:
        return int(p is q) + p.x
    return p.y

def exits(n, limit):
    t = 0
    for i in range(n):
        t += check(make(i), i, limit)
    return t

def ratio(p):
    d = p.y
    return p.x / d

def ratios(n, caught):
    t = 0.0
    for i in range(n):
        try:
            t += ratio(F(1.0, (i % 16) * 1.0))
        except ZeroDivisionError as e:
            tb = e.__traceback__
            while tb.tb_next is not None:
                tb = tb.tb_next
            caught.append(tb.tb_frame.f_locals["p"])
    return t

class D:
    deleted = 0
    def __del__(self):
        D.deleted += 1

def with_d(i, d):
    return i, d

def dels(n):
    t = 0
    for i in range(n):
        a, d = with_d(i, D())
        t += a
    return t

def last(n):
    p = None
    for i in range(n):
        p = make(i)
    return p

@grail.record
class Seg:
    a: P
    b: P

def seg(i):
    return Seg(make(i), make(i + 1))

def length(s, i, limit):
    if i >= limit:
        # Leave the trace with s virtual and its fields held only by s.
        return s.b.y - s.a.x + int(s.a is s.a) * 100
    return s.b.y - s.a.x

def segs(n, limit):
    t = 0
    for i in range(n):
        t += length(seg(i), i, limit)
    return t

def step(i):
    return make(i), i * 3

def shared(n):
    t = 0
    for i in range(n):
        t += i
    return t, n

def caller_a(n):
    a, b = shared(n)
    return a + b

def caller_b(n):
    s = 0
    for k in (1, 2):
        with Ctx():
            a, b = shared(n)
            s += a + b * k
    return s

class Ctx:
    def __enter__(self):
        return self
    def __exit__(self, *args):
        return None

LOG = []

class Logged:
    def __del__(self):
        LOG.append("del")

def drop(d, i):
    p = make(i)
    d = None
    return p.x + p.y

def drops(n):
    t = 0
    for i in range(n):
        t += drop(Logged(), i)
        LOG.append("it")
    return t

def use_step(i):
    p, k = step(i)
    return p.y + k

def steps(n):
    t = 0
    for i in range(n):
        t += use_step(i)
    return t

@grail.record
class V3:
    x: float
    y: float
    z: float

@grail.ops.add.impl
def _(a: V3, b: V3) -> V3:
    return V3(a.x + b.x, a.y + b.y, a.z + b.z)

@grail.ops.sub.impl
def _(a: V3, b: V3) -> V3:
    return V3(a.x - b.x, a.y - b.y, a.z - b.z)

@grail.ops.mul.impl
def _(a: V3, k: float) -> V3:
    return V3(k * a.x, k * a.y, k * a.z)

def dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z

def v3(i):
    return V3(i * 1.0, 2.0, -0.5)

def bigger(a, b):
    return F(a.x if a.x > b.x else b.x, a.y if a.y > b.y else b.y)

def carried(n, start, stop_at, raise_at):
    acc = start
    for i in range(n):
        if i == stop_at:
            break
        if i == raise_at:
            raise ValueError(acc)
        acc = bigger(acc, F(i * 1.0, (n - i) * 1.0))
    return acc

def one(i):
    return (i,)

def concat_step(acc, i):
    t = pair(i)
    a, b = t
    acc = acc + t
    acc = one(a - b) + acc
    return acc[1:-1] if len(acc) > 6 else acc

def concats(n, acc):
    for i in range(n):
        acc = concat_step(acc, i)
    return acc

def matcher(n, limit):
    t = 0
    for i in range(n):
        match make(i):
            case F(x, y):
                t -= 1
            case P(a, b) if i >= limit:
                t += 1000
            case P(a, b):
                t += a * b
    return t

def fused_step(i, c, limit):
    d = (v3(i) + c) - c
    if i >= limit:
        return d.x + float(d is d)
    return dot(d, c)

def fused(n, c, limit):
    t = 0.0
    for i in range(n):
        t += fused_step(i, c, limit)
    return t

@grail.record
class Sq:
    side: float

@grail.record
class Rect:
    w: float
    h: float

@grail.generic
def area(shape, k):
    raise TypeError

@area.impl
def _(shape: Sq, k: V3) -> float:
    return shape.side * shape.side * k.x

@area.impl
def _(shape: Rect, k: V3) -> float:
    return shape.w * shape.h * k.x

def areas(n, shapes):
    t = 0.0
    for i in range(n):
        t += area(Sq(i * 1.0), v3(2.0)) + area(shapes[i % len(shapes)], v3(1.0))
    return t
"""


def load_virtual():
    ns = {}
    exec(VIRTUAL, ns)
    return ns


class VirtualObjectTests(unittest.TestCase):
    def test_cross_call_record(self):
        m = load_virtual()
        for _ in range(REPEAT):
            self.assertEqual(m["chain"](100), 100 * 100)
        if tier2_enabled():
            uops = executor_uops(m["chain"])
            self.assertIn("_VO_NEW_RECORD", uops)
            self.assertIn("_VO_RELEASE", uops)
            self.assertNotIn("_CALL_RECORD", uops)
            self.assertNotIn("_CALL_RECORD_CHECKED", uops)

    def test_returned_tuple(self):
        m = load_virtual()
        for _ in range(REPEAT):
            self.assertEqual(m["pairs"](100), 3 * (99 * 100 // 2))
        if tier2_enabled():
            uops = executor_uops(m["pairs"])
            self.assertIn("_VO_NEW_TUPLE", uops)
            self.assertIn("_VO_UNPACK", uops)
            self.assertNotIn("_BUILD_TUPLE", uops)

    def test_escape_keeps_allocation(self):
        m = load_virtual()
        P = m["P"]
        for _ in range(REPEAT):
            out = []
            self.assertEqual(m["escaping"](100, out), 99 * 100 // 2)
            self.assertEqual(out, [P(i, i + 1) for i in range(100)])
            self.assertEqual(len({id(p) for p in out}), 100)
        if tier2_enabled():
            self.assertNotIn("_VO_NEW_RECORD", executor_uops(m["escaping"]))

    def test_side_exit_rebuilds_aliases(self):
        m = load_virtual()
        n = 100
        for _ in range(REPEAT):
            self.assertEqual(m["exits"](n, n), sum(range(1, n + 1)))
        if tier2_enabled():
            self.assertIn("_VO_NEW_RECORD", executor_uops(m["exits"]))
        # Leave the trace at the branch while p is virtual in two locals of
        # check(): tier 1 must see one real record in both.
        for limit in (n // 2, 0, n - 1):
            expected = sum(i + 1 for i in range(limit)) + \
                       sum(1 + i for i in range(limit, n))
            self.assertEqual(m["exits"](n, limit), expected)

    def test_exception_in_inlined_frame(self):
        m = load_virtual()
        F = m["F"]
        for _ in range(REPEAT):
            caught = []
            expected = 0.0
            for i in range(100):
                if i % 16:
                    expected += 1.0 / (i % 16)
            self.assertEqual(m["ratios"](100, caught), expected)
            # The frame of ratio() shows the record it was given.
            self.assertEqual(caught, [F(1.0, 0.0)] * 7)
            self.assertTrue(all(type(p) is F for p in caught))

    def test_released_fields_are_finalized(self):
        m = load_virtual()
        D = m["D"]
        for _ in range(REPEAT):
            before = D.deleted
            self.assertEqual(m["dels"](100), 99 * 100 // 2)
            self.assertEqual(D.deleted - before, 100)

    def test_loop_carried_record_stays_real(self):
        m = load_virtual()
        P = m["P"]
        for _ in range(REPEAT):
            self.assertEqual(m["last"](100), P(99, 100))

    def test_nested_record(self):
        m = load_virtual()
        n = 100
        for _ in range(REPEAT):
            self.assertEqual(m["segs"](n, n), 2 * n)
        if tier2_enabled():
            counts = executor_uop_counts(m["segs"])
            self.assertEqual(counts["_VO_NEW_RECORD"], 3, counts)
            self.assertNotIn("_CALL_RECORD_CHECKED", counts)
        for limit in (0, n // 2):
            self.assertEqual(m["segs"](n, limit), 2 * n + 100 * (n - limit))

    def test_fused_float_uops(self):
        # Element-wise operator results and operands, and float programs
        # (ElementwiseFusionTests) on virtual records.
        m = load_virtual()
        V3, dot = m["V3"], m["dot"]
        c = V3(0.25, -1.5, 3.0)
        n = 100

        def expected(limit):
            t = 0.0
            for i in range(n):
                d = V3((i * 1.0 + c.x) - c.x, (2.0 + c.y) - c.y,
                       (-0.5 + c.z) - c.z)
                t += d.x + 1.0 if i >= limit else dot(d, c)
            return t

        for _ in range(REPEAT):
            self.assertEqual(m["fused"](n, c, n), expected(n))
        if tier2_enabled():
            counts = executor_uop_counts(m["fused"])
            self.assertEqual(counts["_VO_ELEMENTWISE_FLOAT"], 2, counts)
            self.assertEqual(counts["_VO_FLOAT_PROGRAM"], 1, counts)
            self.assertNotIn("_GRAIL_ELEMENTWISE_FLOAT", counts)
            self.assertNotIn("_CALL_RECORD_CHECKED", counts)
        # Leave the trace with d virtual.
        for limit in (n // 2, 0):
            self.assertEqual(m["fused"](n, c, limit), expected(limit))

    def test_generic_dispatch(self):
        m = load_virtual()
        Sq, Rect = m["Sq"], m["Rect"]
        n = 100
        shapes = [Sq(3.0), Rect(2.0, 5.0)]
        expected = sum(i * i * 2.0 + (9.0 if i % 2 == 0 else 10.0) for i in range(n))
        for _ in range(REPEAT):
            self.assertEqual(m["areas"](n, shapes), expected)
        if tier2_enabled():
            counts = executor_uop_counts(m["areas"])
            self.assertGreaterEqual(counts["_VO_NEW_RECORD"], 2, counts)
        # Another shape type takes a side exit with virtual arguments.
        self.assertEqual(m["areas"](4, [Rect(1.0, 1.0)]),
                         sum(i * i * 2.0 + 1.0 for i in range(4)))

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_recovery_allocation_failure(self):
        # Rebuilding virtual objects at an exit can fail: MemoryError at the
        # exit's target, and the interpreter state stays consistent. Without
        # the record freelist, so that record allocations can fail.
        from test.support import import_helper
        from test.support.script_helper import assert_python_ok
        import_helper.import_module("_testcapi")
        code = """if 1:
            import _testcapi
            from test.test_grail_jit import load_virtual
            m = load_virtual()
            n = 100
            for _ in range(100):
                m["exits"](n, n); m["segs"](n, n)
            raised = 0
            for start in range(200):
                for name, limit in (("exits", 0), ("segs", n // 2)):
                    _testcapi.set_nomemory(start, start + 1)
                    try:
                        m[name](n, limit)
                    except MemoryError:
                        raised += 1
                    finally:
                        _testcapi.remove_mem_hooks()
            assert raised > 0
            assert m["exits"](n, n) == sum(range(1, n + 1))
            assert m["segs"](n, n) == 2 * n
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_RECORD_FREELIST="0")

    def test_loop_carried(self):
        # The accumulator stays virtual across iterations: made virtual at
        # the trace's entry, its next value moved into its slots at the end.
        m = load_virtual()
        F, carried = m["F"], m["carried"]
        n = 100
        start = F(-1.0, -1.0)
        expected = F((n - 1) * 1.0, n * 1.0)
        for _ in range(REPEAT):
            self.assertEqual(carried(n, start, -1, -1), expected)
        if tier2_enabled():
            counts = executor_uop_counts(carried)
            self.assertEqual(counts["_VO_ENTER_LOCAL"], 1, counts)
            self.assertNotIn("_CALL_RECORD_CHECKED", counts)
            self.assertNotIn("_CALL_RECORD", counts)
        # Leaving at the first iteration gives back the entry object itself.
        self.assertIs(carried(n, start, 0, -1), start)
        for stop in (1, n // 2, n - 1):
            self.assertEqual(carried(n, start, stop, -1),
                             F((stop - 1) * 1.0, n * 1.0))
        for at in (1, n // 2, n - 1):
            with self.assertRaises(ValueError) as cm:
                carried(n, start, -1, at)
            self.assertEqual(cm.exception.args[0], F((at - 1) * 1.0, n * 1.0))
        # Entry with another type: the prologue deopts.
        # Entry with another value: the prologue deopts.
        self.assertIsNone(carried(n, None, 0, -1))
        for _ in range(3):
            self.assertEqual(carried(n, start, -1, -1), expected)

    def test_final_exit(self):
        # A trace cut at the length limit while p is virtual ends with an
        # exit that rebuilds it (_VO_RECOVER before the _EXIT_TRACE).
        lines = ["def long_body(n):", "    t = 0", "    for i in range(n):",
                 "        p = make(i)"]
        lines += ["        t += p.x" if k % 2 else "        t -= p.y" for k in range(400)]
        lines += ["        if i == n - 1:", "            return t, p", "    return t, None"]
        m = load_virtual()
        exec("\n".join(lines), m)
        n = 100
        for _ in range(REPEAT):
            self.assertEqual(m["long_body"](n, ), (-200 * n, m["P"](n - 1, n)))
        if tier2_enabled():
            self.assertIn("_VO_NEW_RECORD", executor_uops(m["long_body"]))

    def test_tuple_concat(self):
        # tuple + tuple with a virtual operand builds the result from its
        # items (_VO_TUPLE_CONCAT). A tuple that is only concatenated stays
        # real (one(...)); t, unpacked too, stays virtual.
        m = load_virtual()
        concats = m["concats"]

        def expected(n, acc):
            for i in range(n):
                acc = (-i,) + acc + (i, i * 2)
                if len(acc) > 6:
                    acc = acc[1:-1]
            return acc

        for _ in range(REPEAT):
            self.assertEqual(concats(100, (1, 2)), expected(100, (1, 2)))
        if tier2_enabled():
            self.assertIn("_VO_TUPLE_CONCAT", executor_uops(m["concat_step"]) |
                          executor_uops(concats))
        # An empty real operand: the result is the virtual operand itself.
        self.assertEqual(concats(3, ()), expected(3, ()))
        self.assertEqual(concats(5, ()), expected(5, ()))

    def test_match(self):
        # Class patterns on a virtual record fold by its type
        # (_MATCH_CLASS_RECORD_NONE, _VO_MATCH_CLASS_RECORD_SELF) and its
        # fields unpack from the shadow slots.
        m = load_virtual()
        n = 100
        for _ in range(REPEAT):
            self.assertEqual(m["matcher"](n, n), sum(i * (i + 1) for i in range(n)))
        if tier2_enabled():
            counts = executor_uop_counts(m["matcher"])
            self.assertGreaterEqual(counts["_VO_MATCH_CLASS_RECORD_SELF"], 1, counts)
            self.assertNotIn("_CALL_RECORD_CHECKED", counts)
            self.assertNotIn("_CALL_RECORD", counts)
        # The guard of the second case fails: an exit with p virtual.
        for limit in (0, n // 2):
            self.assertEqual(m["matcher"](n, limit),
                             sum(i * (i + 1) for i in range(limit)) + 1000 * (n - limit))

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_loop_carried_pending_calls(self):
        # Pending calls (and GIL drop requests) while the accumulator is
        # virtual leave the trace at _VO_CHECK_PERIODIC, which rebuilds it:
        # the calls see a real record in the loop's frame.
        import threading, time
        from test.support import import_helper
        _testcapi = import_helper.import_module("_testcapi")
        m = load_virtual()
        F, carried = m["F"], m["carried"]
        start = F(-1.0, -1.0)
        for _ in range(REPEAT):
            carried(100, start, -1, -1)
        seen = []

        def probe():
            f = sys._getframe(1)
            while f is not None and f.f_code is not carried.__code__:
                f = f.f_back
            if f is not None:
                seen.append(f.f_locals.get("acc"))
            return 0

        stop = False

        def pusher():
            while not stop:
                try:
                    _testcapi._pending_threadfunc(probe, num=1, blocking=False)
                except Exception:
                    pass
                time.sleep(0.0005)

        t = threading.Thread(target=pusher)
        t.start()
        try:
            n = 100_000
            self.assertEqual(carried(n, start, -1, -1), F((n - 1) * 1.0, n * 1.0))
        finally:
            stop = True
            t.join()
        self.assertTrue(all(type(acc) is F for acc in seen), seen[:5])

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_loop_carried_profiler(self):
        # A profiler installed for all threads during the loop invalidates
        # the executor; the exit rebuilds the accumulator, and profiled
        # calls see real records.
        import threading, time
        m = load_virtual()
        F, carried, bigger = m["F"], m["carried"], m["bigger"]
        start = F(-1.0, -1.0)
        for _ in range(REPEAT):
            carried(100, start, -1, -1)
        seen = []

        def profiler(frame, event, arg):
            if frame.f_code is bigger.__code__ and event == "call":
                seen.append(frame.f_locals["a"])

        def installer():
            time.sleep(0.005)
            threading.setprofile_all_threads(profiler)

        t = threading.Thread(target=installer)
        t.start()
        try:
            n = 200_000
            self.assertEqual(carried(n, start, -1, -1), F((n - 1) * 1.0, n * 1.0))
        finally:
            t.join()
            threading.setprofile_all_threads(None)
        # The loop may also end before the profiler is installed.
        self.assertTrue(all(type(a) is F for a in seen))

    def test_tuple_of_record(self):
        m = load_virtual()
        for _ in range(REPEAT):
            self.assertEqual(m["steps"](100), sum(i + 1 + 3 * i for i in range(100)))
        if tier2_enabled():
            uops = executor_uops(m["steps"])
            self.assertIn("_VO_NEW_TUPLE", uops)
            self.assertIn("_VO_NEW_RECORD", uops)

    def test_deferred_release_order(self):
        # drop() releases the last reference to a Logged while p is
        # virtual: the finalizer still runs before the caller continues.
        m = load_virtual()
        for _ in range(REPEAT):
            del m["LOG"][:]
            self.assertEqual(m["drops"](100), 100 * 100)
            self.assertEqual(m["LOG"], ["del", "it"] * 100)
        if tier2_enabled():
            uops = executor_uops(m["drops"])
            self.assertIn("_VO_NEW_RECORD", uops)
            self.assertIn("_VO_POP_TOP_DEFER", uops)

    def test_return_out_of_trace_root(self):
        # Traces that start in shared() return its tuple to whichever
        # caller is running; the stack depths of the callers differ.
        m = load_virtual()
        for i in range(400):
            n = 20 + i % 3
            t = n * (n - 1) // 2
            self.assertEqual(m["caller_a"](n), t + n)
            self.assertEqual(m["caller_b"](n), 2 * t + 3 * n)

    @unittest.skipUnless(tier2_enabled(), "needs the JIT or the uop interpreter")
    def test_ablation(self):
        from test.support.script_helper import assert_python_ok
        code = f"""if 1:
            import sys
            sys.path.insert(0, {repr(__file__.rsplit("/", 1)[0])})
            from test.test_grail_jit import load_virtual, executor_uops
            m = load_virtual()
            for _ in range(100):
                assert m["chain"](100) == 100 * 100
            uops = executor_uops(m["chain"])
            assert not any(u.startswith("_VO_") for u in uops), uops
        """
        assert_python_ok("-c", code, PYTHON_GRAIL_JIT_VIRTUAL_OBJECTS="0")


def sum_repeat(value, n):
    t = 0.0
    for _ in range(n):
        t += value
    return t


if __name__ == "__main__":
    unittest.main()
