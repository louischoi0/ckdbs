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

@@SUMMARY@@

Waits: every cell is `durability = relaxed`, so there is no commit wait; the
`SHOW META` floor is 45.4-45.7 µs at p50, INSERT/DELETE sit 4 µs above it, UPDATE 6 µs,
SELECT 19-22 µs, a 20-row INSERT 50 µs; 0 errors and no lock or conflict wait in
96 runs.

Full percentile tables (48,000 statements per arm and shape, 16 runs pooled):

### `cores = 1`

@@C1@@

### `cores = 2`

@@C2@@

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

@@PURGE@@

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
