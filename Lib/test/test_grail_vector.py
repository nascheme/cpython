# Copied from grail-compat tests/ by tools/sync_to_cpython_grail.py; do not edit here.
"""Persistent trie vector API, sharing, ownership, and free-threaded access."""
import collections.abc
import concurrent.futures
import copy
import gc
import operator
import pickle
import random
import sys
import sysconfig
import unittest
import weakref

from grail import Atom, PMap, PVector


class VectorTests(unittest.TestCase):
    def test_construction(self):
        for source in ([], (), range(0), iter([])):
            self.assertEqual(PVector(source), PVector())
        a = PVector(source=range(100))
        self.assertEqual(list(a), list(range(100)))
        self.assertIs(PVector(a), a)
        self.assertIsInstance(a, collections.abc.Sequence)
        self.assertIs(PVector[int].__origin__, PVector)
        source = [1, 2]
        a = PVector(source)
        source[0] = 9
        source.append(10)
        self.assertEqual(list(a), [1, 2])
        self.assertEqual(list(PVector("abc")), list("abc"))
        for source in (None, 1):
            with self.assertRaises(TypeError):
                PVector(source)

    def test_indexing(self):
        a = PVector(range(1057))
        for i in range(len(a)):
            self.assertEqual(a[i], i)
            self.assertEqual(a[i - len(a)], i)
        self.assertEqual(a[True], 1)
        class Index:
            def __index__(self):
                return -2
        self.assertEqual(a[Index()], 1055)
        self.assertEqual(operator.getitem(a, -1), 1056)
        for index in (-1058, 1057, 10**100, -(10**100)):
            with self.assertRaises(IndexError):
                a[index]
        for index in (1.0, "1", None):
            with self.assertRaises(TypeError):
                a[index]

    def test_slicing(self):
        model = list(range(1057))
        a = PVector(model)
        for start in (None, -2000, -33, -1, 0, 1, 31, 32, 33, 1056, 2000):
            for stop in (None, -2000, -1, 0, 32, 1057, 2000):
                for step in (None, 1, 2, 32, 33, -1, -2, -32, sys.maxsize, -sys.maxsize):
                    key = slice(start, stop, step)
                    result = a[key]
                    self.assertIs(type(result), PVector)
                    self.assertEqual(list(result), model[key])
        with self.assertRaises(ValueError):
            a[::0]
        with self.assertRaises(TypeError):
            a[::1.0]
        self.assertEqual(list(a[::10**100]), [0])
        self.assertEqual(list(a[::-10**100]), [1056])
        self.assertEqual(PVector()[::-1], PVector())

    def test_set(self):
        a = PVector(range(33000))
        for index in (0, 31, 32, 63, 64, 1023, 1024, 1055, 1056, 32767, 32768, -1):
            value = []
            b = a.set(index, value)
            position = index % len(a)
            self.assertIs(b[index], value)
            self.assertEqual(a[index], position)
            self.assertEqual(list(b[:position]), list(range(position)))
            self.assertEqual(list(b[position + 1:]), list(range(position + 1, len(a))))
        for index in (-33001, 33000, 10**100):
            with self.assertRaises(IndexError):
                a.set(index, 1)
        with self.assertRaises(TypeError):
            a.set(1.0, 1)
        with self.assertRaises(IndexError):
            PVector().set(0, 1)

    def test_append_pop_boundaries(self):
        a = PVector()
        history = {}
        boundaries = {0, 1, 31, 32, 33, 63, 64, 65, 1023, 1024, 1025,
                      1055, 1056, 1057, 32767, 32768, 32769, 32799, 32800, 32801, 33000}
        for i in range(33001):
            if i in boundaries:
                history[i] = a
                self.assertEqual(list(a), list(range(i)))
            if i != 33000:
                a = a.append(i)
        for i in range(33000, 0, -1):
            self.assertEqual(a[-1], i - 1)
            a = a.pop()
            if i - 1 in boundaries:
                self.assertEqual(list(a), list(range(i - 1)))
        self.assertEqual(a, PVector())
        for count, old in history.items():
            self.assertEqual(list(old), list(range(count)))
        with self.assertRaises(IndexError):
            a.pop()

    def test_build_matches_append(self):
        a = PVector()
        for i in range(33001):
            if i in (0, 1, 32, 33, 64, 65, 1056, 1057, 32800, 32801, 33000):
                built = PVector(range(i))
                self.assertEqual(a, built)
                appended = a
                for _ in range(min(i, 34)):
                    appended, built = appended.pop(), built.pop()
                    self.assertEqual(appended, built)
            a = a.append(i)

    def test_random_history(self):
        rng = random.Random(1024)
        model = list(range(1200))
        a = PVector(model)
        history = []
        for _ in range(1000):
            history.append((a, model.copy()))
            action = rng.randrange(3)
            if action == 0 or not model:
                value = rng.randrange(10000)
                a = a.append(value)
                model.append(value)
            elif action == 1:
                a = a.pop()
                model.pop()
            else:
                index = rng.randrange(-len(model), len(model))
                value = rng.randrange(10000)
                a = a.set(index, value)
                model[index] = value
            self.assertEqual(list(a), model)
        for old, expected in history:
            self.assertEqual(list(old), expected)

    def test_alternating_growth_and_collapse(self):
        for size in (32, 64, 1056, 32800):
            a = PVector(range(size))
            for _ in range(50):
                b = a.append("last")
                self.assertEqual(b[-1], "last")
                self.assertEqual(b.pop(), a)
                a = b.pop()
            self.assertEqual(list(a), list(range(size)))

    def test_large_height_transition(self):
        count = 32**4 + 32
        a = PVector(range(count))
        value = []
        b = a.append(value)
        self.assertIs(b[-1], value)
        for index in (0, 31, 32, 1024, 32768, count // 2, count - 1):
            self.assertEqual(a[index], index)
            self.assertEqual(b[index], index)
            changed = b.set(index, value)
            self.assertIs(changed[index], value)
            self.assertEqual(b[index], index)
        c = b.pop()
        self.assertEqual(len(c), count)
        self.assertEqual(c[-1], count - 1)
        self.assertEqual(c, a)
        self.assertEqual(c.append(value), b)

    def test_comparison(self):
        for a in ([], [1], [1, 2], [1, 3], [2]):
            for b in ([], [1], [1, 2], [1, 3], [2]):
                for compare in (operator.eq, operator.ne, operator.lt,
                                operator.le, operator.gt, operator.ge):
                    self.assertEqual(compare(PVector(a), PVector(b)), compare(a, b))
        self.assertNotEqual(PVector([1]), [1])
        self.assertNotEqual(PVector([1]), (1,))
        with self.assertRaises(TypeError):
            PVector([1]) < [1]
        nan = float("nan")
        a = PVector([nan])
        self.assertEqual(a, PVector([nan]))
        self.assertNotEqual(a, PVector([float("nan")]))
        self.assertIn(nan, a)
        self.assertEqual(a.count(nan), 1)
        self.assertEqual(a.index(nan), 0)

    def test_search(self):
        model = [1, 2, 1, 3] * 100
        a = PVector(model)
        self.assertIn(3, a)
        self.assertNotIn(9, a)
        self.assertEqual(a.count(1), 200)
        self.assertEqual(a.count(9), 0)
        for start in (0, 1, -10, -(10**100), 10**100):
            for stop in (3, -1, 400, 10**100):
                try:
                    expected = model.index(2, start, stop)
                except ValueError:
                    with self.assertRaises(ValueError):
                        a.index(2, start, stop)
                else:
                    self.assertEqual(a.index(2, start, stop), expected)
        with self.assertRaises(TypeError):
            a.index(1, None)

    def test_concat(self):
        a, b = PVector(range(100)), PVector(range(100, 1057))
        self.assertEqual(list(a + b), list(range(1057)))
        self.assertEqual(a + PVector(), a)
        self.assertEqual(PVector() + a, a)
        self.assertEqual(list(a + a), list(range(100)) * 2)
        with self.assertRaises(TypeError):
            a + []
        with self.assertRaises(TypeError):
            [] + a

    def test_sequence_c_api(self):
        try:
            import _testlimitedcapi as capi
        except ImportError:
            self.skipTest("requires _testlimitedcapi")
        a = PVector(range(100))
        self.assertEqual(capi.sequence_getitem(a, -1), 99)
        self.assertEqual(capi.sequence_getitem(a, 32), 32)
        with self.assertRaises(IndexError):
            capi.sequence_getitem(a, 100)
        self.assertEqual(capi.sequence_concat(a, a), a + a)
        with self.assertRaises(TypeError):
            capi.sequence_concat(a, [])
        self.assertEqual(capi.sequence_list(a), list(range(100)))
        self.assertEqual(capi.sequence_tuple(a), tuple(range(100)))

    def test_hash(self):
        for size in (0, 1, 31, 32, 33, 1057):
            a = PVector(range(size))
            self.assertEqual(hash(a), hash(tuple(range(size))))
            self.assertEqual(hash(a), hash(PVector(list(range(size)))))
        a = PVector([PVector([1, 2]), 3])
        self.assertEqual(hash(a), hash(((1, 2), 3)))
        with self.assertRaises(TypeError):
            hash(PVector([[]]))

    def test_iteration_and_match(self):
        a = PVector(range(1057))
        self.assertEqual(list(a), list(range(1057)))
        self.assertEqual(list(reversed(a)), list(range(1056, -1, -1)))
        it = iter(a)
        self.assertIs(iter(it), it)
        self.assertEqual(next(it), 0)
        del a
        self.assertEqual(list(it), list(range(1, 1057)))
        for _ in range(2):
            with self.assertRaises(StopIteration):
                next(it)
        self.assertEqual(list(reversed(PVector())), [])
        match PVector([1, 2, 3]):
            case [first, *rest]:
                self.assertEqual((first, rest), (1, [2, 3]))
            case _:
                self.fail("sequence pattern did not match")

    def test_closed_storage_and_aliases(self):
        value, replacement = [], []
        a = PVector([value])
        self.assertIs(a[0], value)
        self.assertIs(a.set(0, replacement)[0], replacement)
        self.assertIs(a.append(replacement)[-1], replacement)
        for name in ("root", "tail", "count", "_root"):
            with self.assertRaises((AttributeError, TypeError)):
                setattr(a, name, ())
        with self.assertRaises(TypeError):
            a[0] = 1
        with self.assertRaises(TypeError):
            del a[0]
        with self.assertRaises(TypeError):
            PVector.x = 1
        with self.assertRaises(TypeError):
            class Sub(PVector):
                pass
        a.__init__([9])
        self.assertIs(a[0], value)

    def test_copy_and_pickle(self):
        a = PVector(range(1057))
        self.assertIs(copy.copy(a), a)
        self.assertEqual(copy.deepcopy(a), a)
        for dumps, loads in ((pickle.dumps, pickle.loads), (pickle._dumps, pickle._loads)):
            for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
                self.assertEqual(loads(dumps(a, protocol)), a)

    def test_deepcopy_immutable_contents_is_identity(self):
        row = (1, 2.5, "x", None, (b"y", True))
        a = PVector([row] * 40 + [1, "z"])
        self.assertIs(copy.deepcopy(a), a)
        b = PVector([row, [1]])
        c = copy.deepcopy(b)
        self.assertIsNot(c, b)
        self.assertIs(c[0], row)
        self.assertIsNot(c[1], b[1])
        self.assertEqual(c, b)
        mixed = (1, [2])
        b = PVector([row] * 3 + [mixed] * 2 + [(row, row, mixed), row])
        c = copy.deepcopy(b)
        self.assertEqual(c, b)
        self.assertIs(c[2], row)
        self.assertIsNot(c[3], mixed)
        self.assertIs(c[3], c[4])
        self.assertIs(c[5][2], c[3])
        self.assertIs(c[6], row)

    def test_copy_and_pickle_cycles(self):
        leaf = []
        a = PVector([leaf, leaf])
        leaf.append(a)
        b = copy.deepcopy(a)
        self.assertIsNot(a, b)
        self.assertIsNot(b[0], leaf)
        self.assertIs(b[0], b[1])
        self.assertIs(b[0][0], b)
        cell = Atom(None)
        a = PVector([cell])
        cell.reset(a)
        b = copy.deepcopy(a)
        self.assertIs(b[0].value, b)
        for dumps, loads in ((pickle.dumps, pickle.loads), (pickle._dumps, pickle._loads)):
            for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
                b = loads(dumps(a, protocol))
                self.assertIs(b[0].value, b)
                leaf = []
                cyclic = PVector([leaf, leaf])
                leaf.append(cyclic)
                b = loads(dumps(cyclic, protocol))
                self.assertIs(b[0], b[1])
                self.assertIs(b[0][0], b)

    def test_gc_shared_node_cycles(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        a = PVector([leaf] + list(range(2000)))
        b = a.append(2000)
        leaf.parent = a
        del a, leaf
        gc.collect()
        self.assertIsNotNone(ref())
        del b
        gc.collect()
        self.assertIsNone(ref())

    def test_iterator_cycles(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        a = PVector([leaf] + list(range(100)))
        leaf.parent = iter(a)
        next(leaf.parent)
        del a, leaf
        gc.collect()
        self.assertIsNone(ref())

    def test_pop_releases_removed_element(self):
        class Leaf:
            pass
        for count in (1, 32, 33, 64, 65, 1056, 1057):
            leaf = Leaf()
            ref = weakref.ref(leaf)
            a = PVector(range(count - 1)).append(leaf)
            b = a.pop()
            del a, leaf
            gc.collect()
            self.assertIsNone(ref())
            self.assertEqual(list(b), list(range(count - 1)))

    def test_errors_and_callbacks(self):
        class Error:
            def __hash__(self):
                raise ValueError("hash failed")
            def __eq__(self, other):
                raise ValueError("equality failed")
            def __repr__(self):
                raise ValueError("repr failed")
        a = PVector([Error()])
        for operation, message in ((lambda: hash(a), "hash"),
                                   (lambda: a == PVector([0]), "equality"),
                                   (lambda: 0 in a, "equality"),
                                   (lambda: a.count(0), "equality"),
                                   (lambda: a.index(0), "equality"),
                                   (lambda: repr(a), "repr")):
            with self.assertRaisesRegex(ValueError, message):
                operation()
        self.assertEqual(len(a), 1)
        def failing_iterable():
            yield 1
            raise ValueError("iteration failed")
        with self.assertRaisesRegex(ValueError, "iteration failed"):
            PVector(failing_iterable())

    def test_recursive_repr_and_comparison(self):
        leaf = []
        a = PVector([leaf])
        leaf.append(a)
        self.assertIn("...", repr(a))
        class Link:
            def __hash__(self):
                return hash(self.parent)
            def __eq__(self, other):
                return self.parent == other.parent
        x, y = Link(), Link()
        a, b = PVector([x]), PVector([y])
        x.parent, y.parent = a, b
        with self.assertRaises(RecursionError):
            hash(a)
        with self.assertRaises(RecursionError):
            a == b

    def test_deepcopy_error(self):
        class BadCopy:
            def __deepcopy__(self, memo):
                raise ValueError("copy failed")
        a = PVector([BadCopy()])
        with self.assertRaisesRegex(ValueError, "copy failed"):
            copy.deepcopy(a)


class TransientTests(unittest.TestCase):
    def test_lifecycle(self):
        original = PVector([1, 2])
        with original.mutate() as t:
            t.append(3)
            t[0] = 10
            self.assertEqual(t[0], 10)
            self.assertEqual(t[-1], 3)
            self.assertEqual(len(t), 3)
            self.assertEqual(t.pop(), 3)
        with self.assertRaises(RuntimeError):
            t.append(4)
        result = t.finish()
        self.assertEqual(result, PVector([10, 2]))
        self.assertEqual(original, PVector([1, 2]))
        for operation in (t.finish, lambda: len(t), lambda: t[0], t.pop):
            with self.assertRaises(RuntimeError):
                operation()
        with self.assertRaisesRegex(ValueError, "abort"):
            with original.mutate() as t:
                t.append(5)
                raise ValueError("abort")
        with self.assertRaises(RuntimeError):
            t.finish()
        t = PVector().mutate()
        with self.assertRaises(IndexError):
            t.pop()
        with self.assertRaises(IndexError):
            t[0]
        with self.assertRaises(IndexError):
            t[0] = 1
        with self.assertRaises(TypeError):
            t["x"]
        with self.assertRaises(TypeError):
            del t[0]
        with self.assertRaises(TypeError):
            hash(t)
        self.assertEqual(t.finish(), PVector())

    def test_wrong_thread_and_reentrancy(self):
        t = PVector(range(10)).mutate()
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            for operation in (lambda: t.append(1), lambda: t[0], t.finish):
                with self.assertRaisesRegex(RuntimeError, "another thread"):
                    pool.submit(operation).result()
        def reentrant():
            yield 1
            t.append(2)
        with self.assertRaisesRegex(RuntimeError, "reentrant"):
            t.extend(reentrant())
        class Index:
            def __index__(self):
                t.append(3)
                return 0
        with self.assertRaisesRegex(RuntimeError, "reentrant"):
            t[Index()]
        # The guard is released after errors; the consistent prefix remains.
        t.append(4)
        self.assertEqual(list(t.finish()), list(range(10)) + [1, 4])

    def test_random_history(self):
        # In-place edits must never change a published version.
        rng = random.Random(21)
        v, model, history = PVector(), [], []
        for _ in range(80):
            history.append((v, list(model)))
            with v.mutate() as t:
                for _ in range(rng.randrange(1, 200)):
                    op = rng.randrange(6)
                    if op == 0 and model:
                        i = rng.randrange(len(model))
                        t[i] = model[i] = rng.randrange(1000)
                    elif op == 1 and model:
                        self.assertEqual(t.pop(), model.pop())
                    elif op == 2:
                        items = range(rng.randrange(80))
                        t.extend(items)
                        model.extend(items)
                    else:
                        x = rng.randrange(1000)
                        t.append(x)
                        model.append(x)
                    self.assertEqual(len(t), len(model))
                    if model:
                        i = rng.randrange(len(model))
                        self.assertEqual(t[i], model[i])
            v = t.finish()
            self.assertEqual(list(v), model)
        for old, expected in history:
            self.assertEqual(list(old), expected)
            self.assertEqual(old, PVector(expected))

    def test_height_boundaries(self):
        for size in (0, 1, 31, 32, 33, 64, 1023, 1024, 1025, 1056, 1057, 32768 + 32, 32768 + 33):
            base = PVector(range(size))
            t = base.mutate()
            t.extend(range(size, size + 40))
            for i in range(0, size + 40, 7):
                t[i] = -i
            grown = t.finish()
            expected = [-i if i % 7 == 0 else i for i in range(size + 40)]
            self.assertEqual(list(grown), expected)
            self.assertEqual(list(base), list(range(size)))
            # Pop everything back across leaf and root boundaries.
            t = grown.mutate()
            for i in range(size + 39, -1, -1):
                if i % 997 == 0 or i < 70:
                    self.assertEqual(len(t), i + 1)
                    self.assertEqual(t[i], expected[i])
                self.assertEqual(t.pop(), expected[i])
            self.assertEqual(t.finish(), PVector())
            self.assertEqual(list(grown), expected)

    def test_build_from_empty_matches_constructor(self):
        for size in (0, 31, 32, 33, 1024, 1025, 40000):
            with PVector().mutate() as t:
                for i in range(size):
                    t.append(i)
            built = t.finish()
            self.assertEqual(built, PVector(range(size)))
            self.assertEqual(hash(built), hash(tuple(range(size))))
            self.assertEqual(built.append(-1).pop(), built)

    def test_nodes_seen_by_gc_are_not_edited(self):
        base = PVector(range(2000)).mutate()
        base.append(0)
        v = base.finish()
        nodes = [n for n in gc.get_referents(v) if isinstance(n, tuple)]
        snapshot = [tuple(n) for n in nodes]
        t = v.mutate()
        for i in range(2000):
            t[i] = None
        t.finish()
        self.assertEqual([tuple(n) for n in nodes], snapshot)
        self.assertEqual(list(v), list(range(2000)) + [0])

    def test_concat_shares_left(self):
        left = PVector(range(5000))
        right = PVector(range(70))
        joined = left + right
        self.assertEqual(list(joined), list(range(5000)) + list(range(70)))
        self.assertIs(left + PVector(), left)
        self.assertIs(PVector() + right, right)
        self.assertEqual(list(left), list(range(5000)))

    def test_hash_cached_and_compatible(self):
        v = PVector([1, "a", (2, 3)])
        self.assertEqual(hash(v), hash((1, "a", (2, 3))))
        self.assertEqual(hash(v), hash(v))
        bad = PVector([1, []])
        for _ in range(2):
            with self.assertRaises(TypeError):
                hash(bad)

    def test_gc_cycle_through_transient(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        t = PVector(range(100)).mutate()
        t.append(leaf)
        leaf.t = t
        del leaf, t
        gc.collect()
        self.assertIsNone(ref())

    def test_concurrent_builders_from_shared_base(self):
        base = PVector(range(5000))
        def worker(n):
            out = []
            for r in range(10):
                with base.mutate() as t:
                    for i in range(0, 5000, 13):
                        t[i] = (n, r)
                    t.extend(range(100))
                    for _ in range(50):
                        t.pop()
                out.append(t.finish())
            return out
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            results = list(pool.map(worker, range(4)))
        self.assertEqual(list(base), list(range(5000)))
        for n, vectors in enumerate(results):
            for r, v in enumerate(vectors):
                self.assertEqual(len(v), 5050)
                self.assertEqual(v[13], (n, r))
                self.assertEqual(v[14], 14)
                self.assertEqual(v[-1], 49)


class ThreadTests(unittest.TestCase):
    def test_shared_roots(self):
        a = PVector(range(2000))
        def worker(n):
            for i in range(100):
                b = a.set(n, -1).append(i)
                self.assertEqual(b[n], -1)
                self.assertEqual(b[-1], i)
                self.assertEqual(a[n], n)
                self.assertEqual(b.pop(), a.set(n, -1))
                self.assertEqual(list(b[::100]), list(b)[::100])
                self.assertEqual(hash(a), hash(tuple(range(2000))))
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            list(pool.map(worker, range(8)))

    def test_shared_iterator(self):
        a = PVector(range(10000))
        for it in (iter(a), reversed(a)):
            def consume(_):
                return list(it)
            with concurrent.futures.ThreadPoolExecutor(8) as pool:
                parts = list(pool.map(consume, range(8)))
            self.assertCountEqual([value for part in parts for value in part], range(10000))

    def test_atom_updates(self):
        a = Atom(PVector())
        def worker(n):
            for i in range(100):
                a.swap(lambda v: v.append((n, i)))
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            list(pool.map(worker, range(4)))
        self.assertCountEqual(a.value, [(n, i) for n in range(4) for i in range(100)])
        m = PMap({"vector": a.value})
        self.assertEqual(len(m["vector"]), 400)

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
                "from grail import PVector; v = PVector(range(1057)); "
                "assert v.append(1057).pop() == v; "
                "assert v.set(0, 99)[0] == 99; assert list(v[:3]) == [0, 1, 2]")
            self.assertIsNone(error)
        finally:
            _interpreters.destroy(interp)


if __name__ == "__main__":
    unittest.main()
