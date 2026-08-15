# The executor coroutine conversion, priced per statement (P4d-2, re-measured at the terminal split)

**Thesis.** With the terminal split landed (`95946c4` — `AcceptTupleAt` is
synchronous again, the terminal descent inlined as `EmitRow()`), the
coroutine conversion's cost on the walk path is **~8–10 ns per examined
row** against the same base `2bd5030` — down from the **~55 ns/row**
measured at the pre-split head `0fd7fc3` (run of 2026-08-13 14:12 UTC,
same base, same arms, same flags; superseded by this file, headline
numbers kept below as pinned references). The pure walk (`an-plain`,
render and reply removed) moves **+80.4 µs at p50 (+7.6%)** where it
moved +554.5 µs (+51.3%) before; client-visible 10k scans sit at
**+0.7% to +4.1%**, most of it inside this run's render-arm noise floor.
The **~0.5 µs fixed per-statement component did not survive**: the
16-for-16 positive p0 signature the previous run found on the ordinary
mix reads **−0.6 to +0.1 µs** now, sign-mixed around zero. What remains
is a real but small per-row residual on the walk, all server CPU, with
no new wait of any kind.

## 1. The run

| | |
|---|---|
| **Executed** | 2026-08-13, 22:39–22:42 UTC (scan passes ended 22:40:33, mix 22:41:28) |
| **Head** | `95946c4` — "perf: the per-tuple path allocates no coroutine frames - AcceptTupleAt is synchronous again", branch `p4d-suspendable-executor` (committed 22:33:40 UTC) |
| **Base** | `2bd5030` — "fix: Location and TupleLocation carried spans that outlived their pins" (committed 09:02:52 UTC; an ancestor of `origin/main`) — the same base as the superseded run |
| **Trees** | Both sides built in **pristine detached scratch worktrees** (`/home/cdkbs/bench-p4d-fix/wt-{head,base}`, `git status --porcelain` empty on each, removed after the run). The branch worktree's `src/` and `include/` were verified clean this session and served neither binary. |
| **Binaries** | Release, `-DKDS_WITH_TLS=OFF` both sides (identical both sides, so it cancels). Built 22:37:02 (head) and 22:38:30 (base) UTC — both newer than their commits; zero compile errors. |
| **Device** | ext4 on `/dev/root`, data dirs under `/home/cdkbs/bench-p4d-fix/` — not tmpfs. |
| **Machine** | AMD EPYC 9V74, 2 vCPUs, Azure. 1-min load 0.58 at the scan's launch and 0.41 at the mix's (driver-recorded); no compiler running. The pre-existing unrelated `kds_server` (port 15432, pid 343529) idled throughout, as in the previous run. |
| **Server config** | One fresh server process and one fresh data file per side per run, via `bench/run_ab_server.sh`: `cores = 1`, `durability = relaxed`, `log_level = warn`, everything else default. Ports 15601/15602 (mix), 15871/15873 (scan). |
| **Drivers** | The exact invocations of the superseded run: `tools/assertion_abort_benchmark.py` (`--rows 1000 --ordinary-ops 1000 --txns 200 --reservations 10 --seed 1`) and `tools/order_by_benchmark.py` (`--rows 10000 --rounds 10 --limit 20 --seed 1`, 400 ops/arm, base on `--pre-port`), both run from the head tree, documented in `bench/docs/README.md`. Both interleave the two servers block by block inside one run. |
| **Verify** | Passed on both runs: the mix driver's row-count + GROUP-BY-restoration checks (`verify: passed`, 9,000 rows per relation), and the scan driver's eight reply properties per side (`checks: 8`, both sides; a failure aborts the run). |

**What the range contains.** `2bd5030..95946c4` is 13 commits: the
superseded run's 11 (the two P4d code commits, three doc commits, the
`owner_oid` series and two `origin/main` merges) **plus** `5ec61da`
(three pre-existing wrong-answer bug fixes the conversion made
conspicuous) and `95946c4` (the terminal split itself). The A/B prices
the range, not the split alone; §5 says what separates and what no
longer does.

**No PostgreSQL column, by design.** This is a two-ckdbs A/B on one
axis — the same engine either side of one commit range. A PostgreSQL
baseline prices an engine against another engine, not an engine against
itself; the workloads used here already have twins (`pg_benchmark.py`
shapes) for the cross-engine question, which this file does not ask.

## 2. The full-scan side: the split removed ~85% of the per-row cost

At `95946c4` a single-step scan allocates **zero coroutine frames per
row**: `AcceptTupleAt` returns `Status` again and its terminal descent
is the plain function `EmitRow()`, with `RunStep` keeping its coroutine
form per statement, not per row. The measured consequence, on the same
arms and flags as the superseded run — pass 2, 400 ops per arm per
side, 10 interleaved rounds, latencies in µs:

| arm | side | ops | p0 | p25 | p50 | p95 | p99 | cpu µs/op |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| ping | head | 400 | 20.0 | 22.2 | 26.8 | 39.7 | 63.3 | 0 |
| ping | base | 400 | 20.1 | 26.5 | 26.9 | 34.8 | 41.3 | 0 |
| plain | head | 400 | 2820.8 | 2931.7 | 3065.0 | 3863.5 | 4314.3 | 2125 |
| plain | base | 400 | 2783.5 | 2855.7 | 2944.0 | 3930.4 | 4759.8 | 2075 |
| star | head | 400 | 2776.7 | 2841.7 | 2912.8 | 3848.7 | 3997.7 | 2075 |
| star | base | 400 | 2731.5 | 2801.9 | 2882.7 | 3856.3 | 4704.8 | 2075 |
| pk-order | head | 400 | 2827.6 | 2906.1 | 2986.5 | 3960.8 | 4211.0 | 2175 |
| pk-order | base | 400 | 2790.5 | 2855.9 | 2965.1 | 4172.9 | 4299.6 | 2100 |
| nonpk | head | 400 | 4977.4 | 5270.8 | 5359.9 | 6580.6 | 6763.4 | 4450 |
| nonpk | base | 400 | 4981.5 | 5156.3 | 5279.9 | 6476.7 | 6799.0 | 4425 |
| nonpk-desc | head | 400 | 5117.1 | 5225.0 | 5382.0 | 6623.9 | 7604.0 | 4450 |
| nonpk-desc | base | 400 | 5003.4 | 5184.4 | 5352.8 | 6546.2 | 7604.7 | 4375 |
| nonpk-str | head | 400 | 5824.3 | 5950.6 | 6082.2 | 7428.2 | 8290.9 | 5175 |
| nonpk-str | base | 400 | 5663.8 | 5835.5 | 5994.9 | 7307.4 | 8017.1 | 5075 |
| nonpk-lim | head | 400 | 1294.3 | 1315.3 | 1323.5 | 1419.4 | 2738.7 | 1300 |
| nonpk-lim | base | 400 | 1218.8 | 1231.1 | 1237.4 | 1339.6 | 2615.6 | 1250 |
| an-plain | head | 400 | 1115.7 | 1133.2 | 1139.7 | 1222.3 | 1536.4 | 1225 |
| an-plain | base | 400 | 1047.9 | 1055.3 | 1059.3 | 1150.3 | 1323.1 | 1075 |
| an-nonpk | head | 400 | 2864.2 | 3013.6 | 3024.6 | 3107.4 | 3602.0 | 3025 |
| an-nonpk | base | 400 | 2833.6 | 2909.2 | 2922.0 | 3054.0 | 3284.6 | 2925 |
| an-nonpk-lim | head | 400 | 1298.2 | 1315.7 | 1322.3 | 1362.3 | 2031.6 | 1325 |
| an-nonpk-lim | base | 400 | 1200.2 | 1215.5 | 1221.6 | 1587.4 | 1906.4 | 1200 |
| an-plain-lim | head | 400 | 31.8 | 39.2 | 41.4 | 53.8 | 146.7 | 50 |
| an-plain-lim | base | 400 | 31.3 | 34.5 | 41.3 | 52.6 | 71.3 | 25 |
| plain-lim | head | 400 | 30.4 | 38.1 | 39.9 | 55.3 | 512.8 | 50 |
| plain-lim | base | 400 | 29.7 | 38.1 | 39.3 | 51.0 | 420.0 | 50 |
| pk-point | head | 400 | 29.7 | 38.1 | 40.0 | 56.3 | 82.7 | 0 |
| pk-point | base | 400 | 28.4 | 37.3 | 38.9 | 52.8 | 74.2 | 25 |
| plain-again | head | 400 | 2816.5 | 2879.7 | 2994.9 | 4204.7 | 4395.6 | 2050 |
| plain-again | base | 400 | 2777.6 | 2824.4 | 2909.1 | 4168.6 | 4845.1 | 2075 |

