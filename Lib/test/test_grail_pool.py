# Copied from grail-compat tests/ by tools/sync_to_cpython_grail.py; do not edit here.
"""grail.Pool, the thread pool (backend-agnostic: native and pure Python)."""
import contextvars
import gc
import os
import subprocess
import sys
import sysconfig
import threading
import time
import unittest

import grail
from grail import Pool


GIL_ARGS = ("-X", "gil=0") if sysconfig.get_config_var("Py_GIL_DISABLED") else ()


def run_python(code, timeout=60):
    return subprocess.run([sys.executable, *GIL_ARGS, "-c", code],
                          capture_output=True, text=True, timeout=timeout)


def spin(usecs):
    end = time.perf_counter_ns() + usecs * 1000
    while time.perf_counter_ns() < end:
        pass


class PoolTests(unittest.TestCase):
    def setUp(self):
        self.pool = Pool(4)
        self.addCleanup(self.pool.shutdown)

    def test_bad_n_threads(self):
        for n in (0, -1):
            with self.assertRaisesRegex(ValueError, "greater than 0"):
                Pool(n)
        self.assertEqual(self.pool.n_threads, 4)

    def test_bad_arguments(self):
        with self.assertRaises(TypeError):
            self.pool.map(abs)
        with self.assertRaises(TypeError):
            self.pool.map(abs, 1)
        with self.assertRaises(TypeError):
            self.pool.map(abs, [1], chunksize=1)
        with self.assertRaisesRegex(ValueError, "buffersize"):
            self.pool.map(abs, [1], buffersize=0)
        with self.assertRaises(OverflowError):
            self.pool.map(abs, [1], buffersize=-1)

    def test_results(self):
        for n_threads in (1, 2, 5):
            with Pool(n_threads) as pool:
                for buffersize in (None, 1, 7, 1000):
                    with self.subTest(n_threads=n_threads, buffersize=buffersize):
                        it = pool.map(lambda x: x * 2, range(1000),
                                      buffersize=buffersize)
                        self.assertNotIsInstance(it, list)
                        self.assertEqual(list(it), [x * 2 for x in range(1000)])
                        it = pool.map(lambda x: x * 2, range(1000),
                                      buffersize=buffersize, return_in_order=False)
                        self.assertEqual(sorted(it), [x * 2 for x in range(1000)])

    def test_results_keep_pool_alive(self):
        it = Pool(2).map(lambda x: x, range(100))
        gc.collect()
        self.assertEqual(list(it), list(range(100)))

    def test_interrupt_waiting_consumer(self):
        proc = run_python(
            "import grail, os, signal, threading, time\n"
            "threading.Timer(0.3, os.kill, (os.getpid(), signal.SIGINT)).start()\n"
            "pool = grail.Pool(2)\n"
            "try:\n"
            "    list(pool.map(time.sleep, [0.1] * 100))\n"
            "except KeyboardInterrupt:\n"
            "    print('interrupted')\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "interrupted\n")

    def test_several_iterables(self):
        it = self.pool.map(lambda a, b: a + b, range(10), range(100, 105))
        self.assertEqual(list(it), [100, 102, 104, 106, 108])

    def test_empty(self):
        self.assertEqual(list(self.pool.map(abs, [])), [])
        self.assertEqual(list(self.pool.map(abs, [], return_in_order=False)), [])

    def test_order_with_uneven_delays(self):
        delays = [((i * 7919) % 13) * 20 for i in range(300)]
        def f(i, usecs):
            spin(usecs)
            return i
        for buffersize in (None, 1, 16):
            with self.subTest(buffersize=buffersize):
                it = self.pool.map(f, range(300), delays, buffersize=buffersize)
                self.assertEqual(list(it), list(range(300)))

    def test_task_exception(self):
        def f(x):
            if x == 3:
                raise ValueError(x)
            return x
        it = self.pool.map(f, range(6))
        self.assertEqual([next(it), next(it), next(it)], [0, 1, 2])
        with self.assertRaises(ValueError):
            next(it)
        self.assertEqual(list(it), [4, 5])
        it = self.pool.map(f, range(6), return_in_order=False)
        results, errors = [], 0
        while True:
            try:
                results.append(next(it))
            except ValueError:
                errors += 1
            except StopIteration:
                break
        self.assertEqual((sorted(results), errors), ([0, 1, 2, 4, 5], 1))

    def test_iterator_exception(self):
        def gen():
            yield 1
            yield 2
            raise KeyError("input")
        for ordered in (True, False):
            it = self.pool.map(lambda x: x, gen(), return_in_order=ordered)
            got = [next(it), next(it)]
            self.assertEqual(sorted(got), [1, 2])
            with self.assertRaises(KeyError):
                next(it)
            self.assertEqual(list(it), [])

    def test_contextvars_copied_at_map(self):
        var = contextvars.ContextVar("var")
        for value in (1, 2):
            var.set(value)
            it = self.pool.map(lambda _: var.get(), range(100))
            var.set(-1)
            self.assertEqual(set(it), {value})

    def test_inputs_iterated_in_map_context(self):
        var = contextvars.ContextVar("var", default="missing")
        var.set("caller")
        seen = []
        def inputs():
            seen.append(var.get())
            yield 1
        for ordered in (True, False):
            list(self.pool.map(lambda _: None, inputs(), return_in_order=ordered))
        self.assertEqual(seen, ["caller", "caller"])

    def test_task_context_changes_stay_in_pool(self):
        var = contextvars.ContextVar("var", default=0)
        list(self.pool.map(var.set, range(100)))
        self.assertEqual(var.get(), 0)

    def test_interleaved_maps(self):
        var = contextvars.ContextVar("var")
        its = []
        for value in range(100):
            var.set(value)
            its.append(self.pool.map(lambda _: var.get(), range(100),
                                     buffersize=value % 3 + 1 if value % 2 else None))
        for value, it in enumerate(its):
            self.assertEqual(list(it), [value] * 100)

    def test_buffersize_limits_claimed_items(self):
        ran = []
        lock = threading.Lock()
        def task(x):
            with lock:
                ran.append(x)
        for ordered in (True, False):
            ran.clear()
            it = self.pool.map(task, range(100), buffersize=20,
                               return_in_order=ordered)
            while not it._is_full():
                pass
            time.sleep(0.01)
            self.assertLessEqual(len(ran), 20)
            next(it)
            next(it)
            while not it._is_full():
                pass
            time.sleep(0.01)
            self.assertLessEqual(len(ran), 22)
            self.assertEqual(len(list(it)), 98)
            self.assertEqual(len(ran), 100)

    def test_unbounded_map_iterates_lazily(self):
        live = 0
        peak = 0
        lock = threading.Lock()
        class Resource:
            def __init__(self):
                nonlocal live, peak
                with lock:
                    live += 1
                    peak = max(peak, live)
            def __del__(self):
                nonlocal live
                with lock:
                    live -= 1
        def task(r):
            spin(10)
        list(self.pool.map(task, (Resource() for _ in range(2000))))
        self.assertLess(peak, 30 * self.pool.n_threads)

    def test_nested_map(self):
        pool = Pool(2)
        self.addCleanup(pool.shutdown)
        def outer(n):
            return sum(pool.map(lambda y: y, range(n)))
        self.assertEqual(list(pool.map(outer, range(40))),
                         [n * (n - 1) // 2 for n in range(40)])

    def test_dropped_iterator_runs_to_completion(self):
        for buffersize in (None, 5):
            done = []
            lock = threading.Lock()
            def task(x):
                spin(10)
                with lock:
                    done.append(x)
            with Pool(2) as pool:
                it = pool.map(task, range(500), buffersize=buffersize)
                next(it)
                del it
            self.assertEqual(sorted(done), list(range(500)))

    def test_shutdown(self):
        pool = Pool(2)
        it = pool.map(spin, [1000] * 100, buffersize=4)
        next(it)
        pool.shutdown()
        with self.assertRaisesRegex(RuntimeError, "shut down"):
            list(it)
        with self.assertRaisesRegex(RuntimeError, "shut down"):
            pool.map(abs, [1])
        pool.shutdown()

    def test_shutdown_from_task(self):
        # Must not hang. Whether the end of the input was seen before the
        # shutdown is a race, so the map may end with RuntimeError.
        pool = Pool(2)
        it = pool.map(lambda _: pool.shutdown(), [1])
        self.assertIsNone(next(it))
        try:
            next(it)
        except (StopIteration, RuntimeError):
            pass
        pool.shutdown()

    def test_gc_while_workers_idle_or_busy(self):
        list(self.pool.map(lambda _: gc.collect(), range(4)))
        time.sleep(0.05)
        gc.collect()

    def test_concurrent_consumers(self):
        it = self.pool.map(lambda x: x, range(20000))
        out = [[] for _ in range(4)]
        def consume(acc):
            acc.extend(it)
        threads = [threading.Thread(target=consume, args=(acc,)) for acc in out]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        merged = sorted(x for acc in out for x in acc)
        self.assertEqual(merged, list(range(20000)))
        for acc in out:
            self.assertEqual(acc, sorted(acc))


HAVE_GROUPS = sys.version_info >= (3, 11)


class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.pool = Pool(4)
        self.addCleanup(self.pool.shutdown)

    def test_spawn(self):
        done = grail.Atom(0)
        with self.pool.scope() as s:
            for i in range(1000):
                self.assertIsNone(s.spawn(done.add, i))
        self.assertEqual(done.value, sum(range(1000)))

    def test_args_and_kwargs(self):
        seen = []
        with self.pool.scope() as s:
            s.spawn(lambda a, b, *, c: seen.append((a, b, c)), 1, 2, c=3)
            s.spawn(seen.append, "x")
        self.assertEqual(sorted(seen, key=str), [(1, 2, 3), "x"])

    def test_tasks_run_on_workers(self):
        names = set()
        with self.pool.scope() as s:
            for _ in range(20):
                s.spawn(lambda: names.add(threading.current_thread().name))
        self.assertNotIn(threading.current_thread().name, names)

    def test_tasks_spawn_into_the_scope(self):
        total = grail.Atom(0)
        def fib(n, s):
            if n < 2:
                total.add(n)
            else:
                s.spawn(fib, n - 1, s)
                s.spawn(fib, n - 2, s)
        with self.pool.scope() as s:
            s.spawn(fib, 18, s)
        self.assertEqual(total.value, 2584)

    def test_nested_scopes(self):
        for n_threads in (1, 2):
            pool = Pool(n_threads)
            self.addCleanup(pool.shutdown)
            def tree(depth):
                if depth == 0:
                    return 1
                out = grail.Atom(0)
                with pool.scope() as s:
                    for _ in range(3):
                        s.spawn(lambda: out.add(tree(depth - 1)))
                return out.value
            with self.subTest(n_threads=n_threads):
                self.assertEqual(tree(5), 3 ** 5)

    def test_scope_and_map_nest(self):
        def task(n):
            return sum(self.pool.map(lambda x: x, range(n)))
        total = grail.Atom(0)
        with self.pool.scope() as s:
            for n in range(30):
                s.spawn(lambda n=n: total.add(task(n)))
        self.assertEqual(total.value, sum(n * (n - 1) // 2 for n in range(30)))
        def mapped(n):
            out = grail.Atom(0)
            with self.pool.scope() as s:
                for i in range(n):
                    s.spawn(out.add, i)
            return out.value
        self.assertEqual(list(self.pool.map(mapped, range(30))),
                         [n * (n - 1) // 2 for n in range(30)])

    def test_spawn_from_other_threads(self):
        total = grail.Atom(0)
        with self.pool.scope() as s:
            threads = [threading.Thread(target=lambda: [s.spawn(total.add, 1)
                                                        for _ in range(100)])
                       for _ in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        self.assertEqual(total.value, 400)

    def test_contextvars_copied_at_spawn(self):
        var = contextvars.ContextVar("var", default=0)
        seen = []
        with self.pool.scope() as s:
            var.set(1)
            s.spawn(lambda: seen.append(var.get()))
            var.set(2)
            s.spawn(lambda: seen.append(var.get()))
            s.spawn(var.set, 99)
        self.assertEqual(sorted(seen), [1, 2])
        self.assertEqual(var.get(), 2)

    def _errors(self, scope_body):
        try:
            with self.pool.scope() as s:
                scope_body(s)
        except BaseException as exc:
            return exc
        return None

    @unittest.skipUnless(HAVE_GROUPS, "needs exception groups")
    def test_task_errors_raise_a_group(self):
        def bad(i):
            raise ValueError(i)
        exc = self._errors(lambda s: [s.spawn(bad, i) for i in range(3)])
        self.assertIsInstance(exc, ExceptionGroup)
        self.assertGreaterEqual(len(exc.exceptions), 1)
        self.assertTrue(all(isinstance(e, ValueError) for e in exc.exceptions))
        exc = self._errors(lambda s: s.spawn(sys.exit, 3))
        self.assertIsInstance(exc, BaseExceptionGroup)
        self.assertNotIsInstance(exc, ExceptionGroup)

    def test_error_cancels_unstarted_tasks(self):
        pool = Pool(1)
        self.addCleanup(pool.shutdown)
        ran = []
        def bad():
            raise ValueError
        try:
            with pool.scope() as s:
                s.spawn(bad)
                time.sleep(0.05)
                for i in range(100):
                    s.spawn(ran.append, i)
        except BaseException as exc:
            first = exc.exceptions[0] if HAVE_GROUPS else exc
            self.assertIsInstance(first, ValueError)
        else:
            self.fail("no error")
        self.assertEqual(ran, [])

    def test_body_error_cancels_and_propagates(self):
        pool = Pool(1)
        self.addCleanup(pool.shutdown)
        ran = []
        with self.assertRaises(KeyError):
            with pool.scope() as s:
                s.spawn(time.sleep, 0.05)
                for i in range(100):
                    s.spawn(ran.append, i)
                raise KeyError
        self.assertEqual(ran, [])

    @unittest.skipUnless(HAVE_GROUPS, "needs exception groups")
    def test_body_error_joins_task_errors(self):
        def bad():
            raise ValueError
        def body(s):
            s.spawn(bad)
            time.sleep(0.05)
            raise KeyError
        exc = self._errors(body)
        self.assertIsInstance(exc, ExceptionGroup)
        self.assertEqual([type(e) for e in exc.exceptions], [KeyError, ValueError])

    def test_closed_scope(self):
        with self.pool.scope() as s:
            pass
        with self.assertRaisesRegex(RuntimeError, "closed"):
            s.spawn(print)
        with self.assertRaisesRegex(RuntimeError, "closed"):
            with s:
                pass

    def test_scope_waits_for_slow_tasks(self):
        done = []
        with self.pool.scope() as s:
            for i in range(8):
                s.spawn(lambda i=i: (spin(2000), done.append(i)))
        self.assertEqual(sorted(done), list(range(8)))

    def test_shutdown(self):
        pool = Pool(2)
        s = pool.scope()
        pool.shutdown()
        with self.assertRaisesRegex(RuntimeError, "shut down"):
            s.spawn(print)
        with self.assertRaisesRegex(RuntimeError, "shut down"):
            pool.scope()

    def test_interrupt_waiting_scope(self):
        proc = run_python(
            "import grail, os, signal, threading, time\n"
            "threading.Timer(0.3, os.kill, (os.getpid(), signal.SIGINT)).start()\n"
            "pool = grail.Pool(2)\n"
            "ran = []\n"
            "try:\n"
            "    with pool.scope() as s:\n"
            "        for i in range(200):\n"
            "            s.spawn(lambda: (time.sleep(0.05), ran.append(1)))\n"
            "except KeyboardInterrupt:\n"
            "    print('interrupted', len(ran) < 200)\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "interrupted True\n")


class ParallelHelperTests(unittest.TestCase):
    def setUp(self):
        self.pool = Pool(4)
        self.addCleanup(self.pool.shutdown)

    def test_pmap(self):
        for n in (0, 1, 2, 5, 1000):
            for chunk in (None, 1, 3, 2000):
                with self.subTest(n=n, chunk=chunk):
                    got = grail.pmap(lambda x: x * 2, range(n), chunk=chunk,
                                     pool=self.pool)
                    self.assertEqual(got, [x * 2 for x in range(n)])
        self.assertEqual(grail.pmap(abs, iter([-1, -2])), [1, 2])
        self.assertEqual(grail.pmap(abs, (-3,)), [3])

    def test_pmap_default_pool(self):
        self.assertEqual(grail.pmap(abs, [-1, -2, -3]), [1, 2, 3])

    def test_pmap_uses_the_pool_for_slow_items(self):
        idents = set()
        def f(x):
            idents.add(threading.get_ident())
            spin(1000)
            return x
        self.assertEqual(grail.pmap(f, range(40), pool=self.pool), list(range(40)))
        self.assertGreater(len(idents - {threading.get_ident()}), 1)

    def test_pmap_error(self):
        def f(x):
            if x in (30, 700):
                raise ValueError(x)
            return x
        for chunk in (None, 1, 50):
            with self.assertRaises(ValueError) as cm:
                grail.pmap(f, range(1000), chunk=chunk, pool=self.pool)
            self.assertEqual(cm.exception.args, (30,))

    def test_bad_chunk(self):
        for helper in (lambda **kw: grail.pmap(abs, [1], **kw),
                       lambda **kw: grail.parallel_reduce(max, max, [1], 0, **kw)):
            with self.assertRaises(ValueError):
                helper(chunk=0)
            with self.assertRaises(TypeError):
                helper(chunk=1.5)

    def test_parallel_reduce(self):
        add = lambda a, b: a + b
        for n in (0, 1, 2, 63, 64, 65, 10000):
            for chunk in (None, 1, 7):
                with self.subTest(n=n, chunk=chunk):
                    got = grail.parallel_reduce(add, add, range(n), 0,
                                                chunk=chunk, pool=self.pool)
                    self.assertEqual(got, sum(range(n)))
        self.assertEqual(grail.parallel_reduce(add, add, [], "init"), "init")

    def test_parallel_reduce_order(self):
        # Neither fold nor combine is commutative: results stay in order.
        got = grail.parallel_reduce(lambda acc, x: acc + (x,),
                                    lambda a, b: a + b,
                                    range(500), (), chunk=9, pool=self.pool)
        self.assertEqual(got, tuple(range(500)))

    def test_parallel_reduce_with_pmap_init(self):
        def fold(m, word):
            return m.set(word, m.get(word, 0) + 1)
        def merge(a, b):
            for k, v in b.items():
                a = a.set(k, a.get(k, 0) + v)
            return a
        words = ["a", "b", "c", "a"] * 1000
        got = grail.parallel_reduce(fold, merge, words, grail.PMap(), pool=self.pool)
        self.assertEqual(dict(got), {"a": 2000, "b": 1000, "c": 1000})

    def test_parallel_reduce_deterministic_partition(self):
        import random
        rng = random.Random(1)
        xs = [rng.uniform(-1e10, 1e10) for _ in range(20000)]
        add = lambda a, b: a + b
        results = set()
        for n_threads in (1, 3, 8):
            with Pool(n_threads) as pool:
                for _ in range(3):
                    results.add(grail.parallel_reduce(add, add, xs, 0.0, pool=pool))
        self.assertEqual(len(results), 1)
        # The default partition is 64 chunks, folded and then combined as a
        # balanced tree of adjacent results.
        size = -(-len(xs) // 64)
        partial = []
        for i in range(0, len(xs), size):
            acc = 0.0
            for x in xs[i:i + size]:
                acc += x        # not sum(): it compensates on 3.12+
            partial.append(acc)
        while len(partial) > 1:
            odd = partial[-1:] if len(partial) % 2 else []
            partial = [partial[i] + partial[i + 1]
                       for i in range(0, len(partial) - 1, 2)] + odd
        self.assertEqual(results, {partial[0]})

    def test_parallel_reduce_tree_shape(self):
        # A non-associative combine shows the tree: 5 chunks of 1.
        got = grail.parallel_reduce(lambda acc, x: x, lambda a, b: (a, b),
                                    range(5), None, chunk=1, pool=self.pool)
        self.assertEqual(got, (((0, 1), (2, 3)), 4))

    def test_parallel_reduce_errors(self):
        def fold(acc, x):
            if x == 77:
                raise KeyError(x)
            return acc + x
        with self.assertRaises(KeyError):
            grail.parallel_reduce(fold, max, range(1000), 0, pool=self.pool)
        def combine(a, b):
            raise IndexError
        with self.assertRaises(IndexError):
            grail.parallel_reduce(max, combine, range(1000), 0, pool=self.pool)

    def test_helpers_nest_in_tasks(self):
        def row(y):
            return sum(grail.pmap(lambda x: x * y, range(50), chunk=5, pool=self.pool))
        self.assertEqual(grail.pmap(row, range(20), chunk=2, pool=self.pool),
                         [y * 1225 for y in range(20)])


class SharedPoolTests(unittest.TestCase):
    def test_shared_pool(self):
        self.assertIs(grail.pool(), grail.pool())
        cpus = getattr(os, "process_cpu_count", os.cpu_count)()
        self.assertEqual(grail.pool().n_threads, cpus)
        self.assertEqual(list(grail.pool().map(abs, [-1, -2])), [1, 2])

    def test_exit_with_busy_pool(self):
        proc = run_python(
            "import grail, time\n"
            "def f(_): time.sleep(0.01)\n"
            "it = grail.pool().map(f, range(10**6), buffersize=8)\n"
            "next(it)\n"
            "p = grail.Pool(2)\n"
            "it2 = p.map(f, range(10**6))\n"
            "print('ok')\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "ok\n")

    @unittest.skipUnless(hasattr(os, "fork"), "needs fork")
    def test_fork(self):
        proc = run_python(
            "import grail, os, warnings\n"
            "warnings.simplefilter('ignore', DeprecationWarning)\n"
            "list(grail.pool().map(abs, range(10)))\n"
            "p = grail.Pool(2)\n"
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    ok = list(grail.pool().map(abs, [-3])) == [3]\n"
            "    try:\n"
            "        p.map(abs, [1])\n"
            "    except RuntimeError:\n"
            "        os._exit(0 if ok else 2)\n"
            "    os._exit(1)\n"
            "_, status = os.waitpid(pid, 0)\n"
            "print(os.waitstatus_to_exitcode(status))\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "0\n")


if __name__ == "__main__":
    unittest.main()
