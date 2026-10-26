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
        self.assertTrue({"_LOAD_ATTR_RECORD", "_LOAD_ATTR_RECORD_TYPED"} & uops)
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
        self.assertIn("_GRAIL_BINARY_LOAD_IMPL", uops)
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
        self.assertIn("_LOAD_ATTR_RECORD_TYPED", uops)
        self.assertIn("_GRAIL_BINARY_LOAD_IMPL", uops)
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


if __name__ == "__main__":
    unittest.main()
