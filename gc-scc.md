# Prototype plan: SCC-guided cyclic GC clearing

## Goal

After the GC has identified the final unreachable set, run an additional strongly-connected-components (SCC) pass over the unreachable object graph.  Then call `tp_clear` only on a selected subset of objects that is sufficient to break all cycles, instead of every unreachable object.

For this plan, "cyclic SCC" means either:

- an SCC with more than one object, or
- a single-object SCC with an explicit self-edge (`tp_traverse` visits the object itself).

Every directed cycle is contained in one of these SCCs.  The initial prototype should not try to find the mathematically smallest set of objects to clear; minimum directed feedback vertex set is a hard graph problem.  Instead, use a linear-time conservative subset: inside each cyclic SCC, run DFS and cover every DFS back-edge by selecting a clearable object on the corresponding cycle path.  Prefer the back-edge source when it has `tp_clear`; otherwise select another object with `tp_clear` on the DFS path from the back-edge target to the source.  Clearing those selected objects' outgoing references should break all cycles in the SCC, while acyclic tails and unselected SCC members can be reclaimed by normal refcount cascades.

## Investigation summary

Relevant files:

- Default/GIL GC: `Python/gc.c`
- Free-threaded GC: `Python/gc_free_threading.c`
- GC object/list bits: `Include/internal/pycore_gc.h`
- Object stack allocator usable in free-threaded code: `Include/internal/pycore_object_stack.h`
- Background: `InternalDocs/garbage_collector.md`

### Default/GIL GC current flow

`gc_collect_main()` in `Python/gc.c` currently does:

1. Merge generations to collect.
2. `deduce_unreachable(young, &unreachable)`:
   - `update_refs()` copies `Py_REFCNT()` into `_gc_prev` as `gc_refs` and sets `PREV_MASK_COLLECTING`.
   - `subtract_refs()` subtracts references found by `tp_traverse`.
   - `move_unreachable()` moves objects with `gc_refs == 0` into `unreachable`.
3. Move reachable survivors to the older generation.
4. Split legacy finalizers (`tp_del`) into `finalizers`; move objects reachable from them there too.
5. Clear weakrefs with callbacks and call callbacks.
6. Call `tp_finalize` on unreachable objects.
7. `handle_resurrected_objects()` re-runs reachability and produces `final_unreachable`.
8. `clear_weakrefs(&final_unreachable)` clears remaining weakrefs.
9. `delete_garbage(..., &final_unreachable, old)` calls `tp_clear` on every object in `final_unreachable`.

Good insertion point: after `clear_weakrefs(&final_unreachable)` and before `delete_garbage()`.  At this point:

- finalizers and resurrection have already been handled;
- weakrefs have been cleared to avoid exposing broken trash;
- every object still in `final_unreachable` has `PREV_MASK_COLLECTING` set, so `gc_is_collecting(AS_GC(op))` is an efficient membership test for the SCC graph;
- `final_unreachable` is a normal doubly linked GC list, so objects can be moved into a separate `to_clear` list.

### Free-threaded GC current flow

`gc_collect_internal()` in `Python/gc_free_threading.c` currently does:

1. Stop the world.
2. Merge queued/deferred refcounts.
3. Optionally mark known-live objects with `_PyGC_BITS_ALIVE`.
4. `deduce_unreachable_heap()` marks unreachable objects and pushes them onto `state->unreachable`.
   - The unreachable worklist is singly linked through `op->ob_tid`.
   - Each unreachable worklist entry is protected by an extra strong reference from `merge_refcount(op, 1)`.
5. Find weakref callbacks while still stopped.
6. Start the world; call weakref callbacks and `tp_finalize`.
7. Stop the world again; `handle_resurrected_objects()` removes resurrected objects from `state->unreachable`.
8. `clear_weakrefs(state)`.
9. Start the world.
10. `delete_garbage(state)` pops `state->unreachable`, clears `_PyGC_BITS_UNREACHABLE`, calls `tp_clear` on every remaining object, and drops the worklist reference.

