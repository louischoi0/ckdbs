# The statement-local inner build (spec, PROPOSED)

Status: **PROPOSED — nothing built.** Every decision below is offered for
ratification, none is made; the items marked `[OPEN]` are open even
within the proposal. The measurements that motivate and bound this
design are in `bench/results-scenario3-library.md` §7e (the cell that
priced the gap) and §7b/§9b (the shapes it would serve).

Related docs: `docs/parser-v2.md` §5 (the contract this must not break),
`docs/feat-cabin.md` §4a (the machinery this reuses), `docs/feat-index.md`
§8a, `docs/crosscore.md`.

---

## 1. The gap, priced

A join on a column with **no index and no Cabin** walks the inner
relation once per outer row — O(outer × inner) — and nothing in the
engine can improve it: propagation needs a literal, IX17 needs an index,
CB12 needs a Cabin and repetition. `bench/results-scenario3-library.md`
§7e measured the three answers at 10,000 loans, k = 16 outer rows:

| answer | stmts/s | needs |
|---|---:|---|
| ckdbs per-outer-row walk | 117 | nothing |
| PostgreSQL hash join | 1,314 | nothing — one build per statement |
| ckdbs Cabin, converged | 14,870 | a declared Cabin and key repetition |

PostgreSQL's row is the gap: a **per-statement build** needs no
declaration and no repetition, and ckdbs has no operator in that class.
§7e.5 records it as "a third answer ckdbs does not have". This spec
proposes that operator, shaped to this engine's contracts.

The target number follows from §7e's own decomposition: one inner pass
(~527–539 µs at 10,000 rows, the floor both engines share) plus k cheap
probes, so ~560–600 µs at k = 16 against the walk's ~8,500 — roughly
PostgreSQL's rate plus the round-trip advantage this engine already has.

## 2. What is proposed, in one paragraph

When a join's inner step would be a **walked join** — a `kScan` whose
residual binds an own column by equality to an earlier step's or an
enclosing chain's column, with no index and no Cabin to serve it — the
executor builds, **once per statement**, an in-memory map from the join
column's values to the matching rows' pks (and location hints), by
letting the **first outer row's inner walk double as the build**. Every
later outer row probes the map instead of walking. The map is discarded
when the statement ends. Nothing is declared, persisted, recorded,
replayed, or shared.

## 3. Why this does not break the written-order contract

`docs/parser-v2.md` §5 (I12) is the constraint: written order is the
plan, the chain runs front to back, decorrelation rewrites are forbidden
by name. Three facts keep the build inside that contract, and the spec
asks for them to be ratified as such:

1. **The outer relation still drives.** The build changes how an inner
   *match set* is located, never which relation iterates or what joins
   what. It is the same claim IX17 and CB12 already ratified: a
   correlated probe of a structure is not a reorder. The build is a
   correlated probe of a structure whose lifetime is one statement.
2. **The lazy build never changes read scheduling.** The inner relation
   is first read exactly when written order says it is — when the first
   outer row reaches the inner step. The build is that walk's side
   effect, precisely the Recording pattern `feat-cabin.md` §4 ratified
   ("it was going to walk anyway; recording is a side effect").
3. **Emission order is untouched.** The map's buckets are appended in
   walk order, so a probe replays each key's matches in exactly the
   order the walk would have emitted them — for both key modes, since
   build order *is* the walk's order whatever `ASSIGNED`/`EXPLICIT`
   makes that order be. (This is stronger than the pk-sort argument
   IX8a and the Cabin serve need, because the build captures order
   rather than reconstructing it.)

What the build must **not** do, stated as hard rules: never build the
outer side; never reorder emission; never survive the statement; never
feed Waystone (search-class, like every set-returning kind).

## 4. Trust class: none, and that is the point

The map is not a fourth trust class. It is the statement's **own read**,
MVCC-filtered under the statement's snapshot at the moment of the build
— and the snapshot is fixed for the statement in every isolation level,
so a row visible at build time is visible at every later probe of the
same statement. There is no write hook (core-ownership dispatch runs the
statement to completion; nothing writes between build and probe), no
observation threshold, no cap-authority question, no persistence class.
`ANALYZE` reports it honestly (`inner_built=1 build_rows=N probes=k`)
and `IsTrailReplayable` does not move.

