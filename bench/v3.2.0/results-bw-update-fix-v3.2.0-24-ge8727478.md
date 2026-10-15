# BW every-row UPDATE: the hoisted dropped-column gate at `e8727478` takes back 14-20 us of the 17-24 us B - A0 gap, but the host was loaded and the A0 - A0' spread is larger than the whole effect

**Thesis.** On `worktree-bw-instant-add-drop-column`, the engine at `e8727478` (`v3.2.0-24-ge8727478`, **C**) runs the every-row `UPDATE` of 1,000 rows (`UPDATE r SET v = ? WHERE id >= 1`) 14.4 us (literal, -1.8 %) and 20.3 us (prepared, -2.5 %) cheaper in server CPU than `541ce440` (`v3.2.0-22-g541ce440`, **B**, BW closed), and C sits +15.9 us (literal, +2.1 %) and -3.5 us (prepared, -0.4 %) from the pre-BW engine `74b0a3dd` (`v3.2.0-13-g74b0a3dd`, **A0**). C is below B in 7 of 7 paired runs on both paths. But this run was taken on a shared host at load average 2-11, and A0's own second copy (**A0'**) differs from A0 by up to 49.6 us (literal) and 65.8 us (prepared), more than the 30 us regression the BW close reported. So the direction is consistent and the fix is plausible, but the run cannot show C equal to A0 to better than that spread, and it cannot even reproduce B - A0 beyond the spread (+24.1 us literal, +16.8 us prepared; the close measured +30.2 / +30.2 on a quiet host). The point `SELECT` guard moved +1.4 us (literal) and +0.6 us (prepared) at C against A0, inside the largest A0' gap (3.2 / 4.7 us).

Server CPU is the sum of `/proc/<pid>/task/*/schedstat`; wall is the client's. Method, pinning, gates and archive layout are those of `results-bw-close-v3.2.0-22-g541ce440.md`; this file does not repeat them.