Good insertion point: after `clear_weakrefs(state)` and before `delete_garbage(state)`.  The SCC pass may be done while the world is stopped or immediately after restarting the world.  Safer prototype: do it while still stopped, before `_PyEval_StartTheWorld(interp)`, because the object graph and `ob_gc_bits` membership are stable.

Free-threaded complication: non-cleared unreachable objects are protected by worklist references.  If they are skipped, those extra references must still be dropped, otherwise refcount cascades cannot reclaim them.

## Correctness model

Let `U` be the final unreachable set after finalizers, resurrection handling, and weakref clearing.  Consider the directed graph containing only `tp_traverse` edges from objects in `U` to objects in `U`.

If a set of vertices intersects every directed cycle and those vertices have their cycle-forming outgoing references cleared, then the remaining graph is acyclic.  Since `U` has no incoming references from outside `U`, refcounting should reclaim the acyclic remainder as references are decremented.

A simple linear way to choose such a set is to DFS each cyclic SCC and cover every DFS back-edge.  A directed graph is acyclic iff DFS finds no back-edges, so breaking every DFS back-edge cycle breaks every directed cycle.  For each edge from current gray node `u` to gray ancestor `v`, select one object with non-NULL `tp_clear` on the DFS path `v ... u`, preferably `u` itself.  Clearing that object's outgoing references breaks the cycle represented by this back-edge.  This is not guaranteed to be the minimum possible clear set, but it is usually smaller than clearing every object in the SCC and is suitable for a prototype.

The clearability check is important.  Some GC-tracked objects have `tp_traverse` but no `tp_clear` (for example tuples).  Selecting such an object would not break any edge.  If the smaller-subset pass cannot find a clearable object to cover a back-edge, fall back to clearing all objects in that cyclic SCC, or simply fall back to the old full-`final_unreachable` clearing path for the first prototype.

This assumes extension types follow the GC contract: `tp_traverse` exposes all strong references relevant to cycle detection, and `tp_clear` clears enough references to break cycles.  Existing GC already depends on `tp_traverse` for reachability, but the SCC optimization depends on `tp_traverse` being accurate enough to decide which objects are cyclic and on selected objects' `tp_clear` removing the relevant outgoing references.  For a prototype, keep a conservative fallback path.

## Default/GIL implementation plan

### 1. Add an SCC utility in `Python/gc.c`

Add a private helper near `delete_garbage()` or near the reachability helpers:

```c
static int
move_scc_clear_subset(PyGC_Head *unreachable, PyGC_Head *to_clear,
                      Py_ssize_t *num_selected, Py_ssize_t *num_edges);
```

Contract:

- Input `unreachable` is a normal doubly linked GC list.
- All candidate objects in `unreachable` have `PREV_MASK_COLLECTING` set.
- `to_clear` is initialized by the caller or helper.
- On success, move all objects selected for clearing from `unreachable` to `to_clear`.
- Objects left in `unreachable` are acyclic tails and should not have `tp_clear` called by the SCC path.
- On allocation failure, leave the lists in a valid state and return `-1` so the caller can fall back to existing `delete_garbage()` over the whole set.

### 2. Build a temporary graph

Allocate temporary arrays with `PyMem_RawMalloc`/`PyMem_RawCalloc`:

```c
typedef struct {
    PyGC_Head *gc;
    Py_ssize_t index;      // Tarjan DFS index, -1 initially
    Py_ssize_t lowlink;
    Py_ssize_t edge_start;
    Py_ssize_t edge_count;
    Py_ssize_t scc_id;
    unsigned char on_stack;
    unsigned char self_loop;
    unsigned char selected;
    unsigned char dfs_color;   // for post-Tarjan back-edge selection
} GCSccNode;
```

Use two passes:

1. Count/list nodes by iterating `unreachable`; build an open-addressed pointer-to-index table keyed by `PyGC_Head *`.
2. Traverse each object with its `tp_traverse`; for every visited object:
   - ignore non-GC objects;
   - use `PyGC_Head *target = AS_GC(op)`;
   - include the edge only if the target is in the pointer table and `gc_is_collecting(target)` is true;
   - append target index to an edge vector;
   - set `self_loop` if source index equals target index.

