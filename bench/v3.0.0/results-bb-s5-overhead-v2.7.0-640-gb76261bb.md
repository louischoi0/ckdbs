# BB-S5 overhead (v2.7.0-640-gb76261bb)

**Status: BB closed on this measurement (BD-Q12 (b), the operator's word of
2026-10-08, `raft-marks-2026-10-08.md` §6).** On 2026-10-07 the operator
deferred the rest and this file was pushed in part at `dbeb876c`. The
detached job kept running and finished every cell it had been given: C4's
last four runs, C5, and pinned `cores = 2` C3 and C4. Those runs were
recorded at BB's close on `worktree-bb-s5-close`, from the archive as the
job left it. They were not run again. No wait breakdown was taken (below).

**What this measures.** It measures BB's code, not the engine on `main`
today. BD (`workorder-bd-sorted-leaf-named-keys.md`) later withdrew BB-R1,
BB-R2 and BB-R3's below-mark refusal on a btree, so an insert no longer
issues its id under the leaf's hold. BD-R10 waived BD's own measurement.

- **A** = `bddd450c` (`v2.7.0-622-gbddd450c`), Release - BB's opening commit.
- **B** = `b76261bb` (`v2.7.0-640-gb76261bb`), Release - BB's code at
  BB-S4's review. BB-S5's own rows are documents only.
- Binaries' sha256: A `2d7e6450...4e45c22c`, B `3803228d...76e93`
  (`archive/bb-s5-overhead-v2.7.0-640-gb76261bb/serial-start.txt`).
- Method: the AP-S5 file's (`tools/ap_overhead_benchmark.py`), driven by
  `tools/bb_overhead_benchmark.py` (serial), `tools/bb_concurrent_benchmark.py`
  (C3/C4) and `tools/bb_orderby_benchmark.py` (C5). Each (cell, run) gets a
  fresh server and data file, and the arms alternate which goes first. Each
  cell has a repeated arm (`insert-again`, `limit-again`) and a control arm
  (`ping`) for the noise floor. Durability is `relaxed`. `/proc/loadavg` and
  competing processes are recorded before and after each run. Raw JSON and
  logs: `archive/bb-s5-overhead-v2.7.0-640-gb76261bb/`.
