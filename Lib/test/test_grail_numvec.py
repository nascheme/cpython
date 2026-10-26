# Copied from grail-compat tests/ by tools/sync_to_proto.py; do not edit here.
"""Unboxed FloatVector/IntVector API, buffers, transients and threads."""
import array
import collections.abc
import concurrent.futures
import copy
import math
import pickle
import random
import sys
import sysconfig
import unittest

import grail

# Pure Python can export a buffer only through __buffer__ (3.12+).
HAS_BUFFER_EXPORT = getattr(grail, "backend", "shim") != "shim" or sys.version_info >= (3, 12)

from grail import Atom, FloatVector, IntVector, PVector

INT64_MAX = 2**63 - 1
INT64_MIN = -2**63
NAN = float("nan")


class Number:
    def __init__(self, value):
        self.value = value

    def __float__(self):
        return float(self.value)

    def __index__(self):
        return int(self.value)


class VectorTests(unittest.TestCase):
    def test_construction(self):
        for source in ([], (), range(0), iter([]), array.array("d")):
            self.assertEqual(FloatVector(source), FloatVector())
            self.assertEqual(IntVector(source), IntVector())
        a = FloatVector(source=[1, 2.5, True, Number(4)])
        self.assertEqual(a.tolist(), [1.0, 2.5, 1.0, 4.0])
        self.assertTrue(all(type(x) is float for x in a))
        b = IntVector((1, -2, True, Number(4), INT64_MAX, INT64_MIN))
        self.assertEqual(list(b), [1, -2, 1, 4, INT64_MAX, INT64_MIN])
        self.assertTrue(all(type(x) is int for x in b))
        self.assertIs(FloatVector(a), a)
        self.assertIs(IntVector(b), b)
        self.assertEqual(FloatVector(IntVector([1, 2])), FloatVector([1.0, 2.0]))
        self.assertEqual(IntVector(range(5)), IntVector([0, 1, 2, 3, 4]))
        self.assertEqual(IntVector(b"ab"), IntVector([97, 98]))
        source = [1.0, 2.0]
        a = FloatVector(source)
        source[0] = 9.0
        source.append(10.0)
        self.assertEqual(a.tolist(), [1.0, 2.0])
        for cls in (FloatVector, IntVector):
            self.assertIsInstance(cls(), collections.abc.Sequence)
            for source in (None, 1, ["a"], [None]):
                with self.assertRaises(TypeError):
                    cls(source)
        with self.assertRaises(TypeError):
            IntVector([1.0])
        with self.assertRaises(TypeError):
            IntVector([1, 2.5])
        for value in (INT64_MAX + 1, INT64_MIN - 1):
            with self.assertRaises(OverflowError):
                IntVector([value])
        with self.assertRaises(OverflowError):
            FloatVector([10**400])

    def test_buffer_construction(self):
        self.assertEqual(FloatVector(array.array("d", [1.5, 2.5])), FloatVector([1.5, 2.5]))
        self.assertEqual(IntVector(array.array("q", [3, -4])), IntVector([3, -4]))
        self.assertEqual(IntVector(array.array("l", [3, -4])), IntVector([3, -4]))
        # Other formats fall back to element conversion.
        self.assertEqual(FloatVector(array.array("f", [0.5])), FloatVector([0.5]))
        self.assertEqual(IntVector(array.array("b", [-1, 2])), IntVector([-1, 2]))
        self.assertEqual(FloatVector(array.array("i", [7])), FloatVector([7.0]))
        with self.assertRaises(TypeError):
            IntVector(array.array("d", [1.0]))
        # Strided and multidimensional exporters.
        view = memoryview(array.array("d", range(10)))[::3]
        self.assertEqual(FloatVector(view), FloatVector([0, 3, 6, 9]))
        view = memoryview(array.array("q", range(10)))[::-1]
        self.assertEqual(IntVector(view).tolist(), list(range(9, -1, -1)))
        grid = memoryview(array.array("d", range(6))).cast("B").cast("d", (2, 3))
        with self.assertRaises(NotImplementedError):  # iterates, as list(grid)
            FloatVector(grid)
        # Round trip through another vector's buffer.
        if HAS_BUFFER_EXPORT:
            a = FloatVector([1.0, -0.0, math.inf])
            self.assertEqual(FloatVector(memoryview(a)).tolist(), a.tolist())

    @unittest.skipUnless(HAS_BUFFER_EXPORT, "buffer export needs Python 3.12+")
    def test_buffer_export(self):
        a = FloatVector([1.0, 2.5, -3.0])
        m = memoryview(a)
        self.assertTrue(m.readonly)
        self.assertEqual((m.format, m.itemsize, m.ndim, m.shape, m.strides),
                         ("d", 8, 1, (3,), (8,)))
        self.assertTrue(m.c_contiguous)
        self.assertEqual(m.tolist(), [1.0, 2.5, -3.0])
        self.assertEqual(bytes(m), array.array("d", [1.0, 2.5, -3.0]).tobytes())
        with self.assertRaises(TypeError):
            m[0] = 9.0
        b = IntVector([1, -2])
        m = memoryview(b)
        self.assertEqual((m.format, m.shape), ("q", (2,)))
        self.assertEqual(m.tolist(), [1, -2])
        self.assertEqual(memoryview(IntVector()).tolist(), [])
        self.assertEqual(array.array("d", a).tolist(), a.tolist())
        with self.assertRaises(TypeError):
            memoryview(bytearray(24))[:] = a.mutate()  # not a buffer
        import io
        with self.assertRaises(TypeError):
            io.BytesIO(b"x" * 24).readinto(a)  # needs a writable buffer
        try:
            from _testbuffer import ndarray, PyBUF_FULL, PyBUF_FULL_RO
        except ImportError:
            pass
        else:
            self.assertEqual(ndarray(a, getbuf=PyBUF_FULL_RO).tolist(), a.tolist())
            with self.assertRaises(BufferError):
                ndarray(a, getbuf=PyBUF_FULL)
        # The vector outlives its exported views.
        m = memoryview(FloatVector(range(1000)))
        self.assertEqual(m[999], 999.0)

    def test_indexing(self):
        a = IntVector(range(100))
        for i in range(100):
            self.assertEqual(a[i], i)
            self.assertEqual(a[i - 100], i)
        self.assertEqual(a[Number(5)], 5)
        for index in (100, -101, sys.maxsize, -sys.maxsize - 1, 10**100):
            with self.assertRaises(IndexError):
                a[index]
        for index in ("0", 1.0, None):
            with self.assertRaises(TypeError):
                a[index]
        with self.assertRaises(IndexError):
            FloatVector()[0]

    def test_slicing(self):
        model = list(range(50))
        a = IntVector(model)
        self.assertIs(a[:], a)
        for start in (None, -60, -3, 0, 7, 49, 60):
            for stop in (None, -60, -3, 0, 7, 49, 60):
                for step in (None, 1, 2, 7, -1, -3, sys.maxsize):
                    s = slice(start, stop, step)
                    self.assertEqual(a[s].tolist(), model[s])
                    self.assertIs(type(a[s]), IntVector)
        with self.assertRaises(ValueError):
            a[::0]
        f = FloatVector([0.5, 1.5, 2.5])
        self.assertEqual(f[1:], FloatVector([1.5, 2.5]))

    def test_set_append_pop(self):
        a = FloatVector([1.0, 2.0, 3.0])
        b = a.set(1, 9)
        self.assertEqual(b.tolist(), [1.0, 9.0, 3.0])
        self.assertEqual(a.tolist(), [1.0, 2.0, 3.0])
        self.assertEqual(a.set(-1, 0.5)[2], 0.5)
        for index in (3, -4):
            with self.assertRaises(IndexError):
                a.set(index, 1.0)
        with self.assertRaises(TypeError):
            a.set(0, "x")
        with self.assertRaises(TypeError):
            a.set()
        self.assertEqual(a.append(4).tolist(), [1.0, 2.0, 3.0, 4.0])
        self.assertEqual(a.pop().tolist(), [1.0, 2.0])
        self.assertEqual(FloatVector().append(1), FloatVector([1.0]))
        self.assertEqual(FloatVector([1]).pop(), FloatVector())
        with self.assertRaises(IndexError):
            FloatVector().pop()
        with self.assertRaises(TypeError):
            IntVector([1]).append(1.5)
        with self.assertRaises(OverflowError):
            IntVector([1]).set(0, INT64_MAX + 1)

    def test_random_history(self):
        rng = random.Random(5)
        versions = [(IntVector(), [])]
        for _ in range(2000):
            vec, model = versions[rng.randrange(len(versions))]
            op = rng.randrange(3)
            if op == 0:
                value = rng.randrange(INT64_MIN, INT64_MAX)
                vec, model = vec.append(value), model + [value]
            elif op == 1 and model:
                vec, model = vec.pop(), model[:-1]
            elif model:
                i = rng.randrange(len(model))
                value = rng.randrange(-100, 100)
                vec, model = vec.set(i, value), model[:i] + [value] + model[i + 1:]
            versions.append((vec, model))
        for vec, model in versions:
            self.assertEqual(vec.tolist(), model)

    def test_concat(self):
        a, b = FloatVector([1, 2]), FloatVector([3])
        self.assertEqual((a + b).tolist(), [1.0, 2.0, 3.0])
        self.assertIs(a + FloatVector(), a)
        self.assertIs(FloatVector() + a, a)
        for other in ([3.0], (3.0,), IntVector([3]), PVector([3.0])):
            with self.assertRaises(TypeError):
                a + other

    def test_comparison(self):
        f = FloatVector
        self.assertEqual(f([1, 2]), f([1.0, 2.0]))
        self.assertNotEqual(f([1, 2]), f([1, 2, 3]))
        self.assertEqual(f([0.0]), f([-0.0]))
        self.assertLess(f([1, 2]), f([1, 3]))
        self.assertLess(f([1, 2]), f([1, 2, 0]))
        self.assertGreater(f([2]), f([1, 9]))
        self.assertLessEqual(f([1]), f([1]))
        self.assertGreaterEqual(f([1]), f([1]))
        self.assertLess(IntVector([INT64_MIN]), IntVector([INT64_MAX]))
        # Elements compare by value: NaN is never equal, even to itself.
        n = f([NAN])
        self.assertNotEqual(n, n)
        self.assertFalse(n < n or n > n or n <= n or n >= n)
        self.assertEqual(f([1, NAN]) < f([1, 2]), False)
        # Only the same vector type compares equal; lists and tuples never do.
        self.assertNotEqual(f([1]), IntVector([1]))
        self.assertNotEqual(f([1]), [1.0])
        self.assertNotEqual(IntVector([1]), (1,))
        self.assertNotEqual(f([1]), PVector([1.0]))
        with self.assertRaises(TypeError):
            f([1]) < IntVector([1])
        with self.assertRaises(TypeError):
            f([1]) < [1.0]

    def test_hash(self):
        rng = random.Random(3)
        for size in (0, 1, 2, 100):
            values = [rng.uniform(-1e300, 1e300) for _ in range(size)]
            self.assertEqual(hash(FloatVector(values)), hash(tuple(values)))
            ints = [rng.randrange(INT64_MIN, INT64_MAX) for _ in range(size)]
            self.assertEqual(hash(IntVector(ints)), hash(tuple(ints)))
        special = [0.0, -0.0, 1.0, -1.0, 0.5, math.inf, -math.inf, 2.0**61, 1e-310]
        self.assertEqual(hash(FloatVector(special)), hash(tuple(special)))
        special = [0, -1, -2, 1, 2**61 - 2, 2**61 - 1, 2**61, -2**61 + 1, -2**61,
                   INT64_MAX, INT64_MIN, INT64_MIN + 1]
        self.assertEqual(hash(IntVector(special)), hash(tuple(special)))
        self.assertEqual(hash(FloatVector([1, 2])), hash(IntVector([1, 2])))
        a = FloatVector([NAN])
        self.assertEqual(hash(a), hash(FloatVector([NAN])))
        self.assertEqual({FloatVector([1.0]): 1}[FloatVector([1.0])], 1)

    def test_search(self):
        f = FloatVector([1.0, 2.0, 1.0, NAN, -0.0])
        self.assertIn(1.0, f)
        self.assertIn(1, f)
        self.assertIn(True, f)
        self.assertIn(0.0, f)
        self.assertNotIn(NAN, f)
        self.assertNotIn("1", f)
        self.assertEqual(f.count(1.0), 2)
        self.assertEqual(f.count(1), 2)
        self.assertEqual(f.count(NAN), 0)
        self.assertEqual(f.index(1.0), 0)
        self.assertEqual(f.index(1.0, 1), 2)
        self.assertEqual(f.index(1.0, -3), 2)
        self.assertEqual(f.index(2, 0, 2), 1)
        with self.assertRaises(ValueError):
            f.index(2, 0, 1)
        with self.assertRaises(ValueError):
            f.index(7)
        big = FloatVector([2.0**53])
        self.assertIn(2**53, big)
        self.assertNotIn(2**53 + 1, big)
        i = IntVector([1, INT64_MAX, -3])
        self.assertIn(INT64_MAX, i)
        self.assertNotIn(INT64_MAX + 1, i)
        self.assertNotIn(10**100, i)
        self.assertIn(1.0, i)
        self.assertNotIn(1.5, i)
        self.assertIn(Number(-3).value, i)
        self.assertEqual(i.index(-3.0), 2)

        class Bad:
            def __eq__(self, other):
                raise ZeroDivisionError
        with self.assertRaises(ZeroDivisionError):
            Bad() in i
        with self.assertRaises(ZeroDivisionError):
            i.count(Bad())

    def test_iteration_and_match(self):
        a = IntVector(range(100))
        self.assertEqual(list(a), list(range(100)))
        self.assertEqual(list(reversed(a)), list(range(99, -1, -1)))
        it = iter(a)
        self.assertEqual(it.__length_hint__(), 100)
        next(it)
        self.assertEqual(it.__length_hint__(), 99)
        self.assertEqual(list(it), list(range(1, 100)))
        self.assertEqual(it.__length_hint__(), 0)
        self.assertEqual(list(it), [])
        self.assertEqual(reversed(a).__length_hint__(), 100)
        self.assertEqual(list(iter(IntVector())), [])
        match FloatVector([1, 2, 3]):
            case [x, *rest]:
                self.assertEqual((x, rest), (1.0, [2.0, 3.0]))
            case _:
                self.fail("sequence pattern did not match")
        match IntVector([1]):
            case {}:
                self.fail("matched a mapping pattern")
            case [1]:
                pass

    def test_repr(self):
        self.assertEqual(repr(FloatVector([1, 2.5])), "FloatVector([1.0, 2.5])")
        self.assertEqual(repr(IntVector()), "IntVector([])")
        self.assertEqual(repr(FloatVector([NAN, -math.inf])), "FloatVector([nan, -inf])")

    def test_closed_storage(self):
        a = FloatVector([1.0])
        for cls in (FloatVector, IntVector):
            with self.assertRaises(TypeError):
                class Sub(cls):
                    pass
        with self.assertRaises(AttributeError):
            a.x = 1
        with self.assertRaises(TypeError):
            a[0] = 2.0
        with self.assertRaises(TypeError):
            del a[0]
        self.assertFalse(hasattr(a, "__dict__"))

    def test_copy_and_pickle(self):
        for a in (FloatVector([1.0, -0.0, math.inf, 1e-310]), IntVector([INT64_MIN, 0, INT64_MAX]),
                  FloatVector(), IntVector()):
            self.assertIs(copy.copy(a), a)
            self.assertIs(copy.deepcopy(a), a)
            self.assertIs(copy.deepcopy([a, a])[0], copy.deepcopy([a, a])[1])
            for proto in range(pickle.HIGHEST_PROTOCOL + 1):
                b = pickle.loads(pickle.dumps(a, proto))
                self.assertIs(type(b), type(a))
                self.assertEqual(b, a)
        n = pickle.loads(pickle.dumps(FloatVector([NAN])))
        self.assertTrue(math.isnan(n[0]))


