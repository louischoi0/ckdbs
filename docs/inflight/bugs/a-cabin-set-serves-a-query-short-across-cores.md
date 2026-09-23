# A Cabin set serves a query short when another core wrote the relation

**Found** 2026-09-23 by AT-S6's survey, on `at-s6-2pc-retired` at
`e89b398`. **Not fixed** — the fix is AT-S7's, which makes the store the
instance's (AT-0 item 9). Verified by a cell:
`tests/cabin_serve_across_cores_test.cpp`, landed **disabled** because it
is the cell that stage owes.

**Cost: a quiet wrong answer on an ordinary `SELECT`.** A query with an
equality on a Cabin'd column answers a **subset** of the rows that match —
no error, no refusal, and the same statement answers correctly the moment
the Cabin is dropped.

## The wrong behaviour

This is D4's family — the one AT-S5f fixed for the foreign key's reverse
check — on the path the Cabin exists for.

`stats::CabinStore` is a dispatcher's own (`CoreRuntime` holds one,
`Expeditor` holds core 0's), and the write path files an entry into the
**inserting** core's store (`NoteCabinWrite`). Until AT-S5 a relation's
writes all ran on its owner, so one store saw every write and an
exhausted, all-non-matching scan of an observed value's set really was
every row carrying that value — the superset argument `cabin.md` §4b
rests on. **AT-S5 made a write run where the session is**, so two cores
write one relation and each store holds the half it wrote.

`CabinScopeCovers` (`src/exec/step_vm.cpp`) then admits the serve: it
answers `true` immediately for a relation with no directory, which is
every unsplit relation whoever owns it. The set is served, and the rows
another core wrote are not in it.

## Reproduction

`tests/cabin_serve_across_cores_test.cpp`, `DISABLED_` on the two-core
rig. One relation, `CREATE CABIN ON r0(v)`, and the peer funded with a
row-id block so the two cores' ids are distinguishable:

1. core 0 writes a row with `v = 7` (it takes id 17, the block after the
   peer's);
2. core 0 runs `SELECT id FROM r0 WHERE v = 7`, which **walks** and banks
   a set for the value 7 that is complete as of now;
3. a session on core 1 writes a second row with `v = 7` (it takes id 1
   from the peer's block), filing its entry into core 1's store;
4. core 0 runs the same `SELECT` again.

Measured: the second answer is `id\n17` — one row. `SELECT COUNT(*)`
answers 2 from the walk in the same test, which is what makes the answer
short rather than right.

## What it is not

Not the fan-in's scope rule. `cabin.md` §4b rule 3 and `CabinScopeCovers`
are about a **split** relation, where a set covering ranges a stage does
not walk would double-emit; this is an **unsplit** relation, which that
rule waves through in one branch. The rule was right when one core wrote.

Not reachable without a declared Cabin: `CREATE CABIN` is opt-in, and a
relation with none walks.

## The fix

**AT-S7: one `stats::CabinStore` for the instance** (AT-0 item 9, whose
two shapes are AR1 §11's). Then a set is again a superset of every pk
carrying the value, and the serve is authoritative as written.

Until then, two things hold it down and neither is the fix: a relation
with no Cabin is unaffected, and **AT-S6 does not widen it** — a read
made local by that stage consults the Cabin only for a relation this core
owns, which is the state that was already exposed.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S7 and AT-0
item 9. `docs/spec/cabin.md` §4b and §6a are the rules involved.
