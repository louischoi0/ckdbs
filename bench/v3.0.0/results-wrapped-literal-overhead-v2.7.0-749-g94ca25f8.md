# The compile-path cost of the wrapped-integer-literal refusal (`v2.7.0-749-g94ca25f8`)

**Thesis.** On `worktree-fix-int-literal-wrap-in-where` at `94ca25f8` (`v2.7.0-749-g94ca25f8`), refusing an integer
literal that wrapped in the lexer (`CoerceLiteralToColumn`'s new signed-integer/bool arm, `PkBound`'s
`IntLiteralWrapped` test, the `kLookup` gate now calling `PkBound`) **costs nothing this harness can see: no
regression within the noise floor, and no sign of the constant +0.7 µs per-statement offset an earlier build pair
carried**. At 200, 1,000 and 10,000 rows (8 interleaved pairs each, fresh servers per pair) the pk `SELECT` moves
-0.75, -0.60 and +0.50 µs (median of the 8 per-run Δp50; B slower in 1, 1 and 6 of 8 runs) on a 95 to 99 µs
statement; the INSERT and `SHOW META` controls, which cannot
reach the change, move up to 0.6 µs in the same passes, so **the noise floor is about ±0.5 µs per statement
(±0.5 %) and every target-cell delta is at or inside it**. Slot-swap passes separate the binary from the
server slot: at 1,000 rows the binary effect on the pk `SELECT` is -0.45 µs and at 10,000 rows -0.20 µs (B not
slower). The server-side foreground time per statement (`sched_foreground_polled_us`, 25 to 34 µs of the ~100)
moves by -0.34 µs at 1,000 rows and +0.37 µs at 10,000 rows (B slower in 6 of 8, mostly the slot: the swap pass
reads +0.23), so the change adds well under 0.5 µs per statement and the expected few nanoseconds are below
what this method resolves.

**One cell does move, and it is not this change.** The 10,000-row non-pk `SELECT` (a 10,000-row filter walk,
no compile-path involvement beyond the one literal) is **+9.3 and +10.9 µs (+1.2 %), B slower in 8 of 8 runs**
in the main pass. A slot swap halves it to a binary effect of +5.7 µs (+0.7 %, 0.57 ns per row), and a **layout
control binary that adds 6.6 KB of dead, never-called code at the same two translation units and changes no
behaviour moves the same cell by +10.5 µs (+1.3 %)**. The walk's cost follows the link layout of a loop the
change does not touch, so it is not attributed to the change.

- **A** = `6b0d8412` (`v2.7.0-747-g6b0d8412`), the branch's base (origin/main when the branch was cut).
  **B** = `94ca25f8`, the branch tip, tree clean. `git diff --stat 6b0d8412 94ca25f8 -- src` is
  `src/exec/row_codec.cpp` (+22/-8, one switch arm and a comment reword) and `src/exec/step_compiler.cpp`
  (+9/-6: the `IntLiteralWrapped` test in `PkBound`, and the `kLookup` gate calls `PkBound`).
- **The suite was not executed by this measurement** and nothing in the tree changed in it; the stage's own review
  and `tests/types_contract_test.cpp` carry correctness. A functional spot check on the measured copies
  (`archive/.../drivers/func.py`) shows A answering `WHERE id = 18446744073709551616` with an empty result and B
  refusing it `integer literal ... is outside the range of an int64`, so the two binaries are the engines they are
  named for.
- **Baseline.** No earlier results file has this run's exact shape. The nearest is
  `bench/v3.0.0/results-ap-s5-overhead-v2.7.0-606-g0c3268f.md` (cells C1 and C4: a pk `SELECT` at 82 µs and a pk
  `UPDATE` at 51 to 52 µs on an eleven-column table, 143 commits earlier, different host load); this run uses a
  three-column table, so **no delta against it is drawn** and **this run is the baseline the next one is read
  against**. The comparison here is A against B at one point in time. The PostgreSQL floor was not measured for this
  shape.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10, 18:22 to 18:35 UTC. Main matrix 18:22:38-18:27:07 (8 pairs x 3 sizes), swap pass at 1,000 rows 18:27:11-18:28:05 (5 pairs), swap pass at 10,000 rows 18:28:38-18:30:03 (8 pairs), layout control 18:31:05-18:35:19 |
