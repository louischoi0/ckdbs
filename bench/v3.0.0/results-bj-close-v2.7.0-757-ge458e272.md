# BJ's close, the literal path re-measured after the remedy (B')

**Thesis.** The remedy of `e458e272` (`v2.7.0-757-ge458e272`, B') took most of the cost back. Against A
(`e5a7e1b7`, the commit BJ opens from plus BA), **a point `UPDATE ... SET v = <literal> WHERE id = k` is now
+0.25 µs [-0.10, +0.80] (+0.5 %) at `cores = 1`, 1,000 rows, +0.65 µs [+0.30, +1.05] (+1.3 %) at 10,000 rows
and +0.70 µs [+0.00, +1.40] (+1.3 %) at `cores = 2`, 1,000 rows, where B (`8e37baf4`) measured +1.60 to +1.90 µs
(+3.0 to +3.7 %) in the same cells.** A direct B-against-B' pair (`cores = 1`, 1,000 rows) says the remedy
recovered **1.35 µs [0.70, 1.65] on the single literal, 1.20 µs on its replicate and 1.50 µs on the
two-literal form**; a second, flipped pair recovered 1.10, 1.30 and 1.30 µs. **What remains is about
0.3-0.7 µs a point statement (0.5-1.3 %) and 0.65-0.95 µs for the two-assignment form (1.2-1.8 %)**: the
single-literal interval still touches zero in two of the three cells, the two-literal form's does not. **The every-row literal `UPDATE` was not helped**, as the remedy is per statement: +1.8 %, +2.0 % and
+0.4 % at p50 (+17, +19 and +6 ns per row; the first two have intervals above zero, the third does not), where B
read +0.9, +1.0 and +1.9 %; the B-against-B' pairs put the difference at +5 ns per row, unresolved. `INSERT`,
`DELETE`, `SELECT` and `SHOW META` do not move. **On B' the expression's own price over a literal reads
higher, +2.90 µs for `SET v = v + 1`, because the literal got cheaper, not the expression dearer**: its
absolute p50 is 55.3 µs, B's 55.1-55.4 µs.

- **A** = `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`; **the same binary as the first file**, `d2a6977f...223e`
  (hash below). Not rebuilt.
- **B'** = `e458e272`, `v2.7.0-757-ge458e272`, committed 2026-10-10 18:52:34 UTC. Against B (`8e37baf4`) it
  changes 4 files: `include/kds/parser/parser.hpp`, `src/parser/parser.cpp` (`Parser::ParseSetValue` reads a
  leading literal ahead of the expression chain), `src/server/command_dispatcher.cpp` (`UpdateInner` builds its
  `std::function` subquery resolver only when some assignment is an expression) and
  `tests/parser_set_expression_test.cpp`.
- **B** = `8e37baf4`, `v2.7.0-756-g8e37baf4`, measured in
  `bench/v3.0.0/results-bj-close-v2.7.0-756-g8e37baf4.md` ("the first file"), which this file does not edit;
  its numbers are cited as the comparator, from the same driver, binaries and method, an hour or more
  earlier on a busier host.
- Raw output: `archive/bj-close-v2.7.0-757-ge458e272/`.

| Item | Value |
|---|---|
| Executed | 2026-10-10: B' built 18:53-18:54 UTC; matrix 18:55-19:48 (first pass 18:55-19:27, re-runs to 19:48); B-against-B' pair 19:48-19:50; flipped pair 19:54-19:57 |
| Branch / worktree | `worktree-bj-expression-update`, worktree `bj-expression-update` (read-only; only `bench/v3.0.0/` files were added). B' built from a fresh detached checkout `bj-measure-b2`, removed afterwards; nothing edited in it |
| B' | `e458e272`, `v2.7.0-757-ge458e272`. The checkout is a fresh `git worktree add --detach` of that commit, so clean by construction; `git status` could not be run inside it from this session. Release (`CMAKE_BUILD_TYPE` read back from its `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`; binary mtime 2026-10-10 18:54:13 UTC, **newer than its commit** (18:52:34 UTC) |
| Measured copies (`archive/.../binaries.txt`) | A `d2a6977f220a32147b6a2df985aec10b60ee7411fe3fca3381b78e46e525223e` (the first file's A, same hash), B' `164fa531cbb43273feae3aff544fe5625a4231263fae4fd830e7f9d850baa6b7`, B `69733b1bb373ca6ef034ed4d391685c8bd908cfc86f861909c78d983c6bfe99d` (the first file's B, same hash; used by the two pairs). Every server ran from a copy under `/home/cdkbs/bench-runs/bj-close/bin/` |
| Host / device | as the first file: 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure; server on CPU 2 (`cores = 1`) or 2,3 (`cores = 2`), driver on CPU 4; data files and WAL under `/home/cdkbs/bench-runs/bj-close/b2/` on `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Server config | `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 or 2, **`durability = relaxed`**; fresh server and data file per (cell, run); ports 31911 (A) and 31912 (B') |
| Host load | **The host was busy.** Loadavg (1 min) was 10-12 at the start; other sessions ran `ctest -j8`/`-j4` suites and builds throughout the first hour. Per run `/proc/loadavg`, the `pgrep -a -f "cc1plus\|cmake --build\|ctest"` list and a per-CPU busy fraction are in the run's JSON. **18 of the first 48 runs were discarded and re-run** (below); the kept runs have loadavg 1.3-10.3 and other-CPU busy 2.1-12.6 % |
| Suite | **Not executed in this session.** This measurement changes no code |

## What was run, and what was not

Run (all with the first file's `bj_close_ab.py`, unmodified, and its method: fresh server and data file per
(cell, run), A and B' interleaved in 12 blocks of 250 statements per point arm and 10 per every-row arm,
`WHERE id >= 1` writes all N rows, 3,000 point and 120 every-row statements per arm and run; bootstrap 95 % CI over
the 16 runs):

- **A against B', Part 1**, 16 runs per cell, in three cells: `cores = 1` at 1,000 and 10,000 rows, `cores = 2`
  at 1,000 rows: the point `SET v = <lit> WHERE id = k`, its replicate on a second relation, the two-assignment
  literal form, the every-row literal `UPDATE` and its replicate, and the controls (`INSERT`, `SELECT` by pk,
  `DELETE` by pk, `SHOW META`). 48 runs kept.
- **B' only, Part 2 and 3** (the driver runs them in every cell; reported for `cores = 1`, 1,000 rows as asked,
  the other two cells are in the appendix): `SET v = v + 1`, the swap, the `CASE` and the subquery against
  `SET v = <lit>`; the `SELECT`-then-`UPDATE` round trip.
- **B against B'**, `cores = 1`, 1,000 rows: a 12-run Part 1 pair, and a **flipped** 16-run pair (B' in the A slot,
  B in the B slot of the driver), whose Part 2 and 3 then ran on B in the same hour as B''s.

Not run: the rows and cells the coordinator did not ask for (200 rows; `cores = 2` at 10,000 rows), the suite,
`strict`/`group`, the rest of the first file's "Not run" list. Every reply and every updating arm's end state is
checked as in the first file: 0 errors, 0 mismatches and `verify=ok` in every kept run.

**Discard rules** (the first file's, plus one): a run is discarded and re-run if a real `cmake --build`, `ctest` or
`cc1plus` is in its before or after list, if the CPUs outside {server, driver} averaged over 30 % busy, **or (new
here) if the `SHOW META` control's p50 exceeded 53 µs in either arm** (49.3-51.2 µs in every quiet run of both
files), because several runs this time passed the first two tests and still read 56-78 µs on the control. Of the 18
discarded: 7 on a build/ctest in the list alone, 4 on that and the control, 6 on all three, 1 on the control alone
(their summaries are in `archive/.../discarded/`). The re-runs were gated at 6 % other-CPU busy for up to 600 s; pass 2
found nothing more to discard. The strict subset (other-CPU busy at or under 10 %: 16, 16 and 14 runs) gives the
same point-statement deltas to within 0.25 µs.

## 1. What the remedy recovered

Throughput at p50 (derived, `1,000,000 / p50`), A → B', median per-run change and 95 % CI. Statements per second:

| shape | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `UPDATE SET v = <lit> WHERE id = k` | 19,455 → 19,324 /s (-0.5 % [-1.5, +0.2]) | 19,305 → 19,029 /s (-1.2 % [-2.1, -0.6]) | 18,904 → 18,657 /s (-1.3 % [-2.6, +0.0]) |
| same shape, second relation (replicate) | 19,380 → 19,249 /s (-0.8 % [-1.3, +0.0]) | 19,286 → 19,011 /s (-1.2 % [-1.7, -0.6]) | 18,993 → 18,692 /s (-1.4 % [-2.9, -0.4]) |
| `UPDATE SET v = <lit>, w = <lit> WHERE id = k` | 19,139 → 18,904 /s (-1.2 % [-2.0, -0.8]) | 19,084 → 18,692 /s (-1.8 % [-2.4, -1.3]) | 18,657 → 18,382 /s (-1.7 % [-2.8, -0.6]) |
| `UPDATE SET v = <lit> WHERE id >= 1` (every row) | 1,040 → 1,020 /s (-1.7 % [-2.9, -1.5]) | 103 → 101 /s (-1.9 % [-4.7, -1.3]) | 769 → 764 /s (-0.4 % [-1.3, +0.2]) |
| same shape, second relation (replicate) | 1,056 → 1,025 /s (-3.0 % [-3.7, -2.5]) | 103 → 101 /s (-2.2 % [-4.8, -1.5]) | 764 → 751 /s (-2.0 % [-2.7, -1.2]) |
| `INSERT`, pk named (not touched by BJ) | 20,325 → 20,243 /s (-0.4 % [-0.6, -0.2]) | 20,202 → 20,161 /s (-0.2 % [-0.4, +0.0]) | 20,060 → 19,980 /s (-0.7 % [-1.4, +0.0]) |
| `SELECT * WHERE id = k` (control) | 14,771 → 14,937 /s (+0.6 % [-0.1, +1.3]) | 15,326 → 15,432 /s (+0.2 % [-0.2, +1.4]) | 14,641 → 14,695 /s (+0.1 % [-0.6, +0.7]) |
| `DELETE WHERE id = k` (not touched by BJ) | 20,450 → 20,492 /s (-0.1 % [-0.4, +0.4]) | 20,305 → 20,284 /s (-0.2 % [-0.4, +0.0]) | 20,202 → 20,284 /s (+0.4 % [-0.2, +0.9]) |
| `SHOW META` (control) | 19,920 → 19,960 /s (+0.1 % [-0.2, +0.5]) | 19,861 → 19,900 /s (+0.1 % [-0.5, +0.6]) | 19,802 → 19,802 /s (-0.1 % [-0.6, +0.2]) |

The same data as p50 latency: A's p50, then B' - A with the 95 % CI and the relative change:

| shape | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| `UPDATE SET v = <lit> WHERE id = k` | 51.4 µs: +0.25 µs [-0.10, +0.80] (+0.5 %) | 51.8 µs: +0.65 µs [+0.30, +1.05] (+1.3 %) | 52.9 µs: +0.70 µs [+0.00, +1.40] (+1.3 %) |
| same shape, second relation (replicate) | 51.6 µs: +0.40 µs [+0.00, +0.70] (+0.8 %) | 51.8 µs: +0.65 µs [+0.30, +0.90] (+1.3 %) | 52.7 µs: +0.75 µs [+0.20, +1.60] (+1.4 %) |
| `UPDATE SET v = <lit>, w = <lit> WHERE id = k` | 52.2 µs: +0.65 µs [+0.40, +1.05] (+1.2 %) | 52.4 µs: +0.95 µs [+0.70, +1.35] (+1.8 %) | 53.6 µs: +0.90 µs [+0.30, +1.50] (+1.7 %) |
| `UPDATE SET v = <lit> WHERE id >= 1` (every row) | 961 µs: +16.9 µs [+14.3, +28.8] (+1.8 %) | 9,699 µs: +192.1 µs [+126.4, +480.1] (+2.0 %) | 1,300 µs: +5.7 µs [-2.2, +21.5] (+0.4 %) |
| same shape, second relation (replicate) | 947 µs: +29.4 µs [+24.1, +36.6] (+3.1 %) | 9,684 µs: +216.2 µs [+149.8, +482.0] (+2.2 %) | 1,309 µs: +27.2 µs [+16.1, +31.9] (+2.1 %) |
| `INSERT`, pk named (not touched by BJ) | 49.2 µs: +0.20 µs [+0.10, +0.30] (+0.4 %) | 49.5 µs: +0.10 µs [+0.00, +0.20] (+0.2 %) | 49.8 µs: +0.35 µs [+0.00, +0.70] (+0.7 %) |
| `SELECT * WHERE id = k` (control) | 67.7 µs: -0.40 µs [-0.90, +0.10] (-0.6 %) | 65.2 µs: -0.15 µs [-0.90, +0.20] (-0.2 %) | 68.3 µs: -0.05 µs [-0.50, +0.40] (-0.1 %) |
| `DELETE WHERE id = k` (not touched by BJ) | 48.9 µs: +0.05 µs [-0.20, +0.25] (+0.1 %) | 49.2 µs: +0.10 µs [+0.00, +0.20] (+0.2 %) | 49.5 µs: -0.20 µs [-0.40, +0.10] (-0.4 %) |
| `SHOW META` (control) | 50.2 µs: -0.05 µs [-0.20, +0.10] (-0.1 %) | 50.3 µs: -0.05 µs [-0.30, +0.20] (-0.1 %) | 50.5 µs: +0.05 µs [-0.10, +0.20] (+0.1 %) |

The first file's B - A for the same cells, for reading side by side (cited, not re-measured here; the first file's
tables carry the intervals):

| shape | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 1,000 rows |
|---|---|---|---|
| point `SET v = <lit>` | B: +1.60 µs [+1.40, +2.20] | B: +1.90 µs [+1.70, +2.30] | B: +1.70 µs [+1.30, +2.40] |
| its replicate | B: +1.55 µs [+1.40, +2.10] | B: +1.80 µs [+1.50, +2.40] | B: +1.90 µs [+1.40, +2.50] |
| `SET v = <lit>, w = <lit>` | B: +2.15 µs [+1.70, +2.60] | B: +2.00 µs [+1.90, +2.40] | B: +2.10 µs [+1.50, +2.50] |
| every row | B: +8.6 µs [+0.4, +11.6] (+0.9 %) | B: +96.6 µs [+54.4, +151.9] (+1.0 %) | B: +24.6 µs [+15.5, +29.2] (+1.9 %) |
| every row, replicate | B: +16.8 µs [+6.0, +23.7] (+1.8 %) | B: +134.0 µs [+50.1, +165.8] (+1.4 %) | B: +22.3 µs [+9.8, +44.8] (+1.7 %) |

**Reading.** The three point `UPDATE` shapes are 1.0-1.5 µs cheaper than in the first file in every cell, which
is what the remedy was for. What is left, in µs at p50 (B' - A): single literal +0.25 / +0.65 / +0.70,
replicate +0.40 / +0.65 / +0.75, two literals +0.65 / +0.95 / +0.90. **Resolved against the floors of 0.6-1.1 %**: the
two-literal form in all three cells, the replicate in the 10,000-row and `cores = 2` cells and the single literal
at 10,000 rows (+1.3 %); **not resolved**: the single literal at 1,000 rows (both core counts), whose
intervals touch zero. The remainder is therefore real but small and moves with the cell (+0.25 to +0.70 µs for one
assignment); the two-assignment form pays about +0.3 µs more than one, as before. **The every-row literal
`UPDATE` is the same size as in the first file or slightly larger** (+1.8 %, +2.0 %, +0.4 %; its replicate +3.1,
+2.2, +2.1 %): that is a per-row cost (about +17 ns per row at `cores = 1`, +6 at `cores = 2`) that a per-statement fix does
not touch. Controls: `SHOW META` -0.1 to +0.1 %, `SELECT` -0.6 to -0.1 %, `DELETE` -0.4 to +0.2 %, `INSERT` +0.2 to
+0.7 % (+0.1 to +0.35 µs; the +0.20 µs [+0.10, +0.30] at `cores = 1`, 1,000 rows is the one control interval
above zero, 0.4 %).

### 1.2 The same commit pair measured directly

B and B' against each other (right minus left, so B' - B), `cores = 1`, 1,000 rows, Part 1 only, 12 runs, the
first of the two pairs:

| shape | left p50 | right - left | relative |
|---|---|---|---|
| lit-point | 53.2 µs | -1.35 µs [-1.65, -0.70] | -2.53 % [-3.10, -1.32] |
| lit-point-again | 53.3 µs | -1.20 µs [-1.70, -0.80] | -2.25 % [-3.19, -1.50] |
| lit2-point | 54.7 µs | -1.50 µs [-2.25, -1.05] | -2.76 % [-4.12, -1.92] |
| lit-range | 969.4 µs | +5.55 µs [-1.85, +11.45] | +0.58 % [-0.19, +1.20] |
| insert | 49.4 µs | +0.20 µs [+0.05, +0.30] | +0.40 % [+0.10, +0.61] |
| select-pk | 67.5 µs | -0.25 µs [-0.75, +0.45] | -0.37 % [-1.10, +0.68] |
| delete-pk | 49.2 µs | +0.05 µs [-0.10, +0.15] | +0.10 % [-0.20, +0.30] |
| ping | 50.8 µs | -0.15 µs [-0.50, +0.30] | -0.30 % [-0.98, +0.60] |

And the **flipped** pair (16 runs, full driver; the table is B - B', i.e. what the remedy recovered, positive =
B slower):

| shape | B - B', p50, 95 % CI | relative |
|---|---|---|
| point `SET v = <lit>` | +1.10 µs [+0.70, +1.60] | +2.1 % |
| its replicate | +1.30 µs [+0.80, +1.60] | +2.5 % |
| `SET v = <lit>, w = <lit>` | +1.30 µs [+1.10, +1.80] | +2.5 % |
| every row | -4.65 µs [-11.60, +0.00] | -0.5 % (B faster, unresolved) |
| every row, replicate | +4.40 µs [-0.20, +9.70] | +0.5 % (unresolved) |
| `INSERT` / `SELECT` / `DELETE` / `SHOW META` | +0.00 / +0.20 / +0.05 / +0.00 µs, all intervals containing 0 or at the edge | none |

**Both pairs agree the remedy is worth 1.1-1.5 µs (2-3 %) on a point `UPDATE` and nothing resolvable on an every-row
one.** The pairs and the A/B matrix agree on what is left: the first file's B - A of +1.60 µs at `cores = 1`, 1,000
rows minus the pairs' 1.35 and 1.10 µs gives +0.25 and +0.50 µs, against the directly measured B' - A of +0.25 µs
[-0.10, +0.80].

## 2. The expression's own price, restated on B'

`cores = 1`, 1,000 rows, B' only (the driver's Part 2; the other two cells are in the appendix). Statements per second at
p50 (derived), point (`WHERE id = k`), then the prices over B''s own `SET v = <lit>` in the same run:

