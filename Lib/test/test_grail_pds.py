# Copied from grail-compat tests/ by tools/sync_to_proto.py; do not edit here.
"""Grail persistent maps and sets: PMap and PSet.

Ported from cpython-proto/Lib/test/test_grail_pds.py.  Backend-agnostic: runs
against the pure-Python shim and the native prototype (no pytest needed).
"""
import builtins
import collections
import collections.abc
import concurrent.futures
import copy
import gc
import operator
import pickle
import random
import sys
import unittest
import weakref

import grail
from grail import PMap, PSet, PVector

BACKEND = getattr(grail, "backend", "shim")
TIMEOUT = 120

_frozendict = getattr(builtins, "frozendict", None)


class _FrozenDictStandIn(collections.abc.Mapping):
    """frozendict (3.15+) stand-in: a mapping hashed as frozendict is."""
    def __init__(self, *args):
        self._d = dict(*args)
    def __getitem__(self, key):
        return self._d[key]
    def __iter__(self):
        return iter(self._d)
    def __len__(self):
        return len(self._d)
    def __hash__(self):
        return hash(frozenset(self._d.items()))


def frozendict(*args):
    if _frozendict is not None:
        return _frozendict(*args)
    return _FrozenDictStandIn(*args)


def run_threads(fn, args, workers):
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        futures = [pool.submit(fn, a) for a in args]
        return [f.result(timeout=TIMEOUT) for f in futures]


class Collision:
    def __init__(self, value):
        self.value = value

    def __hash__(self):
        return 42

    def __eq__(self, other):
        return isinstance(other, Collision) and self.value == other.value