| Branch / worktree | `worktree-fix-int-literal-wrap-in-where`, worktree `fix-int-literal-wrap-in-where`, clean (`git status --short` empty) at build and at start |
| B | `94ca25f8` (`v2.7.0-749-g94ca25f8`), commit time 2026-10-10 18:16:46 UTC; built in the worktree's `build-release/` (`CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `kds_server` target, `nice` on CPUs 0, 1, 6, 7, 18:17:49 to 18:19:07); source binary mtime 2026-10-10 18:19:07 UTC, **after** the commit |
| A | `6b0d8412` (`v2.7.0-747-g6b0d8412`). The measured copy is the previous run's A (built 16:39 UTC from a detached worktree at that commit). It was **rebuilt this session** at the same commit in a fresh detached worktree (18:20:52 UTC) **and the rebuild is byte-identical** (`ee29d277...`), as is a second rebuild in a different directory, so the hash names the `6b0d8412` build and builds do not depend on the root path |
| Layout control C | `6b0d8412` plus two dead non-inlined functions (700 and 300 volatile stores) inserted before `CoerceLiteralToColumn` and before `PkBound`; never called, no behaviour change; `.text` +6,644 bytes against A (B is +4,556) (`archive/.../drivers/mkpad.py`) |
| Measured copies | A `ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`, B `d634f115d4ec374049521947afe99c57688008859d4a945e2501e619a250fb02`, C `03980907960cba8344f464400165671da0d8c52ce92229d3faf31eba38efcb18` (`archive/.../binaries.sha256`). Every server started from a copy under `~/bench-runs/lw/`; the build tree was not read after the copy |
| Device | data files under `~/bench-runs/lw/work/`, `/dev/root`, ext4 (`df -T $HOME`, 2026-10-10), not tmpfs; a fresh server and data file per (pair, rows), deleted after the pair |
| Host | AMD EPYC 9V74, 8 logical CPUs (CPUs 6 and 7 are siblings of one core), Linux 7.0.0-1014-azure; shared with other sessions (below) |
| Pinning | both servers of a pair on CPU 6 (one is idle while the other is timed), client on CPU 7; ports 15620 (B slot) and 15621 (A slot) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, everything else default; relations `BTREE` |
| Driver | `lw_ab.py` / `lw_run.py` (`archive/.../drivers/`), shaped like the earlier A/B drivers: two servers side by side with identical data, per arm 16 blocks alternating which server goes first, equal statement counts per server. Run with `python3 lw_run.py 8 1000,200,10000 <outdir>` (main), `LW_SWAP=1 python3 lw_run.py 5 1000 <outdir>` / `... 8 10000 ...` (swap: the A slot runs the B binary), `LW_BINB=C ...` (control). `--rows N` is the row count of `t`; flags and defaults are the ones in the script's docstring (scenario drivers' own documentation is closed, `bench/README.md`) |

**Relations and arms.** `t (id int64, qty int64, v int64)` has N rows, N = 200, 1,000 or 10,000 (`--rows`), `id`
1..N and `qty = 3*id` (unique); `d (id, v)` has 6,400 rows (the DELETE target); `ins (id, v)` is the INSERT target.
Point arms run 400 statements per block, 16 blocks, so **6,400 statements per server per run, 51,200 per cell
over 8 runs, equal for A and B**. Scan arms (the non-pk filter) run `max(2, 40000 / N)` statements per block,
so each block visits about 40,000 rows (**equal work**): 200, 40 and 4 statements per block at 200, 1,000 and
10,000 rows, i.e. 25,600, 5,120 and 512 statements per cell. The pk `SELECT` keys cycle `1 + k mod N`, the
non-pk filter's `3*(1 + k mod N)`; every statement carries a distinct literal, so each is a fresh compile.
Reply rows are equal between A and B in every cell and errors are 0 in all 61 runs (24 main, 5 + 8 swap, 16 + 8 control).

**Host.** `/proc/loadavg` at each run's start ran 1.57 to 3.44 (per-run record in `archive/.../*/*.host.txt`);
CPUs 6 and 7 were 0.49 to 0.56 busy in every run (the server and the client of a serial driver).
`pgrep -f 'cc1plus|cmake --build|ctest'` at each run's start found **no compile or test in any of the 61 runs,
so none is excluded for that reason**. **The host was not otherwise quiet**: another session's benchmark (a
pinned server on CPU 2 and its driver on CPU 4, `ps` at 18:16 UTC) was running, and **26 of the 61 runs show
another CPU between 0.11 and 0.55 busy** (CPU 2 in 16 of them, CPUs 0, 1, 3, 4 or 5 in the rest) while the
other 35 read at most 0.06 on every CPU outside 6 and 7. These are not compiles or tests and not on CPUs 6 and 7, so they are
**kept**; the matrix is interleaved (A and B alternate within each pair), which spreads such a neighbour across
both engines, but it cannot be excluded as a source of run-to-run spread. One sensitivity cut was taken, on
the only cell that moved: dropping the 10,000-row main runs 1 and 8 and the swap runs 3, 4, 5, 7 and 8 (the ones
with another CPU at 0.33 to 0.49) leaves the non-pk binary effect at +5.7 and +5.2 µs against +5.7 and +5.8 µs
with them (`archive/.../sensitivity-*.json`). My own C builds ran between passes (18:30 to 18:31), not during
one, and no build overlapped a timed pass.

## 2. The compile-path cells

