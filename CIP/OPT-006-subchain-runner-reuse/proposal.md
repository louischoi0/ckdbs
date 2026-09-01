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

**Implementation.** Branch `opt-006-subchain-runner-reuse` — filled in
with its remote link, commits and suite result when the work lands.
