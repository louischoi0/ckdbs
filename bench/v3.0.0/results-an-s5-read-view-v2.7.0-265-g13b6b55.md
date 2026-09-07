# AN-S5 — the instance read view's two costs, in-process

`instructions/v3.0.0/workorder-an-read-view.md` AN-S5 asks for the number
beside the AN-8 build: the mint's cost as ns per statement — expected
*smaller* than the trx-id predicate it replaced, since a mint is now one
atomic load and a bounded slot walk where it used to fill a 64-entry
in-flight array — and against it the window lookup's cost per tuple at 0,
8 and 64 live transactions per core, which is where the commit-LSN view
spends what the mint saved. This document is that half of AN-S5; the
scenario half (point-lookup/8-statement TPS cells against AL-S8) is
`results-an-s5-scenario0-v2.7.0-265-g13b6b55.md` and
`results-an-s5-scenario2-v2.7.0-265-g13b6b55.md` — the latter two could
not be measured this run for a reason unrelated to AN-S2/AN-S3's own
code, stated there.

## 1. What `kds_read_view_bench` measures

Read from `bench/read_view_bench.cpp`'s own header (§ comments) at commit
`451022b`. One `kds::txn::TransactionManager` over its own
`InstanceVisibility`, unlogged (`wal=nullptr`), single-threaded, in an
`InMemoryPageStore` — the shape every socket-free fixture runs. This
isolates the predicate's own arithmetic from the page span, the decode
and the socket a scenario cell pays around it; it says nothing about
contention on the window latch, which needs two reactors and belongs to
the two-core rig (AV's — not run here). Two calls are timed:

- **`MintReadView`** — the mint, as an autocommit statement takes it: one
  atomic load of the commit-LSN ceiling, capped by `slots_in_use_`, plus
  the Cabin's one-bit `live_` walk (AN-8 §8.1, §8.2 item 9).
- **`ReadView::Visible`**, over its four branches, each priced separately
  because a workload's statement mix decides which one it pays:

  | branch | condition | cost |
  |---|---|---|
  | bootstrap | `t == kAlwaysVisibleTrxId` | one comparison |
  | below the floor | `t < Floor()` | one atomic load |
  | window hit | committed, above the floor | one latch + one hash lookup |
  | window miss | live, above the floor | one latch + one hash miss |

The live count (0, 8, 64) moves two things: the mint's `live_` walk and
the window's size. Every cell is 64 committed transactions held in the
window (an anchor transaction stays open below every commit so the floor
cannot retire them), 2,000,000 calls per run, 5 runs, median and minimum
ns/call reported — the min-vs-median spread is this box's own noise floor
for a loop this short. A `Launder()` (`asm volatile` round trip) hides
each argument from the optimiser: the first, unlaundered run of this
harness printed 0.0 ns for the two constant-comparison arms because the
loop folded away — a bench artifact, not an engine number — and the run
below is the corrected one.

## 2. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-07, 11:29:14 UTC (`precheck-microbench.txt`'s own timestamp) |
| Worktree | `an-s3-snapshot-adoption` (branch `an-s3-snapshot-adoption`) |
| Engine commit measured | `13b6b55`, `git describe --tags` = `v2.7.0-265-g13b6b55` — the merge that landed AN-S3 on main. **Verified this session**: `git diff --stat 13b6b55 HEAD -- src include tools CMakeLists.txt` is empty, so every line of engine code this bench calls (`kds::txn::TransactionManager`, `InstanceVisibility`, `ReadView`) is byte-identical between `13b6b55` and this worktree's `HEAD` (`451022b`) |
| Bench-harness commit | `451022b` (`git describe --tags` = `v2.7.0-267-g451022b`) — the two commits after `13b6b55` touch only `bench/read_view_bench.cpp`: `77a78a6` (a namespace-qualifier fix the Release build needed) and `451022b` (the `Launder()` argument fix so the constant arms are priced, not folded). Neither touches engine code, so this bench's numbers are a measurement of the `13b6b55` engine through a corrected harness |
| Tree cleanliness | Clean (`git status` at the top of this session) |
| Binary | `build-release/kds_read_view_bench`, mtime `2026-09-07 11:29:14.17 UTC`, `sha256 3cb0c9d649c01d7f108e3592d2e668dc575f7534deb17171a28c03e1c6dbabb0`. Run directly from `build-release/`, not copied: this is a single in-process harness built once and run immediately after (`cmake --build` then the run, one shell block, no server, no matrix of configurations a concurrent rebuild could swap underneath) — the copy-the-binary rule (ck-tester rule 5) exists to protect a run that starts a fresh **server** per cell from another session's `cmake --build` landing mid-matrix; this run starts no server and reads no data file at all |
| Device | n/a — `InMemoryPageStore`, no data file, nothing durable. The block-device rule does not apply to this half of AN-S5, as it does not apply to any in-process fixture-shaped bench (AN-8's own build ran the equivalent code path unlogged for the same reason) |
| Build type | `build-release` (`CMAKE_BUILD_TYPE=Release`), not rebuilt this session |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core (SMT) — same host AL-S8 measured on. Single-threaded harness, so the SMT/physical-core distinction does not bear on these numbers the way it does on a `cores = 8` server cell |
| Precheck | `precheck-microbench.txt`: `/proc/loadavg` 1.41 / 3.08 / 3.16 at 11:29:14 UTC; `pgrep -a -f 'cc1plus\|cmake --build\|ctest'` matched only the invoking shell's own `cmake --build ... --target kds_read_view_bench` command line — a false positive of the pattern against its own argv, not a concurrent build (the same shape AL-S8's `precheck.sh` comment documents; this run's own `cmake --build` had already finished before the timed run started, since the build and the timed run are sequential in one `&&`-chain) |

## 3. The numbers

`build-release/kds_read_view_bench --iterations 2000000 --repeats 5`, from
`read_view_bench.txt`. Not a matrix (rule 5a) — a single-connection,
in-process loop with no throughput axis to derive QPS/TPS from; ns/call is
the unit the harness reports and the unit this table states, per rule 5a's
own exception for a shape with no meaningful throughput form.

| Cell | live | median (ns) | min (ns) |
|---|---|---|---|
| mint (`MintReadView`) | 0 | 6.3 | 6.3 |
| mint (`MintReadView`) | 8 | 6.6 | 6.5 |
| mint (`MintReadView`) | 64 | 6.6 | 6.5 |
| `Visible`: bootstrap id | 0 | 0.5 | 0.5 |
| `Visible`: bootstrap id | 8 | 0.5 | 0.5 |
| `Visible`: bootstrap id | 64 | 0.6 | 0.5 |
| `Visible`: below the floor | 0 | 0.5 | 0.5 |
| `Visible`: below the floor | 8 | 0.5 | 0.5 |
| `Visible`: below the floor | 64 | 0.6 | 0.6 |
| `Visible`: window hit (committed) | 0 | 8.5 | 8.2 |
| `Visible`: window hit (committed) | 8 | 8.2 | 8.1 |
| `Visible`: window hit (committed) | 64 | 8.3 | 8.1 |
| `Visible`: window miss (live) | 0 | 8.3 | 8.2 |
| `Visible`: window miss (live) | 8 | 8.0 | 7.9 |
| `Visible`: window miss (live) | 64 | 8.2 | 8.2 |
| `Visible`: hit/miss alternating | 0 | 8.7 | 8.5 |
| `Visible`: hit/miss alternating | 8 | 8.4 | 8.3 |
| `Visible`: hit/miss alternating | 64 | 8.6 | 8.5 |

Rule 9 (sweep the row-set size) reads onto this table as the `live` column
rather than a row count: this bench's cardinality axis is the number of
live transactions held open, not a row count, because both the quantities
priced here (a mint, a per-tuple visibility check) are defined per
statement/per tuple and touch no relation at all — there is no "200 /
1K / 10K rows" for a predicate that never reads a page. What the table
does show across that axis is the one thing rule 9 exists to catch: a
fixed cost from a per-entry one, and every row here reads as fixed (§4).

Noise floor for this table: min vs. median across every cell is 0.0–0.3 ns
(worst case the mint's 6.3→6.6 ns 0-vs-8/64-live step, ≈5%, and the
`Visible` arms' 0.0–0.2 ns min-median gaps). Nothing below is read as a
finding at a resolution finer than that.

## 4. What this run says about the engine

**The mint is now a flat ~6.5 ns and does not scale with the live
count.** 0 live transactions costs 6.3 ns median; 8 costs 6.6 ns; 64 costs
6.6 ns — a ~5% step from 0 to 8 and then nothing further to 64, which
reads as a one-time branch-prediction/cache effect of the first non-empty
`live_` walk rather than a per-entry cost: if the Cabin bit's walk were
O(live), 64 live transactions would cost noticeably more than 8, and it
does not. AN-0's proposal that the mark would make the mint *cheaper*
than the trx-id predicate it replaced (a 64-entry array fill) is borne out
directionally by this number, though this run has no `013b6b55`-adjacent
predecessor to diff against under rule 4 — AN-8's own commentary
(§8.2 item 9) already frames this as "the mint's cost as it should have
been counted," not a delta, and that is how this table reads it too.

**A writer above the floor costs one latched lookup of ~8 ns, whether the
window holds it (a hit) or not (a miss) — the window's own size does not
move it.** Window hit and window miss sit within 0.2–0.4 ns of each other
at every live count (8.5/8.3 ns at 0 live, 8.2/8.0 ns at 8, 8.3/8.2 ns at
64), and neither moves outsize the noise floor as `live` goes 0→8→64.
That is the shape a `std::unordered_map`-style hash lookup under one latch
predicts: the cost is the latch acquisition plus one hash computation and
bucket probe, not a walk whose length tracks how many entries are in the
window. **This is the cost the mark spends what the mint saved** — AN-8
§8.4 names it as "not measured here" at build time; it is ~8 ns/tuple for
any writer the floor cannot already answer, against ~0.5 ns for the two
branches (`bootstrap`, `below the floor`) that never touch the window at
all. A statement whose every read lands on rows below the horizon or
carrying the always-visible id pays the mint once and nothing per tuple;
a statement reading recently-written rows pays ~8 ns per row on top of
that, latch included.

**Alternating hit/miss costs marginally more than either alone** (8.7/8.4/
8.6 ns vs. 8.5/8.2/8.3 hit and 8.3/8.0/8.2 miss) — small, at or just above
this table's own noise floor, and the direction (branch-mispredict cost of
alternating between the hash-hit and hash-miss code paths inside the same
lookup) is the expected one rather than a surprise.

## 5. What this does not measure

**Window-latch contention across reactors.** Every number above is one
thread against one manager; AN-R1's lock-free argument and the mark's
whole cost model for a busy multi-core instance turn on what happens when
eight reactors contend for the same window latch simultaneously, which
this harness cannot produce (it opens no second `TransactionManager`, no
second thread). That is explicitly AV's instrument
(`instructions/v3.0.0/workorder-an-read-view.md`'s own framing, §1: "which
needs two reactors and is the two-core rig's"), not this one's. Nor does
it measure the mint's cost under a **reclamation pass running
concurrently** (AN-R14, AN-8 §8.5's C1/C2/C3 findings) — the floor and
ceiling here are both static for the life of a `Rig`. The scenario-level
`cores = 8` cells this document's sibling files were meant to carry (§1)
would have been the first real evidence of window-latch contention under
load; they did not run this session (see those documents).

Archive: `precheck-microbench.txt`, `read_view_bench.txt`, this table's
source, is at `bench/v3.0.0/archive/an-s5-v2.7.0-265-g13b6b55/`.
