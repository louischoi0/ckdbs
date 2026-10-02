# AP-S5 — what AP costs at its last code commit, `c4b82e4` against `4012617`

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8: the interleaved A/B overhead
measurement over a milestone's whole code change, at its close. Run 2026-10-02
07:45–08:36 UTC on `worktrees/ap-s5-close` (branch `worktree-ap-s5-close`).
**B is `c4b82e4`** (`v2.7.0-600-gc4b82e4`, committed 2026-10-02 07:40:10 UTC),
AP's last code commit. **A is `4012617`** (`v2.7.0-573-g4012617`, committed
2026-10-02 04:37:35 UTC), AP's base. The worktree's HEAD was `0c3268f`
(`v2.7.0-606-g0c3268f`), tree clean; `git diff --stat c4b82e4 0c3268f -- src
include CMakeLists.txt` is empty, so B is the engine HEAD carries. This is
the first run of this driver (`tools/ap_overhead_benchmark.py`) and of these
shapes: it has no earlier kdbs number to read against, and **this file is the
baseline the next run of this driver is read against**. A is the baseline inside
the run.

**B − A is not AP alone** (the close's review). `4012617..c4b82e4` also carries
AZ-R5's engine commits `7ce9718`, `878f40a`, `e2340c4` and `9439497` (a failed
foreign-key check gives back its parent `S`), which touch `fk_check.hpp`,
`lock_table.hpp` and the dispatcher. Their path is foreign-key only and no cell
here reaches it; they count only under the code-layout reading below.

**The short answer.** `cores = 1`, `relaxed`, BTREE, one session, a fresh
server and data file per (cell, run). B − A is the median over runs of the
per-run difference of the client p50, with the pinned series (servers on CPU 2,
client on CPU 4; see *Noise*) beside the control that cannot be affected by AP
(`SHOW META`, the same session, the same blocks) and the same arm repeated.

| cell | what | rows | B − A, pinned (10 runs) | B − A, default placement (15 runs) | control `SHOW META`, pinned | the arm repeated, pinned | reading |
|---|---|---|---|---|---|---|---|
| C1 | `SELECT * FROM ap_t WHERE id = <k>`, literal differs per statement, 82 µs | 200 / 1,000 / 10,000 | +0.05 / +0.35 / +0.20 µs (+0.1 / +0.4 / +0.2 %) | +0.20 / +0.40 / 0.00 µs | −0.20 / +0.25 / −0.10 µs | −0.05 / +0.25 / +0.05 µs | **no cost resolvable**: inside the control's spread |
| C2 | the same with ten named columns (the skipped window), 83 µs | 200 / 1,000 / 10,000 | +0.30 / +0.15 / +0.35 µs | +0.80 / −0.10 / +0.40 µs | as above | (no repeat arm) | **no cost resolvable** |
| C3 | `SELECT id FROM ap_s WHERE v = <x>`, unindexed, one match: 132 µs / 745 µs / 4,107 µs | 1,000 / 10,000 / 60,000 | **+1.35 / +4.65 / +79.2 µs** (+1.0 / +0.6 / +1.9 %), 9 / 8 / 9 of 10 runs positive | +2.0 / +12.2 / +56.7 µs (IQR at 60,000 −14.9 to +94.8) | 0.00 / 0.00 / +0.25 µs | +1.40 / +4.60 / +75.95 µs | **a per-row cost: +1.4 / +0.5 / +1.3 ns a row examined**; the default series resolves it at 10,000 too (IQR +7.05 to +20.5, 13 of 15 positive) but not at 60,000 |
| C4 | `UPDATE ap_t SET a1 = <n> WHERE id = <k>`, 51–52 µs | 200 / 1,000 / 10,000 | +0.15 / +0.35 / −0.10 µs | +0.20 / −0.10 / +0.10 µs | −0.20 / +0.25 / −0.10 µs | +0.15 / +0.40 / +0.05 µs | **at the floor**: at most +0.4 µs (0.8 %), the control's size |
| C4 | `UPDATE ap_t SET a2 = <n> WHERE v = <7k>`, walks N rows, one match: 67 µs / 128 µs / 801 µs | 200 / 1,000 / 10,000 | −0.05 / −0.15 / −3.70 µs | −0.30 / −0.80 / −9.00 µs | as above | (no repeat arm) | **no cost**; the walk's per-row cost of C3 does not show here |
| info | `WHERE DATE(ts) = '<day>'` against `ts BETWEEN '<day> 00:00:00' AND '<day> 23:59:59.999999'`, B only | 1,000 / 10,000 / 60,000 | not a delta; see *Information only* | | | | DATE() 17.6 / 18.4 / 19.3 ns a row dearer than the same range as a scan; a `BETWEEN` can take an index, a function conjunct never does |

