# The correlated sub-chain's per-row cost with the empty-hoist test in `EvaluateSubChain` (`v2.7.0-750-g9004a0cb`)

**Thesis.** On `worktree-fix-nested-uncorrelated-subquery` at `9004a0cb` (`v2.7.0-750-g9004a0cb`),
`EvaluateSubChain` tests `!sub.hoisted.empty()` before it builds `HoistedAllTrue`'s `StatusOr<bool>`,
and the **+35 ns per outer row the correlated `EXISTS` scan cost at `23e53bed`
(`v2.7.0-749-g23e53bed`: +36.0, +38.3 and +28.2 ns per row at 200, 1,000 and 10,000 outer rows, B slower
in 7 of 7 runs at every size) is no longer resolved**. Against the base `6b0d8412` (`v2.7.0-747-g6b0d8412`)
the pre-specified 8 pairs give **+11.5, +21.2 and -16.1 ns per row** at 200, 1,000 and 10,000 rows, with
interquartile ranges that span zero at every size, and 16 pooled runs (the 8 plus 8 more) give **+11.5,
+6.7 and +5.3 ns per row** (bootstrap 95 % interval of the median: [+2.5, +14.0], [-9.2, +24.4],
[-16.2, +27.4]). The slot-swap pass (5 pairs, 1,000 rows) gives +4.6 ns per row, IQR [-13.9, +12.0].
That harness still sees the old cost: a **positive control run in the same hour, A against the previous B
(`23e53bed`), reads +44.0, +46.4 and +18.3 ns per row, B slower in 8 of 8 runs at every size**. The verdict
is therefore **resolved at 1,000 and 10,000 rows to within about +-25 ns per row (the upper bounds, +24
and +27, are below the 35 it replaced), and reduced to about +10 ns per row at 200 rows**, where a small
positive remains in both independent 8-run sets (+2.3 and +2.1 µs per 200-row statement) and a part of
it is the per-statement offset (+0.7 µs) the pk `SELECT` control carries. The pk-select control is **+0.70 µs
(+0.7 %), B slower in 8 of 8 runs, the same size as at `23e53bed` (+0.8 to +1.0 µs)**. The 10,000-row cell
is the weak one: its per-run differences range from -1.4 ms to +0.8 ms on a 30 ms statement, so its sign
is not evidence in either direction.

- **A** = `6b0d8412` (`v2.7.0-747-g6b0d8412`), the branch's base. **B'** = `9004a0cb` (`v2.7.0-750-g9004a0cb`),
  the tip. `git diff 23e53bed 9004a0cb -- src` is one file, `src/exec/step_vm.cpp`, 14 changed lines: the
  empty test around `HoistedAllTrue` in `EvaluateSubChain`. Everything else in B' is the code the previous
  A/B (`bench/v3.0.0/results-nested-hoist-overhead-v2.7.0-749-g23e53bed.md`) measured.
- The suite was **not executed** by this measurement and no code changed in it; the commit's own stage carries it.
- **The PostgreSQL floor was not measured for this shape** (rule 4). The baselines are this engine's own:
  A, and the `23e53bed` numbers cited above.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10. Pre-specified matrix 17:36 to 17:39 UTC (8 pairs per cell, then the 5-pair swap pass 17:39); extension and positive control 17:40 to 17:44 UTC |
