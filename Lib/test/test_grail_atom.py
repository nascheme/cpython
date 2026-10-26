# Copied from grail-compat tests/ by tools/sync_to_proto.py; do not edit here.
"""Grail's synchronized boxed reference cell (backend-agnostic port).

Ported from cpython-proto/Lib/test/test_grail_atom.py; runs against both the
pure-Python shim and the native prototype.
"""
import concurrent.futures
import copy
import gc
import operator
import pickle
import subprocess
import sys
import sysconfig
import threading
import unittest
import weakref

from grail import Atom, PMap, field, is_value_record, record


# Subprocesses keep the GIL disabled on free-threaded builds.
GIL_ARGS = ("-X", "gil=0") if sysconfig.get_config_var("Py_GIL_DISABLED") else ()

TIMEOUT = 60


def assert_python_ok(*args):
    proc = subprocess.run([sys.executable, *args], capture_output=True,
                          text=True, timeout=TIMEOUT)
    if proc.returncode != 0:
        raise AssertionError(f"subprocess failed ({proc.returncode}):\n"
                             f"{proc.stdout}\n{proc.stderr}")


def run_pool(n, fn, items):
    """Run fn over items on n threads; fail instead of hanging."""
    with concurrent.futures.ThreadPoolExecutor(n) as pool:
        futures = [pool.submit(fn, item) for item in items]
        return [f.result(timeout=TIMEOUT) for f in futures]