For the prototype, allow O(N + E) memory.  Grow the edge vector with `PyMem_RawRealloc`.  On OOM, free all temporary memory and return `-1`.

### 3. Run iterative Tarjan

Avoid C recursion.  Use allocated stacks:

- Tarjan stack of node indices;
- DFS frame stack containing `(node_index, next_edge_offset)`.

Assign an `scc_id` to every node.  Record SCC size and whether it is cyclic (`size > 1` or a one-node SCC with `self_loop`).

### 4. Select a smaller clear subset inside cyclic SCCs

For each cyclic SCC, run an iterative DFS restricted to edges whose target has the same `scc_id`.  Use `dfs_color` values white/gray/black and maintain the current DFS path plus each node's position in that path.  Whenever traversal sees an edge from current gray node `u` to gray ancestor `v`, the path segment `v ... u` plus edge `u -> v` is a cycle.  Select one object with non-NULL `tp_clear` on that path segment, preferably `u` if it is clearable.  Mark that object `selected = 1`.

If no clearable object exists on the path segment, the smaller-subset pass cannot prove it can break that cycle.  For the first prototype, treat this as a request to fall back to the old full clearing path.  A less conservative later version could select all clearable objects in that SCC.

Properties:

- linear in the SCC's nodes and internal edges, except for scanning the path segment to find a clearable object; for a prototype this is acceptable, and it can be made linear by tracking the nearest clearable ancestor if needed;
- usually selects fewer nodes than clearing the whole SCC;
- not globally minimum, but sufficient to break all directed cycles if selected objects' `tp_clear` clears the relevant outgoing references.

If this second DFS adds too much implementation complexity, the fallback simplification is to select every object in cyclic SCCs.  That still validates the SCC-vs-tail optimization, just not the smaller-subset refinement.

### 5. Split the list

Iterate over `unreachable` with a saved `next` pointer.  For every selected node, call `gc_list_move(gc, to_clear)`.  Leave unselected nodes in `unreachable`.

This preserves valid GC lists.  Both lists still contain objects with `PREV_MASK_COLLECTING` set, matching what `delete_garbage()` expects.

### 6. Wire into `gc_collect_main()`

Replace the final clearing block:

```c
stats.collected += gc_list_size(&final_unreachable);
delete_garbage(tstate, gcstate, &final_unreachable, old);
```

with prototype logic like:

```c
Py_ssize_t final_count = gc_list_size(&final_unreachable);
stats.collected += final_count;

if (gcstate->debug & _PyGC_DEBUG_SAVEALL) {
    delete_garbage(tstate, gcstate, &final_unreachable, old);
}
else {
    PyGC_Head to_clear;
    gc_list_init(&to_clear);
    if (move_scc_clear_subset(&final_unreachable, &to_clear, NULL, NULL) < 0) {
        delete_garbage(tstate, gcstate, &final_unreachable, old);
    }
    else {
        delete_garbage(tstate, gcstate, &to_clear, old);

        // Safety fallback for the prototype: if acyclic-tail objects did not
        // disappear as expected, clear them with the old behavior.
        if (!gc_list_is_empty(&final_unreachable)) {
            delete_garbage(tstate, gcstate, &final_unreachable, old);
        }
    }
}
```

The fallback preserves correctness but may reduce measured savings in unusual cases.  Keep it enabled by default for the first prototype.  Add counters/logging so fallback triggers are visible.  Later, add an optional diagnostic mode where remaining skipped objects cause an assertion or loud debug log instead of silently using the old clearing path.

### 7. Debug/stat instrumentation

For prototype evaluation, add optional counters under a compile-time macro, e.g.:

```c
#define GC_ENABLE_SCC_CLEAR 1
#define GC_DEBUG_SCC_CLEAR 0
```

Useful counters:

- final unreachable object count;
- number of SCC-selected objects;
- number of skipped objects;
- number of internal edges scanned;
- OOM fallback count;
- skipped-survivor fallback count.