**What AP costs, in one paragraph.** On the per-statement paths every
statement takes — the second fingerprint state over the tokens after the select
list, `StatementIdentity` and the `fetch_id` trail key, `StatementContext`'s
clock read in an `UPDATE`/`DELETE`, `Compile()`'s clock read and
`DeterminismOf` fold — nothing this run can separate from its noise: a
pk point `SELECT` of 82 µs reads +0.05 to +0.35 µs, a ten-column one +0.15 to
+0.35 µs, a pk `UPDATE` of 52 µs −0.10 to +0.35 µs at the three row counts, and
the `SHOW META` control moved by −0.20 to +0.25 µs in the same runs, so
anything under ~0.5 µs (0.6 %) of the statement is not a finding. The engine's
own time per statement, read from `SHOW META`, moved by +0.3 to +0.5 µs of
14–20 µs on those paths and by +0.4 µs on the control. **The one cost the run
resolves is per row, on a statement that walks and rejects: a filter scan pays
B − A = +1.4, +0.5 and +1.3 ns for each row it examines at 1,000, 10,000 and
60,000 rows (+79 µs of 4,107 µs, +1.9 %, at 60,000; 9 of 10 pinned runs
positive, IQR +37.5 to +109.4 µs)**, all of it inside the engine (the engine's
own time rose by +80 µs of 4,016 µs, the client's by +75 µs). The cost is
real in sign and small in size, it does not grow linearly with the row count
(0.47 ns at 10,000 against 1.3 ns at 60,000), and **no line AP added runs on a
rejected row**: `AcceptTupleAt`'s `fn_residual` test sits after the residual's
reject (`src/exec/step_vm.cpp:2280` at `c4b82e4`), and `FilterColumnsOf`/
`ReadColumnsOf` run at compile only. Unattributed; code layout is the reading
left (the close's review).
It does not show in the `UPDATE` that walks (−3.7 µs of 801 µs at 10,000 rows),
whose per-row work is the same walk. Measured at one core, `relaxed`, one
serial session; nothing here says what `group` or `strict` durability, more
than one core or more than one session do to any of it.

---


**Rule 4 and the exclusion rule, as run** (the close's review). The per-cell
`/proc/loadavg` and `pgrep` evidence is in the archive's `host.txt`, not tabled
in this file as the README asks - a departure, kept rather than regenerated. A
run was excluded when its one-minute load after the cell exceeded 1.3 or a
competing process was seen. `point-n200-run14` went out on an after-load of
1.36 (kept runs reached 1.23). `pin-scan-n60000-run3` had a
`cmake --build -j8` from `ba-s1-peer-writeback-gate` running before the cell
started (the pair script's 180 s wait ran out). The excluded pinned-scan runs
read −181 µs at 60,000 rows and +11.7 µs at 10,000, and are in no number above.

## What was measured

All statements go through the KWP/1 client (`tools/ckdbs_cli.py`'s
`ServerConnection`), serially, one session per server. Each arm sends a
different literal every time, so each statement is parsed, fingerprinted and
dispatched fresh; the driver is `tools/ap_overhead_benchmark.py` (usage in its
docstring, `bench/docs/` has no entry for it, which `bench/README.md` says is
the state until a v3 driver's behaviour is not already in its `--help`).
Row-set size is `--rows N` (the table holds exactly N rows; point and update
cells 200 / 1,000 / 10,000, scan cell 1,000 / 10,000 / 60,000, the function
cell 1,000 / 10,000 / 60,000 spread over 100 UTC days, i.e. 10 / 100 / 600
rows a day).

| cell | `--cell` | table | arms |
|---|---|---|---|
| C1, C2, C4 | `point` | `ap_t (id, v, a1..a9)`, eleven `int64`, `v = 7 id`, unindexed | `point-star`, `point-star-again`, `point-wide`, `update-pk`, `update-pk-again`, `update-walk`, `ping` |
| C3 | `scan` | `ap_s (id, v, w)` | `scan-reject`, `scan-reject-again`, `ping` |
| info | `fn` (B only), with `--index` for the indexed variant | `ap_f (id, ts timestamp, v)` | `fn-date`, `between`, `fn-date-again`, `ping` |

An arm runs 16 (point), 10 (scan) or 12 (fn) blocks; inside a block one server
runs its statements and then the other, and the order alternates per block, so
at most one server is active at a time and a drifting host favours neither.
Per arm and server: 8,000 statements (point, 992 for the two walking arms),
1,000 (scan), 600 (fn). Each server's `SHOW META` is read around every block.
Waystone, Cabin and the optimizer are at the server's defaults; the driver
creates no Cabin and no index except in the `--index` variant. Both engines
registered a `sys.patterns` row for the point shapes within the first few
hundred statements of an arm (the probe is in the archive,
`waystone-probe/`); the driver does not count replay hits.

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** Both arms built `Release`
   (`CMakeCache.txt` `CMAKE_BUILD_TYPE:STRING=Release`) from `git archive` of the
   commit into `/home/cdkbs/bench-runs/ap-s5/src-A` and `src-B`, target
   `kds_server` only, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` (`build.sh`),
   alone on the host, before any measurement; nothing was built during a run.
   `build/` of this worktree (Debug) was not touched.
2. **Block device.** Data files under `/home/cdkbs/bench-runs/ap-s5/` on
   `/dev/root`, ext4 (`df -T` at the start of the run: `/` and `/tmp` are both
   `/dev/root` ext4 here).
3. **A copy of the binary, hashed.** Every server started from the copy:

   | arm | commit | describe | binary | sha256 | source binary mtime |
   |---|---|---|---|---|---|
   | A | `4012617` (AP's base, 2026-10-02 04:37:35 UTC) | `v2.7.0-573-g4012617` | `kds_server-A` | `309fc46dc1eddf084c20aef86f5ad1d0bf203fcb9a31b3c80a2c3a1b9b9a888d` | 2026-10-02 07:42:14 UTC |
   | B | `c4b82e4` (AP's last code commit, 2026-10-02 07:40:10 UTC) | `v2.7.0-600-gc4b82e4` | `kds_server-B` | `5e860fafbfd1371c596d4f49a762194319b559d27388b2d151d280341e7b0c7e` | 2026-10-02 07:42:54 UTC |

   Both binaries are newer than the commit they name and were built from an
   exact `git archive` tree, so neither is older than its commit. A's sha256 is
   the one the AZ-S7 file records for its B binary (`9f170b1`); `4012617` is
   AP's base and the engine at `4012617` is byte for byte that build.
4. **Host load, per cell.** Before every run `pair.sh` waited (bounded, 180 s)
   for no `cc1plus|cmake --build|ctest` process and a one-minute load below 1.0;
   `/proc/loadavg` and the `pgrep` result are in every `host.txt` and every
   driver JSON in the archive. **No competing build in any run kept.** One-minute
   load before the runs kept: 0.28–0.99 (default series 0.46–0.99, pinned
   0.70–0.98, function cells 0.28–0.97), after 0.28–1.23; the driver's own
   thread accounts for about 1 of it. **Five cells were loaded and were re-run**, their first runs kept in the archive as `excluded-*` and left out
   of every number here: default `point` rows 1,000 run 7 (one-minute load 3.49
   before the cell, no build visible; another session's work, the wait ran out)
   and rows 200 run 14 (1.36 after), pinned `point` rows 10,000 run 2 (1.64
   after), pinned `scan` rows 10,000 run 2 (1.53 before) and rows 60,000 run 3
   (3.23 after). The replacements ran at 0.71–0.94 before.
5. **Ports chosen.** 15610 (B) and 15611 (A) for every pair. Not 15432 (an
   unrelated `kds_server` under `/home/cdkbs/autotrade` was running and was not
   touched).

Host: 8 CPUs (AMD EPYC 9V74, 2 threads a core, CPUs 2-3 and 4-5 are two
sibling pairs). Every server `cores = 1`, `durability = relaxed`,
`log_level = warn`, otherwise defaults; the config is in every cell's
`A.conf`/`B.conf`. **A fresh server and data file for every (cell, run)**: 15
runs of each default-placement point and scan cell, 10 of each pinned cell, 3
of each function cell. The suite was **not executed** (no engine file was
touched, only a driver added); correctness of the measured statements is
witnessed by zero error replies in every arm of every run and by every
`UPDATE` reply being `UPDATED 1` (the driver counts them: 0 exceptions in all
runs). `strict` and `group` were not measured, `cores > 1` was not measured, and
no concurrent session was run.

## Noise

**Default placement has a bimodal server, and pinning removes it.** In the 180 server-runs of the default point and scan cells, 16 (8 on A, 8 on B) had `SHOW META` at a p50 of 60–64 µs where the other 164 read 37–50 µs; a whole server process sits
in one mode or the other, which is why one run's B − A of `ping` reads ±24 µs
and why the default series' IQRs are wide (−22 µs in some `update-pk` cells).
In the 120 pinned server-runs (`taskset -c 2` on every server, `taskset -c 4`
on the driver) none did, and the IQRs of the differences fall to ±0.5 µs on a
point select. Pinning is a harness choice the order did not name, so both series
are reported; it does not change an engine, only where the scheduler puts the
two threads, and it is applied to both arms equally. The default-placement
series is the one that answers "as the server runs"; the pinned series is the one
whose floor is small enough to resolve a 1 % effect.

**The floor, from inside the run**: the `ping` control's B − A is −0.20 to
+0.25 µs of 38 µs (pinned, 10 runs a cell, six cells), −0.5 to +0.3 µs (default);
the repeated arm's is −0.05 to +0.40 µs. Anything below ~0.5 µs on a point
statement is not a finding here.

**Stalls.** In the `point-wide` arm one statement took 21–144 ms in 15 of 30 server-runs at each of 200, 1,000 and 10,000 rows in the default series, on both engines (A / B of 15: 9 / 6, 9 / 6, 7 / 8); pinned, A / B of 10 are 7 / 3, 1 / 8 and 3 / 6 (rows 1,000 is the one cell where the two sides differ clearly). Most `update` arms had one statement of 5–20 ms on both (two of 79 and 103 ms). They land once an arm, they are on both
engines, they inflate means and the driver's ops/elapsed `qps` and not the
p50, so the matrix's QPS is derived from p50 (1e6 / p50 µs) and says so. A
probe of 3,000 consecutive point selects (`waystone-probe/stall.txt`) on a
quiet server showed none, so the cause is not identified; it was not read as
AP's because the two engines show it and the pinned rows 1,000 split points
the opposite way from the default one.

## C1, C2 — the point `SELECT`: nothing resolvable

The parse-bound cell and the cell that prices the skipped window read the same
as A, at three row counts, in both series. Latency at the client (µs), each
cell the median over the 15 default-placement runs of that run's percentile,
8,000 statements a run:

| rows | arm | commit | ops per run | p0 | p25 | p50 | p90 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| 200 | point-star | A `4012617` | 8,000 | 76.0 µs | 81.0 µs | 82.3 µs | 97.9 µs | 110.4 µs | 124.1 µs |
| 200 | point-star | B `c4b82e4` | 8,000 | 76.5 µs | 81.1 µs | 82.8 µs | 112.0 µs | 115.8 µs | 132.8 µs |
| 200 | point-wide | A `4012617` | 8,000 | 79.9 µs | 82.7 µs | 83.8 µs | 111.4 µs | 117.6 µs | 135.3 µs |
| 200 | point-wide | B `c4b82e4` | 8,000 | 80.6 µs | 83.0 µs | 84.1 µs | 115.7 µs | 119.3 µs | 137.3 µs |
| 200 | update-pk | A `4012617` | 8,000 | 43.2 µs | 51.8 µs | 52.7 µs | 76.0 µs | 77.5 µs | 91.6 µs |
| 200 | update-pk | B `c4b82e4` | 8,000 | 43.6 µs | 51.9 µs | 52.7 µs | 61.1 µs | 68.3 µs | 78.4 µs |
| 200 | update-walk | A `4012617` | 992 | 62.7 µs | 67.4 µs | 68.2 µs | 79.4 µs | 87.5 µs | 96.8 µs |
| 200 | update-walk | B `c4b82e4` | 992 | 62.0 µs | 67.2 µs | 67.9 µs | 79.2 µs | 85.9 µs | 96.1 µs |
| 1,000 | point-star | A `4012617` | 8,000 | 76.3 µs | 81.3 µs | 82.9 µs | 108.9 µs | 114.8 µs | 133.1 µs |
| 1,000 | point-star | B `c4b82e4` | 8,000 | 77.9 µs | 81.5 µs | 83.6 µs | 113.1 µs | 116.4 µs | 133.5 µs |
| 1,000 | point-wide | A `4012617` | 8,000 | 79.7 µs | 83.0 µs | 85.1 µs | 116.3 µs | 120.1 µs | 137.1 µs |
| 1,000 | point-wide | B `c4b82e4` | 8,000 | 80.8 µs | 83.3 µs | 84.8 µs | 116.9 µs | 119.7 µs | 137.5 µs |
| 1,000 | update-pk | A `4012617` | 8,000 | 43.4 µs | 52.0 µs | 53.0 µs | 75.6 µs | 77.5 µs | 93.1 µs |
| 1,000 | update-pk | B `c4b82e4` | 8,000 | 43.4 µs | 51.8 µs | 52.9 µs | 75.6 µs | 77.2 µs | 90.0 µs |
| 1,000 | update-walk | A `4012617` | 992 | 119.1 µs | 126.5 µs | 128.8 µs | 147.6 µs | 151.6 µs | 176.0 µs |
| 1,000 | update-walk | B `c4b82e4` | 992 | 118.4 µs | 125.8 µs | 127.2 µs | 143.1 µs | 149.6 µs | 171.9 µs |
| 10,000 | point-star | A `4012617` | 8,000 | 75.4 µs | 80.8 µs | 82.1 µs | 95.5 µs | 107.1 µs | 123.5 µs |
| 10,000 | point-star | B `c4b82e4` | 8,000 | 75.6 µs | 80.6 µs | 81.8 µs | 98.1 µs | 111.4 µs | 127.1 µs |
| 10,000 | point-wide | A `4012617` | 8,000 | 79.9 µs | 82.3 µs | 83.5 µs | 114.4 µs | 116.9 µs | 133.7 µs |
| 10,000 | point-wide | B `c4b82e4` | 8,000 | 79.5 µs | 82.2 µs | 83.7 µs | 115.6 µs | 118.3 µs | 136.1 µs |
| 10,000 | update-pk | A `4012617` | 8,000 | 43.8 µs | 52.6 µs | 53.5 µs | 65.7 µs | 77.2 µs | 84.9 µs |
| 10,000 | update-pk | B `c4b82e4` | 8,000 | 43.5 µs | 52.3 µs | 53.3 µs | 67.0 µs | 76.2 µs | 84.5 µs |
| 10,000 | update-walk | A `4012617` | 992 | 766.3 µs | 790.2 µs | 799.2 µs | 830.7 µs | 840.5 µs | 967.7 µs |
| 10,000 | update-walk | B `c4b82e4` | 992 | 763.5 µs | 784.4 µs | 791.2 µs | 821.7 µs | 830.3 µs | 952.5 µs |

Reading it: for the point selects and the pk `UPDATE`, p0 to p50 are within
2 µs of each other on the two engines in every row (the walking `UPDATE`'s p50
differs by up to 8 µs at 10,000 rows, B lower); p90–p99 differ by more than that in the default series, in both directions (`point-star`, 200 rows: p90 97.9 µs on A against 112.0 µs on B; `update-pk`, 200 rows: p90 76.0 µs on A against 61.1 µs on B; `point-star`, 10,000 rows: p99 123.5 against 127.1 µs), which is the signature of the placement mode and the stalls and not of an engine.

**Accounting of a point select** (rule 3): of the pinned 82.2 µs client p50 at
1,000 rows, the engine's own time (`sched_foreground_polled_us` per statement)
is 17.1 µs on A and 17.4 µs on B, and the remaining 65 µs is client, socket and
the scheduler's wake: a `SHOW META` costs 11.9 µs of engine time for a
37.7 µs round trip, so a `SHOW META` accounts for 26 µs of the 65 µs outside the engine's polled time; the other ~39 µs is what a row-returning statement adds there and is not decomposed further. Durability: none (`relaxed`, no fsync on the read path); lock wait: none
(one session). The consequence for resolution: AP's per-statement additions
live in the 17 µs, and the measured +0.3 µs engine delta is 1.8 % of it, which
is a larger share than the 0.4 % it is of the client's number; the engine
column is the better thermometer for these cells and it too reads inside the
`SHOW META` control (+0.4 µs).

## C3 — the filter scan: a small per-row cost

The scan rejects 59,999 of 60,000 rows and returns one, so what it prices is
the per-row accept test plus the walk. B is slower than A in the pinned
series in 9 of 10 runs at 1,000 and 60,000 rows and 8 of 10 at 10,000, both
arms alike:

| rows | A p50 | B p50 | B − A (IQR), pinned | per row examined | engine time B − A | default placement B − A (IQR) |
|---|---|---|---|---|---|---|
| 1,000 | 132.1 µs | 133.6 µs | +1.35 µs (+0.50 to +2.25) | +1.4 ns | +1.1 µs of 88.8 µs | +2.00 µs (−0.85 to +9.00) |
| 10,000 | 744.2 µs | 749.2 µs | +4.65 µs (+2.40 to +7.30) | +0.47 ns | +2.6 µs of 695.0 µs | +12.2 µs (+7.05 to +20.5) |
| 60,000 | 4,106.8 µs | 4,181.8 µs | +79.2 µs (+37.5 to +109.4) | +1.3 ns | +79.8 µs of 4,016.3 µs | +56.7 µs (−14.9 to +94.8) |

QPS, derived from p50: 7,570 / 1,344 / 243 on A and 7,485 / 1,335 / 239 on B
at 1,000 / 10,000 / 60,000 rows. The repeated arm gives +1.40 / +4.60 / +75.95 µs,
which is the same number again and so is a repeat and not an independent
control; the independent control is `ping` (0.00 / 0.00 / +0.25 µs). The cost
is not linear in rows: 0.37 ns a row between 1,000 and 10,000 and 1.5 ns a row
between 10,000 and 60,000. At 60,000 rows the relation is ~3 MB of tuples and
about six times what it is at 10,000. No line AP added runs per rejected row (the `fn_residual` test follows the residual's reject at `step_vm.cpp:2280`; the column masks are compile-time), so what is left is the `Step` grown by a vector and code layout - a reading, not a measurement. The run did not isolate it (no profile, no
mutated build). The default-placement series agrees in sign and mean but its IQR
crosses zero at 60,000 rows because the slow-mode runs add ±100–600 µs
(+572 and +613 µs in one run), which is why the pinned series carries the
finding.

Latency, default placement (µs, median over 15 runs of each percentile, 1,000
statements a run):

| rows | arm | commit | ops per run | p0 | p25 | p50 | p90 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| 1,000 | scan-reject | A `4012617` | 1,000 | 128.6 µs | 131.4 µs | 133.8 µs | 153.0 µs | 162.3 µs | 176.7 µs |
| 1,000 | scan-reject | B `c4b82e4` | 1,000 | 130.6 µs | 133.0 µs | 134.7 µs | 151.7 µs | 159.1 µs | 173.3 µs |
| 10,000 | scan-reject | A `4012617` | 1,000 | 712.3 µs | 731.9 µs | 738.9 µs | 770.7 µs | 778.2 µs | 823.5 µs |
| 10,000 | scan-reject | B `c4b82e4` | 1,000 | 721.7 µs | 746.8 µs | 755.9 µs | 787.8 µs | 792.6 µs | 871.3 µs |
| 60,000 | scan-reject | A `4012617` | 1,000 | 3,974.4 µs | 4,036.7 µs | 4,068.3 µs | 4,157.7 µs | 4,181.2 µs | 4,391.0 µs |
| 60,000 | scan-reject | B `c4b82e4` | 1,000 | 4,014.7 µs | 4,102.4 µs | 4,121.9 µs | 4,204.4 µs | 4,327.8 µs | 4,559.4 µs |

**Accounting**: at 60,000 rows the engine's own time is 98 % of the 4,107 µs
(4,016 µs engine, ~91 µs client round trip and reply), so this cell is
engine-bound, and the cost AP adds is in the engine's walk. At 1,000 rows the
engine is 67 % (88.8 of 132.1 µs). Durability and lock waits: none (a read, one
session).

## C4 — the `UPDATE`: at the floor

A pk `UPDATE` pays a `StatementContext` per statement (a `system_clock` read)
and the compile's own clock read; the pinned B − A is +0.15 / +0.35 / −0.10 µs of
51.5–52.3 µs at 200 / 1,000 / 10,000 rows, the repeat +0.15 / +0.40 / +0.05 µs,
against `ping` −0.20 / +0.25 / −0.10 µs. At 1,000 rows both `update-pk` arms
have an IQR above zero (+0.05 to +0.95 and +0.12 to +0.77 µs, 7 and 8 of 10 runs
positive) and the control's IQR is −0.03 to +0.48 µs: a tendency of at most +0.4 µs
(0.8 %) that the control is half the size of, and it is absent at 200 and 10,000
rows. It is not called a cost. The `UPDATE` that walks N rows looks for one
`v`, so the walk is C3's walk: it reads −0.05 / −0.15 / −3.70 µs (of 67 / 128 / 801
µs), the opposite sign to C3 at 10,000 rows, with an IQR that contains zero
(−6.2 to +4.1 µs) — while C3's +4.65 µs at 10,000 rows would sit just above that IQR; the walking `UPDATE` does not show the scan's per-row cost and the run does not say why. Engine time, pinned, rows 1,000: `update-pk` 14.2 µs on A and 14.7 µs on B; `update-walk` 91.5 and 91.2 µs. The `update-pk` and `update-walk` latency distributions are in the first table above.



QPS, derived from p50, pinned: `update-pk` 19,493 / 19,531 / 19,157 on A and
19,417 / 19,305 / 19,120 on B at 200 / 1,000 / 10,000 rows (full matrix below).

## The matrix, every arm

One row per (cell, rows, arm). QPS is derived (1e6 / p50 µs) because the
driver's own ops/elapsed carries the stalls above. "B − A" is the median over
runs of the per-run difference of the p50, with the interquartile range; "B − A as % of A" is that as a share of A's median p50.

**Pinned series** (servers on CPU 2, driver on CPU 4; 10 runs per cell):

| cell | rows | arm | runs | A qps (derived 1e6/p50) | B qps (derived) | A p50 | B p50 | B − A, median of per-run p50 differences (IQR) | B − A as % of A | runs with B > A |
|---|---|---|---|---|---|---|---|---|---|---|
| point | 200 | point-star | 10 | 12,277 | 12,293 | 81.5 µs | 81.3 µs | +0.05 µs (-0.35 to +0.50) | +0.06 % | 5/10 |
| point | 200 | point-star-again | 10 | 12,285 | 12,293 | 81.4 µs | 81.3 µs | -0.05 µs (-0.28 to +0.47) | -0.06 % | 4/10 |
| point | 200 | point-wide | 10 | 12,048 | 12,026 | 83.0 µs | 83.2 µs | +0.30 µs (-0.07 to +0.50) | +0.36 % | 6/10 |
| point | 200 | update-pk | 10 | 19,493 | 19,436 | 51.3 µs | 51.5 µs | +0.15 µs (+0.10 to +0.47) | +0.29 % | 8/10 |
| point | 200 | update-pk-again | 10 | 19,474 | 19,417 | 51.3 µs | 51.5 µs | +0.15 µs (+0.00 to +0.58) | +0.29 % | 6/10 |
| point | 200 | update-walk | 10 | 14,892 | 14,892 | 67.2 µs | 67.2 µs | -0.05 µs (-0.43 to +0.30) | -0.07 % | 3/10 |
| point | 200 | ping | 10 | 26,596 | 26,810 | 37.6 µs | 37.3 µs | -0.20 µs (-0.37 to -0.10) | -0.53 % | 1/10 |
| point | 1,000 | point-star | 10 | 12,158 | 12,107 | 82.2 µs | 82.6 µs | +0.35 µs (+0.10 to +0.80) | +0.43 % | 8/10 |
| point | 1,000 | point-star-again | 10 | 12,180 | 12,136 | 82.1 µs | 82.4 µs | +0.25 µs (-0.27 to +0.40) | +0.30 % | 7/10 |
| point | 1,000 | point-wide | 10 | 11,947 | 11,905 | 83.7 µs | 84.0 µs | +0.15 µs (-0.07 to +0.60) | +0.18 % | 5/10 |
| point | 1,000 | update-pk | 10 | 19,531 | 19,286 | 51.2 µs | 51.8 µs | +0.35 µs (+0.05 to +0.95) | +0.68 % | 7/10 |
| point | 1,000 | update-pk-again | 10 | 19,531 | 19,361 | 51.2 µs | 51.7 µs | +0.40 µs (+0.12 to +0.77) | +0.78 % | 8/10 |
| point | 1,000 | update-walk | 10 | 7,803 | 7,837 | 128.1 µs | 127.6 µs | -0.15 µs (-1.48 to +0.95) | -0.12 % | 5/10 |
| point | 1,000 | ping | 10 | 26,525 | 26,385 | 37.7 µs | 37.9 µs | +0.25 µs (-0.03 to +0.48) | +0.66 % | 7/10 |
| point | 10,000 | point-star | 10 | 12,376 | 12,346 | 80.8 µs | 81.0 µs | +0.20 µs (-0.13 to +0.55) | +0.25 % | 7/10 |
| point | 10,000 | point-star-again | 10 | 12,392 | 12,338 | 80.7 µs | 81.1 µs | +0.05 µs (-0.25 to +0.43) | +0.06 % | 5/10 |
| point | 10,000 | point-wide | 10 | 12,114 | 12,070 | 82.5 µs | 82.8 µs | +0.35 µs (-0.07 to +0.57) | +0.42 % | 6/10 |
| point | 10,000 | update-pk | 10 | 19,157 | 19,102 | 52.2 µs | 52.3 µs | -0.10 µs (-0.18 to +0.55) | -0.19 % | 4/10 |
| point | 10,000 | update-pk-again | 10 | 19,120 | 19,120 | 52.3 µs | 52.3 µs | +0.05 µs (-0.25 to +0.42) | +0.10 % | 5/10 |
| point | 10,000 | update-walk | 10 | 1,248 | 1,253 | 801.4 µs | 798.2 µs | -3.70 µs (-6.22 to +4.07) | -0.46 % | 4/10 |
| point | 10,000 | ping | 10 | 26,455 | 26,455 | 37.8 µs | 37.8 µs | -0.10 µs (-0.18 to -0.02) | -0.26 % | 2/10 |
| scan | 1,000 | scan-reject | 10 | 7,573 | 7,482 | 132.1 µs | 133.6 µs | +1.35 µs (+0.50 to +2.25) | +1.02 % | 9/10 |
| scan | 1,000 | scan-reject-again | 10 | 7,584 | 7,499 | 131.9 µs | 133.4 µs | +1.40 µs (+0.60 to +1.97) | +1.06 % | 9/10 |
| scan | 1,000 | ping | 10 | 26,560 | 26,667 | 37.7 µs | 37.5 µs | +0.00 µs (-0.10 to +0.10) | +0.00 % | 4/10 |
| scan | 10,000 | scan-reject | 10 | 1,344 | 1,335 | 744.2 µs | 749.2 µs | +4.65 µs (+2.40 to +7.30) | +0.62 % | 8/10 |
| scan | 10,000 | scan-reject-again | 10 | 1,342 | 1,335 | 745.4 µs | 749.2 µs | +4.60 µs (+1.17 to +7.78) | +0.62 % | 8/10 |
| scan | 10,000 | ping | 10 | 26,525 | 26,596 | 37.7 µs | 37.6 µs | -0.00 µs (-0.20 to +0.10) | -0.00 % | 5/10 |
| scan | 60,000 | scan-reject | 10 | 244 | 239 | 4,106.8 µs | 4,181.8 µs | +79.20 µs (+37.50 to +109.37) | +1.93 % | 9/10 |
| scan | 60,000 | scan-reject-again | 10 | 243 | 239 | 4,120.7 µs | 4,189.7 µs | +75.95 µs (+43.20 to +96.95) | +1.84 % | 9/10 |
| scan | 60,000 | ping | 10 | 26,560 | 26,490 | 37.7 µs | 37.8 µs | +0.25 µs (+0.02 to +0.38) | +0.66 % | 7/10 |

**Default placement** (15 runs per cell):

| cell | rows | arm | runs | A qps (derived 1e6/p50) | B qps (derived) | A p50 | B p50 | B − A, median of per-run p50 differences (IQR) | B − A as % of A | runs with B > A |
|---|---|---|---|---|---|---|---|---|---|---|
| point | 200 | point-star | 15 | 12,151 | 12,077 | 82.3 µs | 82.8 µs | +0.20 µs (-0.25 to +2.60) | +0.24 % | 8/15 |
| point | 200 | point-star-again | 15 | 12,107 | 12,107 | 82.6 µs | 82.6 µs | +0.10 µs (-0.80 to +1.35) | +0.12 % | 8/15 |
| point | 200 | point-wide | 15 | 11,933 | 11,891 | 83.8 µs | 84.1 µs | +0.80 µs (-0.60 to +1.35) | +0.95 % | 9/15 |
| point | 200 | update-pk | 15 | 18,975 | 18,975 | 52.7 µs | 52.7 µs | +0.20 µs (-3.15 to +0.55) | +0.38 % | 8/15 |
| point | 200 | update-pk-again | 15 | 19,084 | 18,939 | 52.4 µs | 52.8 µs | +0.40 µs (-0.25 to +0.85) | +0.76 % | 11/15 |
| point | 200 | update-walk | 15 | 14,663 | 14,728 | 68.2 µs | 67.9 µs | -0.30 µs (-0.65 to +0.20) | -0.44 % | 4/15 |
| point | 200 | ping | 15 | 26,247 | 26,178 | 38.1 µs | 38.2 µs | +0.00 µs (-0.60 to +0.50) | +0.00 % | 7/15 |
| point | 1,000 | point-star | 15 | 12,063 | 11,962 | 82.9 µs | 83.6 µs | +0.40 µs (-0.80 to +2.20) | +0.48 % | 9/15 |
| point | 1,000 | point-star-again | 15 | 12,121 | 12,063 | 82.5 µs | 82.9 µs | +0.50 µs (+0.10 to +0.85) | +0.61 % | 12/15 |
| point | 1,000 | point-wide | 15 | 11,751 | 11,792 | 85.1 µs | 84.8 µs | -0.10 µs (-1.30 to +1.00) | -0.12 % | 7/15 |
| point | 1,000 | update-pk | 15 | 18,868 | 18,904 | 53.0 µs | 52.9 µs | -0.10 µs (-21.70 to +0.40) | -0.19 % | 7/15 |
| point | 1,000 | update-pk-again | 15 | 18,975 | 19,011 | 52.7 µs | 52.6 µs | -0.40 µs (-0.80 to +0.45) | -0.76 % | 6/15 |
| point | 1,000 | update-walk | 15 | 7,764 | 7,862 | 128.8 µs | 127.2 µs | -0.80 µs (-6.90 to +0.35) | -0.62 % | 5/15 |
| point | 1,000 | ping | 15 | 26,110 | 26,042 | 38.3 µs | 38.4 µs | +0.30 µs (-3.85 to +0.40) | +0.78 % | 9/15 |
| point | 10,000 | point-star | 15 | 12,180 | 12,225 | 82.1 µs | 81.8 µs | +0.00 µs (-1.15 to +0.15) | +0.00 % | 6/15 |
| point | 10,000 | point-star-again | 15 | 12,195 | 12,255 | 82.0 µs | 81.6 µs | -0.30 µs (-2.20 to +0.35) | -0.37 % | 6/15 |
| point | 10,000 | point-wide | 15 | 11,976 | 11,947 | 83.5 µs | 83.7 µs | +0.40 µs (-0.30 to +1.00) | +0.48 % | 8/15 |
| point | 10,000 | update-pk | 15 | 18,692 | 18,762 | 53.5 µs | 53.3 µs | +0.10 µs (-0.80 to +0.70) | +0.19 % | 8/15 |
| point | 10,000 | update-pk-again | 15 | 18,762 | 18,762 | 53.3 µs | 53.3 µs | +0.40 µs (-0.45 to +1.45) | +0.75 % | 8/15 |
| point | 10,000 | update-walk | 15 | 1,251 | 1,264 | 799.2 µs | 791.2 µs | -9.00 µs (-15.85 to +5.60) | -1.13 % | 5/15 |
| point | 10,000 | ping | 15 | 26,110 | 26,247 | 38.3 µs | 38.1 µs | -0.30 µs (-2.15 to +0.15) | -0.78 % | 5/15 |
| scan | 1,000 | scan-reject | 15 | 7,474 | 7,424 | 133.8 µs | 134.7 µs | +2.00 µs (-0.85 to +9.00) | +1.49 % | 9/15 |
| scan | 1,000 | scan-reject-again | 15 | 7,446 | 7,413 | 134.3 µs | 134.9 µs | +0.90 µs (-0.40 to +7.30) | +0.67 % | 10/15 |
| scan | 1,000 | ping | 15 | 26,110 | 26,178 | 38.3 µs | 38.2 µs | -0.20 µs (-0.20 to +1.30) | -0.52 % | 5/15 |
| scan | 10,000 | scan-reject | 15 | 1,353 | 1,323 | 738.9 µs | 755.9 µs | +12.20 µs (+7.05 to +20.50) | +1.65 % | 13/15 |
| scan | 10,000 | scan-reject-again | 15 | 1,336 | 1,330 | 748.5 µs | 751.6 µs | +1.70 µs (-6.00 to +11.00) | +0.23 % | 8/15 |
| scan | 10,000 | ping | 15 | 26,110 | 26,316 | 38.3 µs | 38.0 µs | -0.50 µs (-1.15 to -0.30) | -1.31 % | 3/15 |
| scan | 60,000 | scan-reject | 15 | 246 | 243 | 4,068.3 µs | 4,121.9 µs | +56.70 µs (-14.90 to +94.75) | +1.39 % | 10/15 |
| scan | 60,000 | scan-reject-again | 15 | 246 | 243 | 4,064.9 µs | 4,119.0 µs | +36.20 µs (-11.45 to +116.55) | +0.89 % | 10/15 |
| scan | 60,000 | ping | 15 | 26,110 | 26,042 | 38.3 µs | 38.4 µs | +0.00 µs (-0.30 to +16.20) | +0.00 % | 7/15 |

## Information only, B alone — what a function costs

`DATE(ts)` is a scalar function in a `WHERE` comparison, and the manual
(`manual/sql/sql.md`) says it filters rows found some other way and never
becomes a key, an index range or a Cabin probe. A has no function, so this is
**not a delta**: it is B (`c4b82e4`) against itself, three runs a size, the
rows of each day checked identical between the two forms before anything was
timed (0 of 100 days differed, at every size; 10 / 100 / 600 rows a day).
Client p50, µs, median over three runs:

| rows | form | table has an index on `ts` | QPS (derived) | p0 | p25 | p50 | p90 | p95 | p99 | ops per run |
|---|---|---|---|---|---|---|---|---|---|---|
| 1,000 | `DATE(ts) = day` | no | 5,865 qps | 166.7 µs | 168.8 µs | 170.5 µs | 186.0 µs | 188.3 µs | 200.5 µs | 600 |
| 1,000 | `ts BETWEEN` | no | 6,540 qps | 144.5 µs | 149.9 µs | 152.9 µs | 176.3 µs | 180.5 µs | 194.2 µs | 600 |
| 1,000 | `DATE(ts) = day` | yes | 5,865 qps | 166.8 µs | 169.2 µs | 170.5 µs | 184.8 µs | 188.0 µs | 195.3 µs | 600 |
| 1,000 | `ts BETWEEN` | yes | 12,407 qps | 77.3 µs | 79.5 µs | 80.6 µs | 89.3 µs | 94.4 µs | 99.5 µs | 600 |
| 10,000 | `DATE(ts) = day` | no | 904 qps | 1,093.7 µs | 1,102.4 µs | 1,106.4 µs | 1,120.1 µs | 1,130.4 µs | 1,200.2 µs | 600 |
| 10,000 | `ts BETWEEN` | no | 1,084 qps | 867.9 µs | 898.9 µs | 922.2 µs | 964.7 µs | 1,019.4 µs | 1,034.7 µs | 600 |
| 10,000 | `DATE(ts) = day` | yes | 909 qps | 1,086.9 µs | 1,097.2 µs | 1,100.6 µs | 1,111.6 µs | 1,118.2 µs | 1,136.1 µs | 600 |
| 10,000 | `ts BETWEEN` | yes | 4,398 qps | 219.9 µs | 224.4 µs | 227.4 µs | 238.2 µs | 240.9 µs | 252.9 µs | 600 |
| 60,000 | `DATE(ts) = day` | no | 159 qps | 6,226.3 µs | 6,279.5 µs | 6,301.8 µs | 6,427.6 µs | 6,501.0 µs | 6,832.9 µs | 600 |
| 60,000 | `ts BETWEEN` | no | 194 qps | 4,815.0 µs | 4,996.0 µs | 5,145.2 µs | 5,385.4 µs | 5,462.3 µs | 5,560.6 µs | 600 |
| 60,000 | `DATE(ts) = day` | yes | 160 qps | 6,156.8 µs | 6,217.7 µs | 6,232.8 µs | 6,276.7 µs | 6,289.7 µs | 6,378.7 µs | 600 |
| 60,000 | `ts BETWEEN` | yes | 962 qps | 1,022.0 µs | 1,034.5 µs | 1,039.4 µs | 1,052.5 µs | 1,062.0 µs | 1,082.4 µs | 600 |

What it says. Without an index both forms walk the relation and the function
form costs 17.6 / 18.4 / 19.3 ns more a row (+11.5 % / +20 % / +22.5 % of the
range form's p50), a per-row cost that is constant across sizes, which is what the function conjunct costs as a post-filter: a day conversion on the row's `ts` where the range is two integer comparisons (the cause is a reading, not a measurement). With an index on `ts`
the range form drops to 227 µs at 10,000 rows and 1,039 µs at 60,000 (4.8× and 6.0× faster than the function) while `DATE(ts)` stays exactly where it was
(1,101 µs and 6,233 µs, 1,106 / 6,302 µs without the index): the function
conjunct never uses the index, as the manual says it does not. **So what a
function costs here is not the 18 ns a row; it is the access path it forecloses**
(5–6× at 1 % selectivity over 10,000–60,000 rows). The 1,000-row indexed case
reads 2.1× (170.5 against 80.6 µs). The repeated `fn-date` arm reads −0.4 / +15.2 / −5.9 µs against the first (no index); these runs are default placement and three a size, so treat differences of 1–2 % between rows as unresolved.

## What the run teaches about the engine

- **AP's per-statement additions are not where the cost is.** The second FNV
  state, the identity, the two clock reads: in the 17 µs of engine time a point
  select takes they are inside +0.3 µs, and a `SHOW META`, which does none of
  them, moves by +0.4 µs in the same runs. The design expectation, that
  `fetch_id` is "one more hash state fed only for a `SELECT`" (AP-R1,
  `instructions/v3.0.0/workorder-ap-function-catalog-fetch-id.md`), is met: the
  statement's fixed cost did not move by an amount this harness can see.
- **The one measurable cost is per row, and it is the walk.** +0.5 to +1.4 ns a
  row examined on a statement that rejects nearly every row, engine-side
  (+80 µs of 4,016 µs at 60,000 rows), 0.6–1.9 % of the scan. AP-R2/AP-R4 did not
  predict a per-row term for statements with no function in them, and no line
  AP added runs on a rejected row (`step_vm.cpp:2280`); the cost is
  unattributed, with code layout the reading left.
- **Waystone now shares one trail between select lists, as AP-R2 says.** A probe on both engines (`waystone-probe/probe2.txt`, `SHOW PATTERNS`) shows A registering one pattern per select list (two lists, two patterns, two waystone roots, 149 and 445) and B registering one `fetch_id` pattern whose `uses` count accumulates both lists' statements (1,207 after 1,506 statements, the first ~300 not counted). When a shape registers was not isolated: the first list took between 299 and 598 statements on both engines, A's second under 30. The point cells' timings are taken with those rows registered on both sides.
- **A function costs what it forecloses.** See above: ~18 ns a row as a post-filter,
  5–6× a keyed range when an index on the column could have served the
  equivalent range.

## What was not measured

- `cores > 1`; the instance read view, the lock table and the one WAL
  stream's latch are cross-core structures that a one-core run does not load.
- `group` and `strict` durability; `relaxed` was chosen for the reason in the
  AZ-S7 file (`group`'s commit is 82–85 % of an update), so no number here says
  what AP costs a durable commit. AP does not touch the WAL path, which is
  why the order named `relaxed`, but that is not a measurement.
- More than one session, any contention, a transaction (`BEGIN`/`COMMIT`),
  `DELETE`, a join, a subquery, an aggregate, `ORDER BY`, a Cabin-served read,
  an index-served read of the base relation, the parked-write path
  (`Session::ParkedWrite` grew; a parked statement needs a contender), and
  a `SELECT` whose select list is long enough to matter to the skipped window
  beyond ten columns.
- A function in a statement on A (A has none); `NOW()`.
- Row counts above 60,000, row counts in cell C3 below 1,000, and the
  `UPDATE` walk above 10,000 rows.
- The test suite was **not executed** for this measurement (no engine file
  changed).
- PostgreSQL: the floor was not measured for these shapes.
- What the stalls are (above); what carries the per-row cost (above).

## Archive

`bench/v3.0.0/archive/ap-s5-overhead-v2.7.0-606-g0c3268f/`: one directory per
(cell, rows, run) holding `A.conf`, `B.conf`, `driver.txt`, `host.txt` and the
driver's `result.json`; `excluded-*` for the loaded cells that were re-run; the
pair scripts (`pair.sh`, `pairpin.sh`) and the run scripts; `build.sh`,
`binaries.sha256` and the binaries' mtimes; `analysis/` (the scripts that
produced every table here from the JSON); `waystone-probe/`. No data files and
no WAL segments.

Reproduce: build both trees from `git archive`, copy the binaries, then
`pair.sh <tag> ap_overhead_benchmark.py --cell point --rows 1000 --ops 8000
--walk-ops 1000 --blocks 16` (and `--cell scan --rows 60000 --ops 1000
--blocks 10`, `SOLO=1 ... --cell fn --rows 10000 --ops 600 --blocks 12
[--index]`); `pairpin.sh` is the pinned form.