class AtomTests(unittest.TestCase):
    def test_construction_and_storage(self):
        value = []
        a = Atom(value=value)
        self.assertIs(a.value, value)
        self.assertIs(Atom[int].__origin__, Atom)
        for args in ((), (1, 2)):
            with self.assertRaises(TypeError):
                Atom(*args)
        with self.assertRaises(TypeError):
            Atom(other=1)
        for name in ("value", "_value", "_lock"):
            with self.assertRaises((TypeError, AttributeError)):
                setattr(a, name, 1)
        with self.assertRaises(AttributeError):
            del a.value
        with self.assertRaises(TypeError):
            Atom.x = 1
        with self.assertRaises(TypeError):
            class Sub(Atom):
                pass
        # Repeated initialization cannot bypass reset's synchronization.
        a.__init__(123)
        self.assertIs(a.value, value)
        with self.assertRaises(TypeError):
            hash(a)
        self.assertNotEqual(a, Atom(value))
        self.assertEqual(a, a)

    def test_reset_and_identity_cas(self):
        old, equal, new = [1], [1], [2]
        a = Atom(old)
        self.assertFalse(a.compare_and_set(equal, new))
        self.assertIs(a.value, old)
        self.assertTrue(a.compare_and_set(old, new))
        self.assertIs(a.value, new)
        self.assertFalse(a.compare_and_set(old, equal))
        self.assertIs(a.reset(old), old)
        self.assertTrue(a.compare_and_set(old, old))
        self.assertIs(a.value, old)
        self.assertIsNone(a.reset(None))
        self.assertIsNone(a.value)

    def test_cas_never_compares_values(self):
        class NoCompare:
            def __eq__(self, other):
                raise AssertionError("must not compare")
            def __hash__(self):
                raise AssertionError("must not hash")
        old, other = NoCompare(), NoCompare()
        a = Atom(old)
        self.assertFalse(a.compare_and_set(other, None))
        self.assertTrue(a.compare_and_set(old, None))
        nan = float("nan")
        a.reset(nan)
        self.assertTrue(a.compare_and_set(nan, None))

    def test_identity_aba_is_allowed(self):
        old, temporary = object(), object()
        a = Atom(old)
        a.reset(temporary)
        a.reset(old)
        self.assertTrue(a.compare_and_set(old, 1))

    def test_add(self):
        a = Atom(0)
        for delta in (1, -2, 0, 2**200, -(2**201), 2**200, 1):
            old = a.value
            result = a.add(delta)
            self.assertIs(type(result), int)
            self.assertEqual(result, old + delta)
            self.assertIs(a.value, result)
        self.assertEqual(a.value, 0)
        # Cross the compact/multi-digit and machine-word boundaries both ways.
        for bits in (sys.int_info.bits_per_digit, 63, 64, 127):
            for sign in (-1, 1):
                old = sign * (2**bits - 1)
                a.reset(old)
                self.assertEqual(a.add(sign), sign * 2**bits)
                self.assertEqual(a.add(-sign), old)

    def test_add_rejects_non_exact_ints(self):
        class IntSubclass(int):
            def __add__(self, other):
                raise AssertionError("must not call user arithmetic")
            def __radd__(self, other):
                raise AssertionError("must not call user arithmetic")

        class NoCoercion:
            def __index__(self):
                raise AssertionError("must not coerce")
            def __add__(self, other):
                raise AssertionError("must not call user arithmetic")
            __radd__ = __add__

        invalid = (True, False, IntSubclass(1), 1.0, None, NoCoercion())
        for value in invalid:
            a = Atom(value)
            with self.assertRaises(TypeError):
                a.add(1)
            self.assertIs(a.value, value)
            a.reset(10)
            for delta in invalid:
                with self.assertRaises(TypeError):
                    a.add(delta)
                self.assertEqual(a.value, 10)
        for call in (lambda: a.add(), lambda: a.add(1, 2),
                     lambda: a.add(delta=1)):
            with self.assertRaises(TypeError):
                call()
        self.assertEqual(a.value, 10)

    def test_add_ignores_operator_registration(self):
        # Operator registrations persist for the interpreter's lifetime.
        # (int, int) cannot be registered, so Atom.add's exact-int path can
        # never be overridden.
        assert_python_ok(*GIL_ARGS, "-c", '''
from grail import Atom, ops
try:
    ops.add.register((int, int), lambda a, b: None)
except TypeError:
    pass
else:
    raise AssertionError("builtin pair registered")
a = Atom(2**100)
assert a.add(1) == 1267650600228229401496703205377
assert a.add(-1) == 2**100
''')

    def test_swap_arguments(self):
        a = Atom(3)
        self.assertEqual(a.swap(operator.add, 2), 5)
        self.assertEqual(a.swap(lambda old, x, *, factor: (old + x) * factor,
                                4, factor=2), 18)
        self.assertEqual(a.swap(lambda old, *, f: old + f, f=1), 19)
        self.assertEqual(a.value, 19)
        for call in (lambda: a.swap(), lambda: a.swap(None),
                     lambda: a.swap(f=operator.neg)):
            with self.assertRaises(TypeError):
                call()
        self.assertEqual(a.value, 19)
        # Returning the exact snapshot is a valid successful update.
        self.assertEqual(a.swap(lambda old: old), 19)

    def test_swap_exception(self):
        a = Atom(3)
        def fail(old):
            raise ValueError("callback failed")
        with self.assertRaisesRegex(ValueError, "callback failed"):
            a.swap(fail)
        self.assertEqual(a.value, 3)
        self.assertEqual(a.swap(operator.add, 1), 4)

    def test_reentrant_callback_retries(self):
        a = Atom(0)
        seen = []
        def update(old):
            seen.append(old)
            self.assertEqual(a.value, old)
            if len(seen) == 1:
                a.reset(10)
            return old + 1
        self.assertEqual(a.swap(update), 11)
        self.assertEqual(a.value, 11)
        self.assertEqual(seen, [0, 10])

    def test_reentrant_repr(self):
        a = Atom(1)
        self.assertEqual(repr(a), "Atom(1)")
        a.reset(a)
        self.assertEqual(repr(a), "Atom(...)")
        class Value:
            def __repr__(self):
                a.reset(3)
                return "snapshot"
        a.reset(Value())
        self.assertEqual(repr(a), "Atom(snapshot)")
        self.assertEqual(a.value, 3)

    def test_reentrant_finalizer(self):
        observed = []
        class Retired:
            def __del__(self):
                observed.append(a.value)
                a.reset("finalizer")
        a = Atom(Retired())
        self.assertEqual(a.reset("replacement"), "replacement")
        gc.collect()
        self.assertEqual(observed, ["replacement"])
        self.assertEqual(a.value, "finalizer")

    def test_failed_candidate_finalizer(self):
        observed = []
        a = Atom(0)
        class Candidate:
            def __del__(self):
                observed.append(a.value)
                a.reset(20)
        calls = []
        def update(old):
            calls.append(old)
            if len(calls) == 1:
                a.reset(10)
                return Candidate()
            return old + 1
        self.assertEqual(a.swap(update), 21)
        self.assertEqual(calls, [0, 20])
        self.assertEqual(observed, [10])

    def test_copy(self):
        value = []
        a = Atom(value)
        b = copy.copy(a)
        self.assertIsNot(a, b)
        self.assertIs(a.value, b.value)
        a.reset(1)
        self.assertIs(b.value, value)
        c = copy.deepcopy(b)
        self.assertIsNot(b, c)
        self.assertIsNot(c.value, value)
        self.assertEqual(c.value, value)

    def test_deepcopy_cycles_and_aliases(self):
        a = Atom(None)
        a.reset(a)
        b = copy.deepcopy(a)
        self.assertIsNot(a, b)
        self.assertIs(b.value, b)
        leaf = []
        a.reset([leaf, leaf, a])
        b = copy.deepcopy(a)
        self.assertIsNot(b.value[0], leaf)
        self.assertIs(b.value[0], b.value[1])
        self.assertIs(b.value[2], b)
        a.reset(PMap({"cell": a}))
        b = copy.deepcopy(a)
        self.assertIs(b.value["cell"], b)

    def test_deepcopy_failure(self):
        class BadCopy:
            def __deepcopy__(self, memo):
                raise ValueError("copy failed")
        a = Atom(BadCopy())
        memo = {}
        with self.assertRaisesRegex(ValueError, "copy failed"):
            copy.deepcopy(a, memo)
        self.assertNotIn(id(a), memo)
        self.assertIsInstance(a.value, BadCopy)

    def test_pickle(self):
        for dumps, loads in ((pickle.dumps, pickle.loads), (pickle._dumps, pickle._loads)):
            for protocol in range(pickle.HIGHEST_PROTOCOL + 1):
                a = Atom(PMap({1: 2}))
                b = loads(dumps(a, protocol))
                self.assertIsNot(a, b)
                self.assertEqual(b.value, {1: 2})
                a.reset(a)
                b = loads(dumps(a, protocol))
                self.assertIs(b.value, b)
                leaf = []
                a.reset([leaf, leaf, a])
                b = loads(dumps(a, protocol))
                self.assertIs(b.value[0], b.value[1])
                self.assertIs(b.value[2], b)
                self.assertIsNone(loads(dumps(Atom(None), protocol)).value)

    def test_gc_cycles(self):
        class Leaf:
            pass
        a = Atom(None)
        leaf = Leaf()
        ref = weakref.ref(leaf)
        leaf.parent = a
        a.reset(leaf)
        self.assertTrue(gc.is_tracked(a))
        self.assertIn(leaf, gc.get_referents(a))
        del a, leaf
        gc.collect()
        self.assertIsNone(ref())

    def test_record_field(self):
        with self.assertRaisesRegex(ValueError, "mutable record default"):
            @record
            class Bad:
                counter: Atom[int] = Atom(0)
        @record
        class State:
            counter: Atom[int] = field(factory=lambda: Atom(0))
        self.assertFalse(is_value_record(State))
        a, b = State(), State()
        a.counter.reset(1)
        self.assertEqual(b.counter.value, 0)
        self.assertIsNot(a.counter, copy.copy(a.counter))