Emit with `_PyGC_DEBUG_STATS` or add temporary `PySys_WriteStderr()` guarded by `GC_DEBUG_SCC_CLEAR`.

## Free-threaded implementation plan

Implement after the GIL prototype works.

### 1. Reuse the graph/Tarjan code shape

Add an analogous helper in `Python/gc_free_threading.c`:

```c
static int
split_scc_clear_subset(struct collection_state *state,
                       struct worklist *to_clear,
                       struct worklist *skipped);
```

Graph construction differs only in iteration and membership:

- Iterate `state->unreachable` with `WORKSTACK_FOR_EACH`.
- Key the map by `PyObject *`.
- Include a traverse edge only if `_PyObject_GC_IS_TRACKED(target)` and `gc_is_unreachable(target)` are true.
- Do not use `ob_tid` for SCC metadata; it is already the worklist next pointer.

### 2. Split worklists

Use `WORKSTACK_FOR_EACH_ITER` over `state->unreachable`:

- if selected by the SCC/back-edge coverage pass, `worklist_remove(&iter)` and `worklist_push(to_clear, op)`;
- otherwise move to `skipped` or leave in `state->unreachable` for a later release pass.

It may be cleaner to split into two local worklists and temporarily replace `state->unreachable` when calling delete/release helpers.

### 3. Release skipped objects without `tp_clear`

Because every free-threaded unreachable worklist entry has an extra strong reference, skipped objects need an explicit release path:

```c
static void
release_uncleared_garbage(struct collection_state *state,
                          struct worklist *skipped)
{
    PyObject *op;
    while ((op = worklist_pop(skipped)) != NULL) {
        _PyObject_ASSERT(op, gc_is_unreachable(op));
        gc_clear_unreachable(op);
        state->collected++;
        if (state->gcstate->debug & _PyGC_DEBUG_SAVEALL) {
            PyList_Append(state->gcstate->garbage, op);
        }
        Py_DECREF(op);  // drop worklist reference; no tp_clear
    }
}
```

Then clear selected SCC objects with a refactored `delete_garbage_on_worklist(state, &to_clear)`.

Order choice for the prototype:

1. Split SCC-selected and skipped.
2. Release skipped worklist references without clearing them.
3. Delete/clear selected SCC objects.

This lets acyclic tails deallocate as soon as selected SCC clearing removes their remaining incoming references.  Selected SCC objects remain protected by their own worklist references until `delete_garbage` pops them.

### 4. Preserve SAVEALL behavior

If `_PyGC_DEBUG_SAVEALL` is set, bypass SCC clearing and use existing `delete_garbage(state)` so all collectable objects are appended to `gc.garbage` and no `tp_clear` calls are made.

## Tests

### Existing tests to run

Default build:

```sh
./python -m test test_gc test_gc_stats
./python -m test test_weakref test_finalization
```

Free-threaded build, once implemented:

```sh
./python -m test test_gc test_free_threading.test_gc
```

Also run the crashers manually where appropriate:

```sh
./python Lib/test/crashers/mutation_inside_cyclegc.py
```

### New targeted tests

Add tests under `Lib/test/test_gc.py`, ideally with a tiny `_testcapi` type that counts `tp_clear` calls.  Python-level objects make it hard to count exact `tp_clear` calls because the clear happens in built-in instance/dict/list machinery.

Possible `_testcapi` helper type:

- GC-tracked object with one strong `next` field;
- `tp_traverse` visits `next`;
- `tp_clear` clears `next` and increments a module-visible counter;
- methods to set/get `next` and reset/read the counter.

Target scenarios:

1. Self-cycle: `A -> A`; expect `A` selected and cleared.
2. Two-object cycle with long tail: `A <-> B -> C -> D -> ...`; expect fewer than all unreachable objects to receive `tp_clear` and all objects to be collected.  With back-edge coverage, one of `A`/`B` should usually be enough if both have `tp_clear`.
3. Chain into self-cycle: `Tail1 -> Tail2 -> A -> A`; expect only `A` selected and cleared.
4. Two SCCs connected by a tail: `A <-> B -> C -> D <-> E`; expect at least one object from each cyclic SCC selected, not the acyclic bridge `C`.
5. Weakrefs with callbacks: preserve existing callback behavior.
6. `tp_finalize` resurrection: resurrected objects must be removed before SCC analysis and not cleared.
7. `_PyGC_DEBUG_SAVEALL`: all unreachable objects should still appear in `gc.garbage`; SCC optimization should be bypassed.
8. OOM/fallback path: simulate allocation failure if practical, or guard by code review and debug counter.

## Benchmark/prototype evaluation

Use microbenchmarks that separate SCC size from unreachable tail size:

```python
# Cycle core of 2 objects keeps N tail objects alive.
class Node:
    pass

def make_case(n):
    a = Node(); b = Node()
    a.peer = b; b.peer = a
    cur = b
    for _ in range(n):
        x = Node()
        cur.tail = x
        cur = x
    return a, b
```

Drop external references and call `gc.collect()`.  Compare:

- total collection time;
- `tp_clear` count using the `_testcapi` counter type;
- extra SCC graph scan overhead;
- performance when the whole unreachable set is one large cycle, where SCC adds overhead but skips little or nothing.

## Risks and open issues

1. **Meaning of "only the SCCs".**  This plan treats only cyclic SCCs as eligible for clearing, not every trivial SCC.  Within each cyclic SCC, the preferred prototype selects a clearable object covering each DFS back-edge rather than every object, when practical.
2. **Bad or incomplete `tp_traverse`.**  Existing GC already depends on `tp_traverse`, but SCC selection creates a new way for buggy extensions to skip a useful `tp_clear`.  Keep fallback/debug validation initially.
3. **`tp_clear` may not clear every outgoing edge.**  It is only required to break cycles.  If selected objects survive and continue holding skipped objects, the GIL fallback will clear skipped survivors.  Free-threaded needs a comparable safety strategy or should accept prototype risk.
4. **Memory overhead.**  The SCC pass needs O(N + E) temporary memory.  On failure, fall back to current behavior.
5. **Extra traversal cost.**  The pass adds another full `tp_traverse` over final unreachable objects.  It only helps when skipped `tp_clear` calls are expensive or numerous relative to the cyclic core.
6. **Free-threaded world-stop time.**  Running SCC while stopped increases pause time.  Running after restarting the world is more complex because object graph stability and `ob_gc_bits` membership need careful reasoning.
7. **Stats semantics.**  `gc.collect()` and GC callbacks should continue reporting all collectable unreachable objects, not only SCC-cleared objects.

## Initial implementation recommendation

Start with the default/GIL GC only:

1. Add graph-building and iterative Tarjan in `Python/gc.c`.
2. Add the smaller-subset selection pass: DFS inside each cyclic SCC and select clearable objects that cover DFS back-edges.
3. Insert after `clear_weakrefs(&final_unreachable)`.
4. Keep `SAVEALL` and OOM fallbacks to the existing path.
5. Keep a survivor fallback for skipped objects during the prototype, enabled by default.
6. Add temporary debug counters to quantify skipped `tp_clear` calls and fallback triggers.
7. Add `_testcapi` clear-count test type and targeted tests.
8. Once behavior is validated, port the same graph/Tarjan machinery to `Python/gc_free_threading.c` with explicit skipped-worklist reference release.

## Confirmed decisions and remaining question

1. Prefer clearing a smaller subset when it is not hard.  Plan: select clearable objects covering DFS back-edges inside each cyclic SCC; fall back to selecting the whole cyclic SCC or the old full clearing path if needed.
2. Implement the GIL-enabled GC first.  Free-threaded GC is follow-up work.
3. Fallback policy recommendation: keep the conservative skipped-survivor fallback enabled by default, with counters/logging.  Add a later diagnostic mode that asserts/logs loudly if skipped objects remain after clearing the selected subset.

## Detailed implementation notes for a fresh agent

This section is intended to be enough implementation detail to start coding after a context reset.