- Host: 8 CPUs. No build ran during the serial cells, C3 or C5. During C4
  at `cores = 8`, another worktree's build and test run did: a `cmake
  --build` across A's run 5, and a `ctest` across B's runs 5 and 6 (each
  run's `host_before`/`host_after`).

## The `cores = 1` gate - C1, C1s, C2: no cost resolved

The pinned series (`--pin-server 2`, driver under `taskset -c 4`), 12 runs
per row count, pooled `insert` + `insert-again` p50 delta B - A with a
bootstrap 95% interval:

| Cell | rows | dB-A p50 (us) | CI95 | % | Resolved cost of B? |
|---|---|---|---|---|---|
| C1 omitted pk | 200 | +0.05 | [-0.05, +0.23] | +0.10% | no |
| C1 | 1,000 | +0.03 | [-0.27, +0.27] | +0.05% | no |
| C1 | 10,000 | +0.03 | [-0.08, +0.18] | +0.05% | no |
| C1s spilled value | 200 | -0.10 | [-0.33, +0.05] | -0.19% | no |
| C1s | 1,000 | -0.33 | [-0.75, -0.15] | -0.61% | no - B faster |
| C1s | 10,000 | -0.08 | [-0.45, +0.37] | -0.14% | no |
| C2 named key | 200 | +0.00 | [-0.12, +0.05] | +0.00% | no |
| C2 | 1,000 | -0.03 | [-0.10, +0.15] | -0.05% | no |
| C2 | 10,000 | -0.05 | [-0.18, +0.12] | -0.10% | no |

**At `cores = 1` the A/B resolves no cost of BB.** Every interval contains
zero except C1s at 1,000 rows, where B is faster by 0.33 us. That is one
cell of nine, unexplained, and not claimed as a gain. The control arm
(`ping`), which BB does not touch, moves by -2.2 to +0.7 us across the same
runs, so the noise floor is of that order. An unpinned series (`default`,
10 runs) was also taken. Every interval there straddles zero but is ~10x
wider (up to +/-12 us): it measures the scheduler, not the engine, and
decides nothing.

`recovery_checkpoint_us` on a fresh file ran 7.1-24.7 ms (median ~8.4 ms):
no device stall flagged.

## `cores > 1` - C3 and C4, 10 runs each

Medians over runs; latencies in us. `cores = 2` is pinned: the server under
`taskset -c 0,2` (core 1's reactor pins itself to CPU 1), four clients on
CPUs 4-7, so C4 there inserts into four relations, not eight. The busiest
client ran at a median 0.89-0.99 of its CPU, so the `cores = 2` rows are
close to the driver's own ceiling. `cores = 8` is not pinned apart: the reactors and 8 client
processes share the 8 CPUs, so those rows are indicative only. *spread* is
the runs whose sessions landed on more than one core; the kernel's
SO_REUSEPORT hash places them (BA's P11), so a run's shape varies.

| Cell | cores | arm | rows/s | p0 | p25 | p50 | p95 | p99 | refusals | spread |
|---|---|---|---|---|---|---|---|---|---|---|
| C3 one relation | 2 | A | 45,160 | 39.5 | 64.0 | 74.0 | 100.3 | 123.3 | 0 | 8/10 |
| C3 | 2 | B | 44,620 | 36.8 | 61.5 | 72.8 | 101.3 | 123.8 | 0 | 9/10 |
| C4 four relations | 2 | A | 54,065 | 40.9 | 54.8 | 61.5 | 92.0 | 111.5 | 0 | 8/10 |
| C4 | 2 | B | 49,548 | 39.5 | 60.1 | 69.8 | 94.5 | 123.6 | 0 | 9/10 |
| C3 one relation | 8 | A | 62,296 | 39.9 | 79.3 | 96.4 | 182.6 | 247.8 | 1.5 | 10/10 |
| C3 | 8 | B | 63,041 | 36.9 | 80.3 | 95.2 | 178.2 | 246.9 | 0 | 10/10 |
| C4 eight relations | 8 | A | 62,297 | 35.8 | 74.4 | 90.8 | 177.1 | 250.3 | 0 | 10/10 |
| C4 | 8 | B | 63,429 | 40.6 | 74.6 | 90.7 | 176.8 | 253.1 | 0 | 10/10 |

Paired by run (B's run r minus A's run r), with a bootstrap 95% interval:

| Cell | cores | dB-A p50 (us) | CI95 | dB-A rows/s | CI95 |
|---|---|---|---|---|---|
| C3 | 2 | -4.7 | [-7.7, +9.2] | +1,256 | [-7,451, +3,419] |
| C4 | 2 | -0.7 | [-5.6, +15.3] | -3,749 | [-13,475, +2,386] |
| C3 | 8 | -0.9 | [-10.2, +3.5] | -222 | [-3,061, +3,814] |
| C4 | 8 | -1.7 | [-8.2, +9.5] | +2,357 | [-5,063, +7,457] |

**No cell resolves a cost of BB at `cores > 1`.** Every paired interval
contains zero. C4 at `cores = 2` is the one cell whose medians lean against
B: p50 61.5 -> 69.8 us, and 54,065 -> 49,548 rows/s. Its paired median is
-0.7 us, but its interval is the widest of the four (up to +15.3 us). It is
recorded as unresolved, not as a pass, and the cause was not looked for: no
wait breakdown was taken. C3's refusal median at `cores = 8` falls from 1.5
to 0 (`TXN_CONFLICT retryable=1`, retried). With ten runs that is a
direction, not a resolved difference. At `cores = 8`, C4's run 5 ran under
the other worktree's build and test run on both arms, at a host load of about
4 to 6 (A 21,478 rows/s, B 35,215), and B's run 6 under its test run. They
are kept. Without runs 5 and 6 the paired p50 is -1.7 us [-7.1, +5.5] and
the paired rows/s +2,357 [-1,637, +4,261]: still nothing resolved.

## C5 - `ORDER BY <pk>` over a relation `kUnordered` at A

`bb_u (id int64, v int64) BTREE`, 10,000 rows in 500-row batches of named
keys. A loads them descending, so the relation turns `kUnordered` and its
walk sorts each page. B loads the same keys ascending, because B refuses a
key below the mark. 10 runs; per arm, medians of each run's percentiles:

| arm | A p0 | p25 | p50 | p95 | p99 | B p0 | p25 | p50 | p95 | p99 | dB-A p50 | CI95 | % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `LIMIT 100` | 252.8 | 257.9 | 261.2 | 276.5 | 301.9 | 249.1 | 254.7 | 257.8 | 271.8 | 289.1 | -3.4 | [-4.2, -3.3] | -1.32% |
| `LIMIT 100` again | 252.3 | 257.7 | 261.2 | 277.9 | 302.1 | 249.4 | 255.0 | 258.1 | 273.8 | 295.1 | -3.1 | [-3.7, -2.9] | -1.21% |
| full, 10,000 rows | 19,771 | 19,971 | 20,081 | 21,718 | 23,544 | 19,590 | 19,803 | 19,898 | 21,509 | 21,873 | -186 | [-209, -165] | -0.93% |
| `ping` (control) | 38.4 | 40.9 | 41.3 | 52.8 | 60.2 | 38.7 | 40.8 | 41.2 | 52.0 | 60.5 | -0.1 | [-0.2, -0.0] | -0.24% |

**What B gives back on this cell is resolved and small: about 1%.** That is
3.4 us on `LIMIT 100` and 186 us over 10,000 rows. The two arms are loaded in
opposite orders by construction, so the delta is BB-R10's walk together with
whatever layout difference the two load orders leave; the cell does not
split them. The first `LIMIT 100` reply and the first full reply were
identical between A and B on every run (row set and order). On the first
run B was also sent a descending pair on a probe relation (`500`, then
`499`), and refused `499` as BB-R3 then did: *"primary key 499 is below
relation 4003's high-water mark 501"*. BD has since withdrawn that refusal
on a btree.

## Not executed

- **No wait breakdown** for any cell. The per-run `SHOW META` deltas
  (`wal_syncs`, ring-full, foreground polls) are in each run's JSON and are
  not summarised here.
- **No pinned `cores = 8` cell.** The host has 8 CPUs, so the reactors and
  the clients cannot be pinned apart.

## Insight

BB moved the id issue under the leaf's (or tail's) exclusive hold. At one
core the placement already took that hold, so the issue, the borrow and the
encode only moved inside a span that existed. That is why no cost resolves
there. At `cores > 1` the hold is now held longer, and no cell resolves a
loss either. The one cell that leans against B is C4 at `cores = 2`, where
the inserts share no leaf, so what BB changed that they still share is the
`sys.tables` chain (the reason BB-R8 asked for the cell). A cost there would be
what BB-S1's census priced (`ForFirstRow` holds each `sys.tables` chain page
`X` up to the row). It is unresolved at ten runs. BA's census (BA-S2..S4)
is where it gets a wait breakdown. The engine that census measures has BD's
shape: BD-R6 took the issue back out from under the leaf.

## Raw analysis - pinned series

```
series pinned, exclude after-load > 1.7 or competing build

