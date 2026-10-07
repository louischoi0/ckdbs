# BE's close: a bounded buffer pool against the unbounded one

**A** = `e4b107af` (`v2.7.0-659-ge4b107af`), the commit BE opened at. Its pool
is unbounded, `buffer_pool_frames` is a soft budget, and a miss past the
budget sorts the whole frame table.

**B** = `eef442cf` (`v2.7.0-665-geef442cf`), BE closed:
- the key is required and a hard ceiling;
- frames are slots, reclaimed in bounded batches, inline and on the tick;
- a `SELECT`'s outermost walk faults cold;
- slots are padded by a cache line, and the frame sits in the page table's
  node.

**B₀** = `2f08b71c` (`v2.7.0-664-g2f08b71c`) is BE before the close's
layout fix. The past-the-cap, scan-resistance and bulk-load cells were
measured on B₀, and the layout fix does not touch their paths. Every
overhead and resident-scan cell below is B.

Order: `instructions/v3.0.0/workorder-be-bounded-pool.md` §5. The raw driver
output is under `archive/be-close-v2.7.0-664-g2f08b71c/` (the B₀ run) and
`archive/be-close-v2.7.0-665-geef442cf/` (the B run, the bisection and the
hit microbenchmark).

## How it was run

- **Build.** Release, rebuilt at each measured commit. A and the three
  stage commits were built from `git archive` exports; B was built in the
  worktree's `build-release`. Every server ran from a hashed copy
  (`binaries.txt`): A's `kds_server` is `04457269…`, B's is `8cac64de…`.
- **Data files.** On `/dev/root`, ext4, under `/home/cdkbs/bench-runs/`.
- **Host.** 8 CPUs. The server was pinned to CPU 2 and the client to CPU 4.
  Load averages are recorded per run in each `*.json` (1.2-2.4 across the
  B run). No build or other benchmark ran alongside, and xrock's server
  was stopped throughout.
- **Rule 5, broken.** `bench/README.md` says drivers are unmodified
  `tools/` scripts. These were not. The close's measurement agent wrote
  scratch copies shaped on `tools/bb_overhead_benchmark.py`
  (`be_overhead_ab.py`, `be_warmscan_ab.py`, `pool_past_cap.py`), because
  B refuses to boot without `buffer_pool_frames` and the series' drivers
  write no such key. They are archived beside the results.

## 1. Overhead, a pool larger than the working set (no eviction on either side)

12 interleaved runs per size, `durability = relaxed`, `cores = 1`. The
latencies are pooled over 36,000 statements per arm. The delta is the
median of the per-run p50 deltas, with a bootstrap 95% CI.

| rows | shape | A p0 / p25 / p50 / p90 / p99 | B p0 / p25 / p50 / p90 / p99 | Δ p50 |
|---|---|---|---|---|
| 1,000 | insert | 39.9 / 48.1 / 48.9 / 57.0 / 77.7 µs | 41.2 / 48.1 / 48.9 / 57.0 / 76.9 µs | −0.05 µs [−0.15, +0.15] |
| 1,000 | select-pk | 58.1 / 64.4 / 66.6 / 79.1 / 108.5 µs | 57.7 / 63.8 / 66.0 / 78.0 / 107.7 µs | −0.85 µs [−1.05, −0.20] |
| 1,000 | update-pk | 40.3 / 49.8 / 51.1 / 60.2 / 82.8 µs | 41.2 / 50.2 / 51.3 / 60.5 / 81.3 µs | +0.00 µs [−0.20, +0.40] |
| 1,000 | range100 | 131.6 / 138.8 / 141.8 / 157.1 / 203.9 µs | 134.6 / 141.6 / 145.2 / 162.9 / 213.9 µs | +1.95 µs [+0.90, +4.50] |
| 1,000 | SHOW META | 31.0 / 37.6 / 38.1 / 46.9 / 60.5 µs | 31.9 / 42.3 / 45.0 / 49.3 / 61.4 µs | +7.05 µs [+4.05, +7.25] |
| 10,000 | insert | 40.4 / 48.2 / 49.0 / 57.7 / 80.2 µs | 40.1 / 48.1 / 48.9 / 58.9 / 80.6 µs | −0.10 µs [−0.20, +0.05] |
| 10,000 | select-pk | 58.1 / 63.7 / 64.6 / 74.4 / 96.1 µs | 57.4 / 62.9 / 63.9 / 73.3 / 93.4 µs | −0.60 µs [−1.00, −0.25] |
| 10,000 | update-pk | 42.1 / 50.3 / 51.3 / 59.5 / 80.2 µs | 40.3 / 51.0 / 51.7 / 60.1 / 80.1 µs | +0.55 µs [−0.05, +0.75] |
| 10,000 | range100 | 682.5 / 737.1 / 762.9 / 803.7 / 994.4 µs | 713.7 / 771.6 / 794.6 / 878.9 / 1,082.5 µs | **+27.85 µs [+21.25, +35.30]** |
| 10,000 | SHOW META | 31.0 / 38.0 / 38.5 / 46.7 / 59.4 µs | 31.1 / 43.8 / 45.1 / 48.7 / 61.1 µs | +6.45 µs [+6.30, +6.95] |

