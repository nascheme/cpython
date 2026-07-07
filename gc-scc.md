# SCC-guided tail removal in the cyclic GC

## Goal

Before the GC finalizes and clears an unreachable set, run a
strongly-connected-components (SCC) pass over the unreachable object graph and
remove the "tails" - objects that cannot reach any cycle - from the set.  The
GC then finalizes and clears only the remaining "core" (in full, exactly as
before this work).  The removed tails are broken naturally by the ordinary
refcount cascade once `delete_garbage()` clears the core cycles.

This is implemented for the default/GIL GC in `Python/gc.c`.  It is gated on
`GC_ENABLE_SCC_TAILS`.

## Motivation

`finalize_garbage()` calls `tp_finalize` on the unreachable objects in GC list
order, which is essentially arbitrary.  For a chain of finalizable objects
hanging off a cycle - e.g. a `TextIOWrapper -> BufferedWriter -> FileIO` writer
held only through a reference cycle - arbitrary order can finalize the inner
`FileIO` (closing the fd) before the outer `BufferedWriter` flushes, losing
buffered data.  This is the gh-62052 class of data-loss bug.

If those chain objects are instead removed from the unreachable set before
finalization, they stay alive until the cycle they hang off is cleared by
`delete_garbage()`.  Clearing the cycle drops the reference into the chain, and
the chain then dies through the refcount cascade: `tp_dealloc ->
PyObject_CallFinalizerFromDealloc` (all `_io` deallocs route through
`_PyIOBase_finalize`, `Modules/_io/iobase.c`).  The cascade is referrer-first,
so the chain is finalized outermost-first: the buffer flushes before the fd
closes.  PEP 442 ordering is preserved for these objects, and the data-loss bug
is fixed.

## Core / tail definitions

Build the unreachable-only directed graph: nodes are the unreachable objects,
edges are `tp_traverse` references whose target is also in the unreachable set.

- A **cyclic SCC** is one with size > 1, or size 1 with a self-edge
  (`tp_traverse` visits the object itself).  Every directed cycle lives in one
  of these.
- An object is **core** if its SCC can reach a cyclic SCC (including being one).
  This deliberately includes *bridge* objects between cycles: for
  `cycle1 -> C -> cycle2`, node `C` is a single-node SCC that reaches `cycle2`,
  so `C` is core.  Its finalizer may observe `cycle2`, so it must be finalized
  while the graph is intact.
- An object is a **tail** if it is not core (cannot reach any cycle).

## Why removing tails is sound

Key structural fact: **a tail cannot reference a core object.**  If it did, it
would reach that core object's cycle and would itself be core.  Consequences:

- Removing tails from the unreachable set creates no new external references
  into the core, so `handle_resurrected_objects()`'s internal
  `deduce_unreachable()` stays correct - the core's reachability is unchanged.
- Tail finalizers, running later in the refcount cascade, only ever observe
  intact objects (other tails, or objects that were reachable all along).  PEP
  442 ordering is preserved for tails.
- `delete_garbage()` only ever processes the finalized core, so the existing
  resurrection/survivor handling remains valid with no new code paths.

Every object in the unreachable set has internal in-degree >= 1 (that is how it
became unreachable), so every source of the condensation DAG is a cyclic SCC.
A nonempty unreachable set with zero cyclic SCCs therefore implies a broken
`tp_traverse`; that case is treated as a fallback (no split, old behavior).

## Implementation (`Python/gc.c`)

Gated on `GC_ENABLE_SCC_TAILS`, with a `GC_DEBUG_SCC_TAILS` knob for a per-
collection stderr line (`unreachable/core/tails/sccs` counts).

Data structures and helpers:

- `GCSccNode` - per-object graph node (gc head, Tarjan index/lowlink, edge
  slice into a flat edge vector, scc id, on-stack and self-loop flags).
- `GCSccMapEntry` + `scc_hash_gc` / `scc_map_lookup` / `scc_map_insert` /
  `scc_map_size` - an open-addressing hash map from `PyGC_Head*` to node index,
  used to test unreachable-set membership and resolve edge targets.
- `scc_append_edge` / `scc_visit_edge` - build the flat edge vector during
  `tp_traverse`.  Membership test is map lookup plus `gc_is_collecting`.
- `scc_run_tarjan` - iterative Tarjan.  It additionally records nodes in **pop
  order** into a preallocated array as SCCs are completed.  Pop order groups
  each SCC's members contiguously in ascending scc id, and every cross-SCC edge
  points to a lower scc id.
- `scc_mark_core` - one pass over nodes in pop order.  For a cyclic SCC, set
  `reaches_cycle`.  For a single-node acyclic SCC, set `reaches_cycle` iff some
  successor's SCC already has it (all successors sit in lower-numbered SCCs,
  already processed).  Returns `SCC_SPLIT_FALLBACK` if there are nodes but no
  cyclic SCC.
- `move_scc_tails` - builds the graph, runs Tarjan and `scc_mark_core`, then
  `gc_list_move`s every non-core object into `tails`.  Mutates the lists only on
  `SCC_SPLIT_OK`; returns `SCC_SPLIT_OK` / `SCC_SPLIT_FALLBACK` / `SCC_SPLIT_OOM`.

### Wiring in `gc_collect_main()`

