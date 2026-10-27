# Copied from grail-compat src/grail/ by tools/sync_to_cpython_grail.py; do not edit here.
"""pmap and parallel_reduce: chunked helpers on top of grail.Pool.

The same file serves the native implementation (cpython-grail's
Lib/grail/_parallel.py, copied by tools/sync_to_cpython_grail.py) and
grail-compat.  Only Pool.map is needed from the pool.
"""

import time
from itertools import repeat
from operator import index as _index

try:
    from ._pool import _defer_refcount           # grail-compat
except ImportError:
    from _grail_pool import _defer_refcount      # cpython-grail

__all__ = ["pmap", "parallel_reduce"]

# pmap aims for chunks of about this much work, and runs inputs whose
# remaining work is estimated below _SERIAL_NS in the calling thread.  It
# estimates the work by running a prefix in the calling thread, doubling
# its length until it has taken _PROBE_NS: one cold call is a poor guide.
_CHUNK_TARGET_NS = 50_000
_SERIAL_NS = 100_000
_PROBE_NS = 20_000
# parallel_reduce's default partition: this many chunks, whatever the pool.
_REDUCE_CHUNKS = 64


def _default_pool():
    from grail import pool
    return pool()


def _check_chunk(chunk):
    chunk = _index(chunk)
    if chunk < 1:
        raise ValueError("chunk must be >= 1")
    return chunk


def _as_list(iterable):
    return iterable if type(iterable) in (list, tuple) else list(iterable)


def _apply(func, part):
    return [func(x) for x in part]


def pmap(func, iterable, /, *, chunk=None, pool=None):
    """Return [func(x) for x in iterable], computed in parallel.

    The items are split into chunks of chunk items, each run as one pool
    task; results keep the input order.  Without chunk, a prefix of the
    items is run in the calling thread and timed (about 20 us of work): if
    the rest would take only about 0.1 ms, they run there too, skipping
    the pool; otherwise chunks are sized for about 50 us of work, and at
    least 4 per worker.  So some items, or all, may run in the calling
    thread.  The first exception raised by func, in input order,
    propagates.

    pool defaults to grail.pool().
    """
    items = _as_list(iterable)
    n = len(items)
    if chunk is not None:
        chunk = _check_chunk(chunk)
    if n == 0:
        return []
    _defer_refcount(func)
    if pool is None:
        pool = _default_pool()
    out = []
    start = 0
    if chunk is None:
        t0 = time.perf_counter_ns()
        step = 1
        while True:
            out.extend([func(x) for x in items[start:start + step]])
            start = len(out)
            elapsed = time.perf_counter_ns() - t0
            if start >= n or elapsed >= _PROBE_NS:
                break
            step *= 2
        per_item = max(elapsed // start, 1)
        rest = n - start
        if rest == 0 or per_item * rest < _SERIAL_NS or pool.n_threads == 1:
            out.extend([func(x) for x in items[start:]])
            return out
        by_time = -(-_CHUNK_TARGET_NS // per_item)
        by_balance = rest // (4 * pool.n_threads)
        chunk = max(1, min(by_time, by_balance))
    parts = (items[i:i + chunk] for i in range(start, n, chunk))
    for part in pool.map(_apply, repeat(func), parts):
        out.extend(part)
    return out


def parallel_reduce(fold, combine, iterable, init, *, chunk=None, pool=None):
    """Fold chunks of the items in parallel and combine the results in order.

    Each chunk is folded from init: acc = fold(acc, item) for its items in
    order.  The chunk results are then combined as a balanced binary tree
    of adjacent results, one level at a time: combine(r0, r1),
    combine(r2, r3), ..., then the same on those results, an odd one out
    passing up unchanged.  combine always gets the earlier result first,
    so with an associative combine the result equals a sequential fold.
    init is shared by every chunk, so it must not be mutated; use an
    immutable value such as a PMap or a tuple.  An empty input returns init.

    The partition is chunk items per chunk; by default the items are split
    into 64 chunks, which depends only on the number of items, and the tree
    depends only on the number of chunks.  So for a given input and chunk
    the result does not depend on the pool size or on scheduling, even when
    combine is not associative (as with float addition, where the tree also
    rounds less than a sequential sum).  Folds and tree levels estimated at
    under about 0.1 ms run in the calling thread, with the same result.

    pool defaults to grail.pool().
    """
    items = _as_list(iterable)
    n = len(items)
    if chunk is not None:
        chunk = _check_chunk(chunk)
    if n == 0:
        return init
    if chunk is None:
        chunk = -(-n // _REDUCE_CHUNKS)
    _defer_refcount(fold)
    _defer_refcount(combine)

    def fold_chunk(i):
        acc = init
        for x in items[i:i + chunk]:
            acc = fold(acc, x)
        return acc

    starts = range(0, n, chunk)
    results, pool = _first_then_rest(fold_chunk, [(i,) for i in starts], pool)
    while len(results) > 1:
        pairs = [(results[i], results[i + 1])
                 for i in range(0, len(results) - 1, 2)]
        odd = results[-1:] if len(results) % 2 else []
        results, pool = _first_then_rest(combine, pairs, pool)
        results += odd
    return results[0]


def _first_then_rest(func, arglists, pool):
    """Return [func(*args) for args in arglists] and the pool.  The first
    call runs here and is timed; the rest run on the pool unless they would
    take only about 0.1 ms."""
    t0 = time.perf_counter_ns()
    results = [func(*arglists[0])]
    per_call = max(time.perf_counter_ns() - t0, 1)
    rest = arglists[1:]
    if not rest:
        return results, pool
    if pool is None:
        pool = _default_pool()
    if per_call * len(rest) < _SERIAL_NS or pool.n_threads == 1:
        results += [func(*args) for args in rest]
    else:
        results += pool.map(func, *zip(*rest))
    return results, pool