**Resident full scans** (`be_warmscan_ab.py`, 12 runs, 4,320 statements per
arm):

| rows | A p0 / p25 / p50 / p90 / p99 | B p0 / p25 / p50 / p90 / p99 | Δ p50 |
|---|---|---|---|
| 10,000 | 1,102 / 1,124 / 1,135 / 1,200 / 1,357 µs | 1,132 / 1,151 / 1,160 / 1,231 / 1,322 µs | **+28.7 µs (+2.5 %) [+14.1, +40.7]** |
| 100,000 | 10,398 / 10,498 / 10,593 / 11,300 / 12,133 µs | 10,612 / 10,760 / 10,871 / 11,676 / 12,146 µs | **+274 µs (+2.6 %) [+194, +403]** |

**What the overhead is, and what it is not.**
- **Point statements are flat or better.** `select-pk` is 0.6-0.85 µs faster.
  `insert` and `update-pk` sit within their intervals.
- **`SHOW META` is 7 µs slower because its reply is longer.** The neighbouring-stage
  bisection puts all of it at BE-S3 (`8ef588e0`: 38.0 → 45.4 µs), the stage
  that added fourteen `pool_*` fields to the reply. No other statement path
  moved at that stage. The driver's "ping" arm is `SHOW META`.
- **Resident scans are 2.5-3.7 % slower, and that is a regression B carries.**
  The bisection puts it at BE-S2 (`0da17f9f`, the slot array:
  `range100` +37 µs). The close's layout fix recovered about 40 % of it:
  - The slab laid pages at an exact 8 KiB stride, so every page's header
    and slot directory, which a walk reads first, mapped onto the same few
    cache sets.
  - A hit microbenchmark (`hitbench.cpp`, archived) on a resident 64k-page
    pool measured 33.5 ns a hit at BE-S2 against 23.3 ns at A. Padding each
    slot by 64 bytes and putting the frame back in the table's node brings
    B to 24.0-24.4 ns. With the frame left in its slot, it was
    25.4-25.8 ns.
  - The scan gap fell from +474 µs to +274 µs at 100,000 rows, and from
    +39 µs to +29 µs at 10,000.
  - **What remains is about 270 ns per resident page, and the store's hit
    path does not account for it.** That path is now within 1 ns of A. The
    remaining cost is not attributed, and `perf` is locked out on this host
    (`perf_event_paranoid` = 4).

## 2. Past the cap

`pool_past_cap.py` on B₀ and A. The cap is 16,384 frames (128 MiB) or
65,536 frames (512 MiB). `big2` and `big4` are relations of about 2× and 4×
the cap. The probes are xrock's per-day shape, `COUNT(*), MIN, MAX` over id
ranges, run twice.

