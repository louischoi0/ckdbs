<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-006 — A correlated sub-chain rebuilds its whole runner per outer row

**Hypothesis.** `EvaluateSubChain` (`src/exec/step_vm.cpp:313`) runs
once per accepted outer row (`:2312`) and per call constructs a
`RowSink` `std::function` capturing 7+ locals (`:344`, past the inline
buffer, so one malloc/free), a whole fresh `ChainRunner` (`:420`) with
its `bound_`, `schemas_`, `frame_`, `spills_`, `seen_pks_` and
scratch buffers, a `Bind()` + `frame_.Open()` (`:288`-`:289`, three more
allocations plus N `AstValue` constructions), and a second capture-heavy
`std::function` at `:1783`. That is ~6 malloc/free pairs per outer row
paid **before a single inner row is read**. Invisible when the sub-chain
walks 10,000 inner rows; **dominant once JB4/JB6's build turns the walk
into a bucket probe**, which is exactly the shape
`docs/spec/join-inner-build.md` optimises — and `InnerBuildStore` was
already lifted to statement scope for this reason (`:236-248`), while
the runner around it was not.

**Measurement.** `tools/benchmark.py --join-ops` / `--join-scan-ops`
(`join-probe`, `join-semi`) and `tools/join_benchmark.py` /
`tools/join_ksweep.py` at a large `--join-rows`. `ANALYZE` must show
identical `sub_chain_runs`, `build_probes` and `examined=` — the win is
invisible to every counter, so the interleaved A/B is the only
instrument.

**Reason.** The build made the inner walk cheap; what is left around it
is now the cost.

**Pros / cons.** Pro: the win grows exactly where the engine's own join
work has been heading. Con — stated plainly because it is the reason
this entry is last: a cached runner with an explicit reset is **less
simple** than a fresh one, and `record_through_stops_`, `stopped_`,
`recording_`, `building_` and `depth_` must each be reset per outer row
or a stale flag silently changes what a walk records (the C1 break at
`WalkAndRecord:794-829`). This is the highest-risk entry on the list,
and it trades simplicity for speed.

**Consistency and sanity.** No numbered hard invariant, but it sits
against invariants 8 and 9: what a walk *records* into a Waystone trail
must not change, and a stale `recording_` flag is precisely how it
would. The parent frame `&outer` is one object for the whole outer step,
so caching itself is sound. Proof: the waystone contract suite
byte-for-byte, the join tests, and `ANALYZE`'s counters unchanged.

**Implementation.** Branch `opt-006-subchain-runner-reuse` at `ff27662`,
reworked by the review at `1495016` (`v2.7.0-47-g1495016`). The sink is a
member function reading a member accumulator, not a lambda capturing
seven locals; the runner is built **on the stack the first time and
cached only from the second evaluation** of the same sub-chain. Release
suite **3091/3091**, sim **228/0**.

**Measured** — `results-opt006-subchain-runner-reuse-v2.7.0-47-g1495016.md`,
beside this file. Two pairs (`c578e29` vs `1495016`, the change as it
stands; `ff27662` vs `1495016`, pricing the review's regression fix),
five shapes, three outer-row counts.

| Shape | 200 | 1,000 | 10,000 |
|---|---|---|---|
| `exists` (correlated) | +4.5% | +5.5% | **+8.9%** |
| `in_`, `scalar` (correlated) | 4-9% across sizes | | |
| `control_hoisted` (runs once) | flat | flat | flat |
| `control_update` vs pre-OPT-006 | flat | flat | flat |

**The win scales with outer-row count**, which is what the mechanism
requires — the outer-row count is exactly how many runners used to be
built — and it clears a 0.2-1.3% floor at most sizes. The hoisted
control, the one form genuinely evaluated once, stays flat in both
pairs.

**The regression the review predicted is priced, not just fixed.**
`control_update` (`UPDATE … WHERE x IN (SELECT …)`) shows a reproducible
**4-6% regression on `ff27662`** against `1495016`, at every size, above
its floor — `EvaluateConjuncts` builds a fresh runner per *scanned row*
there, so the first form's unconditional cache could never hit and only
added allocations, on the path OPT-001 and OPT-003 had just sharpened.
Against the pre-OPT-006 baseline the final form is flat. **The measured
cost of shipping the obvious version of this change was a regression on
the write path**, and it was caught by review rather than by the suite,
which stayed green through it.

**One documented expectation that did not fire, reported as such:** the
V19 probe memo now survives across outer rows on a cached runner, so
`probe_memo_hits`/`trail_replays` were expected to diverge. They stayed
empty on every arm — these shapes never exercise it. `sub_chain_runs`
and `examined=` matched exactly across all 32 comparisons.

Correctness: 60 hash comparisons (full tables plus each correlated
query's own result set), zero mismatches, zero errors.
