# Work order CC — statistics and identifiers in the join planner

**CLOSED 2026-10-19 on the operator's word** (`raft-marks-2026-10-19.md` §1),
pushed to `main` at `8fbad5f7`; §11 is the stage log.

Written and opened 2026-10-18 on `worktree-cc-stats-planner` from `65dc45c5`
(`v3.2.0-178-g65dc45c5`, CB merged and pushed), on the operator's words,
recorded in English (`raft-marks-2026-10-18.md` §2):

- **W0**: *"Explore and develop a layer that heuristically finds the best path
  when planning, using statistics and identifiers"*, with
  `/go-ahead-achieving-milestone`, and, sent again: *"... the purpose is query
  performance optimization."*
- **Marks**: every CC-Q item below is CLA's proposal, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone),
  not the operator's own per-item mark. CC-Q1 (a) is read as what W0 itself
  orders: statistics reach the plan, which supersedes CB-Q2 (a) for the order
  of a planned block.

**How the design was reached.** A five-lens survey of the tree (statistics,
identifiers, the planner path, contracts, bench evidence), three independent
designs (catalog statistics first; identifier-keyed feedback first; hybrid with
plan stability), three judges (correctness and contracts; OLTP overhead and
gain; buildability), and one synthesis that grafted the judges' best ideas and
fixed every flaw they raised. The judges scored the identifier-keyed design
highest (7.5 / 8 / 6.5). The sections below are the synthesis, with
`path:line` read at `65dc45c5`.

**Claim tags**: `[source-read]` with `path:line` at `65dc45c5`; `[design]` for
code not yet written; `[quiet-wrong]` where a wrong choice turns a correct
answer into a wrong one with no error.

**The letter.** `[source-read]` CB is the last order under
`instructions/v3.2.0/`; `origin/main` at `65dc45c5` names no CC. CLA's choice.

---

## 0. Overview

The synthesis was read in the `cc-stats-planner` worktree on `worktree-cc-stats-planner` at `65dc45c5` (v3.2.0-178-g65dc45c5).

STARTING POINT. Design 2 had the highest overall score (7.5 / 8 / 6.5). From it CC takes the narrow contract (order only), the exact-key decision memo that always compiles the written order first, and the access-invariant fold at RecordExecution. Four grafts replace parts of it:
- Design 3's subset-cardinality posterior card(T), with the mask corrected to the written step_ids of the executed prefix.
- Design 3's instance-wide banded snapshot, so a generation gives one plan on every core.
- Design 1's shared_ptr pin instead of iteration-based retirement.
- Design 1's exclusion of advisory inputs and its test seam that publishes a snapshot.

CC also fixes three things the judges found in all designs: the tie-margin semantics, the missing ORDER BY/LIMIT pricing, and the unique-index and NDV bound directions.

WHAT CC CHANGES, at `65dc45c5`:
- (1) ChainCost (src/exec/join_order.cpp:34-158, 160 lines; build pricing at :134-141) reads three inputs in place of kAssumedRelationRows=100000 and the constant join selectivity (include/kds/exec/join_order.hpp:23-46), wherever they are known:
  - (a) a per-relation row prior estimated from clustered-tree internal pages;
  - (b) observed join-subset cardinalities card(T), keyed by statement identity;
  - (c) identifier bounds that are free from TableAccess.
- (2) It prices what CB ignores: an ORDER BY elision that an order loses, a sort that would exceed sort_max_rows, and LIMIT.
- (3) CompileBlock (src/exec/step_compiler.cpp:2068-2205) picks the minimum over all candidate orders. It leaves the written order only by a margin measured against the written order's cost, which replaces the best-so-far comparison at :2170.
- (4) A per-core order memo keyed by identifiers turns the n! per-execution search into 1-2 compiles.

WHAT CC DOES NOT CHANGE:
- The access ladder (step_compiler.cpp:2457-2554), every legality test, Cabin selection, DML (CompileWhere), the query layer, sub-blocks, SELECT * and subquery-predicate blocks (:2123).
- The access kind of a slot stays f(shape, catalog, predecessor set).

The amendment is honest about IX9. A data-driven order changes which correlated index, build or Cabin arm a step takes, so IX9's 'must not compile differently as the data changes' (docs/spec/index.md:466-469) is re-scoped, not left as it is.

OLTP-FIRST:
- A point read leaves CompileBlock at :2123, and a tuple-key point join leaves it at the all-descent return :2161-2163, before any statistics read.
- Neither carries a plan record, so RecordExecution (src/server/command_dispatcher.cpp:7660-7667) adds one null compare for them.
- Nothing is added to INSERT, UPDATE or DELETE.
- Everything is memory-resident: no WAL record, no catalog relation, no superblock bump.

Gate: CC-Q1 (a) is adopted on the operator's standing go-ahead of 2026-10-18, read as W0's own order (§ header). S1's data-free part (tie margin, sort and LIMIT pricing, identifier bounds, doc and test fixes) lands first.

## 1. Statistics

All of this was read in the `cc-stats-planner` worktree at `65dc45c5`. The engine keeps no row count, NDV or histogram today. CC collects three kinds of fact into one instance-owned `stats::PlanStats` (new: include/kds/stats/plan_stats.hpp, src/stats/plan_stats.cpp). All of it is memory-resident: emptied by a restart and by a restore, never logged, never a catalog relation.

(1) RELATION PRIOR, keyed by rel_oid. Computed by a steward step on core 0.
- It is read from the clustered tree's own pages through GetForRead, the API BtreeHeight uses (src/storage/btree/btree.cpp:1343-1355).
- Height 1: the root is the leaf, and the prior is its live slot count (slot_count minus delete-marked slots; include/kds/storage/heap/heap_page.hpp:232-236). This is exact up to undecided versions.
- Height 2: leaves = root InternalView::entry_count()+1 (include/kds/storage/btree/btree_page.hpp:142-143), which is exact.
- Height 3 or more: Knuth's random-path estimator over internal levels only, k=4 descents that stop at level 1. Its pages are the ones every probe already walks, so they are normally resident.
- rows = leaves × LeafRowCapacity(row_size) × kAssumedLeafFill.
  - row_size is the fixed row width on the relation's layout (include/kds/catalog/schema.hpp:156; invariant 13).
  - LeafRowCapacity is a new helper beside heap_page.hpp.
  - kAssumedLeafFill is a proposed 0.75 (CC-Q6). An append-split leaf is full and a mid-split one is about half full, so the error is bounded at ×1.5.
- It reads no leaf above height 1, no next_id (GetSysTableRow is a sys.tables ScanAll, src/catalog/catalog.cpp:1520-1528) and no key domain.
- Every page read checks page type and owner_oid (include/kds/storage/page_header.hpp:44-47, owner_oid at offset 24). A mismatch discards that relation's seed for the step.
- Cost: at most 1 + 4×(h-2) page reads per relation. The step budget is 16 page reads.

(2) SUBSET CARDINALITY POSTERIOR card(T), keyed (fetch_id, n, exact bound-oid vector in written order, mask T).
- T is the set of written positions of executed slots 0..k. chain.steps[k].step_id − base is that position, because CB-Q3 rewrites it (step_compiler.cpp:2140-2144). Judge 2 caught that 'the first k written positions' is wrong under a permuted order.
- The value is the step's rows_matched for executed slot k (src/exec/step_vm.cpp:2534). That counter is bumped after the residual and before sub-chains, on every serving path: probe-memo hit through AcceptTupleAt (:688-690), build probe, Cabin serve, index and walk. Keys stay in the residual (index.md:477-480).
- A planned block is top-level only, so it has no outer bindings, and its conjuncts sit on the latest step they read. So card(T) is the row count of the join of T under every conjunct over T. It does not depend on the order within T or on the access kind. Measurements taken under one order price another.
- A built inner step that published its map (inner_builds==1) also contributes build_rows as card({w}) for that relation alone under its non-correlated conjuncts (include/kds/exec/step_vm.hpp:196-206).
- An annotated build step with inner_builds==0 on a complete run with join_build_max_rows>0 is a DECLINE fact for (key, written position). It is set, never averaged.
- The value is a mean over the fetch_id's literals. arg_hash is not in the key (CC-Q12).