class TransientTests(unittest.TestCase):
    def test_lifecycle(self):
        a = FloatVector([1, 2, 3])
        with a.mutate() as t:
            self.assertEqual(len(t), 3)
            t[0] = 10
            t[-1] = Number(30)
            self.assertEqual(t[0], 10.0)
            self.assertEqual(t[-1], 30.0)
            t.append(4)
            t.extend([5, 6.5])
            t.extend(FloatVector([7]))
            t.extend(array.array("d", [8]))
            t.extend(iter([9]))
            self.assertEqual(t.pop(), 9.0)
        b = t.finish()
        self.assertEqual(b.tolist(), [10, 2, 30, 4, 5, 6.5, 7, 8])
        self.assertEqual(a.tolist(), [1.0, 2.0, 3.0])
        for op in (lambda: t.finish(), lambda: t.append(1), lambda: len(t), lambda: t[0]):
            with self.assertRaises(RuntimeError):
                op()
        t = a.mutate()
        self.assertEqual(t.finish(), a)
        with self.assertRaises(RuntimeError):
            t.finish()
        # Exceptional exit aborts.
        with self.assertRaises(ZeroDivisionError):
            with a.mutate() as t:
                t.append(1)
                1 / 0
        with self.assertRaises(RuntimeError):
            t.finish()
        t = a.mutate()
        with t:
            with self.assertRaises(RuntimeError):
                with t:
                    pass
        with self.assertRaises(RuntimeError):
            t.__exit__(None, None, None)
        t.finish()
        with self.assertRaises(TypeError):
            hash(a.mutate())

    def test_errors_leave_builder_consistent(self):
        t = IntVector([1, 2]).mutate()
        with self.assertRaises(IndexError):
            t[2] = 0
        with self.assertRaises(TypeError):
            t[0] = 1.5
        with self.assertRaises(TypeError):
            del t[0]
        with self.assertRaises(OverflowError):
            t.extend([3, 4, INT64_MAX + 1])
        with self.assertRaises(TypeError):
            t.extend(iter([3, 4, "x"]))
        with self.assertRaises(TypeError):
            t.extend(array.array("d", [1.0]))
        with self.assertRaises(TypeError):
            t.append(None)
        self.assertEqual(t.pop(), 2)
        self.assertEqual(t.pop(), 1)
        with self.assertRaises(IndexError):
            t.pop()
        self.assertEqual(t.finish(), IntVector())

    def test_wrong_thread_and_reentrancy(self):
        t = FloatVector().mutate()
        errors = []
        def other():
            try:
                t.append(1)
            except RuntimeError as e:
                errors.append(e)
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            pool.submit(other).result()
        self.assertEqual(len(errors), 1)

        class Reenter:
            def __float__(self):
                t.append(0)
                return 1.0
            def __index__(self):
                t.append(0)
                return 0
        for op in (lambda: t.append(Reenter()), lambda: t.extend([Reenter()]),
                   lambda: t.__setitem__(0, Reenter())):
            with self.assertRaises(RuntimeError):
                op()
        t.append(1)
        with self.assertRaises(RuntimeError):
            t[Reenter()]
        self.assertEqual(t.finish(), FloatVector([1.0]))

    def test_random_history(self):
        rng = random.Random(7)
        base = IntVector(range(50))
        t, model = base.mutate(), list(base)
        for _ in range(3000):
            op = rng.randrange(4)
            if op == 0:
                v = rng.randrange(-1000, 1000)
                t.append(v)
                model.append(v)
            elif op == 1 and model:
                self.assertEqual(t.pop(), model.pop())
            elif op == 2 and model:
                i = rng.randrange(-len(model), len(model))
                t[i] = i
                model[i] = i
            else:
                extra = [rng.randrange(100) for _ in range(rng.randrange(5))]
                t.extend(extra)
                model.extend(extra)
            self.assertEqual(len(t), len(model))
        self.assertEqual(t.finish().tolist(), model)
        self.assertEqual(base.tolist(), list(range(50)))


