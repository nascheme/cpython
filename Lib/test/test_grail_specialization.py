"""Tier-one record field loads, including the mutable-slot ablation."""
import concurrent.futures
import dis
import gc
import os
import sysconfig
import threading
import unittest
import weakref

from test.support import requires_specialization
from test.support.script_helper import assert_python_ok

# Subprocesses keep the GIL disabled on free-threaded builds.
GIL_ARGS = ("-X", "gil=0") if sysconfig.get_config_var("Py_GIL_DISABLED") else ()

import grail


@requires_specialization
class RecordLoadTests(unittest.TestCase):
    def warm(self, read, obj):
        for _ in range(1000):
            read(obj)
        expected = ("LOAD_ATTR_SLOT" if os.environ.get("PYTHON_GRAIL_RECORD_LOADS") == "0"
                    else "LOAD_ATTR_RECORD")
        self.assertIn(expected, {i.opname for i in
                                dis.get_instructions(read, adaptive=True)})

    def test_field_kinds(self):
        @grail.record
        class R:
            x: float
            y: object

        def read(r):
            return r.x, r.y

        child = []
        for obj in (R(1.5, child), R(child, None)):
            self.warm(read, obj)
            self.assertIs(read(obj)[0], obj.x)
            self.assertIs(read(obj)[1], obj.y)
        child.append(42)
        self.assertEqual(read(R(child, child)), ([42], [42]))

    def test_callable_field(self):
        @grail.record
        class R:
            x: object

        def invoke(r):
            return r.x(7)

        obj = R(lambda n: n + 1)
        self.warm(invoke, obj)
        self.assertEqual(invoke(obj), 8)

    def test_temporary_owner_result_lifetime(self):
        @grail.record
        class R:
            x: object

        class Child:
            pass

        def read(r):
            return r.x

        self.warm(read, R(None))
        child = read(R(Child()))
        ref = weakref.ref(child)
        gc.collect()
        self.assertIs(ref(), child)
        del child
        gc.collect()
        self.assertIsNone(ref())

    def test_guard_and_type_lifetime(self):
        def read(r):
            return r.x

        @grail.record
        class R:
            x: object

        self.warm(read, R(1))
        ref = weakref.ref(R)
        del R
        gc.collect()
        self.assertIsNone(ref())  # The inline cache must not retain the type.

        class Ordinary:
            __slots__ = ("padding", "x")

        obj = Ordinary()
        obj.x = 23
        self.assertEqual(read(obj), 23)
        del obj.x
        with self.assertRaises(AttributeError):
            read(obj)
        # Repeated ephemeral record types and different offsets exercise stale
        # scalar cache guards without dereferencing a dead type/layout pointer.
        for n in range(100):
            @grail.record
            class Other:
                padding: object
                x: object
            self.assertEqual(read(Other(None, n)), n)
        with self.assertRaises(AttributeError):
            read(object())

    def test_shared_children(self):
        @grail.record
        class R:
            x: object

        class Child:
            pass

        child = Child()
        obj = R(child)

        def read(r):
            return r.x

        self.warm(read, obj)

        def worker(_):
            # dis exposes the main bytecode copy, not this thread's TLBC.
            # Repetition warms each worker's copy without inspecting it.
            for _ in range(10000):
                if read(obj) is not child:
                    raise AssertionError("field identity changed")

        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            list(pool.map(worker, range(4)))

    def test_cycle_collection(self):
        @grail.record
        class R:
            x: object

        class Child(list):
            pass

        def read(r):
            return r.x

        child = Child()
        obj = R(child)
        child.append(obj)
        self.warm(read, obj)
        ref = weakref.ref(child)
        del obj, child
        gc.collect()
        self.assertIsNone(ref())

    def test_subinterpreter(self):
        import _interpreters
        ident = _interpreters.create()
        try:
            self.assertIsNone(_interpreters.exec(ident, '''
import grail
@grail.record
class R:
    x: object
def read(r):
    return r.x
r = R([])
for _ in range(1000):
    assert read(r) is r.x
'''))
        finally:
            _interpreters.destroy(ident)

    def test_ablation_subprocess(self):
        code = '''
import dis, grail, os, sys
import sysconfig
assert not (sysconfig.get_config_var("Py_GIL_DISABLED") and sys._is_gil_enabled())
@grail.record
class R:
    x: object
def read(r):
    return r.x
r = R([])
for _ in range(1000):
    assert read(r) is r.x
expected = "LOAD_ATTR_RECORD" if os.environ["PYTHON_GRAIL_RECORD_LOADS"] == "1" else "LOAD_ATTR_SLOT"
assert expected in {i.opname for i in dis.get_instructions(read, adaptive=True)}
'''
        for setting in ("0", "1"):
            assert_python_ok(*GIL_ARGS, "-c", code,
                             PYTHON_GRAIL_RECORD_LOADS=setting)


