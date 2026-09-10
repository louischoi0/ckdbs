# `BtreeLookup` hands back a (page, slot) it no longer holds, and every caller re-fetches

**Found** 2026-09-10 by the `critics-developer` pass on AT-S5c
(`9785af4` on `m3-at`), as its C2. **Not fixed.** The fix was a design
choice between two shapes, so it went to the operator (`workorder-at-m3-
uniformity.md` AT-0 item 12), and **the operator marked (a) on 2026-09-10**:
`BtreeLookup` returns the held `PageRef`. Marked, not built - it is its own
stage and waits for the word to start it.

**Cost: a quiet wrong answer, and in one caller a wrong constraint verdict.**
A statement answers **zero rows for a row that exists**, and a foreign-key
check reads a different row than the one it asked about.

## The wrong behaviour

AT-S5c made `BtreeLookup`'s **status** authoritative across cores: a `NotFound`
now proves the row is absent rather than proving the descent was outrun
(`src/storage/btree/btree.cpp`, the miss-path coverage check). It left the
**payload** a hint. `BtreeLookup` returns `Location{page_id, slot}` and drops
the leaf's shared hold when it returns, so between the answer and its use the
leaf is held by nobody.

Every caller then re-fetches by id, each carrying the same comment — *"re-fetched
by id rather than carried out of the lookup … a hash hit on a still-resident
frame"*: `src/exec/step_vm.cpp:596`, `:1386`, `:1515`,
`src/exec/fk_check.cpp:99`, `:259`, `src/server/command_dispatcher.cpp:9072`,
`src/server/cabin_optimizer_exec.cpp:367`.

**A divide compacts slots.** `SplitLeafAndInsert` rebuilds the old leaf with
`CreateEmptyAs` and re-inserts the staying half, so slot indices are not
preserved. The window between the lookup's return and the caller's re-fetch is
therefore a window in which slot *n* becomes a different row.

- **`step_vm.cpp`**: the wrong row is read, `Step::residual` repeats the key and
  drops it, and the statement answers **zero rows**. The residual is what turns
  a wrong read into a silent empty answer rather than a wrong one.
- **`fk_check.cpp:104`** has no residual behind it: `ReadTuple(slot)` on the
  wrong row decides the verdict, and a `NotFound` on a retired slot reads as
  `kViolation`.

**AT-S5c's own cell cannot see this.** `ALookupDoesNotMissARowADivideMovedUnderIt`
asserts on `BtreeLookup`'s status, which is exactly the half that was fixed.

## Reproduction

Two cores, one btree relation. Core 1 inserts a caller-named key that sorts
inside a full leaf (the divide, `heap-and-tuple.md` §4.1). Core 0 runs
`SELECT * FROM t WHERE id = K` for a `K` in that leaf's staying half, timed so
its `BtreeLookup` returns before the divide and its re-fetch lands after.

## The two shapes, and which is honest

- **(a) Return the held leaf.** `BtreeLookup` hands back the `PageRef` beside
  the `Location`, which is what `Descent` already produces and what "Shape C"
  (`btree.cpp`'s `Descent` comment) deliberately dropped. Six callers change.
  **Correct by construction**, and the only version that makes the header's
  *"the tree is the relation's storage, not a hint over it"* true of the
  callers as well as of the function.
- **(b) Verify at the slot.** Callers re-check the Keystone id after the
  re-fetch and re-look-up on a mismatch. Cheaper, needs a bound, and leaves the
  contract a hint.

CLA proposed (a), and **(a) is the operator's mark** (2026-09-10): the cost it
reinstates is a held pin across the caller's read, which is what every other page
access in the engine already holds, and (b) spreads the invariant across seven
sites that must each remember it.

## The same exposure, and worse, one layer up

`step_vm.cpp:562`'s probe memo (`memo_page_`, `memo_slot_`) carries a location
**across statements and suspension points**. `SplitLeafAndInsert` bumps
`relayout_epoch` precisely so a stale location can be caught — the Waystone
trail checks it, this memo does not.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-0 item 12.
`docs/spec/heap-and-tuple.md` §5 states the descent's authority and is where
the answer lands.
