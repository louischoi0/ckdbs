# DU close: a write that finds its rows by a key, base against closing commit

## Stamp

| item | value |
|---|---|
| executed | 1,000,000-row run: 2026-11-26 20:29-20:31 UTC; 100,000-row run: 20:34-20:36 UTC (about 2 minutes of driver time each; the quiet gate held 0 minutes both times) |
| branch / commit | `worktree-du-keyed-write` at `0babe36e6`, the binaries were built at `0babe36e6` from a clean tree before the run; documents under `docs/`, `manual/`, `instructions/` and `CLAUDE.md` were being edited in the worktree by the session that ordered the run, during it; no source or build file changed |
| `git describe --tags` | `v3.4.17-155-g0babe36e6` |
| work order | `instructions/v3.2.0/workorder-du-keyed-write.md` section 5 (DU-S4) |
| base arm | `build-release/du-close-bin/kds_server.base`, built at `60dc593af` (the commit DU opened from), sha256 `d9e683d9b312c7e2f3aa4ddf26d81a1b215afc10ce840ae7abe8ac65c22da9cc` (verified before the run; the driver's workdir copy hashed the same), source mtime 2026-11-26 15:28:10 -0500 |
| close arm | `build-release/du-close-bin/kds_server.close`, built at `0babe36e6`, sha256 `559262fbf6b4a16e3cf23769409df7a98bf103c1c400988d6acbd4ba452e4391` (verified; copy hashed the same), source mtime 2026-11-26 15:28:35 -0500 |
| build | Release (`build-release`); Debug not measured |
| device | `/dev/mapper/fedora-root`, xfs (`df -T build-release`); data files and WAL under `build-release/du-close-run/`, not tmpfs |
| server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, server pinned to CPU 2, driver to CPU 4, one connection an arm, ports 31980 (base) and 31981 (close), a fresh data file an arm |
| driver | `tools/du_close.py --arm base=... --arm close=... --rows 1000000 --reps 3 --port 31980` (its docstring is the driver's documentation) |
| host state | `/proc/loadavg` 2.72 / 5.25 / 4.39 before the start; no `cc1plus`, `cmake --build` or `ctest` running, so no gate hold. Per-block samples in `out.json`: 1-minute load 2.91 to 3.30 (the driver and server themselves account for part of it); `busy_processes` was 0 in every block except one close-arm `walk-small` block that read 1, i.e. a build or test process was present for that block (that cell's delta is +1.4% CPU, inside the floor) |
| prior number | `bench/v3.5.0/results-du-s0-premise-v3.4.17-150-g60dc593af.md` (the premise this closes against) |
| PostgreSQL floor | not measured for this shape |
| raw output | `bench/v3.5.0/archive/du-close-v3.4.17-155-g0babe36e6/out.json` |

## Thesis

1. **The fixed cost of the writes DU does not serve did not move.** A point
   UPDATE by tuple key, a 3-key list, a 1,000-row walk that writes nothing
   and a DELETE that misses run at the same statements/s in both arms on the
   1,000-row relation `s`, in both runs, and the server CPU a statement is
   within -3.4% to +1.5% in all eight cells. The control (a point SELECT, no
   write path on it) read +2.4% CPU in the 1,000,000-row run and +4.5% in the
   100,000-row run, a second control pair on the same `s` (`s` is the same
   in both runs), so **the noise floor is taken as 4.5% in CPU**; no
   fixed-cost delta reaches it and none is called a finding. Point CPU read
   -3.4% and -3.1% and list3 -3.0% at 100,000: same sign, inside the floor,
   not gains. Two p50 readings are nominally outside it (delete-miss +7.3% at
   1,000,000 rows; point and list3 +7.3% and +7.4% at 100,000) and are
   unexplained: no CPU figure confirms them and the two runs do not repeat
   each other's cells.
2. **The writes DU serves gain by a factor that falls with the share of the
   relation written.** At 1,000,000 rows, in server CPU: writing 10 rows by
   index is 481x cheaper (33,547 to 69.7 µs), 1,000 rows by index 18.0x,
   10,000 rows by index 2.7x, and a 1,000-row tuple key range 52.3x (33,606
   to 642 µs).
3. **The index arm stops paying at about 1 write row in 9 to 11 of the
   relation, and the largest loss measured is 11.2% CPU.** Server CPU,
   close over base, at each share of the relation written (rows written /
   relation rows):

   | share written | relation rows | rows written | arm | close/base CPU (x) | delta (%) |
   |---|---|---|---|---|---|
   | 1e-5 | 1,000,000 | 10 | index | 0.002 | -99.8 |
   | 1e-4 | 100,000 | 10 | index | 0.017 | -98.3 |
   | 1e-3 | 1,000,000 | 1,000 | index | 0.056 | -94.4 |
   | 1e-3 | 1,000,000 | 1,000 | range | 0.019 | -98.1 |
   | 1e-2 | 100,000 | 1,000 | index | 0.270 | -73.0 |
   | 1e-2 | 1,000,000 | 10,000 | index | 0.366 | -63.4 |
   | 1e-2 | 100,000 | 1,000 | range | 0.158 | -84.2 |
   | 1 in 10 | 100,000 | 10,000 | index | 0.964 | -3.6 (level, inside the 4.5% floor) |
   | 1 in 20 | 1,000,000 | 50,000 | index | 0.896 | -10.4 |
   | 1 in 2 | 100,000 | 50,000 | index | 1.112 | **+11.2 (loss)** |

   **Per-row cost arithmetic** (µs a added row, from the CPU columns; the
   walk's fixed part is idx-10: 33,547 µs at 1,000,000 rows, 3,313 µs at
   100,000, so 33.5 and 33.1 ns per relation row):

   | quantity | 1,000,000: base / close | 100,000: base / close |
   |---|---|---|
   | (idx-1000 - idx-10) / 990 | 0.92 / 1.87 | 0.72 / 1.04 |
   | (idx-10000 - idx-10) / 9,990 | 1.05 / 1.61 | 0.92 / 1.20 |
   | (idx-50000 - idx-10000) / 40,000 | 2.36 / 2.69 | 2.25 / 2.54 |

   The index arm's marginal cost a written row exceeds the walk's by 0.34
   (1,000,000 rows) and 0.30 µs (100,000 rows) over the 10,000 to 50,000
   step, in both runs, while the walk pays 33 ns a *relation* row once. The
   two terms set the crossing: it falls where the walk saved (33 ns x
   relation rows) equals the gap times the rows written, i.e. at a share of
   about 33 ns / 0.30 to 0.34 µs = 1 in 9 to 1 in 10, whatever the relation
   size. Computed from the cells: at 100,000 rows the lead is 451 µs at
   10,000 rows and shrinks 0.297 µs a row, crossing at about 11,500 rows (1
   in 8.7); at 1,000,000 rows it is 27,962 µs at 10,000 and shrinks 0.34 µs a
   row, crossing at about 93,000 rows (1 in 10.8). **Measured**, the
   crossing lies between 1 in 10 (level, -3.6%, inside the floor) and 1 in
   2 (+11.2%) at 100,000 rows; the 1,000,000-row value is an extrapolation
   from two cells (marked computed) because no cell above 50,000 rows was
   run there. DU-S0's 1 in 16 to 1 in 65 is not matched: the measured and
   computed crossings are at a larger share (1 in 9 to 1 in 11) than the
   top of that band.

   **The largest loss measured** is idx-50000 at 100,000 rows: +11,426 µs, +11.2% in
   CPU (p50 115.1 ms against 102.2 ms, 0.889x in statements/s). **What
   bounds it** is the 65,536-entry bound on the first collection
   (`txn::kMaxLocksPerTxnDefault`, `include/kds/txn/lock_table.hpp:417`,
   read by the session that ordered the run; not re-measured here): a write of more than 65,536 index entries does not take the
   arm as measured, so no larger cell exists to lose more. Computed, by
   extending the 10,000 to 50,000 per-row costs to 65,536 rows written
   (beyond a measured cell): at 100,000 rows base about 137.2 ms against
   close about 153.2 ms, a loss of about 16.0 ms (about +11.7%); at 1,000,000
   rows the same extension leaves the close arm ahead by about 9.2 ms (about
   -5.3%, computed), i.e. no loss at the bound on a relation that large. The
   worst case is thus about +12% CPU, on a relation the write covers about
   two thirds of; an unmeasured extrapolation.
4. **A plan pinning the shape to SCAN gives the walk back, at both sizes**:
   pinned-50k on the close arm is `Scan b arm=plan` and costs 139,325 µs
   against the base's 136,684 µs (+1.9%) at 1,000,000 rows and 102,460 µs
   against 101,618 µs (+0.8%) at 100,000 rows, both inside the floor, so it
   is level with the base's walk. At 100,000 rows that is also the answer to
   the loss: the pinned shape costs 102,460 µs where the unpinned close arm
   costs 113,715 µs.

## Method

| item | value |
|---|---|
| relations | `s`: 1,000 rows `(id, owner, v)`, an index on `owner`. `b`: 1,000,000 rows `(id, o10, o1k, o10k, o50k, v)`, `oN = id % (rows / N)` so each value holds N rows, each `oN` indexed (four indexes an arm) |
| row-count mapping | `--rows 1000000`. The fixed-cost shapes run on the 1,000-row `s`; the large shapes write N rows of the 1,000,000 (10, 1,000, 10,000, 50,000 = 1e-5, 1e-3, 1e-2, 5e-2 of the relation). The written-row count is swept from 10 to 50,000; the relation size was not varied (see the cut list) |
| equal work | a fixed count of statements an arm a repeat: 3,000 (point, delete-miss, read-point), 2,000 (list3), 1,000 (walk-small), 12 (large shapes); 3 repeats, arm order rotated per repeat, a warm block before each cell; the rows each statement writes are checked every time |
| CPU | on-CPU ns summed over the server's threads (`/proc/<pid>/task/*/schedstat`) over the measured blocks, divided by statements |
| statements/s | derived as 1,000,000 / p50 µs, for a serial one-connection driver |

## Every shape, base against close (1,000,000 rows)

Statements/s is derived from p50. CPU is the server's on-CPU time a statement,
in µs. Ratios are close over base: above 1 on statements/s and below 1 on
CPU is faster. The control row is read-point.

| shape | rows written | base stmts/s (1/p50) | close stmts/s (1/p50) | close/base (x) | base CPU (µs/stmt) | close CPU (µs/stmt) | CPU close/base (x) | CPU delta (%) |
|---|---|---|---|---|---|---|---|---|
| point | 1 | 67,114 | 67,114 | 1.000 | 7.72 | 7.46 | 0.966 | -3.4 |
| list3 | 3 | 55,249 | 55,249 | 1.000 | 9.92 | 9.90 | 0.998 | -0.2 |
| walk-small | 0 | 19,881 | 19,841 | 0.998 | 40.55 | 41.11 | 1.014 | +1.4 |
| delete-miss | 0 | 62,112 | 66,667 | 1.073 | 6.44 | 6.35 | 0.986 | -1.4 |
| read-point (control) | read | 60,606 | 61,350 | 1.012 | 6.70 | 6.86 | 1.024 | +2.4 |
| idx-10 | 10 | 29.5 | 13,569 | 460.5 | 33,547.18 | 69.68 | 0.002 | -99.8 |
| idx-1000 | 1000 | 28.7 | 623 | 21.7 | 34,458.43 | 1,918.78 | 0.056 | -94.4 |
| idx-10000 | 10000 | 23.1 | 60.7 | 2.63 | 44,079.11 | 16,117.84 | 0.366 | -63.4 |
| idx-50000 | 50000 | 7.2 | 8.2 | 1.143 | 138,290.39 | 123,850.96 | 0.896 | -10.4 |
| range-1k | 1000 | 29.2 | 1,391 | 47.6 | 33,605.58 | 642.11 | 0.019 | -98.1 |
| pinned-50k | 50000 | 7.3 | 7.2 | 0.994 | 136,683.68 | 139,325.18 | 1.019 | +1.9 |

The noise floor from the control: read-point differs by +2.4% in CPU and
+1.2% in statements/s with no write path involved; the 100,000-row run's
second pair read +4.5% and 1.000 (below), so the floor is taken as 4.5% and a
delta under it is not a finding. Every fixed-cost row is inside it on CPU.
The large shapes' deltas exceed it by far, except pinned-50k (+1.9%, inside
it). Two control pairs are two samples of the floor, not a distribution.

### Per-row costs (derived from the CPU column)

| quantity | base | close |
|---|---|---|
| CPU to write 10 rows (idx-10), µs | 33,547 | 69.7 |
| (CPU idx-1000 - CPU idx-10) / 990 rows, µs/row | 0.92 | 1.87 |
| (CPU idx-10000 - CPU idx-10) / 9,990 rows, µs/row | 1.05 | 1.61 |
| (CPU idx-50000 - CPU idx-10000) / 40,000 rows, µs/row | 2.36 | 2.69 |

The base arm pays a near-constant 33.5 ms to walk the 1,000,000-row relation
(idx-10, idx-1000 and range-1k are 33.5 to 34.5 ms), plus a per-written-row
cost that grows from 0.9 to 2.4 µs a row as the count grows. The close arm
pays no walk and 1.6 to 1.9 µs a row up to 10,000 rows, then 2.69 µs a row
between 10,000 and 50,000. Both marginal costs rise with the count written;
the index arm's is above the walk's at every step (by 0.34 µs a row at the
last), which is the mechanism by which it must lose at some larger share.
Whether the rise is cache residency, the collected key set or the undo is not
decomposed by this run.

## Latency (1,000,000 rows)

All values in µs. p0 is the best case the path reaches, p25 the body. The
large shapes have 36 statements a cell (12 x 3 repeats), so p99 equals max
there and is one statement; read those two columns as "the worst one seen".
The driver does not emit p95.

| shape | arm | n | p0 | p25 | p50 | p75 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| point | base | 9000 | 14.3 | 14.7 | 14.9 | 15.2 | 680.7 | 7,344.5 |
| point | close | 9000 | 14.1 | 14.7 | 14.9 | 15.1 | 17.8 | 3,176.8 |
| list3 | base | 6000 | 17.5 | 17.9 | 18.1 | 18.3 | 20.2 | 371.7 |
| list3 | close | 6000 | 17.4 | 17.9 | 18.1 | 18.3 | 20.8 | 1,728.0 |
| walk-small | base | 3000 | 46.6 | 49.7 | 50.3 | 51.2 | 58.3 | 369.9 |
| walk-small | close | 3000 | 47.5 | 49.6 | 50.4 | 51.6 | 59.0 | 144.1 |
| delete-miss | base | 9000 | 12.6 | 15.7 | 16.1 | 16.6 | 20.5 | 362.1 |
| delete-miss | close | 9000 | 12.2 | 14.8 | 15.0 | 15.6 | 969.0 | 2,603.8 |
| read-point | base | 9000 | 15.6 | 16.2 | 16.5 | 17.7 | 20.6 | 377.0 |
| read-point | close | 9000 | 15.4 | 16.0 | 16.3 | 17.1 | 1,549.2 | 3,014.1 |
| idx-10 | base | 36 | 33,622.5 | 33,890.8 | 33,936.5 | 33,991.3 | 34,154.8 | 34,154.8 |
| idx-10 | close | 36 | 45.4 | 65.9 | 73.7 | 100.5 | 279.8 | 279.8 |
| idx-1000 | base | 36 | 34,645.9 | 34,781.4 | 34,828.6 | 34,910.9 | 81,982.5 | 81,982.5 |
| idx-1000 | close | 36 | 1,357.0 | 1,477.1 | 1,605.2 | 2,822.4 | 61,310.1 | 61,310.1 |
| idx-10000 | base | 36 | 42,888.5 | 43,106.6 | 43,360.7 | 43,576.2 | 560,498.2 | 560,498.2 |
| idx-10000 | close | 36 | 15,001.0 | 15,233.1 | 16,472.8 | 23,190.3 | 909,474.3 | 909,474.3 |
| idx-50000 | base | 36 | 133,620.9 | 136,026.8 | 139,119.3 | 239,807.3 | 1,225,833.7 | 1,225,833.7 |
| idx-50000 | close | 36 | 119,658.4 | 120,667.6 | 121,733.9 | 127,854.0 | 1,532,062.9 | 1,532,062.9 |
| range-1k | base | 36 | 32,831.5 | 33,349.0 | 34,212.2 | 34,913.2 | 115,186.5 | 115,186.5 |
| range-1k | close | 36 | 622.8 | 695.0 | 718.9 | 802.4 | 1,412.9 | 1,412.9 |
| pinned-50k | base | 36 | 133,830.9 | 135,551.6 | 137,681.1 | 161,095.0 | 2,041,014.9 | 2,041,014.9 |
| pinned-50k | close | 36 | 135,540.0 | 136,914.4 | 138,501.8 | 142,321.9 | 1,174,830.0 | 1,174,830.0 |

The close arm's idx-1000 and idx-10000 bodies are bimodal (idx-10000: p25
15.2 ms, p75 23.2 ms; idx-1000: p25 1.48 ms, p75 2.82 ms), which the base
arm's walk is not at those sizes (p25 to p75 within 1.5% at idx-10000).
Suspicious and unexplained: the cause was not isolated here. Each large cell
also holds one statement of 0.06 s to 2.0 s (the p99 column), far above the
CPU mean, so these are stalls off the CPU in both arms; their source
(writeback, checkpoint) was not identified. They are one statement in 36 and
inflate mean wall time, not the CPU figures.

## The arms' own words (1,000,000 rows)

`ANALYZE` of each large shape, `out.json` `arms.<label>.analyze`. On the
base arm every one of them was `refused: expected a SELECT statement` (the
base predates `ANALYZE` of a write), and its `CREATE PLAN ... FOR UPDATE` was
`refused: CREATE PLAN ... FOR takes a SELECT (byte 26)`, so there pinned-50k
is the walk every base write takes. On the close arm (the plan was created:
`plan_id=1`, one variant), verbatim:

| shape | close ANALYZE |
|---|---|
| idx-10 | `rows=10 class=Update \| write IndexProbe b index=b_o10 keys=10` |
| idx-1000 | `rows=1000 class=Update \| write IndexProbe b index=b_o1k keys=1000` |
| idx-10000 | `rows=10000 class=Update \| write IndexProbe b index=b_o10k keys=10000` |
| idx-50000 | `rows=50000 class=Update \| write IndexProbe b index=b_o50k keys=50000` |
| range-1k | `rows=1000 class=Update \| write Range b range=[497624, 498623]` |
| pinned-50k | `rows=50000 class=Update \| write Scan b arm=plan \| planned plan=du_pinned/walk` |

The planner chose the index for idx-50000 (1 in 20 of the relation), and it
was the faster arm there, so the choice was right at the one point nearest
the crossing that was measured.

## At 100,000 rows: where the index arm stops paying

Same driver, same two binaries (hashes as stamped), `--rows 100000 --port
31984`, workdir `build-release/du-close-run-100k`, raw output
`bench/v3.5.0/archive/du-close-v3.4.17-155-g0babe36e6/out-100k.json`.
Executed 2026-11-26 20:34-20:36 UTC. The large shapes write the same row
counts out of a tenth of the relation: idx-10 is 1e-4 of it, idx-1000 and
range-1k 1 in 100, idx-10000 1 in 10, idx-50000 and pinned-50k 1 in 2.
Host state: loadavg 1.05 / 2.72 / 3.56 and no build or test process at the
start; per-block samples ranged 1.29 to 2.83 with `busy_processes` reading 1
or 2 in every block, so some other process matching `cc1plus`, `cmake --build`
or `ctest` was present throughout, on this host's other CPUs (nothing was
found running when checked afterwards; the process was not identified). A
count of exactly 1 in every close-arm block, at a load of 1.3 to 2.8, is
also what a watching shell whose own command line holds the pattern reads
as - `pgrep -f` matches command lines, and this run's watchdog was such a
shell; not verified. The
server was pinned to CPU 2 and the driver to CPU 4; the effect of a
competing process on these cells, if there was one, is not isolated. The 1,000,000-row run had
`busy_processes` 0 in all but one block.