Delta = median over the 8 runs of the per-run (B p50 - A p50); IQR over the same 8 differences; "B slower in"
counts runs where that difference is positive. **QPS is derived**: `1,000,000 / mean µs` of the per-run mean
statement time, median over runs (a serial single-connection client, so it is the exact conversion).
Δ engine is the server's `sched_foreground_polled_us` per statement. `upd-pk`'s QPS rows at 1,000 and 10,000 rows (+6 % and -10 %)
are not results: its per-run *mean* is moved by a few multi-millisecond stalls (its Δ mean changes sign
between the main and swap passes: -5.2 and +10.4 µs at 1,000 rows), while its p50 does not move.

### 2.1 200 rows

| Arm | A QPS | B QPS | Δ QPS | A p50 µs | B p50 µs | Δp50 µs [IQR] | B slower in | Δ engine µs/stmt [IQR] |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pk SELECT `WHERE id = <lit>` (1) | 9,787 | 9,831 | +44 | 98.2 | 97.3 | -0.75 [-1.30, -0.17] | 1/8 | -0.25 [-0.38, -0.07] |
| pk SELECT, other key sequence (noise control) | 9,839 | 9,889 | +49 | 97.8 | 97.4 | -0.80 [-1.25, -0.20] | 2/8 | -0.42 [-0.50, -0.12] |
| pk SELECT `WHERE id = 9223372036854775807` (19-char literal, 0 rows) | 11,152 | 11,238 | +85 | 86.7 | 86.2 | -0.65 [-0.90, -0.30] | 1/8 | -0.23 [-0.31, -0.16] |
| non-pk SELECT `WHERE qty = <lit>`, 1 row (2) | 8,768 | 8,817 | +49 | 110.8 | 110.2 | -0.65 [-0.85, -0.10] | 1/8 | -0.17 [-0.36, -0.02] |
| non-pk SELECT, other key sequence (noise control) | 8,445 | 8,731 | +285 | 111.0 | 110.3 | -0.70 [-1.02, -0.25] | 1/8 | -0.24 [-0.32, -0.00] |
| point UPDATE `WHERE id = <lit>` (3) | 12,460 | 12,440 | -20 | 77.1 | 76.5 | -0.50 [-0.73, -0.28] | 2/8 | -0.19 [-0.35, -0.11] |
| point DELETE `WHERE id = <lit>` (4a) | 13,240 | 13,243 | +3 | 72.0 | 71.9 | -0.20 [-0.43, +0.05] | 2/8 | +0.03 [-0.18, +0.11] |
| INSERT, no WHERE literal (4b, control) | 13,194 | 13,271 | +78 | 72.4 | 72.0 | -0.40 [-0.95, +0.02] | 2/8 | -0.13 [-0.42, +0.09] |
| `SHOW META`, no statement literal (control) | 13,475 | 13,425 | -50 | 72.1 | 72.3 | +0.30 [-0.15, +0.43] | 5/8 | +0.31 [-0.04, +0.41] |

### 2.2 1,000 rows

| Arm | A QPS | B QPS | Δ QPS | A p50 µs | B p50 µs | Δp50 µs [IQR] | B slower in | Δ engine µs/stmt [IQR] |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pk SELECT `WHERE id = <lit>` (1) | 9,671 | 9,690 | +19 | 99.5 | 98.9 | -0.60 [-0.87, -0.30] | 1/8 | -0.34 [-0.44, -0.28] |
| pk SELECT, other key sequence (noise control) | 9,770 | 9,789 | +20 | 99.0 | 98.5 | -0.65 [-0.92, -0.27] | 2/8 | -0.32 [-0.45, -0.26] |
| pk SELECT `WHERE id = 9223372036854775807` (19-char literal, 0 rows) | 11,158 | 11,211 | +53 | 86.8 | 86.5 | -0.45 [-0.95, -0.30] | 1/8 | -0.44 [-0.55, -0.14] |
| non-pk SELECT `WHERE qty = <lit>`, 1 row (2) | 5,747 | 5,792 | +45 | 169.2 | 168.8 | -1.00 [-2.55, +0.15] | 2/8 | -0.57 [-3.97, +0.78] |
| non-pk SELECT, other key sequence (noise control) | 5,765 | 5,774 | +9 | 169.2 | 168.9 | -0.65 [-2.63, +0.50] | 3/8 | -0.37 [-3.62, +0.69] |
| point UPDATE `WHERE id = <lit>` (3) | 11,557 | 12,276 | +719 | 77.3 | 77.2 | -0.25 [-0.85, +0.18] | 3/8 | -0.25 [-0.44, +0.29] |
| point DELETE `WHERE id = <lit>` (4a) | 13,231 | 13,136 | -94 | 72.3 | 71.9 | -0.25 [-0.62, +0.15] | 3/8 | -0.18 [-0.22, +0.11] |
| INSERT, no WHERE literal (4b, control) | 13,203 | 13,306 | +102 | 72.3 | 71.9 | -0.45 [-0.57, -0.03] | 2/8 | -0.11 [-0.25, +0.09] |
| `SHOW META`, no statement literal (control) | 13,358 | 13,419 | +61 | 72.2 | 72.3 | -0.05 [-0.22, +0.12] | 2/8 | +0.06 [-0.05, +0.17] |