| SET value | statements/s at p50 | p50 | extra per statement over the literal (95 % CI) |
|---|---|---|---|
| `SET v = <lit>` | 19,120 /s | 52.3 µs | - |
| `SET v = v + 1` | 18,083 /s | 55.3 µs | +2.90 µs [+2.75, +3.10] |
| `SET v = w, w = v` | 18,282 /s | 54.7 µs | +2.40 µs [+2.25, +2.60] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 16,287 /s | 61.4 µs | +9.15 µs [+8.70, +9.35] |

Every row (`WHERE id >= 1`, 1,000 rows written per statement), statements per second at p50 and the per-row price:

| SET value | p50 per statement | p50 per row | extra per row over the literal (95 % CI) | statements/s at p50 |
|---|---|---|---|---|
| `SET v = <lit>` | 966 µs | 966 ns | - | 1,035 /s |
| `SET v = v + 1` | 1,093 µs | 1,093 ns | +124 ns [+119, +132] | 915 /s |
| `SET v = w, w = v` | 1,054 µs | 1,054 ns | +90 ns [+84, +93] | 949 /s |
| `SET v = CASE ...` | 1,266 µs | 1,266 ns | +295 ns [+291, +305] | 790 /s |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,113 µs | 1,113 ns | +145 ns [+144, +149] | 898 /s |