The thesis: the index arm is level with the walk at 1 in 10 (-3.6%, inside
the floor), loses 11.2% in CPU at 1 in 2, and wins 73% at 1 in 100; the
tuple key range wins at every size. Statements/s derived from p50, as above.

| shape | rows written | base stmts/s (1/p50) | close stmts/s (1/p50) | close/base (x) | base CPU (µs/stmt) | close CPU (µs/stmt) | CPU close/base (x) | CPU delta (%) |
|---|---|---|---|---|---|---|---|---|
| point | 1 | 52,083 | 55,866 | 1.073 | 8.27 | 8.01 | 0.969 | -3.1 |
| list3 | 3 | 42,918 | 46,083 | 1.074 | 12.29 | 11.92 | 0.970 | -3.0 |
| walk-small | 0 | 21,322 | 21,097 | 0.989 | 38.97 | 39.55 | 1.015 | +1.5 |
| delete-miss | 0 | 74,627 | 73,529 | 0.985 | 5.43 | 5.51 | 1.015 | +1.5 |
| read-point (control) | read | 61,728 | 61,728 | 1.000 | 6.24 | 6.52 | 1.045 | +4.5 |
| idx-10 | 10 | 290 | 17,422 | 59.998 | 3,312.59 | 54.98 | 0.017 | -98.3 |
| idx-1000 | 1000 | 241 | 854 | 3.545 | 4,025.19 | 1,085.69 | 0.270 | -73.0 |
| idx-10000 | 10000 | 79.6 | 82.8 | 1.041 | 12,493.40 | 12,042.01 | 0.964 | -3.6 |
| idx-50000 | 50000 | 9.8 | 8.7 | 0.889 | 102,288.99 | 113,714.96 | 1.112 | +11.2 |
| range-1k | 1000 | 254 | 1,431 | 5.631 | 3,773.83 | 596.20 | 0.158 | -84.2 |
| pinned-50k | 50000 | 9.8 | 9.7 | 0.985 | 101,617.92 | 102,459.92 | 1.008 | +0.8 |

