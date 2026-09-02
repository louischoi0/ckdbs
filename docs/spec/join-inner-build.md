# The statement-local inner build (spec)

Status: **built.** §3 is accepted into `docs/spec/parser-v2.md` §5 as the
third sanctioned mechanism beside `ORDER BY` and equality propagation.

Related docs: `docs/spec/parser-v2.md` §5 (the contract this must not break),
`docs/spec/cabin.md` §4a (the machinery this reuses), `docs/spec/index.md`
§8a, `docs/spec/crosscore.md`.

---

## 1. The gap

A join on a column with **no index and no Cabin** walks the inner
relation once per outer row — O(outer × inner) — and nothing else in the
engine improves it: equality propagation needs a literal, the correlated
index probe (`index.md` §8a) needs an index, the correlated Cabin probe
(`cabin.md` §4a) needs a Cabin and key repetition. A **per-statement
build** needs no declaration and no repetition; this spec is that
operator, shaped to this engine's contracts. Its cost floor is one inner
pass plus k cheap probes, and the inner pass is work no design removes.

## 2. What it is, in one paragraph

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

`docs/spec/parser-v2.md` §5 (I12) is the constraint: written order is the
plan, the chain runs front to back, decorrelation rewrites are forbidden
by name. Three facts keep the build inside that contract:

1. **The outer relation still drives.** The build changes how an inner
   *match set* is located, never which relation iterates or what joins
   what. It is the same claim the correlated index probe and the
   correlated Cabin probe make: a correlated probe of a structure is not
   a reorder. The build is a correlated probe of a structure whose
   lifetime is one statement.
2. **The lazy build never changes read scheduling.** The inner relation
   is first read exactly when written order says it is — when the first
   outer row reaches the inner step. The build is that walk's side
   effect, precisely the Recording pattern `cabin.md` §4 states
   ("it was going to scan anyway; recording is a side effect").
3. **Emission order is untouched.** The map's buckets are appended in
   walk order, so a probe replays each key's matches in exactly the
   order the walk would have emitted them — for a named key and an
   issued one alike, since build order *is* the walk's order whatever
   the relation's `key_order` makes that order be. (This is stronger
   than the pk-sort argument IX8a and the Cabin serve need, because the
   build captures order rather than reconstructing it.)

What the build must **not** do, stated as hard rules: never build the
outer side; never reorder emission; never survive the statement; never
feed Waystone (search-class, like every set-returning kind).

## 4. Trust class: none, and that is the point

The map is not a fourth trust class. It is the statement's **own read**,
MVCC-filtered under the statement's snapshot at the moment of the build
— and the snapshot is fixed for the statement in every isolation level,
so a row visible at build time is visible at every later probe of the
same statement. There is no write hook (a SELECT statement writes nothing
between build and probe — and only SELECT compiles the build: a DML
statement's `WHERE` sub-chain is excluded in §8, because its own writes
between outer rows are exactly what would invalidate the map), no
observation threshold, no cap-authority question, no persistence class.
`ANALYZE` reports `inner_built=1 build_rows=N build_probes=k−1`:
`build_probes` counts the outer rows *served from* the map, which is
k−1 because the first row's walk was the build; `build_rows` counts rows
*bucketed*, which a discarded map keeps, so `inner_built=0 build_rows=N`
is the honest rendering of "it got N rows in and threw them away".
`IsTrailReplayable` does not move.

## 5. The selection rule stays `f(shape, catalog)`

The build is the **last arm of the structure ladder**, tried only when
the pk arms, both index arms, and both Cabin arms declined — i.e. for
exactly the walked-join shape. No statistics, no cardinality estimate:
the lazy form is what removes the need for one. At k = 1 the statement
pays one walk plus the build's per-row constant, and a stopping
sub-chain pays it only on the prefix it walks anyway (§6). The
crossover a planner needs statistics to find is dissolved rather than
estimated — the same move the correlated Cabin probe made.

The constant is a real quantity and belongs in this rule: it is **paid
on every bucketed inner row of the first walk**. Measured against the
`bench/` tree at `1769487`, it is **37.2 ns/row** at 10,000 inner rows,
which puts the plain join's break-even **under k = 2 at every row-set
size**; k = 1 pays it with no payback. Two rules follow:

- **The build's own cost is the constant, and driving it down is the
  design work** — not deciding *when* to pay it. Lowering it moves
  break-even and shrinks the k = 1 loss in one motion, where any arming
  policy can only move the loss from one k to another.
- **Deferring the build to the second outer row is declined.** It would
  make k = 1 free by paying the walk *and* the build at k = 2, moving
  the loss rather than removing it and giving back most of the win
  above it, for a shape whose entire reason to exist is k ≫ 1. The
  decline rests on arithmetic over the measured parts, not on a
  measured deferring build; reopening it means building one and
  measuring it.

Ladder order is also the economics: a converged Cabin serve beats any
per-statement rebuild, so banked structures stay ahead of the build, and
the build stays ahead of the walk.

## 6. The stopping sub-chain: a prefix map, positive-first