| cap | cell | A | B₀ |
|---|---|---|---|
| 16,384 | load `big2` (multi-row INSERTs) | 25.3 s, RSS 309 MB | 25.8 s, RSS 138 MB |
| 16,384 | load `big4` | 51.1 s, RSS 897 MB | 49.7 s, RSS 139 MB |
| 16,384 | one `COUNT(*), MIN, MAX` over `big2` | 6.8 s | 0.32 s |
| 16,384 | probes, first pass | 12.8 s | 0.36 s |
| 16,384 | probes, second pass | 16.7 s | 0.35 s |
| 65,536 | load `big2` | 101.4 s, RSS 1,199 MB | 101.4 s, RSS 503 MB |
| 65,536 | one `COUNT(*), MIN, MAX` over `big2` | 74.0 s | 1.3 s |
| 65,536 | probes, first pass | 234.8 s | 1.43 s |
| 65,536 | probes, second pass | 300.3 s | 1.43 s |

- **B₀'s RSS stays within the cap plus the process:** 138 MB at a 128 MiB
  cap, 503 MB at 512 MiB. A's grows with what it touches.
- **Neither load was refused.** Both had 0 errors, and B₀ had zero
  `ResourceExhausted` at a cap above the floor.
- A's first A run at 16,384 timed out its client socket on the scan and
  was re-run. Both runs are in the archive.

**Per miss past the budget** (`kds_pool_sweep_bench`, 131,072 frames, the
whole second half of a 2× file):

| | A (`e4b107af`) | B (`eef442cf`) |
|---|---|---|
| per miss, p0 / p25 / p50 / p90 / p99 | 2,005 / 2,216 / 2,285 / 2,640 / 2,744 µs (4,000 sampled) | 2.1 / 2.4 / 2.5 / 2.9 / 5.1 µs |
| the half | 304 s (extrapolated) | 0.35 s |

The frame moving back into the table's node gives the sweep a hash probe
per occupied step. Measured here, it did not change the per-miss cost:
2.5 µs at p50 on B and on B₀.

## 3. Scan resistance — partial, and that is a finding

`scanres.py` on B₀ at a 16,384-frame cap: a hot set read, then a scan of
`big4` (about 4× the cap), then the hot set again. "Misses" counts the
hot-set pages re-faulted afterwards.

| hot set | warm passes | B₀ misses after the scan | A re-read time, before → after |
|---|---|---|---|
| 8,192 pages | 1 | 7,510 of 8,192 | 0.51 → 4.57 s |
| 8,192 pages | 3 | 8,243 | 0.51 → 4.54 s |
| 12,288 pages | 1 | 12,824 | 0.76 → 7.58 s |

**BE-R5 does not hold a hot set of half the pool through a scan four times
the pool.**
- A cold fault makes each scanned frame the hand's next victim. But the
  scan still drives about eight laps of the hand through the half of the
  pool that is free, and every lap lowers each hot frame's counter.
- A page warmed once or three times survives fewer laps than that. Only a
  hot set read more times than the scan has laps survives, which is what
  `ColdScanTest.AHotWorkingSetSurvivesAScanFourTimesThePool` covers:
  32 pages read five times in a 256-frame pool.
- B₀'s re-read after the scan is still 0.07-0.83 s against A's 4.5-7.6 s,
  but that is the bounded sweep's doing, not scan resistance.
- **This is the case BE-Q7 names for the scan ring:** "(b) becomes its own
  order if BE-S6 measures displacement anyway". It has measured it.

## What this says about the engine

- **A bounded pool costs the OLTP point path nothing measurable**, and it
  turns the scan past the cap that A could not finish in practice into
  seconds.
- **It costs a resident scan 2.5-3.7 %**, of which the slab's cache-set
  aliasing was the larger, findable part.
- **Its scan resistance is CLOCK's, no better.** The cold fault removes the
  scan's own heat, but it cannot stop the hand's laps from wearing down a
  hot set that was touched only a few times.