### 2.3 10,000 rows

| Arm | A QPS | B QPS | Δ QPS | A p50 µs | B p50 µs | Δp50 µs [IQR] | B slower in | Δ engine µs/stmt [IQR] |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pk SELECT `WHERE id = <lit>` (1) | 10,130 | 10,094 | -37 | 95.5 | 96.0 | +0.50 [+0.10, +0.50] | 6/8 | +0.37 [+0.01, +0.47] |
| pk SELECT, other key sequence (noise control) | 10,038 | 9,988 | -50 | 95.8 | 96.2 | +0.30 [+0.12, +0.35] | 6/8 | +0.26 [+0.01, +0.47] |
| pk SELECT `WHERE id = 9223372036854775807` (19-char literal, 0 rows) | 11,268 | 11,249 | -20 | 86.0 | 86.1 | -0.05 [-0.28, +0.32] | 3/8 | +0.25 [-0.04, +0.61] |
| non-pk SELECT `WHERE qty = <lit>`, 1 row (2) | 1,235 | 1,234 | -1 | 799.2 | 809.5 | +9.25 [+7.23, +11.90] | 8/8 | +10.41 [+6.57, +12.37] |
| non-pk SELECT, other key sequence (noise control) | 1,246 | 1,233 | -13 | 803.0 | 812.7 | +10.85 [+4.22, +12.95] | 8/8 | +7.80 [+2.11, +11.37] |
| point UPDATE `WHERE id = <lit>` (3) | 12,269 | 11,062 | -1,207 | 77.2 | 77.2 | -0.20 [-0.45, +0.70] | 3/8 | +0.09 [-0.01, +0.36] |
| point DELETE `WHERE id = <lit>` (4a) | 13,283 | 13,304 | +21 | 71.9 | 71.8 | -0.10 [-0.50, +0.25] | 3/8 | +0.09 [-0.14, +0.31] |
| INSERT, no WHERE literal (4b, control) | 13,062 | 13,215 | +154 | 72.2 | 72.1 | -0.60 [-0.75, -0.10] | 2/8 | -0.13 [-0.41, +0.04] |
| `SHOW META`, no statement literal (control) | 13,457 | 13,485 | +27 | 72.2 | 72.0 | -0.25 [-0.53, +0.25] | 3/8 | +0.21 [-0.13, +0.33] |

The four requested cells are `sel-pk`, `sel-nonpk`, `upd-pk` and `del-pk`/`ctl-insert`. The compile-path cells
(`sel-pk`, `upd-pk`, `del-pk`, `sel-pk-long`, and `sel-nonpk` at 200 and 1,000 rows) sit inside ±0.8 µs at every
size, mostly with B *faster* at 200 and 1,000 rows (B slower in 1 to 3 of 8 runs at those sizes), and the controls that cannot
reach the change move the same way (`ctl-insert` -0.40, -0.45, -0.60 µs; its sibling `sel-pk-again` -0.80, -0.65,
+0.30). The 10,000-row pk `SELECT` reads +0.50 µs with B slower in 6 of 8 runs, and its repeat +0.30 µs, also 6 of 8; the
swap pass in section 4 gives that cell a slot effect of +0.70 µs and a binary effect of -0.20 µs, so the positive
sign belongs to the server slot. The `sel-pk-long` arm is the one that runs `IntLiteralWrapped` past its length early-out (a 19-character
literal, `9223372036854775807`, a valid int64 that wraps nothing): -0.65, -0.45 and -0.05 µs. The 19-character
literal is the worst case of the new test and it does not show it.

## 3. Latency distributions

Pooled over the 8 runs (every statement of the cell, so 51,200 per engine on a point arm); microseconds,
client-observed, includes the socket round trip.

### 3.1 200 rows