The price over a literal went **up** against the first file's (+1.80 / +1.40 / +7.70 µs a statement), and that is
the remedy showing from the other side: the literal got 1.1-1.5 µs cheaper and the expression paths did not.
Same-hour comparison of the expression arms on B (8e37baf4, the flipped runs) and B' (two sets of 16 runs, difference of
the medians with a bootstrap CI over both sets):

| B-only arm | 8e37baf4 p50 (flipped runs) | B' p50 | B' - 8e37baf4 |
|---|---|---|---|
| xp-lit | 53.4 µs | 52.0 µs | -1.35 µs [-1.85, -0.60] |
| xp-expr | 55.4 µs | 55.1 µs | -0.25 µs [-0.60, +0.10] |
| xp-swap | 55.1 µs | 54.6 µs | -0.50 µs [-0.80, -0.05] |
| xp-case | 61.5 µs | 61.2 µs | -0.30 µs [-0.65, +0.15] |
| xr-lit (per row) | 953 ns | 964 ns | +10 ns [-1, +20] |
| xr-expr (per row) | 1065 ns | 1087 ns | +22 ns [+12, +31] |
| xr-swap (per row) | 1019 ns | 1050 ns | +31 ns [+20, +41] |
| xr-case (per row) | 1229 ns | 1260 ns | +31 ns [+21, +46] |
| xr-subq (per row) | 1095 ns | 1112 ns | +17 ns [+5, +28] |
| rt-one | 55.2 µs | 55.0 µs | -0.25 µs [-0.60, +0.20] |
| rt-pair | 124.7 µs | 123.7 µs | -1.00 µs [-2.05, -0.65] |

