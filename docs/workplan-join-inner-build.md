# Workplan — the statement-local inner build

Tasks `JB1`–`JB8`, the artifact `docs/spec-join-inner-build.md` §10 gates
behind ratification (discharged 2026-08-19). The spec owns every design
argument; this file owns the build order, the seams, and the gates.
**JB1 and JB2 are built (2026-08-20); JB3–JB8 are not.** The sanction is
`docs/parser-v2.md` §5's amendment of 2026-08-19; the price of not
having it is `bench/results-scenario3-library.md` §7e (117 stmts/s
against PostgreSQL's 1,314 on the shape neither engine can index away).

## What JB1 declines — the reasons are spec §8's

Multi-column join keys; non-equality joins; a mixed-descriptor key
(`SameDescriptor` fails); a `kFilterScan` (its own literal already
bounds it); any sub-chain compiled through `CompileWhere` (v1 is
SELECT-only); scalar sub-chains (conclusiveness needs `Exists`
semantics, spec §6). Spill-to-disk and cross-statement reuse are out of
scope too but are not compile declines — the first is JB5's runtime
cap, the second is not a form anyone can write.

## JB1 — the compile half: the last ladder arm, as an annotation

**Built 2026-08-20.** As planned, with one seam the plan had not named:
the SELECT-only exclusion is not free — `CompileWhere` compiles its DML
sub-chains through the same recursive `CompileBlock` the SELECT path
uses, so eligibility is a `bool inner_build` threaded through
`CompileBlock`, false from `CompileWhere` down and false from a scalar
sub-chain down (off stays off for everything nested). The multi-column
decline is implemented as "a second correlated equality on the step
declines the arm outright", pk conjuncts excluded as the Cabin arm
excludes them.

`src/exec/step_compiler.cpp` (the structure ladder around
`CorrelatedIndexProbeOf` / `CorrelatedCabinProbeOf`),
`include/kds/exec/step_chain.hpp`.

When every earlier arm — the pk arms (lookup, probe, range), both index
arms, both Cabin arms, and the `kFilterScan` arm — has declined and
`CorrelatedEqualityOf` still names a correlated equality on the step —
same `key_from` shape the probes use, `SameDescriptor` required — the
step takes a `BuildKey` annotation: the own column, the outer source,
the residual position of the correlated conjunct. **An annotation, not a
step kind**: the step stays `kScan`, so trails, access statistics,
`ShippedForm` and every kind-switch downstream are untouched by
construction rather than by audit. The declines above are this arm's,
at compile time.

Still `f(shape, catalog)`: the arm consults no data, and the lazy form
is what removes the need to (spec §5). The invariant the whole design
rests on, stated where it is relied on: `PkBound` accepts literals
only, so no scan bound varies per outer row — a `kScan` inner is always
the full relation, which is what makes one walk's map serve every later
key.

*Done when:* exactly the walked-join shape carries the annotation; a
chain-identity test shows kinds, residuals, `read_columns` and class
byte-identical with and without the arm compiled in; every refusal row
above declines here with a test naming it.

## JB2 — the map

**Built 2026-08-20.** Two deviations from the letter, both toward the
plan's own spirit. The key reuses the *whole* Cabin value identity, not
just the 24-byte entry: `stats::MakeValueKey` (extracted from
`MakeCabinKey` in the review round) builds a `CabinKey` with `cabin_id`
0 — which `MakeCabinKey` refuses, so a build key handed to a CabinStore
by mistake fails closed as a miss instead of matching an authoritative
set; the encoding switch keeps its one home in `cabin_store.cpp`. And
the class is header-only — no `src/exec/inner_build.cpp` — because
`Find` is JB4's per-outer-row probe and inlines. The header states the
invalidation contract JB4/JB6 rely on (hold the bucket `vector*`, never
an iterator, across anything that can extend the map). The walk-order
pin includes the discriminating case: a map that sorted by pk the way
the Cabin's recording does would pass every other test and change
replies on an `EXPLICIT` relation.

`include/kds/exec/inner_build.hpp` (new; header-only).

`exec::InnerBuild`: statement-lifetime, owned by the executor frame,
destroyed with it. One entry per inner row — join-column value
(bucketed), pk, location hint — reusing `CabinEntry`'s 24-byte layout
(`include/kds/stats/cabin_store.hpp`, C6) as the unit rather than
redesigning one. **Buckets append in walk order** and a probe replays a
bucket front to back; that single property is spec §3's third fact, so
it gets its own pin rather than riding on an integration test.

*Done when:* a unit test pins per-key walk-order replay; both key modes
covered (`ASSIGNED` pk order, `EXPLICIT` page-slot order — build order
*is* walk order, whichever that is).

## JB3 — the lazy build

`src/exec/step_vm.cpp`, the walked-join site in the scan arm.

The first outer row's inner walk runs exactly as written, and as a side
effect buckets **every** row that passes the step's non-correlated
residual conjuncts — including rows failing the current outer key's
equality, which emit nothing but enter the map under their own value.
Emission for the first outer row is unchanged (full residual, correlated
conjunct included). This is `WalkAndRecord`'s pattern with one
difference stated up front: the Cabin records the probed key's set, the
build records the whole map, because one full pass is the point.

Two disciplines carried over from the Cabin recording fix, applied from
the start rather than re-learned: the build decodes only the join column
plus what the non-correlated residual reads (and the pk on demand),
never the full row; and build rows charge the row budget normally —
this *is* the walk the statement was going to run.

*Done when:* the first outer row's reply is byte-identical to the
un-built walk's; the map after the first row holds every inner row
passing the non-correlated residual, pinned against a hand-computed
relation.

## JB4 — the probe

Same site. With the map published, every later outer row probes its
bucket instead of walking: fetch each entry by pk through the location
hint, **re-evaluate the full residual on the fetched row**, emit in
bucket order. The re-check is the engine's superset-plus-recheck idiom —
correctness never rests on build bookkeeping, only cost does. MVCC needs
no new argument: the statement's snapshot is fixed, so a row visible at
build time is visible at probe time (spec §4); the re-check is what
makes even that argument non-load-bearing.

*Done when:* the driver's `join-no-literal` phase answers byte-identical
under `--verify`'s ordered row-for-row check with the build on and off;
ANALYZE's `examined=` drops from k·N-class to N-plus-matches-class.

## JB5 — the cap, and the off-switch

Config beside `sort_max_rows` in the dispatcher's knob block.

`join_build_max_rows`, ratified default 65536 rows (spec §7). Exceeded
mid-build: discard the partial map, mark the statement no-build, per-row
walks for the rest — the Cabin's fall-back refusal, never an error,
because the walk is always there. **`0` disables the build outright**
(no map can hold a row), which is the contract-test A/B lever and costs
no second knob. Scoping when a chain carries two walked inners is
`[OPEN]` in `CLAUDE.md` — until decided, one knob, checked per build,
which keeps both readings implementable.

*Done when:* a cap-exceeded statement answers byte-identical to the
walk and never errors; `join_build_max_rows=0` runs the pure walk; the
contract suite compares the two configurations byte-for-byte.

## JB6 — the stopping sub-chain: the prefix map

`src/exec/step_vm.cpp`, the sub-chain mode.

Spec §6's ratified form, after review priced the completion-walk design
out (break-even k ≈ 13 on §7c.4's data, ~2.4× regression at k = 4): the
map is a **walk-order prefix** under a high-water mark. A hit —
re-checked, per JB4 — is conclusive existence; a miss **resumes the
walk at the mark**, extending the map and advancing the mark; a walk
that reaches the end completes the map, and later misses become
conclusive absences. No walk-through-stops, no publication gate, no
budget carve-out: every charged row is a row the statement's own walk
visits, at most once per statement. Under JB5's cap a frozen map keeps
serving its prefix; misses walk from the frozen mark.

The class is the `Exists`-kind stopping walk — `EXISTS`, `NOT EXISTS`,
and `IN (subquery)`'s per-row form — not one member: the sub-chain mode
(`record_through_stops_`) is set for every sub-chain runner, so the
implementation keys on the walk's kind, not on which predicate spelled
it. Scalar sub-chains are JB1's decline.

*Done when:* the driver's `exists-correlated` phase answers
byte-identical under `--verify`; ANALYZE across one statement shows
each inner row examined at most once (`examined=` in the
N-plus-matches class, not k·partial-walks); a k = 4-shaped statement —
the shape the completion design measurably regressed — measures at or
below the pure walk.

## JB7 — observability, trails, shipping

- ANALYZE reports `inner_built=1 build_rows=N probes=k` (spec §4); a
  fallen-back statement reports `inner_built=0` with the walks it paid.
- `FormatPlan` prints `build` on the annotated step's line, the way
  `derived` marks a propagated conjunct — visible before execution, in
  the plan's own vocabulary.
- `IsTrailReplayable` does not move; the build feeds no trail and no
  sighting. Pinned.
- Cross-core: the annotation is compiled state the descriptor never
  carries — `ShippedForm` already downgrades structure-served steps and
  needs no new case, asserted rather than assumed. The peer building
  locally for its own consuming stage stays `[OPEN]` in `CLAUDE.md`.
- The prose the sanction supersedes, each named here so none is
  rediscovered by a reader: `manual/sql/sql.md`'s "there is no hash
  join and no merge join … decorrelation rewrites are forbidden"
  passage; `docs/parser-v2-workplan.md` V20's "no decorrelation rewrite
  exists" test description; `docs/feat-cabin.md` §4a's "the only
  acceleration a heap relation's join column can have at all" (and
  `CLAUDE.md`'s Cabin row echoing it, and `src/exec/step_compiler.cpp`'s
  "the one shape a heap relation can accelerate at all" / "last of the
  structure arms" comments, which JB1 displaces). **The two
  `step_compiler.cpp` comments were amended by JB1 itself (2026-08-20)**
  — JB1's commit made them false at compile time; the doc and manual
  passages stay JB7's, since they become false only when the executor
  consumes the annotation. JB1 also already clears the annotation in
  `ShippedForm` — JB7's cross-core bullet asserts, it need not add.

*Done when:* the shipping-equivalence harness
(`EveryShippableShapeAnswersExactlyWhatLocalExecutionAnswers`) stays
byte-identical with the walked-join shape added; a trail-model test
shows recording and replay unchanged around a built statement; every
passage in the list above is amended in the commit that makes it false.

## JB8 — measurement

ck-tester, `build-release`, interleaved A/B, fresh data file and server
per cell — the standing discipline. Cells:

1. `join-no-literal` and `exists-correlated` under `--index-mode none`,
   no `--cabin`: acceptance is spec §9's — the join cell from ~8.5 ms
   (§7e.5) and the EXISTS cell from ~979 µs (§7c.3's pooled walk;
   §7e.4's 1.4 ms is PostgreSQL's row, not this engine's baseline) to
   the ~600 µs class, **no other cell outside its floor** (the build
   must be invisible to every shape that never enters its arm). Neither
   phase has a published ckdbs number yet (§9b.7): JB8 establishes the
   baselines, then beats them.
2. The k=1 cell: one outer row pays one walk plus the build constant;
   the bound is spec §5's expectation and JB3's decode discipline is
   what keeps it small. A regression here is a finding with its number.
3. The standing PostgreSQL comparison (§7e's three-way table) re-run so
   the "third answer ckdbs does not have" row can be retired in place.

*Done when:* `bench/results-scenario3-library.md` carries a dated
addendum with the commit, both binary sha256s, full percentile tables,
and the §7e follow-up; the workplan's rows above flip to done with their
commits.

## Build order

JB1 → JB2 → JB3 → JB4 → JB5 (the off-switch exists before any
measurement) → JB6 → JB7 → JB8. JB6 may land after JB8's join cell if
that cell is wanted early, but no push ships a stopping sub-chain that
treats a prefix miss as an absence — a conclusive miss without a
completed map is the one state this plan forbids.

## Open

Carried, not decided: the three items live in `CLAUDE.md`'s Open
Decisions index under **Join inner build**, defined in spec §7/§8.