| Arm | Engine | Statements | p0 | p25 | p50 | p75 | p99 |
|---|---|---:|---:|---:|---:|---:|---:|
| sel-pk | A | 51,200 | 91.3 | 96.9 | 98.4 | 104.4 | 131.6 |
| sel-pk | B | 51,200 | 92.1 | 96.1 | 97.7 | 103.6 | 132.9 |
| sel-pk-again | A | 51,200 | 93.2 | 96.9 | 98.4 | 103.4 | 130.3 |
| sel-pk-again | B | 51,200 | 91.5 | 96.2 | 97.6 | 102.4 | 129.4 |
| sel-pk-long | A | 51,200 | 82.6 | 85.8 | 86.9 | 90.0 | 111.4 |
| sel-pk-long | B | 51,200 | 78.9 | 85.3 | 86.3 | 89.5 | 110.9 |
| sel-nonpk | A | 25,600 | 105.9 | 109.4 | 110.8 | 115.4 | 138.6 |
| sel-nonpk | B | 25,600 | 104.4 | 109.1 | 110.6 | 115.0 | 138.9 |
| sel-nonpk-again | A | 25,600 | 106.1 | 109.6 | 111.1 | 115.6 | 138.2 |
| sel-nonpk-again | B | 25,600 | 104.9 | 109.2 | 110.8 | 115.3 | 137.9 |
| upd-pk | A | 51,200 | 69.0 | 76.1 | 77.1 | 80.4 | 109.4 |
| upd-pk | B | 51,200 | 71.4 | 75.7 | 76.7 | 80.0 | 108.4 |
| del-pk | A | 51,200 | 66.7 | 71.2 | 72.1 | 74.8 | 101.9 |
| del-pk | B | 51,200 | 63.8 | 71.1 | 71.9 | 74.7 | 102.5 |
| ctl-insert | A | 51,200 | 65.6 | 71.4 | 72.4 | 75.4 | 105.7 |
| ctl-insert | B | 51,200 | 63.7 | 71.0 | 72.0 | 75.0 | 104.3 |
| ctl-ping | A | 51,200 | 68.6 | 71.2 | 72.2 | 74.3 | 93.7 |
| ctl-ping | B | 51,200 | 63.5 | 71.3 | 72.3 | 74.6 | 94.0 |

### 3.2 1,000 rows

| Arm | Engine | Statements | p0 | p25 | p50 | p75 | p99 |
|---|---|---:|---:|---:|---:|---:|---:|
| sel-pk | A | 51,200 | 90.9 | 97.2 | 99.4 | 107.2 | 135.4 |
| sel-pk | B | 51,200 | 91.5 | 96.7 | 98.9 | 106.8 | 135.7 |
| sel-pk-again | A | 51,200 | 93.4 | 97.5 | 99.1 | 103.9 | 130.0 |
| sel-pk-again | B | 51,200 | 92.8 | 97.0 | 98.6 | 103.5 | 131.0 |
| sel-pk-long | A | 51,200 | 78.2 | 85.8 | 86.9 | 90.0 | 111.0 |
| sel-pk-long | B | 51,200 | 76.8 | 85.3 | 86.4 | 89.5 | 110.3 |
| sel-nonpk | A | 5,120 | 161.1 | 167.2 | 171.4 | 179.1 | 206.1 |
| sel-nonpk | B | 5,120 | 161.2 | 166.3 | 169.2 | 176.9 | 199.8 |
| sel-nonpk-again | A | 5,120 | 160.4 | 167.3 | 171.6 | 180.2 | 204.8 |
| sel-nonpk-again | B | 5,120 | 161.8 | 166.5 | 169.3 | 177.6 | 202.1 |
| upd-pk | A | 51,200 | 68.1 | 76.4 | 77.4 | 80.7 | 109.6 |
| upd-pk | B | 51,200 | 69.8 | 76.1 | 77.3 | 80.6 | 109.9 |
| del-pk | A | 51,200 | 68.6 | 71.4 | 72.3 | 75.0 | 103.0 |
| del-pk | B | 51,200 | 63.8 | 71.0 | 72.1 | 74.9 | 102.6 |
| ctl-insert | A | 51,200 | 64.1 | 71.4 | 72.3 | 75.3 | 103.4 |
| ctl-insert | B | 51,200 | 63.8 | 71.1 | 72.2 | 75.2 | 103.5 |
| ctl-ping | A | 51,200 | 67.2 | 71.3 | 72.3 | 74.5 | 94.7 |
| ctl-ping | B | 51,200 | 63.9 | 71.3 | 72.4 | 74.7 | 94.1 |

### 3.3 10,000 rows

| Arm | Engine | Statements | p0 | p25 | p50 | p75 | p99 |
|---|---|---:|---:|---:|---:|---:|---:|
| sel-pk | A | 51,200 | 90.9 | 94.6 | 95.9 | 99.6 | 122.3 |
| sel-pk | B | 51,200 | 90.2 | 94.7 | 95.9 | 99.9 | 122.9 |
| sel-pk-again | A | 51,200 | 90.6 | 94.4 | 96.0 | 102.7 | 128.8 |
| sel-pk-again | B | 51,200 | 91.1 | 94.6 | 96.1 | 103.0 | 129.7 |
| sel-pk-long | A | 51,200 | 77.7 | 85.1 | 86.2 | 89.4 | 110.8 |
| sel-pk-long | B | 51,200 | 82.0 | 85.1 | 86.2 | 89.4 | 109.5 |
| sel-nonpk | A | 512 | 767.5 | 791.0 | 800.1 | 808.4 | 1383.9 |
| sel-nonpk | B | 512 | 770.9 | 801.0 | 810.0 | 817.4 | 862.6 |
| sel-nonpk-again | A | 512 | 766.8 | 794.4 | 803.0 | 809.6 | 894.4 |
| sel-nonpk-again | B | 512 | 771.8 | 803.4 | 811.9 | 819.8 | 940.0 |
| upd-pk | A | 51,200 | 70.0 | 76.1 | 77.4 | 80.6 | 109.2 |
| upd-pk | B | 51,200 | 69.4 | 76.1 | 77.2 | 80.6 | 111.2 |
| del-pk | A | 51,200 | 65.1 | 71.1 | 72.1 | 74.9 | 103.2 |
| del-pk | B | 51,200 | 62.4 | 71.1 | 72.0 | 74.8 | 102.0 |
| ctl-insert | A | 51,200 | 64.6 | 71.5 | 72.6 | 75.6 | 105.2 |
| ctl-insert | B | 51,200 | 66.0 | 71.2 | 72.1 | 75.2 | 104.8 |
| ctl-ping | A | 51,200 | 66.3 | 71.3 | 72.3 | 74.5 | 93.7 |
| ctl-ping | B | 51,200 | 68.6 | 71.2 | 72.2 | 74.3 | 93.9 |

