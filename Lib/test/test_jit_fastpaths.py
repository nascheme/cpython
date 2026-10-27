"""Tier-2 fast paths for general Python code: results and trace contents.

Each loop runs long enough to be traced. The result checks hold on every
build; the trace-content checks need the JIT or the uop interpreter.
"""
import sys
import unittest

ITERATIONS = 20000
HALF = ITERATIONS // 2
REPEAT = 100   # calls of 100 iterations, enough to trace the loops


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


class FloatIntConstCompareTests(unittest.TestCase):
    """A known float compared with an int constant compares as doubles."""

    def test_results(self):
        ns = {}
        exec("""if 1:
            def count(xs):
                t = 0
                for x in xs:
                    y = x * 1.0
                    t += ((y < 0) + 2 * (y <= 0) + 4 * (y > 0) +
                          8 * (y >= 0) + 16 * (y == 0) + 32 * (y != 0) +
                          64 * (y < 1073741823) + 128 * (y > -1073741823) +
                          256 * (y == 7))
                return t
            """, ns)
        count = ns["count"]
        values = [0.0, -0.0, 0.5, -0.5, 7.0, 7.000000000000001, 1073741823.0,
                  -1073741823.0, 1e300, -1e300, float("inf"), float("-inf"),
                  float("nan")] * 10

        def expected(xs):
            # Through operator's functions, not the traced comparison.
            from operator import lt, le, gt, ge, eq, ne
            t = 0
            for y in xs:
                t += (lt(y, 0) + 2 * le(y, 0) + 4 * gt(y, 0) + 8 * ge(y, 0) +
                      16 * eq(y, 0) + 32 * ne(y, 0) + 64 * lt(y, 1073741823) +
                      128 * gt(y, -1073741823) + 256 * eq(y, 7))
            return t

        for _ in range(REPEAT):
            self.assertEqual(count(values), expected(values))
        if tier2_enabled():
            self.assertIn("_COMPARE_OP_FLOAT_INT_CONST", executor_uops(count))


class TupleTruthTests(unittest.TestCase):
    def test_tuple_truth(self):
        ns = {}
        exec("""if 1:
            def count(xs):
                t = 0
                for x in xs:
                    if x:
                        t += 1
                    if not x:
                        t += 10
                return t
            """, ns)
        count = ns["count"]
        tuples = [(), (1,), (None,), (0, 0)] * 25
        for _ in range(REPEAT):
            self.assertEqual(count(tuples), 75 + 250)
        if tier2_enabled():
            self.assertIn("_TO_BOOL_TUPLE", executor_uops(count))
        mixed = [(), [], [0], 0, 1, "", "a", None, (1,)] * 10
        for _ in range(REPEAT):
            self.assertEqual(count(mixed), 40 + 500)


class FloatIntCompareTests(unittest.TestCase):
    """A known float compared with a known compact int, in either order."""

    def test_results(self):
        ns = {}
        exec("""if 1:
            def count(xs, ns):
                t = 0
                for x in xs:
                    y = x * 1.0
                    for n in ns:
                        m = n + 0
                        t += ((y < m) + 2 * (m <= y) + 4 * (y > m) +
                              8 * (m >= y) + 16 * (y == m) + 32 * (m != y))
                return t
            """, ns)
        count = ns["count"]
        from operator import lt, le, gt, ge, eq, ne
        xs = [0.0, -0.0, 0.5, -7.0, 7.0, 1073741823.0, 1e300, float("inf"),
              float("-inf"), float("nan")]
        ints = [0, 7, -7, 1073741823, -1073741823]
        expected = sum(lt(x, n) + 2 * le(n, x) + 4 * gt(x, n) + 8 * ge(n, x) +
                       16 * eq(x, n) + 32 * ne(n, x) for x in xs for n in ints)
        for _ in range(REPEAT):
            self.assertEqual(count(xs, ints), expected)
        if tier2_enabled():
            self.assertIn("_COMPARE_OP_FLOAT_INT", executor_uops(count))


class MakeFunctionTests(unittest.TestCase):
    """Nested functions made in a trace take the module name and builtins
    of the current globals, also after they change."""

    def test_module_and_builtins(self):
        import builtins
        ns = {"__name__": "mod_a", "__builtins__": builtins}
        exec("""if 1:
            def make(n, switch, action):
                out = []
                for i in range(n):
                    if i == switch:
                        action()
                    def f():
                        return len("ab")
                    out.append((f.__module__, f.__builtins__ is BUILTINS, f()))
                return out
            """, ns)
        ns["BUILTINS"] = builtins.__dict__
        fake = dict(builtins.__dict__)
        fake["len"] = lambda x: 99

        def change():
            ns["__name__"] = "mod_b"
            ns["__builtins__"] = fake

        out = ns["make"](ITERATIONS, HALF, change)
        self.assertEqual(out[0], ("mod_a", True, 2))
        self.assertEqual(out[HALF - 1], ("mod_a", True, 2))
        self.assertEqual(out[HALF], ("mod_b", False, 99))
        self.assertEqual(out[-1], ("mod_b", False, 99))
        if tier2_enabled():
            self.assertIn("_MAKE_FUNCTION_ENV", executor_uops(ns["make"]))


if __name__ == "__main__":
    unittest.main()