The deltas, reduced to per-examined-row cost, **with the superseded
run's numbers beside them** (head `0fd7fc3`, 2026-08-13 14:12 UTC, same
base and flags):

| arm | examined | Δp50 (µs) | Δp50 % | Δp0 (µs) | ns/row | Δp50 at `0fd7fc3` | ns/row then |
|---|---:|---:|---:|---:|---:|---:|---:|
| an-plain (walk only) | 10,000 | +80.4 | +7.6% | +67.8 | 8.0 | +554.5 (+51.3%) | 55.5 |
| an-nonpk | 10,000 | +102.6 | +3.5% | +30.6 | 10.3 | +553.1 (+18.8%) | 55.3 |
| an-nonpk-lim | 10,000 | +100.7 | +8.2% | +98.0 | 10.1 | +586.8 (+47.1%) | 58.7 |
| nonpk-lim | 10,000 | +86.1 | +7.0% | +75.5 | 8.6 | +613.6 (+49.6%) | 61.4 |
| plain-again | 10,000 | +85.8 | +2.9% | +38.9 | 8.6 | +554.9 (+17.5%) | 55.5 |
| pk-order | 10,000 | +21.4 | +0.7% | +37.1 | 2.1 | +629.9 (+21.3%) | 63.0 |
| plain | 10,000 | +121.0 | +4.1% | +37.3 | 12.1 | +673.0 (+22.8%) | 67.3 |
| star | 10,000 | +30.1 | +1.0% | +45.2 | 3.0 | +709.2 (+24.5%) | 70.9 |
| nonpk | 10,000 | +80.0 | +1.5% | −4.1 | 8.0 | +710.1 (+13.4%) | 71.0 |
| nonpk-desc | 10,000 | +29.2 | +0.5% | +113.7 | 2.9 | +731.8 (+13.8%) | 73.2 |
| nonpk-str | 10,000 | +87.3 | +1.5% | +160.5 | 8.7 | +725.5 (+12.1%) | 72.6 |
| plain-lim | 20 | +0.6 | +1.5% | +0.7 | — | +1.4 (+3.5%) | — |
| an-plain-lim | 20 | +0.1 | +0.2% | +0.5 | — | +1.5 (+3.6%) | — |
| pk-point | 1 | +1.1 | +2.8% | +1.3 | — | +0.7 (+1.8%) | — |
| ping (control) | 0 | −0.1 | −0.4% | −0.1 | 0 | −0.2 (−0.7%) | — |

How to read that against this run's own noise floors, established
inside the run:

- **The ANALYZE arms carry the finding.** Their distributions are tight
  (p25→p50 spans of 4–7 µs) and they repeat across passes to within
  2.5 µs (`an-plain` head p50 1141.5 in pass 1 vs 1139.7 in pass 2;
  base 1058.9 vs 1059.3). Against that floor, `an-plain`'s +80.4 µs at
  p50 — +82.6 in pass 1 — is a real residual, and the `/proc` CPU
  column moves +100 to +150 µs/op on the same arms to match. **~8 ns
  per examined row survives the split**, down from 55.5.
- **The client-visible deltas straddle the render floor.** The
  same-binary repeat (`plain` vs `plain-again`) differs by 70.1 µs at
  p50 on head and 34.9 µs on base inside this run, and pass 1 has
  `plain` at **−76.0 µs** (head faster) with `star` at +143.5. The
  +21 to +121 µs spread across the rendered 10k arms is therefore not
  resolvable arm by arm; what is supportable is the envelope — client-
  visible scans now sit at **most a few percent** over base, where the
  superseded run had +13–25%.
- **The short arms no longer show a resolvable fixed cost.**
  `plain-lim` +0.6 µs, `an-plain-lim` +0.1 µs, `pk-point` +1.1 µs at
  p50 — each inside the ~1.5 µs by which the same short arm drifts
  between this run's two passes (`pk-point` head moved 1.4 µs between
  passes on one binary). The mix's p0 instrument in §3, sharper than
  these arms, settles the fixed-cost question properly.

## 3. The ordinary mix at 1,000 rows: the p0 signature is gone

The mix drives autocommitted INSERT / UPDATE / pk-SELECT / DELETE
(1,000 ops per arm per relation per side) plus BEGIN/INSERT×10/COMMIT
and ROLLBACK transaction arms, over four identically loaded 1,000-row
relations per side — `none` and `twin` unasserted (`twin` is the in-run
noise floor), `cnt` and `multi` asserted. Latencies in µs:

| arm | relation | side | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---:|---:|---:|---:|---:|---:|
| ping | — | head | 500 | 21.3 | 21.5 | 21.7 | 32.9 | 43.9 |
| ping | — | base | 500 | 21.2 | 21.5 | 23.4 | 34.0 | 46.0 |
| ac-insert | none | head | 1000 | 23.1 | 31.7 | 32.8 | 45.5 | 64.0 |
| ac-insert | none | base | 1000 | 23.3 | 31.7 | 32.7 | 46.3 | 58.4 |
| ac-insert | twin | head | 1000 | 23.0 | 31.0 | 31.8 | 41.7 | 52.2 |
| ac-insert | twin | base | 1000 | 23.2 | 31.4 | 32.0 | 40.2 | 48.4 |
| ac-insert | cnt | head | 1000 | 24.1 | 32.4 | 33.2 | 43.3 | 64.0 |
| ac-insert | cnt | base | 1000 | 24.1 | 32.8 | 33.3 | 42.6 | 62.2 |
| ac-insert | multi | head | 1000 | 24.5 | 33.7 | 34.8 | 45.7 | 56.6 |
| ac-insert | multi | base | 1000 | 24.7 | 33.9 | 34.9 | 46.1 | 61.6 |
| ac-update | none | head | 1000 | 23.7 | 24.7 | 32.2 | 48.1 | 63.8 |
| ac-update | none | base | 1000 | 23.8 | 24.8 | 32.7 | 49.7 | 69.0 |
| ac-update | twin | head | 1000 | 23.7 | 24.4 | 26.2 | 39.0 | 49.9 |
| ac-update | twin | base | 1000 | 23.7 | 24.5 | 31.0 | 36.6 | 44.7 |
| ac-update | cnt | head | 1000 | 23.9 | 24.6 | 26.5 | 39.6 | 45.5 |
| ac-update | cnt | base | 1000 | 23.8 | 24.7 | 31.2 | 39.7 | 51.8 |
| ac-update | multi | head | 1000 | 25.1 | 26.1 | 28.8 | 45.1 | 66.3 |
| ac-update | multi | base | 1000 | 25.1 | 26.3 | 34.4 | 45.4 | 66.4 |
| ac-select | none | head | 1000 | 24.0 | 25.9 | 30.5 | 53.5 | 93.2 |
| ac-select | none | base | 1000 | 24.6 | 26.0 | 31.5 | 52.9 | 70.9 |
| ac-select | twin | head | 1000 | 24.3 | 25.4 | 27.5 | 42.2 | 51.7 |
| ac-select | twin | base | 1000 | 24.4 | 25.7 | 28.4 | 42.7 | 58.5 |
| ac-select | cnt | head | 1000 | 24.3 | 25.4 | 27.9 | 46.4 | 61.6 |
| ac-select | cnt | base | 1000 | 24.4 | 25.5 | 27.9 | 45.2 | 56.9 |
| ac-select | multi | head | 1000 | 24.3 | 25.5 | 30.0 | 44.7 | 74.3 |
| ac-select | multi | base | 1000 | 24.4 | 25.8 | 30.6 | 45.1 | 57.4 |
| ac-delete | none | head | 1000 | 22.9 | 31.8 | 33.0 | 45.4 | 50.5 |
| ac-delete | none | base | 1000 | 23.1 | 31.8 | 32.8 | 47.4 | 58.0 |
| ac-delete | twin | head | 1000 | 22.7 | 30.6 | 31.8 | 38.6 | 45.5 |
| ac-delete | twin | base | 1000 | 23.0 | 31.0 | 32.0 | 37.6 | 44.2 |
| ac-delete | cnt | head | 1000 | 23.7 | 31.6 | 32.9 | 40.3 | 50.2 |
| ac-delete | cnt | base | 1000 | 23.9 | 32.6 | 33.4 | 41.3 | 50.0 |
| ac-delete | multi | head | 1000 | 23.9 | 25.7 | 33.9 | 45.0 | 52.4 |
| ac-delete | multi | base | 1000 | 24.3 | 33.6 | 34.6 | 44.5 | 55.0 |
| begin | none | head | 400 | 20.6 | 23.5 | 29.0 | 39.1 | 47.3 |
| begin | none | base | 400 | 20.5 | 27.6 | 29.3 | 39.6 | 46.1 |
| insert (in txn) | none | head | 4000 | 22.6 | 26.5 | 31.9 | 42.5 | 50.7 |
| insert (in txn) | none | base | 4000 | 22.7 | 30.9 | 32.4 | 42.1 | 49.7 |
| insert (in txn) | multi | head | 4000 | 23.6 | 31.7 | 33.6 | 43.0 | 50.3 |
| insert (in txn) | multi | base | 4000 | 23.5 | 32.4 | 33.8 | 43.6 | 51.2 |
| commit | none | head | 200 | 20.7 | 22.4 | 29.0 | 34.0 | 39.4 |
| commit | none | base | 200 | 20.8 | 23.2 | 29.2 | 36.1 | 40.5 |
| commit | multi | head | 200 | 22.1 | 26.0 | 31.2 | 37.9 | 42.1 |
| commit | multi | base | 200 | 22.3 | 30.2 | 31.5 | 38.4 | 43.1 |
| rollback | none | head | 200 | 21.8 | 24.2 | 30.5 | 35.8 | 44.7 |
| rollback | none | base | 200 | 21.9 | 29.4 | 30.9 | 37.2 | 43.4 |
| rollback | multi | head | 200 | 26.6 | 33.7 | 36.2 | 42.0 | 50.3 |
| rollback | multi | base | 200 | 26.1 | 35.1 | 36.4 | 44.2 | 49.4 |

(The omitted `twin`/`cnt` transaction-arm rows behave identically; the
full set is in `/home/cdkbs/bench-p4d-fix/mix-1000.json`.)

The question this arm exists to answer here: **does the ~0.5 µs fixed
per-statement component measured at `0fd7fc3` survive the split?** The
sixteen ordinary arm×relation cells, at the distribution floor, with
the superseded run's signature beside them:

| arm | relation | Δp50 (µs) | Δp0 (µs) | Δp0 at `0fd7fc3` |
|---|---|---:|---:|---:|
| ac-insert | none | +0.1 | −0.2 | +0.6 |
| ac-insert | twin | −0.2 | −0.2 | +0.5 |
| ac-insert | cnt | −0.1 | +0.0 | +0.5 |
| ac-insert | multi | −0.1 | −0.2 | +0.8 |
| ac-update | none | −0.5 | −0.1 | +0.6 |
| ac-update | twin | −4.8 | +0.0 | +0.5 |
| ac-update | cnt | −4.7 | +0.1 | +0.7 |
| ac-update | multi | −5.6 | +0.0 | +0.6 |
| ac-select | none | −1.0 | −0.6 | +0.4 |
| ac-select | twin | −0.9 | −0.1 | +0.5 |
| ac-select | cnt | +0.0 | −0.1 | +0.5 |
| ac-select | multi | −0.6 | −0.1 | +0.3 |
| ac-delete | none | +0.2 | −0.2 | +0.5 |
| ac-delete | twin | −0.2 | −0.3 | +0.4 |
| ac-delete | cnt | −0.5 | −0.2 | +0.5 |
| ac-delete | multi | −0.7 | −0.4 | +0.7 |

**It does not survive.** Where the superseded run had sixteen positive
p0 deltas of +0.3 to +0.8 µs against flat controls, this run has
thirteen of sixteen at or below zero, spanning −0.6 to +0.1 µs — a
sign-mixed scatter around zero, inside the ≤0.4 µs by which `none` and
`twin` p0s differ on one binary. Per the rules, that is the finding:
*no fixed per-statement cost is resolvable at `95946c4`.* The p50
columns add nothing: every delta is inside this run's p50 floor, which
the same-binary `none`-vs-`twin` gap puts at up to ~6 µs here (head's
`ac-update` p50 is 32.2 on `none` and 26.2 on `twin` — the apparent
−4.7 to −5.6 µs head advantage on three `ac-update` cells sits on
identical p0s and is distribution-body scheduling, not an effect).

## 4. Where the time waits

Per the accounting rule, the measured unit decomposes as follows on
both shapes; every named wait type either appears or is stated absent.

| wait type | mix arms | 10k scan arms |
|---|---|---|
| client + socket round trip | ~21–26 µs of every ~32 µs statement (the `ping` floor — the dominant share) | ~27 µs of 1.1–6.1 ms — under 1% |
| durability / commit fsync | **0** — `durability = relaxed` puts no fsync on any measured path | **0** — read-only arms reach no durability point |
| read (device) I/O | 0 — 1,000-row relations resident | 0 — 145 pages resident |
| lock / conflict wait | 0 — one connection per side, no concurrent writer | 0 — same |
| server CPU (the residual) | the remaining ~7–12 µs; no A/B delta is resolvable in it | effectively the whole statement; the `/proc` CPU column's +100–150 µs/op on the ANALYZE arms matches the wall delta |

As at the previous head, the delta that exists is CPU and nothing
else — no arm's movement appears in any wait class.

## 5. Attribution, and what this run settles from the previous one

- **The removed ~47 ns/row was coroutine frames.** The split's only
  walk-path change is that `AcceptTupleAt` and the terminal emit no
  longer allocate frames on single-step chains; the per-row delta fell
  from ~55.5 to ~8 ns in response. That retroactively confirms the
  superseded run's attribution of the per-row cost to the conversion
  rather than to anything else in the range.
- **The vanished fixed component was also the conversion's.** The
  previous run could not split its ~0.5 µs/statement between the
  conversion's cold path and `owner_oid`'s admission work. This range
  still contains the whole `owner_oid` series, and the signature is
  gone — so it was the conversion's frames, and the split removed it.
  The rerun against `b779a8b` proposed for that question is no longer
  needed.