class ThreadTests(unittest.TestCase):
    def test_add_contended_results(self):
        a = Atom(0)
        barrier = threading.Barrier(8, timeout=10)
        def worker(_):
            barrier.wait()
            return [a.add(1) for _ in range(500)]
        results = run_pool(8, worker, range(8))
        self.assertEqual(a.value, 4000)
        self.assertEqual(sorted(x for batch in results for x in batch),
                         list(range(1, 4001)))

    def test_add_mixed_updates(self):
        a = Atom(2**100)
        barrier = threading.Barrier(8, timeout=10)
        def worker(n):
            barrier.wait()
            for i in range(200):
                if n % 3 == 0:
                    a.add(1)
                elif n % 3 == 1:
                    a.swap(operator.add, 1)
                else:
                    while True:
                        old = a.value
                        if a.compare_and_set(old, old + 1):
                            break
                if i % 50 == 0:
                    gc.collect()
        run_pool(8, worker, range(8))
        self.assertEqual(a.value, 2**100 + 1600)

    def test_add_with_concurrent_reset(self):
        a = Atom(0)
        barrier = threading.Barrier(4, timeout=10)
        def worker(n):
            barrier.wait()
            for i in range(500):
                if n == 0:
                    a.reset(None if i % 2 else 2**100)
                else:
                    try:
                        result = a.add(-1)
                    except TypeError:
                        pass  # A synchronized snapshot observed None.
                    else:
                        self.assertIs(type(result), int)
        run_pool(4, worker, range(4))
        a.reset(10)
        self.assertEqual(a.add(-1), 9)

    def test_contended_counter(self):
        a = Atom(0)
        def worker(_):
            for _ in range(500):
                a.swap(operator.add, 1)
        run_pool(8, worker, range(8))
        self.assertEqual(a.value, 4000)

    def test_cas_counter(self):
        a = Atom(0)
        def worker(_):
            for _ in range(200):
                while True:
                    old = a.value
                    if a.compare_and_set(old, old + 1):
                        break
        run_pool(8, worker, range(8))
        self.assertEqual(a.value, 1600)

    def test_deterministic_swap_retry(self):
        a = Atom(0)
        barrier = threading.Barrier(2, timeout=10)
        def worker(_):
            calls = []
            def update(old):
                calls.append(old)
                if len(calls) == 1:
                    barrier.wait()
                return old + 1
            return a.swap(update), calls
        results = run_pool(2, worker, range(2))
        self.assertEqual(a.value, 2)
        self.assertCountEqual([value for value, _ in results], [1, 2])
        self.assertCountEqual([calls for _, calls in results], [[0], [0, 1]])

    def test_map_updates(self):
        a = Atom(PMap({"count": 0}))
        def worker(n):
            for _ in range(100):
                a.swap(lambda m: m.set("count", m["count"] + 1).set(n, True))
        run_pool(4, worker, range(4))
        self.assertEqual(a.value, {"count": 400, 0: True, 1: True, 2: True, 3: True})

    def test_snapshot_keeps_old_value_alive(self):
        class Leaf:
            pass
        a = Atom(Leaf())
        ref = weakref.ref(a.value)
        acquired, release = threading.Event(), threading.Event()
        def reader():
            snapshot = a.value
            acquired.set()
            if not release.wait(10):
                raise AssertionError("reader timed out")
            return type(snapshot).__name__
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            future = pool.submit(reader)
            try:
                self.assertTrue(acquired.wait(10))
                a.reset(None)
                gc.collect()
                self.assertIsNotNone(ref())
            finally:
                release.set()
            self.assertEqual(future.result(timeout=TIMEOUT), "Leaf")
        gc.collect()
        self.assertIsNone(ref())

    def test_record_publication(self):
        @record
        class Snapshot:
            n: int
            pair: tuple[int, int]
        a = Atom(Snapshot(0, (0, 0)))
        def worker(_):
            for _ in range(100):
                def update(old):
                    return Snapshot(old.n + 1, (old.n + 1, old.n + 1))
                value = a.swap(update)
                self.assertEqual(value.pair, (value.n, value.n))
                value = a.value
                self.assertEqual(value.pair, (value.n, value.n))
        run_pool(4, worker, range(4))
        self.assertEqual(a.value.n, 400)

    def test_mixed_access_and_gc(self):
        a = Atom(PMap({"n": 0}))
        def worker(n):
            for i in range(150):
                value = PMap({"n": n * 150 + i})
                a.reset(value)
                snapshot = a.value
                a.compare_and_set(snapshot, value)
                a.swap(lambda old: old.set("n", old["n"] + 1))
                repr(a)
                if i % 30 == 0:
                    copy.copy(a)
                    gc.get_referents(a)
                    gc.collect()
        run_pool(4, worker, range(4))
        self.assertIsInstance(a.value["n"], int)

    def test_lock_free_reads_race_retirement(self):
        # Readers acquire references without a lock while writers drop the
        # only other reference to each retired value. Include non-GC objects
        # (floats, strings) and objects owned by other threads.
        class Box:
            __slots__ = ("n",)
            def __init__(self, n):
                self.n = n
        a = Atom(Box(0))
        stop = threading.Event()
        errors = []
        def writer(n):
            for i in range(20000):
                if i % 3 == 0:
                    a.reset(Box(i))
                elif i % 3 == 1:
                    a.reset(float(i) + 0.5)
                else:
                    a.reset(str(i) * 3)
        def reader():
            seen = 0
            while not stop.is_set() or seen < 1000:
                value = a.value
                if type(value) not in (Box, float, str):
                    errors.append(type(value))
                    return
                seen += 1
        readers = [threading.Thread(target=reader) for _ in range(3)]
        for t in readers:
            t.start()
        try:
            run_pool(3, writer, range(3))
        finally:
            stop.set()
            for t in readers:
                t.join(TIMEOUT)
                self.assertFalse(t.is_alive())
        self.assertEqual(errors, [])

    def test_no_gil(self):
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    def test_subinterpreter(self):
        try:
            import _interpreters
        except ImportError:
            self.skipTest("no _interpreters module")
        interp = _interpreters.create()
        try:
            error = _interpreters.run_string(interp,
                "from grail import Atom; a = Atom(1); "
                "assert a.swap(lambda x: x + 1) == 2; "
                "assert a.compare_and_set(a.value, 3); assert a.value == 3; "
                "assert a.add(-1) == 2; assert a.value == 2")
            self.assertIsNone(error)
        finally:
            _interpreters.destroy(interp)