Second control pair: read-point reads +4.5% in CPU and 1.000 in statements/s
with no write path on it, which widens the floor to 4.5% (thesis 1). Against
it: idx-10000 (-3.6%) and pinned-50k (+0.8%) are level; idx-1000 (-73.0%),
range-1k (-84.2%), idx-10 (-98.3%) are gains; idx-50000 (+11.2% CPU, p50
0.889x) is the one loss. In the fixed-cost cells the two reps' signs repeat
(point -3.1%, list3 -3.0%, walk-small +1.5%, delete-miss +1.5%) inside the floor.

Latency, µs; the large shapes have 36 statements a cell, so p99 equals max
there.

| shape | arm | n | p0 | p25 | p50 | p75 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| point | base | 9000 | 14.4 | 15.4 | 19.2 | 19.8 | 22.7 | 1,267.1 |
| point | close | 9000 | 15.0 | 17.6 | 17.9 | 18.3 | 23.4 | 19,976.8 |
| list3 | base | 6000 | 18.1 | 22.2 | 23.3 | 23.9 | 33.8 | 382.2 |
| list3 | close | 6000 | 18.1 | 20.3 | 21.7 | 23.3 | 226.6 | 3,454.3 |
| walk-small | base | 3000 | 45.3 | 46.4 | 46.9 | 47.4 | 54.4 | 391.4 |
| walk-small | close | 3000 | 45.7 | 46.8 | 47.4 | 48.3 | 63.6 | 5,521.1 |
| delete-miss | base | 9000 | 12.5 | 13.3 | 13.4 | 13.6 | 15.1 | 13,129.7 |
| delete-miss | close | 9000 | 12.9 | 13.4 | 13.6 | 13.8 | 15.7 | 8,401.7 |
| read-point | base | 9000 | 15.5 | 16.0 | 16.2 | 16.4 | 17.9 | 197.5 |
| read-point | close | 9000 | 15.5 | 16.1 | 16.2 | 16.5 | 675.3 | 1,785.5 |
| idx-10 | base | 36 | 3,370.0 | 3,425.6 | 3,443.9 | 3,562.1 | 3,625.8 | 3,625.8 |
| idx-10 | close | 36 | 32.0 | 49.6 | 57.4 | 82.5 | 294.9 | 294.9 |
| idx-1000 | base | 36 | 4,053.2 | 4,113.5 | 4,149.3 | 4,230.2 | 6,284.8 | 6,284.8 |
| idx-1000 | close | 36 | 1,021.5 | 1,128.9 | 1,170.4 | 1,209.7 | 1,990.2 | 1,990.2 |
| idx-10000 | base | 36 | 12,322.0 | 12,423.5 | 12,562.6 | 12,909.9 | 346,048.5 | 346,048.5 |
| idx-10000 | close | 36 | 11,680.4 | 11,901.1 | 12,071.5 | 13,107.0 | 214,024.6 | 214,024.6 |
| idx-50000 | base | 36 | 100,275.8 | 101,555.8 | 102,242.5 | 108,140.6 | 576,937.9 | 576,937.9 |
| idx-50000 | close | 36 | 111,700.2 | 112,714.5 | 115,067.4 | 121,272.2 | 1,218,224.7 | 1,218,224.7 |
| range-1k | base | 36 | 3,764.6 | 3,855.9 | 3,933.7 | 4,005.4 | 4,954.4 | 4,954.4 |
| range-1k | close | 36 | 651.5 | 675.8 | 698.6 | 744.8 | 856.2 | 856.2 |
| pinned-50k | base | 36 | 99,934.9 | 101,182.8 | 101,841.6 | 105,589.5 | 1,977,121.4 | 1,977,121.4 |
| pinned-50k | close | 36 | 100,721.5 | 102,085.9 | 103,353.0 | 107,418.9 | 2,146,921.0 | 2,146,921.0 |