- **The residual ~8–10 ns/row is real but not yet attributed.** It
  reproduces across both passes on the tightest arms and shows in
  server CPU. On these single-step arms the split leaves zero frames
  per row, so it is not frame allocation; candidates are the split
  path's extra terminal branching, code layout, and the range's other
  code commits (`5ec61da` touches `step_vm.cpp`; `owner_oid` sits on
  fetch-validation paths). The short arms cannot separate a per-row
  from a per-page model for a delta this small (the two models predict
  +0.16 vs +0.55 µs at 20 rows/1 page — both under those arms' ~1.5 µs
  floor), so per-row is the parsimonious reading, not a proven one.
- **Multi-step chains are not priced here.** At `95946c4` a deeper
  local step still drives through the boundary helper at one frame per
  row; every arm in this file is a single-step chain. That shape's
  price belongs to P4e's matrix (`docs/workplan-crosscore.md`), or
  falls to P4d-4's batching first.

## 6. Insight: the page-boundary rule held, and the rent is now near the floor

The workplan's rule — awaits live at page boundaries, never per row —
was the design claim behind P4d; the conversion as first built
(`0fd7fc3`) violated its spirit on the hot path by paying two frames
per tuple, and this run shows the terminal split restoring it. The
engine now keeps suspension capability on the statement spine
(`RunStep` is still a coroutine, so P4d-3 can put awaits at page
boundaries) at a measured price of **+7.6% on the pure walk and about
one percent, inside the floor, on everything OLTP-shaped** — down from
+51.3% and a half-microsecond per point statement.

Consequences, named for `docs/workplan-crosscore.md` P4e's
accept/reject:

- **Point traffic is clean.** No resolvable fixed cost at the p0 floor
  across sixteen cells; `ping` and `begin` flat as before. The
  conversion no longer taxes the workload KDS is specialized for at
  any percentile this run can resolve.
- **Scan-shaped work pays ≤ ~8% on the walk, single-digit client-
  visible.** Whether the remaining ~8 ns/row is worth chasing is a P4e
  question; the levers named before (a frame recycler; per-page
  coroutine boundaries) now apply to the multi-step shape, which still
  pays one frame per row and is unmeasured.
- **The 28 ns/frame figure stands as the budget.** The split removed
  ~47 ns/row by removing ~2 frames/row, consistent with the superseded
  run's per-frame estimate; P4d-4's batching design should still price
  itself against it.

## 7. Row-set sweep status

By direction this run repeats exactly the superseded run's scan arms
plus the ordinary mix as the no-regression check — the smallest batch
that answers "did the fix land". The sizes present are 1,000 rows
(mix; `--rows 1000`, ops ≤ rows by the driver's rule), 10,000 rows
(scan arms; `--rows 10000`), and the scan run's own 1- and 20-row arms
(`pk-point`, `--limit 20`), which is what separates fixed from per-row
cost inside one run. The 200-row cell of the documented sweep was not
run; the full three-size matrix on both shapes belongs to P4e's
equivalence-and-benchmark rerun (`docs/workplan-crosscore.md`), which
should also cover the multi-step chain shape §5 leaves unpriced.

Raw artifacts: `/home/cdkbs/bench-p4d-fix/mix-1000.json`,
`/home/cdkbs/bench-p4d-fix/scan-10000.json`,
`/home/cdkbs/bench-p4d-fix/scan-10000.out`. The superseded run's
artifacts remain at `/home/cdkbs/bench-p4d-exec/` for provenance; its
prose and tables are replaced by this file per the re-run rule.

## 8. P4d-3 (`ea30544`): the executor-owned page loop prices at null

Measured 2026-08-14 00:09–00:10 UTC, `ea30544` ("awaits live at the page
boundary", branch `worktree-feat-coroutine-2`) against its parent
`9e3ca8f`, same drivers, flags and method as §1 (Release, pristine
detached scratch worktrees, fresh server + fresh ext4 data dir per side
under `/home/cdkbs/bench-p4d3/`, `cores = 1`, `durability = relaxed`,
verify passed on both drivers; 2334/2334 Release tests at `ea30544`).
The per-page work `RunWalkStep`'s new loop adds — one call through the
hoisted `std::function`, a `StatusOr<PageId>` return, the cycle-guard
compare — is **not resolvable**: `an-plain` (10,000 rows, 145 pages)
moved **−1.9/−2.2 µs at p50** across the two passes against a ~2 µs
cross-pass repeat floor, bounding the loop at **≤ ~15 ns/page** and the
per-row cost at zero; `pk-point` and `plain-lim` sat within ±0.4 µs, the
sixteen mix cells' p0 deltas were sign-mixed in −1.4 to +0.6 µs (no
signature), the rendered 10k arms' scatter (−47 to +88 µs at p50,
`nonpk-str` head-faster) stayed inside §2's render floor, and the
`/proc` CPU meter was sign-mixed within its 25 µs quantum on every arm.
Raw artifacts: `/home/cdkbs/bench-p4d3/`.

## 9. P4d-4a (`31319c8`): the local path does not pay for the first genuine suspension