(3) IDENTIFIER BOUNDS, read from the cached TableAccess at compile with no collection:
- tuple key equality ≤1 row (already in the model);
- a unique index with EVERY key column equality-bound ≤1 row (IndexRef::unique and nkeys, schema.hpp:478-481), never on its leading column alone;
- an FK child→parent join is the parent's tuple key (already key_join, join_order.cpp:72-73);
- NDV upper bounds from a Transition state set (schema.hpp:303-312, 433) and from an FK's parent prior.
An NDV upper bound may only RAISE an equality's selectivity to 1/bound, never lower it, because uniform-average selectivity is ≥ 1/NDV (CC-Q11). This corrects Design 3, which applied it in the unsafe direction.

WHERE AND HOW COLLECTED
- Fold: a new CommandDispatcher::RecordPlanStats beside RecordOptimizerSignals in RecordExecution (command_dispatcher.cpp:7660-7667). RecordExecution is reached on all three success paths (:5107 sliced, :7163 aggregated, :7261 ANALYZE).
- A fold happens only when the run carries a PlanRecord, and only when the run was not cut. ExecStats gains a `stopped` flag set from the VM's stopped_ (step_vm.cpp:616, reset at :296), so a LIMIT early stop folds nothing. A failed run never reaches RecordExecution.
- It writes into THIS core's part, a fixed table under its own LatchKind::kStatsRing latch. Its only writer is its core, and the steward is the only other reader. This is sys.statement_stats' per-core-part protocol (include/kds/stats/statement_stats.hpp:26-37), so there is no cross-core write on the statement path.
- Each part stores the sum and count per key and mask since the last merge, plus decline bits, plus the oids it saw without a prior (the 'want seed' set). That is about 1024 keys × 16 masks per core, fixed, with no allocation on the path. A full part drops and counts.

STEWARD (core 0, `system` group, scheduler.SubmitEvery at 1 s, beside the reclaim tick at src/server/expeditor.cpp:2052-2063), one bounded step per tick:
- (a) merge every part under its latch;
- (b) EMA each posterior (alpha 1/4, a shift, fixed point; PO3, docs/spec/physical-optimizer.md:206);
- (c) seed wanted oids, and re-seed known ones round-robin at most every 10 s or when the root page id moved, within 16 page reads;
- (d) band every value;
- (e) publish only when a band moved.
- It takes no statement epoch. It runs synchronously on core 0 like the Cabin tick, and the reclaim step that frees clustered pages runs there too, so it cannot free a page mid-step (include/kds/server/statement_epoch.hpp:34-38; FreePage callers src/server/page_reclaim.cpp:404, src/exec/index_ddl.cpp:169 for index trees only).

BANDS: b = round(2·log2 v), half-octave, with Schmitt hysteresis. A published band moves only when |2·log2 v − b| ≥ 0.75, about ±30%. A decline fact bypasses the hysteresis and publishes at once (the safe direction).

PUBLICATION: an immutable sorted flat `PlanStatsSnapshot` held by std::shared_ptr<const>. The steward swaps it under a mutex and bumps a u64 generation (release).

EXPOSURE:
- A SHOW META `plan_stats` block (beside command_dispatcher.cpp:1611-2068): generation, relations, posteriors, part_drops, seeds, seed_pages, steward_us_max, last_publish_age_ms.
- sys.relation_stats (src/server/sys_views.cpp:615-660) gains est_rows, rows_band and generation. This re-scopes an existing view, adds no new one, and is never read by the planner.

Advisory state is NOT an input: Waystone trails, sys.patterns, sys.access_stats, OptimizerSignals, the CabinStore and index trees are excluded.

## 2. Identifiers

All of this was read in the `cc-stats-planner` worktree at `65dc45c5`.

fetch_id
- Keys posteriors and the memo.
- It reaches the compile through PlanOptions. It is taken from parser.fingerprint(), which already exists before the compile (command_dispatcher.cpp:7343-7346; compile at :7434-7439).
- Not pattern_id: a select list changes no join-subset cardinality, and SELECT * is never planned (:2123).
- fetch_id hashes names, not oids (include/kds/parser/fingerprint.hpp:44-48), so it is never used alone.

Exact bound-oid vector in written order, plus n
- Read from scope.relations after the bind loop (step_compiler.cpp:2096-2112).
- Every key match compares fetch_id, n and every oid exactly, never a hash alone. A collision only misses, and a permutation is never applied to another relation set.
- `USE ns` and DROP-and-reCREATE reach new entries.
- Oids are never reissued (catalog.cpp:994-1010; docs/spec/drop-table.md), so a dead oid's entries are unreachable. The steward prunes an oid whose InitTableAccess fails.

step_id
- Stays the written position (CB-Q3, step_compiler.cpp:2140-2144). CC promotes it to a stated invariant in parser-v2.md.
- Posterior masks, decline facts and Waystone replay (include/kds/exec/trail_replay.hpp:31-62) all key on it.
- The memo path compiles only through CB's compile_order lambda (:2128-2146), which performs the rewrite.

rel_oid: keys relation priors. index_oid keys nothing, so REINDEX's new oid (src/exec/index_ddl.cpp:677) loses nothing.

Snapshot generation (u64)
- Names the statistics a plan was costed under. It is printed by ANALYZE and SHOW META, and is part of the memo key.
- It is NOT the schema word: statistics never bump the word (docs/spec/catalog.md:45-62). The word moves on every root growth (catalog.cpp:2870-2892).

Catalog stamp
- The memo entry stores the core's cache_built_at_ value. Catalog::Revalidate drops the catalog memo when the word moved (catalog.cpp:1069-1080). A memo entry is reused only while that value equals its stamp, so DDL, index or Cabin create/drop, catalog-writer decides and root moves all force a re-search.
- The memo is bypassed whenever ResolveNames mints a view because DDL is in flight (include/kds/server/command_dispatcher.hpp:1166-1201).

Session limits in the memo key: join_build_max_rows and sort_max_rows (command_dispatcher.hpp:353-354, 1680-1685). Both are session-settable and both change the cost.

Literal class
- The only literal value ChainCost reads is a kRange span (join_order.cpp:102-107). CC quantizes that span to its half-octave band centre inside ChainCost, so a plan is an exact function of the band.
- The memo key carries the band of each kRange step of the written chain, which every planned compile builds anyway for its refusal (:2150-2151).
- arg_hash is not used: it hashes literal text (src/parser/fingerprint.cpp:352-388) and carries no selectivity class.

Not used: pattern_id, the sys.patterns rows (use_count reserved 0), KWP PARSE handles (they return pattern_id from a second lex) and arg_hash. OptimizerSignals stays keyed by pattern_id, unchanged.

## 3. The estimator

File:line targets are from the `cc-stats-planner` worktree at `65dc45c5`.

1. CARRIER
- PlanOptions (include/kds/exec/step_compiler.hpp:77-82) gains these fields:
  - `std::uint64_t fetch_id`;
  - `std::size_t sort_max_rows`;
  - `PlanStatsSource* stats`, a pointer to the dispatcher's per-core source object. No load happens at construction;
  - `PlanMemo* memo`;
  - `PlanRecord* record`, an out-param the dispatcher keeps on the SelectRun beside the chain, NOT inside StepChain. Chain identity tests (tests/aggregate_contract_test.cpp:102-106, :271-275) therefore see no new field.
- The dispatcher fills these at command_dispatcher.cpp:7436-7438. That constructor runs per compile already, so a point read pays only the extra stores.

2. READ POINT. In CompileBlock, after the written order compiles and after the all-descent return (step_compiler.cpp:2150-2163), and only when plan != nullptr:
- `stats->Pin()` does one acquire load of the instance generation. If the generation differs from the core's cached one, it locks the publish mutex once and copies the shared_ptr. It returns a const snapshot reference.
- A `PlanPriors` value is built once on the stack from that reference:
  - each slot's rows band, by binary search on rel_oid;
  - the block's up to 16 posterior bands and decline bits, by exact key;
  - the identifier bounds from each slot's TableAccess.
- Every later ChainCost call reads that copy. Up to 24 CompileBound calls cost one lookup. The shared_ptr pin lives on the run, so a sliced walk's yields hold no raw pointer.

