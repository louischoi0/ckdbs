# Ratification AX — coalesce on auxiliary DDL, and the decisions around it

Ratified by the operator 2026-08-31 against `main` at `04d53f4`
(`v2.2.1-159-g04d53f4`). Recorded by CLA. Companion build order:
`instructions/v2.7.0/wirkorder-AX.md`.

**What this ratification is about.** `RefuseAuxiliaryOnSplitRelation`
(`src/catalog/catalog.cpp:1094`) declines `CREATE INDEX`, `CREATE CABIN`
(the optimizer's automatic path included), `CREATE ASSERTION` and an FK
naming the relation on either side, on any relation of two or more
ranges. The gate predates DA1 and was reachable only by configuring
`range_size_ids`; DA1 armed spreading by default (65,536), so a range now
opens on workload and an ordinary multi-core session meets the refusal
without choosing to. Nothing merges ranges (the mover is R5, unbuilt), so
write-then-index — the order an ordinary session produces under DA1 —
was permanently refused for the life of the relation, `DROP TABLE` +
recreate the only way back (`known-gaps.md`, the DA1 enactment entry).
**The operator rules this a defect, not a constraint.** These seven
items decide the shape of the fix.

## AX-D1 — the fix is (c): forced coalesce now, placement decisions to R5

CLA's three routes were: (a) forced coalesce — the auxiliary DDL merges
the relation to one range, then builds; (b) ratify each auxiliary's
under-a-boundary placement and lift the gates individually; (c) both, on
different clocks. **The operator takes (c)**: the defect is removed by
(a) immediately, and the five owning `[OPEN]`s — index per-range/global
(`index.md` §13), Cabin under split/migration (`cabin.md` §11),
assertion group-straddle (`assertion.md` §6.1), cross-core FK
(`cross-owner-txn.md`), var-heap partition (`heap-and-tuple.md`) — move
to R5's schedule, cited from blueprint §11's R5 row as "the auxiliary
placement decision group". None of the five is decided here and none may
be assumed by the build.

## AX-D2 — the armed default stays

`range_size_ids = 65,536` remains the default while the fix is in
flight. The consequence is stated rather than hidden: until AX lands,
the write-then-index permanent refusal persists as a known, decided
condition. `known-gaps.md`'s entry is amended from "does not recover" to
"decided 2026-08-31 (AX-D1), fix in flight" by the build order's docs
row.

## AX-D3 — the absorbing core is the one holding the most pages

Merge direction: every non-surviving range's pages move to the core
holding the most pages of the relation, minimizing pages moved.
Two recorded consequences:

- **Tie rule (CLA's proposal, accepted under this ratification's
  standing pattern; proposed, not measured):** on a page-count tie, the
  lowest `core_id` absorbs — determinism and test reproducibility are
  the whole ground.
- **`owner_core` may change.** The absorber can differ from
  `sys.tables.owner_core`, so coalesce includes a catalog update of
  `owner_core` to the absorber — a core-0-stream catalog write (CC11
  unviolated), one more write inside the DDL's scope.

## AX-D4 — the crash contract: proposed from CC10, promoted by simulation

The merge sequence starts as CC10's migration sequence adapted (quiesce →
flush → durable handoff record → durable directory contraction before
any grant → grant → invalidation broadcast), with the open question
named: whether directory-row **deletion** in the position CC10 gives the
row **write** preserves the abort-to-surviving-state property. The
answer is established by the simulation harness's crash injection
(SIM05-07 style, seed-driven, every step boundary), and the sequence is
tagged proposed until that matrix is green, measured thereafter. The
build order's AX3/AX7 rows own it.

## AX-D5 — synchronous execution, two-phase, and the visible residue

The auxiliary DDL performs the merge inline and builds only after it
completes. Two costs follow, both accepted:

- **DDL latency proportional to pages moved.** First-priority
  measurement of the build order (AX8), at the extremes per rule 4b.
- **Two-phase structure.** A page handoff is not undoable by the catalog
  transaction's compensation, so the statement is *merge completes,
  then the existing DDL transaction begins*. If the DDL half then
  fails, **the relation stays merged** — a valid state (one range is
  always valid), but an observable side effect, written into the spec
  rather than discovered.

## AX-D6 — no re-split after an auxiliary exists (proposal accepted)

The current rule stands: once the relation carries an auxiliary,
`RangeEligible` continues to refuse a split. Changing this *is* the
auxiliary-under-a-boundary question, which AX-D1 moved to R5 — so the
line is drawn in the same place twice, deliberately. Consequence,
promoted from defect to rule-until-R5: **spreading and auxiliaries are
mutually exclusive on one relation.** A coalesced relation forgoes
spreading's gain (1.51× group-arm at k = 5) for as long as its
auxiliary lives.

## AX-D12 — the Cabin optimizer's automatic path does not coalesce (proposal accepted)

The controller's auto-`CREATE CABIN` keeps meeting the refusal, exactly
as today, and its decline stays visible in the gate's counters. Ground:
a synchronous merge is a large physical page movement, and an unattended
background controller triggering one is against the enact-through-named-
gates discipline (`physical-optimizer.md` Part I) — physical change
happens where the operator can see it. Only an explicit statement
(`CREATE INDEX` / `CREATE CABIN` / `CREATE ASSERTION` / FK DDL) triggers
coalesce.

## Recorded obligations, not decisions

- **Measurement (rides rule 4b, not separately ratified):** µs per page
  moved and handoff count; coalesce-inclusive DDL latency at maximum
  range count and page count reachable on the host; baseline is this
  engine's own prior numbers.
- **Retro-record:** `ratification-da.md` gains an amendment note that
  DA1's unnamed cost is decided here, so the two documents cite each
  other rather than disagreeing.

## What this ratification does not decide

The five placement `[OPEN]`s (AX-D1 moved them, it did not touch them);
the mover's policy and constants (R5, untouched); any change to
`RangeEligible`'s five gates or the btree decline; D1 of
`workplan-range-directory.md` (btree relations stay unsplittable, which
is what scopes AX's merge to heap chains); the per-core id-space stripe.
