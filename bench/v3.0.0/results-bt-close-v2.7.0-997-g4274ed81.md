# BT close (BT-S5): a rejected scan row costs 3.3-4.0 ns less at `4274ed81` than at `eeb88f47` (-10.6 % to -11.5 %), the every-row `UPDATE` about 3 % less, and no guard cell regressed

**Thesis.** On `bt-accept-tuple-row`, with the engine at `4274ed81` (`v2.7.0-997-g4274ed81`, **B**) against BT's opening `eeb88f47` (`v2.7.0-992-geeb88f47`, **A**), a `bn` row (`SELECT * FROM s WHERE w = ?`, 10,000 rows, `cores = 1`, `relaxed`, every page resident) costs **27.4 ns of server CPU on the literal path and 27.5 ns on the prepared path against A's 31.0 and 30.9 ns: -3.3 ns (-10.6 %) and -3.5 ns (-11.3 %), paired by run, B below A in 13 of 13 runs on both paths.** `wide-h` (13 columns, `WHERE h = ?`, 10,000 rows) falls by 4.0 ns (-11.4 % and -11.5 %), also in 13 of 13. A' - the bytes of A started as a second server, the in-run control - differs from A by a typical 0.45 / 0.48 ns on `bn` (largest 2.21 / 2.33) and 0.24 / 0.14 ns on `wide-h` (largest 2.46 / 1.20): the saving is 7 times the typical gap and 1.5 times the largest on `bn`, and 17-29 times the typical gap on `wide-h`. The net row is (scan CPU - point `SELECT` CPU on the same relation, arm, path and run) / rows.

- **G1 is met, at 43-46 % of BT-S1's ceiling.** BT-S1's ablation D-all, which also deleted the counts and the charge, saved 7.1 / 8.1 ns (literal / prepared); B saves 3.3 / 3.5. The real change is within 0.2 ns of D2 alone (the tuple read: 3.1 / 3.3), so what the order built beyond BT-S2 - the arming in one lookup, the epoch read on the kept paths, the deleted `spills_.clear()` - does not show separately in this run (section 10). The 3.0 ns bar the work order set for the whole order is passed by 0.3-0.5 ns.
- **G2 holds where this driver can see it.** Rows, digest, `examined=`, `matched=`, `sel=` and `pages=` are identical in A, A' and B in run 0 of every `bm_scan` file, and no reply was wrong in any of the 660 recorded arm-cells of the 38 runs. The row-touch budget's refusal row, Waystone trails and Cabin recording are not checked by these drivers and are the contract suites' (section 4).
- **BT-S4 shows on the every-row `UPDATE`:** over 1,000 rows B is **23.9 µs (-3.1 %) literal and 27.3 µs (-3.5 %) prepared cheaper per statement** than A in the pooled median of 15 paired runs (B slower than A in 2 and 5 of 15), against a typical A-against-A' gap of 12.5 / 12.6 µs. That is 1.9-2.2 times the typical gap and **inside the largest gap (27.8 and 83.9 µs)**, so at this size alone the bar BM-Q12 set (the largest gap) does not resolve it, and the three groups of five runs disagree on its size (literal -15.8, -33.6, -12.6 µs; prepared +0.7, -42.9, -14.1 µs; the quiet-host group U3 alone reads -1.6 % and -1.8 %). The 200-row (-7.4 / -8.8 µs, quiet host) and 10,000-row (-366 / -367 µs, 3 runs) cells, both with B started last, give 37-44 ns and 37 ns per updated row against 24-27 ns at 1,000 rows, with B below A in 6 of 6 and 5 of 6 of those runs: the sign is established, the size is 2-5 % depending on the group and the row count.
- **The point `SELECT` is unchanged:** B - A = -0.17 µs (-0.55 %) literal and +0.02 µs (+0.05 %) prepared of 30 and 28 µs, against a typical gap of 0.33 / 0.59 µs (15 runs). **No guard cell (the every-row `UPDATE`, the point `SELECT`) regressed beyond the A-against-A' spread.**
- **Against the floor,** PostgreSQL 18.6's 29.1 ns for `bn` (the BM close's figure, not re-measured here): B is 0.94 times it and A 1.07 times. A floor is a league check and not a target.

Section 1 states what was run, 2 the scan row, 3 the guard cells, 4 the identity check, 5 the row-count sweep, 6 the latency distributions, 7 the waits, 8 the delta against BT-S1's prediction and the engine's last numbers, 9 the noise floor and host, 10 what the run teaches, 11 what was cut. The drivers are documented in their own docstrings (`bench/README.md`: `bench/docs/` stays closed); the exact invocations are in `archive/bt-close-v2.7.0-997-g4274ed81/scripts/run.sh` and `run2.sh`.