**Point statements with an expression are unchanged** (-0.25 to -0.50 µs: `v + 1` and the `CASE` inside zero, the swap
0.5 µs faster at the edge of its interval; the single literal 1.35 µs faster, as the pairs say). **Every-row
statements are 10-31 ns per row slower on B'** (1-3 %): the literal's +10 is unresolved, the four expression shapes'
+17 to +31 resolved. That is not what a parser fast path should do; the other change, `UpdateInner` building the
resolver only when some assignment is an expression, is on the row path's setup and could be involved, but the
comparison is between two sets of runs, not a same-run pair, and no stage build separates the two changes, so it is
stated as measured and not attributed.

The round trip (B', `cores = 1`, 1,000 rows): `SELECT v` then `UPDATE SET v = <v+1>` 123.6 µs, one `UPDATE SET v = v + 1`
55.1 µs, `BEGIN; SELECT; UPDATE; COMMIT` 188.5 µs at p50: the pair costs **+68.5 µs [+68.2, +68.7]** more than the one
statement, as in the first file (+68.8 to +71.8 µs).

## 3. What remains of the cost

| piece | before (B, first file) | after (B') | what is left |
|---|---|---|---|
| point `SET v = <lit>`, per statement | +1.60 to +1.90 µs (+3.0 to +3.7 %) | +0.25 to +0.70 µs (+0.5 to +1.3 %) | 16-41 % of it; 0 not excluded at 1,000 rows |
| `SET v = <lit>, w = <lit>` | +2.00 to +2.15 µs | +0.65 to +0.95 µs (+1.2 to +1.8 %) | 30-48 %, resolved |
| every-row literal `UPDATE`, per row | +9 to +10 ns at `cores = 1` (unresolved) | +17 to +19 ns at `cores = 1`, +6 at `cores = 2` | not touched |
| `SET v = v + 1`, per statement (absolute p50) | 55.1-55.4 µs | 55.3 µs | unchanged |
| `INSERT`, `DELETE`, `SELECT`, `SHOW META` | flat | flat | nothing |

The first file's stage attribution put 0.65-0.95 µs of the 1.6 before any evaluator exists (parse and type) and
about 0.7 with BJ-S4's evaluator and write loop. The remedy's two changes recovered 1.1-1.5 µs, so it reached
into both halves: the leading-literal path avoids the expression chain at parse (and the `Expr` it allocates), and
the resolver change avoids a heap allocation in `UpdateInner`. **No attribution between the two changes was made**: no
stage build separates them and `perf` is locked out (`perf_event_paranoid` = 4). What is left on a point statement is
0.25-0.7 µs and the per-row +17-19 ns at `cores = 1` (the first file's +9-10 ns, unresolved there, now larger and
resolved); neither is attributed.

## 4. What could not be resolved

- **The single literal at 1,000 rows** (+0.25 [-0.10, +0.80] µs at `cores = 1`, +0.70 [0.00, +1.40] at `cores = 2`): the interval
  includes or touches zero; its replicate is +0.40 [0.00, 0.70] and +0.75 [0.20, 1.60], so 0.25-0.75 µs is the
  honest range and 16 runs do not narrow it.
- **The every-row literal `UPDATE`** (+1.8 %, +2.0 %, +0.4 %) and **its replicate (+3.1, +2.2, +2.1 %)**: the
  replicate reads dearer than the first arm in all three cells, as in the first file (a systematic +0.4 to +1.8 %
  for the second relation), so the floors are 3.0 % (1,000 rows) and 1.1 % (10,000 rows); at 10,000 rows
  `cores = 1` it is resolved (+192 µs [+126, +480], floor 1.1 %); at 1,000 rows the first arm (+1.8 %) is inside the floor and
  the replicate (+3.1 %) is at it.
- **Every-row expression statements, 10-31 ns per row slower than B**: not attributed (section 2).
- **The same stalls as the first file**: a 0.2-0.45 s carve stall lands in one arm per server per run, moving
  that arm's mean and the driver's `ops / elapsed` (INSERT at 1,000 rows reads -67 % and -31 % in the driver's
  throughput columns of the appendix, with a p50 delta of +0.20 and +0.35 µs), and the 10,000-row every-row arms carry
  a 55-800 ms tail; the matrices use p50. Not investigated further here.
- **A noisy host.** 18 of 48 first-pass runs were discarded; the control's p50 reads 50.2-50.5 µs in all three kept
  cells, so the round-trip floor did not move, but a neighbour on the shared L3 is not excluded.
- **Cross-file comparison.** The first file's B numbers and this file's B' numbers were taken an hour or more apart
  against the same A binary; the direct B-against-B' pairs exist to cover exactly that.

## Not run

- **The suite.** Suite: not executed in this session.
- 200 rows, `cores = 2` at 10,000 rows, `strict`/`group`, more than two cores, more than one client, a PostgreSQL
  floor, other types and column kinds: the first file's list stands.
- A stage build separating the parser change from the `UpdateInner` change; `perf`.
- A re-measure of the expression arms in a same-run B/B' pair (only the cross-set comparison of section 2 and the
  flipped pair's Part 2 exist).

## Baseline for the next run

The next measurement of this shape reads against **`v2.7.0-757-ge458e272` (this file)**: point
`SET v = <lit> WHERE id = k` at `cores = 1`, 1,000 rows, B' p50 52.3 µs (the A/B matrix: A 51.4 µs, B' +0.25 µs); every-row
at 1,000 rows 966 µs; `SET v = v + 1` 55.3 µs and 1,093 ns per row.

## Appendix A. Every cell

Each cell: the runs used, host state, Part 1 (A and B' percentiles, deltas, noise floor), Part 2 and Part 3 (B'
only). **In these tables "B" is B' (`e458e272`)**; the driver's labels are A and B. The driver's `throughput (median of
runs)` is `ops / elapsed` and carries the stalls above.

### cores = 1, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.98-10.32, after 1.98-8.88; other-CPU busy over the run 2.1%-7.8% (median 2.4%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 39.9 µs | 50.8 µs | 51.5 µs | 66.9 µs | 83.9 µs | 54.1 µs (sd 1.5) | 18,264 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 42.6 µs | 51.2 µs | 52.0 µs | 66.9 µs | 81.1 µs | 54.3 µs (sd 0.7) | 18,090 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 39.9 µs | 50.9 µs | 51.6 µs | 67.3 µs | 81.9 µs | 55.1 µs (sd 1.6) | 17,873 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.2 µs | 51.2 µs | 52.1 µs | 67.4 µs | 81.4 µs | 55.4 µs (sd 1.2) | 17,700 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 42.9 µs | 51.5 µs | 52.3 µs | 67.9 µs | 83.7 µs | 55.5 µs (sd 1.1) | 17,546 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 43.8 µs | 52.2 µs | 53.1 µs | 68.2 µs | 82.9 µs | 56.5 µs (sd 1.1) | 17,410 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 905.2 µs | 942.5 µs | 966.3 µs | 1,110 µs | 1,302 µs | 991.4 µs (sd 18.6) | 1,011 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 921.5 µs | 966.2 µs | 983.2 µs | 1,139 µs | 1,412 µs | 1,014 µs (sd 16.7) | 988 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 900.1 µs | 935.0 µs | 951.1 µs | 1,081 µs | 1,642 µs | 985.1 µs (sd 35.6) | 1,019 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 910.7 µs | 958.4 µs | 980.5 µs | 2,363 µs | 3,514 µs | 1,255 µs (sd 286.0) | 848 stmt/s |
| INSERT, pk named | A | 48,000 | 35.0 µs | 48.5 µs | 49.2 µs | 64.2 µs | 78.9 µs | 104.5 µs (sd 71.5) | 18,103 stmt/s |
| INSERT, pk named | B | 48,000 | 40.0 µs | 48.6 µs | 49.4 µs | 63.8 µs | 77.7 µs | 146.4 µs (sd 60.8) | 6,078 stmt/s |
| SELECT by pk (control) | A | 48,000 | 59.3 µs | 65.1 µs | 67.6 µs | 88.4 µs | 109.0 µs | 71.4 µs (sd 1.1) | 13,869 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.0 µs | 64.7 µs | 67.3 µs | 87.7 µs | 106.3 µs | 70.7 µs (sd 1.2) | 14,032 stmt/s |
| DELETE by pk | A | 48,000 | 39.3 µs | 48.2 µs | 48.9 µs | 62.8 µs | 73.4 µs | 51.1 µs (sd 0.7) | 19,381 stmt/s |
| DELETE by pk | B | 48,000 | 39.8 µs | 48.2 µs | 48.9 µs | 62.9 µs | 74.4 µs | 51.6 µs (sd 1.2) | 19,261 stmt/s |
| SHOW META (control) | A | 48,000 | 42.4 µs | 49.7 µs | 50.3 µs | 62.1 µs | 70.6 µs | 52.1 µs (sd 0.3) | 19,018 stmt/s |
| SHOW META (control) | B | 48,000 | 43.2 µs | 49.6 µs | 50.2 µs | 62.1 µs | 70.5 µs | 52.1 µs (sd 0.3) | 19,032 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.25 µs [-0.10 µs, +0.80 µs] | +0.48 % [-0.19, +1.56] | +0.16 µs [-0.76 µs, +0.86 µs] | -0.28 % [-1.85, +0.57] | no change resolved |
| UPDATE SET v = lit, by pk (replicate) | +0.40 µs [+0.00 µs, +0.70 µs] | +0.78 % [+0.00, +1.36] | +0.34 µs [-0.53 µs, +1.24 µs] | -0.08 % [-2.39, +0.88] | no change resolved |
| UPDATE SET v = lit, w = lit, by pk | +0.65 µs [+0.45 µs, +1.05 µs] | +1.26 % [+0.95, +2.02] | +1.04 µs [+0.14 µs, +2.04 µs] | -0.76 % [-3.45, +0.09] | **B slower** (+1.3 % at p50, floor 0.6 %) |
| UPDATE SET v = lit, every row | +16.90 µs [+14.30 µs, +28.75 µs] | +1.77 % [+1.51, +3.02] | +23.08 µs [+15.76 µs, +30.67 µs] | -1.94 % [-3.37, -1.40] | CI excludes 0, inside the floor (3.0 %): not resolved |
| UPDATE SET v = lit, every row (replicate) | +29.45 µs [+24.10 µs, +36.60 µs] | +3.13 % [+2.56, +3.85] | +269.69 µs [+189.64 µs, +415.80 µs] | -17.22 % [-17.97, -16.13] | **B slower** (+3.1 % at p50, floor 3.0 %) |
| INSERT, pk named | +0.20 µs [+0.10 µs, +0.30 µs] | +0.40 % [+0.20, +0.61] | +41.86 µs [-20.74 µs, +100.44 µs] | -67.11 % [-72.75, +127.81] | CI excludes 0, inside the floor (0.6 %): not resolved |
| SELECT by pk (control) | -0.40 µs [-0.90 µs, +0.10 µs] | -0.60 % [-1.33, +0.15] | -0.72 µs [-1.44 µs, +0.01 µs] | +0.94 % [-0.08, +2.05] | no change resolved |
| DELETE by pk | +0.05 µs [-0.20 µs, +0.20 µs] | +0.10 % [-0.41, +0.41] | +0.49 µs [-0.11 µs, +1.12 µs] | -0.69 % [-2.16, +0.92] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.20 µs, +0.10 µs] | -0.10 % [-0.40, +0.20] | +0.04 µs [-0.18 µs, +0.28 µs] | +0.13 % [-0.59, +0.71] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.40, +0.20]; replicate (second relation, same binary, per run, both arms pooled) point median +0.00 %, robust sd 0.29 % (plain sd 0.34 %); range median -0.74 %, robust sd 1.13 % (plain sd 1.30 %). Floor used: point 0.58 %, range 3.00 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 40.8 µs | 51.5 µs | 52.3 µs | 66.9 µs | 77.5 µs | 56.0 µs (sd 1.7) | 17,502 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 45.8 µs | 54.6 µs | 55.3 µs | 70.2 µs | 82.5 µs | 57.8 µs (sd 0.8) | 16,934 stmt/s | +2.90 µs [+2.75 µs, +3.10 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 43.5 µs | 54.1 µs | 54.7 µs | 68.8 µs | 78.6 µs | 61.4 µs (sd 8.8) | 16,855 stmt/s | +2.40 µs [+2.25 µs, +2.60 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 50.8 µs | 60.6 µs | 61.4 µs | 77.3 µs | 89.0 µs | 64.5 µs (sd 1.0) | 15,161 stmt/s | +9.15 µs [+8.70 µs, +9.35 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 909.2 µs | 947.5 µs | 966.5 µs | 1,797 µs | 3,252 µs | 1,503 µs (sd 1146.4) | 921 stmt/s = 921,050 rows/s | - | 966 ns | - |
| `SET v = v + 1` | 1,920 | 1,039 µs | 1,070 µs | 1,093 µs | 1,469 µs | 79.46 ms | 3,792 µs (sd 1359.9) | 250 stmt/s = 250,400 rows/s | +123.65 µs [+118.70 µs, +131.90 µs] | 1093 ns | +124 ns [+119, +132] |
| `SET v = w, w = v` | 1,920 | 1,003 µs | 1,036 µs | 1,054 µs | 1,286 µs | 6,718 µs | 1,333 µs (sd 311.5) | 839 stmt/s = 838,700 rows/s | +89.70 µs [+84.35 µs, +92.80 µs] | 1054 ns | +90 ns [+84, +93] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,212 µs | 1,241 µs | 1,266 µs | 1,872 µs | 4,219 µs | 1,601 µs (sd 869.0) | 721 stmt/s = 720,900 rows/s | +295.00 µs [+290.90 µs, +305.30 µs] | 1266 ns | +295 ns [+291, +305] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,062 µs | 1,092 µs | 1,113 µs | 1,475 µs | 3,889 µs | 1,312 µs (sd 285.3) | 830 stmt/s = 830,250 rows/s | +145.40 µs [+143.90 µs, +149.30 µs] | 1113 ns | +145 ns [+144, +149] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.6 µs | 54.4 µs | 55.1 µs | 69.9 µs | 80.2 µs | 61.6 µs (sd 17.5) | 17,209 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 114.3 µs | 121.2 µs | 123.6 µs | 149.0 µs | 169.2 µs | 128.3 µs (sd 1.2) | 7,743 /s | +68.50 µs [+68.20 µs, +68.70 µs] |
|   of which the `SELECT v` | 48,000 | 54.8 µs | 63.7 µs | 64.9 µs | 81.9 µs | 95.0 µs | 67.8 µs (sd 0.6) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.6 µs | 56.3 µs | 57.2 µs | 72.2 µs | 83.4 µs | 59.6 µs (sd 0.7) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.6 µs | 181.9 µs | 188.5 µs | 220.5 µs | 250.8 µs | 261.8 µs (sd 21.7) | 3,741 /s | +133.75 µs [+132.85 µs, +134.15 µs] |


### cores = 1, 10,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.54-6.14, after 2.29-4.95; other-CPU busy over the run 2.2%-7.5% (median 3.7%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 40.2 µs | 51.0 µs | 51.8 µs | 67.3 µs | 83.2 µs | 56.2 µs (sd 7.2) | 18,042 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 41.4 µs | 51.7 µs | 52.7 µs | 67.9 µs | 82.8 µs | 65.5 µs (sd 29.6) | 17,674 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 43.6 µs | 51.1 µs | 51.9 µs | 67.1 µs | 81.2 µs | 56.5 µs (sd 8.5) | 18,005 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 41.9 µs | 51.7 µs | 52.7 µs | 67.8 µs | 82.4 µs | 55.3 µs (sd 1.1) | 17,776 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 43.0 µs | 51.7 µs | 52.6 µs | 68.9 µs | 85.2 µs | 56.4 µs (sd 2.3) | 17,612 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 43.1 µs | 52.8 µs | 53.8 µs | 69.8 µs | 85.5 µs | 57.2 µs (sd 2.0) | 17,412 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 9,292 µs | 9,581 µs | 9,719 µs | 18.66 ms | 761.51 ms | 24.43 ms (sd 7190.0) | 49 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 9,512 µs | 9,823 µs | 10.05 ms | 16.97 ms | 389.78 ms | 19.07 ms (sd 3606.4) | 57 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 9,341 µs | 9,562 µs | 9,700 µs | 19.48 ms | 838.75 ms | 41.34 ms (sd 11128.5) | 27 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 9,440 µs | 9,820 µs | 10.03 ms | 16.36 ms | 57.41 ms | 14.16 ms (sd 4823.8) | 87 stmt/s |
| INSERT, pk named | A | 48,000 | 41.6 µs | 48.7 µs | 49.5 µs | 66.4 µs | 83.3 µs | 56.1 µs (sd 9.0) | 18,440 stmt/s |
| INSERT, pk named | B | 48,000 | 39.6 µs | 48.9 µs | 49.7 µs | 68.0 µs | 85.5 µs | 104.0 µs (sd 136.4) | 18,711 stmt/s |
| SELECT by pk (control) | A | 48,000 | 55.1 µs | 64.3 µs | 65.2 µs | 80.7 µs | 95.2 µs | 68.2 µs (sd 1.7) | 14,505 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.8 µs | 64.0 µs | 64.9 µs | 80.1 µs | 92.7 µs | 67.7 µs (sd 1.2) | 14,642 stmt/s |
| DELETE by pk | A | 48,000 | 36.8 µs | 48.4 µs | 49.2 µs | 64.5 µs | 76.3 µs | 57.2 µs (sd 15.9) | 19,024 stmt/s |
| DELETE by pk | B | 48,000 | 38.5 µs | 48.4 µs | 49.3 µs | 64.2 µs | 76.7 µs | 52.4 µs (sd 3.3) | 19,227 stmt/s |
| SHOW META (control) | A | 48,000 | 43.9 µs | 49.8 µs | 50.4 µs | 62.8 µs | 73.0 µs | 52.6 µs (sd 1.0) | 18,857 stmt/s |
| SHOW META (control) | B | 48,000 | 42.4 µs | 49.8 µs | 50.4 µs | 62.7 µs | 71.3 µs | 52.3 µs (sd 0.7) | 19,028 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.65 µs [+0.30 µs, +1.15 µs] | +1.26 % [+0.58, +2.23] | +9.31 µs [-2.66 µs, +26.49 µs] | -1.39 % [-4.25, -0.70] | **B slower** (+1.3 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, by pk (replicate) | +0.65 µs [+0.30 µs, +0.90 µs] | +1.26 % [+0.58, +1.72] | -1.21 µs [-5.71 µs, +1.35 µs] | -1.63 % [-2.37, -0.07] | **B slower** (+1.3 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, w = lit, by pk | +0.95 µs [+0.70 µs, +1.35 µs] | +1.82 % [+1.34, +2.59] | +0.79 µs [-0.24 µs, +1.81 µs] | -1.31 % [-2.95, -0.30] | **B slower** (+1.8 % at p50, floor 0.8 %) |
| UPDATE SET v = lit, every row | +192.05 µs [+126.40 µs, +480.10 µs] | +1.97 % [+1.30, +4.98] | -5,365 µs [-8,561 µs, -2,710 µs] | +17.21 % [+13.80, +29.67] | **B slower** (+2.0 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row (replicate) | +216.20 µs [+149.80 µs, +482.00 µs] | +2.23 % [+1.54, +4.99] | -27.18 ms [-33.14 ms, -22.84 ms] | +219.76 % [+131.13, +235.39] | **B slower** (+2.2 % at p50, floor 1.1 %) |
| INSERT, pk named | +0.10 µs [+0.00 µs, +0.20 µs] | +0.20 % [+0.00, +0.40] | +47.89 µs [-4.61 µs, +121.11 µs] | +1.47 % [-3.76, +3.23] | no change resolved |
| SELECT by pk (control) | -0.15 µs [-0.90 µs, +0.15 µs] | -0.23 % [-1.37, +0.31] | -0.47 µs [-1.27 µs, +0.19 µs] | +0.65 % [-0.12, +1.07] | no change resolved |
| DELETE by pk | +0.10 µs [-0.00 µs, +0.20 µs] | +0.20 % [+0.00, +0.41] | -4.81 µs [-12.02 µs, -0.26 µs] | +1.00 % [-1.13, +3.79] | no change resolved |
| SHOW META (control) | -0.05 µs [-0.30 µs, +0.20 µs] | -0.10 % [-0.60, +0.49] | -0.33 µs [-0.75 µs, +0.08 µs] | +0.31 % [-0.36, +1.64] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ -0.10 % [-0.60, +0.49]; replicate (second relation, same binary, per run, both arms pooled) point median +0.19 %, robust sd 0.29 % (plain sd 0.48 %); range median +0.12 %, robust sd 0.50 % (plain sd 0.61 %). Floor used: point 0.77 %, range 1.12 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 43.3 µs | 52.1 µs | 53.2 µs | 68.9 µs | 82.4 µs | 58.3 µs (sd 5.3) | 17,283 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 46.7 µs | 55.2 µs | 55.9 µs | 71.6 µs | 85.0 µs | 67.1 µs (sd 14.4) | 16,557 stmt/s | +2.70 µs [+2.55 µs, +2.85 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 46.4 µs | 54.7 µs | 55.4 µs | 70.4 µs | 83.3 µs | 70.0 µs (sd 36.0) | 16,317 stmt/s | +2.20 µs [+2.10 µs, +2.45 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 51.2 µs | 61.2 µs | 62.1 µs | 79.5 µs | 94.8 µs | 90.6 µs (sd 48.9) | 14,194 stmt/s | +8.95 µs [+8.70 µs, +9.10 µs] | - | - |

range (`WHERE id >= 1`, every one of the 10,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 8,723 µs | 9,109 µs | 9,547 µs | 16.94 ms | 409.98 ms | 24.78 ms (sd 8559.2) | 51 stmt/s = 505,500 rows/s | - | 955 ns | - |
| `SET v = v + 1` | 1,920 | 10.01 ms | 10.42 ms | 10.76 ms | 20.73 ms | 416.16 ms | 28.62 ms (sd 9683.8) | 42 stmt/s = 416,500 rows/s | +1,263 µs [+1,214 µs, +1,320 µs] | 1076 ns | +126 ns [+121, +132] |
| `SET v = w, w = v` | 1,920 | 9,623 µs | 10.05 ms | 10.41 ms | 17.36 ms | 78.77 ms | 15.49 ms (sd 2781.3) | 69 stmt/s = 692,000 rows/s | +908.25 µs [+826.50 µs, +948.75 µs] | 1041 ns | +91 ns [+83, +95] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 11.52 ms | 11.95 ms | 12.34 ms | 20.55 ms | 448.61 ms | 35.58 ms (sd 11422.9) | 36 stmt/s = 361,500 rows/s | +2,839 µs [+2,800 µs, +2,904 µs] | 1234 ns | +284 ns [+280, +290] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 9,987 µs | 10.47 ms | 10.92 ms | 19.78 ms | 401.93 ms | 24.45 ms (sd 6040.2) | 48 stmt/s = 480,500 rows/s | +1,380 µs [+1,329 µs, +1,423 µs] | 1092 ns | +138 ns [+133, +142] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 45.9 µs | 54.9 µs | 55.7 µs | 77.4 µs | 173.4 µs | 79.9 µs (sd 42.5) | 15,467 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 110.1 µs | 121.4 µs | 123.5 µs | 154.6 µs | 184.5 µs | 132.3 µs (sd 8.4) | 7,647 /s | +67.75 µs [+67.45 µs, +68.30 µs] |
|   of which the `SELECT v` | 48,000 | 55.3 µs | 63.6 µs | 64.6 µs | 84.2 µs | 99.4 µs | 70.5 µs (sd 8.5) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 49.1 µs | 56.6 µs | 57.4 µs | 75.1 µs | 89.6 µs | 60.8 µs (sd 1.1) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 171.0 µs | 181.9 µs | 186.3 µs | 226.9 µs | 259.6 µs | 235.5 µs (sd 61.0) | 4,834 /s | +130.40 µs [+129.70 µs, +131.20 µs] |


### cores = 2, 1,000 rows

16 runs used of 16 (0 discarded: other-CPU busy > 30% or verify/bad flag); loadavg(1 min) before each run 1.32-2.59, after 1.58-2.66; other-CPU busy over the run 2.2%-12.6% (median 5.3%); 0 errors; gate wait median 3 s; processes matching cc1plus|cmake --build|ctest: 1 distinct

**Part 1 - the literal path and the controls, A and B** (pooled over the runs; `ops` per arm = runs x statements)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | A | 48,000 | 46.6 µs | 52.0 µs | 52.9 µs | 70.4 µs | 82.9 µs | 55.5 µs (sd 0.7) | 17,662 stmt/s |
| UPDATE SET v = lit, by pk | B | 48,000 | 47.3 µs | 52.8 µs | 53.7 µs | 70.6 µs | 81.2 µs | 56.1 µs (sd 0.9) | 17,508 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | A | 48,000 | 44.4 µs | 51.9 µs | 52.8 µs | 69.4 µs | 80.7 µs | 57.8 µs (sd 7.3) | 17,609 stmt/s |
| UPDATE SET v = lit, by pk (replicate) | B | 48,000 | 43.6 µs | 52.8 µs | 53.6 µs | 69.9 µs | 81.6 µs | 57.0 µs (sd 1.3) | 17,278 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | A | 48,000 | 45.8 µs | 52.7 µs | 53.5 µs | 69.7 µs | 79.0 µs | 56.6 µs (sd 0.8) | 17,256 stmt/s |
| UPDATE SET v = lit, w = lit, by pk | B | 48,000 | 47.3 µs | 53.8 µs | 54.5 µs | 70.8 µs | 80.5 µs | 57.5 µs (sd 0.9) | 17,093 stmt/s |
| UPDATE SET v = lit, every row | A | 1,920 | 1,197 µs | 1,278 µs | 1,301 µs | 1,411 µs | 1,774 µs | 1,316 µs (sd 12.7) | 762 stmt/s |
| UPDATE SET v = lit, every row | B | 1,920 | 1,205 µs | 1,287 µs | 1,314 µs | 1,437 µs | 1,683 µs | 1,334 µs (sd 20.8) | 753 stmt/s |
| UPDATE SET v = lit, every row (replicate) | A | 1,920 | 1,261 µs | 1,292 µs | 1,309 µs | 1,493 µs | 2,066 µs | 1,482 µs (sd 525.0) | 742 stmt/s |
| UPDATE SET v = lit, every row (replicate) | B | 1,920 | 1,266 µs | 1,311 µs | 1,340 µs | 2,318 µs | 3,462 µs | 1,682 µs (sd 634.8) | 690 stmt/s |
| INSERT, pk named | A | 48,000 | 39.6 µs | 49.0 µs | 49.8 µs | 68.3 µs | 89.0 µs | 103.2 µs (sd 44.3) | 9,526 stmt/s |
| INSERT, pk named | B | 48,000 | 40.0 µs | 49.2 µs | 50.1 µs | 68.3 µs | 86.9 µs | 134.4 µs (sd 49.3) | 8,035 stmt/s |
| SELECT by pk (control) | A | 48,000 | 60.7 µs | 65.5 µs | 68.4 µs | 88.6 µs | 102.1 µs | 71.8 µs (sd 0.8) | 13,814 stmt/s |
| SELECT by pk (control) | B | 48,000 | 58.4 µs | 65.6 µs | 68.4 µs | 89.4 µs | 103.1 µs | 72.4 µs (sd 2.2) | 13,748 stmt/s |
| DELETE by pk | A | 48,000 | 44.1 µs | 48.7 µs | 49.6 µs | 66.6 µs | 75.6 µs | 52.2 µs (sd 1.1) | 19,059 stmt/s |
| DELETE by pk | B | 48,000 | 42.8 µs | 48.6 µs | 49.3 µs | 65.1 µs | 73.8 µs | 51.9 µs (sd 0.8) | 19,174 stmt/s |
| SHOW META (control) | A | 48,000 | 42.1 µs | 49.9 µs | 50.6 µs | 64.7 µs | 73.3 µs | 52.8 µs (sd 0.6) | 18,862 stmt/s |
| SHOW META (control) | B | 48,000 | 43.6 µs | 49.9 µs | 50.6 µs | 65.8 µs | 74.5 µs | 52.9 µs (sd 0.7) | 18,811 stmt/s |

| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|---|
| UPDATE SET v = lit, by pk | +0.70 µs [+0.00 µs, +1.50 µs] | +1.33 % [+0.00, +2.68] | +0.64 µs [+0.10 µs, +1.19 µs] | -1.42 % [-2.46, +0.40] | no change resolved |
| UPDATE SET v = lit, by pk (replicate) | +0.75 µs [+0.20 µs, +1.60 µs] | +1.42 % [+0.38, +3.03] | -0.86 µs [-4.96 µs, +1.64 µs] | -1.72 % [-3.21, +0.49] | **B slower** (+1.4 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, w = lit, by pk | +0.90 µs [+0.30 µs, +1.50 µs] | +1.68 % [+0.56, +2.84] | +0.86 µs [+0.26 µs, +1.50 µs] | -1.14 % [-2.91, +0.38] | **B slower** (+1.7 % at p50, floor 1.1 %) |
| UPDATE SET v = lit, every row | +5.65 µs [-2.20 µs, +17.00 µs] | +0.44 % [-0.19, +1.56] | +17.76 µs [+7.56 µs, +28.26 µs] | -1.28 % [-2.69, -0.12] | no change resolved |
| UPDATE SET v = lit, every row (replicate) | +27.20 µs [+15.95 µs, +35.00 µs] | +2.08 % [+1.24, +2.69] | +199.06 µs [-195.04 µs, +618.43 µs] | -7.92 % [-10.02, -6.85] | CI excludes 0, inside the floor (3.0 %): not resolved |
| INSERT, pk named | +0.35 µs [+0.00 µs, +0.70 µs] | +0.70 % [+0.00, +1.41] | +31.20 µs [-10.87 µs, +71.53 µs] | -31.38 % [-54.45, +6.68] | no change resolved |
| SELECT by pk (control) | -0.05 µs [-0.50 µs, +0.40 µs] | -0.07 % [-0.73, +0.59] | +0.59 µs [-0.10 µs, +1.73 µs] | -0.41 % [-0.75, +0.25] | no change resolved |
| DELETE by pk | -0.20 µs [-0.40 µs, +0.10 µs] | -0.40 % [-0.81, +0.20] | -0.30 µs [-0.99 µs, +0.37 µs] | +0.76 % [-1.12, +1.76] | no change resolved |
| SHOW META (control) | +0.05 µs [-0.10 µs, +0.30 µs] | +0.10 % [-0.20, +0.60] | +0.11 µs [-0.39 µs, +0.61 µs] | -0.12 % [-1.26, +0.60] | control |

In-run noise floor (relative, p50): control `SHOW META` Δ +0.10 % [-0.20, +0.60]; replicate (second relation, same binary, per run, both arms pooled) point median -0.28 %, robust sd 0.41 % (plain sd 0.38 %); range median +1.11 %, robust sd 0.95 % (plain sd 0.97 %). Floor used: point 1.11 %, range 3.01 %.

**Part 2 - the expression's own price, B only**

point (`WHERE id = k`):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 48,000 | 45.5 µs | 52.9 µs | 53.7 µs | 70.8 µs | 80.5 µs | 65.0 µs (sd 22.9) | 17,237 stmt/s | - | - | - |
| `SET v = v + 1` | 48,000 | 48.4 µs | 55.5 µs | 56.2 µs | 73.7 µs | 82.6 µs | 64.0 µs (sd 17.9) | 16,371 stmt/s | +2.45 µs [+2.40 µs, +2.60 µs] | - | - |
| `SET v = w, w = v` | 48,000 | 47.1 µs | 55.0 µs | 55.7 µs | 73.5 µs | 83.1 µs | 60.1 µs (sd 1.5) | 16,186 stmt/s | +2.00 µs [+1.90 µs, +2.15 µs] | - | - |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 48,000 | 56.3 µs | 61.4 µs | 62.3 µs | 82.3 µs | 92.0 µs | 71.4 µs (sd 19.2) | 14,878 stmt/s | +8.65 µs [+8.30 µs, +8.80 µs] | - | - |

range (`WHERE id >= 1`, every one of the 1,000 rows):

| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `SET v = lit` | 1,920 | 1,114 µs | 1,158 µs | 1,185 µs | 1,878 µs | 3,104 µs | 1,752 µs (sd 1218.0) | 772 stmt/s = 771,550 rows/s | - | 1185 ns | - |
| `SET v = v + 1` | 1,920 | 1,231 µs | 1,278 µs | 1,308 µs | 1,518 µs | 22.78 ms | 3,428 µs (sd 1360.7) | 270 stmt/s = 270,200 rows/s | +120.35 µs [+118.30 µs, +126.00 µs] | 1308 ns | +120 ns [+118, +126] |
| `SET v = w, w = v` | 1,920 | 1,196 µs | 1,242 µs | 1,272 µs | 1,489 µs | 6,863 µs | 1,920 µs (sd 1463.0) | 711 stmt/s = 710,800 rows/s | +83.90 µs [+76.00 µs, +90.70 µs] | 1272 ns | +84 ns [+76, +91] |
| `SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END` | 1,920 | 1,389 µs | 1,445 µs | 1,474 µs | 2,182 µs | 3,268 µs | 1,708 µs (sd 474.5) | 637 stmt/s = 636,750 rows/s | +291.55 µs [+285.90 µs, +301.00 µs] | 1474 ns | +292 ns [+286, +301] |
| `SET v = (SELECT COUNT(*) FROM t)` | 1,920 | 1,252 µs | 1,303 µs | 1,332 µs | 1,555 µs | 2,578 µs | 1,709 µs (sd 659.9) | 705 stmt/s = 704,800 rows/s | +146.45 µs [+142.35 µs, +153.50 µs] | 1332 ns | +146 ns [+142, +154] |

**Part 3 - the client's former round trip, B only, one session**

| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |
|---|---|---|---|---|---|---|---|---|---|
| `UPDATE SET v = v + 1`, one statement | 48,000 | 49.5 µs | 55.2 µs | 55.9 µs | 74.0 µs | 83.3 µs | 106.8 µs (sd 50.8) | 11,769 /s | - |
| `SELECT v` then `UPDATE SET v = <v+1>`, two round trips | 48,000 | 113.2 µs | 123.0 µs | 125.9 µs | 153.4 µs | 169.6 µs | 131.0 µs (sd 1.7) | 7,608 /s | +70.10 µs [+69.15 µs, +70.90 µs] |
|   of which the `SELECT v` | 48,000 | 57.8 µs | 64.6 µs | 66.0 µs | 85.2 µs | 95.9 µs | 69.1 µs (sd 0.8) | - | - |
|   of which the `UPDATE SET v = <lit>` | 48,000 | 50.3 µs | 57.2 µs | 58.1 µs | 75.6 µs | 85.2 µs | 60.9 µs (sd 0.9) | - | - |
| `BEGIN; SELECT; UPDATE; COMMIT`, four round trips | 48,000 | 170.9 µs | 184.3 µs | 191.5 µs | 223.4 µs | 248.7 µs | 231.3 µs (sd 30.2) | 4,231 /s | +135.50 µs [+134.90 µs, +136.25 µs] |