## 5. The selection rule stays `f(shape, catalog)`

The build is the **last arm of the structure ladder**, tried only when
the pk arms, both index arms, and both Cabin arms declined — i.e. for
exactly the walked-join shape. No statistics, no cardinality estimate:
the lazy form is what removes the need for one. At k = 1 the statement
pays one walk plus the build's per-row constant (the Cabin recording
fix bounds the expectation: ~6% before the same decode discipline is
applied, less after); at k ≥ 2 every avoided walk is pure win. The
crossover PostgreSQL's planner needs statistics to find is dissolved
rather than estimated — the same move CB12 made.

Ladder order is also the economics: a converged Cabin serve (~67 µs)
beats any per-statement rebuild (~560 µs), so banked structures stay
ahead of the build, and the build stays ahead of the walk.

## 6. The sub-chain case reuses CB13's license

A correlated `EXISTS` sub-chain's inner walk **stops at the first
qualifying row**, so the first outer row's walk would build a partial
map — the exact problem CB13 solved for Cabin recordings. The same
mechanism applies verbatim: in sub-chain mode (`record_through_stops_`,
whose comment already generalizes), a live build walks through the stop
to completion, and only a completed walk publishes the map. The caps
lesson transfers too: a build that cannot complete (the cap below)
reverts to per-row walks, and — per CB13's budget rule — completion
rows past the stop must not charge the statement's row budget, for the
§1-class reason ratified there: an accelerator must not turn a
within-budget statement into a refusal.

## 7. Memory, and the cap

The map holds one entry per inner row: the join-column value (bucketed),
the pk, and a location hint — the Cabin's 24-byte entry is the natural
unit, reused rather than redesigned. Bounded by a config knob:

- `join_build_max_rows` `[PROPOSED default: 65536]` — rows, not bytes,
  following `aggregate_max_groups`' argument. **Refusal semantics are
  the Cabin's, not the aggregate's**: past the cap the step reverts to
  per-row walks for the rest of the statement — always legal, never an
  error, because the map is a shortcut and the walk is always there.
  (The aggregate fails its statement because it has no fallback; the
  build always has one.)

`[OPEN]`: whether the knob is per statement or per step when a chain
carries two walked inners; whether the build should decline outright
for an inner relation the catalog knows exceeds the cap (a `sys.tables`
row-count is catalog state, so the decline would stay `f(shape,
catalog)` — but stale counts would make the plan flap, which is why
this is open and not decided).

## 8. Cross-core, and the other exclusions

A build is core-local execution state; the descriptor cannot ship it
and does not need to: `ShippedForm` already downgrades structure-served
steps to their walk, and the build — being execution-time, not a
compiled kind — needs no descriptor presence at all. `[OPEN]`: whether
the peer's consuming stage may build locally for its own stage (it runs
the same executor, so the machinery would work unmodified); deferred
with the rest of the re-derivation question in `feat-index.md` §8a.

Out of scope in v1, by decision: multi-column join keys (CB12's scope
rule), non-equality joins, spill-to-disk, building for a `kFilterScan`
whose literal already bounds it, and any reuse of a map across
statements — that last one is what the Cabin *is*, and building a
second, unauthoritative cache of the same shape would be two structures
answering one question.

## 9. Validation plan, already in place

The driver phases landed with §9b.7's closure measure exactly this
shape: `join-no-literal` and `exists-correlated` under
`--index-mode none` (no `--cabin`) are the build's cells, with
`--verify`'s ordered row-for-row checks as the correctness gate and
§7e's PostgreSQL numbers as the standing comparison. Acceptance: the
`none` cells move from ~8.5 ms / ~1.4 ms to the ~600 µs class without
any other cell moving outside its floor.

## 10. What ratification requires

1. This spec's §3 accepted into `docs/parser-v2.md` §5 as the third
   sanctioned mechanism beside `ORDER BY` and equality propagation —
   with the same "adds, never reorders" framing.
2. The `[OPEN]` items above either decided or carried as open into
   `CLAUDE.md`'s index.
3. A workplan (`docs/workplan-join-inner-build.md`) written only after
   1 and 2 — per the project's spec-first rule.