### Scope for the first patch

Only implement the GIL-enabled collector in `Python/gc.c` under `#if !defined(Py_GIL_DISABLED)`.  Do not change `Python/gc_free_threading.c` in the first patch except perhaps to leave comments or TODOs.

Add compile-time knobs near the other GC macros:

```c
#define GC_ENABLE_SCC_CLEAR 1
#define GC_DEBUG_SCC_CLEAR 0
```

Keep the feature easy to disable by changing `GC_ENABLE_SCC_CLEAR` to `0`.

### Return convention

Use a helper that splits selected objects into a separate list:

```c
#define SCC_SPLIT_OK 0
#define SCC_SPLIT_FALLBACK 1
#define SCC_SPLIT_OOM (-1)

static int
move_scc_clear_subset(PyGC_Head *unreachable, PyGC_Head *to_clear,
                      Py_ssize_t *num_selected, Py_ssize_t *num_edges);
```

Recommended behavior:

- `SCC_SPLIT_OK`: `to_clear` contains selected objects; `unreachable` contains skipped objects.
- `SCC_SPLIT_FALLBACK`: helper could not prove a safe smaller clear set, but memory is OK.  Caller should use the old full clearing path on `final_unreachable`.
- `SCC_SPLIT_OOM`: temporary allocation failed.  Caller should free any temporary memory, leave GC lists valid, and use the old full clearing path.

Do not set a Python exception for `SCC_SPLIT_OOM`; GC should silently fall back to the old behavior.  Free all temporary allocations on every return path.  The helper should not mutate the `unreachable` or `to_clear` lists until it knows it will return `SCC_SPLIT_OK`.

### Temporary data structures

Use raw memory allocation (`PyMem_RawMalloc`, `PyMem_RawCalloc`, `PyMem_RawRealloc`, `PyMem_RawFree`) because this runs inside GC.

Suggested structs:

```c
typedef struct {
    PyGC_Head *gc;
    Py_ssize_t index;       // Tarjan index, -1 initially
    Py_ssize_t lowlink;
    Py_ssize_t edge_start;
    Py_ssize_t edge_count;
    Py_ssize_t scc_id;      // -1 initially
    Py_ssize_t path_pos;    // -1 when not on subset DFS path
    unsigned char on_stack;
    unsigned char self_loop;
    unsigned char selected;
    unsigned char dfs_color; // 0 white, 1 gray, 2 black
} GCSccNode;

typedef struct {
    PyGC_Head *key;
    Py_ssize_t value;
} GCSccMapEntry;

typedef struct {
    Py_ssize_t node;
    Py_ssize_t next_edge;
    unsigned char entered;
} GCSccFrame;

typedef struct {
    Py_ssize_t size;
    unsigned char cyclic;
} GCSccInfo;
```

The pointer map can be a simple open-addressed hash table:

- table size power-of-two, at least `2 * node_count`;
- empty entry has `key == NULL`;
- hash pointer as `((uintptr_t)gc >> 4) * 11400714819323198485ull` or similar, then mask;
- insert every `PyGC_Head *` from the `unreachable` list;
- lookup returns `-1` if target is absent.

### Graph construction

Build graph in two stages:

1. Count nodes and fill `nodes[i].gc` by iterating:

```c
for (gc = GC_NEXT(unreachable); gc != unreachable; gc = GC_NEXT(gc)) {
    ...
}
```

2. For each node, call `Py_TYPE(FROM_GC(gc))->tp_traverse` with a custom visit callback.

The callback should:

- ignore objects where `_PyObject_IS_GC(op)` is false;
- convert to `PyGC_Head *target = AS_GC(op)`;
- lookup target in the map;
- include the edge only if lookup succeeds and `gc_is_collecting(target)` is true;
- append the target index to the dynamic `edges` vector;
- mark `nodes[source].self_loop = 1` if target index equals source index.

The `gc_is_collecting(target)` check is a useful guard because the final unreachable set should still have `PREV_MASK_COLLECTING` set, while objects outside the set should not be considered SCC edges.

