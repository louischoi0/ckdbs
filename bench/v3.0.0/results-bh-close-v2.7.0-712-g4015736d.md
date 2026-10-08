# BH's close, re-measured at the second closing commit (B')

**Thesis.** Two fixes landed after the first close measurement
(`bench/v3.0.0/results-bh-close-v2.7.0-711-g04649133.md`, "the 711 file"):
a split that finds a retired slot before it splits (`btree::CompactLeafAndInsert`),
and an integer-literal guard in the parser (`IntLiteralWrapped`). Against the
commit BH opened at, **no point statement and no bulk-insert shape is slower in B'
beyond noise at `cores = 1` or `cores = 2`, at 200, 1,000 and 10,000 rows**; the bulk
INSERT that drives many append splits is 0.5-1.0 µs (0.5-1 %) *faster*. **The
separator refusal of the 711 file's section 5 is gone**: every purged key is placed
again, in every purge cell and in the repro. The one non-trivial positive delta is the
whole-relation scan at 10,000 rows (+14 to +18 µs, about 2 %).

- **A** = `10593366` (`v2.7.0-706-g10593366`), the commit BH opened at; the same binary as the 711 file.
- **B'** = `4015736d` (`v2.7.0-712-g4015736d`), BH closed with the two fixes. The 711 file's
  B was `04649133` (`v2.7.0-711-g04649133`).

| Item | Value |
|---|---|
| Executed | 2026-10-08, 12:09-12:27 UTC |
| Branch / worktree | `worktree-bh-purge-key`, worktree `bh-purge-key` |
| B' | `4015736d`, `v2.7.0-712-g4015736d`; **tree clean** (`git status --short` empty before and after the build); `build-release` rebuilt at HEAD, mtime 2026-10-08 12:08:45 UTC (after HEAD), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, Release. `build/` (Debug) not touched |
| A | the 711 file's A binary, reused unchanged |
| Measured copies (`archive/.../binaries.txt`) | A `d136d1e8dee6f8c18ec19abe250130d78c04f8a9e7b4cfd932bb8685f6203cc8`, B' `44bfd0cf09e439ef265f727d1c965542311142a9197a9d40d787976f54c3da05`; servers ran from copies under `/home/cdkbs/bench-runs/bh-close2/bin/` |
| Host / device | as the 711 file: 8 vCPU AMD EPYC 9V74, `/dev/root` ext4, server on CPU 2 (`cores = 1`) or 2,3, driver on CPU 4 |
| Server config | `buffer_pool_frames = 65536`, `log_level = warn`, `durability = relaxed` (overhead and most purge cells; `strict` for two purge cells), fresh server and data file per (cell, run); ports 31811/31812 (A/B'), 31821 (purge), 31731 (repro) |
| Host load | loadavg and the competing-process list recorded per run: 162 run records, loadavg 1.22-1.74 (the driver's own pair); the only list matches are another session's idle shell and a `pgrep` itself, no build or test run |
| Suite | not executed in this session |

## What was re-run, and what was not

Re-run at B': the overhead A/B (all of the 711 file's shapes, plus one new arm),
at `cores = 1` and `2`, 200/1,000/10,000 rows, 16 runs each (method and noise
reporting exactly as the 711 file's "How it was run"; the driver is the 711 file's
`bh_overhead_ab.py` plus the arm below); the repro of the 711 file's section 5;
the purge-throughput cells (`relaxed` at both core counts, 5 runs, the 711 file's
cells; `strict` at `cores = 1` only, 3 runs).

**New arm, `bulk20`:** one `INSERT` of 20 rows with ascending named keys, 3,000
statements per arm and run (60,000 rows), which drives about 360 append splits
per arm and run. This is the shape that pays the new retired-slot scan on every
full-leaf insert. Scratch driver, rule 5 broken as in the 711 file.

Not re-run (the unchanged cells are in the 711 file, read at `04649133`): the
DELETE-to-PURGE latency cells, the snapshot probe, the `group` cells and `strict`
at `cores = 2`, the stage bisection. The scan shape's sign problem (711 file,
section 2) is not re-bisected here; B' is one more build of that walk.

## 1. Overhead: nothing slower, the bulk insert slightly faster

Median per-run p50 delta B' - A with its 95 % CI (bootstrap of the 16 runs) and the
median per-run throughput delta. The noise floor inside the run: the control
(`SHOW META`) moves -0.1 to 0 µs at `cores = 1` and up to -1.15 µs at one
`cores = 2` cell (200 rows; 16 runs, wide CI); the replicate arm differs by
-0.2 to +0.05 µs. **A delta under ~0.6 µs is not a finding**; deltas over it:

- **UPDATE by pk is 0.35-0.70 µs faster in B'** (1 %), significant at 10,000 rows
  at both core counts and at 1,000 rows at `cores = 2`. Not a cost.
- **Bulk INSERT, 20 rows ascending, is 0.45-0.95 µs faster** (0.5-1 %), CI excluding
  zero at `cores = 1` in all three sizes. The retired-slot scan costs nothing
  resolvable on the path that does the most append splits: a leaf that is full of
  live rows has no retired slot to find, and the scan is a copy-free pass over a
  page already held.
