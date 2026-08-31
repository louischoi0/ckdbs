# Work order AX — coalesce on auxiliary DDL

Drafted 2026-08-31 by CLA against `main` at `04d53f4`
(`v2.2.1-159-g04d53f4`). Decisions: `instructions/v2.7.0/ratification-AX.md`
(AX-D1 through AX-D6, AX-D12, operator 2026-08-31) — **read that first**;
nothing below re-argues a decided item, and nothing below may assume one
of the five placement `[OPEN]`s it moved to R5.

## Background

`RefuseAuxiliaryOnSplitRelation` (`src/catalog/catalog.cpp:1094`) is the
converse arm of `crosscore.md` §6a's split gates: §6a's `RangeEligible`
stops a relation with auxiliaries from splitting, and this function
stops a split relation from gaining one. Its four callers are the index
(`src/catalog/catalog.cpp:3044`), the Cabin (`:2762`, the optimizer's
automatic path routes through the same `Catalog::CreateCabin`), the FK's
both sides (`:2874-2877`) and the assertion
(`src/exec/assertion_catalog.cpp:208`, `:455`). The pair is what makes
the `desc_page_id` first-range-only walks in
`src/exec/cabin_optimizer_exec.cpp` and `src/exec/assertion_build.cpp`
unreachable rather than wrong (known-gaps, the CH3 entry) — an auxiliary
built today on a split relation would be built from range 0 alone.

DA1 (2026-08-31) armed `range_size_ids = 65,536` by default, so a range
opens on workload and the refusal became an ordinary session's
experience: write-then-index is refused for the life of the relation,
nothing merges ranges, `DROP TABLE` + recreate is the way back. The
operator ruled this a defect. The fix ratified is AX-D1(c): the
auxiliary DDL **coalesces the relation to one range synchronously**
(AX-D5), toward the core holding the most pages (AX-D3), then runs the
existing DDL transaction; the five placement decisions stay R5's.

**Scope facts that size the build, both source-read:**