Edge vector growth pattern:

```c
if (edge_count == edge_capacity) {
    Py_ssize_t new_capacity = edge_capacity ? edge_capacity * 2 : 64;
    Py_ssize_t *new_edges = PyMem_RawRealloc(edges,
        new_capacity * sizeof(Py_ssize_t));
    if (new_edges == NULL) { fail_oom; }
    edges = new_edges;
    edge_capacity = new_capacity;
}
edges[edge_count++] = target_index;
```

Before traversing node `i`, set `nodes[i].edge_start = edge_count`.  After traversal, set `nodes[i].edge_count = edge_count - edge_start`.

If the visit callback returns `-1` due to allocation failure, `tp_traverse` should propagate that; the helper should return `SCC_SPLIT_OOM` and the caller should fall back.

Duplicate edges are harmless.  Do not spend time deduplicating them for the prototype.

### Iterative Tarjan outline

Initialize all `nodes[i].index = -1`, `nodes[i].scc_id = -1`, and `nodes[i].on_stack = 0`.

Maintain:

- `Py_ssize_t next_index = 0;`
- `Py_ssize_t scc_count = 0;`
- Tarjan stack `Py_ssize_t *tarjan_stack`, with length `tarjan_top`;
- DFS frame stack `GCSccFrame *frames`, with length `frame_top`;
- parent array `Py_ssize_t *parent`, initialized to `-1`.

Pseudo-code:

```c
for each root v with nodes[v].index == -1:
    push frame {v, 0, 0}; parent[v] = -1;
    while frame stack not empty:
        f = top frame;
        v = f->node;
        if (!f->entered):
            f->entered = 1;
            nodes[v].index = nodes[v].lowlink = next_index++;
            tarjan_stack[tarjan_top++] = v;
            nodes[v].on_stack = 1;

        if (f->next_edge < nodes[v].edge_count):
            w = edges[nodes[v].edge_start + f->next_edge++];
            if (nodes[w].index == -1):
                parent[w] = v;
                push frame {w, 0, 0};
            else if (nodes[w].on_stack):
                nodes[v].lowlink = min(nodes[v].lowlink, nodes[w].index);
            continue;

        pop frame;
        p = parent[v];
        if (p != -1):
            nodes[p].lowlink = min(nodes[p].lowlink, nodes[v].lowlink);

        if (nodes[v].lowlink == nodes[v].index):
            pop tarjan_stack until v;
            assign all popped nodes the current scc_id;
            count SCC size;
            cyclic = (size > 1) or (size == 1 and nodes[v].self_loop);
            scc_count++;
```

Allocate `GCSccInfo *sccs` large enough for `node_count` SCCs, or make a first pass storing per-node SCC IDs and fill info as SCCs are found.

### Clearability-aware smaller-subset selection

After Tarjan, run a second DFS restricted to each cyclic SCC.  This pass chooses selected objects to clear.

Definitions:

```c
static inline int
scc_node_is_clearable(GCSccNode *node)
{
    PyObject *op = FROM_GC(node->gc);
    return Py_TYPE(op)->tp_clear != NULL;
}
```

For each cyclic SCC:

- reset `dfs_color = 0` and `path_pos = -1` for nodes in that SCC;
- run iterative DFS over only edges whose target has the same `scc_id`;
- maintain a path stack of node indexes;
- when entering node `u`: set color gray, `path_pos = path_len`, push `u` on path;
- when exiting node `u`: set color black, `path_pos = -1`, pop `u` from path;
- for an edge `u -> v` where `v` is gray, cover the back-edge cycle:
  1. If `u` has `tp_clear`, select `u`.
  2. Else scan path positions from `path_len - 1` down to `nodes[v].path_pos` and select the first clearable node found.
  3. If no clearable node is found, abort selection and return `SCC_SPLIT_FALLBACK`.

The path scan can make worst-case behavior worse than linear on pathological SCCs.  That is acceptable for the first prototype.  If needed later, optimize by tracking nearest clearable ancestor or a stack of clearable path positions.