The split runs between `handle_weakref_callbacks(&unreachable, old)` and
`finalize_garbage(tstate, &unreachable)`:

```c
#if GC_ENABLE_SCC_TAILS
    if ((gcstate->debug & _PyGC_DEBUG_SAVEALL) == 0) {
        PyGC_Head tails;
        Py_ssize_t n_tails = 0;
        if (move_scc_tails(&unreachable, &tails, &n_tails) == SCC_SPLIT_OK) {
            stats.collected += n_tails;
            gc_list_clear_collecting(&tails);
            gc_list_merge(&tails, old);
        }
    }
#endif
    finalize_garbage(tstate, &unreachable);
```

After the split, `finalize_garbage`, `handle_resurrected_objects`,
`clear_weakrefs` and `delete_garbage` all run on the core exactly as they did
before this work.

Insertion-point invariants (verified): `unreachable` is a normal list with
`PREV_MASK_COLLECTING` set and `NEXT_MASK_UNREACHABLE` clear (cleared by
`move_legacy_finalizers`); `gc_list_move` / `gc_list_merge` preserve those
flags.  Tails are handled like resurrected objects: their collecting flag is
cleared and they are merged into the old generation.

### SAVEALL

Under `gc.DEBUG_SAVEALL` the split is skipped, so every unreachable object is
finalized by the GC and preserved in `gc.garbage`, unchanged from today.

### Statistics

Tails are counted into `stats.collected` at split time, so the `gc.collect()`
return value keeps meaning "all unreachable garbage found".  This carries the
same overcount caveat as before: an object may be counted as collected even if
a finalizer later resurrects it.

### Weakrefs

`handle_weakref_callbacks` still runs on the full unreachable set before the
split, so weakref-callback timing (for weakrefs whose referent is in the
unreachable set) is unchanged.

`clear_weakrefs` later runs only on the core.  Weakrefs that point *to* a tail
are intentionally left intact: a tail dies a normal refcount death in the
cascade, so its weakrefs clear and their callbacks fire exactly as they would
for any normally-deallocated object (this is consistent with running the tail's
own finalizer in the cascade).

One case needs care: a weakref that is *itself* a tail.  `clear_weakrefs`
normally clears every weakref in the trash set so it cannot invoke its callback
during teardown; this is the bpo-38006 safety net, and it matters when the
weakref's referent is not in the unreachable set (e.g. hidden behind a container
with no `tp_traverse`) and therefore dies during the core cascade.  Because
tails are removed before `clear_weakrefs` runs, `clear_weakref_tails()` clears
the weakref tails at split time - silently, without callbacks, matching
`clear_weakrefs`.

### Resurrection

If a core finalizer creates a new reference from a tail (now in `old`) to a
core object, `handle_resurrected_objects` sees an external reference and
resurrects that core object; the resulting new cycle is collected on a later
pass.  A tail whose own finalizer resurrects it (e.g. stores `self` in a global)
survives via `PyObject_CallFinalizerFromDealloc`'s resurrection check and does
not appear in `gc.garbage`.  No unfinalized-clear hazard exists in this design.

## Tests (`Lib/test/test_gc.py`, `@requires_gil_enabled`)

- `test_scc_clear_self_cycle` - self-cycle: one clear, `collect() == 1`.
- `test_scc_clear_skips_acyclic_tail` - 2-cycle with a 25-node tail chain;
  `collect() == tail_len + 2`, and exactly one `tp_clear` (the other cycle
  member and all tails die via cascade; no tail is ever cleared).
- `test_scc_finalize_order_tail_chain` - `Outer(Mid(Inner()))` chain off a
  cycle; asserts finalizer order `['outer', 'mid', 'inner']`.
- `test_scc_bridge_between_cycles_finalized` - `cycle1 -> B -> cycle2`; `B`'s
  finalizer reads through its reference into `cycle2` and must see it intact.
- `test_scc_tail_resurrect_in_del` - a tail finalizer stores `self` in a
  global; the object stays alive and usable and is not in `gc.garbage`.
- `test_scc_saveall_includes_tails` - under `DEBUG_SAVEALL`, the split is
  skipped and tails appear in `gc.garbage`.
- `test_scc_buffered_writer_flushed_in_cycle` - gh-62052 shape: a buffered
  writer reachable only through a cycle is flushed before its fd is closed.

`Modules/_testcapi/gc.c` provides `SccGcNode`, whose `tp_clear` bumps a counter
while its `tp_dealloc` drops references without bumping it, so the tests can
distinguish GC clears from cascade deallocs.

## Previous iterations

An earlier prototype ran the SCC pass *late* (after `clear_weakrefs`, just
before `delete_garbage`) and selected a minimal clearable subset that covered
every DFS back-edge inside each cyclic SCC, calling `tp_clear` only on that
subset.  It required extra per-node state (`path_pos`, `dfs_color`,
`selected`), a back-edge cover DFS (`scc_select_clear_subset`,
`scc_cover_back_edge`, `scc_select_node`, `scc_node_is_clearable`), and a
survivor fallback in the delete stage for objects that did not vanish by
cascade.

That approach was replaced by the simpler early tail removal described above:
it does not try to minimize clears inside the core (the core is cleared in full,
as before), it only removes provably-safe tails, and it moves the pass ahead of
`finalize_garbage` so tail finalizers run in the correct cascade order.  The
back-edge cover approach remains in git history.
