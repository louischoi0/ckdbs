# Work order IB — the index lands on a heap relation: IX3 lifts on batch resolution, priced before it is trusted

Drafted 2026-09-01 by CLA against `main` at `fc9242f`
(`v2.7.0-64-gfc9242f`). Narrow successor to the IH survey (conversation
record, 2026-09-01): of the three gates an index under range ownership
faces, this order takes **IX3 alone** — `CREATE INDEX` on a
heap-clustered relation — and leaves the split gate
(`src/catalog/catalog.cpp:3046`) and `index.md` §13's placement question
untouched. Source of record: `docs/spec/index.md` IX3 (`:112`) and §13
(`:804-853`), `src/catalog/catalog.cpp:3013-3083`
(`Catalog::CheckIndexDef`), `src/exec/step_vm.cpp:606-615` (the heap
resolution path), `:1341` and `:1503-1509` (the Cabin's bulk-heal
precedent), `docs/spec/heap-and-tuple.md` §3.1/§4 (`:23-28`, `:44`),
`src/exec/index_maintain.cpp:56` (`AppendIndexEntry`).

## Rulings — IB-R1..IB-R6

All six are **CLA proposals, recorded here for ratification at IB0**; no
code in this order compiles before the operator's acceptance lands as
spec text. No constant is decided anywhere in this order.

**IB-R1 — The resolution mechanism is batch collect-and-walk.** A probe
of a heap-backed index descends the index structure exactly as on a
btree relation, **collects every matching entry's pk, sorts the set, and
resolves it with one walk bounded to `[min(pk), max(pk)]`** under the
chain's `min_key` pruning (`heap-and-tuple.md:27`, `:44`). N per-entry
resolutions become one bounded scan — which is the structural refutation
of IX3's cost sentence ("one full scan into N partial ones"). The entry
format does not change; no page hint is added; invariant 9's trust table
is untouched (the entry-to-pk half stays lookup-class, the pk-to-row
half stays a search, now bounded).

**IB-R2 — Trails first, the walk for the residue.** For each collected
pk the resolver consults the Waystone trail (`TryReplay`,
`step_vm.cpp:612`) — a replay is one read and never costs more than the
pk's share of the walk — and the **residue** is what the single bounded
walk resolves. The walk records trails for the pks it resolved, which is
`FallBackAndReRecord`'s bulk-heal shape (`step_vm.cpp:1503-1509`)
applied to the index. No threshold decides when to consult and when to
walk: trails are always consulted, the walk always takes the rest.

**IB-R3 — A heap-backed probe grants no key order.** The walk emits in
pk (chain) order, not index-key order (`step_vm.cpp:1341` states the
inversion for the btree case; here it is the emission order itself). The
planner must not claim sortedness from a heap-backed `kIndexProbe` /
`kIndexRange`; an `ORDER BY` on the key column keeps its sort step. A
sortedness property silently granted and wrong is a wrong answer with a
right answer's shape — the same sentence that guards the entry cap
(`catalog.cpp:3017-3020`), applied to order.

**IB-R4 — The superset rule crosses unchanged.** A collected pk whose
row the walk finds retired or invisible under the statement's view is
**skipped and counted** (`rows_examined` advances, matched does not),
never an error — IX1's superset rule is exactly this statement and the
btree arm already lives by it. A probe and the filter scan it replaces
must answer identically on every cell of IB3's matrix; that identity is
the correctness gate, not a hypothesis.

**IB-R5 — The fences, by name.** This order lifts IX3 and nothing
adjacent: **F1's heap-parent FK refusal stays** — `index.md:116-118`
calls the two arguments identical, so the IX3 amendment must state in
its own text that F1 is a separate decision, or the identity reads as an
automatic lift; **the split gate stays** (`catalog.cpp:3046`);
**`UNIQUE` stays refused** (IX11, `catalog.cpp:3034-3036`); **the
index-only scan stays double-gated** (visibility witness and the spilled
covered pointer, `index.md` §13); **`RangeEligible` is untouched** —
the kIndex arm stays dead code and D1 stays, so nothing this order
builds meets a boundary.

**IB-R6 — Planner admission is the btree rule, safety argued
structurally.** A heap-backed index becomes probe-eligible under
exactly the rule a btree relation's index uses today: the predicate
matches the key columns. No cost threshold decides admission, because a
threshold is a constant. The safety argument is structural — at worst
the collected set spans the whole chain and the bounded walk **degrades
to the filter scan it replaced**, plus one descent and one sort — and
IB3's largest-N cell exists to verify that the degradation is graceful
in measurement, not only in argument.

## Background

Three facts, source-read, restated once:

1. **IX3 refuses at the door** (`catalog.cpp:3078-3083`): a heap
   relation has no pk index, so an entry's pk resolves by chain scan,
   and `CheckIndexDef` — "the door every non-DDL caller comes through" —
   refuses the definition before anything is built. The same file's
   split gate at `:3046` fires first on a split relation; this order
   does not touch it, and after IB a split relation still takes no
   index.
2. **The heap resolution path is trail-then-walk**
   (`step_vm.cpp:606-615`): "a trail turns a full chain scan into one
   read, because a heap relation has no pk index for a descent to use in
   the first place." The walk prunes by immutable `min_key`
   (`heap-and-tuple.md:23-28`), and pages are key-ordered chain-wide
   (`:44`), which is what makes a bounded batch walk well-defined.
3. **The bulk-heal precedent exists** (`step_vm.cpp:1503-1509`): the
   Cabin, facing the identical per-entry cost on heap, abandons
   per-entry resolution and takes "one bulk heal rather than a chain
   scan per entry." IB-R1 is that pattern given to the index, with
   IB-R2's trail consultation in front of it.