- **`range100` (whole-relation walk) at 10,000 rows: +14.05 µs [+10.45, +21.20]
  (+1.8 %) at `cores = 1` and +17.80 µs [+10.60, +22.40] (+2.2 %) at `cores = 2`;**
  at 1,000 rows +1.30 µs [+1.00, +2.00] and +0.85 µs [0.00, +1.50]. Small, same
  direction at both core counts, and the walk's own code is unchanged since A (711 file,
  section 2); that file measured this shape's sign moving between -8 % and +13 % with
  the build alone, so a 2 % here is below what that evidence can attribute to either fix.
  Stated as measured, not explained.

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.00], +0.4 % qps | -0.10 µs [-0.20, +0.10], +0.3 % qps | +0.00 µs [-0.10, +0.15], +0.6 % qps | -0.10 µs [-0.50, -0.05], +1.1 % qps | -0.30 µs [-0.35, -0.10], +0.8 % qps | -0.25 µs [-0.50, +0.00], +0.7 % qps |
| INSERT again (replicate of the line above) | -0.10 µs [-0.20, +0.00], +0.3 % qps | -0.05 µs [-0.20, +0.10], +1.0 % qps | +0.05 µs [-0.10, +0.10], +1.1 % qps | -0.15 µs [-0.40, +0.00], +0.2 % qps | -0.20 µs [-0.40, -0.10], +0.7 % qps | -0.10 µs [-0.30, +0.10], +1.3 % qps |
| SELECT by pk | +0.30 µs [-0.05, +0.45], -0.2 % qps | -0.15 µs [-0.80, +0.40], +0.6 % qps | -0.15 µs [-0.70, +0.10], +0.4 % qps | -0.10 µs [-0.40, +0.40], +0.2 % qps | -0.40 µs [-0.60, +0.00], +0.4 % qps | -0.30 µs [-0.90, +0.20], +0.4 % qps |
| UPDATE by pk | -0.20 µs [-0.50, +0.10], -0.1 % qps | -0.35 µs [-1.00, +0.00], +2.6 % qps | -0.55 µs [-0.70, -0.20], +1.8 % qps | -0.30 µs [-0.70, -0.10], +0.1 % qps | -0.55 µs [-0.90, -0.20], +0.6 % qps | -0.70 µs [-0.90, -0.50], +1.1 % qps |
| DELETE by pk | -0.20 µs [-0.30, +0.10], +0.4 % qps | +0.10 µs [-0.10, +0.20], -0.7 % qps | -0.05 µs [-0.10, +0.20], +0.5 % qps | -0.20 µs [-0.50, +0.00], +0.8 % qps | -0.35 µs [-0.50, +0.00], +1.5 % qps | -0.25 µs [-0.60, +0.00], +0.7 % qps |
| bulk INSERT, 20 rows ascending | -0.95 µs [-1.20, -0.75], +0.4 % qps | -0.85 µs [-1.15, -0.20], +0.8 % qps | -0.70 µs [-1.30, -0.35], +1.0 % qps | -0.45 µs [-0.90, +0.10], -0.3 % qps | -0.55 µs [-1.10, -0.15], +0.2 % qps | -0.55 µs [-0.90, +0.10], +0.3 % qps |
| COUNT/MIN/MAX over 100 ids (scans the relation) | -0.40 µs [-0.70, -0.20], +0.6 % qps | +1.30 µs [+1.00, +2.00], -0.8 % qps | +14.05 µs [+10.45, +21.20], -2.0 % qps | -0.15 µs [-0.80, +0.00], +0.4 % qps | +0.85 µs [+0.00, +1.50], -0.4 % qps | +17.80 µs [+10.60, +22.40], -2.8 % qps |
| SHOW META (control) | -0.10 µs [-0.20, +0.05], +0.2 % qps | -0.10 µs [-0.25, +0.00], +0.2 % qps | +0.00 µs [-0.15, +0.10], +0.1 % qps | -1.15 µs [-2.60, -0.10], +3.0 % qps | -0.10 µs [-1.35, +0.10], +0.5 % qps | -0.10 µs [-1.50, +0.20], +0.7 % qps |


A's p50 in each cell (median of the 16 per-run p50s):

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.4 µs | 49.3 µs | 49.2 µs | 49.2 µs | 49.3 µs | 49.3 µs |
| INSERT again (replicate of the line above) | 49.3 µs | 49.4 µs | 49.2 µs | 49.2 µs | 49.2 µs | 49.2 µs |
| SELECT by pk | 65.0 µs | 66.7 µs | 64.6 µs | 66.0 µs | 68.0 µs | 65.4 µs |
| UPDATE by pk | 51.4 µs | 51.7 µs | 52.0 µs | 52.5 µs | 52.6 µs | 53.1 µs |
| DELETE by pk | 49.3 µs | 49.2 µs | 49.2 µs | 49.6 µs | 49.5 µs | 49.5 µs |
| bulk INSERT, 20 rows ascending | 95.4 µs | 94.7 µs | 95.7 µs | 101.5 µs | 101.7 µs | 101.9 µs |
| COUNT/MIN/MAX over 100 ids (scans the relation) | 86.8 µs | 144.1 µs | 790.2 µs | 87.7 µs | 146.6 µs | 798.6 µs |
| SHOW META (control) | 45.4 µs | 45.4 µs | 45.4 µs | 45.7 µs | 45.6 µs | 45.5 µs |

Waits: every cell is `durability = relaxed`, so there is no commit wait; the
`SHOW META` floor is 45.4-45.7 µs at p50, INSERT/DELETE sit 4 µs above it, UPDATE 6 µs,
SELECT 19-22 µs, a 20-row INSERT 50 µs; 0 errors and no lock or conflict wait in
96 runs.

Full percentile tables (48,000 statements per arm and shape, 16 runs pooled):

### `cores = 1`

**cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.28-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.0 µs | 48.4 µs | 49.4 µs | 50.3 µs | 63.3 µs | 77.4 µs | 51.1 µs (sd 0.8) | 19,279 qps |
| INSERT, pk omitted | B | 40.5 µs | 48.3 µs | 49.3 µs | 50.2 µs | 62.5 µs | 74.9 µs | 50.7 µs (sd 0.6) | 19,506 qps |
| INSERT again (replicate) | A | 40.4 µs | 48.4 µs | 49.3 µs | 50.3 µs | 63.1 µs | 76.3 µs | 52.7 µs (sd 0.7) | 18,682 qps |
| INSERT again (replicate) | B | 40.8 µs | 48.3 µs | 49.2 µs | 50.1 µs | 62.8 µs | 74.9 µs | 53.1 µs (sd 1.4) | 18,716 qps |
| SELECT by pk | A | 57.7 µs | 63.9 µs | 64.9 µs | 68.1 µs | 80.7 µs | 94.8 µs | 67.5 µs (sd 0.6) | 14,680 qps |
| SELECT by pk | B | 57.3 µs | 64.2 µs | 65.2 µs | 68.4 µs | 80.9 µs | 95.1 µs | 67.8 µs (sd 0.7) | 14,571 qps |
| UPDATE by pk | A | 40.0 µs | 50.4 µs | 51.5 µs | 53.0 µs | 66.3 µs | 79.2 µs | 56.0 µs (sd 1.4) | 17,767 qps |
| UPDATE by pk | B | 39.9 µs | 49.9 µs | 51.3 µs | 52.7 µs | 65.6 µs | 77.3 µs | 56.0 µs (sd 1.1) | 17,700 qps |
| DELETE by pk | A | 40.9 µs | 48.4 µs | 49.3 µs | 50.3 µs | 62.4 µs | 71.6 µs | 52.8 µs (sd 0.8) | 18,813 qps |
| DELETE by pk | B | 38.8 µs | 48.4 µs | 49.2 µs | 50.2 µs | 61.8 µs | 71.2 µs | 52.5 µs (sd 1.0) | 19,030 qps |
| bulk INSERT, 20 rows ascending | A | 88.0 µs | 93.6 µs | 95.5 µs | 105.8 µs | 121.5 µs | 166.9 µs | 104.1 µs (sd 1.4) | 9,088 qps |
| bulk INSERT, 20 rows ascending | B | 88.7 µs | 92.7 µs | 94.6 µs | 105.4 µs | 120.7 µs | 165.1 µs | 104.1 µs (sd 2.0) | 9,088 qps |
| COUNT/MIN/MAX over 100 ids | A | 80.8 µs | 85.9 µs | 86.9 µs | 89.5 µs | 103.0 µs | 113.1 µs | 89.5 µs (sd 1.1) | 11,094 qps |
| COUNT/MIN/MAX over 100 ids | B | 81.2 µs | 85.6 µs | 86.6 µs | 89.1 µs | 102.6 µs | 112.4 µs | 89.1 µs (sd 0.9) | 11,124 qps |
| SHOW META (control) | A | 34.6 µs | 44.7 µs | 45.4 µs | 46.1 µs | 54.6 µs | 62.0 µs | 46.1 µs (sd 0.5) | 21,527 qps |
| SHOW META (control) | B | 31.3 µs | 44.6 µs | 45.4 µs | 46.0 µs | 54.3 µs | 61.5 µs | 46.0 µs (sd 0.6) | 21,591 qps |

| shape (cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.00] | -0.39 µs [-0.67, -0.16] | +0.39 % [+0.15, +1.21] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.20, +0.00] | +0.34 µs [-0.21, +1.04] | +0.27 % [-1.36, +0.70] | no cost resolved |
| SELECT by pk | +0.30 µs [-0.05, +0.45] | +0.26 µs [+0.07, +0.49] | -0.20 % [-0.63, -0.06] | no cost resolved |
| UPDATE by pk | -0.20 µs [-0.50, +0.10] | -0.05 µs [-0.83, +0.64] | -0.12 % [-1.14, +1.22] | no cost resolved |
| DELETE by pk | -0.20 µs [-0.30, +0.10] | -0.23 µs [-0.73, +0.26] | +0.35 % [-0.34, +1.12] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.95 µs [-1.20, -0.75] | +0.01 µs [-0.95, +1.02] | +0.36 % [-1.19, +0.96] | B faster (-1.0 % at p50) |
| COUNT/MIN/MAX over 100 ids | -0.40 µs [-0.70, -0.20] | -0.41 µs [-0.81, -0.02] | +0.55 % [+0.01, +0.93] | no cost resolved |
| SHOW META (control) | -0.10 µs [-0.20, +0.05] | -0.09 µs [-0.31, +0.14] | +0.19 % [-0.48, +0.90] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.13 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.13 µs.

**cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.42-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.5 µs | 49.4 µs | 50.3 µs | 62.0 µs | 76.9 µs | 50.9 µs (sd 0.4) | 19,338 qps |
| INSERT, pk omitted | B | 40.7 µs | 48.5 µs | 49.3 µs | 50.2 µs | 61.4 µs | 75.5 µs | 50.7 µs (sd 0.4) | 19,473 qps |
| INSERT again (replicate) | A | 40.8 µs | 48.6 µs | 49.4 µs | 50.2 µs | 61.9 µs | 75.7 µs | 53.5 µs (sd 1.0) | 18,565 qps |
| INSERT again (replicate) | B | 40.4 µs | 48.5 µs | 49.3 µs | 50.2 µs | 61.6 µs | 73.9 µs | 52.8 µs (sd 0.7) | 18,753 qps |
| SELECT by pk | A | 58.5 µs | 64.4 µs | 66.7 µs | 72.4 µs | 83.5 µs | 98.2 µs | 69.7 µs (sd 0.8) | 14,256 qps |
| SELECT by pk | B | 57.8 µs | 64.4 µs | 66.6 µs | 72.3 µs | 83.0 µs | 95.6 µs | 69.4 µs (sd 0.6) | 14,328 qps |
| UPDATE by pk | A | 40.2 µs | 50.7 µs | 51.6 µs | 53.0 µs | 64.9 µs | 77.6 µs | 58.4 µs (sd 1.7) | 17,083 qps |
| UPDATE by pk | B | 39.7 µs | 49.7 µs | 51.2 µs | 52.5 µs | 64.5 µs | 76.5 µs | 57.5 µs (sd 1.3) | 17,141 qps |
| DELETE by pk | A | 40.8 µs | 48.4 µs | 49.2 µs | 50.2 µs | 60.7 µs | 69.9 µs | 52.4 µs (sd 0.5) | 18,996 qps |
| DELETE by pk | B | 37.4 µs | 48.5 µs | 49.3 µs | 50.2 µs | 60.8 µs | 70.2 µs | 53.2 µs (sd 1.5) | 18,852 qps |
| bulk INSERT, 20 rows ascending | A | 87.5 µs | 93.1 µs | 94.8 µs | 104.7 µs | 119.4 µs | 161.7 µs | 104.0 µs (sd 1.9) | 9,128 qps |
| bulk INSERT, 20 rows ascending | B | 89.0 µs | 92.4 µs | 94.3 µs | 104.5 µs | 118.9 µs | 162.2 µs | 102.6 µs (sd 1.1) | 9,252 qps |
| COUNT/MIN/MAX over 100 ids | A | 134.7 µs | 141.5 µs | 144.4 µs | 150.2 µs | 162.0 µs | 174.6 µs | 147.0 µs (sd 0.9) | 6,786 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.1 µs | 142.9 µs | 145.8 µs | 151.2 µs | 163.4 µs | 176.7 µs | 148.4 µs (sd 1.0) | 6,712 qps |
| SHOW META (control) | A | 32.1 µs | 44.8 µs | 45.5 µs | 46.1 µs | 53.1 µs | 60.2 µs | 46.0 µs (sd 0.3) | 21,634 qps |
| SHOW META (control) | B | 31.2 µs | 44.5 µs | 45.3 µs | 45.9 µs | 53.3 µs | 60.3 µs | 45.8 µs (sd 0.4) | 21,705 qps |

| shape (cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.10] | -0.18 µs [-0.32, -0.04] | +0.25 % [-0.05, +0.79] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.10] | -0.67 µs [-1.08, -0.27] | +1.01 % [+0.48, +2.61] | no cost resolved |
| SELECT by pk | -0.15 µs [-0.80, +0.40] | -0.31 µs [-0.71, +0.07] | +0.59 % [-0.52, +1.15] | no cost resolved |
| UPDATE by pk | -0.35 µs [-1.00, +0.00] | -0.89 µs [-1.63, -0.14] | +2.62 % [+0.10, +2.99] | no cost resolved |
| DELETE by pk | +0.10 µs [-0.10, +0.20] | +0.74 µs [+0.01, +1.58] | -0.72 % [-1.87, +0.73] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.85 µs [-1.15, -0.20] | -1.40 µs [-2.66, -0.28] | +0.78 % [+0.47, +1.06] | B faster (-0.9 % at p50) |
| COUNT/MIN/MAX over 100 ids | +1.30 µs [+1.00, +2.00] | +1.36 µs [+0.98, +1.72] | -0.85 % [-1.31, -0.73] | **B slower** (+0.9 % at p50) |
| SHOW META (control) | -0.10 µs [-0.25, +0.00] | -0.17 µs [-0.41, +0.06] | +0.17 % [-0.48, +1.02] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.10 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.14 µs.

**cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.36-1.58, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.4 µs | 49.2 µs | 50.2 µs | 61.6 µs | 77.2 µs | 50.8 µs (sd 0.3) | 19,467 qps |
| INSERT, pk omitted | B | 40.9 µs | 48.3 µs | 49.2 µs | 50.0 µs | 60.9 µs | 74.2 µs | 50.5 µs (sd 0.3) | 19,539 qps |
| INSERT again (replicate) | A | 40.2 µs | 48.4 µs | 49.2 µs | 50.1 µs | 62.0 µs | 76.5 µs | 54.7 µs (sd 1.1) | 18,160 qps |
| INSERT again (replicate) | B | 40.9 µs | 48.3 µs | 49.2 µs | 50.0 µs | 61.2 µs | 75.0 µs | 54.6 µs (sd 1.4) | 18,159 qps |
| SELECT by pk | A | 58.9 µs | 63.8 µs | 64.6 µs | 66.0 µs | 76.8 µs | 84.6 µs | 66.2 µs (sd 0.6) | 14,962 qps |
| SELECT by pk | B | 58.1 µs | 63.4 µs | 64.3 µs | 65.7 µs | 76.0 µs | 83.2 µs | 65.8 µs (sd 0.5) | 15,062 qps |
| UPDATE by pk | A | 43.7 µs | 51.4 µs | 52.1 µs | 53.7 µs | 66.5 µs | 81.9 µs | 56.5 µs (sd 1.2) | 17,648 qps |
| UPDATE by pk | B | 41.1 µs | 50.8 µs | 51.6 µs | 52.9 µs | 64.7 µs | 76.5 µs | 56.5 µs (sd 2.7) | 17,884 qps |
| DELETE by pk | A | 40.4 µs | 48.5 µs | 49.2 µs | 50.3 µs | 61.6 µs | 71.0 µs | 50.8 µs (sd 0.4) | 19,605 qps |
| DELETE by pk | B | 40.6 µs | 48.4 µs | 49.3 µs | 50.2 µs | 61.0 µs | 70.0 µs | 50.5 µs (sd 0.4) | 19,681 qps |
| bulk INSERT, 20 rows ascending | A | 89.3 µs | 94.1 µs | 96.2 µs | 106.0 µs | 120.8 µs | 161.2 µs | 105.6 µs (sd 2.0) | 8,988 qps |
| bulk INSERT, 20 rows ascending | B | 86.5 µs | 93.1 µs | 94.9 µs | 105.6 µs | 120.5 µs | 164.0 µs | 104.1 µs (sd 1.5) | 9,078 qps |
| COUNT/MIN/MAX over 100 ids | A | 712.0 µs | 768.0 µs | 792.0 µs | 815.8 µs | 847.6 µs | 899.4 µs | 797.2 µs (sd 9.8) | 1,255 qps |
| COUNT/MIN/MAX over 100 ids | B | 726.9 µs | 781.5 µs | 806.0 µs | 831.0 µs | 863.6 µs | 893.6 µs | 812.9 µs (sd 10.7) | 1,235 qps |
| SHOW META (control) | A | 36.6 µs | 44.8 µs | 45.4 µs | 46.0 µs | 52.9 µs | 59.8 µs | 45.9 µs (sd 0.4) | 21,689 qps |
| SHOW META (control) | B | 31.1 µs | 44.7 µs | 45.4 µs | 46.0 µs | 52.6 µs | 59.3 µs | 45.8 µs (sd 0.4) | 21,728 qps |