The two engines' distributions overlap for every point arm: the largest pooled p50 gap is 0.8 µs (`sel-pk` and
`sel-pk-again` at 200 rows, B faster) and the p99s differ by up to 1.3 µs in both directions. The 1,000-row non-pk
filter reads B 1 to 2 µs faster, and the 10,000-row non-pk row is the exception that is the subject of section 5.

## 4. Slot and binary effect

A pair in the main pass runs A in the 15621 slot and B in the 15620 slot, so the delta is the binary effect `x`
plus whatever the slot costs, `s`. In the swap pass the slots hold the opposite binaries, so its delta (still
labelled slot B minus slot A) is `-x + s`; **x = (main - swap) / 2**, **s = (main + swap) / 2**.

### 4.1 1,000 rows (swap: 5 pairs)

| Arm | main pass Δp50 | swap pass Δp50 (slot labels) | **binary effect x** | slot effect s | x as % of A p50 |
|---|---:|---:|---:|---:|---:|
| sel-pk | -0.60 | +0.30 | **-0.45** | -0.15 | -0.45 % |
| sel-pk-again | -0.65 | +0.30 | **-0.47** | -0.18 | -0.48 % |
| sel-pk-long | -0.45 | -0.30 | **-0.07** | -0.37 | -0.09 % |
| sel-nonpk | -1.00 | -0.80 | **-0.10** | -0.90 | -0.06 % |
| sel-nonpk-again | -0.65 | -1.00 | **+0.18** | -0.82 | +0.10 % |
| upd-pk | -0.25 | +0.10 | **-0.17** | -0.08 | -0.23 % |
| del-pk | -0.25 | +0.00 | **-0.12** | -0.12 | -0.17 % |
| ctl-insert | -0.45 | +0.10 | **-0.28** | -0.17 | -0.38 % |
| ctl-ping | -0.05 | +0.30 | **-0.18** | +0.12 | -0.24 % |

### 4.2 10,000 rows (swap: 8 pairs)

| Arm | main pass Δp50 | swap pass Δp50 (slot labels) | **binary effect x** | slot effect s | x as % of A p50 |
|---|---:|---:|---:|---:|---:|
| sel-pk | +0.50 | +0.90 | **-0.20** | +0.70 | -0.21 % |
| sel-pk-again | +0.30 | +0.95 | **-0.32** | +0.62 | -0.34 % |
| sel-pk-long | -0.05 | +0.35 | **-0.20** | +0.15 | -0.23 % |
| sel-nonpk | +9.25 | -2.10 | **+5.67** | +3.58 | +0.71 % |
| sel-nonpk-again | +10.85 | -0.65 | **+5.75** | +5.10 | +0.72 % |
| upd-pk | -0.20 | +0.05 | **-0.12** | -0.07 | -0.16 % |
| del-pk | -0.10 | +0.25 | **-0.18** | +0.07 | -0.24 % |
| ctl-insert | -0.60 | +0.20 | **-0.40** | -0.20 | -0.55 % |
| ctl-ping | -0.25 | -0.10 | **-0.07** | -0.17 | -0.10 % |

The pk `SELECT` binary effect is -0.45 µs at 1,000 rows and -0.20 µs at 10,000 rows (B faster), and the slot
effect is -0.15 and +0.70 µs. **The +0.7 µs offset an earlier build pair carried on the pk `SELECT` appears here
only as the 10,000-row slot effect** (+0.70 on `sel-pk`, +0.62 on its repeat, +0.15 on `sel-pk-long`), which the
swap removes: it follows the server slot at that size, not the binary and not the change. At 1,000 rows it
does not appear, and the 200-row cells have no swap pass. The pass has 5 pairs at 1,000 rows, so the 1,000-row
`x` values are a median of 5.

## 5. The 10,000-row filter walk and the layout control