## Conclusions (standing)

1. **Spec precedes code**: IB0 lands the amended IX3 text, the order
   property, and the F1 fence before IB2 compiles anything.
2. **Guideline 2 holds trivially and is still asserted**: everything in
   this order is core-local. A relation without an index pays nothing —
   the probe path is entered only from a plan that chose it — and the
   `cores = 1` byte-identity suite runs on every commit of this order
   anyway, because "trivially unaffected" is a claim and claims get
   tests.
3. **Maintenance is already local and stays so**: `AppendIndexEntry`
   (`index_maintain.cpp:56`) writes owner-stamped pages (PW1c-6b-4's
   settlement), and a heap relation's index build routes through the
   same function the write hook loops over (`index_ddl.cpp:101`).
   Nothing in this order touches ownership, streams, or the wire.
4. **No comparison against any pre-IB heap-scan result is stamped
   retroactively**; every number this order cites is produced by IB1's
   harness on IB-era binaries.

## Hypotheses

- **H-IB1 (the crossover exists and is small).** For selective probes
  the batch resolution beats the filter scan, and the crossover N — the
  matching-entry count above which the probe stops paying — is a
  function of relation size that IB3 reports as a measured curve, not a
  chosen constant. Prediction, stated to be falsifiable: at 100k rows
  the probe wins at every N ≤ 1,000 in the matrix.
- **H-IB2 (the point probe is the trail's, and the hint waits).** At
  N = 1 with a warm trail the probe's resolution cost equals one replay
  read; cold, it equals one pruned walk. The entry-embedded page hint
  (the survey's c1) is justified **only if** IB3's cold-point cell shows
  a gap the operator prices as worth an entry-width increase — otherwise
  c1 is recorded as refused-for-now with the cell that refused it.
- **H-IB3 (no answer moves).** On every cell of the matrix the probe
  arm and the filter-scan arm answer byte-identically — same rows, same
  values, superset skips and visibility included. This is the gate on
  IB4, not a finding to report.
- **H-IB4 (the degradation is graceful).** At the largest-N cell the
  probe's cost approaches the scan's from above by no more than the
  descent plus the sort — the structural argument of IB-R6, verified.

## Tasks

**IB0 — the rulings land as text.** Amend, in one commit, before any
code: `index.md` IX3 (heap admitted; the batch-resolution mechanism in
§7's cost narrative; the order property of IB-R3; the F1 fence of IB-R5
in the amendment's own words), `index.md` §13 (the per-range/global
question is *not* answered here — one sentence saying so, so the split
gate's citation stays coherent), and `known-gaps.md`'s IX3-adjacent
entries rewritten to point here. Record IB-R1..R6 as ratified or
amended by the operator. Gate: none.

**IB1 — the harness and the baseline arm.** A benchmark fixture
producing heap relations at {1k, 10k, 100k} rows with a secondary-key
column of controlled selectivity, and the **filter-scan arm (A)**
measured on it: statement latency and `rows_examined` for predicates
matching N ∈ {1, 10, 100, 1,000} rows, trail state ∈ {cold, warm}.
Results under `bench/v2.7.2/`, named by `git describe --tags`. Gate:
IB0.

**IB2 — the build.** (a) `CheckIndexDef`'s IX3 arm narrowed: a heap
relation admits a non-unique index; the refusal text moves to the fences
that remain (`:3046`, IX11). (b) The build path: the existing
scan-and-emit through `AppendIndexEntry`, unchanged in mechanism,
exercised on a heap fixture. (c) The probe path: the collect-sort-walk
resolver per IB-R1/R2, emitting per IB-R3, skipping per IB-R4. (d)
Planner admission per IB-R6. (e) Tests: the byte-identity assertion of
H-IB3 as a unit fixture (probe vs scan on the same relation and view,
including a retired-row cell for the superset skip), the no-sortedness
assertion of IB-R3, and the `cores = 1` suite green. Gate: IB0.

**IB3 — the measurement.** The full matrix — arms (A) filter scan and
(B) probe, interleaved, ≥3 runs with min/max/stddev, `build-release`
only — across N × relation size × trail state. Reported: the crossover
curve (H-IB1), the cold-point gap (H-IB2), the largest-N degradation
(H-IB4), and the per-cell answer identity (H-IB3, pass/fail only).
`rows_examined` / matched / `pages_fetched` beside every latency, so the
mechanism is visible in the counters and not inferred from the mean.
Gate: IB2.

**IB4 — the hint's verdict, from the numbers.** If IB3's cold-point
cell prices a gap: a c1 prototype work order is drafted as IB's
successor, carrying that cell as its background. If not: the refusal is
recorded in the IB3 results file with the cell that refused it, and c1
leaves the horizon. Either way this task writes text, not code. Gate:
IB3.

**IB5 — closure.** `known-gaps.md` updated (IX3's entry closed; the
fences of IB-R5 re-stated where they now stand alone); a verdict section
in the IB3 results file stating which of H-IB1..4 held, refused, or
split; the F1 question and the split-gate question named as the two
successors this order deliberately did not take. Gate: IB4.

## Measurement discipline

`build-release` only; `git describe --tags` naming; ≥3 runs with
min/max/stddev; interleaved arms for every A/B; trail state controlled
and named per cell (a cold cell restarts the process, a warm cell runs
the probe once unmeasured first); `rows_examined` and `pages_fetched`
reported beside latency on every cell; no retroactive stamping of
pre-IB results; claims in every results file tagged **measured** (with
invocation) or **source-read** (with `path:line` and commit).
