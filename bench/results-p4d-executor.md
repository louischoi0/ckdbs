# The executor coroutine conversion, priced per statement (P4d-2)

**Thesis.** Converting the executor spine to stackless coroutines
(`0fd7fc3`, P4d-1/P4d-2) costs **~55 ns per examined row** on the walk
path and **under ~1 µs fixed per point statement**. On a 10,000-row full
scan that is **+51% on the pure walk** (render and reply removed) and
**+18–25% client-visible**; on the ordinary OLTP mix at 1,000 rows the
p50 deltas sit **inside the in-run noise floor**, with a consistent
+0.3–0.8 µs signature at p0. The cost is per-row, not per-statement,
and it is all server CPU — no new wait of any kind appears. The row-set
scaling inside one run (20 vs 10,000 examined rows) is what pins the
delta on the converted walk rather than on anything else in the commit
range.

## 1. The run

| | |
|---|---|
| **Executed** | 2026-08-13, 14:12–14:26 UTC |
| **Head** | `0fd7fc3` — "feat: the executor spine is coroutines, with zero suspension points (P4d-2)", branch `p4d-suspendable-executor` (committed 13:44:36 UTC) |
| **Base** | `2bd5030` — "fix: Location and TupleLocation carried spans that outlived their pins" (committed 09:02:52 UTC; an ancestor of `origin/main`) |
| **Trees** | Both sides built in **pristine detached scratch worktrees** (`git status --porcelain` empty on each). The branch worktree itself was **not** used for either binary: a reviewer was editing it during this session, and `src/exec/step_vm.cpp` was in fact dirty there before this file was written. |
| **Binaries** | Release, `-DKDS_WITH_TLS=OFF` both sides (no OpenSSL headers on this host; identical both sides, so it cancels). Built 14:10:09 (head) and 14:11:30 (base) UTC — both newer than their commits. |
| **Device** | ext4 on `/dev/root`, data dirs under `/home/cdkbs/bench-p4d-exec/` — not tmpfs. |
| **Machine** | AMD EPYC 9V74, 2 vCPUs, Azure. Quiet at run start (load 0.29, no compilers). A pre-existing unrelated `kds_server` (port 15432, pid 343529) idled throughout: +0.37 s CPU across the mix window, +0.81 s across the scan window — ~0.2% of one core, recorded and negligible. |
| **Server config** | One fresh server process and one fresh data file per side per run, via `bench/run_ab_server.sh`: `cores = 1`, `durability = relaxed`, `log_level = warn`, everything else default. Ports 15601/15602 (mix), 15871/15873 (scan). |
| **Drivers** | `tools/assertion_abort_benchmark.py` (ordinary arms, `--rows 1000 --ordinary-ops 1000 --txns 200 --reservations 10 --seed 1`) and `tools/order_by_benchmark.py` (`--rows 10000 --rounds 10 --limit 20 --seed 1`, base on `--pre-port`), both from the head tree, documented in `bench/docs/README.md`. Both interleave the two servers block by block inside one run. |
| **Verify** | Passed on both runs: the mix driver's row-count + GROUP-BY-restoration checks (`verify: passed`), and the scan driver's eight reply properties per side (`checks: 8`, both sides; a failure aborts the run). |

**What the range contains.** `2bd5030..0fd7fc3` is 11 commits: the two
P4d code commits (whose whole code delta is `include/kds/sched/coro.hpp`
and `src/exec/step_vm.cpp`), three doc commits, **and** the `owner_oid`
series (`1e219a7` + review fixes) with two `origin/main` merges. The A/B
therefore prices the range, not the conversion alone; §5 says why the
per-row component is attributable to the conversion anyway, and why the
sub-µs fixed component is not cleanly attributable to either.

**No PostgreSQL column, by design.** This is a two-ckdbs A/B on one
axis — the same engine either side of one commit range. A PostgreSQL
baseline prices an engine against another engine, not an engine against
itself; the workloads used here already have twins (`pg_benchmark.py`
shapes) for the cross-engine question, which this file does not ask.

## 2. The full-scan side: ~55 ns per examined row, all of it CPU