class ThreadTests(unittest.TestCase):
    def test_shared_vectors(self):
        a = FloatVector(range(2000))
        def worker(n):
            for i in range(100):
                b = a.set(n, -1).append(i)
                self.assertEqual(b[n], -1.0)
                self.assertEqual(b[-1], i)
                self.assertEqual(a[n], n)
                self.assertEqual(b.pop(), a.set(n, -1))
                self.assertEqual(b[::100].tolist(), b.tolist()[::100])
                self.assertEqual(hash(a), hash(tuple(float(x) for x in range(2000))))
                if HAS_BUFFER_EXPORT:
                    self.assertEqual(sum(memoryview(a)), 1999000.0)
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            list(pool.map(worker, range(8)))

    def test_shared_iterator(self):
        a = IntVector(range(10000))
        for it in (iter(a), reversed(a)):
            def consume(_):
                return list(it)
            with concurrent.futures.ThreadPoolExecutor(8) as pool:
                parts = list(pool.map(consume, range(8)))
            self.assertCountEqual([value for part in parts for value in part], range(10000))

    def test_concurrent_builders_from_shared_base(self):
        base = IntVector(range(1000))
        def worker(n):
            with base.mutate() as t:
                for i in range(0, 1000, 7):
                    t[i] = -n
                t.extend(range(n))
            return t.finish()
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            results = list(pool.map(worker, range(8)))
        for n, v in enumerate(results):
            self.assertEqual(len(v), 1000 + n)
            self.assertEqual(v[7], -n)
        self.assertEqual(base.tolist(), list(range(1000)))

    def test_atom_updates(self):
        a = Atom(IntVector())
        def worker(n):
            for i in range(100):
                a.swap(lambda v: v.append(n * 1000 + i))
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            list(pool.map(worker, range(4)))
        self.assertCountEqual(a.value, [n * 1000 + i for n in range(4) for i in range(100)])

    def test_gil_disabled(self):
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    def test_subinterpreter(self):
        try:
            import _interpreters
        except ImportError:
            self.skipTest("requires _interpreters")
        interp = _interpreters.create()
        try:
            error = _interpreters.run_string(interp,
                "from grail import FloatVector, IntVector; v = FloatVector(range(100)); "
                "assert v.append(1).pop() == v; assert v.set(0, 9)[0] == 9.0; "
                "assert memoryview(IntVector([1, 2])).tolist() == [1, 2]")
            self.assertIsNone(error)
        finally:
            _interpreters.destroy(interp)


if __name__ == "__main__":
    unittest.main()
