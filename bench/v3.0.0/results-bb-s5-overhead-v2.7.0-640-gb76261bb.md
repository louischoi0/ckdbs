# BB-S5 overhead - partial, deferred (v2.7.0-640-gb76261bb)

**Status: partial. The operator deferred the rest of the measurement on
2026-10-07** to take other work first; this file states what had been
measured when the run was stopped, and nothing here is a pass for a cell
that did not run.

- **A** = `bddd450c` (`v2.7.0-622-gbddd450c`), Release - BB's opening commit.
- **B** = `b76261bb` (`v2.7.0-640-gb76261bb`), Release - BB's code at
  BB-S4's review. BB-S5's own rows are documents only.
- Binaries' sha256: A `2d7e6450...4e45c22c`, B `3803228d...76e93`
  (`archive/bb-s5-overhead-v2.7.0-640-gb76261bb/serial-start.txt`).
- Method: the AP-S5 file's (`tools/ap_overhead_benchmark.py`), driven by
  `tools/bb_overhead_benchmark.py` (serial), `tools/bb_concurrent_benchmark.py`
  (C3/C4) and `tools/bb_orderby_benchmark.py` (C5, never run). Fresh server
  and data file per (cell, run), arms alternating which goes first, a
  repeated arm (`insert-again`) and a control arm (`ping`) for the noise
  floor, `relaxed` durability, `/proc/loadavg` and competing processes before
  and after each run. Raw JSON and logs: `archive/bb-s5-overhead-v2.7.0-640-gb76261bb/`.
- Host: 8 CPUs, no build ran during the serial cells.

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
zero except C1s at 1,000 rows, where B is faster by 0.33 us - one cell of
nine, unexplained, and not claimed as a gain. The control arm (`ping`), which
BB does not touch, moves by -2.2 to +0.7 us across the same runs, so the
noise floor is of that order. An unpinned series (`default`, 10 runs) was also
taken: every interval straddles zero but is ~10x wider (up to +/-12 us) - it
measures the scheduler, not the engine, and decides nothing.

`recovery_checkpoint_us` on a fresh file ran 7.1-24.7 ms (median ~8.4 ms):
no device stall flagged.

## `cores = 8` - C3 complete, C4 partial

Not pinned apart (reactors and 8 client processes share the 8 CPUs; BA-Q2's
clean ceiling is `cores = 2`), so these are indicative only. Medians of the
runs completed:

| Cell | runs | arm | rows/s | p0 | p25 | p50 | p95 | p99 | refusals |
|---|---|---|---|---|---|---|---|---|---|
| C3 one relation | 10 | A | 62,296 | 39.9 | 79.3 | 96.4 | 182.6 | 247.8 | 1.5 |
| C3 | 10 | B | 63,041 | 36.9 | 80.3 | 95.2 | 178.2 | 246.9 | 0.0 |
| C4 eight relations | 6 of 10 | A | 61,520 | 38.9 | 75.6 | 93.5 | 182.4 | 264.2 | 0.0 |
| C4 | 6 of 10 | B | 63,429 | 40.2 | 74.9 | 90.7 | 173.8 | 246.2 | 0.0 |

B is not slower on either shape. C3's refusal median falls from 1.5 to 0
(`TXN_CONFLICT retryable=1`, retried); with ten runs that is a direction,
not a resolved difference. Latencies in us.

## Not executed

- **C4's remaining runs** - the run was left going when the measurement was
  deferred; this file reflects the six completed when it was written.
- **C5** (`ORDER BY <pk>` over a relation `kUnordered` at A) - not executed.
- No wait breakdown was taken for any cell, and no `SHOW META` comparison is
  summarised here; the per-run `SHOW META` captures are in the archive.

## Insight

BB moved the id issue under the leaf's (or tail's) exclusive hold. At one
core that hold was already taken by the placement, so the issue, the borrow
and the encode only moved inside a span that existed - which is why no cost
resolves. The open question is the `cores > 1` shape, where the hold is now
held longer; C3/C4 show no loss at 8 cores unpinned, and the clean answer
waits for a pinned `cores = 2` cell when the measurement resumes.

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