| shape (cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.15] | -0.26 µs [-0.38, -0.14] | +0.56 % [+0.17, +0.85] | no cost resolved |
| INSERT again (replicate) | +0.05 µs [-0.10, +0.10] | -0.09 µs [-0.56, +0.42] | +1.12 % [-1.83, +1.36] | no cost resolved |
| SELECT by pk | -0.15 µs [-0.70, +0.10] | -0.44 µs [-0.78, -0.11] | +0.39 % [+0.06, +1.01] | no cost resolved |
| UPDATE by pk | -0.55 µs [-0.70, -0.20] | -0.05 µs [-1.22, +1.37] | +1.77 % [-0.71, +2.62] | no cost resolved |
| DELETE by pk | -0.05 µs [-0.10, +0.20] | -0.22 µs [-0.31, -0.13] | +0.50 % [+0.25, +0.60] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.70 µs [-1.30, -0.35] | -1.52 µs [-2.64, -0.46] | +0.99 % [-0.11, +3.07] | B faster (-0.7 % at p50) |
| COUNT/MIN/MAX over 100 ids | +14.05 µs [+10.45, +21.20] | +15.64 µs [+8.91, +21.88] | -1.98 % [-3.25, -1.16] | **B slower** (+1.8 % at p50) |
| SHOW META (control) | +0.00 µs [-0.15, +0.10] | -0.06 µs [-0.22, +0.10] | +0.08 % [-0.38, +0.63] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.10 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.17 µs.

### `cores = 2`

**cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.41-1.59, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.3 µs | 50.9 µs | 66.0 µs | 75.8 µs | 51.6 µs (sd 0.5) | 19,141 qps |
| INSERT, pk omitted | B | 42.4 µs | 48.4 µs | 49.1 µs | 50.5 µs | 65.2 µs | 73.8 µs | 51.2 µs (sd 0.4) | 19,279 qps |
| INSERT again (replicate) | A | 43.2 µs | 48.7 µs | 49.3 µs | 50.8 µs | 65.9 µs | 76.6 µs | 53.8 µs (sd 0.8) | 18,374 qps |
| INSERT again (replicate) | B | 41.9 µs | 48.4 µs | 49.1 µs | 50.6 µs | 65.1 µs | 74.9 µs | 53.5 µs (sd 0.7) | 18,520 qps |
| SELECT by pk | A | 58.8 µs | 65.1 µs | 66.0 µs | 69.8 µs | 83.8 µs | 94.8 µs | 69.0 µs (sd 0.7) | 14,375 qps |
| SELECT by pk | B | 60.4 µs | 65.1 µs | 66.0 µs | 69.9 µs | 83.7 µs | 94.5 µs | 68.9 µs (sd 0.7) | 14,394 qps |
| UPDATE by pk | A | 46.5 µs | 51.8 µs | 52.5 µs | 54.3 µs | 68.3 µs | 77.2 µs | 57.6 µs (sd 1.9) | 17,325 qps |
| UPDATE by pk | B | 46.3 µs | 51.4 µs | 52.1 µs | 53.7 µs | 67.5 µs | 75.6 µs | 56.9 µs (sd 1.0) | 17,382 qps |
| DELETE by pk | A | 42.9 µs | 49.0 µs | 49.6 µs | 51.0 µs | 64.6 µs | 72.8 µs | 54.1 µs (sd 1.8) | 18,560 qps |
| DELETE by pk | B | 41.2 µs | 48.6 µs | 49.3 µs | 50.7 µs | 63.4 µs | 71.0 µs | 53.5 µs (sd 1.2) | 18,722 qps |
| bulk INSERT, 20 rows ascending | A | 96.0 µs | 99.3 µs | 101.6 µs | 113.0 µs | 129.5 µs | 166.3 µs | 110.4 µs (sd 1.0) | 8,596 qps |
| bulk INSERT, 20 rows ascending | B | 94.9 µs | 98.9 µs | 101.5 µs | 113.2 µs | 130.2 µs | 169.4 µs | 110.9 µs (sd 1.6) | 8,590 qps |
| COUNT/MIN/MAX over 100 ids | A | 83.0 µs | 86.7 µs | 87.6 µs | 91.0 µs | 107.6 µs | 116.3 µs | 90.8 µs (sd 0.7) | 10,946 qps |
| COUNT/MIN/MAX over 100 ids | B | 83.5 µs | 86.4 µs | 87.4 µs | 90.4 µs | 106.8 µs | 114.3 µs | 90.3 µs (sd 0.9) | 11,009 qps |
| SHOW META (control) | A | 34.0 µs | 44.7 µs | 45.7 µs | 46.4 µs | 56.4 µs | 64.2 µs | 46.4 µs (sd 0.6) | 21,362 qps |
| SHOW META (control) | B | 32.9 µs | 42.7 µs | 45.0 µs | 46.0 µs | 55.3 µs | 62.9 µs | 45.5 µs (sd 0.7) | 21,970 qps |

