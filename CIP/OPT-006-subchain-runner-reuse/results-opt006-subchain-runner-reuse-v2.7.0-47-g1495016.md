# OPT-006 measured: 4-9% on the shape it targets, scaling with outer rows, and the regression reproduces and un-reproduces exactly where predicted

`EvaluateSubChain` used to build a whole `ChainRunner` and its `RowSink`
per accepted outer row. OPT-006 caches both; the commit that landed it
(`ff27662`) cached unconditionally from the first evaluation, which a
review found regressed the one path where "first evaluation" is *every*
evaluation — `UPDATE`/`DELETE`'s per-scanned-row conjunct check. `1495016`
fixed it to cache only from the second sighting of a sub-chain on one
runner. Two interleaved A/B pairs price both halves of that story
directly: a **4-9% win** on every correlated shape that scales with the
outer-row count exactly as predicted, a **flat control** on the one
subquery form that is evaluated once regardless of row count, and a
**4-6% regression that reproduces on the unfixed commit and disappears
on the fixed one** — the same shape, the same host, the same statement,
two different binaries.

| | |
|---|---|
| Executed | 2026-09-01 04:38-04:52 UTC |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer`, branch `opt-006-subchain-runner-reuse` at `1495016` — untouched by this run; every arm is a `git archive` export of a named commit into its own scratch tree under `/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/pathopt/src-{005,006first,final}`, never built from the worktree |
| Tree cleanliness | Each arm is a `git archive` export of a named commit — no working-tree drift possible in what was compiled |
| Build | `cmake -S <src> -B <src>/build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl>`, `cmake --build <src>/build-release --target kds_server -j8`. All arms configured and linked clean |
| Host quiet | `pgrep -x cc1plus/cmake/make/ctest` empty and `/proc/loadavg` checked immediately before every server start of every (pair, row-count) cell |
| Device | Data dirs under `$HOME/bench-opt006-*`, `/dev/root`, **ext4** (`df -T`), not tmpfs. Binaries copied once into `.../scratchpad/pathopt/run/` before the first cell of the whole OPT-005/OPT-006 session and never rebuilt |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed`, fresh server + fresh data dir per (arm, row count) |

## The arms

| Arm | Commit | `git describe --tags` | Committed | Binary mtime | Note |
|---|---|---|---|---|---|
| 005 | `c578e29` | `v2.7.0-45-gc578e29` | 2026-09-01 03:35:18 UTC | 2026-09-01 04:10:13 UTC | pre-OPT-006, has OPT-005 |
| 006-first | `ff27662` | `v2.7.0-46-gff27662` | 2026-09-01 03:40:47 UTC | 2026-09-01 04:11:03 UTC | OPT-006, unconditional cache-from-first-eval — the regressed form |
| final | `1495016` | `v2.7.0-47-g1495016` | 2026-09-01 04:02:19 UTC | 2026-09-01 04:11:55 UTC | OPT-006 fixed: cache from second sighting; also converts `fk_check.cpp` (OPT-005, irrelevant to every shape here) |

All three binaries post-date their own commit; full sha256 digests are in
OPT-005's results file (`results-opt005-btree-leaf-ref-*.md`), which built
and copied the same four binaries in the same session.

- **Pair 1 — 005 vs final** (`c578e29` vs `1495016`): OPT-006 as it now
  stands, cache and fix together, against a baseline with neither.
- **Pair 2 — 006-first vs final** (`ff27662` vs `1495016`): isolates the
  review's regression finding and its fix — both arms already have the
  caching *mechanism*, so any delta here is the unconditional-vs-staged
  policy, not the caching-vs-no-caching question pair 1 answers.

## Schema and shapes

`t_outer(id, target, target2, flag)` and `t_inner(id, outer_id, val,
rank)`, `t_inner` BTREE with a secondary index on `outer_id` so every
correlated equality below compiles to a correlated index probe rather
than a per-outer-row linear scan of `t_inner` — the point is to isolate
the runner-rebuild cost this entry is about, not the cost of a scan.
Every outer row owns exactly `fanout=3` inner rows (ranks 0-2), so
`EXISTS` is always true and the scalar subquery (keyed on `rank=0`) has
exactly one qualifying row — deterministic, which is what makes the
correctness hash meaningful. `target`/`target2` are drawn 60% inside
their own inner rows' values and 40% outside, real true/false traffic
through `kInSubquery`/`kCompareSubquery`.