| Branch / worktree | `worktree-fix-nested-uncorrelated-subquery`, worktree `fix-nested-uncorrelated-subquery`, clean (`git status --short` empty) at the build and at the start |
| B' | `9004a0cb`, `v2.7.0-750-g9004a0cb`, commit time 2026-10-10 17:35:27 UTC. Built in the worktree's `build-release/` (`CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `kds_server` target only, `nice` on CPUs 4-7, nothing else running that was seen), source binary mtime 2026-10-10 17:36:21 UTC, **after** the commit |
| A | `6b0d8412`: the measured copy of the previous run's A, **reused, not rebuilt** (its detached worktree had been removed; its source binary mtime was 16:39:23 UTC); `sha256sum` equals the hash recorded in `archive/nested-hoist-overhead-v2.7.0-749-g23e53bed/binaries.sha256` |
| Measured copies | A `ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`, B' `3bbeb4ef579e79e50531f74e50fe19ef796c410d3c2795ea8b3d344c910d7913`; positive control's second binary, `23e53bed`'s copy, `13a9ba31e8aa6de235defcd257390fa2b90b4a78dcc6c96ed1c84ffc1a580173` (equal to the earlier record). Every server started from a copy under `~/bench-runs/nhf/` or `nhf-ctl/` (`archive/.../binaries*.sha256`) |
| Device | data files under `~/bench-runs/nhf*/work/`, `/dev/root`, ext4 (`df -T`, 2026-10-10), not tmpfs; a fresh server and data file per (run, rows), deleted after the run |
| Host | 8 logical CPUs, Linux 7.0.0-1014-azure; shared with other sessions throughout (below) |
| Pinning | both servers of a pair on CPU 6 (one idle while the other is timed), client on CPU 7; ports 15620 (B slot) and 15621 (A slot) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, everything else default; relations `BTREE` |
| Driver | the previous run's `nh_ab.py` / `nh_run.py`, copied to `archive/.../drivers/` with two edits: `--only` is an exact arm-name list (it was a prefix list) and the data/binary directory is an environment variable. Same relations, same statements, same 16 alternating-order blocks per cell. Scenario drivers' documentation is closed (`bench/README.md`); the commands are `nh_run.py 8 <rows> <outdir> --only <arms>` per size, `NH_SWAP=1 nh_run.py 5 1000 <outdir> --only corr-exists-scan` for the swap, and `drivers/runall.sh` / `runext.sh` for the exact sequence |

**Host.** Load at each run's start ran 1.78 to 3.69 over all 77 runs (per-run record in `archive/.../*/*.host.txt`).
`pgrep -f 'cc1plus|cmake --build|ctest'` at each run's start found **no compile or test in any of the 77 runs, so
none is excluded for that reason**. Another session's `bj-close` measurement (servers on CPUs 2 and 3, driver
on CPU 4) was running when the matrix began (`ps` at 17:36:49 UTC) and its tail was not looked for afterwards. Five runs show
other-CPU activity or a longer duration than their siblings (`10000` main run 4: CPUs 0, 4, 5 at 0.58 to 0.79 busy and
CPU 7 at 0.65; swap runs 3 and 4; control `200` runs 6 and 7); they are **kept** and every statistic is a median, which
does not move when they are left out (10,000-row main median -160.9 µs with run 4 and -159.8 µs without; swap median +4.6 µs with runs 3 and 4 and without).

## 2. The correlated `EXISTS` scan, per outer row

Arm: `SELECT a.id FROM a WHERE EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id)`, one `EvaluateSubChain` per
outer row, an index probe each. `--rows N` is the outer relation's row count: 200, 1,000, 10,000 (`b` has N/2
rows). Scan arms run `max(2, 40000 / N)` statements per block, so each block visits about 40,000 outer rows
(**equal work**). Delta = median over runs of the per-run (B' p50 - A p50); IQR over the same 8 differences.
Reply rows are equal between A and B' in every cell (`rows_equal` true, errors 0).

| Rows | Runs | A p50 | B' p50 | median per-run Δp50 | IQR of Δp50 | B' slower in | **Δ per outer row** | IQR per row | median Δ engine per statement [IQR] |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | 8 | 543.2 µs | 545.5 µs | +2.30 µs | [-0.10, +3.32] µs | 5/8 | **+11.5 ns** | [-0.5, +16.6] ns | +2.26 [-0.31, +3.77] µs |
| 1,000 | 8 | 3,075.9 µs | 3,084.1 µs | +21.2 µs | [-3.1, +32.3] µs | 5/8 | **+21.2 ns** | [-3.1, +32.3] ns | +16.5 [+3.1, +37.2] µs |
| 10,000 | 8 | 30,445.7 µs | 30,434.0 µs | -160.9 µs | [-696.5, +50.0] µs | 2/8 | **-16.1 ns** | [-69.6, +5.0] ns | +28.4 [-102.1, +458.8] µs |

Whether the figure is the cost of the change or noise is decided by three further measurements, none of
which was in the order but all of which run the same cells:

| Check | Rows | Runs | median Δ per outer row | IQR per row | B' (or B) slower in |
|---|---:|---:|---:|---:|---:|
| Extension: 8 more A vs B' pairs | 200 | 8 | +10.5 ns | [+5.9, +13.5] | 7/8 |
| Extension | 1,000 | 8 | -0.0 ns | [-22.2, +14.9] | 4/8 |
| Extension | 10,000 | 8 | +11.7 ns | [+5.5, +31.8] | 7/8 |
| **Pooled, main + extension** | 200 | 16 | **+11.5 ns** (bootstrap 95 % of the median [+2.5, +14.0]) | [+1.8, +15.0] | 12/16 |
| **Pooled** | 1,000 | 16 | **+6.7 ns** ([-9.2, +24.4]) | [-10.7, +24.5] | 9/16 |
| **Pooled** | 10,000 | 16 | **+5.3 ns** ([-16.2, +27.4]) | [-26.0, +28.2] | 9/16 |
| Slot swap: the B' binary in the A slot (Δ reported as B' - A) | 1,000 | 5 | +4.6 ns | [-13.9, +12.0] | 3/5 |
| **Positive control: A vs the previous B (`23e53bed`)** | 200 | 8 | **+44.0 ns** | [+38.9, +57.8] | 8/8 |
| Positive control | 1,000 | 8 | **+46.4 ns** | [+10.0, +59.8] | 8/8 |
| Positive control | 10,000 | 8 | **+18.3 ns** | [+15.2, +39.5] | 8/8 |

The positive control is the point of the table. In the same hour, on the same host, with the same driver, a
binary known to cost about 35 ns per row shows it in 8 of 8 runs at every size (the 10,000-row figure is the
smallest, +18 ns, and its interquartile range still excludes zero); B' does not. The two 8-run sets of B'
against A agree at 200 rows (+11.5 and +10.5 ns per row; the second set's IQR excludes zero, the first's touches it) and
disagree in sign at 10,000 rows (-16.1 and +11.7), which is what a floor of about +-25 ns per row on that
cell looks like. **Verdict, per size: 200 rows, reduced to about +10 ns per row; 1,000 and 10,000 rows,
resolved (no positive resolved; upper bounds +24 and +27 ns per row against 35 before).** A residual of up to
about 25 ns per row at the two larger sizes is not excluded by this data.

The 200-row residual is a statement of +2.3 µs (median Δp50, extension +2.1 µs). The pk `SELECT` control,
a statement that cannot reach the change, costs +0.70 µs in this run (section 4), so about +1.6 µs, or +8
ns per row, is left that a per-statement offset does not explain; it is small against a 543 µs statement
(+0.3 %) and below the 8-run floor of the 1,000-row cell, and this measurement cannot say whether it is the
change or the binaries' layout.

### Latency, pooled over the 8 main runs

Percentiles are pooled over every statement of the 8 runs (nearest rank); `n` is the count per side.
The p95 column of the 10,000-row row (and of the scan-filter control) is a host disturbance, not a tail
of the path: at least 5 % of the statements take twice the median, on both servers and on the control.

| Arm | Rows | Side | n | p0 | p25 | p50 | p95 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| corr-exists-scan | 200 | A | 25,600 | 516.6 µs | 533.2 µs | 543.6 µs | 564.3 µs | 593.7 µs |
| corr-exists-scan | 200 | B' | 25,600 | 517.3 µs | 536.0 µs | 545.7 µs | 568.2 µs | 607.7 µs |
| corr-exists-scan | 1,000 | A | 5,120 | 3,006.4 µs | 3,059.4 µs | 3,078.4 µs | 3,155.9 µs | 3,264.9 µs |
| corr-exists-scan | 1,000 | B' | 5,120 | 3,029.6 µs | 3,074.9 µs | 3,097.8 µs | 3,167.3 µs | 3,407.7 µs |
| corr-exists-scan | 10,000 | A | 512 | 30,020 µs | 30,298 µs | 30,541 µs | 61,152 µs | 65,436 µs |
| corr-exists-scan | 10,000 | B' | 512 | 30,039 µs | 30,290 µs | 30,573 µs | 60,241 µs | 65,030 µs |
| corr-exists-point | 1,000 | A | 51,200 | 97.0 µs | 103.2 µs | 109.1 µs | 130.6 µs | 147.2 µs |
| corr-exists-point | 1,000 | B' | 51,200 | 97.8 µs | 104.2 µs | 110.1 µs | 131.8 µs | 147.6 µs |
| ctl-select-pk | 1,000 | A | 51,200 | 86.7 µs | 94.0 µs | 96.0 µs | 117.6 µs | 133.0 µs |
| ctl-select-pk | 1,000 | B' | 51,200 | 89.3 µs | 94.7 µs | 96.7 µs | 118.4 µs | 134.7 µs |
| ctl-scan-filter | 10,000 | A | 512 | 2,546 µs | 2,605 µs | 2,624 µs | 4,949 µs | 5,153 µs |
| ctl-scan-filter | 10,000 | B' | 512 | 2,545 µs | 2,594 µs | 2,616 µs | 4,842 µs | 5,170 µs |

The p0 of the 1,000-row scan is 23 µs higher on B' (0.8 %); its p25 and p50 are 16 and 19 µs higher, and the
run medians' IQR is [-3, +32]. That is the one place the pooled distribution leans positive and the per-run
differences do not carry it.

## 3. Throughput matrix

Derived, not re-measured (rule 5a): `outer rows per second = N / p50` for a scan, `QPS = 1,000,000 / p50`
for a serial point statement, from the median-over-runs p50 of section 2 and section 4.

| Arm | Rows | A | B' | Δ (B' vs A) |
|---|---:|---:|---:|---:|
| corr-exists-scan (outer rows/s) | 200 | 368,189 | 366,636 | -0.4 % |
| corr-exists-scan | 1,000 | 325,108 | 324,244 | -0.3 % |
| corr-exists-scan | 10,000 | 328,454 | 328,580 | +0.0 % |
| corr-exists-point (QPS) | 1,000 | 9,158 | 9,083 | -0.8 % |
| ctl-select-pk (QPS) | 1,000 | 10,417 | 10,331 | -0.8 % |
| ctl-scan-filter (rows/s) | 10,000 | 3,816,648 | 3,826,140 | +0.2 % |

## 4. The controls and the point cell

| Arm | Rows | Runs | median per-run Δp50 | IQR | B' slower in | median Δ engine per statement [IQR] | block-paired median Δ (128 blocks) [IQR] |
|---|---:|---:|---:|---:|---:|---:|---:|
| ctl-select-pk | 1,000 | 8 | **+0.70 µs** (+0.7 %) | [+0.18, +1.04] µs | 8/8 | +0.50 [+0.29, +0.75] µs | +0.75 [+0.20, +1.30] µs, 111/128 |
| corr-exists-point | 1,000 | 8 | +0.95 µs (+0.9 %) | [+0.65, +1.20] µs | 8/8 | +0.68 [+0.65, +1.00] µs | +0.95 [+0.50, +1.40] µs, 123/128 |
| ctl-scan-filter | 10,000 | 8 | -6.2 µs (-0.2 %) | [-16.8, -4.9] µs | 1/8 | -10.3 [-16.0, -8.2] µs | -8.0 [-22.9, +7.1] µs, 44/128 |

The pk `SELECT` control has no subquery and cannot reach `EvaluateSubChain`; it is slower by +0.70 µs, 8 of 8
runs, on a 96 µs statement. That is the floor this binary pair carries on a point statement, as it did at
`23e53bed` (+0.8 to +1.0 µs): a constant per statement, not a per-row cost. The correlated point arm
(+0.95 µs) is above the control by 0.25 µs, inside both IQRs, so **the correlated point statement shows no
delta beyond the control**. The whole-relation filter walk without a subquery moves by -0.6 ns per row
(B' faster in 7 of 8 runs), which is the scan-shaped control's noise on a 2.6 ms statement; it bounds how
much of a scan's per-row cost the two binaries can be said to differ by at 10,000 rows, by about 1 ns per row.

## 5. Where the time goes (rule 3)

The server's own time per statement (`SHOW META`'s `sched_foreground_polled_us` across each block, divided by
the statements in it) moves with the client's p50 in the two cells that resolve a delta: +2.26 µs engine against
+2.30 µs at 200 rows, +16.5 against +21.2 at 1,000. A's engine time per statement is 349 µs, 2.41 ms and 24.4 ms of a 543 µs, 3.08 ms and 30.4 ms statement at 200,
1,000 and 10,000 rows (64 %, 78 % and 80 %), 44 µs of the 109 µs correlated point statement and 33 µs of the
96 µs pk `SELECT`; the rest is the client, the socket and the reply. No fsync, write-statement
or lock wait exists in these statements (read-only, one connection, `relaxed`). The delta is the server's.

## 6. What the run teaches

- **The cost was the empty `StatusOr`, and it is gone to within the floor.** A few ns of building and testing
  a `StatusOr<bool>` per evaluation, on a path that runs once per outer row, was visible at 35 ns, 8 of 8
  in the positive control, and a one-line emptiness test in the caller brings it below what 8 to 16 pairs can
  resolve at 1,000 and 10,000 rows. The conclusion `23e53bed`'s file drew (the cost is on the correlated
  sub-chain evaluation) is confirmed by the removal.
- **A 35 ns per-row effect is at the limit of this harness on a 30 ms statement.** The 10,000-row cell has a
  per-run spread of about +-1 ms because 5 % or more of its statements take twice the median on this host, and
  only the positive control's 8 of 8 gives the cell any discrimination. A tighter bound at that size needs a
  quiet host or a longer cell, not more pairs.
- **The per-statement offset of about 0.7 to 1.0 µs between A and these binaries is common to every statement
  type, including the pk `SELECT`.** It is not a function of the change and is not distinguishable from the
  build's layout by this measurement.

## 7. Not measured, and what bounds this

- Only the cells the request named were run: no other arm of the earlier matrix (`unc-*`, `nested-*`,
  `ctl-insert`, `ctl-ping`) was run at B', and none of its numbers is repeated here. Rule 9's three sizes are
  covered for the scan arm only; the point and control cells are at the one size named (1,000 and 10,000).
- The suite was not executed. The cells ran at `cores = 1`, `relaxed`; `strict` and `group` add commit waits
  that do not touch a read-only statement and were not run.
- The 8-pair figures are the pre-specified result. The extension (8 more pairs) and the positive control were
  added after the first 8 showed a mixed sign at 1,000 and 10,000 rows; the pooled and control figures are
  labelled as such and the pre-specified figures are reported alongside, unchanged.
- The host was shared (section 1); the floor stated is the floor of this host in this hour.

Raw files, drivers, per-run deltas and the analysis: `bench/v3.0.0/archive/nested-hoist-overhead-followup-v2.7.0-750-g9004a0cb/`
(`report.txt`, `per-run-deltas.txt`, `pooled-16-runs.txt`; `main/`, `swap/`, `ext/`, `control/` hold each run's gzipped JSON, driver output and host record).