The shape that multiplies the cost: on the converted spine
`AcceptTupleAt` is a coroutine called once per examined tuple and the
`RunStep` descent once per emitted row, so a 10,000-row scan pays
~20,000 coroutine frames where the base pays zero. The cleanest arm is
`an-plain` — the same walk under `ANALYZE`, render and reply removed —
and it moves **+554.5 µs at p50 (+51.3%)** and **+554.5 µs at p0**,
i.e. exactly **55.5 ns per examined row**, with server CPU per op up
+600 µs to match. The client-visible scans move by the same absolute
amount plus render-path variance.

Pass 2 of the scan run (400 ops per arm per side, 10 interleaved
rounds; latencies in µs):

| arm | side | ops | p0 | p25 | p50 | p95 | p99 | cpu µs/op |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| ping | head | 400 | 19.9 | 21.5 | 26.6 | 37.0 | 57.4 | 0 |
| ping | base | 400 | 19.9 | 26.2 | 26.8 | 37.1 | 54.9 | 25 |
| plain | head | 400 | 3448.6 | 3537.5 | 3627.6 | 7113.2 | 8405.6 | 2725 |
| plain | base | 400 | 2782.4 | 2890.0 | 2954.6 | 4003.5 | 4238.3 | 2175 |
| star | head | 400 | 3428.0 | 3502.9 | 3599.0 | 4598.2 | 5063.4 | 2675 |
| star | base | 400 | 2732.7 | 2799.8 | 2889.8 | 3889.9 | 3998.5 | 1950 |
| pk-order | head | 400 | 3421.4 | 3508.2 | 3585.8 | 4641.3 | 5576.9 | 2675 |
| pk-order | base | 400 | 2778.1 | 2864.2 | 2955.9 | 4004.3 | 5307.6 | 2125 |
| nonpk | head | 400 | 5684.1 | 5918.9 | 6015.5 | 7307.0 | 10549.4 | 5100 |
| nonpk | base | 400 | 4854.3 | 5208.6 | 5305.4 | 10749.1 | 12176.5 | 4350 |
| nonpk-desc | head | 400 | 5746.6 | 5894.1 | 6016.3 | 7919.5 | 13940.4 | 5175 |
| nonpk-desc | base | 400 | 5026.4 | 5180.9 | 5284.5 | 6932.9 | 11021.8 | 4375 |
| nonpk-str | head | 400 | 6464.6 | 6597.8 | 6737.3 | 13870.2 | 15682.9 | 5900 |
| nonpk-str | base | 400 | 5675.6 | 5862.4 | 6011.8 | 7536.1 | 10901.6 | 5100 |
| nonpk-lim | head | 400 | 1825.2 | 1842.6 | 1850.5 | 3983.7 | 5058.7 | 1825 |
| nonpk-lim | base | 400 | 1218.2 | 1229.7 | 1236.9 | 1341.0 | 2505.0 | 1225 |
| an-plain | head | 400 | 1616.1 | 1628.6 | 1636.2 | 1754.1 | 2623.1 | 1650 |
| an-plain | base | 400 | 1061.6 | 1077.1 | 1081.7 | 2156.8 | 2604.2 | 1050 |
| an-nonpk | head | 400 | 3366.3 | 3484.5 | 3497.2 | 7437.4 | 8351.5 | 3500 |
| an-nonpk | base | 400 | 2838.2 | 2934.8 | 2944.1 | 6484.0 | 7172.6 | 2950 |
| an-nonpk-lim | head | 400 | 1816.5 | 1828.1 | 1833.8 | 3784.8 | 4845.6 | 1825 |
| an-nonpk-lim | base | 400 | 1224.3 | 1240.1 | 1247.0 | 2544.5 | 3187.3 | 1300 |
| an-plain-lim | head | 400 | 33.4 | 39.1 | 43.1 | 55.8 | 127.6 | 0 |
| an-plain-lim | base | 400 | 31.6 | 40.2 | 41.6 | 58.5 | 87.3 | 25 |
| plain-lim | head | 400 | 31.4 | 37.2 | 41.2 | 55.5 | 504.4 | 75 |
| plain-lim | base | 400 | 29.8 | 39.1 | 39.8 | 55.4 | 502.5 | 0 |
| pk-point | head | 400 | 29.0 | 35.8 | 39.5 | 53.6 | 81.2 | 0 |
| pk-point | base | 400 | 28.1 | 37.8 | 38.8 | 57.5 | 87.7 | 0 |
| plain-again | head | 400 | 3454.7 | 3569.4 | 3729.6 | 6856.2 | 8125.0 | 2800 |
| plain-again | base | 400 | 2793.0 | 2929.2 | 3174.7 | 5966.6 | 8743.9 | 2075 |