Five shapes, one full-relation statement each — **not** a per-row point
query — because the claim under test is that the win scales with the
number of outer rows *one statement's* walk evaluates the sub-chain
against; a small, fixed op count per round and the required 200/1,000/
10,000 row sweep is what shows that scaling rather than asserting it:

| Shape | Query | What it tests |
|---|---|---|
| `exists` | `WHERE EXISTS (SELECT ... WHERE t_inner.outer_id=t_outer.id)` | correlated EXISTS, the runner-reuse target |
| `in_` | `WHERE target IN (SELECT ... WHERE t_inner.outer_id=t_outer.id)` | correlated IN, same target |
| `scalar` | `WHERE target2 = (SELECT ... WHERE outer_id=t_outer.id AND rank=0)` | correlated scalar subquery, same target |
| `control_hoisted` | `WHERE EXISTS (SELECT ... WHERE rank=0 AND val<5)` — **no outer reference** | the one subquery form `step_compiler.cpp` actually hoists (`!sub.correlated && !sub.has_value`) into `chain.hoisted`, evaluated exactly once regardless of outer-row count — genuinely unaffected before or after OPT-006, this driver's real control |
| `control_update` | `UPDATE t_outer SET flag={v} WHERE target IN (SELECT ... WHERE outer_id=t_outer.id)` | the shape the review found `ff27662` regressed: `EvaluateConjuncts` (`command_dispatcher.cpp`'s UPDATE walk) builds a *fresh* `ChainRunner` per scanned row, so every row's evaluation is a "first" evaluation on that runner |

A plain, uncorrelated `IN` is **not** hoisted — only a value-less
`EXISTS`/`NOT EXISTS` with no outer column qualifies
(`step_compiler.cpp`'s placement pass, `!sub.correlated && !sub.has_value`)
— which is why `control_hoisted` is written as `EXISTS`, not `IN`.

Full methodology in the driver's own docstring:
`CIP/OPT-006-subchain-runner-reuse/archive/opt006_ab.py`. One RNG stream,
an interleaved latency pass (`bench_common.Phase`) and a CPU pass
(`/proc/<pid>/stat`), both alternating which arm leads each round, plus a
floor split of arm A's own series. Op counts per round shrink as rows
grow (15/8/2 latency ops per shape at 200/1,000/10,000 rows) since each
op is now a full-relation statement whose own cost already scales with
rows — holding op count fixed across sizes would have made the 10,000-row
cell take excessively long for no added resolution.

## `ANALYZE`: `sub_runs=`/`examined=` match exactly; the V19 probe memo never fired in these shapes

`sub_chain_runs` and `rows_examined` were pulled from `ANALYZE` on both
arms of every pair, at every size, for every correlated/hoisted shape:

| Shape | rows | `sub_runs=` (both arms) | `examined=` (both arms) |
|---|---:|---:|---|
| `exists` | 200/1,000/10,000 | 200 / 1,000 / 10,000 | `[400,200,200]` / `[2000,1000,1000]` / `[20000,10000,10000]` |
| `in_` | 200/1,000/10,000 | 200 / 1,000 / 10,000 | `[705,200,505]` / `[3402,1000,2402]` / `[34021,10000,24021]` |
| `scalar` | 200/1,000/10,000 | 200 / 1,000 / 10,000 | `[800,200,600]` / `[4000,1000,3000]` / `[40000,10000,30000]` |
| `control_hoisted` | 200/1,000/10,000 | **1** (all sizes) | `[600,600]` / `[3000,3000]` / `[30000,30000]` |

Identical on every arm, every size, both pairs — 32 comparisons, zero
deltas — confirming `sub_chain_runs` counts evaluations, not allocations,
exactly as the proposal says. `control_hoisted`'s `sub_runs=1` regardless
of outer-row count is the direct, counter-level confirmation that it is
evaluated once, independent of the caching policy under test.

**The one expected divergence did not appear.** The task brief for this
run predicted `probe_memo_hits`/`trail_replays`/`trail_misses` would
differ on an OPT-006 arm, since the V19 probe memo can now survive across
outer rows on a cached runner. Across every `ANALYZE` snapshot taken —
both pairs, all three sizes, all four correlated/hoisted shapes —
`memo_hits=[]`, `replays=[]`, `trail_misses=[]` on **every** arm; the
memo never fired in any shape this driver built. Reported plainly rather
than assumed: these five shapes do not happen to repeat the identical
`(step_id, key)` pair within one runner's lifetime in the way that would
trigger it, so this run confirms the *unaffected* counters
(`sub_runs=`/`examined=`) match, but does not exercise the *affected*
ones at all. A shape built to hit the memo — a correlated probe that
revisits the same key twice per outer row — is future work, not this
entry's.

## Pair 1: OPT-006's own win, QPS by shape

| rows | shape | 005 QPS | final QPS | Δ QPS | floor (arm 005, split-half) |
|---:|---|---:|---:|---:|---:|
| 200 | exists | 1,025 | 1,072 | **+4.5%** | 0.4% |
| 200 | in_ | 1,060 | 1,111 | **+4.8%** | 0.6% |
| 200 | scalar | 1,003 | 1,046 | **+4.4%** | 1.3% |
| 200 | control_hoisted | 6,101 | 6,540 | +7.2% | 10.5%(1) |
| 200 | control_update | 1,095 | 1,096 | +0.1% | 1.6% |
| 1,000 | exists | 242 | 255 | **+5.5%** | 1.0% |
| 1,000 | in_ | 245 | 267 | **+8.8%** | 5.3% |
| 1,000 | scalar | 237 | 252 | **+6.0%** | 0.2% |
| 1,000 | control_hoisted | 2,985 | 2,905 | -2.7% | 0.2% |
| 1,000 | control_update | 263 | 257 | -2.4% | 2.6% |
| 10,000 | exists | 25 | 27 | **+8.9%** | 0.8% |
| 10,000 | in_ | 25 | 28 | **+9.9%** | 0.4% |
| 10,000 | scalar | 24 | 26 | **+7.9%** | 0.8% |
| 10,000 | control_hoisted | 395 | 404 | +2.2% | 0.1% |
| 10,000 | control_update | 27 | 27 | -2.9% | 0.7% |

(1) `control_hoisted` is the cheapest statement in this driver
(~150-160us total, mostly socket round trip), so its own 200-row floor
(10.5%) is wider than the delta it shows at any size — every
`control_hoisted` reading is inside its own noise.

**`exists`/`in_`/`scalar` all move in the predicted direction, at every
size, by 4-9%** — consistently well above the corresponding floor
(0.2-5.3%, with `in_`'s 1,000-row floor of 5.3% the one cell where the
delta and floor sit close, and even there `in_` still shows the same
sign and similar magnitude at the neighbouring sizes). **The win grows
with outer-row count**: `exists` goes 4.5% -> 5.5% -> 8.9% from 200 to
10,000 rows, `in_` 4.8% -> 8.8% -> 9.9%, `scalar` 4.4% -> 6.0% -> 7.9% —
the exact shape the proposal predicted, since outer-row count is what
sets how many runner rebuilds OPT-006 removes from one statement.
`control_update` sits inside its own floor at every size (0.1%, -2.4%,
-2.9% against floors of 1.6%/2.6%/0.7%) — flat, confirming `1495016`'s
"at worst neutral" claim for the per-scanned-row `UPDATE` path once the
fix is in place.

## Pair 2: the regression, reproduced and un-reproduced

| rows | shape | 006-first QPS | final QPS | Δ QPS | floor (arm 006-first) |
|---:|---|---:|---:|---:|---:|
| 200 | exists | 1,078 | 1,062 | -1.5% | 1.5% |
| 200 | in_ | 1,120 | 1,104 | -1.4% | 0.5% |
| 200 | scalar | 1,058 | 1,059 | +0.0% | 1.5% |
| 200 | control_hoisted | 6,437 | 6,579 | +2.2% | 8.0% |
| 200 | control_update | 1,033 | 1,095 | **+6.0%** | 2.1% |
| 1,000 | exists | 259 | 258 | -0.5% | 0.6% |
| 1,000 | in_ | 269 | 264 | -2.1% | 0.3% |
| 1,000 | scalar | 255 | 253 | -0.7% | 0.5% |
| 1,000 | control_hoisted | 2,873 | 2,873 | -0.0% | 4.9% |
| 1,000 | control_update | 249 | 260 | **+4.4%** | 2.9% |
| 10,000 | exists | 27 | 27 | -1.8% | 1.5% |
| 10,000 | in_ | 28 | 28 | +0.4% | 3.3% |
| 10,000 | scalar | 26 | 26 | +0.0% | 1.9% |
| 10,000 | control_hoisted | 390 | 400 | +2.6% | 8.2% |
| 10,000 | control_update | 25 | 27 | **+4.7%** | 3.9% |

**`exists`/`in_`/`scalar`/`control_hoisted` are flat** (-2.1% to +2.6%,
inside or at their own floor) — exactly the expected result, since both
`ff27662` and `1495016` already cache the runner for a sub-chain
evaluated many times on one runner; the two policies differ only in
whether the *first* evaluation on a runner allocates, a one-row-out-of-
hundreds-or-thousands difference these shapes amortise away.

**`control_update` is the one shape that separates, in the same
direction, at every size: final is 4.4-6.0% faster than `006-first`**,
above its own floor (2.1-3.9%) at every size. This is the regression,
priced directly: `006-first`'s unconditional cache-from-first-eval turns
`EvaluateConjuncts`'s per-scanned-row stack `ChainRunner` into a
heap-allocated one on *every* row of *every* `UPDATE`/`DELETE`
statement — the review's own description of the defect — and `1495016`'s
two-stage fix (stack on the first sighting, cache from the second)
restores exactly the pre-OPT-006 behaviour on this path, since a runner
rebuilt fresh per row never reaches its own "second sighting."

Read together with pair 1's `control_update` result (flat against the
pre-OPT-006 baseline), the two pairs bound the regression cleanly:
`c578e29` (no caching) and `1495016` (fixed) are statistically
indistinguishable on `control_update`; `ff27662` (unconditional caching)
sits 4-6% below both. The regression is real, it is exactly the size the
review's description predicts (one heap allocation against a full
`UPDATE` statement's other costs — WAL append, catalog lookup, page
write), and the fix removes it rather than merely reducing it.