class ApiDetailTests(unittest.TestCase):
    """Observable details checked against the native type."""

    def test_type_identity(self):
        self.assertEqual(Atom.__module__, "grail")
        self.assertEqual(Atom.__qualname__, "Atom")
        self.assertEqual(Atom.__name__, "Atom")
        self.assertEqual(repr(Atom[int]), "grail.Atom[int]")
        self.assertIn("synchronized reference cell", Atom.__doc__)
        self.assertIsInstance(Atom(1), Atom)

    def test_no_weakref_or_dict(self):
        a = Atom(1)
        with self.assertRaises(TypeError):
            weakref.ref(a)
        with self.assertRaises(TypeError):
            vars(a)
        with self.assertRaises(AttributeError):
            a.other = 1

    def test_messages(self):
        a = Atom(None)
        cases = [
            (lambda: Atom(), "missing required argument 'value'"),
            (lambda: Atom(1, 2), "at most 1 argument"),
            (lambda: a.add(1.0), "add delta must be an exact int"),
            (lambda: a.add(1), "add value must be an exact int"),
            (lambda: a.swap(), "swap requires a positional callable"),
            (lambda: a.swap(1), "swap's first argument must be callable"),
            (lambda: a.__deepcopy__([]), "deepcopy memo must be a dict"),
        ]
        for call, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(TypeError, message):
                    call()
        with self.assertRaisesRegex(AttributeError, "not writable"):
            a.value = 1
        with self.assertRaisesRegex(TypeError, "immutable type 'grail.Atom'"):
            Atom.x = 1
        with self.assertRaisesRegex(TypeError, "not an acceptable base type"):
            type("Sub", (Atom,), {})

    def test_positional_only(self):
        a = Atom(0)
        for call in (lambda: a.reset(value=1),
                     lambda: a.compare_and_set(expected=0, desired=1),
                     lambda: a.compare_and_set(0),
                     lambda: a.reset()):
            with self.assertRaises(TypeError):
                call()
        self.assertEqual(a.value, 0)
        # Keyword names of the callback do not collide with swap's own.
        self.assertEqual(a.swap(lambda old, f, self: old + f + self,
                                f=1, self=2), 3)

    def test_reduce_shape(self):
        a = Atom(5)
        self.assertEqual(a.__reduce__(), (Atom, (None,), 5))
        b = Atom(None)
        self.assertIsNone(b.__setstate__(7))
        self.assertEqual(b.value, 7)

    def test_copy_snapshot_is_independent(self):
        a = Atom([1])
        b = copy.copy(a)
        b.reset(2)
        self.assertEqual(a.value, [1])

    def test_deepcopy_memo_shared(self):
        a = Atom([1])
        memo = {}
        b = copy.deepcopy(a, memo)
        self.assertIs(copy.deepcopy(a, memo), b)


if __name__ == "__main__":
    unittest.main()