The close arm's idx-10000 and idx-1000 bodies are tight at this size (p25 to
p75 11.9 to 13.1 ms and 1.13 to 1.21 ms), unlike the 1,000,000-row run, so
the bimodality noted there is not reproduced here. Each large cell again
holds one stalled statement (0.2 s to 2.1 s), off the CPU, in both arms.

Close arm's `ANALYZE` lines at this size (the base refused all six as
before, and its `CREATE PLAN ... FOR UPDATE` as before):

| shape | close ANALYZE |
|---|---|
| idx-10 | `analyze rows=10 class=Update \| write IndexProbe b index=b_o10 keys=10` |
| idx-1000 | `analyze rows=1000 class=Update \| write IndexProbe b index=b_o1k keys=1000` |
| idx-10000 | `analyze rows=10000 class=Update \| write IndexProbe b index=b_o10k keys=10000` |
| idx-50000 | `analyze rows=50000 class=Update \| write IndexProbe b index=b_o50k keys=50000` |
| range-1k | `analyze rows=1000 class=Update \| write Range b range=[62203, 63202]` |
| pinned-50k | `analyze rows=50000 class=Update \| write Scan b arm=plan \| planned plan=du_pinned/walk` |

## Waits

A fsync / lock / read decomposition with shares was not measured and the
engine's wait views were not read. What composes the latency, from the
configuration: one connection on one core, so no lock or conflict wait;
`durability = relaxed`, so no commit parks on a sync; the pool is 131,072
frames of 8 KiB (1 GiB) against a relation of 1,000,000 narrow rows and four
indexes, which is expected to be resident (residency not checked). For a
point statement the server's CPU is 7.5 µs of a 14.9 µs p50, so about half is
the loopback round trip and the client; the large shapes are CPU bound (idx-10000
base: p50 43.4 ms against 44.1 ms of CPU), apart from the stalled statements
above.

