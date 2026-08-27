# The statement-local inner build (spec, RATIFIED)

Status: **RATIFIED 2026-08-19 — nothing built.** §10's gate is
discharged: §3 is accepted into `docs/spec/parser-v2.md` §5 as the third
sanctioned mechanism (the amendment of the same date), the `[OPEN]`
items of §7/§8 are carried open into `CLAUDE.md`'s index rather than
decided, and the workplan is `docs/workplan-join-inner-build.md`
(JB1–JB8). The measurements that motivate and bound this design are in
`bench/results-scenario3-library.md` §7e (the cell that priced the gap)
and §7b/§9b (the shapes it would serve).

Related docs: `docs/spec/parser-v2.md` §5 (the contract this must not break),
`docs/spec/cabin.md` §4a (the machinery this reuses), `docs/spec/index.md`
§8a, `docs/spec/crosscore.md`.

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
(§7e's ~530–550 µs at 10,000 rows, the floor both engines share) plus k
cheap probes, so ~560–600 µs at k = 16 against the walk's ~8,500 — roughly
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

`docs/spec/parser-v2.md` §5 (I12) is the constraint: written order is the
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
   effect, precisely the Recording pattern `cabin.md` §4 ratified
   ("it was going to scan anyway; recording is a side effect").
3. **Emission order is untouched.** The map's buckets are appended in
   walk order, so a probe replays each key's matches in exactly the
   order the walk would have emitted them — for a named key and an issued one alike, since
   build order *is* the walk's order whatever the relation's `key_order`
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
same statement. There is no write hook (a SELECT statement writes nothing between build
and probe — and only SELECT compiles the build: a DML statement's
`WHERE` sub-chain is excluded in §8, because its own writes between
outer rows are exactly what would invalidate the map), no
observation threshold, no cap-authority question, no persistence class.
`ANALYZE` reports it honestly (`inner_built=1 build_rows=N
build_probes=k−1` as built — JB7 renamed this sketch's `probes=k` twice
over: the counter is the outer rows *served from* the map, which is k−1
because the first row's walk was the build, and a bare `probes=` on a
line whose second token is the access kind would read as pk `Probe`s on
a `Scan` step. `build_rows` counts rows *bucketed*, which a discarded
map keeps — `inner_built=0 build_rows=N` is the honest rendering of "it
got N rows in and threw them away")
and `IsTrailReplayable` does not move.

## 5. The selection rule stays `f(shape, catalog)`

The build is the **last arm of the structure ladder**, tried only when
the pk arms, both index arms, and both Cabin arms declined — i.e. for
exactly the walked-join shape. No statistics, no cardinality estimate:
the lazy form is what removes the need for one. At k = 1 the statement
pays one walk plus the build's per-row constant, and a stopping
sub-chain pays it only on the prefix it walks anyway (§6). The
crossover PostgreSQL's planner needs statistics to find is dissolved
rather than estimated — the same move CB12 made.

**Amended 2026-08-20, by measurement.** This paragraph used to read "at
k ≥ 2 every avoided walk is pure win", and bounded the k = 1 cost at
"~6%" from the Cabin recording fix. Both were wrong, and the JB5 gate
priced them: the constant is **paid on every bucketed inner row of the
first walk**, which at 10,000 inner rows was 83.7 ns/row — 866 µs
against a 600 µs walk, so k = 1 cost +137% and break-even sat at
k ≈ 2.6, not 2. The constant is a real quantity and belongs in this
rule, so the rule now names it rather than an expectation:

- **The build's own cost is the constant, and driving it down is the
  design work** — not deciding *when* to pay it. Halving it moves
  break-even and shrinks the k = 1 loss in one motion, where any arming
  policy can only move the loss from one k to another. The
  post-JB5 follow-up (workplan §"The build constant") took it to
  **43.2 ns/row**, break-even **under k = 2 at every row-set size**,
  k = 1 at +20%/+41%/+70% (200/1,000/10,000 inner rows, from
  +26%/+80%/+139%) and k = 2 a 0.4%/6%/11% win.
- **k = 1 pays the constant with no payback, and no arming rule this
  engine can afford avoids it.** Deferring the build to the second
  outer row (the Cabin's ratified `kAutoRecordThreshold` n = 2) makes
  k = 1 free. What it costs is arithmetic on the measured parts — at
  10,000 inner rows a walk of 565 µs per outer row, a build of 432 µs,
  a probe of ~1 µs: k = 2 would pay 565 + (565 + 432) = 1,562 µs
  against the walk's 1,131 (**+36%**, where the eager build wins 11%),
  and k = 3 would win 8% where the eager build wins 40%. It moves the
  loss from k = 1 to k = 2 and gives back most of the win above it, for
  a shape whose entire reason to exist is k ≫ 1. **Declined on that
  arithmetic**, not
  on a measured configuration: a deferring build was never built, and
  reopening this means building one and measuring it, not re-reading
  this paragraph.

Ladder order is also the economics: a converged Cabin serve (~67 µs)
beats any per-statement rebuild (~560 µs), so banked structures stay
ahead of the build, and the build stays ahead of the walk.

## 6. The stopping sub-chain: a prefix map, positive-first

A correlated `EXISTS`-class sub-chain's inner walk **stops at the first
qualifying row**, so the first outer row's walk yields a partial map.
The ratified design does not complete it — it makes partiality safe:

- **The map is a walk-order prefix.** Rows are bucketed up to a
  high-water mark, and every walk traverses the engine's one walk
  order, so what the map covers is a position, not a guess.
- **A hit is conclusive.** A bucketed row, re-checked against the full
  residual, proves the row exists — the positive-only rule
  `docs/spec/parser-v2.md` §6 already ratifies for `Exists` replay: a
  partial structure can prove presence, never absence.
- **A miss resumes the walk at the mark.** Rows before the mark are
  exactly the bucketed ones and the probe already answered them for
  this key, so the resumed walk starts where the last one stopped,
  extends the map, and advances the mark. A walk that reaches the end
  completes the map, and later misses become conclusive absences.

The economics that ratified this form: **every inner row is visited at
most once per statement**, so the statement pays at most one full pass
plus probes, with no earn gate, no publication gate, and no budget
carve-out (every charged row is a row the statement's own walk visits).
The plain join of §2 is the degenerate case: its first walk never
stops, so the mark reaches the end immediately and the map is total
from the second outer row on.

**Amended 2026-08-20, by measurement** (workplan JB6). This paragraph
also said "at or below the plain walk's cost at every k", and that is
false for the same reason §5's "pure win at k ≥ 2" was: it counts rows
visited and not the nanoseconds of visiting them. What the prefix
actually trades is **the sum of the per-outer-row walks for the longest
single walk, plus the build constant on every row of that walk**, so
the crossover is where the sum exceeds the max by more than the
constant — measured at **k ≈ 5** once the constant was cut a third time
(37.2 ns/row), with k = 4 at +8% (100 rows per key) to +11% (5 rows per
key) and k = 16 at −18% to −59%. The acceptance cell of §9 passes at
k = 20 (1,681.3 → 547.2 µs, ×3.07).
The rule stands as ratified; what changed is the claim about its cost
at small k, which no longer says "every".

The class is what compiles to an `Exists`-kind stopping walk —
`EXISTS`, `NOT EXISTS` (its hit proves existence, its completed miss
proves absence; both conclusive), and `IN (subquery)`'s per-row form
(parser-v2 §2: `IN` compiles to `Exists`). A scalar sub-chain also
stops early, but its cardinality check is conclusive only against a
*complete* map; it stays excluded (§8).

Under the cap (§7) a frozen map stops extending but keeps serving its
prefix: hits stay conclusive, misses walk from the frozen mark — still
never worse than the plain walk.

History, because the first proposed form was reviewed out on
ratification day: the CB13-license design (walk through the stop,
publish only completed maps) prices at ~13 partial walks per completion
on `bench/results-scenario3-library.md` §7c.4's data (~762 rows/key
against a 10,000-row pass) — a data-dependent break-even of k ≈ 13 and
a ~2.4× regression at k = 4 — and CB14 is the same-day precedent
against paying an unearned recording walk. The prefix form deletes the
completion walk, the publication gate and the CB13 budget carve-out in
one move.

## 7. Memory, and the cap

The map holds one entry per inner row: the join-column value (bucketed),
the pk, and a location hint — the Cabin's 24-byte entry is the natural
unit, reused rather than redesigned. Bounded by a config knob:

- `join_build_max_rows` `[RATIFIED default: 65536]` — rows, not bytes,
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
with the rest of the re-derivation question in `index.md` §8a.

Out of scope in v1, by decision: multi-column join keys (CB12's scope
rule); non-equality joins; spill-to-disk; building for a `kFilterScan`
whose literal already bounds it (it still walks per outer row, so the
same win is forgone — a decision, not an impossibility); **any
sub-chain compiled through `CompileWhere`** — v1 is a SELECT feature,
because a DML statement's own writes between outer rows invalidate a
map its first outer row built (§4's no-write argument is a SELECT
argument); **scalar sub-chains** (§6's conclusiveness needs `Exists`
semantics); and any reuse of a map across statements — that last one is
what the Cabin *is*, and building a second, unauthoritative cache of
the same shape would be two structures answering one question.

## 9. Validation plan, already in place

The driver phases landed with §9b.7's closure measure exactly this
shape: `join-no-literal` and `exists-correlated` under
`--index-mode none` (no `--cabin`) are the build's cells, with
`--verify`'s ordered row-for-row checks as the correctness gate and
§7e's PostgreSQL numbers as the standing comparison. Acceptance: the
join cell moves from ~8.5 ms (§7e.5's 117 stmts/s) to the ~600 µs
class, and the EXISTS cell from ~979 µs (§7c.3's pooled walk — §7e.4's
1.4 ms is PostgreSQL's row, not this engine's baseline) toward the same
class, without any other cell moving outside its floor. Neither driver
phase has a published ckdbs number yet (§9b.7), so JB8 establishes the
baselines it is then judged against.

**Amended 2026-08-21 by JB8's closing measurement**
(`bench/results-scenario3-library.md` §7g, at `aa3e26c`). The EXISTS
half was met — 1,598.8 → 534.3 µs, ×2.99, inside the class. **The join
half's target was arithmetically unreachable, and this section is where
the error was.** The ~600 µs class was derived from §7e's measurement of
*one pass of the inner relation*; on the box JB8 ran, that same pass
costs 668 µs, so the target sits below the cost of the work the design
explicitly cannot remove. §7g.2 shows the difference is a whole-run
offset, not a regression. Make the build free and the cell still lands
at ~682 µs.

So the join cell's honest acceptance is **not** a wall-clock class but a
ratio: at `aa3e26c` it runs ×9.60 against the walk (103 → 989 stmts/s,
closing the gap to PostgreSQL from 1.92× to 1.28×), and the build's own
cost is **33% of the statement** against 57% at `2755045`. The other 66%
is the k = 1 walk statement — client, socket, the outer range, and the
one pass — which no arming rule and no cheaper map reaches. This is the
third ratified claim in this spec that measurement retracted: §5's "at
k ≥ 2 every avoided walk is pure win", §6's "at or below the plain walk
at every k", and now §9's class. All three were reasoned from rows
visited; none of them priced the nanoseconds of visiting one.

## 10. What ratification requires

1. This spec's §3 accepted into `docs/spec/parser-v2.md` §5 as the third
   sanctioned mechanism beside `ORDER BY` and equality propagation —
   with the same "adds, never reorders" framing.
2. The `[OPEN]` items above either decided or carried as open into
   `CLAUDE.md`'s index.
3. A workplan (`docs/workplan-join-inner-build.md`) written only after
   1 and 2 — per the project's spec-first rule.

All three discharged 2026-08-19, in that order; the status line at the
top carries where each landed. §6's first form (the CB13 license) was
replaced the same day by the review that priced it — the history is in
§6 itself.
