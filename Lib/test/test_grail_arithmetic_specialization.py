"""Builtin arithmetic specializations alongside central registration."""
import sysconfig
import unittest

from test.support import requires_specialization

@requires_specialization
class ArithmeticSpecializationTests(unittest.TestCase):
    def run_isolated(self, code):
        import _interpreters
        ident = _interpreters.create()
        try:
            result = _interpreters.exec(ident, '''
import dis, grail, os, sys
import sysconfig
assert not (sysconfig.get_config_var("Py_GIL_DISABLED") and sys._is_gil_enabled())
def warm(f, a, b):
    for _ in range(1000):
        f(a, b)
inline = os.environ.get("PYTHON_GRAIL_INLINE_OPERATORS") != "0"
def specialized(f):
    """A builtin fast path that would bypass the registry."""
    return any(i.opname.startswith("BINARY_OP_") and i.opname != "BINARY_OP_GRAIL"
               for i in dis.get_instructions(f, adaptive=True))
def grail_specialized(f):
    return any(i.opname == "BINARY_OP_GRAIL"
               for i in dis.get_instructions(f, adaptive=True))
''' + code)
            self.assertIsNone(result)
        finally:
            _interpreters.destroy(ident)

    def test_builtin_pairs_rejected(self):
        self.run_isolated('''
pairs = [(int, int), (float, float), (int, float), (float, int), (str, str),
         (str, int), (int, str), (bytes, bytes), (list, list), (tuple, tuple),
         (tuple, int), (dict, dict), (complex, complex), (object, object)]
for registry in (grail.ops.add, grail.ops.sub, grail.ops.mul, grail.ops.truediv):
    for pair in pairs:
        for method in (registry.register, registry.override):
            try:
                method(pair, lambda a, b: "registered")
            except TypeError as e:
                assert "two built-in types" in str(e), e
            else:
                raise AssertionError(pair)
assert 1 + 2 == 3 and 1.5 * 2 == 3.0 and "a" + "b" == "ab"
''')

    def test_unrelated_pairs_keep_builtin_fast_paths(self):
        self.run_isolated('''
@grail.record
class R:
    x: object
for registry in (grail.ops.add, grail.ops.sub, grail.ops.mul, grail.ops.truediv):
    registry.register((R, R), lambda a, b: NotImplemented)
    registry.register((R, int), lambda a, b: NotImplemented)
    registry.register((float, R), lambda a, b: NotImplemented)
cases = [("a + b", 12345, 23456), ("a - b", 12345, 23456),
         ("a * b", 12345, 3), ("a + b", 1.25, 2.5),
         ("a - b", 1.25, 2.5), ("a * b", 1.25, 2.5),
         ("a + b", "abc", "def"), ("a + b", 1.25, 3),
         ("a + b", 3, 1.25), ("a / b", 1.25, 3),
         ("a / b", 3, 1.25), ("a + b", [1], [2]),
         ("a + b", (1,), (2,)), ("a + b", b"a", b"b"),
         ("a * b", "abc", 3), ("a * b", 3, "abc"),
         ("a * b", b"abc", 3), ("a * b", 3, b"abc"),
         ("a * b", (1,), 3), ("a * b", 3, (1,)),
         ("a | b", 3, 4), ("a | b", {1: 2}, {3: 4})]
for expr, a, b in cases:
    f = eval("lambda a, b: " + expr)
    warm(f, a, b)
    assert specialized(f), (expr, type(a), type(b))
''')

    def test_heap_types_with_builtins_reach_registry(self):
        self.run_isolated('''
# Builtin kernels require exact builtin types, so a pair with a subclass or
# other heap type reaches the registry even at a site warmed on builtins.
class MyInt(int): pass
class MyList(list): pass
marker = object()
cases = [("+", grail.ops.add, MyInt(5), 3), ("+", grail.ops.add, 3, MyInt(5)),
         ("-", grail.ops.sub, MyInt(5), 3), ("*", grail.ops.mul, MyInt(5), 3),
         ("/", grail.ops.truediv, 3.5, MyInt(5)),
         ("+", grail.ops.add, MyList([1]), [2])]
for op, registry, a, b in cases:
    ns = {}
    exec("def binary(a, b): return a " + op + " b\\n"
         "def inplace(a, b):\\n a " + op + "= b\\n return a", ns)
    binary, inplace = ns["binary"], ns["inplace"]
    builtin_a = list(a) if isinstance(a, list) else type(a).__mro__[-2](a)
    builtin_b = list(b) if isinstance(b, list) else type(b).__mro__[-2](b)
    warm(binary, builtin_a, builtin_b)
    assert specialized(binary), (op, a, b)
    registry.register((type(a), type(b)), lambda x, y: marker)
    for f in (binary, inplace):
        assert f(a, b) is marker, (op, a, b, f)
    # The builtin pair keeps its result at the same site.
    assert binary(builtin_a, builtin_b) == (
        builtin_a + builtin_b if op == "+" else
        builtin_a - builtin_b if op == "-" else
        builtin_a * builtin_b if op == "*" else builtin_a / builtin_b)
    registry.override((type(a), type(b)), lambda x, y: NotImplemented)
    assert binary(a, b) is NotImplemented
''')

    def test_concurrent_late_registration(self):
        self.run_isolated('''
import concurrent.futures, threading
class R:
    def __mul__(self, other): return "plain"
ready = threading.Barrier(3)
published = threading.Event()
marker = object()
r = R()
def f(a, b): return a * b
def reader():
    warm(f, r, 3)
    ready.wait()
    while not published.is_set():
        result = f(r, 3)
        assert result == "plain" or result is marker
    for _ in range(1000):
        assert f(r, 3) is marker
with concurrent.futures.ThreadPoolExecutor(2) as pool:
    futures = [pool.submit(reader) for _ in range(2)]
    ready.wait()
    try:
        grail.ops.mul.register((R, int), lambda a, b: marker)
    finally:
        published.set()
    for future in futures:
        future.result()
''')

    def test_interpreter_isolation(self):
        code = '''
class R:
    pass
if REGISTER:
    grail.ops.add.register((R, float), lambda a, b: "registered")
    assert R() + 2.5 == "registered"
else:
    try:
        R() + 2.5
    except TypeError:
        pass
    else:
        raise AssertionError("registration leaked between interpreters")
'''
        self.run_isolated("REGISTER = True\n" + code)
        self.run_isolated("REGISTER = False\n" + code)


if __name__ == "__main__":
    unittest.main()