## c1 rows=200 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 3200", "B": "count(*) 3200"}'}
 insert        A p50=  49.20 B p50=  49.20 dB-A median= +0.05 IQR[-0.03,+0.20] CI95[-0.05,+0.20] pos=6/12  mean-us A=51.35 B=51.10
   engine us/op dB-A median=-0.10
 insert-again  A p50=  49.10 B p50=  49.10 dB-A median= +0.05 IQR[-0.10,+0.12] CI95[-0.10,+0.15] pos=6/12  mean-us A=53.65 B=53.05
   engine us/op dB-A median=-0.41
 ping          A p50=  38.20 B p50=  38.00 dB-A median= +0.05 IQR[-0.28,+0.22] CI95[-0.35,+0.25] pos=6/12  mean-us A=40.70 B=40.55
   engine us/op dB-A median=-0.19
 pooled(insert,again) dB-A median=+0.05 CI95[-0.05,+0.23] pct=+0.10%
  A insert p0=41.8 p25=48.4 p50=49.2 p95=63.5 p99=77.6 mean=51.3
  B insert p0=41.6 p25=48.5 p50=49.2 p95=62.3 p99=74.5 mean=51.1
 recovery_checkpoint_us on fresh file: min=7199.0 median=8295.5 max=17524.0

## c1 rows=1000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 4000", "B": "count(*) 4000"}'}
 insert        A p50=  49.20 B p50=  49.35 dB-A median= +0.05 IQR[-0.07,+0.23] CI95[-0.15,+0.25] pos=6/12  mean-us A=51.65 B=51.90
   engine us/op dB-A median=-0.15
 insert-again  A p50=  49.15 B p50=  49.25 dB-A median= +0.00 IQR[-0.13,+0.20] CI95[-0.15,+0.20] pos=5/12  mean-us A=55.45 B=53.50
   engine us/op dB-A median=-0.29
 ping          A p50=  38.20 B p50=  38.35 dB-A median= -0.05 IQR[-0.50,+0.55] CI95[-0.50,+0.70] pos=6/12  mean-us A=40.65 B=40.50
   engine us/op dB-A median=+0.28
 pooled(insert,again) dB-A median=+0.03 CI95[-0.27,+0.27] pct=+0.05%
  A insert p0=41.9 p25=48.3 p50=49.2 p95=63.6 p99=76.2 mean=51.7
  B insert p0=41.7 p25=48.5 p50=49.3 p95=62.5 p99=74.2 mean=51.9
 recovery_checkpoint_us on fresh file: min=7132.0 median=8434.0 max=17587.0