## What this says about the engine

- The walk's cost is the relation, not the answer: a write finding its rows
  by a key spends 33.5 ms at 1,000,000 rows to write 10 of them. A tuple key
  range or an index entering the WHERE removes that term (range-1k 0.64 ms,
  idx-10 0.07 ms).
- Fixed-cost write paths DU does not serve are level with the base (point,
  list3, walk-small, delete-miss within the 4.5% floor on CPU), so DU added
  nothing measurable to a statement that takes no new arm.
- Both arms' marginal cost a written row rises with the count written; the
  index arm's is higher by 0.3 µs a row in both runs, against a walk that
  costs 33 ns a relation row. The crossing is therefore a share of the
  relation (about 1 in 9 to 1 in 11), not a row count; DU-S0's 1 in 16 to 1
  in 65 (the DU-S0 premise file and the work order's section 5) is
  contradicted: at 100,000 rows the index arm is level at 1 in 10 and loses
  11.2% at 1 in 2.
- A plan pinning SCAN is a working escape: the walk comes back at base cost
  (+1.9% and +0.8%, in the floor).

## What was cut and what this does not measure

- One core and `relaxed` durability only; `strict` and `group` not measured,
  so any commit-sync share of a write is absent.
- Two relation sizes (1,000,000 and 100,000 rows, plus the 1,000-row `s`).
  No cell above 50,000 rows written, so the 65,536-entry worst case is
  computed, not measured, and the 1,000,000-row crossing is an
  extrapolation from two cells; the bound itself was not read or probed.
  Cells between 10,000 and 50,000 rows at 100,000 rows (the span in which the
  crossing falls) were not run.
- UPDATE only for the large shapes; DELETE only as a miss on `s`. No
  `FOR UPDATE`, no heap relation (btree only), no unique-key write.
- No concurrent writer, no lock or deadlock path, no crash or recovery.
- 12 statements a cell for the large shapes (36 over three repeats); p99 is
  the worst one, and the single stalled statement per cell is a large share
  of any wall-clock mean.
- The large-shape cells write the same relation `b` in sequence, so later
  cells ran on a relation earlier cells had updated; both arms ran the same
  sequence with the arm order rotated.
- Release only; Debug not measured. The host was sampled by load average and
  a process count only, beyond pinning the server and driver.
- PostgreSQL floor not measured.
