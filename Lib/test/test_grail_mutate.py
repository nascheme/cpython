# Copied from grail-compat tests/ by tools/sync_to_cpython_grail.py; do not edit here.
import copy
import pickle
import threading
import unittest

import grail
from grail import PMap, PVector, finish, mutate, original


@grail.record
class Point:
    x: int
    y: int


@grail.record
class Task:
    name: str
    queue: tuple[int, ...]
    done: bool = False


@grail.record
class State:
    tasks: PVector
    index: PMap
    origin: Point
    count: int = 0


def make_state():
    tasks = PVector([Task("a", ()), Task("b", (1,)), Task("c", ())])
    return State(tasks, PMap({"a": 0, "b": 1}), Point(0, 0))


class MutateRecordTest(unittest.TestCase):
    def test_fields(self):
        p = Point(1, 2)
        with mutate(p) as d:
            self.assertEqual(d.x, 1)
            d.x = 5
            d.y += 1
            self.assertEqual(d.x, 5)
        self.assertEqual(finish(d), Point(5, 3))
        self.assertEqual(p, Point(1, 2))

    def test_without_with(self):
        d = mutate(Point(1, 2))
        d.y = 0
        self.assertEqual(finish(d), Point(1, 0))

    def test_unchanged_is_original(self):
        s = make_state()
        with mutate(s) as d:
            d.tasks[1].queue           # reads create drafts but change nothing
            d.origin.x
            d.count = 0                # same object: no change
        self.assertIs(finish(d), s)

    def test_nested(self):
        s = make_state()
        with mutate(s) as d:
            d.tasks[1].queue += (2,)
            d.tasks[2].done = True
            d.origin.y = 7
            d.count += 1
        r = finish(d)
        self.assertEqual(r.tasks[1], Task("b", (1, 2)))
        self.assertEqual(r.tasks[2], Task("c", (), True))
        self.assertIs(r.tasks[0], s.tasks[0])
        self.assertIs(r.index, s.index)
        self.assertEqual(r.origin, Point(0, 7))
        self.assertEqual(r.count, 1)
        self.assertEqual(s, make_state())

    def test_same_child_draft(self):
        s = make_state()
        with mutate(s) as d:
            a = d.tasks[1]
            b = d.tasks[1]
            self.assertIs(a, b)
            a.done = True
            self.assertTrue(b.done)
        self.assertTrue(finish(d).tasks[1].done)

    def test_assign_draft(self):
        s = make_state()
        with mutate(s) as d:
            t = d.tasks[0]
            t.done = True
            d.tasks[2] = t
        r = finish(d)
        self.assertEqual(r.tasks[0], Task("a", (), True))
        self.assertIs(r.tasks[2], r.tasks[0])

    def test_assign_replaces_child_draft(self):
        s = make_state()
        with mutate(s) as d:
            d.origin.x = 3
            d.origin = Point(9, 9)
        self.assertEqual(finish(d).origin, Point(9, 9))

    def test_finish_frees_drafts(self):
        # Child drafts refer to their root; finish() must not leave cycles.
        import gc
        s = make_state()
        gc.collect()
        gc.disable()
        try:
            for _ in range(100):
                with mutate(s) as d:
                    t = d.tasks[1]
                    t.queue += (2,)
                    d.index["c"] = d.origin
                    d.origin.x = 1
                finish(d)
            del d, t
            self.assertEqual(gc.collect(), 0)
        finally:
            gc.enable()

    def test_draft_contains_itself(self):
        @grail.record
        class Box:
            item: object

        d = mutate(Box(1))
        d.item = d
        with self.assertRaises(ValueError):
            finish(d)
        d = mutate(Box(1))
        d.item = [d]               # not drafted: the list is stored as is
        self.assertIs(type(finish(d).item[0]), type(d))

    def test_unknown_field(self):
        with mutate(Point(1, 2)) as d:
            with self.assertRaises(AttributeError):
                d.z
            with self.assertRaises(AttributeError):
                d.z = 1
            with self.assertRaises(AttributeError):
                del d.x
        self.assertEqual(finish(d), Point(1, 2))

    def test_original(self):
        s = make_state()
        with mutate(s) as d:
            d.count = 4
            self.assertIs(original(d), s)
            self.assertIs(original(d.tasks), s.tasks)
        self.assertEqual(finish(d).count, 4)

    def test_draft_is_not_the_value(self):
        with mutate(Point(1, 2)) as d:
            self.assertNotIsInstance(d, Point)
            self.assertNotEqual(d, Point(1, 2))
            with self.assertRaises(TypeError):
                hash(d)
            with self.assertRaises(TypeError):
                copy.copy(d)
            with self.assertRaises(TypeError):
                pickle.dumps(d)


