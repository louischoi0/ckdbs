# CI close: an autocommit `CREATE INDEX` no longer stalls a writer for the build; it costs the writer a 4x slower statement, and costs the log 5x

**Thesis.** On `614864c4` (`v3.4.3-13-g614864c4`) the `CREATE INDEX` of a 10,000,000-row
relation holds the relation `IS`, not `X`. The one autocommit `UPDATE` writer that was
stalled for the whole build on the base (26.8-28.1 s, 3 of 3 builds, equal to the build
to within 10 ms) is on the closing binary never stalled for more than **0.70-1.29 s**, and
`sys.lock_waits` has **no row at all**. The price moved elsewhere: the writer runs at
about 171 statements/s during the build (p50 3.6 ms against 1.0 ms), the build is 22-61%
longer (33.3 / 44.1 / 32.9 s against 27.4 s), and the build writes **1.37 GB of log
against 0.27 GB, 5.04x**, the estimate in CI §3.5 A12. With no build running, a writer on an
indexed relation measures the same on both binaries inside a 3-22% run-to-run floor, and
writes the same number of log bytes per statement. The stall is gone; "a writer pays nothing"
holds for the idle case and does not hold during a build.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-23, 13:21-13:30 UTC (arm B, 6 builds) and 13:30-13:46 UTC (arm A, 36 server runs); template loads 13:16-13:19 UTC |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/online-index-build`, `worktree-online-index-build` |
| Base | `dd437a76` (`v3.4.3-10-gdd437a76`, committed 2026-10-23 08:36:58 UTC), the milestone's opening commit |
| Closing | `614864c4` (`v3.4.3-13-g614864c4`, committed 2026-10-23 13:12:19 UTC) |
| Tree | **dirty, documents and one comment-level header only**: `CLAUDE.md`, `docs/`, `manual/`, `instructions/`, `include/kds/exec/leaf_cursor.hpp` (8 lines, modified 13:17:53 UTC, after the closing binary's mtime 13:16:04; not read, not in either binary as far as the mtimes show), and the two drivers (below). No other `include/` or `src/` file is modified |
| Binaries | `build-release/ci-ab/kds_server-dd437a76` (mtime 2026-10-23 13:12:30 UTC, sha256 `316f0b283cdf9fb3...`) and `build-release/ci-ab/kds_server-614864c4` (mtime 13:16:04 UTC, sha256 `06273d741192a443...`). Release, prebuilt, **run from those files unmodified** (they are the copies; hashes re-checked after the runs; nothing was built). The base binary's mtime is 12 s after the closing commit; it was built from the base commit by the order that supplied it, not rebuilt here |
| Build type | Release |
| Device | data files and WAL under `build-release/ci-close/` of the worktree on `/dev/root`, ext4 (`df -T`), not tmpfs |
| Server config | `cores = 4`, `durability = group`, `buffer_pool_frames = 65536`, every other key default; server pinned to CPUs 2-5, driver to CPUs 6-7 (host: 8 CPUs) |
| Host state | a sampler wrote `pgrep -x` counts of `cc1plus`, `clang`, `clang++`, `kds_tests` and the load average every 2 s (`archive/.../host.log`, 827 samples, 13:17:51-13:46:07 UTC): **0 compiler or test processes in every sample**, load average at most 3.10. Each run started only after 20 s with none. No pair had to be re-run |
| Prior number | arm B: `results-ci-s0-premise-v3.4.3-10-gdd437a76.md` (the premise, on the base commit). Arm A and the log volume: no prior kdbs number for these shapes; this file is the baseline |
| PostgreSQL floor | not measured for these shapes |
| Drivers | `tools/ci_s0_premise.py` (arm B; extended here with `--load-only`, `--tail`, and a WAL/meta capture around the build) and `tools/ci_close_arm_a.py` (arm A, new). See section 9 |
| Raw series | `bench/v3.4.3/archive/ci-close-v3.4.3-13-g614864c4/` (4.8 MB: writer series per build, arm A latencies in microseconds, the host log, the analysis scripts) |

## 2. Arm B: the premise again

Setup as in the S0 file's section 2: table `t (id, c1, c2, c3 int64) BTREE`, N = 10,000,000, `c1` scrambled; a
writer session (autocommit `UPDATE t SET c2 = c2 + 1 WHERE id = k`) and a point-reader session
(`SELECT * FROM t WHERE id = k`) from t = 0, and `CREATE INDEX ix ON t (c1)` on a third session at t = 5 s.
One difference from S0: the window is not a fixed 45 s but **runs until 20 s after the build returns**
(`--tail 20`, cap 200 s), because the closing build is longer than 45 s minus 5 s in one run. Fresh
server, fresh copy of a per-binary template (the closing commit bumps the superblock to 34 per its log, so a base volume is not assumed readable by the closing binary; each binary loaded its own, 125.7 s for the base), rotation base / closing / base / closing / base / closing.

### The stall is gone, the build is not free

| Run | Build elapsed | Writer max stall | Writer statements inside the build | Writer rate inside the build | `sys.lock_waits` rows | Log appended during the build |
|---|---|---|---|---|---|---|
| base 0 | 27.161 s | **27.151 s** | 1 | 0.04 /s | 1 (IX, waited 26.79 s, granted) | 271,292,840 B |
| closing 0 | 33.300 s | 0.735 s | 5,517 | 166 /s | 0 | 1,369,378,376 B |
| base 1 | 28.142 s | **28.141 s** | 2 | 0.07 /s | 1 (IX, waited 27.57 s, granted) | 271,733,920 B |
| closing 1 | 44.053 s | 1.291 s | 8,484 | 193 /s | 0 | 1,370,358,080 B |
| base 2 | 26.776 s | **26.768 s** | 1 | 0.04 /s | 1 (IX, waited 26.23 s, granted) | 271,541,584 B |
| closing 2 | 32.864 s | 0.701 s | 5,096 | 155 /s | 0 | 1,369,610,448 B |
| **base mean** | 27.360 s | 27.353 s | | | | 271,522,781 B |
| **closing mean** | 36.739 s | 0.909 s (max of 3: 1.291 s) | | 171 /s | | 1,369,782,301 B |

On the base binary the writer's one stalled statement is the build (stall within 8-11 ms of the build's elapsed time,
3 of 3). On the closing binary the writer completes 5,096-8,484 statements inside the build. The closing
builds are 5.5 s, 16.7 s and 5.1 s longer than the base builds of the same pair (+20%, +57%, +23%). The
second pair's 44.1 s build is the outlier: the closing builds' spread is 34% (32.9-44.1 s), the base's is 5%, and
the cause of the 44.1 s is not established (the host log shows no other process; the run's server CPU was also high, 93.1 s
against 70.7 and 67.8 s, over a window 11 s longer). Treat the closing build as **33-44 s, median 33.3 s**, and do not read the +34% mean as a constant.

### The writer's distribution, pooled over the three builds of each binary

Writer statements, milliseconds, split at the build's start and end (`pre` is the 5 s before the build, `in` is the build, `post` is the 20 s after it).

| Binary | Segment | Statements | p0 | p25 | p50 | p90 | p99 | p99.9 | p100 |
|---|---|---|---|---|---|---|---|---|---|
| base | pre | 10,770 | 0.836 ms | 0.957 ms | 1.030 ms | 1.339 ms | 3.941 ms | 63.265 ms | 347.793 ms |
| base | in | 4 | 106.647 ms | 26,767.876 ms | 27,150.992 ms | 28,141.314 ms | 28,141.314 ms | 28,141.314 ms | 28,141.314 ms |
| base | post | 38,411 | 0.829 ms | 0.988 ms | 1.149 ms | 1.529 ms | 4.299 ms | 61.681 ms | 1,549.944 ms |
| base | whole run | 49,185 | 0.829 ms | 0.980 ms | 1.119 ms | 1.507 ms | 4.269 ms | 63.451 ms | 28,141.314 ms |
| closing | pre | 11,321 | 0.842 ms | 0.954 ms | 0.992 ms | 1.276 ms | 4.002 ms | 57.913 ms | 102.616 ms |
| closing | **in** | **19,097** | 1.089 ms | 3.087 ms | **3.619 ms** | 5.259 ms | 17.661 ms | 520.975 ms | **1,291.386 ms** |
| closing | post | 34,973 | 0.843 ms | 0.973 ms | 1.042 ms | 2.294 ms | 7.731 ms | 62.763 ms | 410.650 ms |
| closing | whole run | 65,391 | 0.842 ms | 0.987 ms | 1.266 ms | 4.080 ms | 9.792 ms | 238.127 ms | 1,291.386 ms |

The base `in` row is four statements (the stalled ones, plus one that started in the build's last milliseconds), so
its percentiles are the four values and nothing more; the rest of the base run's number is in `post`. The idle floor
is the same on both binaries (`pre` p0 0.836 / 0.842 ms, p50 1.03 / 0.99 ms): the ~0.83 ms of an
autocommit write under `group` durability. **Inside the closing build the body of the distribution moves
about 3.6x (p50 3.6 ms, p25 3.1 ms) and the tail moves much further (p99.9 521 ms)**, and the p100 of 0.70-1.29 s
repeats as a band of 0.5-0.7 s statements through the whole build (the six slowest statements of each closing run are all 0.52-0.74 s
except run 1's 1.29 s). A slow statement of that length is a regular feature of the build, not a single event.

After the build the closing writer has a longer tail than the base writer in the same 20 s: `post` p90 2.29 ms against 1.53 ms,
p99 7.7 against 4.3 ms, p100 411 against 1,550 ms (the base's p100 is the checkpoint-shaped statement S0 section 6 describes). The closing
`post` segment spans the checkpoint of 1.37 GB of log and 33k new index pages; the cause was not isolated and the segment ends
at 20 s, so the length of the tail is not known.

### Readers

A point `SELECT` by tuple key rate: the base reader ran 14,465 / 16,514 / 16,471 /s inside the build (17,138 / 17,196 / 17,085 /s before it); the closing
reader ran 12,005-13,354 /s inside the build (16,429-16,730 /s before it). The reader is not blocked on either binary (no `sys.lock_waits`
row for a reader in any run); on the closing binary it loses 19-28% (13,287 / 13,354 / 12,005 /s against 16,730 / 16,429 / 16,694 /s) against 4-16% on the base while the build runs
(the closing build also has a writer making progress, which is the other consumer of the server's four cores).

## 3. Against this engine's own last number (the S0 premise)

| Quantity | S0, `v3.4.3-10-gdd437a76` (3 builds) | Base re-measured here (3 builds) | Closing, `v3.4.3-13-g614864c4` (3 builds) |
|---|---|---|---|
| Build elapsed | 27.7 / 28.4 / 34.8 s | 27.2 / 28.1 / 26.8 s | 33.3 / 44.1 / 32.9 s |
| Writer max stall | 27.7 / 28.4 / 34.8 s (equal to the build) | 27.2 / 28.1 / 26.8 s (equal to the build) | 0.74 / 1.29 / 0.70 s |
| Writer statements started inside the build | 1-2 | 1-2 | 5,096-8,484 |
| `sys.lock_waits` for the writer | 1 row, IX, 27.2-34.1 s | 1 row, IX, 26.2-27.6 s | none |
| Reader point `SELECT` inside the build | 13,909-15,746 /s (S0, with the reader's own run-to-run floor) | 14,465-16,514 /s | 12,005-13,354 /s |
| Log appended by the build | not measured | 271.5 MB (27.2 B/row) | 1,369.8 MB (137.0 B/row) |

The base re-measurement agrees with S0 on every shape (stall equals build; one lock wait, IX, granted after 26-28 s); the base
mean build time (27.4 s) is 10% below S0's mean (30.3 s), inside S0's own 25% spread. **The delta of the change against the prior number: the writer's
worst statement falls from 27.7-34.8 s to 0.70-1.29 s (21-50x shorter, comparing the extremes of the two sets), the
build's elapsed time rises from 27.4 s (mean) to 33.3-44.1 s, and the log volume rises 5.04x.** The S0 file's writer throughput
(ops/s over a 45 s window) is not comparable to this run's, because the window here is built around the build's length.

## 4. Wait breakdown

| Wait type | Measured? | Value |
|---|---|---|
| Lock / conflict wait on the relation (`sys.lock_waits`) | yes | base: 1 row per build, `unit = relation`, `mode = IX`, `outcome = granted`, `holder_trx` the build, waited 26.79 / 27.57 / 26.23 s, 98% of the stalled statement. Closing: **0 rows in 3 of 3 builds**. The `IX` writer is compatible with the build's `IS`; `lock_waits_recorded` counts nothing |
| Durability / commit (fsync), WAL | partly, server-wide counters over the build window (closing run 0, run 2 in parentheses; base run 0) | `contention_wal_stream_wait_us` 8.28 s (8.24 s) over 4,231 waits (base 0.31 s, 1 wait); `contention_wal_drain_latch_wait_us` 8.27 s (8.19 s) (base 0); `contention_wal_roll_blocked_wait_us` 8.27 s over 35 waits (8.24 s over 38) (base 0.31 s over 1); `contention_wal_sync_gate_wait_us` 4.03 s over 22 waits (6.49 s over 36) (base 0). `wal_rolls` 21 on closing, 4 on base. These are all sessions' waits including the backfill's own appender, not the writer's alone; the per-session share is not exposed |
| Write statement | yes | `sys.statement_stats` `UPDATE` `total_us`: base run 0 51.9 s over 14,020 calls (max 27.13 s, the stalled one); closing run 0 57.3 s over 21,310 calls (max 0.74 s); closing run 1 67.8 s over 23,979 calls (max 1.29 s) |
| Backfill's re-read of the writer-contended rows (CI-R7) | yes | `contention_inflight_asks` 10,000,000 and `contention_inflight_misses` 10,000,000 during each closing build (base 0): one in-flight lookup per backfilled row, all misses |
| Client and socket round trip | not separable | `sys.statement_stats` times a statement whole; `pre`'s 0.83 ms p0 is the floor and includes it |
| Rest of the stalled statement (base) | not separable | 0.5-1.0 s of each 27-28 s stall is outside the lock wait, as in S0 |

Where the closing writer's 3.6 ms goes is **not decomposed**. The candidates the counters point at, each untested here,
are the WAL stream (the backfill appends 1.37 GB, 137 B per row, through 21 segment rolls; writers wait on the roll and the sync gate behind it) and the cores' time
(the four server cores also run the backfill). The writer's `UPDATE` of `c2` does not touch `c1`, so index maintenance by the writer itself is the least likely share, but no run changed it.

## 5. Arm A: a writer with no build running (CI-R5's overhead)

Table `t (id, c1, c2, c3 int64) BTREE` with one secondary index `ix (c1)` created before the load; N preloaded rows; then on
one connection K = 8,000 statements of each shape, serially, autocommit, `group`: **ins** a single-row `INSERT` of a new id with a scrambled `c1`;
**upd2** `UPDATE t SET c2 = c2 + 1 WHERE id = k` (the indexed column untouched); **upd1** `UPDATE t SET c1 = c1 + 1 WHERE id = k` (the indexed column changes, so the index is maintained).
The sweep: N = 200, 10,000 and 200,000 preloaded rows (the point lookup does not scale with rows, and the three sizes are the evidence). Equal work: 8,000 completed statements per phase per run. Three
pairs, rotation base / closing within each (N) cell, 18 runs per binary; a fresh server and data file per run. Replies checked: 0 mismatches
in 144,000 statements per binary, no `sys.lock_waits` row in any run. The rows below are QPS derived as 8,000 / elapsed.

### Throughput, three runs per cell

| N | Shape | Base QPS (3 runs) | Closing QPS (3 runs) | Base mean | Closing mean | Delta | Base spread | Closing spread |
|---|---|---|---|---|---|---|---|---|
| 200 | INSERT | 949 / 786 / 817 | 949 / 774 / 806 | 851 /s | 843 /s | -0.9% | 19.1% | 20.8% |
| 200 | UPDATE c2 | 949 / 792 / 803 | 966 / 790 / 801 | 848 /s | 853 /s | +0.5% | 18.4% | 20.7% |
| 200 | UPDATE c1 | 959 / 774 / 803 | 943 / 818 / 805 | 845 /s | 855 /s | +1.2% | 21.9% | 16.1% |
| 10,000 | INSERT | 948 / 805 / 825 | 783 / 826 / 813 | 859 /s | 807 /s | -6.0% | 16.6% | 5.4% |
| 10,000 | UPDATE c2 | 942 / 799 / 800 | 767 / 802 / 796 | 847 /s | 788 /s | -6.9% | 16.9% | 4.4% |
| 10,000 | UPDATE c1 | 936 / 823 / 811 | 783 / 807 / 797 | 857 /s | 796 /s | -7.1% | 14.7% | 3.0% |
| 200,000 | INSERT | 749 / 778 / 786 | 761 / 779 / 764 | 771 /s | 768 /s | -0.4% | 4.8% | 2.4% |
| 200,000 | UPDATE c2 | 750 / 792 / 787 | 759 / 775 / 773 | 777 /s | 769 /s | -1.0% | 5.4% | 2.1% |
| 200,000 | UPDATE c1 | 732 / 764 / 774 | 746 / 758 / 771 | 757 /s | 759 /s | +0.2% | 5.5% | 3.2% |

**No cell has a delta outside the noise floor.** The floor from inside the run is the spread across repeats of one
configuration, 2-22% (largest at N = 200 and 10,000, where the first run of the rotation is 940-966 /s and the next
ones 770-830 /s on both binaries). The one apparent signal, -6% to -7% at N = 10,000, is carried entirely by the first pair: there the base ran fast (936-948 /s) and the
closing run slow (767-783 /s), while in pairs 2 and 3 the two binaries agree within 2%
(INSERT 815 vs 820 /s, UPDATE c2 800 vs 799 /s, UPDATE c1 817 vs 802 /s, means of two runs). It follows the position in the rotation, as in the
N = 200 cells where both binaries were fast in pair 1, and is not attributed to the binary. The resolution of this arm is about +-7%: a per-statement overhead of a few percent of the 0.83 ms floor cannot be seen here.

### Latency distribution, pooled over the three runs

Milliseconds, 24,000 statements per row. (p95 in place of p90 in this arm; the driver was written to rule 6's five points.) N = 10,000's closing rows carry the slow first pair above, which is why their p0 is 0.93-0.96 ms against 0.80-0.82 ms.

| N | Shape | Binary | p0 | p25 | p50 | p95 | p99 | p100 |
|---|---|---|---|---|---|---|---|---|
| 200 | INSERT | base | 0.816 ms | 1.005 ms | 1.143 ms | 1.485 ms | 2.656 ms | 24.005 ms |
| 200 | INSERT | closing | 0.815 ms | 1.009 ms | 1.144 ms | 1.525 ms | 2.766 ms | 29.604 ms |
| 200 | UPDATE c2 | base | 0.820 ms | 1.019 ms | 1.160 ms | 1.459 ms | 2.560 ms | 19.282 ms |
| 200 | UPDATE c2 | closing | 0.819 ms | 1.007 ms | 1.147 ms | 1.499 ms | 2.523 ms | 25.957 ms |
| 200 | UPDATE c1 | base | 0.821 ms | 1.024 ms | 1.164 ms | 1.502 ms | 2.646 ms | 15.205 ms |
| 200 | UPDATE c1 | closing | 0.821 ms | 1.020 ms | 1.139 ms | 1.387 ms | 2.617 ms | 20.765 ms |
| 10,000 | INSERT | base | 0.805 ms | 1.007 ms | 1.137 ms | 1.413 ms | 2.607 ms | 28.077 ms |
| 10,000 | INSERT | closing | 0.931 ms | 1.118 ms | 1.177 ms | 1.524 ms | 2.668 ms | 13.528 ms |
| 10,000 | UPDATE c2 | base | 0.819 ms | 1.019 ms | 1.151 ms | 1.388 ms | 2.875 ms | 29.838 ms |
| 10,000 | UPDATE c2 | closing | 0.955 ms | 1.151 ms | 1.197 ms | 1.511 ms | 2.796 ms | 19.635 ms |
| 10,000 | UPDATE c1 | base | 0.823 ms | 1.022 ms | 1.145 ms | 1.377 ms | 2.641 ms | 18.481 ms |
| 10,000 | UPDATE c1 | closing | 0.957 ms | 1.137 ms | 1.184 ms | 1.492 ms | 2.942 ms | 27.884 ms |
| 200,000 | INSERT | base | 0.948 ms | 1.143 ms | 1.207 ms | 1.589 ms | 3.209 ms | 83.223 ms |
| 200,000 | INSERT | closing | 0.952 ms | 1.144 ms | 1.210 ms | 1.571 ms | 3.231 ms | 55.409 ms |
| 200,000 | UPDATE c2 | base | 0.948 ms | 1.142 ms | 1.185 ms | 1.511 ms | 3.043 ms | 45.490 ms |
| 200,000 | UPDATE c2 | closing | 0.924 ms | 1.147 ms | 1.191 ms | 1.525 ms | 3.129 ms | 64.156 ms |
| 200,000 | UPDATE c1 | base | 0.964 ms | 1.133 ms | 1.179 ms | 1.517 ms | 3.519 ms | 62.478 ms |
| 200,000 | UPDATE c1 | closing | 0.956 ms | 1.140 ms | 1.189 ms | 1.506 ms | 3.516 ms | 63.719 ms |

### The log cost of one statement is identical

`wal_append_lsn` per statement over each 8,000-statement phase, from `SHOW META`:

| N | INSERT | UPDATE c2 | UPDATE c1 |
|---|---|---|---|
| 200 | 717 B on both binaries | 265 B on both | 386 B on both |
| 10,000 | 719 B on both | 266 B on both | 322 B on both |
| 200,000 | 630-631 B on both | 273-274 B on both | 334-335 B on both |

To the byte, the closing binary appends what the base appends for a writer with no build in progress. This is a stronger
statement than the throughput table, which cannot resolve below about 7%: CI-R5's "a writer pays nothing" is true of the log in the idle case, and not resolved below noise for time.

## 6. Log volume of a build (CI §3.5 A12)

| Quantity | Base | Closing |
|---|---|---|
| `wal_append_lsn` delta across `CREATE INDEX` (3 builds) | 271,292,840 / 271,733,920 / 271,541,584 B | 1,369,378,376 / 1,370,358,080 / 1,369,610,448 B |
| Mean | 271.5 MB | 1,369.8 MB |
| Per backfilled row (N = 10,000,000) | 27.2 B | 137.0 B |
| Writer's own appends inside the build (statements x measured bytes per statement) | ~0.0003 MB (1-2 statements) | 1.5-2.5 MB (5,096-8,484 statements x 276-301 B, bytes per statement from the 20 s after the build) |
| Net of the writer | 271.5 MB | 1,367.8-1,368.2 MB |
| Segment rolls during the build (`wal_rolls`) | 4 | 21 |
| Ratio, closing / base | | **5.04x** |

The estimate in CI §3.5 A12 was about 5x; the measure is 5.04x, with a spread across the three builds of 0.07%. The volume is a function of N and not of how long the
build took (the 44.1 s build wrote the same 1.37 GB as the 32.9 s ones, within 0.07%), so the log is deterministic where the elapsed time is not. `pages_allocated` grew by the same ~32.9k
pages on both (81,704 to ~114,630 on base, 81,684 to 114,636-114,680 on closing), so the data-file footprint of the index is unchanged and the 5x is entirely the log.

## 7. What this teaches about the engine

1. **The writer's 27-28 s relation-lock wait is the whole of what `IS` removed.** The base's wait was a single `IX` request behind the build's `X`; with the build on
   `IS` the request is granted and no `lock_waits` row exists. But the writer does not run at its idle speed: it runs at
   ~171 /s against ~750 /s with a 4x slower body and a band of 0.5-0.7 s statements. The wait moved from the lock table to the WAL (`wal_stream_wait`, the roll and the sync gate: 8 s and 4-6 s summed over all sessions
   in a 33 s build) and to the four cores. For CI's claim "writers keep running" this is the number to carry: they keep running at about a quarter of speed, with a worst case of about 1 s.
2. **The cost of online is paid in the log, and it is exactly the logging of each backfilled entry.** 137 B per row against 27 B, 21 segment rolls against 4. The `post` segment's longer p90 is consistent with the checkpoint then catching up on that, but no run isolated it. The log volume is the
   figure to size `wal` retention against, not the elapsed time.
3. **The build's elapsed time is the noisiest number of the run.** 33.3 / 44.1 / 32.9 s on closing against 27.2 / 28.1 / 26.8 s on base; the log volume to 0.07% does not vary, so the variance
   is in waiting (the WAL sync gate waits ranged 4.0-6.5 s between two closing runs), not in work. The 44.1 s run is reported, not discarded.
4. **A single-writer idle overhead cannot be seen in time and cannot be seen in the log.** The 144,000-statement arm shows identical log bytes per statement and throughput within a 2-22% floor.
   The design expectation in the CI work order (CI-R5, "a writer pays nothing") is supported for the log and not contradicted for time; time's resolution is the limit.
5. **Where the data supports no insight:** the cause of the 0.5-0.7 s regular band and of the closing `post` tail; neither was decomposed.

## 8. Limits and what was cut

- **Row-set sweep:** arm A swept N = 200, 10,000, 200,000 (the work order's "smaller N"); rule 9's 1K was not run. Arm B ran at N = 10,000,000 only, one cardinality, as in S0, because the stall's size is the build's length.
- **Runs:** three pairs in both arms (6 builds, 36 arm A runs). Nothing was cut for time beyond arm A's K = 8,000 statements per shape, and no run was dropped or repeated. Arm A's interleave is by (N) cell: base, closing, then the next N, three rounds.
- **Not measured:** a writer that changes `c1` during a build (so the building index is maintained with a changed key; arm B's writer touches `c2` only), several writers, `relaxed` durability, `cores` other than 4, a reader that scans or ranges, the rolled-back or crashed build, and a closing run at 100M rows.
- **The reader runs beside the writer in arm B** (as in S0), so the writer's in-build rate is a writer sharing four cores with a reader and the backfill.
- **The base volume and the closing volume differ** (the closing commit bumps the superblock to 34), so the two templates are separate loads of the same logical data (same driver, same scramble); no cross-binary file reuse.
- **A12 and the WAL figure** are `wal_append_lsn` deltas, byte offsets of the append position; not file sizes (segments are preallocated at 64 MiB, so directory sizes do not move).
- **Dirty tree:** see the stamp; the engine sources are those of `614864c4`.
- **Checks:** no `--verify` mode exists in either driver; every reply tag was checked (0 mismatches). The test suite was not run (no engine change was made).

## 9. Reproduce

Drivers' flags are in their own docstrings (`bench/docs/` does not exist on this branch). Both binaries were started from the files named in the stamp.

```
# template per binary (about 2 min each)
python3 tools/ci_s0_premise.py --bin <BIN> --workdir build-release/ci-close/<v> --rows 10000000 \
  --progress <P> --out <O> --load-only --keep-template --port 31970
# one arm B run (fresh server, build at t=5 s, window ends 20 s after the build returns)
python3 tools/ci_s0_premise.py --bin <BIN> --workdir build-release/ci-close/<v> --rows 10000000 \
  --window 200 --build-at 5 --tail 20 --runs 1 --run-offset <i> --arms B --reuse-template --keep-template \
  --progress <P> --out <O>
# one arm A run
python3 tools/ci_close_arm_a.py --bin <BIN> --workdir build-release/ci-close/a-<v> --rows <N> --ops 8000 --out <O>
```

`--rows` is N, the number of rows preloaded (tuple key 1..N). The rotation scripts (`armb.sh`, `master.sh`, `gate.sh`) and the analysis scripts are in the archive directory.
