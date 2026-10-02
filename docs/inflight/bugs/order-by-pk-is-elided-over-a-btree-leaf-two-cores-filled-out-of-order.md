# ORDER BY <pk> is elided over a btree leaf that two cores filled out of order

## What is wrong

When two cores insert into one btree relation with omitted pks, a leaf can
hold the issued ids out of order. The relation still claims `kAscending`,
so an `ORDER BY <pk>` over it is discarded and its rows come back in the
wrong order.

**Issuing and placing an omitted pk are two latched spans, not one.**

- **The id is issued under catalog page 7** by `Catalog::AllocateRowId`
  (`src/server/command_dispatcher.cpp:5051`).
- **Other work runs before the row is placed:** the row borrow
  (`:5056-5060`) and the spill set-up (`:5063-5066`), among other steps.
- **The row is then placed under its leaf** (`InsertIntoRelation`, `:5079`).

So a second core can issue `n + 1` and place it before the first core
places `n`.

**A btree leaf does not keep its slots in key order.**

- The descent picks the leaf whose range covers the id.
- The leaf then appends at its slot count (`leaf.InsertTuple`,
  `src/storage/btree/btree.cpp:930`, which appends at `nr_slots`,
  `src/storage/heap/heap_page.cpp:162-163`).
- So the leaf ends up holding `n + 1` in a lower slot than `n`.

**Only a named key marks a relation unordered.**

- `key_order` turns `kUnordered` only when a named key is admitted below the
  mark (`src/catalog/catalog.cpp:2560-2568`; its cache mirror,
  `src/catalog/catalog_cache.cpp:129-132`).
- An issued id placed out of order changes nothing, so the relation stays
  `kAscending`.

**`kAscending` is what licenses dropping the sort.** While the relation is
`kAscending`, `exec::CompileStepChain` clears the `ORDER BY <pk>` and the
walk emits slot order. `emit_in_key_order` is asked for only on a
`kUnordered` relation (`src/exec/step_compiler.cpp:1863-1891`).

**Two documents say the btree case cannot happen:**

- `docs/spec/heap-and-tuple.md:250-258` states the heap half of this window
  (a refusal), and says *"A btree relation has none, the descent placing
  each id where it sorts"*. The descent places the id in the right **leaf**,
  not in the right **slot**.
- The comment at `src/storage/btree/btree.cpp:943-945` says only a
  caller-supplied id can sort inside a leaf.

**Point lookups are not affected.** `FindSlotForId` falls back to a linear
pass when its binary search misses (`btree.cpp:254-311`).

Verified by reading at `d3d90b5` on `worktree-parallelism-workorder`, found
by that order's review. Not run.

## Smallest reproduction (not run)

1. Start `cores = 2` and create a btree relation.
2. Two sessions on different cores insert with omitted pks.
3. Pause core A between `AllocateRowId` and `InsertIntoRelation`; a test
   seam is needed for this. Core A now holds id 100.
4. Core B issues 101 and places it in the same leaf.
5. Release core A, which places 100.
6. `SELECT id FROM t ORDER BY id` returns 101 before 100.

Without the seam, the same race needs only concurrent omitted-pk inserts
from two cores into one relation, which scenario 0's `cores = 8` cells
run.

## What it costs

**A quiet wrong answer.** `ORDER BY <pk>` returns rows out of order, and
under `LIMIT` it can return a different set of rows. No error is raised,
and `DESCRIBE` still reports `key_order=ascending`.

**Not checked:** whether any range walk stops at the first slot above its
bound, on the assumption that slots are sorted. If one does, a range
predicate would drop rows in the same way.

**A heap relation** (creatable only before SUS-1) has the same window:

- When the race crosses a tail-page boundary, it ends in the refusal the
  spec states (`OutOfRange`).
- Inside one tail page it can misorder, as on a btree, and a heap relation
  can never be `kUnordered`.

## The fix

Not decided. `instructions/v3.0.0/workorder-ba-parallelism.md` BA-R1b and
BA-S1b carry it, with the choice in BA-Q14:

- **(a)** A placement that lands below its leaf's highest live id flips
  `key_order` to `kUnordered` once, as a below-mark named key does.
- **(b)** The id is issued under the hold of the leaf it lands on, so issue
  order and placement order are one.

A heap relation needs (a)'s per-page key-order emission whenever pk order
is asked for.