| Binary vs A | Arm | main pass Δp50 [IQR], B slower | swap pass Δp50 [IQR], B slower | binary effect x | x per row |
|---|---|---:|---:|---:|---:|
| **B** `94ca25f8` | sel-nonpk | +9.25 [+7.23, +11.90], 8/8 | -2.10 [-9.10, +0.03], 2/8 | **+5.67 µs** (+0.71 %) | +0.57 ns |
| **B** `94ca25f8` | sel-nonpk-again | +10.85 [+4.22, +12.95], 8/8 | -0.65 [-7.90, +2.65], 4/8 | **+5.75 µs** (+0.72 %) | +0.57 ns |
| **C** layout control, no behaviour change | sel-nonpk | +12.15 [+2.10, +19.58], 7/8 | -8.90 [-13.95, -3.90], 1/8 | **+10.53 µs** (+1.31 %) | +1.05 ns |
| **C** layout control, no behaviour change | sel-nonpk-again | +9.45 [+4.10, +18.93], 7/8 | -7.70 [-14.62, -2.40], 1/8 | **+8.57 µs** (+1.06 %) | +0.86 ns |

The non-pk filter at 10,000 rows is a walk of 10,000 leaf rows per statement (727 µs of engine time at A). B reads
+5.7 µs after the slot is removed (+0.7 %, 0.57 ns per row), 8 of 8 slower in the main pass. C, which differs from
A by dead code and nothing else, reads **+10.5 µs** (+1.3 %), 7 of 8 and 7 of 8. A change that is a once-per-statement
compile test cannot cost a per-row nanosecond, and a binary that adds no behaviour does the same thing to the
same cell; the walk's speed depends on where the linker placed the loop. **The conclusion is that the 10,000-row
scan figure is a property of the build's layout, with ±1 % reachable by moving code that is never executed.**
At 1,000 rows the same cell reads x = -0.10 and +0.18 µs.

Control binary C against A, main pass, Δp50 µs [IQR], runs slower of 8, for the cells the target change touches
and the ones it cannot:

### 5.1 1,000 rows

| Arm | B Δp50 µs [IQR], B slower | C Δp50 µs [IQR], C slower |
|---|---:|---:|
| sel-pk | -0.60 [-0.87, -0.30], 1/8 | -0.15 [-0.73, +0.05], 2/8 |
| sel-pk-again | -0.65 [-0.92, -0.27], 2/8 | -0.10 [-0.67, +0.00], 1/8 |
| sel-pk-long | -0.45 [-0.95, -0.30], 1/8 | -0.45 [-0.62, -0.07], 1/8 |
| sel-nonpk | -1.00 [-2.55, +0.15], 2/8 | -0.35 [-0.45, +0.20], 3/8 |
| sel-nonpk-again | -0.65 [-2.63, +0.50], 3/8 | -0.40 [-0.72, +0.12], 3/8 |
| upd-pk | -0.25 [-0.85, +0.18], 3/8 | +0.30 [-0.60, +0.85], 5/8 |
| del-pk | -0.25 [-0.62, +0.15], 3/8 | +0.25 [-0.02, +0.60], 5/8 |
| ctl-insert | -0.45 [-0.57, -0.03], 2/8 | +0.20 [-0.23, +0.53], 5/8 |
| ctl-ping | -0.05 [-0.22, +0.12], 2/8 | +0.35 [+0.17, +0.50], 7/8 |

### 5.2 10,000 rows

| Arm | B Δp50 µs [IQR], B slower | C Δp50 µs [IQR], C slower |
|---|---:|---:|
| sel-pk | +0.50 [+0.10, +0.50], 6/8 | +0.15 [-0.20, +0.68], 4/8 |
| sel-pk-again | +0.30 [+0.12, +0.35], 6/8 | -0.30 [-0.50, +0.10], 2/8 |
| sel-pk-long | -0.05 [-0.28, +0.32], 3/8 | -0.35 [-0.55, +0.03], 2/8 |
| sel-nonpk | +9.25 [+7.23, +11.90], 8/8 | +12.15 [+2.10, +19.58], 7/8 |
| sel-nonpk-again | +10.85 [+4.22, +12.95], 8/8 | +9.45 [+4.10, +18.93], 7/8 |
| upd-pk | -0.20 [-0.45, +0.70], 3/8 | +0.05 [-0.32, +0.40], 4/8 |
| del-pk | -0.10 [-0.50, +0.25], 3/8 | +0.00 [-0.10, +0.40], 3/8 |
| ctl-insert | -0.60 [-0.75, -0.10], 2/8 | +0.45 [+0.13, +0.68], 6/8 |
| ctl-ping | -0.25 [-0.53, +0.25], 3/8 | +0.20 [-0.13, +0.25], 5/8 |

C's per-statement deltas on the point arms run -0.45 to +0.45 µs at both sizes, the same band as B's, which is
the estimate of the harness's resolution on a layout change of this size.

## 6. Noise floor