Measured 2026-08-14 00:50–00:51 UTC, `31319c8` ("feat: the first genuine
suspension - the remote step producer streams under credit", branch
`worktree-feat-coroutine-2`) against its parent `18db442`, same drivers,
flags and method as §1/§8 (Release, pristine detached scratch worktrees,
binaries built 00:35/00:40 UTC — both newer than their commits — fresh
server + fresh ext4 data dir per side under `/home/cdkbs/bench-p4d4a/`,
`cores = 1`, `durability = relaxed`; the quiet gate opened at 1-min load
0.62, verify passed on both drivers, 2338/2338 Release tests at
`31319c8`). What the commit adds to a **local** statement — `Execute` is
now a wrapper that allocates one `ExecuteAsync` coroutine frame per
statement and drives it inline, one extra `co_await` level at completion,
and a `resume_gate_ != nullptr` compare per page on the outermost walk
(null for every local statement; the streaming producer the gate exists
for has no benchmark and is not priced here) — is **not resolvable on any
arm**. Fixed per statement: the sixteen ordinary arm×relation p0 deltas
span −0.8 to +0.3 µs, fifteen at or below zero — the opposite of the
16-for-16 positive signature this instrument resolved at `0fd7fc3` — and
`pk-point` moved +1.0/+0.3 µs at p50 across the two passes, inside the
~2.6 µs the same arm drifts between passes on one binary (the wrapper
frame at §6's ~28 ns budget is two orders below that floor). Per page:
`an-plain` (10,000 rows, 145 pages) read **head-faster** by 17.9/21.3 µs
at p50 in the two passes against a 0.5–2.9 µs cross-pass repeat floor,
`an-nonpk` −17.6 beside it and `an-nonpk-lim` flat at +0.1 — a
consistent-signed −1.7% that cannot be the mechanism, since the diff only
*adds* instructions to that loop, and is read as binary layout; what it
certifies is that the null-gate compare and the extra await level price
at ≤ ~0 ns/page as measured. The rendered 10k arms scattered −6.7 to
+33.1 µs at p50 (−0.5% to +0.8%), inside this run's own render floor
(same-binary `plain` vs `plain-again` gaps of 28.0 and 22.7 µs), the
`/proc` CPU meter was sign-mixed from −100 to +175 µs/op with no wall
correlate (`nonpk-desc` and `nonpk-str` read opposite signs on adjacent
shapes), and the three `ac-delete` p50 cells at +3.6 to +4.4 µs sit on
p0 deltas of −0.2 to −0.8 and mirror §3's `ac-update` artifact with the
sign reversed — distribution body, not an effect. Sizes present as §7
(1,000-row mix; 10,000-row scans; the 1- and 20-row short arms). Raw
artifacts: `/home/cdkbs/bench-p4d4a/`.

## 10. P4d-4b-3 (`f2f101d`): the local statement pays three predicates, and nothing in this run can see them

**Thesis.** The session side of the cross-core pipeline puts exactly one
new thing on the **local** statement path: a second remote-eligibility
short-circuit chain in `HandleSelect`, evaluated before local execution
is reached. A one-step SELECT exits it after **three** predicates
(`remote_reads_ != nullptr`, `!analyze`, `steps.size() == 2`); an
`ANALYZE` exits after two; with `cores = 1` the pointer is null and it
exits after one. Everything else the commit range adds — the whole
consuming-stage and plan machinery, the refactored `FinishRemoteRead`
renderer — is reached only by a statement that ships. The measurement
below is therefore a null-hunt, and over 38 runs — 32 interleaved A/B
plus 6 same-binary controls — it returns a null at **every** row-set
size, on **both** core counts, on the arm that pays the check and on the
arms that cannot reach it alike. At `cores = 2`, where the chain is
actually entered, the exposed arm's per-run mean Δp0 spans **−0.55 to
+0.13 µs** across eight mix runs against a cross-run floor of
0.15–0.30 µs — sign-mixed, mostly negative, and two orders above what
three predicates can cost. The run's more useful product is the
second one: a **same-binary null control** that shows this harness
manufacturing up to **+55 µs** of apparent p50 delta on a 10,000-row
`an-plain` arm, and **+232 µs** on `plain`, when both servers are the
same build — which is the floor any per-row claim read off those arms
has to clear.

### 10.1 The run

| | |
|---|---|
| **Executed** | 2026-08-15, 01:32:18–02:25:56 UTC (32 A/B runs to 02:05:06, then six same-binary controls from 02:07:45) |
| **Head** | `f2f101d` — "review: the 4b-3 gate - the uint64 orientation hole refused, one renderer, and the third edge decode checked" (committed 01:14:05 UTC), the P4d-4b-3 feature `a9718e0` plus its review gate |
| **Base** | `53fd2ce` — "review: the 4b-2 gate - three validation holes, the upstream-cancel leak closed, and one credit gate" (committed 2026-08-14 02:51:49 UTC), the commit immediately before the feature |
| **Range** | `53fd2ce..f2f101d` is two commits: `a9718e0` (the session side) and `f2f101d` (its review gate). 1,240 insertions over 16 files; the only one on a local statement path is `src/server/command_dispatcher.cpp` |
| **Trees** | Both sides built in **pristine detached scratch worktrees** — `/tmp/ck-4b3-base` and `/tmp/ck-4b3-new`, `git status --porcelain` empty on each at build time, removed after the run. The branch worktree (`worktree-feat-coroutine-2`) was **not built in and not modified**; another session held it. |
| **Binaries** | Release (`-O3 -DNDEBUG`, g++ 13.3.0, C++20), `-DKDS_WITH_TLS=OFF` on **both** sides so it cancels; no OpenSSL headers on this box and the bench does not need TLS. Linked 01:24:41 (base) and 01:26:29 (head) UTC — **both newer than their commits**; `make kds_server` reported zero errors on both |
| **Device** | ext4 on `/dev/root` (`/dev/nvme0n1p1`), data dirs under `/home/cdkbs/bench-p4d4b3/` — not tmpfs. `/tmp` on this box is not a tmpfs either; it holds only source and object files |
| **Machine** | AMD EPYC 9V74, **2 vCPUs**, Azure. Every run is gated by `bench/wait_quiet.sh` (no `cc1plus`/`ld`/`kds_tests`, 1-min load < 0.70); the gate held the schedule's first run until 01:32:18 because another session's `kds_tests` occupied a vCPU until 01:32:01, which is exactly the concurrency it exists to catch, and it re-gated before every one of the 38 runs. Driver-recorded 1-min load 0.63–0.67 at each mix run's launch. The pre-existing unrelated `kds_server` (port 15432, pid 343529) idled throughout, as in §1 |
| **Server config** | Fresh server process **and** fresh data file per side per run: `durability = relaxed`, `log_level = warn`, everything else default. Two configurations, `cores = 1` and `cores = 2` — see §10.2, this is the knob that decides whether the new chain is entered at all. Ports 15601/15602 and 15611/15612 (mix), 15871/15873 and 15881/15883 (scan) |
| **Drivers** | `tools/assertion_abort_benchmark.py` (`--rows R --ordinary-ops N --txns 200 --reservations 10 --seed 1`) and `tools/order_by_benchmark.py` (`--rows R --rounds 10 --limit 20 --seed 1`, base on `--pre-port`), both documented in `bench/docs/README.md`, both interleaving the two servers block by block inside one run. Launchers: `bench/run_ab_server.sh` verbatim for `cores = 1`, and a scratch copy differing only in the `cores` line for `cores = 2` |
| **Driver delta** | The drivers were run from a copy of `tools/` carrying **one added line** — `"p75_us": round(self.percentile(75) * 1e6, 1)` in `bench_common.Phase.summary()`, so this file's p75 column exists. It is computed after timing ends, from the same nearest-rank function as the other five, and touches no timed path. Nothing else in `tools/` differs from `f2f101d` |
| **Verify** | Passed on all 38 runs: the mix driver's row-count and GROUP-BY-restoration checks (`verify: passed`) and the scan driver's eight reply properties per side (`checks: 8` on both sides, both binaries, every size) |
| **Correctness suite** | **Not executed this session** — no code was changed for this measurement, and the coordinator's standing instruction was to start no further long build. This section reports no test result, green or otherwise |

**Provenance of the claim, not of the tree.** This section pins
`f2f101d`. The branch has since moved (`a45a4d4`, `e12c468` landed while
the schedule ran); §10.9 says what `a45a4d4` does and does not change
about the shape measured here.

**No PostgreSQL column, by design**, for §1's reason: this is a
two-ckdbs A/B on one axis — the same engine either side of one commit
range. A PostgreSQL baseline prices an engine against another engine.
The cross-engine question for these workloads has its twins already
(`tools/pg_benchmark.py` shapes); the one genuinely missing twin in this
family is still `tools/pg_order_by_benchmark.py`, named in
`bench/docs/README.md` as the task that would build it.

### 10.2 What a local statement pays, and why `cores` is the knob

The added block sits in `CommandDispatcher::HandleSelect`, after the
P4c single-step remote block (which both binaries have) and before
`CheckReadAffinity`. Its head term is `remote_reads_ != nullptr`, and
that pointer is set only inside `Expeditor::Serve`'s `config_.cores > 1`
branch. **With `cores = 1` the chain dies on its first predicate**, so a
run at the harness's default core count measures a single null compare
of a member already in cache — the cheapest possible reading of the
exposure, and not the honest one. `cores = 2` is therefore measured
beside it: there the pointer is non-null, the default `kCreatingCore`
placement leaves every relation the drivers create on core 0, every
statement stays local, and each dispatched SELECT walks the chain to its
third predicate before falling through. Both sides pay the second
reactor thread equally; it blocks in epoll for up to 10 ms when idle and
cost 80 ms of CPU over a 10 s idle probe.

Statically, the block is not free of consequence even when it is free of
cost: `HandleSelect`'s hot text grows **5,413 → 6,548 bytes** (+1,135,
+21%) between the two binaries, and its cold clone 364 → 422. That is a
layout-scale change to a large function, which §10.6 shows is the scale
of thing the long-scan arms actually resolve.

### 10.3 The point-statement table: `cores = 2`, 1,000 rows

`ac-select` (`SELECT amount FROM t WHERE id = N`, a one-step projected
SELECT) is the only arm in the mix that dispatches through the new
chain. `ac-insert`/`ac-update`/`ac-delete`/`ping`/`begin`/`commit`/
`rollback` **cannot reach it** — they are the in-run control group, and
the four relations (`none`, `twin` unasserted; `cnt`, `multi` asserted)
give a same-binary floor per arm. Latencies in µs, run `mix1k-c2-a`:

| arm | relation | side | ops | p0 | p25 | p50 | p75 | p95 | p99 |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| ac-select | none | head | 1000 | 25.1 | 34.8 | 37.2 | 41.7 | 57.5 | 77.4 |
| ac-select | none | base | 1000 | 25.2 | 34.7 | 37.3 | 42.4 | 55.9 | 75.3 |
| ac-select | twin | head | 1000 | 24.7 | 33.9 | 35.5 | 38.9 | 46.4 | 124.8 |
| ac-select | twin | base | 1000 | 25.1 | 33.8 | 36.0 | 39.4 | 46.1 | 55.7 |
| ac-select | cnt | head | 1000 | 24.9 | 33.9 | 35.8 | 39.6 | 47.1 | 77.5 |
| ac-select | cnt | base | 1000 | 25.1 | 33.2 | 35.6 | 39.8 | 48.7 | 60.5 |
| ac-select | multi | head | 1000 | 24.5 | 34.2 | 35.9 | 38.9 | 44.8 | 60.5 |
| ac-select | multi | base | 1000 | 25.2 | 31.9 | 35.8 | 39.6 | 47.2 | 57.7 |
| ac-insert (control) | none | head | 1000 | 23.1 | 31.5 | 32.8 | 35.7 | 48.9 | 65.2 |
| ac-insert (control) | none | base | 1000 | 23.3 | 31.6 | 32.8 | 35.1 | 47.6 | 65.3 |
| ac-update (control) | none | head | 1000 | 23.7 | 25.4 | 33.0 | 36.0 | 49.9 | 75.7 |
| ac-update (control) | none | base | 1000 | 24.4 | 25.8 | 33.0 | 36.1 | 49.8 | 70.3 |
| ac-delete (control) | none | head | 1000 | 23.1 | 31.2 | 32.6 | 34.8 | 48.0 | 61.1 |
| ac-delete (control) | none | base | 1000 | 23.1 | 31.7 | 33.0 | 35.5 | 48.1 | 64.7 |
| ping (control) | — | head | 500 | 22.4 | 28.6 | 29.6 | 30.6 | 38.4 | 45.7 |
| ping (control) | — | base | 500 | 21.4 | 21.7 | 22.3 | 29.8 | 35.8 | 44.3 |

The largest single p50 movement in that table is **+7.3 µs, on `ping`** —
`SHOW META`, which reaches no chain, no catalog and no step. It is the
one arm the driver measures as a contiguous per-side block rather than
interleaved per block, and it repeated at +6.4 and +7.4 in two more runs
before reading **−3.5** in the fourth. That is what a whole-block
scheduling artifact looks like on this box, and it is nearly 7× the
largest movement the exposed arm shows in any of the four runs
(|Δp50| ≤ 1.1 µs on all sixteen `ac-select` cells).

### 10.4 The result: exposed and control move together, at every configuration