A correlated `EXISTS`-class sub-chain's inner walk **stops at the first
qualifying row**, so the first outer row's walk yields a partial map.
The design does not complete it — it makes partiality safe:

- **The map is a walk-order prefix.** Rows are bucketed up to a
  high-water mark, and every walk traverses the engine's one walk
  order, so what the map covers is a position, not a guess.
- **A hit is conclusive.** A bucketed row, re-checked against the full
  residual, proves the row exists — the positive-only rule
  `docs/spec/parser-v2.md` §6 states for `Exists` replay: a partial
  structure can prove presence, never absence.
- **A miss resumes the walk at the mark.** Rows before the mark are
  exactly the bucketed ones and the probe already answered them for
  this key, so the resumed walk starts where the last one stopped,
  extends the map, and advances the mark. A walk that reaches the end
  completes the map, and later misses become conclusive absences.

**Every inner row is visited at most once per statement**, so the
statement pays at most one full pass plus probes, with no earn gate, no
publication gate, and no budget carve-out (every charged row is a row the
statement's own walk visits). The plain join of §2 is the degenerate
case: its first walk never stops, so the mark reaches the end immediately
and the map is total from the second outer row on.

What the prefix trades is **the sum of the per-outer-row walks for the
longest single walk, plus the build constant on every row of that walk**,
so the crossover is where the sum exceeds the max by more than the
constant. Measured against the `bench/` tree at `1769487` at the
37.2 ns/row constant, that crossover is **k ≈ 5**, with k = 4 at +8% (100
rows per key) to +11% (5 rows per key) and k = 16 at −18% to −59%; the
rule holds at every k, its cost does not.

The class is what compiles to an `Exists`-kind stopping walk —
`EXISTS`, `NOT EXISTS` (its hit proves existence, its completed miss
proves absence; both conclusive), and `IN (subquery)`'s per-row form
(parser-v2 §2: `IN` compiles to `Exists`). A scalar sub-chain also
stops early, but its cardinality check is conclusive only against a
*complete* map; it stays excluded (§8).

Under the cap (§7) a frozen map stops extending but keeps serving its
prefix: hits stay conclusive, misses walk from the frozen mark — still
never worse than the plain walk.

The alternative form — walk on through the stop and publish only a
completed map, the license `cabin.md` §4a grants a recording Cabin walk —
is **rejected** for the build: it prices a data-dependent break-even and
pays unearned completion walks, which the prefix form deletes along with
the publication gate and the budget carve-out.

## 7. Memory, and the cap

The map holds one entry per inner row: the join-column value (bucketed),
the pk, and a location hint — the Cabin's 24-byte entry is the natural
unit, reused rather than redesigned. Bounded by a config knob:

- `join_build_max_rows` (default `65536`, `kDefaultJoinBuildMaxRows`) —
  rows, not bytes, following `aggregate_max_groups`' argument. **Refusal
  semantics are the Cabin's, not the aggregate's**: past the cap the step
  reverts to per-row walks for the rest of the statement — always legal,
  never an error, because the map is a shortcut and the walk is always
  there. (The aggregate fails its statement because it has no fallback;
  the build always has one.) `0` disables the build outright — the
  opposite reading of the row-touch budget's zero, deliberately, because
  "unlimited map" has a number where "unlimited work" does not.

The knob is one number checked per build; whether it scopes per statement
or per step when a chain carries two walked inners, and whether a catalog
row count may decline a build, are decisions unrecorded here.

## 8. Cross-core, and the other exclusions

A build is core-local execution state; the descriptor cannot ship it
and does not need to: `ShippedForm` already downgrades structure-served
steps to their walk, and the build — being execution-time, not a
compiled kind — needs no descriptor presence at all. A peer's consuming
stage does not build for its own stage; whether it may is a decision
unrecorded here.

Out of scope, by decision: multi-column join keys (the Cabin's scope
rule, `cabin.md` §2); non-equality joins; spill-to-disk; building for a
`kFilterScan` whose literal already bounds it (it still walks per outer
row, so the same win is forgone — a decision, not an impossibility);
**any sub-chain compiled through `CompileWhere`** — the build is a SELECT
feature, because a DML statement's own writes between outer rows
invalidate a map its first outer row built (§4's no-write argument is a
SELECT argument); **scalar sub-chains** (§6's conclusiveness needs
`Exists` semantics); and any reuse of a map across statements — that last
one is what the Cabin *is*, and building a second, unauthoritative cache
of the same shape would be two structures answering one question.

## 9. Validation

`tools/scenario3_library.py`'s `join-no-literal` and `exists-correlated`
phases under `--index-mode none` (no `--cabin`) are the build's cells,
with `--verify`'s ordered row-for-row checks as the correctness gate. The
join cell's acceptance is a **ratio against the walk**, never a
wall-clock class: the walk statement's other cost — client, socket, the
outer range, and the one inner pass — is what no map reaches.

## 10. Ratification

Discharged: §3 is `parser-v2.md` §5's third sanctioned mechanism, and the
decisions §7 and §8 leave unrecorded are carried in `CLAUDE.md`'s index.
