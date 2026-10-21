# CE drain attribution: the close's 1-4 % `drain` regression does not reproduce, and removing the per-row test seam is worth at most 1.4 %, at the noise floor

**Thesis.** On `ce-streaming-reply`, engine source `9e568b44` (`v3.3.0-52-g9e568b44`, binary **B**), against the engine at CE's opening commit `1f57f37b` (binary **A**) and against **C**, B with the one per-row `select_row_emitted_for_test_` block deleted from `SelectRun::Execute`'s row visitor, a client that decodes nothing sees **no cost from CE** in this window.
Five rounds, three fresh servers per round, the order rotated each round: B over A is +0.2 % at 100 rows, **-1.1 % at 1,000 rows** and **-2.0 % at 10,000 rows** in wall per statement, and -0.4 %, -1.0 %, -2.1 % in server CPU. The close (`results-ce-close-v3.3.0-52-g9e568b44.md` §5) found +1.3 %, +4.2 % and +2.6 % at the same sizes.
B was slower than A in 3 of 3 pairs there; here B is slower than A in 0 of 5 pairs at 1,000 rows and in 0 of 5 at 10,000 rows.
B's own medians reproduce the close's (495.4 against 497.7 µs at 1,000 rows); **A's do not: 501.0 µs here against 477.5 µs in the close, +4.9 %, the same binary on the same host.** The close's gap came from A being fast in that window, and the cross-window movement of A alone is as large as the gap that was attributed.
C against B: C is faster by 6.9 µs wall (1.4 %) at 1,000 rows, 5 pairs of 5, and 48.1 µs (1.1 %) at 10,000 rows, 4 pairs of 5, a seam cost of 5 to 8 ns per row in server CPU; both are at or inside the run's spreads (1.2 % to 4.8 %), so **neither is a finding**.
With nothing to attribute, the sampler was not run (section 2).