## c1 rows=10000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 13000", "B": "count(*) 13000"}'}
 insert        A p50=  49.20 B p50=  49.25 dB-A median= +0.00 IQR[-0.03,+0.23] CI95[-0.05,+0.25] pos=5/12  mean-us A=51.25 B=52.30
   engine us/op dB-A median=-0.07
 insert-again  A p50=  49.00 B p50=  49.00 dB-A median= +0.00 IQR[-0.13,+0.03] CI95[-0.15,+0.05] pos=3/12  mean-us A=53.85 B=53.60
   engine us/op dB-A median=-0.16
 ping          A p50=  38.00 B p50=  38.35 dB-A median= +0.20 IQR[-0.03,+0.42] CI95[-0.05,+0.45] pos=8/12  mean-us A=40.95 B=41.00
   engine us/op dB-A median=+0.09
 pooled(insert,again) dB-A median=+0.03 CI95[-0.08,+0.18] pct=+0.05%
  A insert p0=41.7 p25=48.5 p50=49.2 p95=64.0 p99=78.6 mean=51.2
  B insert p0=42.2 p25=48.3 p50=49.2 p95=65.4 p99=76.5 mean=52.3
 recovery_checkpoint_us on fresh file: min=7107.0 median=8497.5 max=24716.0

## c1s rows=200 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 3200", "B": "count(*) 3200"}'}
 insert        A p50=  52.00 B p50=  51.80 dB-A median= -0.15 IQR[-0.42,+0.00] CI95[-0.45,+0.00] pos=2/12  mean-us A=55.20 B=54.50
   engine us/op dB-A median=-0.21
 insert-again  A p50=  52.25 B p50=  51.75 dB-A median= -0.15 IQR[-0.50,+0.10] CI95[-0.50,+0.10] pos=4/12  mean-us A=58.70 B=57.05
   engine us/op dB-A median=-0.42
 ping          A p50=  38.30 B p50=  38.00 dB-A median= -0.25 IQR[-1.65,-0.07] CI95[-2.20,-0.05] pos=2/12  mean-us A=42.00 B=41.75
   engine us/op dB-A median=-0.05
 pooled(insert,again) dB-A median=-0.10 CI95[-0.33,+0.05] pct=-0.19%
  A insert p0=46.8 p25=51.2 p50=52.0 p95=67.2 p99=82.2 mean=55.2
  B insert p0=46.4 p25=51.2 p50=51.8 p95=67.1 p99=81.3 mean=54.5
 recovery_checkpoint_us on fresh file: min=7265.0 median=9181.0 max=14670.0

## c1s rows=1000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 4000", "B": "count(*) 4000"}'}
 insert        A p50=  53.15 B p50=  52.10 dB-A median= -0.60 IQR[-0.90,-0.35] CI95[-0.90,-0.30] pos=2/12  mean-us A=56.05 B=54.70
   engine us/op dB-A median=-0.09
 insert-again  A p50=  53.20 B p50=  53.05 dB-A median= -0.50 IQR[-0.77,+0.15] CI95[-0.85,+0.20] pos=4/12  mean-us A=60.70 B=59.20
   engine us/op dB-A median=-1.76
 ping          A p50=  44.25 B p50=  43.55 dB-A median= -0.35 IQR[-0.70,+0.13] CI95[-0.70,+0.15] pos=4/12  mean-us A=44.65 B=43.90
   engine us/op dB-A median=-0.09
 pooled(insert,again) dB-A median=-0.33 CI95[-0.75,-0.15] pct=-0.61%
  A insert p0=46.2 p25=52.2 p50=53.2 p95=68.7 p99=82.8 mean=56.0
  B insert p0=46.0 p25=51.4 p50=52.1 p95=67.6 p99=79.7 mean=54.7
 recovery_checkpoint_us on fresh file: min=7003.0 median=7927.5 max=16766.0

