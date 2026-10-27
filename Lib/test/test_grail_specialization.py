"""Tier-one record field loads, including the mutable-slot ablation."""
import concurrent.futures
import dis
import gc
import os
import sysconfig
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


if __name__ == "__main__":
    unittest.main()