The deltas, reduced to per-examined-row cost (all arms examine 10,000
tuples except the three short ones):

| arm | examined | Δp50 (µs) | Δp50 % | Δp0 (µs) | ns per examined row |
|---|---:|---:|---:|---:|---:|
| an-plain (walk only) | 10,000 | +554.5 | +51.3% | +554.5 | 55.5 |
| an-nonpk | 10,000 | +553.1 | +18.8% | +528.1 | 55.3 |
| an-nonpk-lim | 10,000 | +586.8 | +47.1% | +592.2 | 58.7 |
| nonpk-lim | 10,000 | +613.6 | +49.6% | +607.0 | 61.4 |
| plain-again | 10,000 | +554.9 | +17.5% | +661.7 | 55.5 |
| pk-order | 10,000 | +629.9 | +21.3% | +643.3 | 63.0 |
| plain | 10,000 | +673.0 | +22.8% | +666.2 | 67.3 |
| star | 10,000 | +709.2 | +24.5% | +695.3 | 70.9 |
| nonpk | 10,000 | +710.1 | +13.4% | +829.8 | 71.0 |
| plain-lim | 20 | +1.4 | +3.5% | +1.6 | ~70 (20 rows + fixed) |
| an-plain-lim | 20 | +1.5 | +3.6% | +1.8 | ~75 (20 rows + fixed) |
| pk-point | 1 | +0.7 | +1.8% | +0.9 | fixed cost only |
| ping (control) | 0 | −0.2 | −0.7% | 0.0 | 0 |

Three facts in that table carry the section:

- **The absolute delta is the same whether or not a sort runs.** `nonpk`
  moves +710 µs against `plain`'s +673 — the sort itself did not get
  slower, only the walk feeding it. The conversion's cost is confined to
  the converted spine.
- **The delta scales with rows, not pages.** 20 examined rows cost
  +1.4–1.5 µs; 10,000 cost +550–710 µs; one row costs +0.7 µs. A
  per-page cost (145 pages at 10k rows, 1 page on the short arms) fits
  no line of that; ~55–70 ns per examined row fits all of them.
- **It is all CPU.** The `/proc` server-CPU column moves +525–800 µs/op
  on the 10k arms, matching the wall-clock delta. Nothing waits.

Pass 1 of the same run (the arms both binaries answer, separately
interleaved) agrees: `an-plain` +561.2 µs at p50, `plain` +577.8,
`pk-point` +2.5, ping −1.8 — the finding is not a property of one
pass's interleaving.

**Tails are below this run's resolution.** Head shows the wider p95 on
`plain` in pass 2 (7113 vs 4004) but base shows the wider p95 on
`nonpk` (10749 vs 7307), and pass 1 flips several of them; one 731 ms
outlier sits in `nonpk[head]`'s max. Nothing consistent enough to
report survives both passes above p50.

## 3. The ordinary mix at 1,000 rows: inside the floor at p50, a +0.5 µs signature at p0

The mix run drives autocommitted INSERT / UPDATE / pk-SELECT / DELETE
(1,000 ops per arm per relation per side) plus BEGIN/INSERT×10/COMMIT
and ROLLBACK transaction arms, over four identically loaded 1,000-row
relations per side — `none` and `twin` unasserted (`twin` is the in-run
noise floor), `cnt` and `multi` asserted. Latencies in µs:

| arm | relation | ops/side | head p50 | base p50 | Δp50 | head p0 | base p0 | Δp0 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| ping | — | 500 | 29.2 | 22.0 | +7.2 | 21.3 | 21.3 | +0.0 |
| ac-insert | none | 1000 | 33.0 | 32.8 | +0.2 | 23.3 | 22.7 | +0.6 |
| ac-insert | twin | 1000 | 31.9 | 31.8 | +0.1 | 23.2 | 22.7 | +0.5 |
| ac-insert | cnt | 1000 | 33.4 | 33.2 | +0.2 | 24.1 | 23.6 | +0.5 |
| ac-insert | multi | 1000 | 34.7 | 34.2 | +0.5 | 24.8 | 24.0 | +0.8 |
| ac-update | none | 1000 | 34.1 | 34.0 | +0.1 | 24.1 | 23.5 | +0.6 |
| ac-update | twin | 1000 | 33.0 | 32.9 | +0.1 | 23.8 | 23.3 | +0.5 |
| ac-update | cnt | 1000 | 33.2 | 33.0 | +0.2 | 24.1 | 23.4 | +0.7 |
| ac-update | multi | 1000 | 35.7 | 35.4 | +0.3 | 25.3 | 24.7 | +0.6 |
| ac-select | none | 1000 | 36.3 | 36.0 | +0.3 | 24.9 | 24.5 | +0.4 |
| ac-select | twin | 1000 | 35.1 | 34.6 | +0.5 | 24.8 | 24.3 | +0.5 |
| ac-select | cnt | 1000 | 35.4 | 34.8 | +0.6 | 24.6 | 24.1 | +0.5 |
| ac-select | multi | 1000 | 35.4 | 35.3 | +0.1 | 24.6 | 24.3 | +0.3 |
| ac-delete | none | 1000 | 33.0 | 32.9 | +0.1 | 23.0 | 22.5 | +0.5 |
| ac-delete | twin | 1000 | 31.8 | 32.1 | −0.3 | 22.9 | 22.5 | +0.4 |
| ac-delete | cnt | 1000 | 33.0 | 33.4 | −0.4 | 23.9 | 23.4 | +0.5 |
| ac-delete | multi | 1000 | 34.3 | 34.1 | +0.2 | 24.5 | 23.8 | +0.7 |
| begin | none | 400 | 29.1 | 29.1 | +0.0 | 20.7 | 20.7 | +0.0 |
| insert (in txn) | none | 4000 | 32.4 | 32.2 | +0.2 | 23.0 | 22.5 | +0.5 |
| insert (in txn) | multi | 4000 | 34.0 | 33.6 | +0.4 | 24.1 | 23.4 | +0.7 |
| commit | none | 200 | 29.3 | 29.1 | +0.2 | 20.8 | 20.7 | +0.1 |
| commit | multi | 200 | 31.5 | 31.2 | +0.3 | 22.3 | 22.0 | +0.3 |
| rollback | none | 200 | 30.5 | 30.8 | −0.3 | 21.8 | 21.6 | +0.2 |
| rollback | multi | 200 | 36.1 | 36.0 | +0.1 | 26.1 | 25.8 | +0.3 |

(The omitted `twin`/`cnt` transaction-arm rows behave identically; the
full percentile set for every cell is in
`/home/cdkbs/bench-p4d-exec/mix-1000.json`. p25/p95/p99 per cell were
inspected and add nothing the two quoted percentiles do not show.)

Two readings, and they must be kept apart:

- **At p50, no reportable delta.** The head−base gaps span −0.4 to
  +0.6 µs. The in-run floor — `none` vs `twin`, the same binary driving
  two identically loaded relations — is ~1.0–1.2 µs on the same arms.
  Every p50 delta is inside it, and per the rules that is the finding:
  *the mix's median cost did not measurably move.*
- **At p0, a consistent signature.** All sixteen ordinary arm×relation
  cells are positive, +0.3 to +0.8 µs, while the two arms that never
  enter the executor — `ping` (+0.0) and `begin` (+0.0) — do not move
  at all, on either side of 500/400 samples. A 16-for-16 sign
  agreement at the distribution floor, with clean negative controls, is
  a real fixed cost of roughly half a microsecond per statement that
  runs the executor — visible at the floor, dissolved in scheduling
  noise by the median. (`ping`'s +7.2 µs at p50 with p0 identical is
  harness scheduling on an arm with no engine work; it appears in
  neither scan pass.)

## 4. Where the time waits

Per the accounting rule, the measured unit decomposes as follows on
both shapes; every named wait type either appears or is stated absent.

