# Workplan — the statement-local inner build

Tasks `JB1`–`JB8`, the artifact `docs/spec-join-inner-build.md` §10 gates
behind ratification (discharged 2026-08-19). The spec owns every design
argument; this file owns the build order, the seams, and the gates.
**JB1–JB7 are built (2026-08-20); JB8 is the closing measurement.** JB6
and JB7 were built in separate worktrees on the same day and are
independent — JB7 reads counters JB3/JB4 already collect and adds no
executor state, and JB6 adds state no counter's meaning depends on.

**The ledger, which JB8's done-when asks each row to carry.** Every
task named with the commit that landed it, so a claim below can be
checked against the tree it was true of rather than against a date:

| task | commit | what landed |
|---|---|---|
| JB1 | `fc44ac6` | the `BuildKey` annotation, last ladder arm |
| — | `772a524` | the annotation moved to `Step`'s tail (a measured cache-line regression, found by JB1's own A/B) |
| JB2 | `9f67833` | `exec::InnerBuild`, the map |
| JB3 | `d454cf4` | the lazy build |
| JB4 | `b625855` | the probe |
| JB5 | `1b28c9f` | `join_build_max_rows`, the cap and off-switch |
| — | `74c1a3a` | the constant, second cut: 83.7 → 43.2 ns/row |
| JB6 | `041410d` | the stopping sub-chain's prefix map |
| — | `c8126cf` | the constant, third cut: 43.2 → 37.2 ns/row |
| JB7 | `e186e7d` | ANALYZE's `build` marker and the three counters |

Two of those ten rows are not tasks at all but answers to findings the
series' own measurements produced — which is the shape this workplan
kept taking, and worth seeing in one place.

JB8's measurement at `2755045` is `bench/results-scenario3-library.md`
§7f; it predates both constant cuts and JB6, so its EXISTS row records
a class that is now served. The refresh at HEAD is §7g. The sanction is
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
an iterator, across anything that can extend the map — **superseded
2026-08-20 by "The build constant" below**: the storage is an arena with
per-key chains, `Find` returns a `Bucket` by value, and an index-held
Bucket survives every `Add`, its own key's included). The walk-order
pin includes the discriminating case: a map that sorted by pk the way
the Cabin's recording does would pass every other test and change
replies on an `EXPLICIT` relation.