3. CHAINCOST. The signature becomes ChainCost(chain, build_cap, const PlanPriors*). Passing nullptr gives CB's arithmetic except for S1's data-free corrections.
- Inside Estimate (join_order.cpp:34-145):
  - (a) `own` starts at the slot's rows prior, falling back to the constant (:37). The walk's per-execution cost becomes rows (:140), and the build's once-cost becomes rows + own×kBuildInsertCost (:137).
  - (b) The kRange span is band-quantized and divided by the rows prior instead of 1e5 (:102-107). This assumes dense keys, which is stated; it is exact for issued keys before deletes.
  - (c) An equality on a column with an NDV upper bound B uses max(kEqualitySelectivity, 1/B) (:90).
  - (d) A step with every key column of a unique index equality-bound, by literals or earlier columns, sets own or matches to at most 1.
  - (e) A non-key join equality uses 1/rows(inner) instead of 1/1e5 (:112, kJoinSelectivity).
- The ChainCost loop (:149-158): after executed slot k, `rows` is card(T_k) when that band is known for the prefix's written-position mask; otherwise rows_{k-1} × matches_k. When card({w}) is known for a step, `own` takes it for build and walk pricing.

4. BUILD (CC-Q9). A build is priced as a build only when own×4 ≤ build_cap and no decline fact exists for (key, written position). Otherwise it is priced as the per-row walk (:134-141). An under-estimate is the catastrophic direction: the executor declines into a per-row walk (step_vm.cpp:1129-1160).

5. ORDER BY AND LIMIT (CC-Q10). ChainCost reads the compiled chain's sort_keys and limit. The elision is computed per compiled chain at step_compiler.cpp:2674-2681.
- If the chain has a sort, add out × log2(out), where out is the final rows.
- If the chain has a sort the written chain did not have, and out > sort_max_rows/2, the cost is +inf. The planner itself never moves a statement onto a ResourceExhausted refusal.
- If the chain has no sort and has a LIMIT, scale the per-row part by min(1, (limit+offset)/out). Once-costs are not scaled.