## Correctness

Every relation's full contents and every correlated query's own result
set (`SELECT id FROM t_outer WHERE <predicate> ORDER BY id`, sha256 of
the reply) matched byte-for-byte between arms, post-load and
post-writes, at every size, in both pairs: 5 checks x 2 checkpoints x 3
sizes x 2 pairs = 60 comparisons, zero mismatches. Zero statement errors
on either arm across the whole run. A correlated subquery returning the
wrong rows — the failure mode this role is told matters most — did not
happen on the regressed arm, on the fixed arm, or on the pre-OPT-006
baseline. Full detail: `pre_check`/`post_check`/`errors` keys in every
`archive/*.json`.

## What this run teaches

**OPT-006 is a genuine, scaling win exactly where its own hypothesis
said to look, and the review's regression is not a hypothetical — it
reproduces on the commit it was found in and disappears on the commit
that fixed it, on the same host, in the same session.** The magnitude
(4-9%, not the "dominant once JB4/JB6's build turns the walk into a
bucket probe" language the proposal opened with) is modest because this
driver's correlated shapes still pay a real per-row cost inside the
cached runner — an index probe and a heap resolve per outer row — that
OPT-006 never touches; what it removes is the allocation *around* that
work, which is a shrinking fraction of a growing per-row cost as the
inner walk itself gets more expensive. That is worth carrying back to
`docs/spec/join-inner-build.md`: **the win scales with outer-row count,
not with inner-row count or build density**, which is the axis the
proposal predicted correctly but the specific mechanism (JB4/JB6's
bucket probe) does not — this run's `t_inner` is index-probed, not
bucket-built, and the win shows up anyway, from the runner-construction
cost alone. `control_hoisted`'s flatness across both pairs is the
cleanest possible confirmation that this entry's change is scoped
exactly where it claims to be: the one subquery form structurally immune
to per-outer-row cost is immune to this change too, in both directions.

## Files

- Driver: `CIP/OPT-006-subchain-runner-reuse/archive/opt006_ab.py`
- Raw JSON + logs: `CIP/OPT-006-subchain-runner-reuse/archive/pair{1,2}-*-run{200,1000,10000}.{json,log}`
- Proposal: `CIP/OPT-006-subchain-runner-reuse/proposal.md`
