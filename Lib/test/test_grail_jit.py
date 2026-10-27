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


def sum_repeat(value, n):
    t = 0.0
    for _ in range(n):
        t += value
    return t


if __name__ == "__main__":
    unittest.main()