| wait type | mix arms | 10k scan arms |
|---|---|---|
| client + socket round trip | ~21–26 µs of every ~33 µs statement (the `ping` floor — the dominant share) | ~27 µs of 1.1–6.7 ms — under 1% |
| durability / commit fsync | **0** — `durability = relaxed` puts no fsync on any measured path | **0** — read-only arms reach no durability point |
| read (device) I/O | 0 — 1,000-row relations resident | 0 — 145 pages resident |
| lock / conflict wait | 0 — one connection per side, no concurrent writer | 0 — same |
| server CPU (the residual) | the remaining ~7–12 µs; the A/B delta lives here | effectively the whole statement; the `/proc` CPU column matches the wall delta arm by arm |

The conversion added CPU and nothing else: no arm's delta appears in
any wait class, which is what "zero suspension points" should look like
from outside.

## 5. What is attributable to the conversion, and what is not

The commit range carries `owner_oid` (`1e219a7`) besides the
conversion. The two components separate on scaling:

- **The per-row ~55 ns is the conversion's.** The pure P4d code delta
  is `coro.hpp` + `step_vm.cpp` — the walk spine. The measured delta
  scales with examined rows across three orders of magnitude (1 → 20 →
  10,000) and is indifferent to page count; `owner_oid` is a
  common-page-header field whose costs sit on page creation and
  fetch-validation paths, and a per-page model does not fit the data.
- **The fixed ~0.5 µs per statement is not cleanly attributable.**
  ~3–5 coroutine frames per point statement at the scan-derived frame
  cost explains only ~0.1–0.2 µs of it; the remainder may be the
  conversion's cold-path behavior, `owner_oid`'s admission work, or
  both. A rerun against base `b779a8b` (the branch immediately before
  P4d-1, with the merges already in) would isolate it; that base was
  not run here by direction — smallest batch.

## 6. Insight: the engine now pays rent per row for a capability it does not use yet

At `0fd7fc3` the conversion is deliberately suspension-free — the spine
is coroutines so that P4d-3 can put awaits at the page boundary, but
today nothing suspends, so today's cost is pure overhead purchased
against that future. The measurement says the rent is **per examined
row, not per statement**: ~55 ns/row on the walk ≈ two coroutine frames
per emitted row (`AcceptTupleAt`, then the `RunStep` descent into the
sink), ≈ **28 ns per frame create-resume-destroy cycle**. That is heap
allocation plus two symmetric transfers: `Coro::promise_type` has no
custom `operator new`, and elision (HALO) is structurally unavailable
because `initial_suspend` is `suspend_always` and the handle escapes
through `ResumeDeepest` — the parent's own survey predicted exactly
this suspect, and the numbers confirm it.

Consequences worth acting on, named for `docs/workplan-crosscore.md`
P4e's accept/reject:

- **OLTP point traffic is safe.** Sub-µs fixed cost, invisible at p50
  under a ~25 µs socket floor. The conversion does not tax the
  workload KDS is specialized for.
- **Scan-shaped work pays 18–51%.** Aggregations, non-pk filters,
  `ORDER BY` feeds and every physical-optimizer walk ride the same
  spine. If P4d-3 keeps frames per *row*, this cost stays; a frame
  recycler (a custom promise `operator new` over a per-core freelist)
  or moving the coroutine boundary from per-tuple to per-page are the
  two obvious levers, and the 28 ns/frame figure is the budget either
  must beat.
- **The sort is exonerated.** `nonpk`'s absolute delta equals
  `plain`'s; `exec::OutputSort` sits outside the converted spine and
  its cost did not move.

## 7. Row-set sweep status

By direction this run is the smallest batch that stresses the change:
one interleaved A/B per shape, mix at one size plus one scan size. The
sizes present are 1,000 rows (mix; `--rows 1000`, ops ≤ rows by the
driver's rule), 10,000 rows (scan arms; `--rows 10000`), and the scan
run's own 1- and 20-row arms (`pk-point`, `--limit 20`), which is what
separates the fixed cost from the per-row cost inside one run. The
200-row cell of the documented sweep was not run; the full three-size
matrix on both shapes belongs to P4e's equivalence-and-benchmark rerun
(`docs/workplan-crosscore.md`), which should also take the `b779a8b`
base that isolates the conversion from `owner_oid`.

Raw artifacts: `/home/cdkbs/bench-p4d-exec/mix-1000.json`,
`/home/cdkbs/bench-p4d-exec/scan-10000.json`,
`/home/cdkbs/bench-p4d-exec/scan-10000.out`.