Driver, flags and invocation: `archive/ce-drain-attribution-v3.3.0-52-g9e568b44/run_drain.sh` over `rowcost/ce_s0_rows.py` (the close's `drain` supplement, `--modes drain --reps 1 --target-s 2.5`, with a `SHAPES_ONLY` environment filter added); `bench/docs/` stays closed (`bench/README.md`). Measured by `ck-tester`.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time executed | 2026-10-20. Variant build 11:52:47 to 11:54:00 UTC; measurement 11:54:48 to 12:01:42 UTC (15 server visits of 27 s) |
| Worktree and branch | `ce-streaming-reply` (`/home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply`), HEAD `380f6a1d` (`v3.3.0-55-g380f6a1d`). Tree: one uncommitted modification, `instructions/v3.2.0/workorder-ce-streaming-reply.md`; no engine file. `git diff --stat 9e568b44 HEAD -- src include CMakeLists.txt tools` is empty, so the engine at HEAD is B's |
| Binary A | `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`, mtime 2026-10-20 03:37:20, built at `9682b725` (engine source equals `1f57f37b`), **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`**. Older than HEAD: it measures the engine CE started from |
| Binary B | the close's copy `/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44/bin/kds_server-B`, mtime 10:59:19 (commit `9e568b44` is 10:56:45), **sha256 `85a168627b666a95c8bfddc7fd36949d58435b18003c258df8338dacc7dd536e`** |
| Binary C | built in a scratch worktree `ce-drain-variant` at `9e568b44` (removed afterwards), `src/server/command_dispatcher.cpp` with the three-line block `if (d_.select_row_emitted_for_test_) { ... ++rows_emitted_ ... }` deleted from the row visitor (`archive/.../C-vs-B.diff`); `cmake -DCMAKE_BUILD_TYPE=Release`, `--target kds_server -j4`, 73 s; mtime 11:54:00; **sha256 `f59f97f3c106e6500a6da30278db068936b5817cf44b4913a026158367cac7c3`**; 32 bytes smaller than B. The three binaries were copied to the run's `bin/` and every server started from the copy |
| Build type | Release for all three |
| Host | AMD EPYC 9V74, 8 vCPUs, Linux 7.0.0-1014-azure |
| Data device | `/dev/root`, **ext4** (`df -T $HOME`, checked at 11:52), data files under `/home/cdkbs/bench-runs/ce-drain-attribution-v3.3.0-52-g9e568b44/data/`, deleted after every visit; not tmpfs. Free space 50 GB; the 2 GB stop rule was armed |
| Server configuration | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, port 15761; no other key. Server pinned to CPU 2, client to CPU 6 (`taskset`), as the close |
| Quiet host | Before every visit a gate (no `cc1plus` / `cmake --build` / `ctest` / `kds_tests`, at most 8 % busy over 2 s, four checks in a row); every gate passed on its first four checks. A 1-second monitor (`monitor.log.gz`) checked every cell's window afterwards: no build or test process and no foreign process at 50 % or more in any cell window. Five monitor samples show this session's own `claude` process at 12 % to 31 % CPU (11:56:21, 11:58:31, 12:00:11 to 12:00:31); none is a heavy process, and the effect, if any, is inside the spreads below. Ports 15432 and 15434 untouched; servers stopped by recorded PID |
| Suite | not executed; no source in the tree under `src/` was changed (C was built in a scratch worktree) |
| PostgreSQL | not measured for this shape |

## 2. What was run, and what was cut

The workload is the close's `drain` path: `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N` (table of 60,480 rows, 69 per page), the client reading frame headers only and touching no cell. N = 1 (the control), 100, 1,000 and 10,000 rows; the 200 / 1K / 10K of rule 9 are the 100, 1,000 and 10,000 rows, as in the close. Each cell is one run of a fresh server on a fresh, freshly loaded data file, calibrated to about 2.5 s (1,794 to 74,898 statements per binary pooled over five visits), wall and server CPU (`/proc/<pid>/stat`, 10 ms ticks, 250 ticks per cell) per statement, every statement's latency.

| Round | Order | | Round | Order |
|---|---|---|---|---|
| 1 | A B C | | 4 | C B A |
| 2 | B C A | | 5 | B A C |
| 3 | C A B | | | |

"Spread" is (max - min) / median over a binary's five visits; **a difference smaller than the larger spread of the two binaries is not a finding.** Throughput is derived (`QPS = 1,000,000 / wall µs per statement`, exact for a serial single-connection driver).

**Cut** (20 minutes for the stage, building included; the measurement finished 9 minutes after the task started): the 60,480-row and filtered-walk shapes (the close's -5.9 % and +4.4 % cells; the task named 1 to 10,000 rows); the `send` and `split` client paths; any `cores`, `durability` or sizes other than the one configuration; a second window; an A-against-A control inside the run (the cross-window movement of A, section 4, is the control it would have supplied, at a larger distance); **the sampler** (`tools/kds_sampler.c`, `perf` being refused on this host), because the task ordered it only for the case where C equals B *and* B is slower than A, and B is not slower than A here. Also: the gate was four checks of 2 s (the close: six of 3 s) and the per-cell validity wait 2 s (the close: 3 s); **visit r1-A's first two cell passes failed at start** (the run's `json/` directory did not exist yet, no cell executed), so its measured pass ran 30 s after the server came up instead of 5 s; its numbers are in range with A's other four visits.

## 3. The result: B equals A, and C is not distinguishable from B

Throughput, statements per second, median over five visits (range in the second line of each cell; derived from wall per statement):

| shape | rows returned | A | B | C | B over A | C over A | C over B | spread A / B / C |
|---|---|---|---|---|---|---|---|---|
| range-1 (control) | 1 | 20,956 (20,469 to 21,317) | 20,859 (20,447 to 21,163) | 20,668 (20,400 to 21,145) | -0.5 % | -1.4 % | -0.9 % | 4.1 / 3.4 / 3.6 % |
| range-100 | 100 | 10,782 (10,662 to 10,885) | 10,756 (10,623 to 10,815) | 10,789 (10,599 to 10,863) | -0.2 % | +0.1 % | +0.3 % | 2.1 / 1.8 / 2.4 % |
| range-1000 | 1,000 | 1,996 (1,971 to 2,019) | 2,018 (2,012 to 2,035) | 2,047 (2,026 to 2,059) | +1.1 % | +2.6 % | +1.4 % | 2.4 / 1.2 / 1.6 % |
| range-10000 | 10,000 | 232 (223 to 233) | 236 (234 to 238) | 239 (231 to 243) | +1.7 % | +3.0 % | +1.3 % | 4.6 / 1.7 / 4.7 % |

Wall and server CPU per statement (µs), same medians, with the paired differences (same round, three servers in sequence; "+" counts the pairs in which the first binary was slower):

| shape | A wall | B wall | C wall | A server CPU | B server CPU | C server CPU | B-A wall | B-A pairs slower | C-B wall | C-B pairs slower |
|---|---|---|---|---|---|---|---|---|---|---|
| range-1 | 47.7 | 47.9 | 48.4 | 30.9 | 31.9 | 31.7 | +0.2 µs | 3 of 5 | +0.4 µs | 4 of 5 |
| range-100 | 92.7 | 93.0 | 92.7 | 76.0 | 75.7 | 75.1 | +0.2 µs | 2 of 5 | -0.3 µs | 2 of 5 |
| range-1000 | 501.0 | 495.4 | 488.5 | 478.8 | 473.8 | 466.2 | -5.5 µs | 0 of 5 | -6.9 µs | 0 of 5 |
| range-10000 | 4,319.6 | 4,234.7 | 4,186.6 | 4,305.6 | 4,213.6 | 4,158.8 | -84.9 µs | 0 of 5 | -48.1 µs | 1 of 5 |

- **B against A:** the sign is reversed from the close at 1,000 and 10,000 rows, and the magnitude (-1.1 %, -2.0 %) is inside A's spread (2.4 %, 4.6 %). At 100 rows and 1 row it is +0.2 µs and +0.2 µs, inside every spread. **B is not slower than A at any of the four sizes**; B is not demonstrably faster either.
- **C against B:** C is faster in 5 of 5 pairs at 1,000 rows (by 2.7 to 10.6 µs) and in 4 of 5 at 10,000 rows (by 8 to 112 µs, one pair 124 µs the other way), and the medians are -6.9 µs (1.4 %) and -48.1 µs (1.1 %), against spreads of 1.2 % / 1.6 % and 1.7 % / 4.7 %. The server CPU difference is 7.6 µs at 1,000 rows (7.6 ns per row), 54.8 µs at 10,000 rows (5.5 ns per row) and 0.5 µs at 100 rows (5 ns per row). A load and a branch per row costs roughly that, so the direction is plausible, but 1.4 % at the edge of the floor and 1.1 % inside it do not make a finding. A larger-N A/B of B against C at 1,000 rows would resolve it; the seam is set only by `tests/ce_streaming_reply_test.cpp` and "unset in production" (`command_dispatcher.hpp`), but those tests need it, so removing the per-row check means moving the seam (to a slice boundary, or out of the loop), which is a design call and not this run's.
- **The control moves as much as the cells do:** the 1-row cell, which no change here can affect, differs by up to 1.4 % between binaries with a 3.4 % to 4.1 % spread. That is this instrument's floor at 48 µs per statement.

Reply-complete distribution per statement, pooled over the five visits of a binary (rule 6; the pooled sets differ in size because each visit calibrates its own statement count):

| shape | binary | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| range-1 | A | 68,851 | 44.1 µs | 45.8 µs | 46.3 µs | 58.4 µs | 68.2 µs |
| range-1 | B | 74,898 | 40.9 µs | 45.8 µs | 46.5 µs | 57.7 µs | 67.9 µs |
| range-1 | C | 74,805 | 43.5 µs | 45.9 µs | 46.4 µs | 58.9 µs | 68.6 µs |
| range-100 | A | 43,902 | 82.9 µs | 88.4 µs | 91.2 µs | 106.6 µs | 124.2 µs |
| range-100 | B | 44,583 | 83.6 µs | 88.8 µs | 91.4 µs | 106.9 µs | 117.2 µs |
| range-100 | C | 43,689 | 82.5 µs | 88.4 µs | 91.3 µs | 106.5 µs | 118.4 µs |
| range-1000 | A | 16,844 | 467.3 µs | 486.0 µs | 496.7 µs | 522.5 µs | 581.0 µs |
| range-1000 | B | 17,217 | 468.7 µs | 483.7 µs | 489.1 µs | 515.0 µs | 653.2 µs |
| range-1000 | C | 17,321 | 465.7 µs | 480.5 µs | 484.7 µs | 507.6 µs | 528.6 µs |
| range-10000 | A | 1,794 | 4,210.2 µs | 4,277.0 µs | 4,337.1 µs | 4,524.5 µs | 4,687.8 µs |
| range-10000 | B | 2,570 | 4,085.0 µs | 4,172.3 µs | 4,216.4 µs | 4,325.4 µs | 4,479.0 µs |
| range-10000 | C | 2,586 | 4,063.6 µs | 4,117.6 µs | 4,146.8 µs | 4,392.9 µs | 4,652.3 µs |

p0 is the same for A and B at 100 and 1,000 rows (within 1.5 µs): the best case the path can reach has not moved. The 1,000-row p50 gaps (B 7.6 µs under A, C 4.4 µs under B) are the medians of the table above seen per statement; B's p99 (653 µs) has a longer tail than A's (581 µs) and C's (529 µs), and a tail of that kind is one stall in a 2.5 s cell, not a per-row cost.

## 4. Why the close saw a regression: A, not B, moved between windows

The same two binaries, the same statements, the same pinning, the same host, about an hour apart (medians of wall per statement over a binary's visits; the close's are three visits, this run's five):

| shape | A in the close | A here | B in the close | B here | A, window to window | B, window to window |
|---|---|---|---|---|---|---|
| range-100 | 93.5 µs | 92.7 µs | 94.8 µs | 93.0 µs | -0.9 % | -1.9 % |
| range-1000 | 477.5 µs | 501.0 µs | 497.7 µs | 495.4 µs | **+4.9 %** | -0.5 % |
| range-10000 | 4,263.8 µs | 4,319.6 µs | 4,375.3 µs | 4,234.7 µs | +1.3 % | -3.2 % |

At 1,000 rows A's server CPU also moved, 452.8 µs in the close and 478.8 µs here (+5.7 %), while B's did not (474.9 µs and 473.8 µs). The close's A visits at 1,000 rows (473 to 480 µs) and this run's (495 to 507 µs) do not overlap. **A binary that did not change moved 4.9 % at 1,000 rows between two windows, more than the 4.2 % attributed to CE; at 10,000 rows B moved 3.2 %, more than the 2.6 %.** The cause of the window-to-window movement is not measured: the host (it is a cloud VM), the kernel's placement of the server's memory, and the sequence of work before the cell are all candidates, and this run cannot tell them apart. What it does establish is that a single-window, three-visit A/B on a `drain` cell has a floor of at least 5 % between windows, above the 1 % to 4 % effect the close read from it, and that a regression claim on this path needs the control inside the window, which the close did not have and this run does.

## 5. Waits

A `drain` cell has no meaningful decomposition into the waits rule 3 names: it is a read-only `SELECT` with no commit (no fsync), no write statement, no lock or conflict wait, and the client reads headers only. The one split available is server CPU against the rest of the statement: at 1,000 rows server CPU is 95.6 % of wall in A (478.8 of 501.0 µs), 95.6 % in B (473.8 of 495.4) and 95.4 % in C (466.2 of 488.5); the remaining 4.4 % to 4.6 % is the client's frame reading and the socket round trip on a loopback, the same in all three.

## 6. Noise floor, validity and what was set aside

- **Floor from inside the run:** a binary's five visits spread 1.2 % to 4.8 % in wall per statement (control cell 3.4 % to 4.1 %), and the largest B-against-A or C-against-B difference at 1,000 rows or more is 2.0 % (B over A at 10,000 rows, -2.0 % against spreads of 4.6 % and 1.7 %). **No difference in this run's tables clears its spread**, except the sign pattern of the paired differences (section 3), which is not a magnitude.
- **Validity:** `verify` returned the expected row counts in all 15 visits (60 of 60 shapes). No cell was contaminated or set aside (the two failed passes of r1-A executed no cell). Gate waits were the minimum (6 s on the log's scale) in every visit.
- **Limits:** one cell per binary per shape per visit; five visits per binary; one configuration (`cores = 1`, `group`); a read-only path, so the finding does not extend to writes, and the `send` and `split` paths were not run.

## 7. What the run teaches about the engine

- **CE adds no per-reply or per-batch cost that a headers-only client can see at 100 to 10,000 rows.** B's `drain` wall equals A's within the run's floor at every size, and the 1,000-row cell, one slice and three batches with no boundary reached, shows B at 495.4 µs against A's 501.0 µs. That is consistent with the hypothesis this attribution started from, that below one slice everything CE adds is per statement and the only per-row addition is the seam check.
- **The per-row test seam costs on the order of 5 to 8 ns per row of server CPU, if anything,** about 1 % of a row's 470 ns of server CPU at 1,000 rows; it is a hook that only `tests/ce_streaming_reply_test.cpp` sets, and those tests need it, so whether the 1 % is worth moving the seam is a call for the owner of CE, after a larger A/B of B against C confirms it.
- **The close's `drain` finding is withdrawn by this run, not by a patch:** `results-ce-close-v3.3.0-52-g9e568b44.md` §5 reads a window effect on A as an engine cost. The close's headline numbers on the paths a decoding client uses (time to first batch, `send` and `split` wall) are not touched by this run.
- **An attribution that needs the sampler is open only if B is slower than A in a window with a control in it.** If a future close sees the gap again, the first step is C and B interleaved in that window; the sampler second.

## 8. Archive

`archive/ce-drain-attribution-v3.3.0-52-g9e568b44/`: `run_drain.sh`, `lib.sh`, `monitor.sh`, `watchdog.sh` (the 60-second watchdog's log is `watchdog.log`; it never saw the master dead before `all.done`), `rowcost/ce_s0_rows.py` (the close's driver with the `SHAPES_ONLY` filter and the 2 s validity wait), `cellvalid.py`, `analyze.py` and its output `analysis.md` (per-run values, the paired differences), `json/drain-cells.tar.gz` (every cell of every visit, with its statement latencies), `logs/r1-A.*` (the failed-pass visit), `timeline`, `monitor.log.gz`, `binary.sha256`, `binary-mtimes.txt`, `C-vs-B.diff`, the variant's build script and log. No data file, WAL segment or binary is archived.