## c1s rows=10000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 13000", "B": "count(*) 13000"}'}
 insert        A p50=  54.75 B p50=  55.00 dB-A median= -0.10 IQR[-0.42,+0.23] CI95[-0.45,+0.25] pos=4/12  mean-us A=57.55 B=57.75
   engine us/op dB-A median=-0.18
 insert-again  A p50=  54.95 B p50=  55.05 dB-A median= -0.20 IQR[-0.33,+0.32] CI95[-0.35,+0.35] pos=4/12  mean-us A=61.80 B=61.00
   engine us/op dB-A median=-0.67
 ping          A p50=  38.15 B p50=  38.40 dB-A median= +0.20 IQR[-0.33,+0.40] CI95[-0.35,+0.40] pos=7/12  mean-us A=40.80 B=41.60
   engine us/op dB-A median=-0.09
 pooled(insert,again) dB-A median=-0.08 CI95[-0.45,+0.37] pct=-0.14%
  A insert p0=49.0 p25=54.1 p50=54.8 p95=71.8 p99=87.1 mean=57.5
  B insert p0=49.0 p25=54.4 p50=55.0 p95=71.9 p99=85.4 mean=57.8
 recovery_checkpoint_us on fresh file: min=7031.0 median=8745.0 max=316859.0

## c2 rows=200 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 3200", "B": "count(*) 3200"}'}
 insert        A p50=  49.45 B p50=  49.65 dB-A median= +0.10 IQR[-0.20,+0.10] CI95[-0.30,+0.10] pos=7/12  mean-us A=51.70 B=52.30
   engine us/op dB-A median=-0.13
 insert-again  A p50=  49.30 B p50=  49.30 dB-A median= +0.00 IQR[-0.02,+0.15] CI95[-0.05,+0.20] pos=4/12  mean-us A=53.80 B=53.80
   engine us/op dB-A median=-0.44
 ping          A p50=  38.10 B p50=  37.90 dB-A median= -0.10 IQR[-0.60,+0.10] CI95[-0.70,+0.10] pos=5/12  mean-us A=40.50 B=39.95
   engine us/op dB-A median=-0.23
 pooled(insert,again) dB-A median=+0.00 CI95[-0.12,+0.05] pct=+0.00%
  A insert p0=42.5 p25=48.7 p50=49.5 p95=64.4 p99=76.3 mean=51.7
  B insert p0=42.4 p25=48.8 p50=49.7 p95=64.2 p99=77.3 mean=52.3
 recovery_checkpoint_us on fresh file: min=7192.0 median=8560.5 max=21638.0

## c2 rows=1000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 4000", "B": "count(*) 4000"}'}
 insert        A p50=  49.20 B p50=  49.30 dB-A median= -0.05 IQR[-0.20,+0.20] CI95[-0.20,+0.20] pos=4/12  mean-us A=51.40 B=51.15
   engine us/op dB-A median=-0.12
 insert-again  A p50=  49.25 B p50=  49.30 dB-A median= +0.10 IQR[-0.12,+0.12] CI95[-0.15,+0.15] pos=7/12  mean-us A=53.50 B=53.25
   engine us/op dB-A median=-0.42
 ping          A p50=  38.10 B p50=  38.00 dB-A median= -0.10 IQR[-0.35,+0.03] CI95[-0.40,+0.05] pos=3/12  mean-us A=40.50 B=40.15
   engine us/op dB-A median=-0.10
 pooled(insert,again) dB-A median=-0.03 CI95[-0.10,+0.15] pct=-0.05%
  A insert p0=42.1 p25=48.5 p50=49.2 p95=63.6 p99=75.8 mean=51.4
  B insert p0=42.0 p25=48.5 p50=49.3 p95=63.5 p99=74.8 mean=51.1
 recovery_checkpoint_us on fresh file: min=7057.0 median=8380.5 max=23669.0

## c2 rows=10000 runs=12 kept=12 excluded=[]
errors=0 counts={'{"A": "count(*) 13000", "B": "count(*) 13000"}'}
 insert        A p50=  49.25 B p50=  49.25 dB-A median= +0.00 IQR[-0.23,+0.02] CI95[-0.25,+0.05] pos=3/12  mean-us A=51.85 B=51.75
   engine us/op dB-A median=-0.12
 insert-again  A p50=  49.15 B p50=  49.15 dB-A median= -0.10 IQR[-0.13,+0.05] CI95[-0.15,+0.10] pos=3/12  mean-us A=55.25 B=54.20
   engine us/op dB-A median=-0.37
 ping          A p50=  41.05 B p50=  38.05 dB-A median= +0.00 IQR[-0.35,+0.20] CI95[-0.50,+0.20] pos=5/12  mean-us A=41.65 B=42.35
   engine us/op dB-A median=-0.05
 pooled(insert,again) dB-A median=-0.05 CI95[-0.18,+0.12] pct=-0.10%
  A insert p0=42.5 p25=48.5 p50=49.2 p95=64.2 p99=78.9 mean=51.8
  B insert p0=42.6 p25=48.6 p50=49.2 p95=64.2 p99=77.7 mean=51.8
 recovery_checkpoint_us on fresh file: min=6973.0 median=8807.0 max=128868.0
```
