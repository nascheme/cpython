"""
Benchmark results on Linux-3.4.9-2.fc16.x86_64-x86_64-with-fedora-16-Verne
with Intel(R) Core(TM) i7-2600 CPU @ 3.40GHz.

Bench fact
Stack:     54.7 ms
Register:  54.9 ms (+0.4%)
Bench fact_loop
Stack:     77.0 ms
Register:  76.8 ms (-0.2%)
Bench pow2
Stack:     85.9 ms
Register:  81.1 ms (-5.5%)
Bench loop_sum
Stack:     27.0 ms
Register:  24.1 ms (-11.0%)
Bench loop_noop
Stack:     105.1 ms
Register:  102.5 ms (-2.4%)
Bench sieve
Stack:     45.5 ms
Register:  45.6 ms (+0.3%)
Bench bisect
Stack:     40.1 ms
Register:  36.5 ms (-8.9%)
Bench qsort2
Stack:     158.5 ms
Register:  156.1 ms (-1.5%)
"""
from registervm import Converter, optimize_func, Config, LABEL
import dis
import gc
import sys
import time
import types

CHECK_REFLEAKS = True

class RewriteBytecode:
    def __init__(self, func, config=None):
        if isinstance(func, types.MethodType):
            func = func.__func__
        self.func = func
        self.code = Converter(func.__code__, config)
        self.name = func.__name__

    def dump(self):
        #self.code.dump()
        dis.dis(self.func)
        ninstr = 0
        for instr in self.code.instructions:
            if instr.name is not LABEL:
                ninstr += 1
        print("-> %s instructions" % ninstr)

    def patch(self, dump_bytecode=True, dump_blocks=True):
        if dump_bytecode:
            self.dump()
            print("=== %s ===>" % self.name)
        self.code.convert(dump_blocks=dump_blocks)
        if 0:
            self.dump()
            print("=== %s ===>" % self.name)
        self.func.__code__ = self.code.compile()
        if dump_bytecode:
            self.dump()
        self.code.check_inefficient_code()
        print("=== %s ===>" % self.name)

def use_register_bytecodes(config, func, *args):
    if 1:
        RewriteBytecode(func, config).patch()
    else:
        dis.dis(func)
        print("====>")
        optimize_func(func, config)
        dis.dis(func)
        print("====>")
    print(func(*args))

    if CHECK_REFLEAKS and hasattr(sys, "gettotalrefcount"):
        for prepare in range(10):
            func(*args)
            gc.collect()

        before = None
        leak = None
        repeat = 10
        before = sys.gettotalrefcount()
        for index in range(repeat):
            func(*args)
            gc.collect()
        del index
        gc.collect()
        leak = sys.gettotalrefcount() - before
        if leak:
            raise Exception("reference leak: %s!" % leak)
        print("(no reference leak)")

def unary_not(x):
    x = not x
    return x

def subx():
    x = 1
    x = x - x
    return x

def inplace_add(a, b, c):
    return a + b + c

def loop_sum(n):
    total = 0
    for i in range(n):
        total += i
    return total

def loop_noop(n):
    for i in range(n):
        pass

def noop():
    return

def load_const():
    total = 42
    return total

def fact(n):
    if n < 2:
        return 1
    else:
        return n * fact(n-1)

def fact_loop(n):
    n += 1
    f = 1
    for i in range(2, n+1):
        f *= i
    return f

def pow2(n):
    x = 1
    for i in range(n):
        x *= 2
    return x

def call_func(text):
    x = len(text)
    return x

def noop():
    pass

def _bench(func, args, repeat, loops):
    best = None
    for run in range(repeat):
        before = time.perf_counter()
        for loop in range(loops):
            func(*args)
        dt = (time.perf_counter() - before)
        if best is not None:
            best = min(best, dt)
        else:
            best = dt
    return best

def bench(config, func, *args, **kw):
    repeat = kw.get('repeat', 50)
    loops = kw.get('loops', 5)
    print("Bench %s" % func.__name__)
    best_empty = _bench(noop, (), repeat, loops)
    a = _bench(func, args, repeat, loops)
    optimize_func(func, config)
    if func is qsort2:
        optimize_func(partition, config)
    b = _bench(func, args, repeat, loops)
    a -= best_empty
    b -= best_empty
    print("Stack:     %.1f ms" % (a*1e3))
    k = (b-a)*100./a
    print("Register:  %.1f ms (%+.1f%%)" % (b*1e3, k))

def sieve(max):
    primes = list(range(2,max+1))
    for i in primes:
        j = 2
        while i * j <= primes[-1]:
            if i * j in primes:
                primes.remove(i*j)
            j += 1
    return primes

def arccot(x, unity):
    sum = xpower = unity // x
    n = 3
    sign = -1
    while 1:
        xpower = xpower // (x*x)
        term = xpower // n
        if not term:
            break
        sum += sign * term
        sign = -sign
        n += 2
    return sum

def pi(digits):
    unity = 10**(digits + 10)
    pi = 4 * (4*arccot(5, unity) - arccot(239, unity))
    return pi // 10**10

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
    """
    Quicksort using a partitioning function
    >>> qsort2<<docstring test numeric input>>
    <<docstring test numeric output>>
    >>> qsort2<<docstring test string input>>
    <<docstring test string output>>
    """
    if not sequence:
        return []
    else:
        pivot = sequence[0]
        lesser, equal, greater = partition(sequence[1:], [], [pivot], [])
        return qsort2(lesser) + equal + qsort2(greater)

def build_list(x, y, z):
    return x + y + z

def build_list(x, y, z):
    print(x, y, z)

def yield_value():
    x = 3
    yield x

def test_func(x):
    if not x:
        return 1
    else:
        return x+1

# enable all optimizations!
config = Config()
config.quiet = False
config.enable_unsafe_optimizations()

#use_register_bytecodes(config, unary_not, True)
#use_register_bytecodes(config, subx)
#use_register_bytecodes(config, loop_sum, 10)
#use_register_bytecodes(config, load_const)
#use_register_bytecodes(config, noop)
#use_register_bytecodes(config, loop_noop, 3)
#use_register_bytecodes(config, fact, 10)
#use_register_bytecodes(config, fact_loop, 10)
#use_register_bytecodes(config, pow2, 32)
#use_register_bytecodes(config, sieve, 100)
#use_register_bytecodes(config, arccot, 5, 10**(10 + 10))
#use_register_bytecodes(config, call_func, "abc")
#use_register_bytecodes(config, bisect, tuple(range(100)), 1)
data = list(range(50)); import random; random.shuffle(data); data=tuple(data)
#use_register_bytecodes(config, qsort2, data)
#use_register_bytecodes(config, partition, [1, 2, 3], [], [2], [])
#use_register_bytecodes(config, inplace_add, [1], [2, 3], [4])
#use_register_bytecodes(config, build_list, [1], [2], [3])
#use_register_bytecodes(config, make_func)
#use_register_bytecodes(config, yield_value)
#use_register_bytecodes(config, test_func, 3)

if 1:
    bench(config, fact_loop, 1000, loops=100)
    bench(config, fact, 800, loops=10**2)
    bench(config, pow2, 1000, loops=10**3)
    bench(config, loop_sum, 10**5)
    bench(config, loop_noop, 10**6)
    bench(config, sieve, 10**3)
    bench(config, bisect, tuple(range(10**7)), 1, loops=10**4)
    data = list(range(700)); import random; random.shuffle(data); data=tuple(data)
    bench(config, qsort2, data, loops=10, runs=100)