| shape (cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.50, -0.05] | -0.44 µs [-0.66, -0.22] | +1.07 % [+0.15, +1.35] | no cost resolved |
| INSERT again (replicate) | -0.15 µs [-0.40, +0.00] | -0.34 µs [-0.84, +0.16] | +0.23 % [-0.24, +1.35] | no cost resolved |
| SELECT by pk | -0.10 µs [-0.40, +0.40] | -0.06 µs [-0.40, +0.29] | +0.18 % [-0.46, +0.68] | no cost resolved |
| UPDATE by pk | -0.30 µs [-0.70, -0.10] | -0.70 µs [-1.76, +0.05] | +0.11 % [-0.28, +1.24] | no cost resolved |
| DELETE by pk | -0.20 µs [-0.50, +0.00] | -0.63 µs [-1.84, +0.51] | +0.84 % [-0.65, +2.50] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.45 µs [-0.90, +0.10] | +0.42 µs [-0.33, +1.22] | -0.29 % [-0.45, +0.41] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -0.15 µs [-0.80, +0.00] | -0.53 µs [-0.98, -0.08] | +0.45 % [+0.05, +0.91] | no cost resolved |
| SHOW META (control) | -1.15 µs [-2.60, -0.10] | -0.92 µs [-1.42, -0.36] | +3.02 % [+0.25, +3.57] | B faster (-2.5 % at p50) |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.12 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.05 µs, sd 0.14 µs.

**cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.37-1.57, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.3 µs | 50.8 µs | 65.8 µs | 75.1 µs | 51.5 µs (sd 0.3) | 19,165 qps |
| INSERT, pk omitted | B | 42.6 µs | 48.4 µs | 49.1 µs | 50.5 µs | 65.5 µs | 74.7 µs | 51.1 µs (sd 0.3) | 19,301 qps |
| INSERT again (replicate) | A | 39.3 µs | 48.6 µs | 49.3 µs | 50.7 µs | 65.5 µs | 75.3 µs | 54.2 µs (sd 0.9) | 18,325 qps |
| INSERT again (replicate) | B | 40.6 µs | 48.4 µs | 49.1 µs | 50.4 µs | 64.8 µs | 74.4 µs | 54.5 µs (sd 2.2) | 18,411 qps |
| SELECT by pk | A | 60.8 µs | 65.6 µs | 68.0 µs | 73.8 µs | 86.8 µs | 99.3 µs | 71.0 µs (sd 0.5) | 13,958 qps |
| SELECT by pk | B | 61.2 µs | 65.3 µs | 67.6 µs | 73.5 µs | 86.3 µs | 98.0 µs | 70.7 µs (sd 0.4) | 14,017 qps |
| UPDATE by pk | A | 47.1 µs | 51.9 µs | 52.6 µs | 54.4 µs | 68.4 µs | 76.9 µs | 59.6 µs (sd 1.2) | 16,619 qps |
| UPDATE by pk | B | 46.8 µs | 51.4 µs | 52.1 µs | 53.8 µs | 67.8 µs | 75.4 µs | 59.4 µs (sd 1.3) | 16,700 qps |
| DELETE by pk | A | 43.5 µs | 48.9 µs | 49.5 µs | 51.1 µs | 64.9 µs | 72.4 µs | 54.0 µs (sd 0.7) | 18,450 qps |
| DELETE by pk | B | 42.9 µs | 48.5 µs | 49.2 µs | 50.6 µs | 63.1 µs | 70.8 µs | 53.2 µs (sd 0.8) | 18,764 qps |
| bulk INSERT, 20 rows ascending | A | 95.9 µs | 99.4 µs | 101.7 µs | 113.1 µs | 130.2 µs | 167.0 µs | 111.0 µs (sd 1.5) | 8,586 qps |
| bulk INSERT, 20 rows ascending | B | 95.3 µs | 98.9 µs | 101.3 µs | 113.1 µs | 129.3 µs | 171.7 µs | 110.9 µs (sd 1.5) | 8,542 qps |
| COUNT/MIN/MAX over 100 ids | A | 135.9 µs | 143.3 µs | 146.6 µs | 154.7 µs | 169.7 µs | 184.5 µs | 150.3 µs (sd 1.3) | 6,628 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.7 µs | 144.3 µs | 147.7 µs | 154.9 µs | 169.8 µs | 180.8 µs | 151.0 µs (sd 1.1) | 6,595 qps |
| SHOW META (control) | A | 36.5 µs | 43.5 µs | 45.5 µs | 46.2 µs | 55.5 µs | 63.6 µs | 46.0 µs (sd 0.9) | 21,501 qps |
| SHOW META (control) | B | 31.5 µs | 42.8 µs | 45.2 µs | 46.1 µs | 54.8 µs | 62.3 µs | 45.6 µs (sd 0.8) | 21,808 qps |