`include/kds/exec/inner_build.hpp` (new; header-only). The landing A/B
measured +2.2–2.7% on the two walked correlated cells and the finding
was investigated rather than shipped or reverted blind: the server
binary's only delta is `cabin_store.o`, the refactored path measures
flat, and with placement equalized both commits sit at the floor — the
cost was link-layout displacement of a loop this change never touches
(JB8's measurement note owns the consequence).

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

**Built 2026-08-20.** The dispatch's scan arm routes an annotated step
through `WalkAndBuild` — `WalkAndRecord`'s shape: arm, walk, publish only
a completed walk (a sink-stopped first walk resets the map; the reset
makes "a cut build is never served" state, not control-flow luck).
Bucketing splits the residual around `BuildKey::residual_pos` with the
loop body extracted as `EvaluateConjunct`, so the split and the whole
evaluation cannot drift; emission still passes the full list. The
sub-chain gate is `!record_through_stops_` at the dispatch — the plan's
forbidden state has one enforcement point. `StepStats` carries
`inner_builds`/`build_rows` from here (JB7 prints them), and the
done-condition's map pin is discharged at count level plus the reply
pins; the content-level pin is JB4's probe made observable, and lands
with it. **Note: JB3 alone is a deliberate interim state** — the first
walk pays the bucketing and nothing probes yet, so no measurement runs
until JB4/JB5 land (this workplan's own rule: the off-switch exists
before any measurement).

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
plus what the non-correlated residual reads (and the pk on demand — read
straight out of the Keystone word since "The build constant" below,
which is cheaper than the on-demand decode it replaced and is the same
saving on the Cabin's recording walk), never the full row; and build
rows charge the row budget normally —
this *is* the walk the statement was going to run.

*Done when:* the first outer row's reply is byte-identical to the
un-built walk's; the map after the first row holds every inner row
passing the non-correlated residual, pinned against a hand-computed
relation.

## JB4 — the probe

**Built 2026-08-20.** `ProbeBuild` replaces the built step's walk, with
three deliberate differences from `ServeFromCabin`, each stated as a
correctness line at the site: no sort (bucket order is the emission
contract), no dedup and no hint verification (one entry per walked row
by construction; a same-statement location cannot move, and every entry
still goes through `AcceptTupleAt`'s MVCC and full-residual re-check),
and a missing bucket conclusive (the map published only off a completed
walk). `build_probes` joins the counters. The driver-level `--verify`
gate runs at JB8; the in-tree byte-identity pin is the exec tests' hand-
computed walk replies, which the probe now answers, plus the examined
drop (7 versus the walk's 15 on the fixture join).

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

**Built 2026-08-20.** The knob rides `Budget` rather than a 13th
`Execute` parameter (the JB4 review's seam call): it is the statement's
resource envelope, already threaded to every runner and sub-chain, and
the entry points' fresh-counter copy now carries it across explicitly.
The cap signals through `BuildRecording::over_cap` — never by clearing
`building_`, which also drives the split evaluation — and the refused
step goes `kDeclined`, a per-statement verdict (re-attempting per outer
row would bucket to the cap and discard each time, strictly worse than
the walk the fall-back protects). `0` gates at the arm: nothing arms,
nothing buckets. The contract suite
(`tests/inner_build_contract_test.cpp`) sweeps caps 0/1/2/5/default
byte-for-byte over the join, filtered, nested and EXISTS shapes, and
pins the mid-build decline, the pure-walk off-switch, and the exact-fit
boundary (`rows() >= max` trips only past the cap, so a map equal to it
still publishes).

Two facts the JB5 review recorded for later tasks. **The peer-dispatcher
knob gap**: `set_join_build_max_rows` reaches core 0's dispatcher only,
like every knob before it (`sort_max_rows`, the aggregate limits, the
optimizer thetas) — latent because peers are driven by nothing in
production and `ShippedForm` strips the annotation anyway, but JB5's off
position is a *measurement lever*, and a peer that ignored
`join_build_max_rows = 0` would silently spoil an A/B the day peers
execute statements. **The sub-runner shares the `Budget` by reference**
(`EvaluateSubChain`), so JB6's prefix map inherits the cap with no new
plumbing.

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

**Built 2026-08-20** on `jb6-prefix-map` from `c379d7f`, with two seams
the plan had not named and one done-condition it does not meet — both
below, the second in its own subsection because it is a finding, not a
footnote.

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

### The two seams the plan had not named

**The map had to leave the runner.** `EvaluateSubChain` builds a fresh
`ChainRunner` per outer row, so a map living in the runner — where
JB3/JB5 put it, correctly, for a top-level chain — would be filled and
destroyed once per outer row, which is the entire cost the map exists
to remove. The store is now owned by the top-level runner and shared
down by pointer, the way `stats_` and `budget_` already are, and step
ids being global across a statement (step_chain.hpp) is what makes one
map per id need no further scoping.

**The mark is a position, and a position needs a unit.** A resumable
walk needs to start mid-relation, so `RunWalkStep` takes an optional
`WalkPrefix`: a page plus the count of that page's rows already
covered, **in the page's own emission order**. Rows, not slots, because
`emit_in_key_order` sorts a page by Keystone id before emitting it —
counting slots would resume in the wrong place on exactly the relations
(`EXPLICIT` key mode) whose order the sort exists to fix. Every
accepted tuple of every walk now goes through one `accept` lambda that
owns the ordinal, so the count a resume skips cannot drift from the
count a mark recorded. The mark's soundness rests on the same
no-writes-between-outer-rows argument JB4's location hints rest on,
and it is stated at both sites.

**The cap freezes the mark where the map stopped taking rows**, not
where the walk stopped: the row that trips the cap is visited and not
bucketed, so a mark past it would claim coverage the map does not
have, and a later miss would resume past rows it never replayed — a
dropped row, not a slow one. The trip is detected in the walk (the
build's `over_cap` transition across one accepted row) and freezes the
mark there for the statement.

### Why the n = 2 deferral is not available here

Worth recording because the join form's constant made it the obvious
question (spec §5, declined there on arithmetic): for the prefix form
it is not a trade-off but an unsound state. The map and the mark must
advance together — a walk that advanced the mark without bucketing
would leave rows before the mark unreachable by any probe and skipped
by every later resume, which is a false absence rather than a slow
answer. Deferring the map means deferring the mark, and deferring the
mark means the second outer row walks from the head: the prefix, and
the whole design, only exists once the first walk pays.

### The measurement, and the done-condition it misses

`build-release`, config-levered A/B on one binary
(`join_build_max_rows` 0 vs 65536, which is immune to the placement
band), fresh server and data file per cell, 10,000 loans.

**The acceptance cell passes.** The driver's own `exists-correlated`
phase (k = 20, `--index-mode none`, no `--cabin`, 30 ops):
**1,569.9 µs → 612.9 µs, ×2.56**, which lands in spec §9's "~600 µs
class" — the target the JB5 gate's join cell missed by its build
constant. `--verify 50` reports `verify_problems: []` on both sides,
and every other phase of the sweep sits inside its floor (pk-user 39.9
→ 40.4, loans-by-user 622.2 → 622.2, overdue 838.7 → 840.2).

**The examined class collapses**, which is JB6's own done-condition and
the thing the design is for: at k = 16 the same statement examines
**27,888 rows walked → 6,736**, one visit per inner row across the
statement rather than one partial walk per outer row.

**A k = 4-shaped statement does not measure at or below the pure
walk.** It is +13% at 100 rows per key and +25% at 5, and the crossover
sits at **k ≈ 6–8**, not below 4:

| rows/key | k=1 | k=2 | k=4 | k=8 | k=16 |
|---|---|---|---|---|---|
| 100 (dense) | +3% | +4% | **+13%** | −8% | −15% |
| 5 (sparse) | **+76%** | +33% | +25% | — | −52% |

The cause is arithmetic, and it is the JB5 gate's finding in a second
shape: the prefix trades **the sum of the per-outer-row walks for the
longest single walk, plus the build constant charged on every row of
that longest walk**. At k = 16 the sum is many times the max and the
trade is overwhelming; at k = 4 the sum is barely above the max, and
the constant is more than the difference. The sparse row is worse for a
reason worth naming: with 2,000 keys over 10,000 rows the first outer
row's walk already covers two thirds of the relation before its own
match, so the build pays for the whole prefix while the walks it saves
are short.

Spec §6's "at or below the plain walk's cost at every k, with no earn
gate" is therefore **false as measured**, in the same way §5's "at
k ≥ 2 every avoided walk is pure win" was: both counted rows visited
and neither counted the nanoseconds of visiting them. §6 is amended
with the crossover rather than the claim.

## The build constant, third cut (2026-08-20, `jb6-prefix-map` at `c8126cf`)

The map's last allocation, removed: keys now live in an append-only
vector found through an **open-addressed table of 8-byte slots**, so a
distinct key costs no allocation at all — not the bucket vector a
`vector`-per-key cost, not the node `std::unordered_map` cost — and the
per-lookup integer division libstdc++ does (`hash % prime`) becomes a
power-of-two mask. The key stays the Cabin's whole value identity,
compared in full behind an 8-byte tag; a hash-only key would be faster
still and would rest its correctness on the probe's re-check, which a
container has no business doing (JB2's decision).

**The constant: 43.2 → 37.2 ns/row**, read off the same join-at-k=1
cell as before (`--shape join --loans 10000`, same-binary A/B: 651.9 µs
off, 1,023.7 on, 371.8 µs over 10,000 bucketed rows). Break-even on the
walked join is k ≈ 1.7.

**One thing measured and rejected, recorded because it looks obvious:**
finalizing the hash with a murmur3 mix. `CabinKeyHash` maps a small
integer key to a near-consecutive value, so consecutive keys take
consecutive slots and the fill writes the table sequentially with
almost no collisions; mixing scatters them and measured *worse on every
cell* — 15.1 → 16.3 ns/row at 10,000 rows over 2,000 keys, 22.9 → 43.8
on all-distinct keys. The locality is worth more than the tag's
filtering. The note lives at `TagOf` so the next reader does not
re-derive it.

(A caution for anyone re-running the map micro-benchmark: its
all-distinct cells are dominated by an allocator artefact, not by the
container. Each repetition frees and re-allocates the whole table and
key arena, and glibc's dynamic mmap threshold makes that cost jump
around by hundreds of microseconds per pass — which is why the 10,000
and 65,536 all-distinct cells disagree in *sign*. The server pays it
once per process, not once per statement, and the end-to-end cells
above are what the constant is quoted from.)

### What the cut moved

| shape | cell | before | after |
|---|---|---|---|
| join | build constant | 43.2 ns/row | **37.2** |
| exists, 100/key | k=1 | +3% | **−4%** |
| exists, 100/key | k=4 | +13% | **+8%** |
| exists, 100/key | k=16 | −15% | **−18%** |
| exists, 5/key | k=1 | +76% | **+58%** |
| exists, 5/key | k=2 | +33% | **+18%** |
| exists, 5/key | k=4 | +25% | **+11%** |
| exists, 5/key | k=16 | −52% | **−59%** |

The acceptance cell with it: the driver's `exists-correlated` phase
**1,681.3 → 547.2 µs, ×3.07** (from ×2.43), `verify_problems: []` on
both sides. The crossover moves from k ≈ 6–8 to **k ≈ 5**.

**The k = 4 done-condition still fails**, at +8% (dense) and +11%
(sparse) rather than +13% and +25%. What is left of it is the constant
itself, and the remaining candidates each cost something the map has so
far refused to spend: a hash-only key (correctness resting on the
re-check), or an earn gate (spec §6 ratified against one, and it would
give back the k ≥ 8 wins in proportion).

## JB7 — observability, trails, shipping

**Built 2026-08-20** on `jb7-observability`. Two renderings differ from
the sketch in the bullets below, each for a stated reason.
**`probes=k` is printed `build_probes=`**: the counter is the outer rows
*served from* the map, which is k−1 for a k-row outer side because the
first row's walk was the build — labelling it §4's `probes=k` would make
the number read as one larger than it is, and a bare `probes=` in that
flat token namespace would be read against the `Probe` *kind* the plan
line above it prints. `docs/spec-join-inner-build.md` §4 still carries
the `probes=k` sketch and wants the same one-line amendment. And **`inner_built=` keys off the compiled
annotation, not off a non-zero counter**, so a fallen-back step prints
`inner_built=0` rather than being omitted by the zero-suppression every
other counter uses; no other number distinguishes "annotated and did not
publish" from "never eligible", which is exactly the reading an operator
chasing a slow join needs.

The two harness done-conditions were discharged by *strengthening*
existing suites rather than adding parallel ones. The walked-join shape
was already in the shipping-equivalence harness — JB7 adds the assertion
that the local side really builds, so those rows compare build against
shipped walk rather than walk against walk (the same non-vacuity
argument its own `stages_opened` guard makes). And the trail-model pin
is one statement added to `waystone_contract_test.cpp`'s shared query
set, which runs it through all five configurations at once — including
the corrupted and the deleted trail — so no trail state moves a built
join's reply.

**What the query-set comparison does not reach, and what does.** The
converse — *a build feeds no trail* — is invisible to those replies:
recording is gated on `IsTrailReplayable(step.kind)` and consultation
happens only in `RunPointStep`, so an entry wrongly recorded for the
annotated kScan step would be written and never read, and every reply in
that suite would stay byte-identical. The pin that bites inspects the
trail rather than the reply, and **is built** as
`WaystoneContractTest.ABuiltJoinFeedsNoTrail`: a statement that recorded
nothing has no pattern row, so the walked join's `pattern_id` must be
absent from `ListPatterns()` after four recorded runs — with a keyed
statement in the same database asserted *present*, so the absence cannot
be a recorder that never ran. That test is what fails if someone drops
the `IsTrailReplayable` gate at the recording site.

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

**Done 2026-08-21**, addendum §7g at `aa3e26c` (binary sha256
`39ac4e42…9175d`), the ledger above carrying every task's commit. Four
results and one correction:

- **EXISTS reaches the class**: 1,598.8 → 534.3 µs (**×2.99**),
  independently confirming JB6's own ×3.07 on a fresh seed. §7f had
  recorded this cell unchanged with the class gated out; that record is
  superseded.
- **The join cell runs ×9.60 and still misses the class — and the
  class was wrong.** One pass of `loans` costs 668 µs here, so spec §9's
  ~600 µs target sits *below* the pass the design cannot remove; a free
  build still lands at ~682. Spec §9 is amended. The honest reading is
  the ratio and the share: the build is now **33% of the statement**
  against 57% at `2755045`.
- **The constant is 33.6 ns/row** (`c8126cf`'s 37.2 confirmed in class,
  10% below it — re-measurement, not a fourth cut), and **break-even is
  under k = 2 at every size** (1.24 / 1.50 / 1.56), where §7f had k = 2
  losing at all three.
- **JB6's k = 4 condition still fails** (+9.1% dense, +6.2% sparse;
  crossover k ≈ 5.5 / 4.5), and the counters yielded the model that
  explains every cell: `Δ = rows_saved × 62.2 ns − rows_bucketed ×
  33.6 ns`. **A bucketed row costs about half a walked row**, so the
  prefix wins exactly when it saves more than half the rows it buckets —
  and the same ratio derives the join's break-even at k > 1.5. k = 4
  fails on *how few rows a stopping walk saves*, not on the constant.
- **The four-way, refreshed**: 103 / 989 / 1,271 / 14,035 stmts/s, the
  gap to PostgreSQL closed 1.92× → **1.28×**. And on `exists-correlated`
  ckdbs leads PostgreSQL **1,872 to 712 (×2.63)** — the first cell in
  that file where this engine beats PostgreSQL with nothing banked,
  because PostgreSQL decorrelates into a `HashAggregate` over the whole
  relation where JB6's prefix stops at row 6,689 of 10,000.

`--verify` passed all 16 driver cells, 163,864 operations, 0 errors.
Two things are explicitly **not** settled and say so in §7g: the JB5
gate's lever-off plumbing question (3–6 ns/row, needs a
placement-equalized cross-commit A/B this run's offset would swallow),
and §7f.8's `composite` / `covering` / `indexes = off` columns, not
re-run.

**Measurement note (2026-08-20, the JB1/JB2 landing rounds):** the walked
correlated-inner loop is placement-sensitive at ±2–3% wall on the
measurement box — three consecutive rounds moved it without touching its
code (fc44ac6's mid-`Step` field via data layout, reverted at 772a524;
9f67833's `cabin_store` TU via link layout, investigated and disproven as
a code cost: with placement equalized, both commits measure at the
floor). Two consequences JB8 inherits: an effect claim on
`join-no-literal` / `exists-correlated` needs replicated fresh-instance
pairs adjudicated against same-binary floor draws, and a cross-commit
verdict that matters should be confirmed with placement equalized — both
sides built `-falign-functions=32` in scratch dirs, measured 2026-08-20
to collapse the band while sitting ~1.5–2.3% above the best placement
draw. The flag is therefore a measurement instrument, not a shipped
setting; adopting it in Release is a toolchain proposal awaiting
ratification (CLAUDE.md Open Decisions, Project).

## The JB5 gate's measurement (2026-08-20, at 1b28c9f exactly)

Config-levered A/B on one binary (`join_build_max_rows` 0 vs 65536),
which the placement band cannot confound; every pair's `--verify` passed
row-for-row (JB4's done-when, 26/26 pairs). Verdicts:

- **The acceptance cell: pass with a named miss.** join-no-literal at
  10000/k=16 moves 113.8 → 675.3 stmts/s (×6.0; §7e's walk baseline 117
  reproduced by the off side) — from 11.2× behind PostgreSQL to 1.9×
  behind, 22× short of the converged Cabin, the ladder-order economics
  §5 predicted. The ~600 µs class is missed: on-side p50 is 1466 µs, and
  the miss is exactly the build constant below (walk 603 + build ~840 +
  16 probes ~26).
- **The finding, with its number: the build costs ~80–84 ns per
  bucketed inner row** — join-k1 regresses +26%/+80%/+139% at
  200/1000/10000, break-even k ≈ 2.6 (10k) to 5.3 (200). Spec §5's
  "k ≥ 2 every avoided walk is pure win" is quantitatively wrong on this
  box (k=2 loses at 10k), and nothing today can decline a build by k —
  the cap gates rows. This is the open cap-scoping decision's first real
  datum; the Cabin's ratified n=2 observation rule
  (`kAutoRecordThreshold`: the first miss counts, the second records) is
  the spec-consistent candidate answer — defer the build to the second
  outer row's walk, making k=1 free and moving break-even to ~3.6.
  Decision not taken here; it amends spec §5. **Taken 2026-08-20 in
  "The build constant" below, and against the candidate**: the constant
  was the defect, not the arming rule, and half of it was removable.
- **exists-correlated: unchanged within floors** (the JB6 gate holds).
  **Floor sweep: pass** (pk, filter-scan, cabin; the Cabin keeps ladder
  priority — plans identical under both configs).
- **Unresolved-leaning, recheck at JB6:** with the build *off*, aligned
  cross-commit draws put exists +1.7% (4/4 rank-perfect but only 4
  draws) — a possible ~3–6 ns/row executor-plumbing cost riding even
  when disabled.

## The build constant (2026-08-20, on `jb-k1-deferral`: the baseline at `2755045`, the result at `74c1a3a`)

The JB5 gate ended on a finding and a candidate answer: 83.7 ns per
bucketed inner row, k=1 at +137%, and the Cabin's n=2 rule as the
spec-consistent way to make k=1 free. The candidate was not taken.
**Re-measuring the premise first (CLAUDE.md's rule) showed the constant
was not irreducible, and that no arming rule can beat removing it.**

### Attribution — where 83.7 ns/row went

Config-levered A/B on one binary (`join_build_max_rows` 0 vs 65536),
10,000 loans over 2,000 users, k ∈ {1,2,4,16}, fresh server and data
file per cell (`bench/run_cell.sh`), 40 ops per k. The harness is
**`tools/join_ksweep.py`, added here** — the driver's relations, seeding
and statement with k lifted out of the module, which is the sweep
`scenario3_library.py`'s own comment says is a harness's job, and which
JB8 needs too. The build constant is read off k=1, where the map is paid
in full and probed once:

| binary | k=1 p50 | constant | break-even |
|---|---|---|---|
| `2755045` (JB5 as landed) | 1,445 µs | 83.7 ns/row | k ≈ 2.6 |
| + arena-chained map | 1,155 µs | 54.6 ns/row | k ≈ 2.0 |
| + Keystone-word pk | 1,062 µs | 45.3 ns/row | k ≈ 1.9 |

A hot-loop micro-benchmark of the map alone (scratch, not in tree) put
`MakeValueKey` at 4.6 ns and the vector-per-key insert at ~27 ns on the
10,000-rows-over-2,000-keys shape; in situ the map cost about twice
that, which is the cache pressure of a 10,000-entry map interleaved
with a 163-page walk. Nothing else in the bucketing site is material:
the split evaluation (`EvaluateAllExcept` plus the correlated conjunct)
re-runs exactly the conjuncts the plain walk ran, and on this shape the
non-correlated half is empty.

### The two changes

**The map is an arena with per-key chains** (`inner_build.hpp`,
JB2's class, rewritten). Entries append to one vector, a parallel
`next_` vector links each key's rows head-to-tail, and the hash map
holds a 8-byte `{head, tail}` per key. A distinct key cost two
allocations (map node plus the bucket vector) and a growth realloc per
bucket besides; it now costs one. Walk order is preserved by appending
at the *tail* — a head-insert list is one instruction cheaper and would
reverse every reply, so the tail append is a correctness line, not a
style one.

`Find` returns a `Bucket` value rather than a `std::vector*`, which
makes two things structural. An empty Bucket is the only "no rows"
answer, so there is no null to forget. And a Bucket holds an *index*,
so it survives any `Add` — including one under its own key, whose
appended row a walk that has not passed the tail then sees. That is
strictly stronger than the pointer contract it replaces (a `push_back`
could reallocate the buffer a probe was iterating), and it is what
JB6's resumed walk wants: extending the map under a live probe is the
prefix rule's normal case. `inner_build_test.cpp` pins it.

`Add` returns `bool`. The only refusal is the index type's own limit
(2^32-1 entries), reachable only through a `join_build_max_rows` above
it — the config accepts any unsigned value — and the caller takes the
cap's verdict for it. A silently dropped row would be the one state
the publish rule forbids: a map claiming to be the whole relation
while missing rows from it.

**The pk comes out of the Keystone word** (`EntryForRow`,
`step_vm.cpp`). Building an entry decoded column 0 through
`DecodeColumnsInto` — a schema walk and an AstValue per bucketed row —
where `RowKeystoneId` reads the id at its fixed offset. Measured at
11.6 ns/row on this shape. It also drops a `int_val < 0 ? 0` guard that
could only ever have masked a corrupt decode (a Keystone id is 40 bits
and unsigned), and it removes the decode's side effect of filling frame
slot 0 — which nothing read: the trail's pk comes from `read_columns`,
where `ReadColumnsOf` puts column 0 for every trail-replayable step.
**The Cabin's recording walk shares this function** and pays the same
11.6 ns per recorded row; that path was not separately benchmarked, and
the change can only remove work from it.

### The result, and the deferral declined

The final binary at all three row-set sizes the JB5 gate reported, one
fresh cell per config position, p50 of 40 ops (the 10,000 row is also
the median of the two earlier interleaved rounds, which agreed within
2%):

| rows | k | walk (off) | build (on) | verdict | JB5 |
|---|---|---|---|---|---|
| 200 | 1 | 55.2 µs | 66.5 µs | **+20%** | +26% |
| 200 | 2 | 68.9 | 68.6 | −0.4% | |
| 200 | 16 | 237.0 | 84.3 | **2.8×** | |
| 1,000 | 1 | 103.7 | 145.8 | **+41%** | +80% |
| 1,000 | 2 | 152.8 | 143.9 | −6% | |
| 1,000 | 16 | 957.1 | 161.4 | **5.9×** | |
| 10,000 | 1 | 618.5 | 1,050.7 | **+70%** | +139% |
| 10,000 | 2 | 1,175.0 | 1,048.5 | −11% | +26% |
| 10,000 | 4 | 2,288.1 | 1,050.2 | −54% | |
| 10,000 | 16 | 9,098.2 | 1,064.7 | **8.5×** | ×6.0 |

Constant 43.2 ns/row at 10,000 and 42.1 at 1,000 (56.5 at 200, where 40
keys over 200 rows and a ~53 µs per-statement floor make the per-row
figure the least meaningful of the three). **Break-even is under k = 2
at every size**, where JB5's range was 2.6 to 5.3, so the shape that
used to need three outer rows to pay for itself now needs two. The
acceptance cell's miss against spec §9's ~600 µs class shrinks from
866 µs to 432 (walk 565 + build 432 + 16 probes ~16 = 1,048 measured).

**The n=2 deferral is declined**, and spec §5 now carries the
arithmetic on the measured parts (walk 565 µs per outer row, build
432 µs, probe ~1 µs at 10,000 rows): deferring to the second outer row
would cost k = 2 **+36%** where the eager build wins 11%, and leave
k = 3 at −8% where the eager build wins 40%. It moves the loss from
k = 1 to k = 2 and gives back most of the win above it, on a shape
whose reason to exist is k ≫ 1. Nothing was built to measure it — the
decline is arithmetic on measured parts, and reopening it means
building one and measuring that.

### The floor sweep, and the order effect it exposed

The full scenario3 phase set under both config positions, twice, with
the orders reversed. **Read the second pass, not the first**: off-then-on
put five unrelated phases 5–18% up on the on side (`overdue` +18%,
`books-by-author` +16%, `books-by-genre` +14%), and on-then-off put the
same phases within ±4% with the sign flipped. The effect follows the
*position*, not the config, which no single-order sweep could have told
apart — the placement band this workplan already documents, in its
whole-cell form. Within-pair, `join-no-literal` is 8,972 → 1,101 µs in
one order and 8,978 → 1,102 in the other, and `exists-correlated` does
not move (1,480 both ways, the JB6 gate holding).

Suite: 2,514/2,514 green in `build-release` at this state, the
`inner_build_contract_test.cpp` cap sweep included — the byte-for-byte
comparison across `join_build_max_rows` 0/1/2/5/default is what says
the map rewrite did not change a reply. The Debug suite was **not**
executed in this session.

## Build order

JB1 → JB2 → JB3 → JB4 → JB5 (the off-switch exists before any
measurement) → JB6 → JB7 → JB8. JB6 may land after JB8's join cell if
that cell is wanted early, but no push ships a stopping sub-chain that
treats a prefix miss as an absence — a conclusive miss without a
completed map is the one state this plan forbids.

## Open

Carried, not decided: the three items live in `CLAUDE.md`'s Open
Decisions index under **Join inner build**, defined in spec §7/§8.