6. CLAMP. Every rows value, card band and prior is ≥1. A zero is never priced as free, and no statistic is read outside ChainCost (invariant 9's analogue).

7. CHOICE (CC-Q8) replaces best-so-far at step_compiler.cpp:2166-2176.
- Compile and cost every permutation and take the minimum. Ties go to the lexicographically first permutation, which is the written order when it ties.
- Adopt the minimum only if cost_min < cost_written × (1 − m).
  - m = kCostTieMargin = 1e-9 when no snapshot fact entered any slot. This is CB's behaviour.
  - m = kStatsSwitchMargin = 0.10 when one did.
- The winner then no longer depends on next_permutation order.
- Greedy above 4 relations (:2177-2202) reads priors and bounds, takes no posteriors, and uses the same margin rule.

8. MEMO (CC-Q12). A per-core fixed table of 1024 entries, 2-way set-associative, replace on miss, no clear-whole cliff.
- Probed after the written compile and the descent skip.
- Hit: compile the remembered permutation through compile_order if it differs from the written one (1-2 compiles, against n!).
- A refused remembered order invalidates the entry and returns the written chain.
- Miss: run 7, then store the permutation.

9. PlanRecord: the key, base_step_id and the written position of each executed slot. It is set only on a planned chain, so RecordPlanStats returns at one null compare for every other chain.

10. ANALYZE (src/exec/plan_printer.cpp:405-424) prints:
- per step, est=<rows> and obs=<rows_matched>;
- per block, order=written|planned|memo, gen=<generation>, and switch=<cost ratio>.

## 4. Plan stability

All of this was read in the `cc-stats-planner` worktree at `65dc45c5`.

THE CONTRACT. A top-level planned block's order is f(shape, catalog, snapshot generation, kRange span bands, join_build_max_rows, sort_max_rows). The memo is an exact cache of that function, so it adds no history dependence.
- A slot's access kind is f(shape, catalog, the set of relations before it). The ladder is unchanged.
- Every refusal and its byte position stay the written order's: it compiles first on every path, the memo hit included (step_compiler.cpp:2121-2122, :2150-2151, :2168).
- Every core reading generation g compiles the same order. Two cores briefly on different generations are safe, because every order is result-neutral.

WHEN A PLAN MAY CHANGE:
- (i) the catalog changes, as under CB;
- (ii) the generation moves, which happens at most once per steward tick (1 s) and only when a half-octave band crossed its Schmitt margin, or at once on a decline fact;
- (iii) a range literal moves to another span band;
- (iv) a session limit changes;
- (v) a restart or restore, which returns to CB's data-free costs until the first publish.

Even when (ii) happens, a new order must beat the WRITTEN order by 10%. This fixes the best-so-far semantics (step_compiler.cpp:2170), whose winner depended on enumeration order.

FEEDBACK STABILITY
- card(T) is a data fact, independent of the order and of the access path. Measuring it under one plan does not bias another plan's estimate of the same subset, so a plan cannot oscillate on its own measurements. Only data crossing a band back and forth can move it, and the Schmitt margin absorbs ±30% around a boundary.
- A subset no executed order visited keeps its prior, which is the 'unvisited subset' blind spot all judges named. CC narrows it with build_rows as card({w}) for built inner relations and with priors for every bound relation, and states the rest in known-gaps.
- The OptimizerSignals CandidateRef is the first kFilterScan in EXECUTED order (command_dispatcher.cpp:7848-7886). CC-Q15 derives it in written-position order instead, so a data-driven reorder cannot move the Cabin controller's candidacy.

WAYSTONE
- Trails stay keyed (fetch_id, arg_hash) and are replayed by (step_id, key) with rel_oid checked. step_id is the written position, so a reorder costs at most misses. IsTrailReplayable stays kLookup/kProbe only (include/kds/exec/step_chain.hpp:172-174).
- The plan inputs exclude every advisory state, and rows_matched is invariant to replay, Cabin hit or miss, `indexes` and recording. So deleting Waystone or toggling recording or access_statistics changes no plan input, and invariant 8 holds in its byte-for-byte reading (docs/spec/waystone-concpets.md:175) within one history.

CONTRACT SUITES. They construct CommandDispatcher with no Expeditor, so they run with no steward and an empty snapshot.
- S2 adds CommandDispatcher::PublishPlanStatsForTest(snapshot) and ClearPlanMemoForTest().
- Each of the four suites (waystone, index, cabin, inner-build) runs its configuration comparison twice: under the empty snapshot and under one pinned populated snapshot. A suite that only ever saw the cold path would not test the shipped planner.
- Comparisons without ORDER BY adopt CB-Q1's multiset rule (tests/cabin_contract_test.cpp:183-200).
- The LIMIT 3 join without ORDER BY (tests/index_contract_test.cpp:219-220) gains ORDER BY.
- The written-order pins (tests/inner_build_contract_test.cpp:128-129; tests/waystone_contract_test.cpp:571) stay under join_order=written.
- index_contract_test.cpp:35-36 ('the index changed neither the plan nor the answer') stays true. Priors and posteriors are index-blind, so configurations with and without the index see the same inputs.
- AG1 (tests/aggregate_contract_test.cpp:102-106) gains a PlanOptions case with a pinned snapshot. The cost reads neither the projection nor the aggregate, and PlanRecord lives outside StepChain.
- 'Same statement, same catalog, same chain' (:271-275) becomes 'same snapshot'.

OFF SWITCH. `join_order = written` (command_dispatcher.hpp:1688, :2396) passes plan=nullptr, which means no order search, no statistics, no memo and no fold. No new key is added.

## 5. Hot-path cost

All of this was read in the `cc-stats-planner` worktree at `65dc45c5`.

POINT READ (single relation, about 48 us)
- The compile adds only the new PlanOptions field stores at command_dispatcher.cpp:7436, which already runs once per compile.
- CompileBlock returns at step_compiler.cpp:2123 (refs.size()<2) before any Pin, memo or snapshot access.
- RecordExecution adds one null compare on the run's PlanRecord.
- There is no atomic, no latch and no allocation. Expected: unmeasurable.

TUPLE-KEY POINT JOIN (all descents)
- The written order compiles as today. The all-descent return at :2161-2163 comes before the Pin and the memo.
- There is no PlanRecord, so the fold is one null compare. Zero added work otherwise.

INSERT, UPDATE, DELETE and the KWP load: nothing. There are no commit-time deltas, no per-row counters and no change on the write path.

PLANNED JOIN (2-4 relations, not all descents) pays:
- one acquire load of the generation (a mutex and a shared_ptr copy only on the first compile on that core after a publish, at most once a second);
- one PlanPriors build: binary searches per slot, one exact posterior lookup and TableAccess bound reads, about 100-300 ns, inferred;
- one memo probe.
- On a hit, CB's n! compiles become 1-2. At CB's measured 15 us for 6 orders (step_compiler.cpp:2155-2157) this saves about 10 us on a 3-relation block and about 50 us on a 4-relation one, inferred.
- At the end, one uncontended per-core kStatsRing latch, the statement_stats protocol (include/kds/stats/statement_stats.hpp:26-37), with at most 4 sum/count updates and a decline bit.

STEWARD (core 0, `system` group, once per second):
- merge the per-core parts, one latch each;
- the EMA fold;
- at most 16 page reads for seeds, on internal pages that probes keep resident, plus a height-1 root leaf;
- a snapshot rebuild only when a band moved.
A fault is a synchronous pread on core 0's reactor (src/storage/file_page_device.cpp:102), so a cold seed can stall core 0's sessions for up to 16 reads. That is why the close measures point-read p99 on core 0 and the pool hit/miss delta, not only mean CPU.

GATE: interleaved A/B in build-release. Point read and tuple-key point join server CPU per statement and p50/p99 within the bn-census noise (2-3%), and single-row INSERT and UPDATE within noise. Steward at ≤1% of core 0, with no measurable pool miss delta.

## 6. Contracts amended

- docs/spec/index.md:459-469 (IX9): re-scoped, not left as it is. A slot's index selection stays f(shape, catalog) given the relations placed before it. The block's order, and with it which correlated index, build or Cabin arm a slot can take, becomes f(shape, catalog, statistics generation) for a planned top-level block. The reason 'pattern_id names a plan' is replaced by CB-Q3 (written-position step_id) plus rule 0 (keys stay in the residual, index.md:477-480). The crossover paragraph (:471-475) stays an open gap, since CC does not rank kinds.
- docs/spec/index.md:929-933 (section 12.3): unchanged ('indexes' steers inside a kind). Add one sentence: statistics move only the order, at compile, never at run.
- docs/spec/parser-v2.md:138 (I12 as CB amended it) and :149: the order is f(shape, catalog, generation, span bands, session limits). Refusals stay the written order's. CB-Q3 is stated as an invariant beside test 6 (:241).
- docs/spec/parser-v2.md:58, :69, :72, :94, :244: stale since CB ('written order is execution order', 'pure function of the AST without its select list', already false through stmt.star() at step_compiler.cpp:2123, 'no plan enumeration exists', test 8). Rewritten.
- docs/spec/join-inner-build.md:99-104 (section 5, 'No statistics, no cardinality estimate'): the annotation stays shape-driven. The order that lets a build pay is priced from estimates under the cap/4 margin and the decline fact. The lazy build stays the runtime guard.
- docs/spec/cabin.md:160-163 and :210: narrowed to 'nothing about the data chooses a step's kind or Cabin given its predecessors'. The order around a Cabin step may read relation statistics, never Cabin runtime state. include/kds/exec/step_chain.hpp:99-102 stands.
- docs/spec/aggregate.md:22 (AG6 'deterministic row stream') and :221-223 (pure compile): restated under one snapshot. AG3 records the order-dependent SUM overflow that docs/inflight/known-gaps.md:1324-1332 says aggregate.md owns, now movable by data as well as by catalog.
- docs/spec/parser-v2.md:204 (max_rows_touched): recorded as order-dependent and therefore movable by statistics.
- docs/spec/crosscore.md:36-37 ('never silently reordered') and docs/spec/waystone-concpets.md:35, :102, :177: corrected to planned execution order, with trails keyed by written position.
- docs/spec/physical-optimizer.md (Part II, CandidateRef): the candidate is derived in written-position order (CC-Q15). This is a stated input change to the controller.
- New docs/spec/plan-stats.md (CC-Q18) owns collection, the per-core parts, the steward, bands, publication, the read rule and the 'advisory inputs excluded' rule. It is written in S2 before the code, per R2 (physical-optimizer.md:42). It cites statement_stats' part protocol as the adopted discipline.
- include/kds/exec/join_order.hpp:7-46 ('Data-free ... never a row count'), include/kds/exec/step_compiler.hpp:21-27, src/exec/step_compiler.cpp:988-994, :1236-1242, :1479, :2040-2042 and :2114-2122: comments rewritten to the new contract and the corrected margin semantics.
- include/kds/catalog/schema.hpp:540-548 and include/kds/catalog/catalog_cache.hpp:22-23 ('a fact that moves without DDL is not cacheable'): reaffirmed, unchanged. Statistics never enter TableAccess or the catalog memo.
- docs/rules/rules.md:69 and docs/spec/ddl-transactional.md:516-522: NOT amended. CC adds no relation, so sys.access_stats stays the sole unlogged statistic relation (holds under CC-Q2 (a)).
- manual/sql/sql.md:1071-1075 ('never from row counts ... one statement over one catalog always runs one plan'): becomes 'one catalog and one statistics generation'. sql.md:1325-1327's keyset advice gains ORDER BY (CC-Q17).
- CLAUDE.md:50 (query-language row, 'data-free cost model ... one catalog gives one plan'): flipped at the close in one line linking plan-stats.md.
- Rulings superseded by CC-Q1's ruling, operator only: CB-Q2 (a) (instructions/v3.2.0/workorder-cb-join-planner.md:74; instructions/v3.2.0/raft-marks-2026-10-18.md:11).
- Tests: tests/index_contract_test.cpp:219-220 (LIMIT 3 gains ORDER BY), index, inner-build and waystone suites to the multiset rule and run under an empty and a pinned snapshot, tests/aggregate_contract_test.cpp:102-106 and :271-275 restated under a pinned snapshot.
- Stale comments fixed on the way: include/kds/stats/decay.hpp:20-22 ('nothing consumes this'), include/kds/exec/assertion_check.hpp:30 ('plan cache'), include/kds/stats/access_stats.hpp:11-13 (relayout shadow).
- docs/inflight/known-gaps.md:1340-1345 (data-free planner) closed for 2-4 relation blocks. New entries: the cold window after mount or restore, unvisited-subset priors, the leaf-fill constant, more than 4 relations (priors only), and the kind ranking (IX9 crossover) still data-free.

## 7. `[quiet-wrong]` risks

- [quiet-wrong] ORDER BY elision on a planned driver. step_compiler.cpp:2674-2681 drops the sort when executed slot 0 is not kCabinProbe. CC adds no AccessKind, and the elision is computed on each permuted chain, so it is correct. Statistics, however, put new relations on slot 0 far more often. S1 adds a test that enumerates every AccessKind against driving_emits_tuple_key_order, so a new kind fails until the predicate names it, plus a planned order that moves the ORDER BY relation off slot 0 and must keep its sort.
- [quiet-wrong] Keyset pagination over a join. manual/sql/sql.md:1325-1327 recommends `WHERE id > <last> LIMIT n` with no ORDER BY. Over a join, an order change between pages (a generation move) silently skips or repeats rows. For a single relation it is not quiet-wrong: one relation is never planned (step_compiler.cpp:2123) and every single-relation kind emits tuple-key order. The manual adds ORDER BY anyway (CC-Q17).
- [quiet-wrong] A statistic used as authority. A zero card(T), a zero prior or a decline fact must never skip a step, shorten a walk, declare a result empty or replace a legality test. Clamps of ≥1 apply, and nothing outside ChainCost reads PlanStats. Legality stays untouched: the predicate Cabin's canonical-text match (step_compiler.cpp:1337-1349), SameDescriptor (:764-766), BuildKeyOf (:1483-1522), BuildIsArmable (step_vm.cpp:1099-1106) and the correlated-index leading-column guard (step_vm.cpp:1418-1427). A test with rows=0 priors and posteriors pins that the walk still runs.
- [quiet-wrong] step_id rewrite skipped. If the memo path called CompileBound directly instead of compile_order (step_compiler.cpp:2128-2146), executed positions would become step ids. In a self-join, only rel_oid (trail_replay.hpp:54-62) would then stand between a wrong replay and a miss, and posterior masks would fold into the wrong subset. The rule: every planned compile goes through compile_order, tested with a self-join replayed under two generations that choose different orders.
- [quiet-wrong] Memo applied to another relation set. A permutation is applied only after an exact match of fetch_id, n and every oid, never a hash alone, plus the catalog stamp and the generation. It is then recompiled through the full ladder, so it never carries a stale chain's literals or root_page_id (step_chain.hpp:228-230, 278-300).
- Advisory-only by construction: the steward reading a freed and reused page. The page type and owner_oid check (page_header.hpp:44-47) discards the sample. Since nothing acts on a prior for correctness, a wrong prior costs speed only. If any later consumer acts on a CC statistic for correctness, the statistic becomes 'a relation a reader may act on' (rules.md:69).
- Not quiet-wrong, but order-dependent outcomes now move with data: SUM overflow over a join (known-gaps.md:1324-1332), max_rows_touched (parser-v2.md:204), emission order without ORDER BY, and the rows a LIMIT without ORDER BY returns. None is a wrong row. All are recorded (CC-Q16). The sort_max_rows refusal a lost elision could add is priced at +inf and is not created by the planner.
- Not quiet-wrong: the catastrophic slowdown. A stale low estimate can choose a build the executor declines into a per-row walk (step_vm.cpp:1129-1160). This is bounded by the cap/4 margin, and a decline fact publishes at once and flips the order back within one tick.

## 8. Items

| # | Question | Options | CLA's proposal and reason | Mark |
|---|---|---|---|---|
| CC-Q1 | What may the plan read? IX9 (docs/spec/index.md:459-469) and CB-Q2 (a) (workorder-cb-join-planner.md:74) say only the shape and the catalog. | (a) The order of a planned top-level block reads the published statistics snapshot. The ladder, Cabin selection and every legality test stay f(shape, catalog) given the predecessors. IX9 is re-scoped, because the order moves which correlated index or build a slot takes.<br>(b) (a) plus ranking among legal kinds: index against walk, correlated index against build (Design 1's S3).<br>(c) Stay data-free. CC ships only S1 and the memo (S4) on CB's costs. | **(a)** - The measured and inferred gains (s1 compare-all, s2 customer-statement) are order and build-pricing gains. The kind decisions measured break-even (books-by-genre 2,111 against 2,113 qps; symbol-history -3%). (b) puts statistics risk on single-relation OLTP shapes, and its kFilterScan swap feeds the Cabin controller a candidate no plan probes. (c) is the fallback if the ruling is no. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q2 | Where do planner statistics live? | (a) Memory-resident only. Empty after a mount or restore; data-free costs until the first publish.<br>(b) An unlogged catalog relation (rules.md:69's class test, superblock 31, older volumes refused).<br>(c) A logged relation. | **(a)** - The seed reads at most 1 + 4(h-2) internal pages per relation, and posteriors re-converge within a few executions. (b) is a second unlogged statistic against ddl-transactional.md:516-522's 'sole'. (c) puts WAL on the statistics path. Removal first: nothing new on disk. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q3 | Fold topology: how do executions reach the instance's statistics? | (a) A per-core fixed part under its own kStatsRing latch, written only by its core and merged by the core-0 steward each second (statement_stats.hpp:26-37 precedent). One snapshot for the instance.<br>(b) A per-core unsynchronized store and per-core plans (Design 2).<br>(c) Per-core SPSC rings and iteration-based snapshot retirement (Design 3, CA-R4).<br>(d) One instance table under one latch. | **(a)** - It reuses an existing, uncontended protocol: no cross-core write on the path and no new reclamation clock. It gives one plan per generation on every core, which (b) cannot. (c) needs Scheduler::iterations() made atomic (scheduler.hpp:36-43, 142) and an unbuilt retirement scheme. (d) contends across cores. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q4 | How does a compile read the snapshot? | (a) A per-core PlanStatsSource: an acquire load of the generation, and a mutex plus shared_ptr copy only after a publish; the pin is held on the run.<br>(b) atomic<const Snapshot*> with epoch retirement.<br>(c) Reading sys.relation_stats pages. | **(a)** - The steady path is one atomic load. A sliced walk's yields hold a refcount, not a raw pointer, so the use-after-free class disappears. (c) takes page latches up to 24 times per statement. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q5 | What is the feedback fact and its key? | (a) card(T) = rows_matched of executed slot k, with T the set of written positions of executed slots 0..k; plus build_rows as card({w}) and decline facts; keyed (fetch_id, n, exact oids, mask); folded only from complete, uncut runs.<br>(b) Per-step matched/opens per (written position, predecessor set) (Design 2).<br>(c) Per-step matched/examined per (pattern_id, step_id). | **(a)** - card(T) does not depend on the order or the access kind: rows_matched is counted after the residual on every path (step_vm.cpp:2534). One plan's runs therefore price another plan, and invariant 8 holds. (c)'s examined depends on the kind. (b) learns the same thing with more cells. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q6 | Where does a relation's row prior come from? | (a) Clustered internal pages only (height 1 exact live slots, height 2 exact leaves, Knuth over internal levels above), times LeafRowCapacity(row_size) times a 0.75 fill constant.<br>(b) (a) plus up to 4 sampled leaves' live slots.<br>(c) sys.tables.next_id.<br>(d) Commit-time row deltas on txn::Transaction (Design 3). | **(a), with S0 measuring the error; (b) only if S0 shows the fill constant is off by more than 2x.** - Internal pages are the ones probes keep resident, so a seed rarely faults core 0's reactor (pread, file_page_device.cpp:102). (c) is a sys.tables ScanAll per call (catalog.cpp:1520-1528) and wrong for KWP-named keys. (d) touches every commit path for a speed-only fact. | **(c), revised at CC's close**: the close read a height-3 tree at 0.38x; its review traced it to four random descents averaging in the under-full rightmost subtree (0.52x) more than to the fill constant (0.75x), so the prior descends under each root child (the rightmost apart, the rest sampled) and samples the leaf each reaches. Adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q7 | Steward cadence and seed budget. | (a) 1 s tick; at most 16 page reads per tick; each known relation re-seeded at most every 10 s or when its root page id moved.<br>(b) A 50 ms tick with continuous sampling (Design 1). | **(a)** - (b) is up to about 1,300 page reads per second through the one pool, synchronously on core 0. Plans can move at most once per tick anyway. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q8 | Hysteresis and the switch rule. | (a) Half-octave Schmitt bands (±0.75) on inputs. The plan is the minimum over all orders, adopted only if it beats the WRITTEN cost by 10% when any statistic entered (1e-9 otherwise).<br>(b) An incumbent order with a challenger margin (Design 2).<br>(c) Keep CB's best-so-far 1e-9. | **(a)** - At step_compiler.cpp:2170, best-so-far with any real margin makes the winner depend on next_permutation order. (a) is a pure function of the snapshot, testable PO10-style. (b) puts history into per-core state. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q9 | When may an estimate price a build the cap could decline? | (a) Only when own×4 ≤ join_build_max_rows and no decline fact exists; a decline publishes at once.<br>(b) own ≤ cap, as CB. | **(a)** - A declined build is a per-row walk, about 1e10 model units, while an over-estimate only forgoes a build. The asymmetric margin matches PO3. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q10 | Should ChainCost price ORDER BY and LIMIT? | (a) Yes. A sort costs out·log2(out); an order adding a sort the written order lacked is +inf above sort_max_rows/2; LIMIT without a sort scales the per-row part.<br>(b) No, as CB. | **(a)** - Without it, a data-driven order loses the elision (step_compiler.cpp:2674-2681) and can move a statement onto a sort_max_rows refusal that the planner itself caused. (a) is data-free and lands in S1. | **(a) revised at CC-S1's review**: an added sort is never taken unless LIMIT bounds it within `sort_max_rows` (a top-N cannot refuse); the cap/2 estimate rule is dropped, since no estimate knows a reply's size. Adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q11 | Which identifier bounds, and in which direction? | (a) ≤1 row only when every key column of a unique index (IndexRef nkeys, schema.hpp:478-481) or the tuple key is equality-bound. Transition and FK-parent NDV upper bounds may only raise an equality's selectivity to 1/bound.<br>(b) Leading-column unique and 1/|states| as a lower selectivity (Designs 1 and 3). | **(a)** - UNIQUE(a,b) does not make a unique, and an NDV upper bound gives average selectivity ≥ 1/bound, not ≤. (b) under-estimates in the direction that chooses builds the executor declines. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q12 | Memo key and literal sensitivity. | (a) Exact (fetch_id, n, oids) plus generation, catalog stamp, join_build_max_rows, sort_max_rows and kRange span bands, with ChainCost reading the band centre so the memo is an exact cache; the value is a permutation.<br>(b) Add arg_hash.<br>(c) (fetch_id) and the schema word only. | **(a)** - The span is the only literal the model reads (join_order.cpp:102-107). arg_hash hashes text and would never reuse an entry. (c) is unsound across namespaces and DROP/CREATE, and blind to session limits (command_dispatcher.hpp:353-354). | **(b) revised at CC-S4's review**: the key holds the statement's literals (`arg_hash`) besides the block's key, the generation, the catalog's stamps and the limits; the written chain's kinds and span bands could not stand for literals the written order hides. Adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q13 | What is the off-switch? | (a) `join_order = written` means no search, no statistics, no memo and no fold; `planned` with an empty snapshot is S1's data-free planner.<br>(b) A new planner_statistics key. | **(a)** - CLAUDE.md forbids a second name for a quantity an existing setting expresses. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q14 | What do the contract suites compare once plans read statistics? | (a) Multiset comparison without ORDER BY in all four suites; an ORDER BY on the LIMIT-without-ORDER-BY cell; every configuration run under an empty and under one pinned populated snapshot.<br>(b) Freeze the suites on CB's costs. | **(a)** - The suites build CommandDispatcher with no Expeditor, so without a pinned snapshot they would never test the shipped planner. Inputs are index-, Cabin- and Waystone-blind, so the configuration claims stay true. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q15 | The OptimizerSignals CandidateRef is the first kFilterScan in executed order (command_dispatcher.cpp:7848-7886). Now that the order moves with data, what should it be? | (a) Derive it in written-position order (the lowest step_id), so a reorder never moves the controller's candidacy.<br>(b) Leave it. | **(a)** - It cuts the planner-to-controller feedback loop at its one coupling point for the cost of a different iteration order. The controller's other inputs are unchanged. It needs a line in physical-optimizer.md. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q16 | Order-dependent refusals (SUM overflow, max_rows_touched) can now flip with data. | (a) Record them in aggregate.md AG3 and parser-v2.md as stated behaviour; a wider accumulator checked at Finish is a later aggregate.md milestone.<br>(b) Exclude aggregated blocks from statistics. | **(a)** - Neither is a wrong answer. (b) forfeits the s2 customer-statement, one of the two motivating statements. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q17 `[quiet-wrong]` | Keyset pagination advice without ORDER BY (manual/sql/sql.md:1325-1327). | (a) Require ORDER BY on the paged key, labelled quiet-wrong for joins only.<br>(b) Leave it; sql.md:1264-1267 already disclaims order. | **(a)** - Over a join whose driver flips between pages, the advice as written silently skips or repeats rows. Single relations are never planned, so for them the advice is only unsafe in principle. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q18 | Which spec owns collection and the snapshot? | (a) A new docs/spec/plan-stats.md, cited from parser-v2 I12.<br>(b) A section of physical-optimizer.md.<br>(c) A section of parser-v2.md. | **(a)** - Under R2 (physical-optimizer.md:42) the collection layer is spec'd before the code. Its consumer is the join planner, not the Cabin controller, and parser-v2 is already the planner's contract. | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |
| CC-Q19 | Scope beyond CB's top-level blocks: more than 4 relations, SELECT *, DML, the query layer, subquery blocks. | (a) Greedy above 4 relations reads priors and bounds only, with no posteriors and no memo; everything else stays as CB left it.<br>(b) Widen to SELECT * and the query layer. | **(a)** - No s1-s4 statement has more than 3 relations, and none has a measured gap in the excluded forms. Subquery blocks collide with the decorrelation ban (parser-v2.md:140). | **(a)**, adopted on the operator's standing go-ahead of 2026-10-18 (go-ahead-achieving-milestone) |

## 9. Stages

| Stage | What | Gate |
|---|---|---|
| CC-S0 | Premise census, with no engine code. (1) CC-Q1..Q19 adopted on the operator's standing go-ahead (§8). (2) A census in build-release at 65dc45c5 (v3.2.0-178-g65dc45c5), at most 20 minutes, with a 60 s file-polled watchdog, measuring server CPU through tools/kds_sampler.c in interleaved A/B, written order against the hand-forced alternative under join_order=written:<br>- s1 compare-all at 8 models / 2,872 results (tools/scenario1_backtest.py:1054-1068), r Scan + m Probe against m-first + r build;<br>- s2 customer-statement at 10k cargos / 9k freights, and at defaults with 1 booker (tools/scenario2_freight.py:1135-1137);<br>- CB's search cost on the s1 cross-join, planned against written.<br>(3) The prior's accuracy: the internal-page estimate against sys.relation_stats' walked leaves × live slots for every s1 and s2 relation. (4) One steward seed step's wall time, cold and warm.<br>Results go to bench/v3.2.0/results-cc-s0-premise-<describe>.md. | At least one flip saves at least 15% of server CPU per statement over 3 interleaved pairs. If none does, CC is cut to S1 plus the memo (S4) on CB's data-free costs, and the results file says so. The prior is within 2x on every measured relation, or CC-Q6 is revisited. A seed step takes ≤200 us warm. |
| CC-S1 | Data-free corrections, inside CB-Q2 (a).<br>- (a) The choice rule: minimum over all orders, adopted against the written cost with a margin; m stays 1e-9 here (step_compiler.cpp:2166-2176).<br>- (b) ORDER BY/LIMIT pricing in ChainCost: a sort costs out·log2(out); an added sort above sort_max_rows/2 is +inf; LIMIT scales the per-row part. PlanOptions gains sort_max_rows.<br>- (c) kRange span quantized to its half-octave band.<br>- (d) Identifier bounds: a unique index with every key column equality-bound gives ≤1; Transition NDV upper bounds may only raise selectivity.<br>- (e) Doc and test fixes: the manual's keyset ORDER BY, index_contract_test.cpp:219-220 ORDER BY, the multiset rule in the index, inner-build and waystone suites, the AccessKind × elision enumeration test, and the stale CB lines in parser-v2, crosscore and waystone-concpets. | Every CB join-order test and CB close cell produces an identical chain, except the cells (b)-(d) change, each enumerated in a test that names its reason. The 4 contract suites are green. The full suite (scripts/test.sh) is green. A critics-developer review is done. Overhead is not measured here; it is measured at the milestone's close. |
| CC-S2 | Collection and publication, with no planner consumer. Write docs/spec/plan-stats.md first.<br>- Build stats::PlanStats: per-core fixed parts under kStatsRing latches; the steward on core 0's system tick at 1 s beside src/server/expeditor.cpp:2052-2063 (merge, EMA, seeds through GetForRead with the page type and owner_oid check, a 16-page budget, half-octave Schmitt bands, publish on change); the immutable snapshot with a shared_ptr, a generation and a mutex; the per-core PlanStatsSource pin.<br>- ExecStats gains `stopped`, set from step_vm.cpp:616. PlanRecord lives on the SelectRun.<br>- RecordPlanStats sits beside RecordOptimizerSignals (command_dispatcher.cpp:7660-7667) and folds rows_matched by written mask, build_rows and decline facts.<br>- Also: PublishPlanStatsForTest, the SHOW META plan_stats block, and the sys.relation_stats est_rows, rows_band and generation columns (src/server/sys_views.cpp:615-660).<br>- CC-Q15: CandidateRef is derived in written-position order. | Unit tests:<br>- a counting source shows 0 pins and 0 folds for single-relation and all-descent statements;<br>- a LIMIT-cut run folds nothing;<br>- card(T) is identical under indexes=off, Waystone recording on or off, a Cabin present or absent, and access_statistics off;<br>- a self-join's masks land on written positions under a permuted order;<br>- the prior is within 2x on uniform, skewed and append-grown trees, and exact at height 1 and 2 for leaves;<br>- a reused page discards the seed;<br>- the Schmitt bands do not republish under ±25% churn;<br>- a decline publishes at once;<br>- the snapshot outlives a sliced walk's yields.<br>A sim harness run crashes at an arbitrary op: the statistics are empty after restart and the answers are unchanged. Every chain is byte-identical with the store full or empty. Full suite green, review done. |
| CC-S3 | Statistics into the order (CC-Q1 (a)).<br>- CompileBlock builds PlanPriors once after the descent skip (step_compiler.cpp:2161-2163).<br>- ChainCost reads priors, card(T), card({w}), decline facts and bounds, with the cap/4 build margin and the ≥1 clamps.<br>- kStatsSwitchMargin = 0.10 when any fact entered.<br>- Greedy above 4 relations reads priors only.<br>- ANALYZE prints est=, obs=, gen= and switch=.<br>- Amend IX9 (index.md:459-469), parser-v2 I12/I13 and :204, join-inner-build section 5, cabin.md:160-163 and :210, aggregate AG1/AG3/AG6, the manual at sql.md:1071-1075 and join_order.hpp.<br>- The contract suites run under an empty and a pinned snapshot. | An empty snapshot gives S1's chains byte for byte (the regression gate). Injected s1 and s2 snapshots produce the S0 flips: m-first with a build; c-first at the CA sizes; the written order kept once freights exceed cap/4. The rows=0 posterior test still walks. The self-join replay is checked across two generations. The four suites are green in both snapshot modes, including waystone's five-configuration byte comparison. AG1 is green under a snapshot. Full suite green, review done. |
| CC-S4 | Identifier-keyed order memo, per core: a fixed 1024-entry, 2-way table.<br>- Key: exact fetch_id, n, oids, generation, catalog stamp (cache_built_at_, catalog.cpp:1069-1080), join_build_max_rows, sort_max_rows and span bands.<br>- Value: a permutation.<br>- A hit compiles the written order and then the remembered one through compile_order. A refused remembered order invalidates the entry.<br>- The memo is bypassed under a minted view (command_dispatcher.hpp:1166-1201).<br>- ANALYZE prints order=memo. | Tests:<br>- 2 compiles on a hit for 3 and 4 relations, 1 when the remembered order is the written one;<br>- the written order's refusal surfaces on a hit;<br>- USE ns, and DROP plus re-CREATE under one fetch_id, never reuse an entry;<br>- CREATE INDEX and a generation move re-search;<br>- a memo hit equals a cleared-memo search under the same inputs (purity).<br>Full suite green, review done. |
| CC-close | Milestone close in build-release, about 1 hour including the build, with a 60 s file-polled watchdog and an interleaved A/B of 65dc45c5 against the closing commit. Cells:<br>- point read and tuple-key point join: server CPU per statement, p50 and p99, on core 0 and on another core;<br>- single-row INSERT and UPDATE;<br>- s1 compare-all and s2 customer-statement at the CA size: the cold first execution reported separately from steady state;<br>- s1 cross-join: memo hit against CB's search;<br>- pool hit and miss deltas and the steward's share of core 0, from SHOW META plan_stats;<br>- a 60 s static run counting generations and distinct orders per fetch_id;<br>- restart: time to the first publish and to the warm plan.<br>Also: the CLAUDE.md row flip, the known-gaps entries, and the results file bench/v3.2.0/results-cc-close-<describe>.md, which states what was cut. | The point read, tuple-key point join, INSERT and UPDATE are within noise in mean and p99. The steward is ≤1% of core 0. The S0 flips are realised, or the shortfall is stated. Steady state shows one order per fetch_id. The full suite is green on the closing tree, with its pass count stated. The chain stops before the merge to main for the operator's word. |

## 10. Measurement

Every number names its `git describe --tags`. The base is 65dc45c5 = v3.2.0-178-g65dc45c5. Every number comes from build-release, never ./build. Server CPU is taken through tools/kds_sampler.c and kds_sampler_report.py, because perf is refused on this host. Every run has a 60 s file-polled watchdog, and an orphaned run is resumed at once. If recovery_checkpoint_us flags a device stall, the pair is repeated, not the cell.

CC-S0, ≤20 min including setup, results in bench/v3.2.0/results-cc-s0-premise-<describe>.md:
- the premise flips: s1 compare-all, and s2 customer-statement at the CA size and at the 1-booker default, written against forced, 3 interleaved pairs;
- CB's search cost on the s1 cross-join;
- the prior's error against walked counts for the s1 and s2 relations;
- one seed step, cold and warm.
If the budget forces it, the default s2 size is cut first, and the file says so.

PER STAGE: the full suite, and S1/S3's chain-identity gates. Each stage lands with 'overhead not measured; measured at the milestone's close', never as an implied pass.

CC-close, about 1 h including the build, interleaved A/B of 65dc45c5 against the closing commit:
- OVERHEAD:
  - point read and tuple-key point join, server CPU per statement plus p50 and p99, measured with sessions on core 0 and off it, because the steward's synchronous faults land on core 0;
  - single-row INSERT and UPDATE;
  - the bar is bn-census session noise (2-3%).
- POOL: SHOW META pool hits, misses and resident before and after a steward-active run.
- STEWARD: the plan_stats block deltas (seeds, seed_pages, steward_us_max, publishes, part_drops) and its share of core 0. The bar is ≤1%.
- GAIN:
  - s1 compare-all and s2 at the CA size, cold first execution (data-free) reported apart from steady state;
  - s1 cross-join, memo hit against CB's search (CB's 15 us baseline, step_compiler.cpp:2155-2157).
- STABILITY: a 60 s static mixed run, counting generation bumps and distinct orders per fetch_id after convergence. Expected: 0-1 bumps and 1 order.
- RESTART: mount to first publish, and to the warm plan.

The close results go to bench/v3.2.0/results-cc-close-<describe>.md. It states the full suite's pass count and the commit it ran at, says what was cut, and reports an unrun cell as 'not executed', never as a pass.

## 11. Stage log

- **CC-S0** measured at `5745d82e` on `65dc45c5`
  (`bench/v3.2.0/results-cc-s0-premise-v3.2.0-178-g65dc45c5.md`): the
  other join order saves 21.6-60.7 % of server CPU on seven of eight cells
  (s1 compare-all at 200 to 10,000 results, s2 customer-statement at 200 to
  10,000 cargos); at s2's driver defaults (200,000 cargos) 0.7 %, a cell
  real sizes tell apart from the 10,000-cargo one and the constant prior
  cannot. CB's search costs about 21 us on the 3-relation s1 backtest read
  (+1.7 % at 60,480 rows, +32 % at 200). The prior's formula is 0.75-0.98x
  on 30 relations of height 2-3 and 4-6.8x off on small height-1 ones, so
  the exact root slot count at height 1 is required (§1). The seed step's
  time is not executed (no steward at that commit). Gate met.
- **CC-S1**: the data-free corrections. The stage review (three lenses,
  every finding put to two skeptics; 20 survived, 1 refuted) found the
  added-sort guard resting on an estimate that cannot know a reply's size:
  **CC-Q10 revised** - an order that adds a sort the written order elides is
  never taken unless LIMIT bounds it to a top-N heap within `sort_max_rows`
  (a bounded sort is priced at `out log2(limit+offset)`); adopted on the
  standing go-ahead. Also applied: the Transition bound for a join key too,
  `exec::StateCount` replacing a re-implemented state set, `PlanOptions`
  moved beside the cost model and carried whole, the margin seeded into the
  minimum, `EmitsTupleKeyOrder` named with no `default` and an enumeration
  test, the end-to-end elision test on data whose arrival order disagrees,
  the shared `tests/unordered_reply.hpp` in the Cabin and index suites, a
  planned-order cap test, the manual's keyset advice (a key unique in the
  reply, `[quiet-wrong]` label), and every stale `ChainCost` / written-order
  line in parser-v2, plan_printer.hpp, ast.hpp and step_compiler.hpp.
  **Rejected**: the multiset rule in the Waystone suite - its five
  configurations do not change a plan input, so its byte comparison stays
  the stronger test; dropping the inert join half of the unique bound - kept
  with a comment, S3 makes it live.
- **CC-S2**: collection and publication, no plan consumer
  (`docs/spec/plan-stats.md`, `stats::PlanStats`). The review (three
  lenses, two skeptics a finding; 32 survived) found three bugs, all fixed:
  **the prior read held a parent page while asking for its child** - a
  latch cycle against a split, which holds bottom-up; it now holds one page
  at a time and re-reads the root per descent; **the EMA ran once per core
  part**, so a posterior moved faster the more cores ran it - parts are now
  summed first and folded once; **`ExecStats::stopped` was not reset** on a
  reused scratch. Also applied: parts copied out under their latch and
  folded after it, eviction once a step by `nth_element`, CC-Q15's
  candidate by two minimum trackers with no allocation, `build_rows` no
  longer folded as `card({w})` (it leaves out NULL keys), every searched
  block's relations wanted for priors (`StepChain::planned_relations`, so a
  block of more than 4 relations seeds too), the Schmitt step simplified and
  tested directly, `PublishForTest` and `Publish` sharing `Install`, dead
  state deleted, one constant for the 4-relation bound, the spec's PS4/PS5
  labels, PS9 and PS10 restated, and physical-optimizer.md PO7b for CC-Q15.
  **Deviations, recorded**: `StepChain` carries the planned key and the
  searched relations (§3's plan put a `PlanRecord` on the run) - every
  success path already holds the chain, and `limit`/`aggregate` set the
  precedent of dispatcher-only fields; re-seeding is time-based only (every
  10 s), the root-moved trigger was cut; `rows_band` and a publish-age field
  were not added (`est_rows` is the band's centre). **Rejected**: a
  multiset rule for the inner-build suite - its fixture compiles written
  chains, so the planner never reaches it (the planned cap check is
  `JoinPlannerTest.EveryBuildCapAnswersTheSameRowsPlanned`); removing the
  minimum step id in the fold - one pass over at most four steps keeps the
  fold independent of where a block's ids start. **Cut, with reason**: the
  Waystone-recording and Cabin invariance cells (PS10 holds because
  `rows_matched` is counted after every serving path; the index cell pins
  it), a sim crash cell (the store is memory-resident by construction and
  no plan reads it yet), a height-3 prior test (needs ~130,000 rows; the
  close measures `est_rows` against true counts on the scenarios' trees).
  Overhead not measured; measured at the milestone's close.
- **CC-S3**: statistics into the order. A relation's prior replaces the
  constant rows, a block's `card(T)` replaces the product of selectivities
  for a measured prefix, `card({w})` floors a relation's rows and prices
  its build, a decline fact prices that build as the walk, and an order must
  beat the written one by 10 % once a fact prices the block. The compile
  pins the snapshot only after the tuple key chain's skip. ANALYZE prints
  `planned order= gen= switch= est=`. The review (55 agents; 24 survived)
  found four bugs, all fixed: **a non-key join's selectivity was 1/rows of
  the inner relation**, so a join's estimated size depended on its order -
  now 1/rows of the earlier side's relation where its column is that
  relation's tuple key, else 1/max of the two, symmetric with the key join;
  **the build's quarter-cap margin turned on any published snapshot** - it
  and the switch margin now turn on one `informed` bit, a fact about this
  block; **ANALYZE cast an unbounded double to an integer**; **a key join's
  matches could exceed a row** once `card({w})` raised `own` above the
  prior. Also applied: priors resolved once a compile (`written_rows`), the
  estimate's step rows filled only for the kept chain, the span band read
  through `stats::BandOf`, decline facts lapsing after 60 s (PS3), estimates
  labelled by step id, one shared `tests/learn_plan_stats.hpp`, and the
  stale `f(shape, catalog)` lines in aggregate.md, index.md §12.3,
  join-inner-build.md §3, step_compiler.cpp/.hpp and the manual. Tests
  added: the cap/4 boundary, a snapshot ignorant of the block changing
  nothing, a join's size equal from both sides, the clamped zero
  posterior read, a self-join under two generations with the same rows,
  AG1 under shared priors, a five-relation greedy block, and learned-
  statistics cells in the index, Cabin and Waystone suites that assert a
  plan read the statistics. **Rejected**: an inner-build suite cell - its
  fixture compiles written chains, so the planner never reaches it.
  **Cut, with reason**: the s2 c-first flip and the 10 % margin's own cell
  (the close measures the scenario shapes; the margin is one comparison).
  Overhead not measured; measured at the milestone's close.
- **CC-S4**: the order memo (`exec::PlanMemo`, plan-stats.md PS6): per
  core, 512 sets of two ways, keyed by the block's key, the generation, the
  catalog's two stamps, the statement's limits and the written chain's step
  kinds and span bands; a hit compiles the written order and the
  remembered one; not read under a minted view. The review (41 agents; 19
  survived) found **the key inexact**: it held the written chain's kinds and
  span bands, but the search costs every order, and a relation the written
  order probes shows its literal range, its tuple key literal's validity or
  a LIMIT only in another order - two literals of one fetch id shared a key
  and the first served the second. **CC-Q12 revised to (b)**: the key holds
  the statement's literals (`arg_hash`), which covers every literal the cost
  reads, LIMIT and OFFSET included; a statement with a `?` bound later is
  not memoized; the kinds and span bands left the key. The price, stated in
  PS6: a statement whose literals vary between runs misses every time.
  Adopted on the operator's standing go-ahead of 2026-10-18. Also applied:
  `Forget` deleted (a remembered order that no longer compiles is searched
  again and replaced), the entry's `used` flag folded into `used_at`, every
  key field hashed into the set, the counters printed as the answering
  core's in a `plan_memo` block outside the store's gate, PS6 and the
  `order=memo` grammar written down. Tests: a hit equals the search, a
  generation or a DDL searches again, a recreated relation and another
  namespace's relation reach new keys, two literals on the probed relation
  and two LIMITs are two keys while a repeated literal hits, a
  three-relation hit equals a cleared memo's search, the dispatcher's memo
  serves a repeated statement. **Cut, with reason**: a compile counter for
  the "two compiles a hit" claim (the hit path calls `compile_order` once
  by construction; the close measures the time), and a refused remembered
  order (no schema change that keeps every stamp can refuse an order).
- **CC-close**, measured at `c547ef14` against `65dc45c5`
  (`bench/v3.2.0/results-cc-close-v3.2.0-184-gc547ef14.md`, commit
  `b78799fa`): **overhead within the floor** - pooled over 36 paired rounds
  (20 for the writes) the point read +0.81 %, the tuple-key point join
  +0.43 %, a single-row INSERT -0.10 % and UPDATE +0.50 %, against a control
  server of A's binary at +0.86 / +0.82 / +0.65 / -0.09 %. **The S0 flips
  realised** on six of eight cells: compare-all -33 to -40 % CPU at 1,000 to
  10,000 results (-13 % at 200, short of 15 %: the planned path's 19-25 us
  is 17 % of a 144 us statement), customer-statement -29 to -62 % at 200 to
  10,000 cargos; at 200,000 cargos the planner flips on an estimate the
  walk does not realise (+7 to +10 % on the harness's servers, -4.6 to +4.1
  % on clean ones; known-gaps). **The memo** saves 15-17 % on a repeated
  3-relation cross-join at 200-1,000 rows, 5 % at 10,000, nothing at
  60,480; a unique literal per run costs +1-3 % over A's search. **The
  steward**'s longest step was 54-69 us, 17 publishes, no part drop, every
  seed page resident. **Stable**: no generation move and one order per
  statement over 60 s. **The prior** was 0.38x on one height-3 tree, past
  S0's 2x gate, so CC-Q6 was revisited: the stratified prior (CC-Q6 (c),
  `1a9168d3`, reviewed, suite 4126/4126) re-measured at `1a9168d3`
  (`bench/v3.2.0/results-cc-close-prior-v3.2.0-186-g1a9168d3.md`, commit
  `17ab91cf`): 0.72-1.16x on all 28 relations (1.08 on that tree), longest
  step 105 us, 8-20 extra pool misses from cold leaves, and the four gain
  cells' flips unchanged (B against the c547ef14 binary inside the 2 %
  floor). **Cut**: sessions off core 0 (`cores = 1`), `durability =
  strict`, the PostgreSQL floor, the overhead cells at 200 and 1,000 rows,
  and in the re-measure every cell but the prior, the steward and four gain
  cells. The re-measure did not reproduce the close's -44.7 % clean-server
  figure at 10,000 cargos (-28.8 % on both binaries); its cause stays
  undiagnosed, as the known-gaps entry says.