| Item | Value |
|---|---|
| Executed | 2026-10-13, UTC. B built 08:46:04-08:48:25. Measurement 08:49:37-09:34:50 in ten phases (`progress.log`): p1a 08:49:37-08:55:29 (cut after its first run, section 9), p1b 08:55:42-09:05:38, p2 09:05:38-09:13:28, p3 09:13:28-09:21:41, p4 09:21:41-09:23:56, p5 09:23:56-09:25:28, p6 09:25:28-09:26:32, p7 09:26:32-09:28:13, p8 09:28:15-09:30:11, p9 09:30:11-09:34:50 |
| Worktree / branch | `bt-accept-tuple-row`, branch `worktree-bt-accept-tuple-row`. HEAD was `4274ed81` (`v2.7.0-997-g4274ed81`, committed 2026-10-13 08:45:12 UTC) when the run began and **moved to `4be1632e` (`v2.7.0-998-g4be1632e`, committed 09:22:05) during p3**, by a commit that changes `docs/spec/parser-v2.md` alone (`git diff --stat 4274ed81 4be1632e`); `src/`, `include/` and `tools/` are the same at both, so the engine measured is HEAD's |
| Commits measured | **A** = `eeb88f47ea158c816f31ae38fd17a459ed6b7faa`, `v2.7.0-992-geeb88f47`, committed 07:24:18 UTC (BT's opening). **B** = `4274ed8151d07cba6533964473e33eb4d48fb49c`, `v2.7.0-997-g4274ed81` (BT-S2, BT-S3, BT-S4 and the BT-S1 results). `git diff --stat eeb88f47 4274ed81 -- src include`: 7 files, 208 insertions, 214 deletions (`chain_frame.hpp/.cpp`, `step_vm.hpp/.cpp`, `heap_page.hpp/.cpp`, `command_dispatcher.cpp`); the rest of the range is the work order, `tests/heap_page_test.cpp`, `tools/bn_parts.json`, a bug entry and the BT-S1 results, none of it in a binary |
| Tree | B was built from `git archive 4274ed81` exported to `/home/cdkbs/bench-runs/bt-close/src`, never from the worktree. **The worktree was dirty** at 08:49 in `docs/spec/parser-v2.md` alone; by 09:42 (`archive/.../host.txt`, regenerated then to correct its CPU line; the 08:49 version differed only in that line, the date, HEAD `4274ed81` and that one-file status) it also carried a merge in progress staged by another operation in the worktree after 09:22 (files under `src/`, `include/`, `tests/`, `docs/` and `instructions/`, `UU instructions/v3.0.0/index.md`). None of it is in a binary. The drivers were read from the worktree's `tools/` throughout; their mtimes (07:26:28) and sha256 are the same at 08:49 and 09:42. A is BT-S1's A, built from `git archive eeb88f47`, untouched since |
| Binary provenance | Copies in `/home/cdkbs/bench-runs/bt-close/bin/`, hashed after the copy, every server started from them (`archive/.../binaries.txt`). **A and A'** (the same file started as two servers) sha256 `833501d0226cbfa0dfad4d87caaca67ab04e52c69b6a7d96476f9e74183c5c8d`, source mtime 2026-10-13 07:31:52.43 UTC (7 min 34 s after A's commit), equal to the hash BT-S1 recorded. **B** `0243f8d50bc481f6607a71ad6ea1ab75b2c8d7b5d718528051ad02a754730a74`, source `rel/kds_server` mtime 08:48:25.46 UTC (3 min 13 s after B's commit). Neither binary is older than its commit, and `sha256sum` of the three copies after the last phase (09:43) gave the same hashes |
| Build | `CMAKE_BUILD_TYPE=Release` (read from `CMakeCache.txt`), no extra flags, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j8`, gcc 13.3.0. B's build took 2 min 21 s and finished before any measurement started; no build of this session overlapped a measurement |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure, 16 GB. **Data files and WAL under `/home/cdkbs/bench-runs/bt-close/wd-*/` on `/dev/root`, ext4, not tmpfs** (`df -T`); the binaries' copies are on the same device |
| Pinning | Servers (one process per arm, one busy at a time) on CPU 2, the driver on CPU 4; the SMT siblings 3 and 5 and CPUs 0, 1, 6, 7 were left to whatever else ran. **The host was shared** (section 9) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, Waystone at its defaults, `scan_kernel` not written (default, on); a fresh server and data file per arm per run; ports 32811-32891 (not the default 15432). Of the 114 servers, the 111 whose directories survive (all but p1a's three) have an empty server log and a stderr of the four start-up and stop lines |
| Drivers | `tools/bm_scan.py` sha256 `1896ef20...`, `tools/bn_census.py` `d7b80765...`, `tools/kwp.py` `51f707d0...`, unchanged since `34ab4269` (the BM-S6 results commit). Tables: `archive/.../scripts/bt_close_scan.py`, `bt_close_bn.py`, `ident.py`, `hostsummary.py`; this file is `assemble.py` over `bt_close.tmpl`, so no table was typed |
| Suite | **Not executed in this session.** The close's full-suite run is not part of this measurement; the work order's BT-S2 and BT-S3 rows record 3690/3692 each, the two failures being two-core waits that timed out under other sessions' load and passed on re-run |

## 1. What was run

Two drivers, three arms each: **A** (`eeb88f47`), **A'** (A's bytes as a second server, the spread) and **B** (`4274ed81`). Per run a fresh server per arm and a fresh data file; the same relations loaded into each; every cell measured on all three block by block (12 blocks), the server order rotating per block and per run and the two driver paths alternating inside a block, so a server's position and host drift fall on every arm alike. The measure is the server's on-CPU time (the sum of `/proc/<pid>/task/*/schedstat`); wall time is the client's. The gate flags are BM-S6's (`--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1`, `--quiet-load 2`); the run-level load gate's wait was capped at 180 s in p1a and 30 s elsewhere (section 9).

| group | driver | cell / shape | rows | start order | runs | per-run work (equal across arms) | phases |
|---|---|---|---|---|---|---|---|
| S1 | `bm_scan.py census` | `bn`, `wide-h` (+ their point cells) | 10,000 | A, A', B | 5 | 192 scans per path per arm (16 per block), 3,000 point `SELECT`s per path | p1a (1 run) + p1b (4 runs) |
| S2 | same | same | 10,000 | B, A', A | 3 | same | p4 |
| S3 | same | same | 10,000 | A, A', B | 5 | same | p8 |
| Z | same | `rows-200`, `rows-1k` | 200; 1,000 | A, A', B | 4 | same | p5 |
| U1 | `bn_census.py` | `select-pk`, `update-range` | 1,000 | A, A', B | 5 | 240 every-row `UPDATE`s and 3,000 point `SELECT`s per path per arm | p2 |
| U2 | same | same | 1,000 | B, A', A | 5 | same | p3 |
| U3 | same | same | 1,000 | B, A, A' | 5 | same | p9 |
| Z200 | same | same | 200 | A, A', B | 3 | 600 `UPDATE`s, 3,000 `SELECT`s | p6 |
| Z10000 | same | same | 10,000 | A, A', B | 3 | 60 `UPDATE`s, 3,000 `SELECT`s | p7 |

**Row counts and flags.** `bm_scan.py --cells bn` is relation `s`, 10,000 rows `(id, v, w)`; `wide-h` is `wd`, 10,000 rows of 13 columns (`h decimal(30,4)`, one `varchar` that spills); `rows-200` and `rows-1k` are `s200` and `s1k`. `--scan-ops 192` is the scans per path per arm per run, `--point-ops 3000` the point statements. In `bn_census.py`, `--rows N` is both the table's size and the number of rows the every-row `UPDATE r SET v = ? WHERE id >= 1` writes; `--range-ops` is its statements per path per arm per run; `--scan-rows` and `--scan-ops` belong to the `scan` shape, which `--only select-pk,update-range` does not run.

**Start order.** The drivers start the servers in `--arm` order and rotate only the statement order. A server started last is a position the driver does not balance, so each shape has batches of both orders (S1 and S3 against S2; U1 against U2 and U3), and the tables give the groups separately and pooled.

## 2. The scan row: -3.3 ns on `bn`, -4.0 ns on `wide-h`, in 13 of 13 runs

Each group moves the same way, which is the result: B - A on `bn` literal is -3.2, -3.3 and -3.4 ns in S1, S2 and S3, and on `wide-h` -4.0, -4.0 and -3.9 ns, with different start orders and a host that went from load 15 to load 1 between S1 and S3. Net row, ns; B - A paired by run; the spread is |A' - A| per run (typical = median, largest = max).

| group | cell | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B below A | A' - A typical / largest \|gap\| |
|---|---|---|---|---|---|---|---|---|---|---|
| S1 | bn | literal | 5 | 31.0 ns | 31.9 ns | 27.3 ns | -3.2 [-3.8, -2.9] ns | -10.4 % | 5 of 5 | 0.56 / 1.40 ns |
| S2 | bn | literal | 3 | 30.7 ns | 31.4 ns | 27.5 ns | -3.3 [-5.5, -3.1] ns | -10.7 % | 3 of 3 | 1.29 / 2.21 ns |
| S3 | bn | literal | 5 | 31.2 ns | 31.0 ns | 27.4 ns | -3.4 [-4.2, -3.1] ns | -11.0 % | 5 of 5 | 0.22 / 0.49 ns |
| pooled | bn | literal | 13 | 31.0 ns | 31.4 ns | 27.4 ns | -3.3 [-5.5, -2.9] ns | -10.6 % | 13 of 13 | 0.45 / 2.21 ns |
| S1 | bn | prepared | 5 | 30.7 ns | 31.6 ns | 27.3 ns | -3.4 [-3.7, -2.6] ns | -10.9 % | 5 of 5 | 0.48 / 2.19 ns |
| S2 | bn | prepared | 3 | 30.6 ns | 31.6 ns | 28.1 ns | -3.1 [-5.5, -2.0] ns | -10.0 % | 3 of 3 | 1.24 / 2.33 ns |
| S3 | bn | prepared | 5 | 31.2 ns | 30.8 ns | 27.5 ns | -3.7 [-3.9, -3.0] ns | -12.0 % | 5 of 5 | 0.40 / 0.77 ns |
| pooled | bn | prepared | 13 | 30.9 ns | 31.3 ns | 27.5 ns | -3.5 [-5.5, -2.0] ns | -11.3 % | 13 of 13 | 0.48 / 2.33 ns |
| S1 | wide-h | literal | 5 | 35.8 ns | 35.7 ns | 31.8 ns | -4.0 [-11.0, -2.8] ns | -11.1 % | 5 of 5 | 0.44 / 2.46 ns |
| S2 | wide-h | literal | 3 | 34.5 ns | 34.9 ns | 30.5 ns | -4.0 [-4.2, -3.9] ns | -11.5 % | 3 of 3 | 0.24 / 0.63 ns |
| S3 | wide-h | literal | 5 | 34.6 ns | 34.6 ns | 30.7 ns | -3.9 [-4.6, -2.5] ns | -11.3 % | 5 of 5 | 0.15 / 0.26 ns |
| pooled | wide-h | literal | 13 | 34.7 ns | 34.9 ns | 30.9 ns | -4.0 [-11.0, -2.5] ns | -11.4 % | 13 of 13 | 0.24 / 2.46 ns |
| S1 | wide-h | prepared | 5 | 35.5 ns | 35.1 ns | 31.1 ns | -4.4 [-6.8, -3.9] ns | -12.4 % | 5 of 5 | 0.11 / 0.31 ns |
| S2 | wide-h | prepared | 3 | 34.7 ns | 34.8 ns | 31.1 ns | -3.5 [-4.7, -3.5] ns | -10.2 % | 3 of 3 | 0.54 / 1.20 ns |
| S3 | wide-h | prepared | 5 | 34.7 ns | 34.3 ns | 30.7 ns | -3.9 [-4.1, -3.0] ns | -11.3 % | 5 of 5 | 0.10 / 0.88 ns |
| pooled | wide-h | prepared | 13 | 34.7 ns | 34.7 ns | 30.9 ns | -4.0 [-6.8, -3.0] ns | -11.5 % | 13 of 13 | 0.14 / 1.20 ns |

The pooled tables, with rows/s derived as 1e9 / the median net row and every run, clean or not, included:


**literal path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | 10,000 | 13 | 31.0 [29.3, 36.0] ns | 31.4 [28.7, 36.4] ns | 27.4 [26.1, 32.1] ns | 32,303,253 /s | 36,497,321 /s | -3.3 [-5.5, -2.9] ns | -10.6 % | 13 of 13 | -0.1 [-2.2, +1.4] ns | 0.45 ns | 2.21 ns | -3.6 ns |
| wide-h | 10,000 | 13 | 34.7 [34.1, 67.4] ns | 34.9 [34.2, 65.0] ns | 30.9 [30.1, 56.5] ns | 28,784,732 /s | 32,410,674 /s | -4.0 [-11.0, -2.5] ns | -11.4 % | 13 of 13 | -0.0 [-2.5, +0.6] ns | 0.24 ns | 2.46 ns | -4.1 ns |

**prepared path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | 10,000 | 13 | 30.9 [29.8, 34.5] ns | 31.3 [30.2, 36.7] ns | 27.5 [26.4, 32.0] ns | 32,385,735 /s | 36,305,239 /s | -3.5 [-5.5, -2.0] ns | -11.3 % | 13 of 13 | +0.4 [-2.3, +2.2] ns | 0.48 ns | 2.33 ns | -3.7 ns |
| wide-h | 10,000 | 13 | 34.7 [34.1, 63.4] ns | 34.7 [34.2, 63.4] ns | 30.9 [30.1, 56.6] ns | 28,799,186 /s | 32,406,964 /s | -4.0 [-6.8, -3.0] ns | -11.5 % | 13 of 13 | -0.0 [-1.2, +0.5] ns | 0.14 ns | 1.20 ns | -4.0 ns |

Two `wide-h` windows and two `point:s` windows were marked unclean by the driver (p1b runs 1 and 2: a foreign task on the pinned CPUs or a sibling above 20 % busy after the one retry; section 9). Dropping the affected (run, cell) pairs moves `bn` by 0.1 ns and leaves `wide-h` where it was:


**literal path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | 10,000 | 12 | 31.1 [30.3, 36.0] ns | 31.4 [30.4, 36.4] ns | 27.4 [27.3, 32.1] ns | 32,154,986 /s | 36,462,127 /s | -3.4 [-5.5, -2.9] ns | -10.8 % | 12 of 12 | -0.1 [-2.2, +1.4] ns | 0.35 ns | 2.21 ns | -3.7 ns |
| wide-h | 10,000 | 12 | 34.7 [34.1, 36.1] ns | 34.9 [34.2, 35.8] ns | 30.8 [30.1, 32.4] ns | 28,799,439 /s | 32,417,706 /s | -3.9 [-4.9, -2.5] ns | -11.3 % | 12 of 12 | +0.0 [-0.4, +0.6] ns | 0.21 ns | 0.63 ns | -4.1 ns |

**prepared path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bn | 10,000 | 12 | 31.0 [30.4, 34.5] ns | 31.4 [30.5, 36.7] ns | 27.8 [27.1, 32.0] ns | 32,218,820 /s | 36,007,772 /s | -3.6 [-5.5, -2.0] ns | -11.5 % | 12 of 12 | +0.3 [-2.3, +2.2] ns | 0.63 ns | 2.33 ns | -3.5 ns |
| wide-h | 10,000 | 12 | 34.7 [34.1, 35.7] ns | 34.6 [34.2, 35.7] ns | 30.8 [30.1, 32.2] ns | 28,817,169 /s | 32,501,348 /s | -4.0 [-4.7, -3.0] ns | -11.4 % | 12 of 12 | -0.1 [-1.2, +0.5] ns | 0.17 ns | 1.20 ns | -4.0 ns |

The per-run net rows (S1a and S1b are the two files of group S1; S1b's run 1 is the contaminated `wide-h` window at 63-67 ns, in which B - A is still negative):


`bn` / literal, net row per run (ns):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| S1a | 0 | 36.0 ns | 36.4 ns | 32.1 ns | -3.8 ns | +0.4 ns |
| S1b | 0 | 31.4 ns | 32.4 ns | 28.5 ns | -2.9 ns | +1.0 ns |
| S1b | 1 | 30.5 ns | 31.9 ns | 27.3 ns | -3.2 ns | +1.4 ns |
| S1b | 2 | 29.3 ns | 28.7 ns | 26.1 ns | -3.2 ns | -0.6 ns |
| S1b | 3 | 31.0 ns | 31.2 ns | 27.3 ns | -3.6 ns | +0.2 ns |
| S2 | 0 | 30.3 ns | 31.6 ns | 27.3 ns | -3.1 ns | +1.3 ns |
| S2 | 1 | 33.6 ns | 31.4 ns | 28.1 ns | -5.5 ns | -2.2 ns |
| S2 | 2 | 30.7 ns | 30.6 ns | 27.5 ns | -3.3 ns | -0.1 ns |
| S3 | 0 | 30.5 ns | 30.5 ns | 27.3 ns | -3.3 ns | -0.1 ns |
| S3 | 1 | 31.2 ns | 31.0 ns | 27.4 ns | -3.8 ns | -0.2 ns |
| S3 | 2 | 31.6 ns | 31.4 ns | 28.1 ns | -3.4 ns | -0.2 ns |
| S3 | 3 | 32.2 ns | 31.7 ns | 27.9 ns | -4.2 ns | -0.5 ns |
| S3 | 4 | 30.5 ns | 30.4 ns | 27.4 ns | -3.1 ns | -0.1 ns |

`wide-h` / literal, net row per run (ns):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| S1a | 0 | 35.8 ns | 35.8 ns | 31.8 ns | -4.0 ns | -0.0 ns |
| S1b | 0 | 34.8 ns | 35.2 ns | 30.9 ns | -3.9 ns | +0.4 ns |
| S1b | 1 | 67.4 ns | 65.0 ns | 56.5 ns | -11.0 ns | -2.5 ns |
| S1b | 2 | 36.1 ns | 35.7 ns | 31.3 ns | -4.9 ns | -0.4 ns |
| S1b | 3 | 35.2 ns | 35.0 ns | 32.4 ns | -2.8 ns | -0.2 ns |
| S2 | 0 | 34.3 ns | 34.5 ns | 30.4 ns | -3.9 ns | +0.2 ns |
| S2 | 1 | 35.1 ns | 34.9 ns | 30.9 ns | -4.2 ns | -0.2 ns |
| S2 | 2 | 34.5 ns | 35.1 ns | 30.5 ns | -4.0 ns | +0.6 ns |
| S3 | 0 | 34.1 ns | 34.2 ns | 30.1 ns | -4.0 ns | +0.1 ns |
| S3 | 1 | 34.7 ns | 34.9 ns | 30.1 ns | -4.6 ns | +0.1 ns |
| S3 | 2 | 34.5 ns | 34.3 ns | 32.0 ns | -2.5 ns | -0.3 ns |
| S3 | 3 | 34.6 ns | 34.8 ns | 30.7 ns | -3.9 ns | +0.2 ns |
| S3 | 4 | 34.7 ns | 34.6 ns | 30.8 ns | -3.9 ns | -0.1 ns |

`bn` / prepared, net row per run (ns):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| S1a | 0 | 34.5 ns | 36.7 ns | 32.0 ns | -2.6 ns | +2.2 ns |
| S1b | 0 | 31.7 ns | 32.2 ns | 28.0 ns | -3.7 ns | +0.5 ns |
| S1b | 1 | 30.4 ns | 31.6 ns | 27.2 ns | -3.2 ns | +1.2 ns |
| S1b | 2 | 29.8 ns | 30.2 ns | 26.4 ns | -3.4 ns | +0.4 ns |
| S1b | 3 | 30.7 ns | 31.2 ns | 27.3 ns | -3.5 ns | +0.5 ns |
| S2 | 0 | 30.4 ns | 31.6 ns | 27.3 ns | -3.1 ns | +1.2 ns |
| S2 | 1 | 33.7 ns | 31.3 ns | 28.1 ns | -5.5 ns | -2.3 ns |
| S2 | 2 | 30.6 ns | 31.8 ns | 28.6 ns | -2.0 ns | +1.2 ns |
| S3 | 0 | 30.9 ns | 30.5 ns | 27.1 ns | -3.7 ns | -0.4 ns |
| S3 | 1 | 31.2 ns | 30.8 ns | 27.5 ns | -3.7 ns | -0.4 ns |
| S3 | 2 | 32.0 ns | 31.3 ns | 28.2 ns | -3.9 ns | -0.8 ns |
| S3 | 3 | 32.0 ns | 31.9 ns | 28.2 ns | -3.8 ns | -0.1 ns |
| S3 | 4 | 30.5 ns | 30.5 ns | 27.5 ns | -3.0 ns | +0.1 ns |

`wide-h` / prepared, net row per run (ns):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| S1a | 0 | 35.7 ns | 35.7 ns | 31.3 ns | -4.4 ns | +0.1 ns |
| S1b | 0 | 34.9 ns | 34.8 ns | 30.4 ns | -4.5 ns | -0.1 ns |
| S1b | 1 | 63.4 ns | 63.4 ns | 56.6 ns | -6.8 ns | +0.0 ns |
| S1b | 2 | 35.5 ns | 35.1 ns | 31.1 ns | -4.4 ns | -0.3 ns |
| S1b | 3 | 34.5 ns | 34.3 ns | 30.6 ns | -3.9 ns | -0.2 ns |
| S2 | 0 | 34.6 ns | 35.1 ns | 31.1 ns | -3.5 ns | +0.5 ns |
| S2 | 1 | 34.7 ns | 34.8 ns | 31.1 ns | -3.5 ns | +0.1 ns |
| S2 | 2 | 35.4 ns | 34.2 ns | 30.7 ns | -4.7 ns | -1.2 ns |
| S3 | 0 | 34.1 ns | 34.2 ns | 30.1 ns | -3.9 ns | +0.1 ns |
| S3 | 1 | 34.6 ns | 34.5 ns | 30.5 ns | -4.1 ns | -0.0 ns |
| S3 | 2 | 35.1 ns | 34.2 ns | 32.2 ns | -3.0 ns | -0.9 ns |
| S3 | 3 | 34.7 ns | 34.3 ns | 30.9 ns | -3.9 ns | -0.5 ns |
| S3 | 4 | 34.7 ns | 34.7 ns | 30.7 ns | -4.0 ns | +0.0 ns |

## 3. The guard cells: the every-row `UPDATE` is about 3 % cheaper, the point `SELECT` is level

The every-row `UPDATE` is the guard BM-S6 set (BM-Q12) and the cell BT-S4 changed (`ConjunctWellFormed` and its per-row calls deleted, `CheckConjuncts` once per statement). **Server CPU per statement, µs.** B - A paired by run; the spread is |A' - A| per run.

| group | shape | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B slower than A | A' - A typical / largest \|gap\| |
|---|---|---|---|---|---|---|---|---|---|---|
| U1 | select-pk | literal | 5 | 30.02 µs | 30.41 µs | 29.86 µs | -0.67 [-3.15, +0.55] µs | -2.22 % | 1 of 5 | 0.39 / 1.28 µs |
| U2 | select-pk | literal | 5 | 30.34 µs | 29.89 µs | 30.17 µs | +0.18 [-1.20, +1.49] µs | +0.60 % | 3 of 5 | 0.63 / 0.93 µs |
| U3 | select-pk | literal | 5 | 30.14 µs | 29.81 µs | 29.56 µs | -0.09 [-0.58, +0.94] µs | -0.29 % | 2 of 5 | 0.25 / 0.58 µs |
| pooled | select-pk | literal | 15 | 30.14 µs | 29.89 µs | 29.76 µs | -0.17 [-3.15, +1.49] µs | -0.55 % | 6 of 15 | 0.33 / 1.28 µs |
| U1 | select-pk | prepared | 5 | 28.86 µs | 28.02 µs | 27.96 µs | -0.17 [-2.50, +1.06] µs | -0.58 % | 2 of 5 | 0.59 / 0.84 µs |
| U2 | select-pk | prepared | 5 | 28.14 µs | 28.88 µs | 27.85 µs | -0.43 [-1.39, +1.78] µs | -1.54 % | 1 of 5 | 1.12 / 1.53 µs |
| U3 | select-pk | prepared | 5 | 28.26 µs | 28.19 µs | 28.28 µs | +0.34 [+0.02, +1.09] µs | +1.20 % | 5 of 5 | 0.41 / 1.95 µs |
| pooled | select-pk | prepared | 15 | 28.36 µs | 28.19 µs | 27.89 µs | +0.02 [-2.50, +1.78] µs | +0.05 % | 8 of 15 | 0.59 / 1.95 µs |
| U1 | update-range | literal | 5 | 782.7 µs | 796.2 µs | 762.9 µs | -15.8 [-53.2, +38.1] µs | -2.02 % | 2 of 5 | 14.97 / 15.64 µs |
| U2 | update-range | literal | 5 | 779.3 µs | 758.3 µs | 739.0 µs | -33.6 [-57.7, -5.5] µs | -4.31 % | 0 of 5 | 12.48 / 27.80 µs |
| U3 | update-range | literal | 5 | 767.7 µs | 770.3 µs | 755.1 µs | -12.6 [-28.6, -1.6] µs | -1.64 % | 0 of 5 | 10.12 / 20.89 µs |
| pooled | update-range | literal | 15 | 779.3 µs | 766.8 µs | 752.4 µs | -23.9 [-57.7, +38.1] µs | -3.07 % | 2 of 15 | 12.48 / 27.80 µs |
| U1 | update-range | prepared | 5 | 779.2 µs | 783.0 µs | 760.7 µs | +0.7 [-51.2, +16.9] µs | +0.10 % | 3 of 5 | 19.78 / 83.89 µs |
| U2 | update-range | prepared | 5 | 772.6 µs | 780.2 µs | 731.2 µs | -42.9 [-55.7, -27.3] µs | -5.55 % | 0 of 5 | 13.37 / 47.51 µs |
| U3 | update-range | prepared | 5 | 767.2 µs | 771.5 µs | 755.1 µs | -14.1 [-48.9, +2.5] µs | -1.83 % | 2 of 5 | 10.01 / 14.79 µs |
| pooled | update-range | prepared | 15 | 773.7 µs | 774.4 µs | 755.1 µs | -27.3 [-55.7, +16.9] µs | -3.53 % | 5 of 15 | 12.60 / 83.89 µs |

The pooled cells in full, with the statements/s derived (1e6 / the median server CPU) and the effect of the start order:

**Throughput, derived (1e6 / median server CPU per statement), 15 paired runs**

| shape | path | A | A' | B | B / A (CPU per statement) |
|---|---|---|---|---|---|
| select-pk | literal | 33,179 /s | 33,459 /s | 33,604 /s | 0.9873 |
| select-pk | prepared | 35,266 /s | 35,477 /s | 35,850 /s | 0.9837 |
| update-range | literal | 1,283 /s | 1,304 /s | 1,329 /s | 0.9656 |
| update-range | prepared | 1,293 /s | 1,291 /s | 1,324 /s | 0.9760 |

**Server CPU per statement (µs), median over 15 runs [min, max]; deltas paired by run**

| shape | path | A | A' | B | B - A | B - A' | A' - A | typical \|A' - A\| | largest \|A' - A\| | B slower than A | A' slower than A |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | 30.14 [26.77, 33.80] µs | 29.89 [26.67, 32.56] µs | 29.76 [27.16, 33.24] µs | -0.17 [-3.15, +1.49] µs (-0.55 %) | -0.12 [-1.87, +1.52] µs | -0.25 [-1.28, +0.81] µs | 0.33 µs | 1.28 µs | 6 of 15 | 3 of 15 |
| select-pk | prepared | 28.36 [25.96, 50.06] µs | 28.19 [27.05, 50.05] µs | 27.89 [26.36, 49.19] µs | +0.02 [-2.50, +1.78] µs (+0.05 %) | -0.04 [-1.67, +0.78] µs | -0.08 [-1.12, +1.95] µs | 0.59 µs | 1.95 µs | 8 of 15 | 7 of 15 |
| update-range | literal | 779.3 [760.6, 825.1] µs | 766.8 [757.1, 827.3] µs | 752.4 [726.1, 838.8] µs | -23.9 [-57.7, +38.1] µs (-3.07 %) | -22.5 [-65.3, +23.1] µs | +1.7 [-27.8, +20.9] µs | 12.48 µs | 27.80 µs | 2 of 15 | 8 of 15 |
| update-range | prepared | 773.7 [750.2, 853.2] µs | 774.4 [758.9, 893.0] µs | 755.1 [716.9, 826.0] µs | -27.3 [-55.7, +16.9] µs (-3.53 %) | -31.5 [-67.0, +12.2] µs | +4.3 [-47.5, +83.9] µs | 12.60 µs | 83.89 µs | 5 of 15 | 10 of 15 |

**By batch (server start order differs), median µs**

| batch | start order | shape | path | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|---|---|
| U1 | A,A2,B | select-pk | literal | 30.02 | 30.41 | 29.86 | -0.67 | -0.11 |
| U1 | A,A2,B | select-pk | prepared | 28.86 | 28.02 | 27.96 | -0.17 | +0.20 |
| U1 | A,A2,B | update-range | literal | 782.71 | 796.18 | 762.88 | -15.80 | +2.20 |
| U1 | A,A2,B | update-range | prepared | 779.23 | 783.01 | 760.72 | +0.74 | +3.78 |
| U2 | B,A2,A | select-pk | literal | 30.34 | 29.89 | 30.17 | +0.18 | -0.33 |
| U2 | B,A2,A | select-pk | prepared | 28.14 | 28.88 | 27.85 | -0.43 | -0.39 |
| U2 | B,A2,A | update-range | literal | 779.28 | 758.30 | 739.00 | -33.57 | -12.48 |
| U2 | B,A2,A | update-range | prepared | 772.57 | 780.22 | 731.20 | -42.85 | -13.37 |
| U3 | B,A,A2 | select-pk | literal | 30.14 | 29.81 | 29.56 | -0.09 | -0.25 |
| U3 | B,A,A2 | select-pk | prepared | 28.26 | 28.19 | 28.28 | +0.34 | -0.08 |
| U3 | B,A,A2 | update-range | literal | 767.71 | 770.30 | 755.11 | -12.61 | +3.04 |
| U3 | B,A,A2 | update-range | prepared | 767.16 | 771.49 | 755.08 | -14.05 | +10.01 |

Reading it. B - A on the `UPDATE` is negative in all three groups and both paths but one (U1 prepared, +0.7 µs): -15.8, -33.6, -12.6 µs literal and +0.7, -42.9, -14.1 µs prepared. The group in which A was started last (U2) reads the largest saving and the group in which B was started last (U1) the smallest, which is the direction a penalty on the last-started server would push; U3 (A' last, the quiet host) sits between and is the smallest on the prepared path after U1. The groups differ by more than their own typical gaps (10-20 µs), so the pooled median, which has all three orders, is the figure to carry, and the sizes below (both started B last) are the stricter evidence for the sign. **Against the spread:** the pooled B - A is 1.9 (literal) and 2.2 (prepared) times the typical A' - A gap and inside the largest (27.8 and 83.9 µs; the latter is one run, U1 run 0, in which A' read 893 µs); by the bar BM-Q12 set (the largest gap) the 1,000-row cell does not resolve the gain, by the typical gap it does, and the sizes in section 5 settle the sign. **The point `SELECT`** reads B - A = -0.17 µs literal and +0.02 µs prepared, 0.5 and 0.03 times the typical gap; B is slower than A in 6 and 8 of 15 runs and group U3 reads +0.34 µs (+1.2 %) in 5 of 5 prepared runs, a sign that U1 and U2 do not share, so the cell carries no direction. Every per-run end-state check (`range_v_ok`, `spill_txt_ok`: the final column values of every row) passed in all arms of all 15 runs, and the `UPDATE` cells were clean in every run; the four unclean windows in these files are point `SELECT` cells, and leaving them out moves the point `SELECT`'s pooled B - A to -0.09 and +0.02 µs.

Per run, `update-range`, server CPU per statement (µs):


`update-range` / literal, server CPU per statement per run (µs):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| U1 | 0 | 781.2 | 796.2 | 819.3 | +38.1 | +15.0 |
| U1 | 1 | 778.7 | 763.1 | 762.9 | -15.8 | -15.6 |
| U1 | 2 | 782.7 | 784.4 | 729.6 | -53.2 | +1.7 |
| U1 | 3 | 825.1 | 827.3 | 838.8 | +13.6 | +2.2 |
| U1 | 4 | 786.4 | 801.8 | 736.5 | -49.9 | +15.4 |
| U2 | 0 | 761.3 | 758.3 | 755.8 | -5.5 | -3.0 |
| U2 | 1 | 770.3 | 765.0 | 739.0 | -31.3 | -5.3 |
| U2 | 2 | 786.0 | 758.2 | 752.4 | -33.6 | -27.8 |
| U2 | 3 | 783.8 | 757.6 | 726.1 | -57.7 | -26.2 |
| U2 | 4 | 779.3 | 766.8 | 733.2 | -46.1 | -12.5 |
| U3 | 0 | 767.3 | 757.1 | 743.4 | -23.9 | -10.1 |
| U3 | 1 | 760.6 | 763.6 | 731.9 | -28.6 | +3.0 |
| U3 | 2 | 768.5 | 789.3 | 766.8 | -1.6 | +20.9 |
| U3 | 3 | 767.7 | 770.3 | 755.1 | -12.6 | +2.6 |
| U3 | 4 | 796.5 | 814.8 | 786.9 | -9.5 | +18.3 |

`update-range` / prepared, server CPU per statement per run (µs):

| batch | run | A | A' | B | B - A | A' - A |
|---|---|---|---|---|---|---|
| U1 | 0 | 809.1 | 893.0 | 826.0 | +16.9 | +83.9 |
| U1 | 1 | 757.2 | 758.9 | 760.7 | +3.5 | +1.7 |
| U1 | 2 | 779.2 | 783.0 | 728.0 | -51.2 | +3.8 |
| U1 | 3 | 773.7 | 805.9 | 774.4 | +0.7 | +32.2 |
| U1 | 4 | 780.3 | 760.5 | 734.1 | -46.2 | -19.8 |
| U2 | 0 | 831.6 | 799.2 | 789.7 | -41.9 | -32.3 |
| U2 | 1 | 758.5 | 771.1 | 731.2 | -27.3 | +12.6 |
| U2 | 2 | 853.2 | 805.7 | 800.9 | -52.3 | -47.5 |
| U2 | 3 | 772.6 | 759.2 | 716.9 | -55.7 | -13.4 |
| U2 | 4 | 772.1 | 780.2 | 729.3 | -42.9 | +8.1 |
| U3 | 0 | 767.2 | 771.5 | 718.3 | -48.9 | +4.3 |
| U3 | 1 | 750.2 | 760.2 | 736.1 | -14.1 | +10.0 |
| U3 | 2 | 759.6 | 774.4 | 760.9 | +1.3 | +14.8 |
| U3 | 3 | 776.7 | 767.0 | 779.2 | +2.5 | -9.7 |
| U3 | 4 | 777.4 | 789.6 | 755.1 | -22.3 | +12.2 |

## 4. The same answer (BT-R3, G2)

`bm_scan` records each arm's `ANALYZE` of one statement and a digest of its rows in run 0 of every file; the table is all of them. **No arm differs from another in the digest, `examined=`, `matched=`, `sel=` or `pages=`**, in any of the five `bm_scan` files, and `bad` (a reply that was not the expected tag) is 0 in every cell of every run, `bn_census`'s included.

| file | cell | arm | examined | matched | sel | pages | spills | row digest |
|---|---|---|---|---|---|---|---|---|
| p1a | bn | A | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1a | bn | A' | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1a | bn | B | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1a | wide-h | A | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p1a | wide-h | A' | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p1a | wide-h | B | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p1b | bn | A | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1b | bn | A' | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1b | bn | B | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p1b | wide-h | A | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p1b | wide-h | A' | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p1b | wide-h | B | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p4 | bn | A | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p4 | bn | A' | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p4 | bn | B | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p4 | wide-h | A | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p4 | wide-h | A' | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p4 | wide-h | B | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p8 | bn | A | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p8 | bn | A' | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p8 | bn | B | 10,000 rows | 1 row | 0 % | 61 pages | 0 | `1:6f3e98fc04fa4510` |
| p8 | wide-h | A | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p8 | wide-h | A' | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p8 | wide-h | B | 10,000 rows | 1 row | 0 % | 304 pages | 1 | `1:5fa29e54b8b3a1a4` |
| p5 | rows-200 | A | 200 rows | 1 row | 0 % | 2 pages | 0 | `1:c5f7e021c2bcf531` |
| p5 | rows-200 | A' | 200 rows | 1 row | 0 % | 2 pages | 0 | `1:c5f7e021c2bcf531` |
| p5 | rows-200 | B | 200 rows | 1 row | 0 % | 2 pages | 0 | `1:c5f7e021c2bcf531` |
| p5 | rows-1k | A | 1,000 rows | 1 row | 0 % | 7 pages | 0 | `1:2845109b97144221` |
| p5 | rows-1k | A' | 1,000 rows | 1 row | 0 % | 7 pages | 0 | `1:2845109b97144221` |
| p5 | rows-1k | B | 1,000 rows | 1 row | 0 % | 7 pages | 0 | `1:2845109b97144221` |

cells whose arms differ in counts or digest: 0

The counts B reports equal A's. The cells these drivers cannot see - the row-touch budget refusing on the same row, Waystone trails, Cabin recording, `sel=` on a non-zero selectivity - are the contract suites' (waystone, index, cabin, inner-build), **not run in this session**. The `bn_census` end-state checks (the final values of every row after the every-row `UPDATE`s, and the pages allocated by the spilled and unspilled update passes):

```
end-state checks (arm, check, value): count
  ('A', 'range_v_ok', True) 15
  ('A', 'spill_txt_ok', True) 15
  ('A2', 'range_v_ok', True) 15
  ('A2', 'spill_txt_ok', True) 15
  ('B', 'range_v_ok', True) 15
  ('B', 'spill_txt_ok', True) 15
run 0 pages-unspilled {"A": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}} pages-spilled {"A": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}
run 0 pages-unspilled {"B": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}, "A": {"updates": 1000, "pages_allocated_delta": 0}} pages-spilled {"B": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}, "A": {"updates": 1000, "pages_allocated_delta": 0}}
run 0 pages-unspilled {"B": {"updates": 1000, "pages_allocated_delta": 0}, "A": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}} pages-spilled {"B": {"updates": 1000, "pages_allocated_delta": 0}, "A": {"updates": 1000, "pages_allocated_delta": 0}, "A2": {"updates": 1000, "pages_allocated_delta": 0}}
```

## 5. The row-count sweep: at least 3.2 ns of the scan saving is per row, and the `UPDATE`'s is per row

The scan row at 200, 1,000 and 10,000 rows (4, 4 and 13 runs) and the `UPDATE` at 200, 1,000 and 10,000 rows (3, 15 and 3 runs). The scan saving per row is not smaller at the small sizes (5.9, 4.5 and 3.3 ns literal). Read as S(rows) = a + b x rows, the 10,000- and 1,000-row cells give b = 3.2 ns per row and a = 1.3 µs per statement; the 200-row cell (4 runs, largest |A' - A| 7.7 ns per row, 1.5 µs per statement) cannot separate a from zero. So 3.2 ns of the saving is per row, and a fixed part of up to about 1.3 µs per statement is possible and not established.

| group | cell | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B below A | A' - A typical / largest \|gap\| |
|---|---|---|---|---|---|---|---|---|---|---|
| Z | rows-200 | literal | 4 | 34.0 ns | 32.5 ns | 28.5 ns | -5.9 [-10.9, -2.1] ns | -17.3 % | 4 of 4 | 2.14 / 7.67 ns |
| Z | rows-200 | prepared | 4 | 34.0 ns | 32.7 ns | 28.8 ns | -5.7 [-10.3, -2.7] ns | -16.9 % | 4 of 4 | 1.81 / 4.01 ns |
| Z | rows-1k | literal | 4 | 33.1 ns | 30.9 ns | 27.3 ns | -4.5 [-7.8, -4.0] ns | -13.7 % | 4 of 4 | 0.93 / 3.92 ns |
| Z | rows-1k | prepared | 4 | 32.3 ns | 30.9 ns | 27.3 ns | -4.5 [-7.0, -3.1] ns | -13.9 % | 4 of 4 | 0.93 / 3.61 ns |

Net row detail for the two small sizes (the 10,000-row cell is section 2):


**literal path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 4 | 34.0 [32.3, 36.4] ns | 32.5 [27.4, 32.6] ns | 28.5 [24.1, 30.8] ns | 29,437,838 /s | 35,129,886 /s | -5.9 [-10.9, -2.1] ns | -17.3 % | 4 of 4 | -2.1 [-7.7, +0.3] ns | 2.14 ns | 7.67 ns | -2.8 ns |
| rows-1k | 1,000 | 4 | 33.1 [31.6, 34.4] ns | 30.9 [30.3, 34.7] ns | 27.3 [26.4, 30.4] ns | 30,194,017 /s | 36,659,226 /s | -4.5 [-7.8, -4.0] ns | -13.7 % | 4 of 4 | -0.9 [-3.9, +0.3] ns | 0.93 ns | 3.92 ns | -3.8 ns |

**prepared path**

| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \|A' - A\| | largest \|A' - A\| | B - A' |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rows-200 | 200 | 4 | 34.0 [33.1, 34.6] ns | 32.7 [29.1, 33.8] ns | 28.8 [22.8, 30.8] ns | 29,396,941 /s | 34,719,503 /s | -5.7 [-10.3, -2.7] ns | -16.9 % | 4 of 4 | -1.8 [-4.0, +0.3] ns | 1.81 ns | 4.01 ns | -3.9 ns |
| rows-1k | 1,000 | 4 | 32.3 [30.8, 33.8] ns | 30.9 [30.2, 34.6] ns | 27.3 [26.7, 29.0] ns | 30,922,065 /s | 36,647,675 /s | -4.5 [-7.0, -3.1] ns | -13.9 % | 4 of 4 | -0.3 [-3.6, +1.5] ns | 0.93 ns | 3.61 ns | -4.0 ns |

**The `UPDATE` per updated row.** B - A divided by the rows written: 200 rows 37 ns literal and 44 ns prepared (-7.4, -8.8 µs), 1,000 rows 24 and 27 ns, 10,000 rows 37 ns on both paths (-366, -367 µs). The 200- and 10,000-row cells have 3 runs each; the 10,000-row cell is noisy (B - A from -96 to -940 µs literal) and its sign is what it proves. The point `SELECT` does not scale with rows, and shows it: A reads 26.8 / 30.1 / 28.2 µs literal at 200 / 1,000 / 10,000 rows, with B - A between -0.5 and -0.2 µs at 200 and 10,000 rows, inside the spread.

| group | shape | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B slower than A | A' - A typical / largest \|gap\| |
|---|---|---|---|---|---|---|---|---|---|---|
| Z200 | select-pk | literal | 3 | 26.79 µs | 26.76 µs | 26.84 µs | -0.21 [-0.46, +1.69] µs | -0.78 % | 1 of 3 | 0.06 / 1.25 µs |
| Z1000 | select-pk | literal | 15 | 30.14 µs | 29.89 µs | 29.76 µs | -0.17 [-3.15, +1.49] µs | -0.55 % | 6 of 15 | 0.33 / 1.28 µs |
| Z10000 | select-pk | literal | 3 | 28.23 µs | 27.67 µs | 27.73 µs | -0.53 [-0.87, +0.55] µs | -1.89 % | 1 of 3 | 0.52 / 0.59 µs |
| Z200 | select-pk | prepared | 3 | 25.82 µs | 25.84 µs | 25.52 µs | -0.33 [-0.38, +1.04] µs | -1.26 % | 1 of 3 | 0.43 / 0.64 µs |
| Z1000 | select-pk | prepared | 15 | 28.36 µs | 28.19 µs | 27.89 µs | +0.02 [-2.50, +1.78] µs | +0.05 % | 8 of 15 | 0.59 / 1.95 µs |
| Z10000 | select-pk | prepared | 3 | 26.86 µs | 26.75 µs | 26.63 µs | -0.40 [-0.80, +0.17] µs | -1.48 % | 1 of 3 | 0.11 / 0.44 µs |
| Z200 | update-range | literal | 3 | 178.2 µs | 179.2 µs | 170.8 µs | -7.4 [-8.1, -1.3] µs | -4.14 % | 0 of 3 | 1.06 / 1.68 µs |
| Z1000 | update-range | literal | 15 | 779.3 µs | 766.8 µs | 752.4 µs | -23.9 [-57.7, +38.1] µs | -3.07 % | 2 of 15 | 12.48 / 27.80 µs |
| Z10000 | update-range | literal | 3 | 7590.2 µs | 7609.7 µs | 7117.9 µs | -365.7 [-940.3, -96.3] µs | -4.82 % | 0 of 3 | 35.21 / 71.88 µs |
| Z200 | update-range | prepared | 3 | 181.6 µs | 181.0 µs | 174.2 µs | -8.8 [-10.3, +0.0] µs | -4.86 % | 1 of 3 | 2.02 / 3.51 µs |
| Z1000 | update-range | prepared | 15 | 773.7 µs | 774.4 µs | 755.1 µs | -27.3 [-55.7, +16.9] µs | -3.53 % | 5 of 15 | 12.60 / 83.89 µs |
| Z10000 | update-range | prepared | 3 | 7556.7 µs | 7603.4 µs | 7277.0 µs | -367.2 [-433.9, -39.4] µs | -4.86 % | 0 of 3 | 98.14 / 98.66 µs |

## 6. Latency distributions

The scan statement as the driver sees it (wall time, client to reply), pooled over the clean windows of the 13 runs; linear-interpolated percentiles; scans/s at p50 is derived (1e6 / p50). The body falls with the row cost (`bn` literal: p0 347.0 to 315.5 µs, p25 364.4 to 330.0, p50 375.3 to 340.0: 31-35 µs, which brackets the 33 µs that 3.3 ns x 10,000 rows predicts); **the p99 of 2,300-2,500 samples is about the 25th-worst and carries host hiccups**, not code. The two unclean `wide-h` windows are left out of this table; with them the `wide-h` literal p90-p99 read 590-1,757 µs for A.

| cell | path | arm | n (scans, all runs) | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 2,496 | 347.0 µs | 364.4 µs | 375.3 µs | 411.5 µs | 446.2 µs | 602.3 µs | 2,665 /s |
| bn | literal | A' | 2,496 | 347.7 µs | 366.3 µs | 376.9 µs | 406.1 µs | 430.6 µs | 590.6 µs | 2,653 /s |
| bn | literal | B | 2,496 | 315.5 µs | 330.0 µs | 340.0 µs | 372.1 µs | 408.2 µs | 503.0 µs | 2,941 /s |
| bn | prepared | A | 2,496 | 342.7 µs | 360.5 µs | 371.3 µs | 409.5 µs | 443.1 µs | 644.9 µs | 2,693 /s |
| bn | prepared | A' | 2,496 | 345.4 µs | 362.2 µs | 373.2 µs | 409.1 µs | 446.7 µs | 588.8 µs | 2,680 /s |
| bn | prepared | B | 2,496 | 312.9 µs | 325.9 µs | 336.9 µs | 369.7 µs | 404.4 µs | 499.1 µs | 2,968 /s |
| wide-h | literal | A | 2,304 | 398.5 µs | 417.1 µs | 428.4 µs | 470.3 µs | 539.0 µs | 831.2 µs | 2,334 /s |
| wide-h | literal | A' | 2,304 | 398.1 µs | 418.0 µs | 428.3 µs | 464.0 µs | 549.3 µs | 834.1 µs | 2,335 /s |
| wide-h | literal | B | 2,304 | 359.4 µs | 376.7 µs | 388.3 µs | 435.1 µs | 520.8 µs | 767.6 µs | 2,575 /s |
| wide-h | prepared | A | 2,304 | 393.8 µs | 413.5 µs | 424.4 µs | 464.8 µs | 540.7 µs | 840.5 µs | 2,356 /s |
| wide-h | prepared | A' | 2,304 | 393.7 µs | 413.3 µs | 423.6 µs | 457.2 µs | 501.4 µs | 823.0 µs | 2,361 /s |
| wide-h | prepared | B | 2,304 | 354.3 µs | 374.0 µs | 384.5 µs | 424.4 µs | 489.4 µs | 791.1 µs | 2,601 /s |

The every-row `UPDATE` and the point `SELECT` (percentiles are the median over runs of each run's percentile; n is the statements pooled):

| shape | path | arm | n (statements, all runs) | p0 | p25 | p50 | p90 | p99 | statements/s at p50 (derived) |
|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 45,000 | 53.2 µs | 55.7 µs | 58.4 µs | 73.1 µs | 102.1 µs | 17,115 /s |
| select-pk | literal | A' | 45,000 | 52.9 µs | 55.9 µs | 58.4 µs | 70.9 µs | 100.9 µs | 17,118 /s |
| select-pk | literal | B | 45,000 | 52.3 µs | 55.4 µs | 58.0 µs | 71.4 µs | 100.9 µs | 17,230 /s |
| select-pk | prepared | A | 45,000 | 50.4 µs | 52.5 µs | 54.6 µs | 66.3 µs | 89.7 µs | 18,331 /s |
| select-pk | prepared | A' | 45,000 | 50.2 µs | 52.6 µs | 54.6 µs | 66.2 µs | 90.6 µs | 18,328 /s |
| select-pk | prepared | B | 45,000 | 50.1 µs | 52.3 µs | 53.7 µs | 66.6 µs | 94.4 µs | 18,618 /s |
| update-range | literal | A | 3,600 | 727.0 µs | 769.0 µs | 785.6 µs | 894.7 µs | 1334.0 µs | 1,273 /s |
| update-range | literal | A' | 3,600 | 730.2 µs | 764.7 µs | 782.0 µs | 900.2 µs | 2064.9 µs | 1,279 /s |
| update-range | literal | B | 3,600 | 694.8 µs | 740.8 µs | 758.2 µs | 873.6 µs | 2179.4 µs | 1,319 /s |
| update-range | prepared | A | 3,600 | 722.4 µs | 761.2 µs | 777.1 µs | 895.2 µs | 2043.0 µs | 1,287 /s |
| update-range | prepared | A' | 3,600 | 720.3 µs | 760.0 µs | 778.9 µs | 902.6 µs | 2065.2 µs | 1,284 /s |
| update-range | prepared | B | 3,600 | 686.4 µs | 734.7 µs | 762.3 µs | 863.7 µs | 1972.6 µs | 1,312 /s |

(percentiles are the median over runs of each run's percentile; n is the statements pooled)

B's `UPDATE` body sits 15-36 µs below A's from p0 to p50 (p0 -32 / -36 µs, p25 -28 / -27, p50 -27 / -15, literal / prepared) and its p90 21-32 µs below; the p99 (1,300-2,200 µs) is not ordered by arm. The point `SELECT` shows no ordering at any percentile.

## 7. Waits

A latency is a sum. The scan statement is 75-83 % per-row server work (rows x net row), 27-30 µs the fixed statement cost (a point `SELECT`), and the rest is the client and socket round trip; the table is medians over all runs (the per-row term is 10,000 x the pooled median net row; the last-but-one column is the wall mean minus the server CPU, so it carries the tail).

| cell | path | arm | wall mean | per-row work (rows x net row) | fixed statement (point SELECT server CPU) | client + socket (wall mean - scan server CPU) | per-row share of wall | driver CPU |
|---|---|---|---|---|---|---|---|---|
| bn | literal | A | 382.7 µs | 309.6 µs | 28.0 µs | 42.9 µs | 81 % | 44.0 µs |
| bn | literal | A' | 381.0 µs | 313.6 µs | 27.8 µs | 39.2 µs | 82 % | 44.7 µs |
| bn | literal | B | 345.0 µs | 274.0 µs | 28.1 µs | 40.7 µs | 79 % | 43.8 µs |
| bn | prepared | A | 380.1 µs | 308.8 µs | 26.6 µs | 41.8 µs | 81 % | 41.7 µs |
| bn | prepared | A' | 378.5 µs | 313.3 µs | 26.7 µs | 36.8 µs | 83 % | 42.6 µs |
| bn | prepared | B | 342.2 µs | 275.4 µs | 26.8 µs | 34.6 µs | 80 % | 41.7 µs |
| wide-h | literal | A | 439.8 µs | 347.4 µs | 30.3 µs | 62.2 µs | 79 % | 76.8 µs |
| wide-h | literal | A' | 443.4 µs | 348.6 µs | 30.1 µs | 64.7 µs | 79 % | 76.5 µs |
| wide-h | literal | B | 409.0 µs | 308.5 µs | 30.0 µs | 70.2 µs | 75 % | 76.0 µs |
| wide-h | prepared | A | 438.2 µs | 347.2 µs | 29.4 µs | 61.6 µs | 79 % | 74.2 µs |
| wide-h | prepared | A' | 436.1 µs | 346.9 µs | 29.1 µs | 60.1 µs | 80 % | 74.4 µs |
| wide-h | prepared | B | 398.8 µs | 308.6 µs | 29.0 µs | 61.2 µs | 77 % | 74.0 µs |

wait counters, summed over every run and batch (scan cells; then point cells):

| kind | cell | path | arm | lookups | lookups_instance | window_wait_us | pool_misses | bad |
|---|---|---|---|---|---|---|---|---|
| scan | bn | literal | A | 0 | 0 | 0 | 0 | 0 |
| scan | bn | literal | A' | 0 | 0 | 0 | 0 | 0 |
| scan | bn | literal | B | 0 | 0 | 0 | 0 | 0 |
| scan | bn | prepared | A | 0 | 0 | 0 | 0 | 0 |
| scan | bn | prepared | A' | 0 | 0 | 0 | 0 | 0 |
| scan | bn | prepared | B | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | literal | A | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | literal | A' | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | literal | B | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | prepared | A | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | prepared | A' | 0 | 0 | 0 | 0 | 0 |
| scan | wide-h | prepared | B | 0 | 0 | 0 | 0 | 0 |
| point | point:s | literal | A | 0 | 0 | 0 | 2499 | 0 |
| point | point:s | literal | A' | 0 | 0 | 0 | 2514 | 0 |
| point | point:s | literal | B | 0 | 0 | 0 | 2499 | 0 |
| point | point:s | prepared | A | 0 | 0 | 0 | 1439 | 0 |
| point | point:s | prepared | A' | 0 | 0 | 0 | 1424 | 0 |
| point | point:s | prepared | B | 0 | 0 | 0 | 1439 | 0 |
| point | point:wd | literal | A | 0 | 0 | 0 | 0 | 0 |
| point | point:wd | literal | A' | 0 | 0 | 0 | 0 | 0 |
| point | point:wd | literal | B | 0 | 0 | 0 | 0 | 0 |
| point | point:wd | prepared | A | 0 | 0 | 0 | 0 | 0 |
| point | point:wd | prepared | A' | 0 | 0 | 0 | 0 | 0 |
| point | point:wd | prepared | B | 0 | 0 | 0 | 0 | 0 |

The client-and-socket term is level across arms on `bn` (34.6-42.9 µs) and moves between arms on `wide-h` literal (A 62.2, A' 64.7, B 70.2 µs) without a pattern on the prepared path (61.6, 60.1, 61.2 µs): it carries the tail of the wall mean, which the per-row term does not, so on the literal path B's `wide-h` wall mean falls by 30.8 µs while its per-row term falls by 38.9 (prepared: 39.4 and 38.6). The terms close to the wall mean by construction here (the client term is wall mean minus server CPU), not as an independent partition.

| wait type | this engine at this configuration |
|---|---|
| durability / commit (fsync) | not applicable to a scan; **the every-row `UPDATE` ran under `relaxed` only**: no commit wait was measured, so `group` and `strict`, which add a commit wait to the statement, are unmeasured, and a 24-27 µs saving would be a smaller share of such a statement |
| write statement | the every-row `UPDATE` is write work: server CPU is 98-100 % of its wall p50 (table below); the driver's 5 % overlaps it. The split of the 750-780 µs between the walk, the per-row write and the MVCC bookkeeping needs a profile and was not taken |
| read | the per-row term above. `pool_misses` = 0 in every scan cell (every page resident; the point cells' first-touch misses are equal in the three arms, counters above); `lookups` = 0 and `lookups_instance` = 0 in every cell: no row took a commit-window lookup, so every `Classify` ran on the floor path |
| client and socket round trip | 35-43 µs of the `bn` scan's wall mean (9-11 %), 60-70 µs of `wide-h`'s; the driver's own CPU is 42-45 and 74-77 µs of it and overlaps the server's |
| lock or conflict wait | none: `window_wait_us` = 0 in every cell (one connection, no writer) |

The `UPDATE` and point `SELECT` decomposition (wall p50 against server and driver CPU; the three overlap, so the last column is negative):

| shape | path | arm | wall p50 per statement | server CPU | driver CPU | wall p50 - server CPU - driver CPU (socket, wake-up, scheduling) |
|---|---|---|---|---|---|---|
| select-pk | literal | A | 58.4 µs | 30.1 µs (52 %) | 33.3 µs (57 %) | -5.0 µs (-9 %) |
| select-pk | literal | A' | 58.4 µs | 29.9 µs (51 %) | 33.4 µs (57 %) | -4.8 µs (-8 %) |
| select-pk | literal | B | 58.0 µs | 29.8 µs (51 %) | 33.1 µs (57 %) | -4.8 µs (-8 %) |
| select-pk | prepared | A | 54.6 µs | 28.4 µs (52 %) | 30.3 µs (56 %) | -4.1 µs (-7 %) |
| select-pk | prepared | A' | 54.6 µs | 28.2 µs (52 %) | 30.3 µs (56 %) | -3.9 µs (-7 %) |
| select-pk | prepared | B | 53.7 µs | 27.9 µs (52 %) | 30.4 µs (57 %) | -4.6 µs (-9 %) |
| update-range | literal | A | 785.6 µs | 779.3 µs (99 %) | 41.8 µs (5 %) | -35.4 µs (-5 %) |
| update-range | literal | A' | 782.0 µs | 766.8 µs (98 %) | 42.8 µs (5 %) | -27.6 µs (-4 %) |
| update-range | literal | B | 758.2 µs | 752.4 µs (99 %) | 41.8 µs (6 %) | -36.0 µs (-5 %) |
| update-range | prepared | A | 777.1 µs | 773.7 µs (100 %) | 37.1 µs (5 %) | -33.7 µs (-4 %) |
| update-range | prepared | A' | 778.9 µs | 774.4 µs (99 %) | 39.6 µs (5 %) | -35.1 µs (-5 %) |
| update-range | prepared | B | 762.3 µs | 755.1 µs (99 %) | 38.4 µs (5 %) | -31.2 µs (-4 %) |

## 8. Against BT-S1's prediction, the engine's last numbers, and the floor

**What BT-S1 predicted and what B delivers.** BT-S1 priced the order by ablation at `eeb88f47` (`results-bt-s1-premise-v2.7.0-992-geeb88f47.md`; savings are paired medians against that run's A, 5 runs). The real change is one number, and it can be read against the ablations only as an enclosure:

| BT-S1 ablation | saved, literal | saved, prepared | built in B? | what B delivers against it |
|---|---|---|---|---|
| D2 (#5, the tuple read) | 3.1 ns | 3.3 ns | yes: `TryReadTupleIn`, BT-S2 | B is 106 % of D2 on both paths |
| D1 (#1, #2, #4, row invariants) | 2.6 ns | 2.5 ns | #1 deleted, #4 under the span on the kept paths, #2 as one lookup rather than a hoist (BT-S3) | not separable from D2 here |
| D3 (#7, #8, counters and charge) | 3.2 ns | 3.2 ns | **no**: BT-R3 keeps the counts and #8's bool form generated the same code (BT-S3) | - |
| D-all (D1 + D2 + D3) | 7.1 ns | 8.1 ns | partly | B: 3.3 / 3.5 ns = **46 % / 43 %** |
| D4 (the walk rejects first) | 7.9 ns | 8.8 ns | no: forbidden by BT-R1 | B is 42 % / 40 % |
| **B, `4274ed81`, this run (13 runs)** | **3.3 ns** [2.9, 5.5] | **3.5 ns** [2.0, 5.5] | - | - |

The ceiling was a ceiling: BT-S1 said D-all "is a ceiling for S2 + S3, not a forecast", and the order's own bar (the whole order passes at 3.0 ns) is met by 0.3 and 0.5 ns. B reaches the tuple read's saving and not the counts' (D3 removed work BT-R3 keeps). The remaining distance to D-all is 3.8 / 4.6 ns (7.1 - 3.3, 8.1 - 3.5), to D4 4.6 / 5.3.

**Against the engine's last numbers.** The baseline for this shape is BT-S1's A, which is this run's A byte for byte (`833501d0`); its A read 1.0 and 1.5 ns higher about two hours earlier. That difference is a session effect and **not a finding**: the same binary moved by more than the typical A-against-A' gap between two sessions (BM-S6's results saw the same `bn` baseline move 4 ns between sessions).

| `bn` net row | A, BT-S1 (`eeb88f47`, 5 runs) | A, this run | B, this run | D-all, BT-S1 | B, BM close (`6b771ec1`, 8 runs) | PostgreSQL 18.6 (BM close, not re-measured) |
|---|---|---|---|---|---|---|
| literal | 32.0 ns [31.3, 33.1] (31.2 M rows/s) | 31.0 ns [29.3, 36.0] (32.3 M rows/s) | **27.4 ns** [26.1, 32.1] (36.5 M rows/s) | 24.7 ns (40.5 M rows/s) | 31.0 ns [30.8, 32.6] (32.3 M rows/s) | 29.1 ns [28.6, 29.5] (34.4 M rows/s) |
| prepared | 32.4 ns [31.5, 34.5] (30.8 M rows/s) | 30.9 ns [29.8, 34.5] (32.4 M rows/s) | **27.5 ns** [26.4, 32.0] (36.3 M rows/s) | 24.5 ns (40.7 M rows/s) | 31.2 ns [30.8, 32.5] (32.1 M rows/s) | not measured (simple protocol only) |

| `wide-h` net row | A, this run | B, this run | B, BM close (`6b771ec1`) | PostgreSQL 18.6 (BM close, not re-measured) |
|---|---|---|---|---|
| literal | 34.7 ns [34.1, 67.4] (28.8 M rows/s) | **30.9 ns** [30.1, 56.5] (32.4 M rows/s) | 35.6 ns [34.9, 37.2] | 68.5 ns [67.7, 75.8] |
| prepared | 34.7 ns [34.1, 63.4] (28.8 M rows/s) | **30.9 ns** [30.1, 56.6] (32.4 M rows/s) | 35.6 ns [34.7, 36.6] | not measured |

(The 56-67 ns maxima are the one contaminated window of section 9; the medians are not affected.) B is 0.94 times PostgreSQL's `bn` figure and 0.45 times its `wide-h` figure, A 1.07 and 0.51: the engine is below the floor on these shapes. **The floor was not measured in this run**, and a result is neither good because it beat PostgreSQL nor bad because it did not. B's `bn` figure is 3.6 ns (11.6 %) under the BM close's B, from a different session and a different commit chain; read that as the direction BT-S1's A already showed, not as a delta of its own.

**The `UPDATE` and `SELECT` against the last baseline for the shape** (`results-bm-s6-remedy-v2.7.0-948-g952c67e5.md`: B2 `952c67e5`, 15 runs, server CPU per statement). The sessions are not comparable in absolute terms (A reads 779.3 and 773.7 µs here against 737.0 and 738.3 there, +42 and +35 µs for the same kind of arm), so only the paired gap carries:

| shape | B2 - A, BM-S6 | B - A, this run | A' - A typical / largest, BM-S6 | A' - A typical / largest, this run |
|---|---|---|---|---|
| every-row `UPDATE`, 1,000 rows, literal | +8.9 µs (+1.2 %) | **-23.9 µs (-3.1 %)** | 14.7 / 41.8 µs | 12.5 / 27.8 µs |
| every-row `UPDATE`, 1,000 rows, prepared | +3.2 µs (+0.4 %) | **-27.3 µs (-3.5 %)** | 7.5 / 28.9 µs | 12.6 / 83.9 µs |
| point `SELECT`, literal | +0.15 µs | -0.17 µs | 0.08 / 0.35 µs | 0.33 / 1.28 µs |
| point `SELECT`, prepared | +0.43 µs | +0.02 µs | 0.17 / 0.58 µs | 0.59 / 1.95 µs |

## 9. The noise floor and the host

**The control, inside the run.** A' is A's bytes; their per-run difference on the net row is the spread. On `bn` it is -0.1 / +0.4 ns at the median (typical 0.45 / 0.48, largest 2.21 / 2.33 ns, literal / prepared), on `wide-h` 0.0 / 0.0 (typical 0.24 / 0.14, largest 2.46 / 1.20); the largest values are single runs and the smallest per-run B - A on `bn` (-2.0 ns, prepared, in S2) is inside the largest gap while the median is 1.5 times above it. On the `UPDATE` the typical gap is 12.5 / 12.6 µs and the largest 27.8 / 83.9; on the point `SELECT` 0.33 / 0.59 and 1.28 / 1.95 µs. **A delta under the typical gap is not reported as a finding**: the point `SELECT`'s B - A is under it at every size; the 1,000-row `UPDATE` is reported with its position effect and its size sweep (sections 3 and 5) because it is 1.9-2.2 times the typical gap and has the same sign and a per-row size of the same order at 200 and 10,000 rows.

**The host was not quiet, and the gates saw it.** At 08:48, right after B's build, `ps` showed another session's `cc1plus` jobs on six CPUs and another session's test binary on CPU 2; the 1-minute load average stayed at 4-16 until 09:21 and fell to 0.7-2.3 from 09:24, with a rise to 7.0 near the end of p9 (its run 4 has one unclean point `SELECT` window). Sampled every 10 s into `hostwatch.log` (`competing` is the count of processes whose command line matches `cc1plus|cmake --build|ctest`; it is at least 3 in every sample because other sessions' shells carry those words, so it is not a count of running builds):

| phase | window (UTC) | samples | 1-min load min / median / max | samples with a competing build or test (pgrep count > 0) |
|---|---|---|---|---|
| p1 | 08:49:37-08:55:29 | 36 | 7.11 / 12.86 / 14.99 | 36 of 36 |
| p1b | 08:55:42-09:05:38 | 59 | 4.47 / 13.35 / 15.82 | 59 of 59 |
| p2 | 09:05:38-09:13:28 | 47 | 4.33 / 9.65 / 10.44 | 47 of 47 |
| p3 | 09:13:28-09:21:41 | 49 | 7.88 / 10.20 / 11.66 | 49 of 49 |
| p4 | 09:21:41-09:23:56 | 14 | 1.64 / 3.40 / 7.39 | 14 of 14 |
| p5 | 09:23:56-09:25:28 | 9 | 1.16 / 1.37 / 1.95 | 9 of 9 |
| p6 | 09:25:28-09:26:32 | 6 | 0.75 / 0.91 / 1.11 | 6 of 6 |
| p7 | 09:26:32-09:28:13 | 11 | 1.23 / 1.60 / 2.18 | 11 of 11 |
| p8 | 09:28:15-09:30:11 | 12 | 0.83 / 1.02 / 1.45 | 12 of 12 |
| p9 | 09:30:11-09:34:50 | 28 | 1.10 / 2.34 / 7.03 | 28 of 28 |

whole watch: 272 samples, 08:49:37-09:34:46, load min 0.75 median 9.41 max 15.82

The run-level load gate (`--quiet-load 2`) did not open for the 1-minute average in p1a-p3 and the first two runs of p4: it waited 180 s in p1a's run 0 (load at release 7.11) and the 30 s cap in every run after it (load at release 2.7-13.1), and released by itself (0-25 s, load 0.7-1.9) from p4's third run on. The per-cell gate, which measures the pinned CPUs and their siblings rather than the load average, did what it is for: **the driver's clean test (siblings at most 20 % busy, a foreign task at most 15 % of the pinned CPUs) passed in all but 8 of the (run, cell, path) windows, and each of those was retried once and kept with `clean = False`**: p1b run 1 `wide-h` (both paths: a sibling 99.3 % busy, a foreign task on 80.5 % of the driver's CPU), p1b run 2 `point:s` (both paths, a sibling 51.2 % busy), and four point `SELECT` windows in the `bn_census` files (p2 run 0, p3 runs 1 and 2, p9 run 4). The `UPDATE` windows were all clean. The driver's `competing` flag was true in every cell, as the `hostwatch` count shows, so it does not discriminate here. The per-run records follow.

Scan files (S1 = p1a + p1b, S2 = p4, S3 = p8; the arm order of each run is in the JSON):


batch S1: arm order per run: [['A', 'A2', 'B']]
| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 180 s | 7.11 | 14.91 | 8 | 8 of 8 | 2 | 68 s | 14.34-14.91 | 17.9 % | 1.2 / 5.9 % | True | 4688/4322/4473 |

batch S1: arm order per run: [['A', 'A2', 'B'], ['A', 'A2', 'B']]
| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 30 s | 12.58 | 12.45 | 8 | 8 of 8 | 1 | 52 s | 10.68-12.45 | 4.2 % | 1.8 / 2.2 % | True | 5936/7206/7153 |
| 1 | 30 s | 6.59 | 5.13 | 8 | 6 of 8 | 2 | 64 s | 4.72-11.13 | 99.3 % | 39.0 / 80.5 % | True | 4308/4399/4701 |
| 2 | 30 s | 13.13 | 15.25 | 8 | 6 of 8 | 2 | 120 s | 14.55-15.25 | 51.2 % | 4.0 / 6.0 % | True | 6951/6322/6235 |
| 3 | 30 s | 12.47 | 11.16 | 8 | 8 of 8 | 1 | 28 s | 9.30-11.16 | 13.8 % | 5.2 / 3.7 % | True | 5468/4995/5323 |

batch S2: arm order per run: [['B', 'A2', 'A'], ['B', 'A2', 'A']]
| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 30 s | 4.93 | 4.12 | 8 | 8 of 8 | 1 | 2 s | 3.64-4.12 | 3.7 % | 2.2 / 1.8 % | True | 5193/4708/4281 |
| 1 | 30 s | 2.67 | 2.37 | 8 | 8 of 8 | 1 | 2 s | 2.01-2.37 | 4.5 % | 3.0 / 4.2 % | True | 4939/4718/4328 |
| 2 | 5 s | 1.85 | 1.64 | 8 | 8 of 8 | 1 | 2 s | 1.64-1.75 | 6.3 % | 4.4 / 5.0 % | True | 5194/4233/4563 |

batch S3: arm order per run: [['A', 'A2', 'B'], ['A', 'A2', 'B']]
| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 s | 1.44 | 1.33 | 8 | 8 of 8 | 1 | 2 s | 1.20-1.33 | 2.7 % | 1.3 / 1.3 % | True | 4741/4140/4781 |
| 1 | 0 s | 1.20 | 1.01 | 8 | 8 of 8 | 1 | 2 s | 0.93-1.01 | 1.9 % | 1.8 / 1.9 % | True | 4583/3977/5784 |
| 2 | 0 s | 0.93 | 0.94 | 8 | 8 of 8 | 1 | 2 s | 0.86-0.94 | 5.0 % | 2.4 / 1.5 % | True | 5302/5231/5004 |
| 3 | 0 s | 0.88 | 0.97 | 8 | 8 of 8 | 1 | 2 s | 0.96-1.05 | 8.2 % | 3.2 / 2.7 % | True | 4371/4346/4358 |
| 4 | 0 s | 0.96 | 0.82 | 8 | 8 of 8 | 1 | 2 s | 0.82-0.84 | 2.0 % | 1.8 / 2.3 % | True | 4598/4473/5059 |

`bn_census` files (U1 = p2, U2 = p3, U3 = p9):


batch U1 (start order A,A2,B):

| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 30 s | 6.15 | 6.15 / 10.41 | 4 | 3 of 4 | 2 | 72 s | 10.07-10.36 | 34.6 % | 4.4 / 7.0 % | 15.2 [38.4] | True |
| 1 | 30 s | 9.74 | 9.74 / 10.05 | 4 | 4 of 4 | 1 | 10 s | 9.51-10.06 | 15.0 % | 3.0 / 5.3 % | 12.1 [24.1] | True |
| 2 | 30 s | 9.38 | 9.38 / 7.79 | 4 | 4 of 4 | 1 | 2 s | 7.79-8.54 | 4.7 % | 1.6 / 5.5 % | 8.2 [22.1] | True |
| 3 | 30 s | 4.94 | 4.94 / 10.47 | 4 | 4 of 4 | 2 | 62 s | 4.07-10.34 | 11.6 % | 4.6 / 5.4 % | 11.8 [40.1] | True |
| 4 | 30 s | 10.04 | 10.04 / 10.78 | 4 | 4 of 4 | 1 | 4 s | 10.09-10.31 | 13.7 % | 6.8 / 7.4 % | 16.5 [28.1] | True |

batch U2 (start order B,A2,A):

| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 30 s | 10.05 | 10.05 / 9.75 | 4 | 4 of 4 | 2 | 16 s | 9.75-10.51 | 9.8 % | 1.5 / 3.6 % | 10.9 [32.5] | True |
| 1 | 30 s | 8.69 | 8.69 / 11.07 | 4 | 3 of 4 | 2 | 120 s | 7.88-10.54 | 50.7 % | 3.7 / 3.7 % | 14.3 [25.2] | True |
| 2 | 30 s | 11.66 | 11.66 / 11.49 | 4 | 3 of 4 | 2 | 22 s | 10.97-11.27 | 35.5 % | 3.2 / 6.0 % | 10.6 [34.0] | True |
| 3 | 30 s | 10.79 | 10.79 / 11.47 | 4 | 4 of 4 | 1 | 4 s | 10.62-11.07 | 11.1 % | 9.2 / 5.4 % | 13.9 [22.6] | True |
| 4 | 30 s | 10.66 | 10.66 / 7.94 | 4 | 4 of 4 | 1 | 2 s | 7.94-8.64 | 1.9 % | 2.7 / 5.2 % | 7.4 [21.8] | True |

batch U3 (start order B,A,A2):

| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 s | 0.84 | 0.84 / 2.69 | 4 | 4 of 4 | 1 | 24 s | 2.32-2.69 | 2.6 % | 3.2 / 2.2 % | 6.0 [21.9] | True |
| 1 | 25 s | 1.90 | 1.90 / 1.36 | 4 | 4 of 4 | 1 | 2 s | 1.36-1.48 | 5.8 % | 2.7 / 3.4 % | 7.3 [24.0] | True |
| 2 | 0 s | 1.36 | 1.36 / 1.57 | 4 | 4 of 4 | 1 | 2 s | 1.18-1.20 | 1.6 % | 1.8 / 3.9 % | 6.7 [23.8] | True |
| 3 | 0 s | 1.57 | 1.57 / 1.41 | 4 | 4 of 4 | 1 | 2 s | 1.40-1.53 | 1.6 % | 2.4 / 3.6 % | 6.9 [23.7] | True |
| 4 | 0 s | 1.41 | 1.41 / 6.24 | 4 | 3 of 4 | 2 | 120 s | 1.17-6.70 | 50.0 % | 4.0 / 3.1 % | 7.9 [21.4] | True |

bad replies: 0

The size cells' host records (p5: scan sizes; p6, p7: `UPDATE` sizes). Their loads were 0.7-2.0 and every window was clean:


batch Z: arm order per run: [['A', 'A2', 'B'], ['A', 'A2', 'B']]
| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 s | 1.69 | 1.79 | 8 | 8 of 8 | 1 | 2 s | 1.59-1.79 | 3.3 % | 3.5 / 2.3 % | True | 4233/4062/4192 |
| 1 | 0 s | 1.59 | 1.50 | 8 | 8 of 8 | 1 | 4 s | 1.37-1.62 | 1.1 % | 2.2 / 4.5 % | True | 4970/4134/4052 |
| 2 | 0 s | 1.37 | 1.16 | 8 | 8 of 8 | 1 | 2 s | 1.16-1.39 | 7.5 % | 3.9 / 6.1 % | True | 4361/4436/4524 |
| 3 | 0 s | 1.35 | 1.34 | 8 | 8 of 8 | 1 | 2 s | 1.20-1.34 | 4.6 % | 4.2 / 2.8 % | True | 5213/4138/4835 |


batch Z200 (start order A,A2,B):

| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 s | 1.20 | 1.20 / 0.86 | 4 | 4 of 4 | 1 | 2 s | 0.86-1.02 | 9.9 % | 4.4 / 5.3 % | 5.1 [23.4] | True |
| 1 | 0 s | 0.86 | 0.86 / 0.69 | 4 | 4 of 4 | 1 | 2 s | 0.69-0.75 | 2.9 % | 3.7 / 2.0 % | 5.4 [24.8] | True |
| 2 | 0 s | 0.69 | 0.69 / 1.45 | 4 | 4 of 4 | 1 | 2 s | 0.97-0.97 | 5.2 % | 3.7 / 2.8 % | 5.7 [21.6] | True |

batch Z10000 (start order A,A2,B):

| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 s | 1.45 | 1.45 / 1.96 | 4 | 4 of 4 | 1 | 2 s | 1.13-1.60 | 2.2 % | 1.3 / 3.2 % | 13.1 [26.6] | True |
| 1 | 0 s | 1.96 | 1.96 / 2.18 | 4 | 4 of 4 | 1 | 2 s | 1.52-1.96 | 1.9 % | 2.8 / 5.3 % | 10.9 [24.6] | True |
| 2 | 10 s | 1.85 | 1.85 / 1.44 | 4 | 4 of 4 | 1 | 2 s | 1.22-1.44 | 4.6 % | 2.5 / 1.5 % | 12.4 [28.2] | True |

bad replies: 0

**What was done about the gate, and what it cost.** The first invocation (p1a, 5 runs, `--quiet-wait 180`) spent 180 s of its first run's gate waiting for a lull that did not come (load 7.1 at release and 14.9 at the first cell) and was cut after that run to fit the budget; its run 0 is kept as group S1's first run (it differs from p1b only in the gate wait) and p1b ran 4 more with `--quiet-wait 30`. The remaining phases kept the 30 s cap: a longer wait would have bought nothing while the sessions around kept building and testing, and would not have fit the one-hour budget; the cell gate and the pairing are what protect the numbers. When the host went quiet (p4 onward) two extra batches (p8, p9) were added with the time that freed. S1 and U1/U2 ran loaded, S3, U3 and the size cells quiet; the scan row's B - A is the same in both (-3.2 to -3.4 ns), the `UPDATE`'s is not stable between groups (section 3).

## 10. What the run teaches

1. **BT built the cheap half of the ceiling.** The tuple read is a per-row cost of about 3 ns because it returns a `Status` through memory on the success path; B removes it (B - A is 106 % of BT-S1's D2), and nothing else in the order - the deleted `spills_.clear()`, the epoch read moved under the span, the arming in one lookup - shows above the 0.2 ns by which B exceeds D2, which is under the typical A-against-A' gap (0.45 / 0.48 ns). This run measures the sum of the order and cannot attribute a share to S3, so "S3 delivered nothing" is not what the data say; "S3 delivered less than this run can resolve" is. BT-S1 expected the real S3 to land at or below D1 + D3 less their overlap (at most 5.7 ns) and said the premise priced it worst; B's total of 3.3 / 3.5 ns, S2 included, is well below even that.
2. **The counts and the charge are the largest unbuilt piece.** D3 priced them at 3.2 / 3.2 ns, BT-R3 requires them and BT-S3 found the `Status`-free charge compiles to the same code, so the remaining 3.8 / 4.6 ns to the ceiling is mostly work the engine does on purpose. The row after BT is 27.4 / 27.5 ns; BT-S1's floor (D4, 24.0 / 23.7 ns in another session) is 3-4 ns under it, and what is left of the row is BM's: the kernel's compare, the slot loop, the visibility decision and the walk (BT-S1's insight 5).
3. **The saving is larger on the wider relation.** `wide-h` saves 4.0 ns against `bn`'s 3.3 on both paths, in every group (-3.9 to -4.0 against -3.2 to -3.4 literal): 0.7 ns, 1.6 times the typical gap on `bn`. `wd` is 13 columns and 304 pages against `s`'s 3 columns and 61, with a spilled `varchar`; this run does not say why the saving is larger there. A is also 3.7 ns higher on `wide-h` (34.7 against 31.0 ns).
4. **BT-S4's effect per updated row is an order of magnitude the scan's per-row saving.** 24-44 ns per updated row against 3.3 ns per rejected row. The only B-side change specific to `UPDATE` is `ConjunctWellFormed`'s deletion (BT-S4); `ReadTupleIn` (BT-S2) may also be reached by `UPDATE`'s walk, and the cell carries all of the order's changes, so **this run supports "the `UPDATE` got cheaper, by about 3 % at 1,000 rows and 2-5 % across groups and sizes" and not "BT-S4 is worth X"**. The per-row size is steady within a factor of 1.8 across 200, 1,000 and 10,000 rows (37-44, 24-27, 37 ns), which is what a per-row deletion predicts. BM-S6's deferred note (`known-gaps.md`, "The scan row (BM)": checking once per statement deletes the per-row test) acquires its first data point: the guard that read "within the spread, not proven zero" at the remedy reads below A here.
5. **A busy host moved the medians by less than a session does.** A reads 31.0 ns here with the load average at 5-15 for the first half and 1-2 for the second, and 32.0 ns in BT-S1 at load 1; the groups S1 (loaded) and S3 (quiet) give B - A of -3.2 and -3.4 ns. What load did produce was two outliers at twice the median, which the driver's clean flag identified. For a per-row CPU saving of 10 %, the paired design with a same-bytes control carried the result through a noisy host; the 1,000-row `UPDATE` (a 3 % effect on a 750 µs statement with a typical gap of 12-13 µs) is where it did not carry it alone.
6. **The `UPDATE` cell is group-sensitive; the scan row is not.** B - A on the 1,000-row `UPDATE` is -15.8 / -33.6 / -12.6 µs literal and +0.7 / -42.9 / -14.1 prepared across U1 (B last), U2 (A last) and U3 (A' last), a spread of 21-44 µs for a pooled -24 / -27: the direction of a last-started penalty, not proof of one. The scan row, whose effect is seven times the gap, is -3.2 / -3.3 / -3.4 ns across S1 (B last), S2 (A last) and S3 (B last) and shows nothing of it. A 3 % effect on a 750 µs statement needs the size sweep and 15 runs to be read at all, as BM-S6 found.
7. **No open decision in `CLAUDE.md` acquired a data point.** BT-Q1's bar (D-all at 3.0 ns or more) was passed by the ablation at 7.1 / 8.1 ns; the order as built passes the same 3.0 ns at 3.3 / 3.5 ns, so the gate's ceiling was 2.2-2.3 times what was built.

## 11. What was cut, and what was not run

Cut to the operator's time budget (about one hour including the build: 08:45 to the end of the measurement at 09:35):

- **Runs.** The scan row ran 13 runs in three groups (5 + 3 + 5) instead of 5; `bn_census` ran 15 runs in three groups (5 + 5 + 5) instead of 10; both were extended when the host went quiet. The size cells ran 4 (scan, 200 and 1,000 rows) and 3 (`UPDATE`, 200 and 10,000 rows) runs.
- **The load gate.** `--quiet-wait` 180 s for one run, 30 s for the rest, against a host that stayed above the 2.0 gate for about 35 minutes (section 9); p1a was cut after its first run, whose data is kept.
- **Not run:** the other `bm_scan` cells (`sel-0`..`sel-100`, `wide-h-k`, `wide-a`..`wide-m`, `nul-0`, `nul-50`, `bn-recent`, `rows-100k`, `out-200`, `out-1k`): **B's kept-row paths (BT-S3 moved the epoch read onto them) are therefore not measured here**, and neither is a rejected row of any column type but `wide-h`'s decimal(30,4); `scan_kernel = off`; the layout probe; the profile (no sampler build, so no share of the row is attributed); a split of B into BT-S2, BT-S3 and BT-S4 (only A and B are binaries); PostgreSQL 18 (the floor is the BM close's figure); `durability = group` and `strict` for the `UPDATE`; `cores > 1`; `bn_census`'s `update-lit`, `scan` and other write-path shapes; the contract suites and the full test suite on `4274ed81`.
- **Nothing here is extrapolated across a cut.** The 200- and 10,000-row `UPDATE` cells are 3 runs each and read as signs and orders of magnitude.

## Appendix. Reproducing

All from `/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row` (`tools/` unchanged since `34ab4269`), copies in `/home/cdkbs/bench-runs/bt-close/bin/` (`kds_server-A` and `kds_server-A2` the same file, copied from `/home/cdkbs/bench-runs/bt-s1/bin/kds_server-A`; `kds_server-B` from `git archive 4274ed81` built with `cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` and `cmake --build --target kds_server -j8`). `GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"`, `QW` 30 (180 for p1a):

    python3 tools/bm_scan.py census --arm A=<A> --arm A2=<A copy> --arm B=<B> --workdir <dir> --port 32811 --runs 4 \
        --pin-server 2 --pin-driver 4 --cells bn,wide-h --scan-ops 192 --point-ops 3000 --quiet-load 2 --quiet-wait $QW $GATE --out p1b.json   # S1
    python3 tools/bm_scan.py census --arm B=<B> --arm A2=<A copy> --arm A=<A> ... --runs 3 ... --out p4.json                                     # S2 (start order reversed)
    python3 tools/bm_scan.py census --arm A=<A> --arm A2=<A copy> --arm B=<B> ... --runs 5 ... --out p8.json                                     # S3
    python3 tools/bm_scan.py census ... --runs 4 --cells rows-200,rows-1k ... --out p5.json                                                      # sizes
    python3 tools/bn_census.py --arm A=<A> --arm A2=<A copy> --arm B=<B> --workdir <dir> --port 32821 --runs 5 --pin-server 2 --pin-driver 4 \
        --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 --only select-pk,update-range \
        --quiet-load 2 --quiet-wait $QW $GATE --out p2.json                                                                                      # U1
    (p3: --arm B --arm A2 --arm A;  p9: --arm B --arm A --arm A2;  p6: --rows 200 --scan-rows 200 --range-ops 600, 3 runs;  p7: --rows 10000 --scan-rows 10000 --scan-ops 200 --range-ops 60, 3 runs)

Tables: `scripts/bt_close_scan.py net|groups|runs|pct|waits|host|identity S1=<file> ...` (`CLEAN=1` drops windows the driver marked unclean, `NOPOOL=1` omits the pooled row), `scripts/bt_close_bn.py guard|groups|runs|pct|waits|host|end U1=<file> ...`, `scripts/ident.py`, `scripts/hostsummary.py`, and `scripts/assemble.py bt_close.tmpl out.md`, which fills every table in this file from the JSON (it ran in `/home/cdkbs/bench-runs/bt-close` over `out/p*.json`; to re-run from the archive, gunzip the `*.json.gz` into `out/`, put `hostwatch.log` in `out/` and `progress.log` as `progress` beside it). p1a's invocation was p1b's with `--runs 5` and `--quiet-wait 180` (`scripts/p1a.txt`).

Raw output: `bench/v3.0.0/archive/bt-close-v2.7.0-997-g4274ed81/` - the ten JSON files and logs (gzip), `binaries.txt`, `host.txt` (device, toolchain, build cache, driver hashes, tree status, the diff stat), `hostwatch.log` (load average and competing count every 10 s), `progress.log`, `watch.log`, `build.out` and the run, table and assembly scripts.