| Source | Cells | Range of Δp50 (main pass) | After slot correction |
|---|---|---:|---:|
| B against A, controls with no WHERE literal (`ctl-insert`, `ctl-ping`) | 200, 1,000, 10,000 | -0.60 to +0.30 µs | -0.40 to -0.07 µs (1,000 and 10,000 only) |
| B against A, repeated pk `SELECT` (`sel-pk-again`) | 200, 1,000, 10,000 | -0.80 to +0.30 µs | -0.47 to -0.32 µs (1,000 and 10,000 only) |
| C against A, all point arms | 1,000, 10,000 | -0.45 to +0.45 µs | -0.27 to +0.43 µs |

**The floor is about ±0.5 µs per statement on a 72 to 99 µs statement, ±0.5 %.** Every compile-path delta in this
run is at or inside it, in both signs. A cost of the change smaller than that is not resolved here; the
expected size of it (a switch arm and a length test, a few nanoseconds per statement) is about two orders
of magnitude under the floor, so this measurement supports "no cost within noise" and not "a cost of exactly N ns".
The 10,000-row scan is the one cell above the floor in the main pass (+9 to +11 µs) and section 5 shows the
control reproduces it.

## 7. Where the time goes

A statement is a sum, and this driver splits it in two. The server's own foreground time per statement
(`sched_foreground_polled_us` over the statement) is **about 31 to 34 µs for a pk `SELECT`, 28 µs for an UPDATE,
24 to 25 µs for a DELETE or INSERT and 31 µs for `SHOW META`**; the remaining 60 to 70 µs of the ~72 to 99 µs
client-observed time is the client's own work, the socket round trip and the scheduler's wake, which this
driver cannot separate from one another. Durability is `relaxed`, so no statement waits for an fsync (the write
arms are no slower than the read arms by a device wait). Lock and conflict waits do not occur: one connection
per server, no concurrent writer. **The change lives inside the engine's foreground time at compile**, whose
paired delta is -0.34 µs (1,000 rows) and +0.37 µs (10,000 rows, slot-confounded; swap +0.23) per statement
on 31 to 34 µs, 1 % and inside the floor. A per-statement compile cost cannot be pulled out of the 25 to 34 µs
by this method (the engine does not time its compile step on its own), so the claim above is a bound on the sum, not
a measured compile share.

## 8. What the run says about the engine

- **The compile-path guard is not a per-statement price.** The change adds a switch arm in
  `CoerceLiteralToColumn`, a length early-out in `IntLiteralWrapped` and one call in the `kLookup` gate. Its
  worst case, a 19-character literal that has to be inspected (`sel-pk-long`), is -0.65 to -0.05 µs in B against A,
  indistinguishable from nothing.
- **Layout alone moves a long scan by about 1 %.** 6.6 KB of dead code ahead of the walk's callers moves a 10,000-row
  non-pk filter by +10 µs (+1.3 %), and the pk point arms by less than 0.5 µs. Any A/B on a scan arm at 10,000 rows is
  read with a layout control or a slot swap beside it, or it measures the linker.
- **The server slot has an effect of its own at 10,000 rows**: 0.6 to 0.7 µs on the pk `SELECT`, up to +5 µs on
  the filter walk (`s` in section 4.2), (ports 15620 against 15621; the cause is not established). The swap pass is what separates
  it from the binary; a single main pass would have shown B 0.5 µs slower on the pk `SELECT` at 10,000 rows.
- **Builds at one commit are bit-reproducible here** whatever root they are built in (A's hash was equal across
  three builds in two roots), which is what lets a copy of an earlier A stand in for
  a rebuild.
- No stated design expectation is contradicted. `docs/spec/types.md` §3.2a is the owner of the refusal itself and
  this run measures its price, not its behaviour.

## 9. Limits

- 8 pairs per cell (5 at 1,000 in the 1,000-row swap pass). The resolution is about ±0.5 µs per statement; the
  expected effect is below it.
- The swap pass covers 1,000 and 10,000 rows only; the 200-row cells carry no slot correction and read -0.8 to
  +0.3 µs on all arms, controls included.
- The compile step was not timed separately; the figures bound the whole statement.
- `cores = 1`, `relaxed`, one connection: nothing here says anything about contended or `strict` workloads,
  which the change does not touch (it runs once per statement, at compile).
- The other session's benchmark shared the machine (section 1); the runs are kept and the sensitivity cut moves
  no estimate.

## 10. Reproduction

Raw output, the drivers and every hash are in
`bench/v3.0.0/archive/wrapped-literal-overhead-v2.7.0-749-g94ca25f8/` (`res/` main, `res-swap/` and `res-swap10k/` the
swap passes, `res-ctl/` and `res-ctl-swap/` the layout control; per-run JSON gzipped, `*.driver.txt`,
`*.host.txt`, `analysis.json`, `pooled.json`; `drivers/` holds `lw_ab.py`, `lw_run.py`, `mkpad.py` for C, and
the analysis scripts). `analyze.py <dir>` writes `analysis.json` and `pooled.json`, `slot.py` and `render.py`
print the tables above.