| shape (cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.30 µs [-0.35, -0.10] | -0.39 µs [-0.58, -0.19] | +0.80 % [+0.39, +1.29] | no cost resolved |
| INSERT again (replicate) | -0.20 µs [-0.40, -0.10] | +0.34 µs [-0.57, +1.50] | +0.72 % [-0.86, +1.48] | no cost resolved |
| SELECT by pk | -0.40 µs [-0.60, +0.00] | -0.38 µs [-0.65, -0.11] | +0.35 % [+0.17, +1.00] | no cost resolved |
| UPDATE by pk | -0.55 µs [-0.90, -0.20] | -0.16 µs [-0.73, +0.41] | +0.64 % [-1.72, +1.57] | no cost resolved |
| DELETE by pk | -0.35 µs [-0.50, +0.00] | -0.80 µs [-1.42, -0.21] | +1.52 % [-0.05, +2.71] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.55 µs [-1.10, -0.15] | -0.09 µs [-1.05, +0.92] | +0.19 % [-0.82, +1.12] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +0.85 µs [+0.00, +1.50] | +0.69 µs [-0.06, +1.60] | -0.35 % [-0.85, +0.14] | no cost resolved |
| SHOW META (control) | -0.10 µs [-1.35, +0.10] | -0.43 µs [-0.96, +0.11] | +0.48 % [-0.23, +2.55] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.16 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.20 µs.

**cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.27-1.59, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.8 µs | 48.6 µs | 49.4 µs | 51.0 µs | 67.2 µs | 83.7 µs | 51.9 µs (sd 1.0) | 19,105 qps |
| INSERT, pk omitted | B | 42.0 µs | 48.3 µs | 49.1 µs | 50.6 µs | 66.0 µs | 77.4 µs | 51.3 µs (sd 0.6) | 19,212 qps |
| INSERT again (replicate) | A | 42.4 µs | 48.5 µs | 49.2 µs | 50.6 µs | 65.6 µs | 76.4 µs | 57.1 µs (sd 1.0) | 17,368 qps |
| INSERT again (replicate) | B | 41.6 µs | 48.4 µs | 49.1 µs | 50.4 µs | 65.1 µs | 75.9 µs | 56.2 µs (sd 0.8) | 17,626 qps |
| SELECT by pk | A | 59.2 µs | 64.8 µs | 65.5 µs | 67.4 µs | 81.2 µs | 90.1 µs | 67.7 µs (sd 0.6) | 14,630 qps |
| SELECT by pk | B | 59.2 µs | 64.5 µs | 65.3 µs | 67.0 µs | 80.6 µs | 89.6 µs | 67.4 µs (sd 0.6) | 14,702 qps |
| UPDATE by pk | A | 47.6 µs | 52.3 µs | 53.2 µs | 55.2 µs | 69.2 µs | 78.0 µs | 57.7 µs (sd 0.8) | 17,205 qps |
| UPDATE by pk | B | 46.1 µs | 51.8 µs | 52.6 µs | 54.6 µs | 68.4 µs | 76.3 µs | 57.1 µs (sd 1.1) | 17,460 qps |
| DELETE by pk | A | 43.1 µs | 48.8 µs | 49.5 µs | 51.2 µs | 64.5 µs | 72.8 µs | 51.5 µs (sd 0.6) | 19,380 qps |
| DELETE by pk | B | 41.8 µs | 48.5 µs | 49.2 µs | 50.6 µs | 63.3 µs | 71.0 µs | 51.0 µs (sd 0.4) | 19,497 qps |
| bulk INSERT, 20 rows ascending | A | 95.9 µs | 99.5 µs | 102.1 µs | 112.7 µs | 129.1 µs | 168.3 µs | 111.0 µs (sd 1.4) | 8,557 qps |
| bulk INSERT, 20 rows ascending | B | 95.3 µs | 99.0 µs | 101.6 µs | 112.6 µs | 128.8 µs | 170.6 µs | 110.1 µs (sd 0.9) | 8,590 qps |
| COUNT/MIN/MAX over 100 ids | A | 718.1 µs | 778.5 µs | 803.0 µs | 828.0 µs | 860.2 µs | 919.6 µs | 807.7 µs (sd 10.6) | 1,237 qps |
| COUNT/MIN/MAX over 100 ids | B | 734.0 µs | 793.8 µs | 818.6 µs | 844.2 µs | 874.5 µs | 907.8 µs | 828.9 µs (sd 9.4) | 1,206 qps |
| SHOW META (control) | A | 36.7 µs | 43.0 µs | 45.4 µs | 46.2 µs | 55.9 µs | 64.1 µs | 45.9 µs (sd 0.8) | 21,500 qps |
| SHOW META (control) | B | 31.5 µs | 42.7 µs | 45.3 µs | 46.1 µs | 55.6 µs | 63.4 µs | 45.7 µs (sd 0.9) | 21,644 qps |

| shape (cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.25 µs [-0.50, +0.00] | -0.63 µs [-0.99, -0.34] | +0.74 % [+0.25, +1.61] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | -0.90 µs [-1.31, -0.54] | +1.27 % [+0.60, +2.10] | no cost resolved |
| SELECT by pk | -0.30 µs [-0.90, +0.20] | -0.34 µs [-0.75, +0.03] | +0.45 % [-0.56, +1.59] | no cost resolved |
| UPDATE by pk | -0.70 µs [-0.90, -0.50] | -0.60 µs [-1.17, +0.05] | +1.07 % [+0.80, +2.01] | B faster (-1.3 % at p50) |
| DELETE by pk | -0.25 µs [-0.60, +0.00] | -0.54 µs [-0.93, -0.18] | +0.66 % [-0.04, +1.78] | no cost resolved |
| bulk INSERT, 20 rows ascending | -0.55 µs [-0.90, +0.10] | -0.86 µs [-1.72, -0.12] | +0.33 % [-0.23, +0.85] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +17.80 µs [+10.60, +22.40] | +21.22 µs [+13.96, +27.91] | -2.77 % [-3.71, -2.09] | **B slower** (+2.2 % at p50) |
| SHOW META (control) | -0.10 µs [-1.50, +0.20] | -0.21 µs [-0.77, +0.41] | +0.68 % [-0.37, +2.73] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.15 µs, sd 0.23 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.20 µs.

## 2. The repro of the 711 file's section 5: gone

Same scripts (`archive/.../drivers/repro_reinsert*.py`), same binary-independent steps,
run against B' (`repro1.txt`, `repro2.txt`, `repro3.txt`).

| step | B (`04649133`, 711 file) | B' (`4015736d`) |
|---|---|---|
| 1,000 rows, purge all of keys 167..332 (a whole non-first leaf), `INSERT` 167 | `ERR separator key 167 is already present in this internal node` | `INSERTED id=167 page=132 slot=0` |
| `INSERT` 168 / 332 / 250 | placed (a different page) | placed, same page |
| purge every key of a 1,000-row relation, re-insert singly ascending | 995 placed, 5 refused (167, 333, 499, 665, 831) | **1,000 of 1,000 placed** |
| purge every key, re-insert as one 500-row `INSERT` | refused at row 167; a following `INSERT` of key 1 refused | `INSERTED rows=500 first_id=1 last_id=500` |
| purge-throughput verification (re-insert every key singly) | 1 refusal per 166-key leaf in every multi-leaf cell | **0 refusals of any kind in every cell** (`sep_refused=0 other_refused=0`, every run, both core counts, `strict`) |

The one remaining `ERR` in `repro3.txt`, `duplicate primary key 167 already present`,
is the correct answer to naming a live key a second time. In `repro1.txt` the
last three `ERR duplicate` lines are likewise correct (keys 1..500 had just been placed).

## 3. Purge throughput at B'

Same cells as the 711 file's section 3 (`relaxed`, 5 runs; `strict`, 3 runs). Medians;
`PURGE per key` is the reciprocal, derived.

**durability = relaxed**
| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 18,023 keys/s [17,799, 18,111] | 55.48 µs | 41.2 µs / 50.0 µs / 54.9 µs / 65.2 µs / 72.3 µs | 19,875 keys/s | 48.5 µs |
| 1 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,660 keys/s [17,395, 17,824] | 56.62 µs | 40.8 µs / 52.3 µs / 56.0 µs / 64.5 µs / 72.6 µs | 20,284 keys/s | 48.0 µs |
| 1 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,219 keys/s [17,089, 17,414] | 58.08 µs | 40.8 µs / 52.2 µs / 56.4 µs / 65.9 µs / 75.1 µs | 19,654 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 166,683 keys/s [165,220, 167,568] | 6.00 µs | 1,048.2 µs / 1,126.6 µs / 1,201.1 µs / 1,326.1 µs / 1,347.8 µs | 110,724 keys/s | 1,805.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 171,708 keys/s [152,162, 172,976] | 5.82 µs | 5.73 ms / 5.79 ms / 5.82 ms / 5.88 ms / 5.91 ms | 466,268 keys/s | 2.08 ms |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 175,112 keys/s [174,912, 175,216] | 5.71 µs | 57.00 ms / 57.00 ms / 57.10 ms / 57.22 ms / 57.22 ms | 1,481,561 keys/s | 6.71 ms |
| 2 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 17,719 keys/s [16,963, 17,892] | 56.44 µs | 45.6 µs / 50.2 µs / 55.7 µs / 66.1 µs / 74.5 µs | 19,835 keys/s | 48.8 µs |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,456 keys/s [17,168, 17,560] | 57.29 µs | 41.7 µs / 52.5 µs / 56.8 µs / 66.4 µs / 77.2 µs | 20,117 keys/s | 48.3 µs |
| 2 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,114 keys/s [16,779, 17,147] | 58.43 µs | 40.8 µs / 52.4 µs / 56.7 µs / 67.2 µs / 78.1 µs | 19,585 keys/s | 48.1 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 144,744 keys/s [144,038, 160,340] | 6.91 µs | 1,207.1 µs / 1,302.2 µs / 1,377.0 µs / 1,524.5 µs / 1,554.0 µs | 95,618 keys/s | 2.08 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 149,083 keys/s [148,974, 169,268] | 6.71 µs | 6.61 ms / 6.69 ms / 6.71 ms / 6.75 ms / 6.79 ms | 390,191 keys/s | 2.54 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 170,341 keys/s [151,187, 172,300] | 5.87 µs | 58.62 ms / 58.62 ms / 58.67 ms / 58.81 ms / 58.81 ms | 1,165,851 keys/s | 8.54 ms |
**durability = strict**
| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 711 keys/s [690, 733] | 1,406.07 µs | 1,032.2 µs / 1,169.5 µs / 1,232.6 µs / 1,906.3 µs / 6.01 ms | 714 keys/s | 1,225.5 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 132,877 keys/s [130,374, 134,361] | 7.53 µs | 7.31 ms / 7.41 ms / 7.52 ms / 7.73 ms / 7.73 ms | 308,981 keys/s | 3.26 ms |
**DELETE commit to PURGE success**

Against the 711 file (B): one key per statement 17,114-18,023 keys/s
(711: 17,216-18,114) and the 1,000-key window 171,708 keys/s at `cores = 1`
(711: 172,196): **unchanged within 1 %**. The `cores = 2` windows are lower here
(144,744 / 149,083 keys/s for 200 / 1,000-key windows against 161,956 / 166,689):
those are the cells the 711 file found bimodal across runs by session placement
(147,300 or 167,600 keys/s), and the two runs' ranges (711 [143,354, 163,078], B' [144,038, 160,340]; 711 [164,568, 169,080], B' [148,974, 169,268]) overlap
(the 200-key window and 1,000-key window ranges overlap), so this is the same placement
effect, not a change. `strict`, one key per statement: 711 keys/s, 1.23 ms at p50
(711 file: 726 keys/s, 1.23 ms). The ~95 % commit wait is as before.

## Not run

- DELETE-to-PURGE latency under readers, the snapshot probe, `group` durability, `strict`
  at `cores = 2`, and the stage bisection were not re-run (see above); the 711 file's
  numbers for them describe `04649133`, not B'.
- Overhead at `strict`/`group`, `cores` above 2, a multi-client write mix, and a
  PostgreSQL floor: not run, as in the 711 file.
- The test suite was not executed here.
- No attribution inside a purge or a split: `perf` is locked out (`perf_event_paranoid` = 4).