Self-loop handling: when `u == v`, this is a gray-edge cycle of length 1.  Select `u` if it has `tp_clear`; otherwise return `SCC_SPLIT_FALLBACK`.

If a cyclic SCC produces no selected nodes despite being cyclic, return `SCC_SPLIT_FALLBACK`.

All arrays used by this pass can be sized to `node_count`: Tarjan stack, frame stack, parent array, path stack, and `sccs`.  The edge vector is the only structure that needs dynamic growth based on traversed edges.

### Splitting GC lists

Only after all allocation, graph construction, Tarjan, and selection have succeeded should the helper mutate GC lists.

Then iterate safely:

```c
for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
    next = GC_NEXT(gc);
    idx = map_lookup(gc);
    if (idx >= 0 && nodes[idx].selected) {
        gc_list_move(gc, to_clear);
    }
}
```

Both `unreachable` and `to_clear` are normal doubly linked GC lists.  Objects in both lists still have `PREV_MASK_COLLECTING` set.  This matches `delete_garbage()` expectations.

### Wiring in `gc_collect_main()`

Replace only the final `delete_garbage()` block after `clear_weakrefs(&final_unreachable)`.

Important stats rule: keep `stats.collected` as the total number of final unreachable collectable objects, not just selected objects.

Suggested code shape:

```c
Py_ssize_t final_count = gc_list_size(&final_unreachable);
stats.collected += final_count;

#if GC_ENABLE_SCC_CLEAR
if ((gcstate->debug & _PyGC_DEBUG_SAVEALL) == 0) {
    PyGC_Head to_clear;
    gc_list_init(&to_clear);
    Py_ssize_t selected = 0;
    Py_ssize_t edges = 0;
    int split = move_scc_clear_subset(&final_unreachable, &to_clear,
                                      &selected, &edges);
    if (split == SCC_SPLIT_OK) {
        delete_garbage(tstate, gcstate, &to_clear, old);

        // Conservative prototype fallback: skipped objects should normally
        // vanish by refcount cascades.  If any remain, use the old behavior.
        if (!gc_list_is_empty(&final_unreachable)) {
#if GC_DEBUG_SCC_CLEAR
            PySys_WriteStderr(
                "gc-scc: survivor fallback for %zd objects\n",
                gc_list_size(&final_unreachable));
#endif
            delete_garbage(tstate, gcstate, &final_unreachable, old);
        }
    }
    else {
#if GC_DEBUG_SCC_CLEAR
        PySys_WriteStderr("gc-scc: split fallback, code=%d\n", split);
#endif
        delete_garbage(tstate, gcstate, &final_unreachable, old);
    }
}
else
#endif
{
    delete_garbage(tstate, gcstate, &final_unreachable, old);
}
```

`_PyGC_DEBUG_SAVEALL` must bypass SCC splitting so `gc.garbage` behavior remains identical to the current collector.

### Why the survivor fallback exists

With SCC splitting, objects left in `final_unreachable` do not receive `tp_clear` immediately.  They are expected to be destroyed when selected objects' `tp_clear` calls break cycles and trigger refcount cascades.  The fallback checks whether that actually happened.

- If `final_unreachable` is empty after clearing `to_clear`, the optimization worked and skipped all remaining `tp_clear` calls.
- If some skipped objects remain, the prototype calls the old `delete_garbage()` path on them.  This preserves current behavior and avoids leaks while measuring how often the smaller clear set was insufficient.
- A later diagnostic mode can replace this fallback with an assertion or loud log to expose cases needing better selection.

Objects deallocated by refcount cascades will remove themselves from whichever GC list they are on during their `tp_dealloc`, so `final_unreachable` remains a valid list while `to_clear` is being processed.

### Tests to prioritize in the first patch

If time is limited, first build and run existing tests:

```sh
./python -m test test_gc test_gc_stats test_weakref
```

Then add targeted tests only after the core implementation works.  The best targeted test requires a `_testcapi` helper type with a counted `tp_clear`; Python-level tests alone cannot reliably count how many underlying `tp_clear` slots were called.
