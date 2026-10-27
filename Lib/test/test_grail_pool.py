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