Each cell below pools every relation and every repeat of one
configuration — head minus base, in µs, positive meaning head slower —
beside the **cross-run floor**, the median over cells of one binary's own
spread across the repeats of that configuration. Two slot orders (head
on the driver's first server, then head on the second) are inside every
pool, so an artifact of visit order shows as a sign flip rather than a
cost.

| driver | rows | cores | runs | group | Δp0 | Δp25 | Δp50 | Δp75 | Δp95 | Δp99 | n |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|
| mix | 200 | 1 | 2 | ac-select (pays) | −2.35 | −0.65 | −0.26 | +0.08 | +1.05 | +1.91 | 8 |
| mix | 200 | 1 | 2 | controls | −0.92 | +0.03 | +0.16 | +0.28 | +0.13 | −4.28 | 58 |
| mix | 200 | 1 | 2 | *cross-run floor* | 0.20 | 0.70 | 0.20 | 0.30 | 1.45 | 3.15 | |
| mix | 200 | 2 | 2 | ac-select (pays) | −0.26 | −0.58 | −0.31 | −0.62 | −0.81 | −1.25 | 8 |
| mix | 200 | 2 | 2 | controls | −0.14 | +1.43 | +0.28 | +0.18 | −0.19 | +0.58 | 58 |
| mix | 200 | 2 | 2 | *cross-run floor* | 0.15 | 1.25 | 0.20 | 0.25 | 0.95 | 3.25 | |
| mix | 1,000 | 1 | 4 | ac-select (pays) | −0.03 | −0.41 | −0.33 | −0.28 | −0.41 | +0.52 | 16 |
| mix | 1,000 | 1 | 4 | controls | −0.03 | +0.03 | −0.09 | −0.13 | −0.22 | −0.50 | 116 |
| mix | 1,000 | 1 | 4 | *cross-run floor* | 0.50 | 3.75 | 0.80 | 0.60 | 2.05 | 5.80 | |
| mix | 1,000 | 2 | 4 | ac-select (pays) | −0.15 | −1.14 | −0.37 | −0.46 | −0.59 | −0.10 | 16 |
| mix | 1,000 | 2 | 4 | controls | −0.05 | +0.12 | +0.09 | −0.10 | −0.09 | +0.35 | 116 |
| mix | 1,000 | 2 | 4 | *cross-run floor* | 0.30 | 4.60 | 0.60 | 0.65 | 1.80 | 8.75 | |
| mix | 10,000 | 1 | 2 | ac-select (pays) | −0.28 | −0.43 | −0.50 | −0.58 | −0.09 | +2.65 | 8 |
| mix | 10,000 | 1 | 2 | controls | −0.19 | −2.58 | −0.07 | +0.19 | +0.06 | +0.13 | 58 |
| mix | 10,000 | 1 | 2 | *cross-run floor* | 0.20 | 0.70 | 0.20 | 0.20 | 1.00 | 3.60 | |
| mix | 10,000 | 2 | 2 | ac-select (pays) | −0.21 | −0.18 | −0.33 | +0.01 | −0.16 | −2.08 | 8 |
| mix | 10,000 | 2 | 2 | controls | −0.05 | −0.66 | −0.09 | −0.09 | −0.23 | −0.37 | 58 |
| mix | 10,000 | 2 | 2 | *cross-run floor* | 0.20 | 1.15 | 0.40 | 0.30 | 1.15 | 2.45 | |
| scan | 200 | 1 | 2 | SELECT cells, both passes | +0.85 | +0.55 | +0.41 | +0.27 | +0.20 | +0.41 | 44 |
| scan | 200 | 1 | 2 | ping only (control) | −0.30 | −0.05 | −0.07 | −0.08 | −0.53 | −0.03 | 4 |
| scan | 200 | 1 | 2 | *cross-run floor* | 1.00 | 1.25 | 0.80 | 0.70 | 0.80 | 1.80 | |
| scan | 200 | 2 | 2 | SELECT cells, both passes | +0.71 | +0.88 | +0.98 | +1.34 | −1.33 | −6.41 | 44 |
| scan | 200 | 2 | 2 | ping only (control) | −1.53 | −2.08 | −0.20 | −0.05 | +0.58 | +0.47 | 4 |
| scan | 200 | 2 | 2 | *cross-run floor* | 0.70 | 7.25 | 6.10 | 3.50 | 457.50 | 870.20 | |
| scan | 1,000 | 1 | 2 | SELECT cells, both passes | −1.22 | −2.54 | −2.54 | −2.52 | −2.44 | +0.67 | 44 |
| scan | 1,000 | 1 | 2 | ping only (control) | +1.17 | +0.12 | +0.18 | +0.03 | −0.12 | +0.97 | 4 |
| scan | 1,000 | 1 | 2 | *cross-run floor* | 1.85 | 1.05 | 1.30 | 1.20 | 1.45 | 11.25 | |
| scan | 1,000 | 2 | 2 | SELECT cells, both passes | +2.68 | +3.31 | +4.36 | +5.23 | +7.31 | +8.08 | 44 |
| scan | 1,000 | 2 | 2 | ping only (control) | +0.10 | −0.35 | −0.05 | +0.08 | −0.10 | +0.10 | 4 |
| scan | 1,000 | 2 | 2 | *cross-run floor* | 2.80 | 2.15 | 1.50 | 2.40 | 2.45 | 9.45 | |
| scan | 10,000 | 1 | 4 | SELECT cells, both passes | −30.67 | −29.07 | −45.97 | −30.56 | −49.79 | +22.75 | 88 |
| scan | 10,000 | 1 | 4 | ping only (control) | +0.02 | −0.70 | −0.02 | +0.16 | +1.16 | +47.30 | 8 |
| scan | 10,000 | 1 | 4 | *cross-run floor* | 27.95 | 26.85 | 65.95 | 550.70 | 470.75 | 1153.90 | |
| scan | 10,000 | 2 | 4 | SELECT cells, both passes | +11.17 | +10.48 | +10.02 | +15.24 | +18.44 | +26.48 | 88 |
| scan | 10,000 | 2 | 4 | ping only (control) | +0.10 | +0.25 | −0.21 | −0.09 | −0.62 | +0.93 | 8 |
| scan | 10,000 | 2 | 4 | *cross-run floor* | 47.75 | 39.65 | 39.20 | 75.65 | 71.00 | 155.40 | |

Three readings, in order of how much weight they carry:

- **The p0 instrument, which is the one that can resolve a fixed
  per-statement cost, says no.** `ac-select`'s Δp0 is −0.26, −0.15 and
  −0.21 µs at 200 / 1,000 / 10,000 rows with `cores = 2` — negative at
  every size, against a floor of 0.15–0.30 µs and controls at −0.14 to
  −0.05. This is the same instrument that resolved a clean 16-for-16
  **+0.3 to +0.8 µs** signature at `0fd7fc3` (§3), so its sensitivity is
  established inside this file. Three predicates on a modern core are
  single-digit nanoseconds; the instrument's floor is ~300 ns. The
  finding is a null, and it is a null with two orders of headroom.
- **The exposed group and the control group are indistinguishable.**
  Wherever the SELECT arms drift, the arms that cannot reach the new
  code drift with them (mix 200 `cores = 1`: exposed Δp0 −2.35, controls
  −0.92; scan 1,000 `cores = 1`: exposed −2.54 at p50, ping +0.18).
  A cost that existed would have to show up in the first group and not
  the second, and it does not.
- **The 10,000-row scan cells contradict each other and are worthless as
  evidence.** `cores = 1` says head is **45.97 µs faster** at p50;
  `cores = 2` says head is **10.02 µs slower** — same two binaries, one
  config apart, and the floor column in both cases is larger than the
  effect. §10.6 shows where those numbers actually come from.

### 10.5 The sweep: 200, 1,000 and 10,000 rows

The check is per statement, so a per-row shape would be its refutation —
this is the axis that separates them. Two arms carry it, one from each
driver, at `cores = 2` (the config where the chain is entered), run `a`
of each size, in µs:

| arm | rows | pages/examined | side | ops | p0 | p25 | p50 | p75 | p95 | p99 |
|---|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| ac-select | 200 | 1 row, pk descent | head | 200 | 25.2 | 35.7 | 37.4 | 41.3 | 57.3 | 74.8 |
| ac-select | 200 | | base | 200 | 25.1 | 34.2 | 36.1 | 41.0 | 58.9 | 72.3 |
| ac-select | 1,000 | 1 row, pk descent | head | 1000 | 25.1 | 34.8 | 37.2 | 41.7 | 57.5 | 77.4 |
| ac-select | 1,000 | | base | 1000 | 25.2 | 34.7 | 37.3 | 42.4 | 55.9 | 75.3 |
| ac-select | 10,000 | 1 row, pk descent | head | 1000 | 24.9 | 34.4 | 35.9 | 39.8 | 55.0 | 70.2 |
| ac-select | 10,000 | | base | 1000 | 25.2 | 34.4 | 35.9 | 40.1 | 52.7 | 61.3 |
| pk-point | 200 | 1 row, 1 page | head | 6000 | 27.2 | 29.4 | 30.1 | 33.2 | 59.7 | 711.7 |
| pk-point | 200 | | base | 6000 | 26.9 | 29.0 | 29.7 | 33.0 | 55.1 | 811.4 |
| pk-point | 1,000 | 1 row, 1 page | head | 2000 | 27.4 | 36.1 | 37.8 | 39.4 | 48.7 | 61.5 |
| pk-point | 1,000 | | base | 2000 | 27.5 | 36.2 | 37.8 | 39.6 | 48.7 | 60.3 |
| pk-point | 10,000 | 1 row, 1 page | head | 400 | 29.9 | 39.1 | 40.5 | 42.7 | 59.1 | 79.5 |
| pk-point | 10,000 | | base | 400 | 29.3 | 37.5 | 39.5 | 42.0 | 54.6 | 77.6 |
| plain (rendered scan) | 200 | 200 rows, 3 pages | head | 6000 | 71.8 | 75.5 | 78.8 | 88.1 | 603.4 | 981.9 |
| plain | 200 | | base | 6000 | 70.0 | 73.4 | 76.4 | 85.6 | 579.9 | 923.0 |
| plain | 1,000 | 1,000 rows, 15 pages | head | 2000 | 269.5 | 286.3 | 294.4 | 303.2 | 325.2 | 411.3 |
| plain | 1,000 | | base | 2000 | 259.9 | 274.4 | 280.1 | 287.8 | 299.8 | 346.7 |
| plain | 10,000 | 10,000 rows, 145 pages | head | 400 | 2848.2 | 2928.1 | 3020.6 | 3468.4 | 4096.1 | 4209.7 |
| plain | 10,000 | | base | 400 | 2950.3 | 3057.3 | 3160.8 | 3785.7 | 4188.5 | 4393.0 |

The mapping from flag to row count is direct on both drivers: `--rows R`
**is** R rows loaded, and the scan driver's `--ops` defaults to 6000 /
2000 / 400 per arm at 200 / 1,000 / 10,000 so each arm still clears
`/proc`'s jiffy; the mix's `--ordinary-ops` was 200 / 1,000 / 1,000 (the
driver caps it at `--rows`). `pk-point` and `ac-select` are shapes that
by construction **do not scale with rows** — one row through one pk
descent — and they are shown at all three sizes as the evidence of it:
their p50s move 30.1 → 37.8 → 40.5 µs across the sweep on head and
29.7 → 37.8 → 39.5 on base, which is the tree getting deeper and the
relation colder, identically on both sides. `plain` is the per-row
counterpart at 78.8 → 294.4 → 3020.6 µs. Against that 38× span of
per-statement work, the head-base delta stays inside ±1.3 µs on the
point arms and changes **sign** on the scan arms — the signature of a
constant that is zero, not of a per-row cost.

### 10.6 The null control: this harness invents ±55 µs on a 10k scan

The six extra runs put **one binary on both servers** — same build, same
config, two fresh data files, one driver — and ask what the instrument
reports when there is provably nothing to report. Δp50 in µs, "head"
and "base" here being two processes of the identical build:

| control run | cores | an-plain (ab) | an-plain (feat) | an-nonpk | plain | nonpk | ping | pk-point | plain-lim |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| head vs head, a | 1 | −3.90 | −3.40 | −39.50 | +1.10 | −40.70 | −0.50 | +0.40 | +0.50 |
| base vs base, a | 1 | +5.10 | +2.90 | −32.60 | +122.30 | −26.80 | −0.40 | −0.30 | −0.30 |
| head vs head, a | 2 | +24.70 | +14.00 | +5.10 | +54.60 | +41.30 | −0.80 | +0.70 | −0.30 |
| head vs head, b | 2 | −9.80 | −14.80 | −13.90 | +32.90 | −6.20 | +0.10 | −0.20 | −1.50 |
| base vs base, a | 2 | +2.00 | −18.10 | −16.20 | −9.00 | −58.30 | 0.00 | +0.80 | −0.30 |
| base vs base, b | 2 | +55.20 | −6.20 | −4.70 | +231.80 | +40.10 | −0.10 | +0.60 | −0.50 |

This is the section's most transferable result. The `an-plain` arm —
the tight ANALYZE walk that §2 used to price the coroutine conversion at
8 ns/row, and §8 to bound the page loop at ≤15 ns/page — produces
**+55.2 µs** of p50 delta between two processes of one binary. `plain`
produces **+231.8**. The real A/B's most eye-catching cell (`an-plain`
at `cores = 2`, +39.5 to +86.2 µs, positive in all four runs) sits
squarely inside that, and its sign reverses at `cores = 1` (−1.9 to
−22.7 across four runs) with the binaries unchanged — so it is not the
commit. Two things follow, and the second matters beyond this section:

- For **this** measurement: nothing on a 10,000-row arm is admissible
  evidence either way, and the null stands on the point arms and the p0
  column, where the same control reads ±1.5 µs at worst.
- For **the file**: a per-row or per-page cost read off `an-plain`
  without a same-binary control beside it is being read against a floor
  that this run measures at ±55 µs, i.e. ±5.5 ns/row at 10,000 rows.
  §2's 8 ns/row survives that only because it reproduced across passes
  *and* moved the `/proc` CPU column; §9's −17.9/−21.3 µs "head-faster"
  reading, which that section already declined to attribute, is inside
  this floor outright.

### 10.7 Where the time waits

Per the accounting rule, every named wait type either appears with its
share or is stated absent. The measured unit is one client-visible
statement on one connection per side.

| wait type | mix point arms (~32–37 µs) | scan short arms (~30–42 µs) | 10k scan arms (1.1–6.1 ms) |
|---|---|---|---|
| client + socket round trip | ~22–29 µs of every statement — the `ping` floor, the dominant share | ~19–27 µs — the dominant share | ~27 µs — under 1% |
| durability / commit fsync | **0** — `durability = relaxed` puts no fsync on any measured path; the mix's own COMMIT arm reaches no device sync | **0** — read-only | **0** — read-only |
| read (device) I/O | 0 — relations resident (3 to 145 pages) | 0 — resident | 0 — 145 pages resident |
| lock / conflict wait | 0 — one connection per side, no concurrent writer | 0 | 0 |
| write-statement work | present on the control arms only (INSERT/UPDATE/DELETE); the exposed arm is a read | n/a | n/a |
| server CPU (where any delta would have to be) | the residual ~7–12 µs; **no A/B delta resolvable in it** | the residual ~5–15 µs; same | effectively the whole statement; the `/proc` meter is sign-mixed within its 25 µs quantum on every arm (e.g. `plain` head 2100 vs base 2250 µs/op, `plain-again` head 2325 vs base 2200) |

The new work is three predicates of pure CPU. It appears in no wait
class because it appears nowhere: there is no wait type on today's
engine that could carry it and no instrument here that resolves it.

### 10.8 What this settles, and what it explicitly does not

- **The local path is unchanged in price by P4d-4b-3, at every size
  measured.** The 4b-3 session side taxes the workload KDS is
  specialized for by nothing this run can resolve, at `cores = 1` (where
  the chain is a null compare) or at `cores = 2` (where it is three
  predicates). Bound: **≤ ~0.3 µs per statement** from the p0 instrument
  that has resolved 0.3 µs before in this same file, with the measured
  sign negative.
- **The cross-core path itself is not priced here, by construction.**
  Every relation in every run was owned by the session's core; no
  statement shipped. The cost the workplan carries by name — *the
  per-input-row runner cost*, one `ExecuteAsync` frame plus a `Bind` and
  a frame `Open` per input row, the shape `95946c4` removed locally and
  4b-3 reintroduced one level up (`docs/workplan-crosscore.md`, P4d-4b-3
  and P4e) — is untouched by this section and remains P4e's to measure.
  Nothing here should be quoted as evidence about a pipeline.
- **One local shape walks deeper than three predicates and is
  unpriced.** A *local two-step projected join with a probe inner* —
  `SELECT c.id, p.x FROM child c JOIN parent p ON c.parent_id = p.id
  WHERE c.id = N`, the shape `tools/join_benchmark.py` drives — passes
  the class test's shape gate at `f2f101d` and then pays **two
  `catalog_.InitTableAccess` lookups** (warm hash-map hits) before the
  owner comparison sends it back to the local path. No documented driver
  can interleave an A/B on a join shape inside one run, which is the
  method this whole file rests on; measuring it sequentially would put a
  ~100 ns question against a whole-run drift the mix driver's own
  docstring measures at 26%. The task that would close it is an
  `--ab-port` for `join_benchmark.py`, or a join arm in
  `order_by_benchmark.py`, either of which inherits the interleave for
  free. Stated as unmeasured rather than assumed null.
- **`a45a4d4` does not disturb this section, and does not extend it.**
  That commit moves the shape rules out of the dispatcher into
  `BuildTwoStepPipeline`, leaving `remote_reads_ != nullptr && !analyze
  && chain.steps.size() == 2` in `HandleSelect` — for a **one-step**
  SELECT, the shape measured here, the same three predicates in the same
  order, so this null carries over unchanged. What it does change is the
  local *two-step* path above: at `a45a4d4` every two-step chain takes
  the two catalog lookups before any shape test, where `f2f101d` reached
  them only for a probe-inner projected join. That widens the one
  unpriced local shape rather than narrowing it.
  **Closed at `51418a8`, on this finding.** The eligible class split into
  a chain-only `TwoStepPipelineEligible` — no catalog lookup, so the
  dispatcher asks it *before* resolving any schema — with
  `BuildTwoStepPipeline` calling it too, so the rule keeps one home. The
  ordering `f2f101d` measured is restored and the shape gate is now
  strictly cheaper than it was there: a two-step chain that will never
  ship is refused without touching the catalog at all. Still unpriced,
  for the interleaving reason above; the widening is simply no longer
  there to price.
- **The instrument learned something the engine did not.** The
  same-binary control (§10.6) is the first one in this file, and it
  retires the 10,000-row rendered and ANALYZE arms as evidence for
  deltas under ~50 µs. Every future entry in this series should carry
  one: it costs two runs and it is the difference between "head is 1.5%
  slower" and "this harness produces 1.8% from nothing".

### 10.9 Row-set sweep status and artifacts

All three documented sizes ran on **both** drivers and **both** core
counts: 200 / 1,000 / 10,000 rows, `--rows` mapping directly to the row
count on each, plus the scan driver's own 1-row (`pk-point`) and 20-row
(`--limit 20`) arms inside every run — 32 A/B runs in all: **four** at
the mix's 1,000-row cell and four at the scan's 10,000-row cell (two
slot orders × two repeats each, per core count), **two** at every other
cell, plus 6 same-binary controls. This closes the 200-row cell §7
recorded as missing, for the mix and the scan alike.

Raw artifacts, one JSON and one console log per run:
`/home/cdkbs/bench-p4d4b3/` — `mix{200,1k,10k}-c{1,2}-{a..d}.json`,
`scan{200,1k,10k}-c{1,2}-{a..d}.json`,
`self{head,base}10k-c{1,2}-{a,b}.json`, the schedule
(`schedule.sh`, `schedule.log`), the launchers and the reduction scripts
(`aggregate.py`, `summary.py`, `perarm.py`, and their `.txt` output).
Each run's `kds.conf` and server log are kept beside them in
`<tag>-{head,base}/`; the data files and WAL segments themselves were
deleted after the run (8.6 GB of them), since a fresh one is mandatory
per side per run and none of them is reusable evidence.