class MapTests(unittest.TestCase):
    def test_construction(self):
        self.assertEqual(PMap(), {})
        self.assertEqual(PMap(source={"a": 1}), {"a": 1})
        self.assertEqual(PMap([(1, 2), (1, 3)]), {1: 3})
        self.assertIsInstance(PMap(), collections.abc.Mapping)
        self.assertIs(PMap[str, int].__origin__, PMap)
        for bad in ([1], [(1,)], [(1, 2, 3)]):
            with self.assertRaises((TypeError, ValueError)):
                PMap(bad)
        with self.assertRaises(TypeError):
            PMap({[]: 1})

    def test_updates(self):
        a = PMap({"a": 1})
        b = a.set("b", 2)
        c = b.set("a", 3)
        self.assertEqual(a, {"a": 1})
        self.assertEqual(b, {"a": 1, "b": 2})
        self.assertEqual(c, {"a": 3, "b": 2})
        self.assertEqual(c.delete("a"), {"b": 2})
        with self.assertRaises(KeyError):
            a.delete("missing")
        with self.assertRaises(KeyError):
            a["missing"]
        self.assertEqual(a.get("a"), 1)
        self.assertIsNone(a.get("missing"))
        self.assertEqual(a.get("missing", 4), 4)
        self.assertEqual(a | {"a": 9, "b": 2}, {"a": 9, "b": 2})
        self.assertEqual(a | PMap({"b": 2}), {"a": 1, "b": 2})
        self.assertEqual({"a": 9, "b": 2} | a, {"a": 1, "b": 2})
        with self.assertRaises(TypeError):
            a | []
        self.assertNotEqual(a, ["a"])
        self.assertEqual({"a": 1}, a)

    def test_views_and_match(self):
        m = PMap({"a": 1, "b": 2})
        self.assertEqual(m.keys(), {"a", "b"})
        self.assertEqual(m.items(), {("a", 1), ("b", 2)})
        self.assertCountEqual(m.values(), [1, 2])
        self.assertEqual(dict(m), {"a": 1, "b": 2})
        match m:
            case {"a": a, **rest}:
                self.assertEqual((a, rest), (1, {"b": 2}))
            case _:
                self.fail("mapping pattern did not match")

    def test_collisions_and_old_versions(self):
        m = PMap()
        history = []
        for i in range(100):
            history.append(m)
            m = m.set(Collision(i), i)
        for i, old in enumerate(history):
            self.assertEqual(len(old), i)
            for j in range(i):
                self.assertEqual(old[Collision(j)], j)
        for i in range(100):
            m = m.delete(Collision(i))
        self.assertEqual(m, {})

    def test_random_history(self):
        rng = random.Random(73)
        m, model, history = PMap(), {}, []
        for _ in range(1000):
            history.append((m, model.copy()))
            key = rng.randrange(120)
            if key in model and rng.randrange(3) == 0:
                m = m.delete(key)
                del model[key]
            else:
                value = rng.randrange(1000)
                m = m.set(key, value)
                model[key] = value
            self.assertEqual(m, model)
        for old, expected in history:
            self.assertEqual(old, expected)

    def test_identity_and_nan(self):
        value = []
        m = PMap().set("x", value)
        self.assertIs(m["x"], value)
        replacement = []
        self.assertIs(m.set("x", replacement)["x"], replacement)
        nan = float("nan")
        self.assertEqual(PMap({nan: nan}), PMap({nan: nan}))
        self.assertIs(PMap({nan: nan})[nan], nan)
        self.assertEqual(len(PMap([(True, 1), (1, 2)])), 1)

    def test_hash_and_pickle(self):
        a = PMap({1: "a", 2: "b"})
        b = PMap([(2, "b"), (1, "a")])
        self.assertEqual(hash(a), hash(b))
        self.assertEqual(hash(a), hash(frozendict(a)))
        self.assertEqual(a, frozendict(a))
        self.assertEqual(hash(PMap()), hash(frozendict()))
        with self.assertRaises(TypeError):
            hash(PMap({1: []}))
        self.assertIs(copy.copy(a), a)
        self.assertEqual(copy.deepcopy(a), a)
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            self.assertEqual(pickle.loads(pickle.dumps(a, protocol)), a)

    def test_deepcopy_immutable_contents_is_identity(self):
        row = (1,) * 100
        inner = PVector([row] * 10)
        m = PMap({"a": (row, row), "b": inner, (1, "k"): 2.0})
        self.assertIs(copy.deepcopy(m), m)
        s = PSet([row, "x", 3])
        self.assertIs(copy.deepcopy(s), s)
        leaf = [row]
        m = PMap({"a": row, "b": leaf, "c": inner})
        cloned = copy.deepcopy(m)
        self.assertIsNot(cloned, m)
        self.assertIs(cloned["a"], row)
        self.assertIs(cloned["c"], inner)
        self.assertIsNot(cloned["b"], leaf)
        self.assertEqual(cloned, m)

    def test_deepcopy_and_pickle_cycle(self):
        leaf = []
        m = PMap({"a": leaf, "b": leaf})
        leaf.append(m)
        cloned = copy.deepcopy(m)
        self.assertIsNot(cloned, m)
        self.assertIsNot(cloned["a"], leaf)
        self.assertIs(cloned["a"], cloned["b"])
        self.assertIs(cloned["a"][0], cloned)
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            cloned = pickle.loads(pickle.dumps(m, protocol))
            self.assertIs(cloned["a"], cloned["b"])
            self.assertIs(cloned["a"][0], cloned)

    def test_recursion_guards(self):
        class Link:
            def __hash__(self):
                return hash(self.parent)
            def __eq__(self, other):
                return self.parent == other.parent
        x, y = Link(), Link()
        a, b = PMap({"x": x}), PMap({"x": y})
        x.parent, y.parent = a, b
        old_limit = sys.getrecursionlimit()
        try:
            sys.setrecursionlimit(100)
            with self.assertRaises(RecursionError):
                hash(a)
            with self.assertRaises(RecursionError):
                a == b
        finally:
            sys.setrecursionlimit(old_limit)

    def test_closed_storage(self):
        m = PMap({1: 2})
        with self.assertRaises(TypeError):
            m[1] = 3
        with self.assertRaises((AttributeError, TypeError)):
            m.root = {}
        with self.assertRaises(TypeError):
            PMap.x = 1
        with self.assertRaises(TypeError):
            class Sub(PMap):
                pass
        m.__init__({3: 4})
        self.assertEqual(m, {1: 2})

    def test_exceptions(self):
        class BadHash:
            def __hash__(self):
                raise ValueError("hash")
        class BadEq:
            def __hash__(self):
                return 42
            def __eq__(self, other):
                raise ValueError("eq")
        with self.assertRaisesRegex(ValueError, "hash"):
            PMap().set(BadHash(), 1)
        m = PMap({BadEq(): 1})
        with self.assertRaisesRegex(ValueError, "eq"):
            m.set(BadEq(), 2)
        self.assertEqual(len(m), 1)

    def test_gc_cycle(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        m = PMap({"leaf": leaf})
        leaf.parent = m
        self.assertTrue(gc.is_tracked(m))
        del leaf, m
        gc.collect()
        self.assertIsNone(ref())

    def test_repr_cycle(self):
        leaf = []
        m = PMap({"leaf": leaf})
        leaf.append(m)
        self.assertIn("...", repr(m))


class SetTests(unittest.TestCase):
    def test_basics(self):
        a = PSet([1, 2, 2])
        self.assertIsInstance(a, collections.abc.Set)
        self.assertEqual(a, {1, 2})
        self.assertEqual({1, 2}, a)
        self.assertEqual(a.add(3), {1, 2, 3})
        self.assertEqual(a.discard(1), {2})
        self.assertEqual(a.discard(9), a)
        self.assertEqual(a, {1, 2})
        self.assertEqual(hash(a), hash(frozenset(a)))
        self.assertEqual(a | PSet([3]), {1, 2, 3})
        self.assertEqual(a & {2, 3}, {2})
        self.assertEqual(a - {2, 3}, {1})
        self.assertEqual(a ^ {2, 3}, {1, 3})
        self.assertEqual({2, 3} | a, {1, 2, 3})
        self.assertEqual({2, 3} & a, {2})
        self.assertEqual({2, 3} - a, {3})
        self.assertEqual({2, 3} ^ a, {1, 3})
        self.assertTrue(a < PSet([1, 2, 3]))
        self.assertTrue(a <= {1, 2})
        self.assertIs(PSet[int].__origin__, PSet)
        self.assertEqual(pickle.loads(pickle.dumps(a)), a)
        self.assertIs(copy.copy(a), a)

    def test_gc_cycle(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        leaf.parent = PSet([leaf])
        del leaf
        gc.collect()
        self.assertIsNone(ref())


class FoldedHash:
    """Distinct 64-bit hashes that the HAMT folds to the same 32 bits."""
    def __init__(self, n, high):
        self.n = n
        self.high = high

    def __hash__(self):
        return (self.high << 32) | (self.high ^ 7)

    def __eq__(self, other):
        return isinstance(other, FoldedHash) and (self.n, self.high) == (other.n, other.high)


def mixed_keys(rng, count):
    # Plain ints spread over array nodes, full collisions, and folded hashes.
    kind = rng.randrange(4)
    if kind == 0:
        return Collision(rng.randrange(count // 10 + 1))
    if kind == 1:
        return FoldedHash(rng.randrange(3), rng.randrange(1, 4))
    return rng.randrange(count)


class BuilderTests(unittest.TestCase):
    def test_transient_random_history(self):
        # In-place builder edits must never change a published version.
        rng = random.Random(11)
        m, model, history = PMap(), {}, []
        for _ in range(60):
            history.append((m, model.copy()))
            with m.mutate() as t:
                for _ in range(rng.randrange(1, 80)):
                    key = mixed_keys(rng, 600)
                    if key in model and rng.randrange(3) == 0:
                        del t[key]
                        del model[key]
                    else:
                        value = rng.randrange(1000)
                        t[key] = value
                        model[key] = value
                    self.assertEqual(len(t), len(model))
                    self.assertEqual(t[key] if key in model else None, model.get(key))
            m = t.finish()
            self.assertEqual(m, model)
        for old, expected in history:
            self.assertEqual(old, expected)
            self.assertEqual(dict(old.items()), expected)

    def test_set_transient_random_history(self):
        rng = random.Random(12)
        s, model, history = PSet(), set(), []
        for _ in range(60):
            history.append((s, frozenset(model)))
            t = s.mutate()
            for _ in range(rng.randrange(1, 80)):
                key = mixed_keys(rng, 600)
                if rng.randrange(3) == 0:
                    t.discard(key)
                    model.discard(key)
                else:
                    t.add(key)
                    model.add(key)
            s = t.finish()
            self.assertEqual(s, model)
        for old, expected in history:
            self.assertEqual(old, expected)
            self.assertEqual(hash(old), hash(expected))

    def test_finished_result_is_not_edited_later(self):
        a = PMap(dict.fromkeys(range(100)))
        t = a.mutate()
        for i in range(100):
            t[i] = i
        b = t.finish()
        t2 = b.mutate()
        for i in range(100):
            t2[i] = -i
            t2[i + 1000] = i
        c = t2.finish()
        self.assertEqual(b, {i: i for i in range(100)})
        self.assertEqual(c, {i: -i for i in range(100)} | {i + 1000: i for i in range(100)})
        self.assertEqual(a, dict.fromkeys(range(100)))

    def test_concurrent_builders_from_shared_base(self):
        base = PMap({i: i for i in range(2000)})
        def worker(n):
            results = []
            for r in range(20):
                with base.mutate() as t:
                    for i in range(0, 2000, 7):
                        t[i] = (n, r)
                    for i in range(3000, 3100):
                        t[i] = n
                results.append(t.finish())
            return results
        all_results = run_threads(worker, range(4), 4)
        self.assertEqual(base, {i: i for i in range(2000)})
        for n, results in enumerate(all_results):
            for r, m in enumerate(results):
                self.assertEqual(len(m), 2100)
                self.assertEqual(m[7], (n, r))
                self.assertEqual(m[8], 8)
                self.assertEqual(m[3050], n)

    def test_construction_sources(self):
        d = {i: str(i) for i in range(300)}
        self.assertEqual(PMap(d), d)
        self.assertEqual(PMap(frozendict(d)), d)
        self.assertEqual(PMap(d.items()), d)
        self.assertEqual(PSet(d), set(d))
        self.assertEqual(PSet(PMap(d)), set(d))
        self.assertEqual(PSet(frozendict(d)), set(d))
        self.assertEqual(PMap(d) | PMap({1: "x", 500: "y"}), d | {1: "x", 500: "y"})
        self.assertEqual(PMap({1: "x"}) | PMap(d), {1: "x"} | d)
        self.assertEqual(PMap(d) | {1: "x"}, d | {1: "x"})
        # A source mutated during construction does not crash.
        source = {}
        class Evil:
            def __init__(self, n):
                self.n = n
            def __hash__(self):
                return 0
            def __eq__(self, other):
                source.clear()
                return self is other
        source.update({Evil(i): i for i in range(5)})
        self.assertLessEqual(len(PMap(source)), 5)

    def test_hash_matches_builtins(self):
        rng = random.Random(13)
        for _ in range(50):
            items = {rng.randrange(1000): rng.randrange(5) for _ in range(rng.randrange(60))}
            m = PMap(items)
            self.assertEqual(hash(m), hash(frozendict(items)))
            self.assertEqual(hash(m), hash(m))
            s = PSet(items)
            self.assertEqual(hash(s), hash(frozenset(items)))
        self.assertEqual(hash(PSet()), hash(frozenset()))
        m = PMap({1: []})
        for _ in range(2):
            with self.assertRaises(TypeError):
                hash(m)
        self.assertEqual(hash(PMap({"a": (1, 2)})), hash(frozendict({"a": (1, 2)})))

    def test_set_algebra_and_comparisons(self):
        rng = random.Random(14)
        for _ in range(200):
            a = {mixed_keys(rng, 100) for _ in range(rng.randrange(40))}
            b = {mixed_keys(rng, 100) for _ in range(rng.randrange(40))}
            pa, pb = PSet(a), PSet(b)
            self.assertEqual(pa & pb, a & b)
            self.assertEqual(pa - pb, a - b)
            self.assertEqual(pb - pa, b - a)
            self.assertEqual(pa ^ pb, a ^ b)
            self.assertEqual(pa | pb, a | b)
            for op in ("__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__"):
                self.assertEqual(getattr(pa, op)(pb), getattr(a, op)(b), op)
            self.assertEqual(pa, a)
            self.assertEqual(pb, b)
        same = PSet(range(10))
        self.assertIs(same & same, same)
        self.assertIs(same - PSet(), same)

    def test_bulk_built_tries_support_later_edits(self):
        rng = random.Random(16)
        for size in (1, 2, 16, 17, 33, 500, 3000):
            pairs = [(mixed_keys(rng, size * 3), rng.randrange(100)) for _ in range(size)]
            model = dict(pairs)
            m = PMap(pairs)
            self.assertEqual(m, model)
            self.assertEqual(len(m), len(model))
            self.assertEqual(hash(m), hash(frozendict(model)))
            for _ in range(300):
                key = mixed_keys(rng, size * 3)
                if key in model and rng.randrange(2):
                    m = m.delete(key)
                    del model[key]
                else:
                    m = m.set(key, -1)
                    model[key] = -1
                self.assertEqual(len(m), len(model))
            self.assertEqual(m, model)
            s = PSet(k for k, _ in pairs)
            self.assertEqual(s, {k for k, _ in pairs})
            with s.mutate() as t:
                for k, _ in pairs[::2]:
                    t.discard(k)
            self.assertEqual(t.finish(), {k for k, _ in pairs} - {k for k, _ in pairs[::2]})

    def test_bulk_build_duplicates_and_errors(self):
        # Like dict: the first key object and the last value are kept.
        a, b = Collision(1), Collision(1)
        m = PMap([(a, 1), (Collision(2), 2), (b, 3)])
        self.assertEqual(len(m), 2)
        self.assertIs(next(k for k in m if k == a), a)
        self.assertEqual(m[b], 3)
        self.assertEqual(PMap([(1, "a"), (1.0, "b"), (True, "c")]), {1: "c"})
        self.assertIs(type(next(iter(PMap([(1, "a"), (1.0, "b")])))), int)
        self.assertEqual(PSet([1, 1.0, 2]), {1, 2})
        with self.assertRaises(TypeError):
            PMap([(1, 2), ([], 3)])
        class BadEq(Collision):
            __hash__ = Collision.__hash__
            def __eq__(self, other):
                raise ZeroDivisionError
        with self.assertRaises(ZeroDivisionError):
            PMap([(BadEq(1), 1), (BadEq(2), 2)])
        with self.assertRaises(ValueError):
            PMap([(1, 2), (3,)])
        self.assertEqual(PMap(iter([])), {})

    def test_update_with(self):
        a = PMap({"x": 1, "y": 2})
        b = PMap({"y": 10, "z": 5})
        self.assertEqual(a.update_with(b, operator.add), {"x": 1, "y": 12, "z": 5})
        self.assertEqual(a.update_with({"x": 3}, max), {"x": 3, "y": 2})
        self.assertEqual(a.update_with([("q", 1)], operator.add), {"x": 1, "y": 2, "q": 1})
        self.assertIs(PMap().update_with(b, operator.add), b)
        self.assertEqual(a, {"x": 1, "y": 2})
        calls = []
        def record(old, new):
            calls.append((old, new))
            return old + new
        a.update_with({"y": 3, "w": 4}, record)
        self.assertEqual(calls, [(2, 3)])
        with self.assertRaises(ZeroDivisionError):
            a.update_with(b, lambda old, new: 1 / 0)
        with self.assertRaises(TypeError):
            a.update_with(b, None)
        with self.assertRaises(TypeError):
            a.update_with([1], operator.add)
        # The callback may use the maps involved.
        nested = a.update_with(b, lambda old, new: a.update_with(b, operator.add)["y"] + old)
        self.assertEqual(nested["y"], 14)
        self.assertEqual(a, {"x": 1, "y": 2})
        # A fold-and-merge: per-chunk counts combined into one map.
        rng = random.Random(15)
        words = [rng.randrange(50) for _ in range(2000)]
        parts = []
        for start in range(0, 2000, 300):
            with PMap().mutate() as t:
                for w in words[start:start + 300]:
                    t[w] = (t[w] + 1) if w in t else 1
            parts.append(t.finish())
        total = PMap()
        for part in parts:
            total = total.update_with(part, operator.add)
        self.assertEqual(total, collections.Counter(words))

    def test_views_iterate_natively(self):
        d = {i: str(i) for i in range(100)}
        m = PMap(d)
        self.assertCountEqual(list(m.keys()), d)
        self.assertCountEqual(list(m.values()), d.values())
        self.assertCountEqual(list(m.items()), d.items())
        self.assertIn((3, "3"), m.items())
        self.assertIn("3", m.values())
        self.assertEqual(m.keys() & {1, 2, 500}, {1, 2})
        self.assertEqual(len(m.items()), 100)
        # A view constructed over another mapping falls back to the mixins.
        view = type(m.items())({"a": 1})
        self.assertEqual(list(view), [("a", 1)])
        it = iter(m.items())
        self.assertIs(iter(it), it)
        self.assertEqual(len(list(it)), 100)
        self.assertEqual(list(it), [])


class TransientTests(unittest.TestCase):
    def test_map_lifecycle(self):
        original = PMap({1: 2})
        with original.mutate() as t:
            t[1] = 3
            t[2] = 4
            self.assertEqual(t[1], 3)
            self.assertEqual(len(t), 2)
            self.assertIn(1, t)
            del t[1]
        with self.assertRaises(RuntimeError):
            t[3] = 4
        result = t.finish()
        self.assertEqual(result, {2: 4})
        self.assertEqual(original, {1: 2})
        for operation in (t.finish, lambda: len(t), lambda: t[2]):
            with self.assertRaises(RuntimeError):
                operation()
        with original.mutate() as t:
            result = t.finish()
        self.assertEqual(result, original)

    def test_set_lifecycle(self):
        a = PSet([1])
        t = a.mutate()
        t.add(2)
        t.discard(1)
        t.discard(100)
        self.assertIn(2, t)
        self.assertEqual(t.finish(), {2})
        self.assertEqual(a, {1})
        with self.assertRaises(RuntimeError):
            t.add(3)

    def test_abort_and_nested_context(self):
        t = PMap().mutate()
        with self.assertRaisesRegex(ValueError, "abort"):
            with t:
                t[1] = 2
                raise ValueError("abort")
        with self.assertRaises(RuntimeError):
            t.finish()
        t = PMap().mutate()
        with t:
            with self.assertRaises(RuntimeError):
                with t:
                    pass
        self.assertEqual(t.finish(), {})

    def test_wrong_thread(self):
        t = PMap({1: 2}).mutate()
        def check():
            for operation in (t.finish, lambda: len(t), lambda: t[1], t.__enter__):
                with self.assertRaises(RuntimeError):
                    operation()
        # Futures propagate assertions in worker threads to the test thread.
        run_threads(lambda _: check(), [None], 1)
        self.assertEqual(t.finish(), {1: 2})

    def test_reentrancy_and_exception_cleanup(self):
        t = PMap().mutate()
        class Reenter:
            def __hash__(self):
                t.finish()
                return 1
        with self.assertRaisesRegex(RuntimeError, "reentrant"):
            t[Reenter()] = 1
        t[1] = 2
        self.assertEqual(t.finish(), {1: 2})

    def test_transient_cycle(self):
        class Leaf:
            pass
        leaf = Leaf()
        ref = weakref.ref(leaf)
        t = PMap().mutate()
        t[1] = leaf
        leaf.parent = t
        del t, leaf
        gc.collect()
        self.assertIsNone(ref())


class ThreadTests(unittest.TestCase):
    def test_shared_roots(self):
        m = PMap((i, i) for i in range(1000))
        def worker(n):
            for _ in range(20):
                t = m.mutate()
                t[n] = -1
                result = t.finish()
                self.assertEqual(result[n], -1)
                self.assertEqual(m[n], n)
                self.assertEqual(hash(m), hash(frozendict(m)))
                self.assertEqual(len(list(m)), 1000)
        run_threads(worker, range(8), 8)

    def test_shared_iterator(self):
        m = PMap((i, i) for i in range(10000))
        it = iter(m)
        def consume(_):
            return list(it)
        parts = run_threads(consume, range(8), 8)
        self.assertCountEqual([key for part in parts for key in part], range(10000))

    def test_gil_disabled(self):
        if sysconfig_free_threaded():
            self.assertFalse(sys._is_gil_enabled())

    def test_subinterpreter(self):
        try:
            import _interpreters
        except ImportError:
            self.skipTest("no _interpreters module")
        interp = _interpreters.create()
        try:
            error = _interpreters.run_string(interp,
                "from grail import PMap, PSet; assert PMap({1: 2})[1] == 2; "
                "assert PSet([1]).add(2) == {1, 2}")
            self.assertIsNone(error)
        finally:
            _interpreters.destroy(interp)


class ApiTests(unittest.TestCase):
    """Observable API details, checked against the native prototype."""

    def test_repr(self):
        self.assertEqual(repr(PMap()), "PMap({})")
        self.assertEqual(repr(PMap({1: 2})), "PMap({1: 2})")
        self.assertEqual(repr(PSet()), "PSet([])")
        self.assertEqual(repr(PSet([1])), "PSet([1])")
        m = PMap({1: 2})
        self.assertEqual(repr(m.keys()), "PMapKeysView(PMap({1: 2}))")
        self.assertEqual(repr(m.values()), "PMapValuesView(PMap({1: 2}))")
        self.assertEqual(repr(m.items()), "PMapItemsView(PMap({1: 2}))")
        leaf = []
        s = PSet([1])
        self.assertIn("PSet", repr(PMap({"s": s, "leaf": leaf})))

    def test_names(self):
        self.assertEqual((PMap.__module__, PMap.__qualname__), ("grail", "PMap"))
        self.assertEqual((PSet.__module__, PSet.__qualname__), ("grail", "PSet"))
        m = PMap({1: 2})
        for view, name, base in ((m.keys(), "PMapKeysView", collections.abc.KeysView),
                                 (m.values(), "PMapValuesView", collections.abc.ValuesView),
                                 (m.items(), "PMapItemsView", collections.abc.ItemsView)):
            self.assertEqual(type(view).__name__, name)
            self.assertEqual(type(view).__module__, "grail")
            self.assertIsInstance(view, base)
            with self.assertRaises(AttributeError):
                view.x = 1
        t = m.mutate()
        self.assertEqual(type(t).__name__, "_Transient")
        self.assertEqual(PMap[str, int].__args__, (str, int))
        self.assertEqual(repr(PMap[int, str]), "grail.PMap[int, str]")

    def test_constructor_signature(self):
        m = PMap({1: 2})
        self.assertIs(PMap(m), m)
        self.assertIs(PMap(source=m), m)
        s = PSet([1])
        self.assertIs(PSet(s), s)
        self.assertIs(PSet(source=s), s)
        self.assertEqual(PSet(source=[1, 2]), {1, 2})
        with self.assertRaises(TypeError):
            PMap({}, {})
        with self.assertRaises(TypeError):
            PMap(foo=1)
        with self.assertRaises(TypeError):
            PMap(None)
        with self.assertRaises(TypeError):
            PSet(1)
        with self.assertRaisesRegex(ValueError, "length two"):
            PMap([(1, 2, 3)])
        self.assertEqual(PMap(PSet([(1, 2)])), {1: 2})
        self.assertEqual(PMap([[1, 2], "ab"]), {1: 2, "a": "b"})
        self.assertIs(PMap.__init__, object.__init__)

    def test_methods(self):
        m = PMap({1: 2})
        with self.assertRaises(TypeError):
            m.get()
        with self.assertRaises(TypeError):
            m.set(1)
        with self.assertRaises(TypeError):
            m.get(key=1)
        with self.assertRaisesRegex(TypeError, "requires a callable"):
            m.update_with({}, 1)
        with self.assertRaises(KeyError) as cm:
            m.delete(5)
        self.assertEqual(cm.exception.args, (5,))
        with self.assertRaises(KeyError) as cm:
            m[5]
        self.assertEqual(cm.exception.args, (5,))
        self.assertIn(1, m)
        self.assertNotIn(2, m)
        self.assertEqual(list(m), [1])
        for name in ("isdisjoint", "union", "issubset"):
            self.assertFalse(hasattr(PSet([1]), name), name)
        for name in ("update", "pop", "setdefault", "copy", "__reversed__"):
            self.assertFalse(hasattr(m, name), name)
        with self.assertRaises(TypeError):
            weakref.ref(m)

    def test_closed_set(self):
        s = PSet([1])
        with self.assertRaises(AttributeError):
            s.root = 1
        with self.assertRaises(TypeError):
            PSet.add = None
        with self.assertRaises(TypeError):
            del PMap.get
        with self.assertRaises(TypeError):
            class Sub(PSet):
                pass
        with self.assertRaises(TypeError):
            s.add(1)[0] = 1

    def test_mixed_operators(self):
        self.assertEqual(PSet([1]) | [2], {1, 2})
        self.assertIsInstance(PSet([1]) | [2], PSet)
        r = [2] | PSet([1])
        self.assertIsInstance(r, PSet)
        self.assertEqual(r, {1, 2})
        self.assertEqual(PSet([1]) & [1, 3], {1})
        self.assertEqual([1, 3] - PSet([1]), {3})
        self.assertEqual((1, 3) ^ PSet([1]), {3})
        self.assertIsInstance({2} & PSet([2]), PSet)
        self.assertIsInstance(frozenset({2}) - PSet([2]), PSet)
        with self.assertRaises(TypeError):
            PSet([1]) | 5
        with self.assertRaises(TypeError):
            PMap({1: 2}) | PSet([3])
        with self.assertRaises(TypeError):
            PMap({1: 2}) & PSet([3])
        self.assertEqual(PSet([3]) | PMap({1: 2}), {1, 3})
        self.assertEqual(PSet([1, 3]) & PMap({1: 2}), {1})
        self.assertIsInstance({"a": 1} | PMap({"b": 2}), PMap)
        self.assertEqual(PMap({1: 2}) | collections.OrderedDict({3: 4}), {1: 2, 3: 4})
        empty = PMap()
        m = PMap({1: 2})
        self.assertIs(m | empty, m)
        self.assertIs(empty | m, m)
        s = PSet([1])
        self.assertIs(s | PSet(), s)
        self.assertIs(PSet() | s, s)
        self.assertIs(s ^ PSet(), s)
        e = PSet()
        self.assertIs(e & s, e)
        self.assertIs(s - e, s)

    def test_comparisons(self):
        m = PMap({1: 2})
        self.assertIs(m.__eq__(5), NotImplemented)
        self.assertIs(m.__eq__(PSet([1])), NotImplemented)
        self.assertEqual(m, collections.OrderedDict({1: 2}))
        self.assertEqual(m, frozendict({1: 2}))
        self.assertNotEqual(m, {1: 3})
        self.assertTrue(m != {1: 3})
        with self.assertRaises(TypeError):
            m < PMap()
        with self.assertRaises(TypeError):
            m <= {}
        s = PSet([1, 2])
        self.assertIs(s.__lt__(5), NotImplemented)
        self.assertIs(s.__le__([1, 2]), NotImplemented)
        self.assertIs(s.__eq__([1, 2]), NotImplemented)
        self.assertNotEqual(s, [1, 2])
        self.assertEqual(s, frozenset({1, 2}))
        self.assertEqual(frozenset({1, 2}), s)
        self.assertTrue({1} < s)
        self.assertTrue(s >= {1})
        self.assertTrue(s > PSet([2]))
        self.assertEqual(s, PMap({1: 0, 2: 0}).keys())
        self.assertNotEqual(PSet([1]), PMap({1: 2}))
        self.assertEqual(hash(s), hash(frozenset({1, 2})))
        self.assertEqual(hash(m), hash(frozenset(m.items())))

    def test_pickle_all_protocols(self):
        m = PMap({1: "a", "b": (2, 3)})
        s = PSet([1, "x", (2,)])
        for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
            for value in (m, s, PMap(), PSet()):
                copied = pickle.loads(pickle.dumps(value, protocol))
                self.assertIs(type(copied), type(value))
                self.assertEqual(copied, value)
        self.assertEqual(m.__reduce__(), (PMap, ({1: "a", "b": (2, 3)},)))
        self.assertEqual(PSet([1]).__reduce__(), (PSet, ([1],)))
        with self.assertRaisesRegex(TypeError, "memo must be a dict"):
            m.__deepcopy__([])
        self.assertIs(copy.copy(s), s)

    def test_transient_api(self):
        t = PMap({1: 2}).mutate()
        with self.assertRaises(TypeError):
            hash(t)
        with self.assertRaises(TypeError):
            iter(t)
        with self.assertRaises(TypeError):
            pickle.dumps(t)
        with self.assertRaises(TypeError):
            copy.copy(t)
        with self.assertRaises(TypeError):
            type(t)()
        with self.assertRaisesRegex(TypeError, "require a set transient"):
            t.add(1)
        with self.assertRaises(KeyError):
            del t[5]
        with self.assertRaises(KeyError):
            t[5]
        self.assertEqual(t.finish(), {1: 2})
        st = PSet([1]).mutate()
        with self.assertRaisesRegex(TypeError, "not subscriptable"):
            st[1]
        with self.assertRaisesRegex(TypeError, "add/discard"):
            st[1] = 2
        with self.assertRaisesRegex(TypeError, "add/discard"):
            del st[1]
        self.assertEqual(len(st), 1)
        st.add(1)
        st.add(2)
        self.assertEqual(len(st), 2)
        result = st.finish()
        self.assertIsInstance(result, PSet)
        self.assertEqual(result, {1, 2})
        with self.assertRaisesRegex(RuntimeError, "no longer open"):
            st.finish()

    def test_transient_context_rules(self):
        with PMap().mutate() as x:
            x[1] = 2
        with self.assertRaisesRegex(RuntimeError, "no longer open"):
            x[1]
        with self.assertRaisesRegex(RuntimeError, "no longer open"):
            x.__enter__()
        with self.assertRaisesRegex(RuntimeError, "invalid transient context exit"):
            x.__exit__(None, None, None)
        self.assertEqual(x.finish(), {1: 2})
        with self.assertRaisesRegex(RuntimeError, "no longer open"):
            x.finish()
        t = PMap().mutate()
        with self.assertRaisesRegex(RuntimeError, "invalid transient context exit"):
            t.__exit__(None, None, None)
        t2 = PMap().mutate()
        def other_thread():
            with self.assertRaisesRegex(RuntimeError, "another thread"):
                t2[1] = 2
            with self.assertRaisesRegex(RuntimeError, "another thread"):
                1 in t2
        run_threads(lambda _: other_thread(), [None], 1)
        t2[1] = 2
        self.assertEqual(t2.finish(), {1: 2})

    def test_transient_does_not_edit_base(self):
        base_items = {i: i for i in range(10)}
        m = PMap(base_items)
        s = PSet(range(10))
        t = m.mutate()
        u = s.mutate()
        for i in range(10):
            t[i] = -i
            u.discard(i)
        self.assertEqual(m, base_items)
        self.assertEqual(s, set(range(10)))
        self.assertEqual(t.finish(), {i: -i for i in range(10)})
        self.assertEqual(u.finish(), set())

    def test_shared_set_and_view_iterators(self):
        s = PSet(range(5000))
        m = PMap((i, -i) for i in range(5000))
        for it, expected in ((iter(s), range(5000)),
                             (iter(m.values()), range(-4999, 1)),
                             (iter(m.items()), [(i, -i) for i in range(5000)])):
            parts = run_threads(lambda _: list(it), range(8), 8)
            self.assertCountEqual([x for part in parts for x in part], expected)

    def test_shared_update_and_hash(self):
        m = PMap((i, i) for i in range(500))
        s = PSet(range(500))
        def worker(n):
            for r in range(50):
                m2 = m.set(n, r) | {1000 + n: r}
                self.assertEqual(m2[n], r)
                self.assertEqual(len(m2), 501)
                self.assertEqual(hash(m), hash(frozendict(m)))
                self.assertEqual(hash(s), hash(frozenset(range(500))))
                self.assertEqual((s - {n}) | {n}, s)
                self.assertEqual(m.update_with({n: 1}, operator.add)[n], n + 1)
        run_threads(worker, range(8), 8)
        self.assertEqual(m, {i: i for i in range(500)})


def sysconfig_free_threaded():
    import sysconfig
    return bool(sysconfig.get_config_var("Py_GIL_DISABLED"))


if __name__ == "__main__":
    unittest.main()
