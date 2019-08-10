from test import support
import gc
import re
import registervm
import sys
import types
import unittest

def unary_not(x):
    x = not x
    return x

def noop():
    return

def loop_sum(n):
    total = 0
    for i in range(n):
        total += i
    return total

def fact(n):
    if n < 2:
        return 1
    else:
        return n * fact(n-1)

def fact_loop(n):
    f = 1
    for i in range(2, n+1):
        f *= i
    return f

def pow2(n):
    x = 1
    for i in range(n):
        x *= 2
    return x

def bisect(a, x):
    lo = 0
    hi = len(a)
    while lo < hi:
        mid = (lo+hi)//2
        if x < a[mid]:
            hi = mid
        else:
            lo = mid+1
    return lo

def sieve(n):
    primes = list(range(2, n+1))
    for i in primes:
        j = 2
        while i * j <= primes[-1]:
            if i * j in primes:
                primes.remove(i*j)
            j += 1
    return primes

def merge_load():
    lst = []
    lst.append(len("a"))
    lst.append(len("a"))
    lst.append(len("a"))
    return lst

def clear_reg(value):
    lst = ()
    # useless instruction
    lst[:]
    return lst

def partition(sequence, l, e, g):
    if not sequence:
        return (l, e, g)
    else:
        head = sequence[0]
        if head < e[0]:
            return partition(sequence[1:], l + [head], e, g)
        elif head > e[0]:
            return partition(sequence[1:], l, e, g + [head])
        else:
            return partition(sequence[1:], l, e + [head], g)

def qsort2(sequence):
    if not sequence:
        return []
    else:
        pivot = sequence[0]
        lesser, equal, greater = partition(sequence[1:], [], [pivot], [])
        return qsort2(lesser) + equal + qsort2(greater)

def make_func():
    def f(a, *b, c=5):
        return (a, b, c)
    return f("a", "b1", "b2")

def store_map():
    mapping = {"key1": 1, "key2": 2}
    return mapping

def store_subscr():
    d = {}
    d["key"] = "value"
    return d

def build_list_tuple():
    x = 1
    y = 2
    l = [x, y]
    return (x, y, l)

def delete_subscr():
    numbers = [1, 2, 3, 4, 5]
    del numbers[1:3]
    return numbers

def loop_concat():
    s = 'om'
    for i in range(3):
        s = s + 'xbx'
    return s

def dictcomp():
    return {x:x for x in "abc"}

def yield_value():
    x = 3
    yield x

def yield_use_result():
    x = 3
    y = yield x
    yield y

def getline(lines, lineno):
    if 1 <= lineno <= len(lines):
        return lines[lineno-1]
    else:
        return ''

def move_instr():
    x = "x"
    try:
        y = x.unknown_attr + 1
    except AttributeError:
        y = 1

def loop_compare_str():
    t = "t"
    s = "s"
    for loop in range(3):
        # useless instruction
        t >= s
    return True

def set_attr(obj):
    obj.attr1 += 1
    obj.attr2 = 3

def get_loader(module_or_name):
    # regression test extracted from the pkgutil module of Python 3.4
    if module_or_name:
        loader = getattr(module_or_name, '__loader__', None)
        fullname = module_or_name.__name__
    else:
        fullname = module_or_name
    return find_loader(fullname)

def find_loader(name, path=None):
    try:
        loader = sys.modules[name].__loader__
        if loader is None:
            raise ValueError('{}.__loader__ is None'.format(name))
        else:
            return loader
    except KeyError:
        pass
    return _bootstrap._find_module(name, path)

def store_load_global():
    global global_var
    global_var = 5
    return global_var
global_var = None