| Item | Value |
|---|---|
| Executed | 2026-10-14 UTC. C built 08:48:54-08:51:27; measurement 08:51:46-09:07:41 (`archive/bw-update-fix-v3.2.0-24-ge8727478/progress.txt`). Whole task 08:48 to the commit of this file, about 21 minutes, one minute over the 20-minute budget |
| Worktree / branch | `bw-instant-add-drop-column`, branch `worktree-bw-instant-add-drop-column`; HEAD `e8727478`, tree dirty only by the untracked `bench/v3.2.0/archive/bw-update-profile/` (the profile stage's archive) |
| Commits measured | **A0** = `74b0a3dd` (`v3.2.0-13-g74b0a3dd`); **A0'** = A0's binary again (the spread); **B** = `541ce440` (`v3.2.0-22-g541ce440`); **C** = `e8727478` (`v3.2.0-24-ge8727478`, BW's row-codec change: `EncodeRow`, `DecodeColumnsInto`, `DecodeRowInto` read `layout.has_dropped()` once per call and test `has_dropped && columns[i].dropped()` per column) |
| Binary provenance | A0 and B reused from `/home/cdkbs/bench-runs/bw-close/bin/` (built 08:15:13-14 from `git archive`; sha256 `911b4b85...` for A0 and A0', `07628c94...` for B, identical to the close's `binaries.txt`). C built from `git archive e8727478` into `/home/cdkbs/bench-runs/bw-update-fix/src-C`, binary mtime 08:51:27.865 UTC, sha256 `6e393d49e46b733f9793dad11265ca3a9c1f8f2248585f8bf00a2731e70b498e`. Every server started from a copy in `bin/` (`archive/.../binaries.txt`) |
| Build | `CMAKE_BUILD_TYPE=Release` (C's `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`. A0 and B as the close |
| Host / device | 8 vCPU, Linux 7.0.0-1014-azure; data files and WAL under `/home/cdkbs/bench-runs/bw-update-fix/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`). **Not quiet**: another session's build (`fix-write-subquery-own-writes`, 12 `cc1plus`) was running during C's build, and the load average before the measured runs was 7.6, 6.4, 9.4, 10.5, 10.7, 4.7, 2.1. It was not a build after 08:51:30 (`pgrep cc1plus` 0), but something else held load near 10 |
| Pinning | One server per arm, one at a time, on CPU 2; the driver on CPU 4 (`--pin-server 2 --pin-driver 4`) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default; fresh server and data file per arm per run |
| Drivers | `tools/bn_census.py` sha256 `7d28608e...`, `tools/kwp.py` `99341280...` (both as at the close). Gates `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1`; 20 of 112 cells ended flagged unclean |
| Suite | Not executed in this task; no code was changed by it. The suite for `e8727478` is the commit message's (Debug, 3839 of 3839 passed) |

## 1. What was run and what was cut

Four arms (A0, A0', B, C), a fresh server and data file per arm per run, interleaved block by block (6 blocks per cell), both driver paths alternating. Row count 1,000 (`--rows 1000 --scan-rows 1000`); `select-pk` is 2,000 point `SELECT`s per path, `update-range` 120 every-row `UPDATE`s of 1,000 rows per path. Work per run per arm is equal across arms. Row count, as the BW close, is a single size: the every-row `UPDATE` is the per-row-cost probe, and 200 / 10K were not re-run.

| group | cells | rows | start order | runs |
|---|---|---|---|---|
| u1 | `select-pk`, `update-range` | 1,000 | C, B, A0', A0 | 5 |
| u2 | same | 1,000 | A0, A0', B, C | 2 |

**Cut:** the `bm_scan.py census --cells bn` scan guard was not run (the budget was spent by the loaded host's quiet-gate waits). u2 was cut from 5 runs to 2 to stay near 20 minutes, so the two start orders are unbalanced (5 and 2); no 200-row or 10K-row `UPDATE`, no `INSERT` cell, and no cell where the scan row cost is measured at C. Scan is therefore unguarded for this fix (the BW close found it inside the spread at B, and the fix touches the same codec loops on the decode side).

## 2. Headline: server CPU per statement, us

Pooled over u1+u2 (7 runs), paired by run. Brackets are [min, max] of the paired differences; the last column is |A0' - A0| (typical = median, largest = max over runs). Statement wall and driver CPU are in section 3.

| cell | n runs | A0 | A0' | B | C | B - A0 (% of A0) | C - A0 (% of A0) | C - B (% of B) | A0' - A0 typical / largest |
|---|---|---|---|---|---|---|---|---|---|
| every-row UPDATE literal | 7 | 776.59 | 759.33 | 801.37 | 779.63 | +24.13 [-6.9, +76.5] (+3.1 %) | +15.92 [-12.4, +27.7] (+2.1 %) | **-14.40 [-55.7, -3.4] (-1.8 %)** | 9.60 / 49.62 |
| every-row UPDATE prepared | 7 | 779.92 | 741.62 | 796.70 | 774.49 | +16.78 [-5.2, +38.6] (+2.2 %) | -3.49 [-43.9, +12.6] (-0.4 %) | **-20.27 [-82.5, -3.8] (-2.5 %)** | 39.48 / 65.79 |
| select-pk literal (guard) | 7 | 31.88 | 31.36 | 32.13 | 33.00 | +0.25 [-1.7, +0.9] (+0.8 %) | +1.38 [+0.6, +2.5] (+4.3 %) | +1.30 [+0.5, +2.4] (+4.0 %) | 0.94 / 3.23 |
| select-pk prepared (guard) | 7 | 29.59 | 29.32 | 28.88 | 29.49 | +0.59 [-1.6, +7.0] (+2.0 %) | +0.55 [-0.2, +6.3] (+1.8 %) | +0.66 [-0.6, +1.5] (+2.3 %) | 0.27 / 4.68 |

Reading it:

- **C - B is negative in every run on both UPDATE paths** (maxima -3.4 and -3.8 us). That is the one result the spread does not contradict: both arms ran inside the same block, so a host-load swing hits them together. Per updated row it is -14 and -20 ns, against a close-measured B - A of +30 ns per row.
- **C - A0 is inside the spread** (15.9 vs 49.6 us largest, -3.5 vs 65.8). It is not shown equal to zero, only not distinguishable from it. The close's regression of +30.2 us is itself inside this run's spread, so this run could not have detected it; the evidence that C removed it is C - B, not C - A0.
- **The point `SELECT` guard** is +1.38 us (literal) and +0.55 us (prepared) at C - A0, +1.30 / +0.66 at C - B, all inside the largest A0' gap (3.23 / 4.68). The literal-path +4 % is above the typical gap (0.94) and not above the largest; it is not a finding, and the fix did not touch the point-SELECT path beyond the decode loops.
- The A0' - A0 spread here (9.6 / 49.6 us literal, 39.5 / 65.8 prepared) is 3-5 times the close's (8.1 / 27.1, 10.1 / 17.2). That is the host's load, not the engine: the same binary twice.

## 3. Latency and waits

Wall per statement, median over runs of each run's percentile, 7 runs pooled. The driver reports p0, p25, p50, p90, p99 (it does not emit p95). `statements/s` is derived: 1,000,000 / mean us.

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A0 | 13986 | 53.8 | 56.9 | 63.0 | 75.2 | 98.1 | 15307 | 31.9 | 32.3 |
| select-pk | literal | A0' | 13986 | 54.2 | 56.6 | 62.1 | 73.1 | 100.0 | 15493 | 31.4 | 32.8 |
| select-pk | literal | B | 13986 | 54.1 | 56.6 | 62.5 | 71.5 | 101.4 | 15667 | 32.1 | 32.3 |
| select-pk | literal | C | 13986 | 55.4 | 57.8 | 63.2 | 75.6 | 100.2 | 15367 | 33.0 | 32.3 |
| select-pk | prepared | A0 | 13986 | 51.1 | 53.7 | 55.4 | 69.2 | 101.7 | 16620 | 29.6 | 29.9 |
| select-pk | prepared | A0' | 13986 | 50.2 | 53.0 | 54.3 | 68.2 | 94.9 | 16810 | 29.3 | 29.6 |
| select-pk | prepared | B | 13986 | 50.5 | 53.5 | 54.7 | 68.2 | 100.5 | 16983 | 28.9 | 29.4 |
| select-pk | prepared | C | 13986 | 52.1 | 54.3 | 55.5 | 67.8 | 112.6 | 16081 | 29.5 | 29.8 |
| update-range | literal | A0 | 840 | 700.6 | 748.5 | 779.5 | 1087.0 | 1266.3 | 1177 | 776.6 | 42.0 |
| update-range | literal | A0' | 840 | 699.9 | 748.6 | 769.4 | 1079.7 | 2567.3 | 1135 | 759.3 | 43.8 |
| update-range | literal | B | 840 | 760.8 | 809.2 | 821.8 | 1127.7 | 1251.7 | 1134 | 801.4 | 43.6 |
| update-range | literal | C | 840 | 704.7 | 757.7 | 782.9 | 1089.5 | 4028.7 | 958 | 779.6 | 45.7 |
| update-range | prepared | A0 | 840 | 693.9 | 736.0 | 763.4 | 1250.5 | 4943.4 | 585 | 779.9 | 38.7 |
| update-range | prepared | A0' | 840 | 696.5 | 732.0 | 775.2 | 1594.6 | 3727.3 | 857 | 741.6 | 36.7 |
| update-range | prepared | B | 840 | 728.5 | 781.8 | 799.6 | 1222.6 | 3277.2 | 1009 | 796.7 | 39.8 |
| update-range | prepared | C | 840 | 708.0 | 742.1 | 764.6 | 1264.9 | 2758.1 | 496 | 774.5 | 37.7 |

The body of the distribution shows the same ordering as the CPU: on the literal `UPDATE`, p0 / p25 / p50 are A0 700.6 / 748.5 / 779.5, B 760.8 / 809.2 / 821.8, C 704.7 / 757.7 / 782.9. B's whole distribution is shifted up by 40-60 us from p0 (best case, so a fixed per-statement cost, not a tail); C is within 4-9 us of A0 at p0-p50. The p90-p99 tails and the "statements/s at mean" column are dominated by load-induced outliers (single 2-5 ms statements) and do not rank the arms; use p50 and the CPU column. Wall exceeds server CPU by 3-6 us at p50 and driver CPU is 37-46 us per `UPDATE`: the statement is server-CPU-bound.

**Waits (rule 3).** `durability = relaxed`: no fsync wait exists in any measured statement (commit/durability share 0). Single connection, one statement at a time: lock and conflict wait 0 (`bad` replies 0, end-state checks all true). Client and socket round trip is the wall-minus-server-CPU residue plus the driver's own CPU (the driver's CPU is about 5 % of an `UPDATE` and about equal to the server's on a point `SELECT`). The rest is server CPU: the write statement, of which the row codec is the part this change moved. A finer split of server CPU was the profile stage's (`archive/bw-update-profile/`), which found the delta spread across the codec loops, not in one function.

## 4. Insight

The BW cost on the every-row `UPDATE` was a per-column, per-row test of `layout.dropped` state read through a layout accessor inside the codec loops. Hoisting `has_dropped()` into a local, as `has_nulls` already was, removes it for an unaltered relation, and the paired C - B of -14 / -20 ns per row is of the same size as the +30 ns per row the close attributed to BW. It is consistent with the profile stage's finding that the cost was spread across loops (no single function was responsible), so a loop-invariant hoist is the right shape of fix, and nothing in this run suggests a second cost remains in the UPDATE path. The run does not support more than that: with A0' differing from A0 by up to 66 us, the residual between C and A0 (+16 literal, -3 prepared) is unmeasured. Closing it properly needs the quiet-host 12-block protocol of BT-close; `bench/README.md`'s rule is that a delta inside the floor is not a result, and C - A0 is inside it.

## 5. Baseline and floor

Baseline: the BW close, `bench/v3.2.0/results-bw-close-v3.2.0-22-g541ce440.md` (B - A: +30.2 us literal, +30.2 us prepared, quiet host). Against it, this run's B - A0 is +24.1 / +16.8 us (smaller, inside this run's spread) and C - B is -14.4 / -20.3 us. PostgreSQL twin not measured for this shape (the every-row `UPDATE` of 1,000 rows has no twin run here).

## 6. Raw data

`bench/v3.2.0/archive/bw-update-fix-v3.2.0-24-ge8727478/`: `out/u1.json`, `out/u2.json` (per-run, per-arm, per-block records including host gating), `out/u1.log`, `out/u2.log`, `scripts/` (`run.sh`, `run2.sh`, `chain2.sh`, `watch2.sh`, `start.sh`, `analyze.py`), `analysis.md` (all tables, u1 and u2 apart), `binaries.txt`, `progress.txt` (the one-minute watchdog lines; no orphan), `build.sh`. Reproduce: `python3 tools/bn_census.py --arm C=... --arm B=... --arm A2=... --arm A=... --workdir W --port P --runs 5 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --pin-server 2 --pin-driver 4 --only select-pk,update-range --quiet-load 2 --quiet-wait 30 <gates> --out O` (`run.sh` / `run2.sh` hold the exact lines; u1 ran 5 runs, u2 2).
