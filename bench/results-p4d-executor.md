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