- **The merge is heap-only.** `workplan-range-directory.md`'s D1 is not
  taken and every btree relation is unsplittable (CLAUDE.md milestone
  row; the btree decline sits beside `RangeEligible`'s five gates), so
  a split relation is a heap relation.
- **The relation being coalesced carries nothing else.** §6a's forward
  gates mean a split relation is non-spilling, unindexed, un-cabined,
  FK-free and un-asserted — so the merge moves heap pages and only heap
  pages: no var-heap page, no index maintenance, no cabin state, no
  assertion registry can exist on it.

## Conclusions (decided; the build enacts, it does not revisit)

1. Trigger set is exactly the four explicit DDL callers. The Cabin
   optimizer's automatic path keeps the refusal and its decline counter
   (AX-D12).
2. Absorber = most pages; tie → lowest `core_id` (AX-D3, tie rule
   proposed). `sys.tables.owner_core` is updated to the absorber when
   they differ — a catalog write in core 0's stream, CC11 intact.
3. Synchronous, two-phase: merge completes durably, then the ordinary
   DDL transaction begins. A DDL failure after the merge leaves the
   relation merged — valid, observable, specified (AX-D5).
4. The final directory state is **zero rows** for the relation (a
   directory with rows must partition the whole space from lo = 0, so a
   one-range relation is represented by absence, resolved through
   `sys.tables.owner_core`).
5. After the auxiliary lands, `RangeEligible` refuses re-split exactly
   as today (AX-D6). Spreading and auxiliaries are mutually exclusive
   per relation until R5.

## Hypotheses (each verified or refuted by a named row below)

- **H-AX1 — chain concatenation is a legal merge.** Ranges are disjoint
  and key-ordered and a heap range is its own chain with its own head
  (CC8), and `min_key` is immutable per page (`heap-and-tuple.md`
  §3.1). So linking the per-range chains tail-to-head in range order
  should yield one chain whose page `min_key`s remain usable for
  pruning, with no tuple ever moving to a page whose `min_key` exceeds
  its pk. If source reading refutes this — anything in the walk,
  relayout or recovery assumes one chain per relation with a property
  concatenation breaks — the fallback is page-by-page re-placement into
  absorber-allocated pages, and the cost model changes from
  O(handoffs) to O(rows). **AX2 answers this from source before any
  code.**
- **H-AX2 — the flush dominates.** Per CC10 the handoff is flush →
  durable record → grant; PW1c priced the grant path low, so µs/page
  should be flush-bound and the DDL's added latency ≈ dirty pages ×
  device sync. AX8 measures.
- **H-AX3 — the crash matrix is closed by ordering alone.** CC10's
  "directory durable before any grant" adapted to deletion (AX-D4)
  should leave every crash point resolving to either the original split
  state or the fully merged state, never a hybrid a mount cannot
  serve. AX7's seed sweep confirms or finds the counterexample; H9's
  lesson (a structure published before its router) is the shape to hunt.

## Rows

**AX0 — spec first.** Write the merge sequence into
`docs/spec/crosscore.md` as §6c (or a CC14 row; pick one, not both),
including: the AX-D4 proposed ordering with its proposed tag, the
two-phase DDL structure and its merged-residue-on-failure clause, the
zero-rows final directory state, conclusion 5's standing rule, and
AX-D12's carve-out. Cross-cite `ratification-AX.md`. Nothing in AX1+
lands before this row.

**AX1 — absorber selection.** Page counts per range via
`TableAccess::WalkHeadsFor` (`src/catalog/schema.cpp:92`) or a cheaper
per-range count if one already exists — source-read first, do not add a
walk if a count is cached. Tie → lowest `core_id`. Output: the merge
plan (surviving core, ordered list of departing ranges).

**AX2 — the merge mechanics, and H-AX1's source verdict.** Read the
walk, relayout survey, recovery and `WalkHeadsFor` paths for any
one-chain-per-relation assumption concatenation would break; record the
verdict with path:line. Then build the chosen mechanism: per departing
range — quiesce (in-flight stages finish or cancel; new plans surface
the stale retryable step error, §5's rule), flush, PL-B durable handoff
record per page (`page-lsn-cross-stream.md` §9), grant to the absorber,
absorber's first write restamps per PL-C. Chain link-up happens on the
absorber after grant.

**AX3 — directory contraction and the owner_core write.** The ordering
AX-D4 proposes: contraction durable in core 0's stream **before** any
grant of that range's pages, `owner_core` updated in the same catalog
transaction as the final contraction. Open, to be settled by AX7 and
recorded in AX0's spec text: whether contraction proceeds
range-at-a-time (crash leaves a smaller split relation — still valid) or
all-at-once. CLA's proposal: range-at-a-time, because every intermediate
state is then a state the engine already serves.

**AX4 — two-phase DDL integration.** The four callers change from
`RefuseAuxiliaryOnSplitRelation` → error to: run the merge plan, then
fall through to the existing build. DDL ships to core 0 (CR2/CR5), so
core 0 drives the merge — quiesce/flush/grant legs to departing owners
ride the ring; enumerate the message kinds needed and reuse before
minting (the PW1c legs exist). The refusal function itself **stays**,
callable, for AX-D12's path and for any future caller.

**AX5 — AX-D12's carve-out.** The optimizer's auto path calls the
refusal as today; verify its decline lands in the §6a decline counters
and nothing in the controller retries hot.

**AX6 — observability.** `SHOW META`: `coalesce_runs`,
`coalesce_pages_moved`, `coalesce_us_total`, per core, absent until the
first caller exists (the absent-rather-than-zeroed rule).

**AX7 — the crash matrix.** Simulation harness cells crashing at every
step boundary of AX2/AX3's sequence, seed-driven, oracle asserting: the
mounted relation is either the prior split shape or a legally smaller
one, every row readable exactly once, chain integrity ascending
(H9's checker shape). AX-D4's proposed ordering is promoted to measured
by this row or amended by its counterexample.

**AX8 — measurement, rule 4b.** Build-release only. µs/page moved and
handoff count; DDL latency with coalesce at the extreme reachable on
this host (two ranges is the two-CPU ceiling — state it, per DA-c's
precedent, and report the per-page figure so a larger host's number is
predictable); `cores = 1` untouched (Guideline 2 — no lease, no range,
the path is unreachable); the k-sweep insert numbers unmoved for
relations that never meet an auxiliary DDL. Baseline: our own prior
numbers, named by `git describe --tags`.

**AX9 — docs closure.** `known-gaps.md`'s "does not recover" entry
amended to decided-and-fixed with the commit; `ratification-da.md` gains
the AX cross-note; blueprint §11's R5 row cites "auxiliary placement
decision group (AX-D1)"; CLAUDE.md's milestone row updated last, one
line.

## Measurement

Every claim lands tagged: **measured** with the invocation and
`git describe --tags`, or **source-read** with path:line at commit.
AX8's numbers go to `bench/v2.7.0/results-ax-coalesce-<describe>.md`.
No constant is chosen in this order — AX introduces none; if the build
finds one wanting (a quiesce timeout, a batch of grants), it stops and
reports rather than picking.

## Improvement

What this order buys: the last non-recovering refusal of DA1's ten
becomes a synchronous cost, order between writes and auxiliary DDL stops
being destiny, and CC10's migration sequence gains its first exercised
consumer — which is R5's mechanism half arriving early, leaving R5 the
policy half alone. What it does not buy, stated so nobody reads it
wider: no auxiliary lives on a split relation (that is R5's decision
group), a coalesced relation forgoes spreading while its auxiliary
lives (AX-D6), and the automatic optimizer still declines (AX-D12).
