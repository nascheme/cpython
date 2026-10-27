"""Tier-2 fast paths for general Python code: results and trace contents.

Each loop runs long enough to be traced. The result checks hold on every
build; the trace-content checks need the JIT or the uop interpreter.
"""
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