class MutateCollectionTest(unittest.TestCase):
    def test_vector(self):
        v = PVector([Point(0, 0), 1, 2])
        with mutate(v) as d:
            self.assertEqual(len(d), 3)
            d[0].x = 1
            d[-1] = 20
            d.append(Point(5, 5))
            d[3].y = 6
            d.extend([7, 8])
            self.assertEqual(len(d), 6)
            with self.assertRaises(IndexError):
                d[6]
            with self.assertRaises(TypeError):
                d["0"]
        self.assertEqual(finish(d), PVector([Point(1, 0), 1, 20, Point(5, 6), 7, 8]))
        self.assertEqual(v, PVector([Point(0, 0), 1, 2]))

    def test_vector_many_changes(self):
        v = PVector([Point(i, 0) for i in range(100)] + list(range(100)))
        with mutate(v) as d:
            for i in range(0, 100, 2):
                d[i].y = i              # element drafts
            for i in range(100, 200, 3):
                d[i] = -i               # assignments
            d[4].y = 40                 # an existing change, after many
            d[1] = d[1]                 # its own draft: no change
            d.append(Point(7, 7))
            d[-1].x = 8
        r = finish(d)
        expected = ([Point(i, i if i % 2 == 0 else 0) for i in range(100)]
                    + [-i if (i - 100) % 3 == 0 else i - 100 for i in range(100, 200)]
                    + [Point(8, 7)])
        expected[4] = Point(4, 40)
        self.assertEqual(list(r), expected)
        for i in range(1, 100, 2):
            self.assertIs(r[i], v[i])
        self.assertEqual(v[0], Point(0, 0))

    def test_vector_few_changes(self):
        for n in (3, 33, 100, 1100):
            v = PVector(range(n))
            for indices in ((n - 1,), (0, n - 1), (n - 1, n - 2, 0), (1, 1), (0, n // 2)):
                with mutate(v) as d:
                    for k, i in enumerate(indices):
                        d[i] = -k - 1
                expected = list(range(n))
                for k, i in enumerate(indices):
                    expected[i] = -k - 1
                self.assertEqual(list(finish(d)), expected)
            self.assertEqual(list(v), list(range(n)))

    def test_map(self):
        m = PMap({"a": Point(0, 0), "b": 1})
        with mutate(m) as d:
            d["a"].x = 1
            d["c"] = 3
            del d["b"]
            self.assertNotIn("b", d)
            self.assertIn("c", d)
            self.assertEqual(d.get("b", 9), 9)
            self.assertEqual(len(d), 2)
            with self.assertRaises(KeyError):
                d["b"]
            with self.assertRaises(KeyError):
                del d["b"]
        self.assertEqual(finish(d), PMap({"a": Point(1, 0), "c": 3}))

    def test_map_unchanged(self):
        m = PMap({"a": 1})
        with mutate(m) as d:
            d["a"] = 1
        self.assertIs(finish(d), m)


class MutateLifecycleTest(unittest.TestCase):
    def test_not_draftable(self):
        for value in (1, (1, 2), [], {}):
            with self.assertRaises(TypeError):
                mutate(value)
        d = mutate(Point(1, 2))
        with self.assertRaises(TypeError):
            mutate(d)
        with self.assertRaises(TypeError):
            finish(Point(1, 2))

    def test_sealed_after_with(self):
        with mutate(Point(1, 2)) as d:
            d.x = 3
        with self.assertRaises(RuntimeError):
            d.x
        with self.assertRaises(RuntimeError):
            d.x = 4
        self.assertEqual(finish(d), Point(3, 2))
        self.assertIs(finish(d), finish(d))

    def test_closed_after_finish(self):
        d = mutate(Point(1, 2))
        r = finish(d)
        with self.assertRaises(RuntimeError):
            d.x = 1
        self.assertIs(finish(d), r)

    def test_abort(self):
        with self.assertRaises(ZeroDivisionError):
            with mutate(Point(1, 2)) as d:
                d.x = 3
                1 / 0
        with self.assertRaises(RuntimeError):
            finish(d)

    def test_child_draft(self):
        s = make_state()
        d = mutate(s)
        t = d.tasks[0]
        with self.assertRaises(RuntimeError):
            finish(t)
        with self.assertRaises(RuntimeError):
            with t:
                pass
        child = t
        finish(d)
        with self.assertRaises(RuntimeError):
            child.done = True

    def test_foreign_draft(self):
        a = mutate(make_state())
        b = mutate(make_state())
        with self.assertRaises(ValueError):
            a.tasks[0] = b.tasks[0]

    def test_other_thread(self):
        d = mutate(Point(1, 2))
        errors = []

        def use():
            for op in (lambda: d.x, lambda: setattr(d, "x", 1), lambda: finish(d)):
                try:
                    op()
                except RuntimeError:
                    errors.append(True)

        t = threading.Thread(target=use)
        t.start()
        t.join()
        self.assertEqual(errors, [True] * 3)
        self.assertEqual(finish(d), Point(1, 2))


if __name__ == "__main__":
    unittest.main()