@requires_specialization
class VectorSubscriptTests(unittest.TestCase):
    def test_int_subscript(self):
        def get(v, i):
            return v[i]

        v = grail.PVector(range(40))
        for _ in range(1000):
            get(v, 3)
        self.assertIn("BINARY_OP_EXTEND",
                      {i.opname for i in dis.get_instructions(get, adaptive=True)})
        self.assertEqual([get(v, i) for i in (0, 31, 32, 39, -1, -40)],
                         [0, 31, 32, 39, 39, 0])
        for i in (40, -41, 2**100):
            with self.assertRaises(IndexError):
                get(v, i)
        self.assertEqual(get(v, slice(1, 3)), grail.PVector([1, 2]))
        self.assertEqual(get((5, 6), 1), 6)
        self.assertEqual(get(grail.PVector(), slice(None)), grail.PVector())
        with self.assertRaises(IndexError):
            get(grail.PVector(), 0)


@requires_specialization
class RecordDraftAttrTests(unittest.TestCase):
    def make(self):
        @grail.record
        class Inner:
            v: int

        @grail.record
        class R:
            x: int
            inner: Inner
            obj: object
        return Inner, R

    def functions(self):
        # Fresh code objects, so that repeated runs (-R) start unspecialized.
        ns = {}
        exec("def bump(d):\n    d.x += 1\n    return d.x\n"
             "def inner(d):\n    return d.inner\n"
             "def put(d, value):\n    d.obj = value\n", ns)
        return ns["bump"], ns["inner"], ns["put"]

    def test_specialized(self):
        Inner, R = self.make()
        bump, inner, put = self.functions()
        r = R(1, Inner(2), [])
        for _ in range(1000):
            d = grail.mutate(r)
            self.assertEqual(bump(d), 2)
            child = inner(d)
            self.assertIs(inner(d), child)
            child.v = 5
            put(d, "s")
            self.assertEqual(grail.finish(d), R(2, Inner(5), "s"))
        names = set()
        for func in (bump, inner, put):
            names |= {i.opname for i in dis.get_instructions(func, adaptive=True)}
        self.assertIn("LOAD_ATTR_RECORD_DRAFT", names)
        self.assertIn("STORE_ATTR_RECORD_DRAFT", names)
        self.assertEqual(r, R(1, Inner(2), []))

        # Specialized sites still check the draft's state, owner and type.
        d = grail.mutate(r)
        with d:
            pass
        with self.assertRaises(RuntimeError):
            bump(d)
        with self.assertRaises(RuntimeError):
            put(d, 1)
        d = grail.mutate(r)
        other = grail.mutate(r)
        with self.assertRaises(ValueError):
            put(d, other.inner)
        errors = []

        def use():
            for f in (lambda: bump(d), lambda: put(d, 1)):
                try:
                    f()
                except RuntimeError:
                    errors.append(True)

        t = threading.Thread(target=use)
        t.start()
        t.join()
        self.assertEqual(errors, [True, True])
        self.assertEqual(grail.finish(d), r)

        # Another record type with the same field names, and non-drafts.
        @grail.record
        class S:
            obj: object
            x: int
            inner: object
        d = grail.mutate(S(None, 10, 3))
        self.assertEqual(bump(d), 11)
        self.assertEqual(inner(d), 3)
        put(d, 4)
        self.assertEqual(grail.finish(d), S(4, 11, 3))

        class Plain:
            pass
        p = Plain()
        p.x, p.inner = 7, 8
        self.assertEqual(bump(p), 8)
        self.assertEqual(inner(p), 8)
        put(p, 9)
        self.assertEqual(p.obj, 9)


@requires_specialization
class VectorDraftSubscriptTests(unittest.TestCase):
    def test_int_subscript(self):
        ns = {}
        exec("def get(d, i):\n    return d[i]\n", ns)  # fresh code object
        get = ns["get"]
        v = grail.PVector(range(40))
        for _ in range(1000):
            with grail.mutate(v) as d:
                get(d, 3)
        self.assertIn("BINARY_OP_EXTEND",
                      {i.opname for i in dis.get_instructions(get, adaptive=True)})
        d = grail.mutate(v)
        d.append(40)
        self.assertEqual([get(d, i) for i in (0, 39, 40, -1, -41)], [0, 39, 40, 40, 0])
        for i in (41, -42):
            with self.assertRaises(IndexError):
                get(d, i)
        grail.finish(d)
        with self.assertRaises(RuntimeError):
            get(d, 0)
        self.assertEqual(get(v, 5), 5)
        self.assertEqual(get((5, 6), 1), 6)


if __name__ == "__main__":
    unittest.main()