class OptimizeTests(unittest.TestCase):
    maxDiff = 80*80

    def setUp(self):
        # enable most optimizations
        self.config = registervm.Config()
        self.config.quiet = True
        self.config.enable_unsafe_optimizations()
        self.config.enable_buggy_optimizations()

    def optimize_code(self, code):
        converter = registervm.Converter(code, self.config)
        converter.convert()
        code = converter.compile()
        return converter, code

    def optimize_func(self, func):
        if isinstance(func, types.MethodType):
            func = func.__func__
        converter, code = self.optimize_code(func.__code__)
        func.__code__ = code
        return converter

    def optimize_expr(self, code_str):
        code = compile(code_str, "<string>", "exec")
        return self.optimize_code(code)

    def get_bytecode(self, converter):
        text = '\n'.join(str(instr) for instr in converter.instructions)
        return re.sub('at 0x[a-f0-9]+, file "[^"]+", line [0-9]+',
                      'at (...)',
                      text)

    def check_bytecode(self, code, expected):
        """
        Check the formatted code: expected can contain "(...)" which matches
        any string.
        """
        expected = expected.strip()
        text = self.get_bytecode(code)
        self.assertEqual(expected, text)

    def can_reoptimize(self, code):
        # Optimize a function twice must not fail
        if isinstance(code, types.CodeType):
            code_obj = code
        else:
            code_obj = code.__code__
        converter = registervm.Converter(code_obj, self.config)
        before = self.get_bytecode(converter)

        if isinstance(code, types.CodeType):
            converter2, code2 = self.optimize_code(code)
        else:
            converter2 = self.optimize_func(code)
        after = self.get_bytecode(converter2)
        self.assertEqual(before, after)

    def check_exec(self, expected, func, *args, is_generator=False, reoptimize=True):
        # FIXME: always reoptimize
        if reoptimize:
            self.can_reoptimize(func)

        if is_generator:
            generator = func
            def read_generator():
                return list(generator(*args))
            func = read_generator
            args = ()
        result = func(*args)
        self.assertEqual(result, expected)
        if hasattr(sys, "gettotalrefcount"):
            for prepare in range(50):
                func(*args)
                gc.collect()

            before = None
            before = sys.gettotalrefcount()
            for repeat in range(3):
                func(*args)
                gc.collect()
            gc.collect()
            del repeat
            leak = sys.gettotalrefcount() - before
            if leak:
                self.fail("reference leak: %s" % leak)

    def exec_code(self, code, reoptimize=False):
        # FIXME: always reoptimize
        if reoptimize:
            self.can_reoptimize(code)

        ns = {}
        exec(code, ns, ns)
        return ns

    def check_expr(self, varname, expected, code):
        ns = self.exec_code(code)
        value = ns[varname]
        self.assertEqual(value, expected)

    def test_unary_not(self):
        converter = self.optimize_func(unary_not)
        self.check_bytecode(converter, """
UNARY_NOT_REG 'x', 'x'
RETURN_VALUE_REG 'x'
        """)
        self.check_exec(False, unary_not, True)

    def test_store_map(self):
        converter = self.optimize_func(store_map)
        self.check_bytecode(converter, """
BUILD_MAP_REG 'mapping', 2
LOAD_CONST_REG R0, 1 (const#1)
LOAD_CONST_REG R1, 'key1' (const#2)
STORE_MAP_REG 'mapping', R1, R0
LOAD_CONST_REG R0, 2 (const#3)
LOAD_CONST_REG R1, 'key2' (const#4)
STORE_MAP_REG 'mapping', R1, R0
RETURN_VALUE_REG 'mapping'
        """)
        self.check_exec({'key1': 1, 'key2': 2}, store_map)

    def test_store_subscr(self):
        converter = self.optimize_func(store_subscr)
        self.check_bytecode(converter, """
BUILD_MAP_REG 'd', 0
LOAD_CONST_REG R0, 'value' (const#1)
LOAD_CONST_REG R1, 'key' (const#2)
STORE_SUBSCR_REG 'd', R1, R0
RETURN_VALUE_REG 'd'
        """)
        self.check_exec({'key': 'value'}, store_subscr)

    def test_build_list_tuple(self):
        converter = self.optimize_func(build_list_tuple)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'x', 1 (const#1)
LOAD_CONST_REG 'y', 2 (const#2)
BUILD_LIST_REG 'l', 2, 'x', 'y'
BUILD_TUPLE_REG R0, 3, 'x', 'y', 'l'
RETURN_VALUE_REG R0
        """)
        self.check_exec((1, 2, [1, 2]), build_list_tuple)

    def test_delete_subscr(self):
        converter = self.optimize_func(delete_subscr)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, 1 (const#1)
LOAD_CONST_REG R1, 2 (const#2)
LOAD_CONST_REG R2, 3 (const#3)
LOAD_CONST_REG R3, 4 (const#4)
LOAD_CONST_REG R4, 5 (const#5)
BUILD_LIST_REG 'numbers', 5, R0, R1, R2, R3, R4
CLEAR_REG R1
CLEAR_REG R3
CLEAR_REG R4
BUILD_SLICE2_REG R0, R0, R2
CLEAR_REG R2
DELETE_SUBSCR_REG 'numbers', R0
RETURN_VALUE_REG 'numbers'
        """)
        # FIXME: reoptimize reuses R3 for "LOAD_CONST_REG R2, 3 (const#3)"
        self.check_exec([1, 4, 5], delete_subscr, reoptimize=False)

    def test_loop_concat(self):
        converter = self.optimize_func(loop_concat)
        self.check_bytecode(converter, """
LOAD_CONST_REG 's', 'om' (const#1)
SETUP_LOOP <relative jump +56>
LOAD_GLOBAL_REG R0, 'range' (name#0)
LOAD_CONST_REG R1, 3 (const#2)
LOAD_CONST_REG R2, 'xbx' (const#3)
CALL_FUNCTION_REG R0, R0, (1 positional), R1
CLEAR_REG R1
GET_ITER_REG R0, R0
FOR_ITER_REG 'i', R0, <relative jump +10>
BINARY_ADD_REG 's', 's', R2
JUMP_ABSOLUTE 40
CLEAR_REG R2
CLEAR_REG R0
POP_BLOCK
RETURN_VALUE_REG 's'
        """)
        self.check_exec("omxbxxbxxbx", loop_concat, reoptimize=False)

    # FIXME: fix MAP_ADD_REG and stack tracker
    @unittest.skipIf(True, "FIXME")
    def test_dictcomp(self):
        converter = self.optimize_func(dictcomp)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, <code object <dictcomp> at (...)> (const#1)
LOAD_CONST_REG R1, 'dictcomp.<locals>.<dictcomp>' (const#2)
MAKE_FUNCTION_REG R1, R1, R0, 0, 0, 0
LOAD_CONST_REG R0, 'abc' (const#3)
GET_ITER_REG R0, R0
CALL_FUNCTION_REG R1, R1, (1 positional), R0
RETURN_VALUE_REG R1
        """)

        func_code = dictcomp.__code__
        const_index = 1
        converter2, code2 = self.optimize_code(func_code.co_consts[const_index])
        self.check_bytecode(converter2, """
BUILD_MAP_REG R0, 0
PUSH_REG R0
CLEAR_REG R0
FOR_ITER_REG 'x', '.0', <relative jump +10>
MAP_ADD_REG R0, 'x', 'x'
JUMP_ABSOLUTE 11
RETURN_VALUE
        """)
        dictcomp.__code__ = registervm.patch_code_obj(func_code, const={const_index: code2})

        self.check_exec({"a": "a", "b": "b", "c": "c"}, dictcomp, reoptimize=False)

    def test_yield_value(self):
        converter = self.optimize_func(yield_value)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'x', 3 (const#1)
YIELD_REG 'x'
POP_TOP
LOAD_CONST_REG R0, None (const#0)
RETURN_VALUE_REG R0
        """)

        self.check_exec([3], yield_value, is_generator=True)

    def test_yield_use_result(self):
        converter = self.optimize_func(yield_use_result)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'x', 3 (const#1)
YIELD_REG 'x'
POP_REG 'y'
YIELD_REG 'y'
POP_TOP
LOAD_CONST_REG R0, None (const#0)
RETURN_VALUE_REG R0
        """)

        self.check_exec([3, None], yield_use_result, is_generator=True)

    def test_getline(self):
        converter = self.optimize_func(getline)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, 1 (const#1)
PUSH_REG 'lineno'
COMPARE_REG R0, '<=', R0, 'lineno'
JUMP_IF_FALSE_REG R0, 48
LOAD_GLOBAL_REG R1, 'len' (name#0)
CALL_FUNCTION_REG R1, R1, (1 positional), 'lines'
PUSH_REG R1
CLEAR_REG R1
COMPARE_OP '<='
JUMP_FORWARD <relative jump +4>
POP_REG R0
POP_TOP
JUMP_IF_FALSE_REG R0, 79
LOAD_CONST_REG R2, 1 (const#1)
BINARY_SUBTRACT_REG R2, 'lineno', R2
BINARY_SUBSCR_REG R2, 'lines', R2
RETURN_VALUE_REG R2
LOAD_CONST_REG R0, '' (const#2)
RETURN_VALUE_REG R0
        """)
        self.check_exec("b", getline, ["a", "b", "c"], 2, reoptimize=False)

    def test_move_instr(self):
        # LOAD_ATTR_REG must not be moved outside the try block
        # LOAD_GLOBAL_REG must not be moved outside the except block
        converter = self.optimize_func(move_instr)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'x', 'x' (const#1)
SETUP_EXCEPT <relative jump +29>
LOAD_ATTR_REG R0, 'x', 'unknown_attr' (name#0)
LOAD_CONST_REG R1, 1 (const#2)
BINARY_ADD_REG 'y', R0, R1
CLEAR_REG R0
CLEAR_REG R1
POP_BLOCK
JUMP_FORWARD <relative jump +53>
POP_REG R2
PUSH_REG R2
LOAD_GLOBAL_REG R3, 'AttributeError' (name#1)
COMPARE_REG R2, 'exception match', R2, R3
CLEAR_REG R3
JUMP_IF_FALSE_REG R2, 86
POP_TOP
POP_TOP
POP_TOP
LOAD_CONST_REG R4, 1 (const#2)
PUSH_REG R4
CLEAR_REG R4
POP_REG 'y'
POP_EXCEPT
JUMP_FORWARD <relative jump +4>
CLEAR_REG R2
END_FINALLY
LOAD_CONST_REG R5, None (const#0)
RETURN_VALUE_REG R5
        """)
        self.check_exec(None, move_instr)

    def test_list_append(self):
        converter, code = self.optimize_expr("""
x = []
for i in range(30):
    x.append(i)
        """)
        self.check_bytecode(converter, """
BUILD_LIST_REG R0, 0
STORE_NAME_REG 'x' (name#0), R0
CLEAR_REG R0
SETUP_LOOP <relative jump +71>
LOAD_NAME_REG R1, 'range' (name#1)
LOAD_CONST_REG R2, 30 (const#0)
CALL_FUNCTION_REG R1, R1, (1 positional), R2
CLEAR_REG R2
GET_ITER_REG R1, R1
FOR_ITER_REG R3, R1, <relative jump +33>
STORE_NAME_REG 'i' (name#2), R3
LOAD_NAME_REG R4, 'x' (name#0)
LOAD_ATTR_REG R4, R4, 'append' (name#3)
CALL_PROCEDURE_REG R4, (1 positional), R3
CLEAR_REG R4
CLEAR_REG R3
JUMP_ABSOLUTE 43
CLEAR_REG R1
POP_BLOCK
LOAD_CONST_REG R5, None (const#1)
RETURN_VALUE_REG R5
        """)
        self.check_expr('x', list(range(30)), code)

    def test_store_load_name(self):
        converter, code = self.optimize_expr("""
def f():
    pass
f()
        """)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, <code object f at (...)> (const#0)
LOAD_CONST_REG R1, 'f' (const#1)
MAKE_FUNCTION_REG R1, R1, R0, 0, 0, 0
STORE_NAME_REG 'f' (name#0), R1
CALL_PROCEDURE_REG R1, (0 positional)
CLEAR_REG R1
LOAD_CONST_REG R0, None (const#2)
RETURN_VALUE_REG R0
        """)
        self.exec_code(code, reoptimize=False)

    # FIXME: LIST_APPEND and stack tracker
    @unittest.skipIf(True, "FIXME")
    def test_gen_list_append(self):
        converter, code = self.optimize_expr("""
x = [i for i in range(30)]
        """)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, <code object <listcomp> at (...)> (const#0)
LOAD_CONST_REG R1, '<listcomp>' (const#1)
MAKE_FUNCTION_REG R1, R1, R0, 0, 0, 0
LOAD_NAME_REG R2, 'range' (name#0)
LOAD_CONST_REG R0, 30 (const#2)
CALL_FUNCTION_REG R2, R2, (1 positional), R0
CLEAR_REG R0
GET_ITER_REG R2, R2
CALL_FUNCTION_REG R1, R1, (1 positional), R2
CLEAR_REG R2
PUSH_REG R1
CLEAR_REG R1
STORE_NAME 'x' (name#1)
LOAD_CONST None (const#3)
RETURN_VALUE
        """)

        converter2, code2 = self.optimize_code(code.co_consts[0])
        self.check_bytecode(converter2, """
BUILD_LIST_REG R0, 0
PUSH_REG R0
CLEAR_REG R0
FOR_ITER_REG 'i', '.0', <relative jump +8>
LIST_APPEND_REG R0, 'i'
JUMP_ABSOLUTE 11
RETURN_VALUE
        """)
        code = registervm.patch_code_obj(code, const={0: code2})

        self.check_expr('x', list(range(30)), code)

    def test_return_none(self):
        converter = self.optimize_func(noop)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, None (const#0)
RETURN_VALUE_REG R0
        """)
        self.check_exec(None, noop)

    def test_loop_sum(self):
        converter = self.optimize_func(loop_sum)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'total', 0 (const#1)
SETUP_LOOP <relative jump +38>
LOAD_GLOBAL_REG R0, 'range' (name#0)
CALL_FUNCTION_REG R0, R0, (1 positional), 'n'
GET_ITER_REG R0, R0
FOR_ITER_REG 'i', R0, <relative jump +8>
INPLACE_ADD_REG 'total', 'i'
JUMP_ABSOLUTE 27
CLEAR_REG R0
POP_BLOCK
RETURN_VALUE_REG 'total'
        """)
        self.check_exec(4950, loop_sum, 100)

    def test_factorial_loop(self):
        converter = self.optimize_func(fact_loop)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'f', 1 (const#1)
SETUP_LOOP <relative jump +63>
LOAD_GLOBAL_REG R0, 'range' (name#0)
LOAD_CONST_REG R1, 2 (const#2)
LOAD_CONST_REG R2, 1 (const#1)
BINARY_ADD_REG R2, 'n', R2
CALL_FUNCTION_REG R0, R0, (2 positional), R1, R2
CLEAR_REG R1
CLEAR_REG R2
GET_ITER_REG R0, R0
FOR_ITER_REG 'i', R0, <relative jump +8>
INPLACE_MULTIPLY_REG 'f', 'i'
JUMP_ABSOLUTE 52
CLEAR_REG R0
POP_BLOCK
RETURN_VALUE_REG 'f'
        """)
        self.check_exec(3628800, fact_loop, 10, reoptimize=False)

    def test_factorial(self):
        converter = self.optimize_func(fact)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, 2 (const#1)
COMPARE_REG R0, '<', 'n', R0
JUMP_IF_FALSE_REG R0, 27
LOAD_CONST_REG R1, 1 (const#2)
RETURN_VALUE_REG R1
LOAD_GLOBAL_REG R0, 'fact' (name#0)
LOAD_CONST_REG R2, 1 (const#2)
BINARY_SUBTRACT_REG R2, 'n', R2
CALL_FUNCTION_REG R0, R0, (1 positional), R2
CLEAR_REG R2
BINARY_MULTIPLY_REG R0, 'n', R0
RETURN_VALUE_REG R0
        """)
        self.check_exec(3628800, fact, 10)

    def test_pow2(self):
        converter = self.optimize_func(pow2)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'x', 1 (const#1)
LOAD_CONST_REG R0, 2 (const#2)
SETUP_LOOP <relative jump +41>
LOAD_GLOBAL_REG R1, 'range' (name#0)
CALL_FUNCTION_REG R1, R1, (1 positional), 'n'
GET_ITER_REG R1, R1
FOR_ITER_REG 'i', R1, <relative jump +8>
INPLACE_MULTIPLY_REG 'x', R0
JUMP_ABSOLUTE 32
CLEAR_REG R0
CLEAR_REG R1
POP_BLOCK
RETURN_VALUE_REG 'x'
        """)
        self.check_exec(4294967296, pow2, 32, reoptimize=False)

    def test_sieve(self):
        converter = self.optimize_func(sieve)
        self.check_bytecode(converter, """
LOAD_GLOBAL_REG R0, 'list' (name#0)
LOAD_GLOBAL_REG R1, 'range' (name#1)
LOAD_CONST_REG 'j', 2 (const#1)
LOAD_CONST_REG R2, 1 (const#2)
LOAD_CONST_REG R3, -1 (const#3)
BINARY_ADD_REG R4, 'n', R2
CALL_FUNCTION_REG R1, R1, (2 positional), 'j', R4
CLEAR_REG R4
CALL_FUNCTION_REG 'primes', R0, (1 positional), R1
CLEAR_REG R0
CLEAR_REG R1
SETUP_LOOP <relative jump +119>
GET_ITER_REG R5, 'primes'
FOR_ITER_REG 'i', R5, <relative jump +97>
SETUP_LOOP <relative jump +91>
BINARY_MULTIPLY_REG R6, 'i', 'j'
BINARY_SUBSCR_REG R3, 'primes', R3
COMPARE_REG R6, '<=', R6, R3
JUMP_IF_FALSE_REG R6, 166
BINARY_MULTIPLY_REG R7, 'i', 'j'
COMPARE_REG R7, 'in', R7, 'primes'
LOAD_ATTR_REG R8, 'primes', 'remove' (name#2)
JUMP_IF_FALSE_REG R7, 155
BINARY_MULTIPLY_REG R9, 'i', 'j'
CALL_PROCEDURE_REG R8, (1 positional), R9
CLEAR_REG R8
CLEAR_REG R9
CLEAR_REG R7
INPLACE_ADD_REG 'j', R2
JUMP_ABSOLUTE 79
CLEAR_REG R6
POP_BLOCK
JUMP_ABSOLUTE 69
CLEAR_REG R2
CLEAR_REG R3
CLEAR_REG R5
POP_BLOCK
RETURN_VALUE_REG 'primes'
        """)
        self.check_exec([1, 1, 1], merge_load, reoptimize=False)

    def test_bisect(self):
        converter = self.optimize_func(bisect)
        self.check_bytecode(converter, """
LOAD_CONST_REG 'lo', 0 (const#1)
LOAD_GLOBAL_REG R0, 'len' (name#0)
CALL_FUNCTION_REG 'hi', R0, (1 positional), 'a'
CLEAR_REG R0
SETUP_LOOP <relative jump +88>
COMPARE_REG R1, '<', 'lo', 'hi'
JUMP_IF_FALSE_REG R1, 109
CLEAR_REG R1
BINARY_ADD_REG R2, 'lo', 'hi'
LOAD_CONST_REG R3, 2 (const#2)
BINARY_FLOOR_DIVIDE_REG 'hi', R2, R3
CLEAR_REG R3
BINARY_SUBSCR_REG R2, 'a', 'hi'
COMPARE_REG R2, '<', 'x', R2
JUMP_IF_FALSE_REG R2, 88
JUMP_ABSOLUTE 25
CLEAR_REG R2
LOAD_CONST_REG R4, 1 (const#3)
BINARY_ADD_REG 'lo', 'hi', R4
CLEAR_REG R4
JUMP_ABSOLUTE 25
CLEAR_REG R1
POP_BLOCK
RETURN_VALUE_REG 'lo'
        """)
        self.check_exec(2, bisect, tuple(range(100)), 1, reoptimize=False)

    def test_clear_reg(self):
        converter = self.optimize_func(clear_reg)
        self.check_bytecode(converter, """
BUILD_TUPLE_REG 'lst', 0
LOAD_CONST_REG R0, None (const#0)
BUILD_SLICE2_REG R0, R0, R0
BINARY_SUBSCR_REG R0, 'lst', R0
RETURN_VALUE_REG 'lst'
        """)
        self.check_exec((), clear_reg, 5, reoptimize=False)

    def test_qsort2(self):
        converter = self.optimize_func(qsort2)
        self.check_bytecode(converter, """
JUMP_IF_TRUE_REG 'sequence', 13
BUILD_LIST_REG R0, 0
RETURN_VALUE_REG R0
LOAD_CONST_REG R1, 0 (const#1)
BINARY_SUBSCR_REG 'pivot', 'sequence', R1
LOAD_GLOBAL_REG R1, 'partition' (name#0)
LOAD_CONST_REG R2, 1 (const#2)
LOAD_CONST_REG R3, None (const#0)
BUILD_SLICE2_REG R2, R2, R3
BINARY_SUBSCR_REG R2, 'sequence', R2
BUILD_LIST_REG R3, 0
BUILD_LIST_REG R4, 1, 'pivot'
BUILD_LIST_REG R5, 0
CALL_FUNCTION_REG R1, R1, (4 positional), R2, R3, R4, R5
CLEAR_REG R4
CLEAR_REG R5
UNPACK_SEQUENCE_REG R1, 3, 'lesser', 'equal', 'greater'
CLEAR_REG R1
LOAD_GLOBAL_REG R2, 'qsort2' (name#1)
CALL_FUNCTION_REG R3, R2, (1 positional), 'lesser'
BINARY_ADD_REG R3, R3, 'equal'
CALL_FUNCTION_REG R2, R2, (1 positional), 'greater'
BINARY_ADD_REG R3, R3, R2
RETURN_VALUE_REG R3
        """)
        data = list(range(50)); import random; random.shuffle(data); data=tuple(data)
        expected = list(range(50))
        self.check_exec(expected, qsort2, data, reoptimize=False)

    def test_make_func(self):
        converter = self.optimize_func(make_func)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, 'c' (const#1)
LOAD_CONST_REG R1, 5 (const#2)
LOAD_CONST_REG R2, <code object f at (...)> (const#3)
LOAD_CONST_REG R3, 'make_func.<locals>.f' (const#4)
MAKE_FUNCTION_REG 'f', R3, R2, 0, 1, R1, R0, 0
CLEAR_REG R3
LOAD_CONST_REG R0, 'a' (const#5)
LOAD_CONST_REG R1, 'b1' (const#6)
LOAD_CONST_REG R2, 'b2' (const#7)
CALL_FUNCTION_REG R0, 'f', (3 positional), R0, R1, R2
RETURN_VALUE_REG R0
        """)
        self.check_exec(("a", ("b1", "b2"), 5), make_func, reoptimize=False)

    def test_loop_compare_str(self):
        converter = self.optimize_func(loop_compare_str)
        self.check_bytecode(converter, """
LOAD_CONST_REG 't', 't' (const#1)
LOAD_CONST_REG 's', 's' (const#2)
SETUP_LOOP <relative jump +53>
LOAD_GLOBAL_REG R0, 'range' (name#0)
LOAD_CONST_REG R1, 3 (const#3)
CALL_FUNCTION_REG R0, R0, (1 positional), R1
CLEAR_REG R1
GET_ITER_REG R0, R0
FOR_ITER_REG 'loop', R0, <relative jump +15>
COMPARE_REG R2, '>=', 't', 's'
CLEAR_REG R2
JUMP_ABSOLUTE 40
CLEAR_REG R0
POP_BLOCK
LOAD_CONST_REG R3, True (const#4)
RETURN_VALUE_REG R3
        """)
        self.check_exec(True, loop_compare_str, reoptimize=False)

    def test_set_attr(self):
        converter = self.optimize_func(set_attr)
        self.check_bytecode(converter, """
LOAD_ATTR_REG R0, 'obj', 'attr1' (name#0)
LOAD_CONST_REG R1, 1 (const#1)
INPLACE_ADD_REG R0, R1
STORE_ATTR_REG 'obj', 'attr1' (name#0), R0
LOAD_CONST_REG R1, 3 (const#2)
STORE_ATTR_REG 'obj', 'attr2' (name#1), R1
CLEAR_REG R1
LOAD_CONST_REG R0, None (const#0)
RETURN_VALUE_REG R0
        """)

        class Dummy:
            pass

        dummy = Dummy()
        dummy.attr1 = 0
        dummy.attr2 = 0
        self.check_exec(None, set_attr, dummy, reoptimize=False)

    def test_get_loader(self):
        converter = self.optimize_func(get_loader)
        # "LOAD_GLOBAL_REG R3, 'find_loader'" must not be moved in a
        # conditional branch, but before the if
        self.check_bytecode(converter, """
JUMP_IF_FALSE_REG 'module_or_name', 52
LOAD_GLOBAL_REG R0, 'getattr' (name#0)
LOAD_CONST_REG R1, '__loader__' (const#1)
LOAD_CONST_REG R2, None (const#0)
CALL_FUNCTION_REG 'loader', R0, (3 positional), 'module_or_name', R1, R2
CLEAR_REG R0
CLEAR_REG R1
CLEAR_REG R2
LOAD_ATTR_REG 'fullname', 'module_or_name', '__name__' (name#2)
JUMP_FORWARD <relative jump +5>
MOVE_REG 'fullname', 'module_or_name'
LOAD_GLOBAL_REG R3, 'find_loader' (name#3)
CALL_FUNCTION_REG R3, R3, (1 positional), 'fullname'
RETURN_VALUE_REG R3
        """)
        self.can_reoptimize(get_loader)

    def test_find_loader(self):
        converter = self.optimize_func(find_loader)
        self.check_bytecode(converter, """
SETUP_EXCEPT <relative jump +95>
LOAD_GLOBAL_REG R0, 'sys' (name#0)
LOAD_ATTR_REG R0, R0, 'modules' (name#1)
BINARY_SUBSCR_REG R0, R0, 'name'
LOAD_ATTR_REG 'loader', R0, '__loader__' (name#2)
LOAD_CONST_REG R0, None (const#0)
COMPARE_REG R0, 'is', 'loader', R0
JUMP_IF_FALSE_REG R0, 95
LOAD_GLOBAL_REG R1, 'ValueError' (name#4)
LOAD_CONST_REG R2, '{}.__loader__ is None' (const#1)
LOAD_ATTR_REG R2, R2, 'format' (name#5)
CALL_FUNCTION_REG R2, R2, (1 positional), 'name'
CALL_FUNCTION_REG R1, R1, (1 positional), R2
CLEAR_REG R2
PUSH_REG R1
CLEAR_REG R1
RAISE_VARARGS 1
RETURN_VALUE_REG 'loader'
POP_REG R3
PUSH_REG R3
LOAD_GLOBAL_REG R4, 'KeyError' (name#6)
COMPARE_REG R3, 'exception match', R3, R4
CLEAR_REG R4
JUMP_IF_FALSE_REG R3, 133
POP_TOP
POP_TOP
POP_TOP
POP_EXCEPT
JUMP_FORWARD <relative jump +4>
CLEAR_REG R3
END_FINALLY
LOAD_GLOBAL_REG R5, '_bootstrap' (name#7)
LOAD_ATTR_REG R5, R5, '_find_module' (name#8)
CALL_FUNCTION_REG R5, R5, (2 positional), 'name', 'path'
RETURN_VALUE_REG R5
        """)
        self.can_reoptimize(find_loader)


    def test_store_load_global(self):
        converter = self.optimize_func(store_load_global)
        self.check_bytecode(converter, """
LOAD_CONST_REG R0, 5 (const#1)
STORE_GLOBAL_REG 'global_var' (name#0), R0
RETURN_VALUE_REG R0
        """)
        self.check_exec(5, store_load_global)


def test_main():
    support.run_unittest(
        OptimizeTests,
    )

if __name__ == "__main__":
    test_main()
