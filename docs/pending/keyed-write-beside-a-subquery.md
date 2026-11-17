# A write whose `WHERE` holds a subquery walks its relation twice, whatever key the clause names

Status: **PENDING.** Raised 2026-11-17 on the operator's word
(`instructions/v3.2.0/raft-marks-2026-11-17.md` §3), which took the keyed
write beside other conjuncts into work order CU as CU-S3b and left this
case out of it. Every `[source-read]` line number is on worktree
`cu-key-set-access` at `04d9afe9` (CU-S3b). Related:
`instructions/v3.2.0/workorder-cu-key-set-access.md` §4 (CU-Q14), §7
(CU-S3b); `docs/spec/parser-v2.md` I10, R2; `docs/rules/rules.md` §3 (the
page latch row); `docs/inflight/known-gaps.md`, "Key sets (CU)".

## What prompted it

`[operator]` CU-S3's report noted that a write descends only where the
tuple key's conjunct is its whole `WHERE`, though the row function asks
the whole clause of the row a descent finds. The operator took the
widening into CU and deferred one case: *"Take it into CU, leave the
subquery case as pending work, and carry on with the work"* (2026-11-17,
given in Korean; recorded in English).

`[source-read]` Since CU-S3b an `UPDATE` or `DELETE` descends for
`id = 1 AND v = 3` and for `id IN (1, 2) AND v = 3`. It does not for
`id = 1 AND v IN (SELECT ...)`: `exec::WriteKeyOf` and
`exec::WriteKeySetOf` name no target for a `WHERE` with a sub-chain
(`src/exec/step_compiler.cpp:2256`, `:2266`).

## The tree as read

- `[source-read]` **Why a write evaluates no subquery under its leaf.** A
  descent and a write walk hold the leaf exclusive while the row function
  runs. A sub-chain evaluated there reads other pages under that hold: a
  held `X` asking an `S`, which two statements on two cores close into a
  cycle no latch wait breaks (`docs/rules/rules.md` §3, the page latch
  row). Over the relation the statement writes, a sub-chain run per row
  would also read rows the statement has already written
  (`docs/spec/parser-v2.md` R2).
- `[source-read]` **What it does instead**
  (`CommandDispatcher::WriteWalk::CollectTargets`,
  `src/server/command_dispatcher.cpp:10174`). A read walk visits every
  row of the relation, asks the whole `WHERE` of it - sub-chains included,
  through `exec::EvaluateConjuncts` (`src/exec/step_vm.cpp:3029`) - and
  collects the tuple keys that matched. A write walk then visits every
  slot of every leaf under a write latch and writes the rows whose key is
  collected, their `WHERE` not asked again (`:10058`). The set is carried
  across a park (`WalkCursor::targets`, `include/kds/server/session.hpp:107`),
  so a resumed statement does not collect again. A pass that collects
  nothing ends the statement before the write walk.
- `[source-read]` Both passes are walks whatever the clause names. `UPDATE
  w SET v = 4 WHERE id = 20 AND v IN (SELECT v FROM w WHERE id = 21)`
  takes every leaf of `w` exclusive
  (`tests/key_set_test.cpp`,
  `KeySetWriteHoldTest.AWriteBesideASubqueryOverABoundOrByAnotherColumnsListWalks`).
- Not traced: whether an uncorrelated sub-chain is answered once for the
  statement or once per row in the collection pass. `EvaluateConjuncts`
  builds a runner for the row in hand; the read path keeps a hoisted
  chain's answer per statement (`StatementStore::hoisted`).
- Not measured: how often a write carries both a tuple key and a
  subquery. No census of the shape exists.

## Options `[design]`

Each is unbuilt and unmeasured.

- **(a) Key the write pass by the collected targets.** The collected set
  is a set of tuple keys, so the second pass could be one descent per
  target, ascending, through the loop CU-S3 built
  (`WriteWalk::WriteKeySet`) - for every write with a subquery, whether
  or not its `WHERE` names a key. The exclusive walk goes; the read walk
  stays. A target set near the relation's size is slower by descents than
  by the walk, so this needs either a rule in the shape alone or the
  cardinality choice CU-Q3 (a) declined for reads. A resume already
  carries the targets and a position by key.
- **(b) Key the collection pass by the clause's key.** Where the `WHERE`
  names a tuple key or a list, the read pass descends to those keys
  instead of walking, under the shared hold it walks under today, and
  asks the whole clause of each row found. Alone it leaves the exclusive
  walk; with (a) the statement touches the leaves its keys are in, twice.
- **(c) Answer an uncorrelated subquery before the leaf is taken.** A
  sub-chain with no reference to the written row has one answer for the
  statement. Folded to a value or a set before the descent, the clause
  holds no sub-chain to evaluate under the leaf and the point arm could
  take it as it takes any other conjunct. It does nothing for a
  correlated subquery, and R2's half of the reason - a subquery over the
  written relation must not see the statement's own writes - has to be
  kept by answering it before the first row is written.
- **(d) Leave it.** The statement is correct today and costs two walks.

Each of (a) to (c) needs its own argument for the two reasons above, and
a test that the statement leaves what today's two walks leave.

## What a ruling needs

Whether the shape is worth a stage - a census of client statements, or a
measurement of the two walks against a descent at a relation size that
matters - and, for (a), where the rule that picks descents over the walk
would live.
