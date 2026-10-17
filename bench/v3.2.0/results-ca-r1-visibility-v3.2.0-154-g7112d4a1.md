# CA round 1: the commit-lookup latch on the read path (base vs C1 vs C1+C2)

Work order: `instructions/v3.2.0/workorder-ca-background-advisory-optimizer.md` (exploratory round).
Harness: `bench/v3.2.0/archive/ca-harness/README.md`. Raw files:
`bench/v3.2.0/archive/ca-r1-v3.2.0-154-g7112d4a1/` (driver `ca-r1-run.sh`, `compare.md`, per-cell JSON/stdout/logs, the sampled dumps).

**Finding.** Giving a walk a per-writer visibility memo (C1) takes `InstanceVisibility::LookupCommit` from
6.9% / 5.7% of sampled server CPU (s1 / s2, base) to 0.2% / 0.6% (s1 / s2, c12), and scenario 1's walked reads
get 12-26% faster, outside the noise floor on four of five rows. Scenarios 2, 3 and 4 do not move outside
their floors. C2 (burn threshold 4096 -> 1) adds nothing measurable over C1 on any row. One row is worse: s1
`load-bars` is 9.1% slower on c1 (outside both spreads), 3.2% slower on c12 (inside the 7.7% spread).

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-17, 11:46:26-12:05:34 UTC (19.1 min wall, incl. the sampled pass) |
| Worktree / branch | `bz-background-advisory-optimizer` / `worktree-bz-background-advisory-optimizer` |
| `git describe --tags` | `v3.2.0-154-g7112d4a1`. **Tree dirty at write time**: `include/kds/exec/step_vm.hpp` and `src/exec/step_vm.cpp` modified by another session after the binaries were built; they are not what was measured |
| Binaries | copies in `/home/cdkbs/bench-runs/bz/`, every server started from the copy |
| base | `kds_server.base`, engine `e1333a77`, sha256 `1dac7afcbdfa8544001c805053fdce7458193cb9ea86c0cf5e267ffa90abc53e`, copy mtime 11:20:30 |
| c1 | `kds_server.c1`, commit `4e11968a` (11:45:08 UTC), sha256 `6b23fa975f9be73f29f284e3aa37f79dce3f4016e5cd4b0a4291d5acacd3d7a3`, copy mtime 11:45:35 |
| c12 | `kds_server.c12`, commit `7112d4a1` (11:45:35 UTC), sha256 `9f9fe65ef286b92a72b00c8eec96308cbf9f15a385379b88270788505072c189`, copy mtime 11:45:38 |
| Build / device | Release (builds were supplied by the requester, not rebuilt here); `/dev/root` ext4, data files under `/home/cdkbs/bench-runs/bz/data/<cell>`, fresh per server. Not tmpfs |
| Host | 8 CPUs; loadavg 0.85-2.61 at cell starts; no `cc1plus`/`cmake --build`/`ctest` process in any of the 35 prechecks (`*/sN.precheck`). Other servers on the host (other ports) were running and were not touched; cells are not pinned |
| Server config | as CA-S0: `durability = group`, `log_level = warn`, `buffer_pool_frames = 65536`, `cores = 8` (s1-s3), s4 `cores = 1` x 3 servers; default advisory switches (no adv-off) |
| Sizes | identical to CA-S0 (`results-ca-s0-premise-v3.2.0-152-ga9f0f5cd.md` Method): s1 10,080 bars, s2 `--cargos 10000 --bookings 9000`, s3 `--loans 10000`, s4 `--days 2 --blocks 4`. No row-count sweep (rule 9 cut, as S0) |

## Method and cuts

- Order per rep (rotating, to separate position): rep 1 `base c1 c12`, rep 2 `c1 c12 base`, rep 3 `c12 base c1`.
  Each pass runs s1, s2, s3, then s4.
- **Cuts to fit 20 minutes:** s4 ran in reps 1-2 only (n=2; a spread from two points is a weak floor). s1-s3: 3 reps. The sampled pass
  is one c12 run of s1 and s2 (no s3, s4). No `none` index-mode cell for s3, no row-count sweep.
- Cell = median of reps (derived qps = ops/elapsed, or `1e6/mean_us` where the driver prints elapsed 0.0, as in S0);
  spread = (max-min)/median over the arm's reps; `~` = |delta| inside the larger spread of the two arms: not a finding.
- No before/after history is claimed: base is the engine at `e1333a77`, the candidates are the two named commits.
- Position effect: s1's scan rows are monotone in arm and not in position. `agg-day-slice` c1 = 2,510 (position 2), 2,565 (position 1),
  2,583 (position 3); base = 2,019 (1), 2,142 (2), 2,042 (3). So the +22-26% is not a position artefact. Baseline reps of `backtest-replay`
  (1,714 / 1,729 / 1,480) show one slow base rep (rep 3, position 2), which is why its spread is 14.5%.
- Rule 3 (wait decomposition) does not apply: a switch/candidate comparison, not a latency study. Waits are not separated.

## Throughput by scenario (qps, median of reps, delta vs base)

Full table: `archive/ca-r1-v3.2.0-154-g7112d4a1/compare.md`.

### Scenario 1 (BTREE copy, 10,080 bars)

| Headline | base | spread | c1 | delta | spread | c12 | delta | spread |
|---|---|---|---|---|---|---|---|---|
| load-bars (insert) | 20,101 | 1.7% | 18,278 | -9.1% | 5.1% | 19,461 | -3.2% ~ | 7.7% |
| result-insert | 672 | 4.9% | 558 | -16.9% ~ | 23.4% | 678 | +1.0% ~ | 22.6% |
| backtest-read (3-rel join, cold) | 1,601 | 4.9% | 1,571 | -1.9% ~ | 19.9% | 1,810 | +13.0% | 12.0% |
| backtest-replay (warm) | 1,714 | 14.5% | 2,000 | +16.7% | 4.1% | 1,910 | +11.5% ~ | 6.2% |
| read-day-slice | 1,872 | 3.9% | 2,188 | +16.9% | 1.2% | 2,119 | +13.2% | 3.6% |
| read-bar-lookup (pk) | 12,188 | 28.4% | 12,326 | +1.1% ~ | 3.7% | 10,632 | -12.8% ~ | 17.5% |
| read-join-point (pk) | 12,111 | 27.4% | 12,423 | +2.6% ~ | 5.3% | 12,113 | 0.0% ~ | 11.9% |
| agg-day-slice | 2,042 | 6.0% | 2,565 | +25.6% | 2.8% | 2,489 | +21.9% | 3.5% |

The walked reads (`read-day-slice`, `agg-day-slice`) gain 13-26% with spreads of 1-6% in every arm: a finding. `backtest-replay` and
`backtest-read` point the same way (+11 to +17%) but sit inside one arm's large spread in two of four cells (base's slow rep 3; c1's
`backtest-read` rep 1 of 1,847 against 1,535 and 1,571), so read them as probable, not established. The pk shapes are 8 ms phases and
say nothing (spreads 4-28%).

`load-bars` is the only row that points against the candidates. c1: 17,727 / 18,659 / 18,278 against base 20,101 / 19,820 / 20,170 (no
overlap, delta 9.1% over spreads of 1.7% and 5.1%), so on c1 it is outside the floor. c12 (19,770 / 19,461 / 18,276) overlaps base and is
inside its spread. C1 changes the walk, not the insert path, so no mechanism is claimed here; with n=3 and one insert phase, treat it as
a flag to repeat, not a regression.

### Scenario 2 (freight, 8 bookers, `group`)

| Headline | base | spread | c1 | delta | spread | c12 | delta | spread |
|---|---|---|---|---|---|---|---|---|
| booking txn (ops/s) | 2,108 | 1.4% | 2,085 | -1.1% ~ | 2.4% | 2,051 | -2.7% ~ | 5.4% |
| commit (committed TPS) | 1,537 | 1.5% | 1,525 | -0.8% ~ | 2.4% | 1,503 | -2.2% ~ | 6.0% |
| charge-insert | 8,644 | 1.5% | 8,574 | -0.8% ~ | 2.4% | 8,455 | -2.2% ~ | 5.9% |

One number (all rows share an elapsed). Flat inside the floor. This is a write-bound scenario; `group` fsync dominates it.

### Scenario 3 (library, 10,000 loans, `--index-mode all`)

| Headline | base | spread | c1 | delta | spread | c12 | delta | spread |
|---|---|---|---|---|---|---|---|---|
| pk-user (control) | 14,065 | 5.3% | 14,104 | +0.3% ~ | 29.1% | 14,144 | +0.6% ~ | 1.8% |
| loans-by-user | 10,173 | 5.0% | 10,299 | +1.2% ~ | 15.1% | 10,582 | +4.0% ~ | 3.1% |
| loans-by-book | 11,173 | 2.0% | 11,364 | +1.7% ~ | 18.3% | 11,338 | +1.5% ~ | 3.2% |
| books-by-genre | 1,993 | 2.7% | 1,997 | +0.2% ~ | 6.5% | 2,004 | +0.6% ~ | 2.4% |
| loans-by-daterange | 618 | 3.0% | 635 | +2.9% ~ | 0.2% | 634 | +2.7% ~ | 1.4% |
| overdue | 987 | 3.8% | 998 | +1.1% ~ | 2.6% | 1,005 | +1.8% ~ | 1.3% |
| join-loan-user | 9,662 | 1.3% | 9,833 | +1.8% ~ | 21.5% | 9,606 | -0.6% ~ | 14.7% |
| exists-correlated | 6,489 | 7.3% | 6,456 | -0.5% ~ | 1.9% | 6,418 | -1.1% ~ | 3.2% |
| count-by-user | 14,144 | 3.7% | 14,368 | +1.6% ~ | 12.1% | 14,225 | +0.6% ~ | 27.7% |

Nothing moves. Every c1/c12 delta is positive-leaning by 0-4% with the base spread at 1-7%, below any claim. s3 at this size is a
durability and wire scenario (S0: 25% `fdatasync`/`pwrite`, 28% idle, `LookupCommit` 0.6% of CPU), so a visibility-latch change has
little to remove.

### Scenario 4 (cabin optimizer days, 1 core per server, n=2)

| Headline | base | spread | c1 | delta | spread | c12 | delta | spread |
|---|---|---|---|---|---|---|---|---|
| board probe, off arm | 1,949 | 0.0% | 1,991 | +2.2% | 1.6% | 1,995 | +2.4% | 0.3% |
| board probe, on arm | 3,241 | 1.2% | 3,259 | +0.6% ~ | 2.6% | 3,209 | -1.0% ~ | 5.6% |
| board probe, declared arm | 3,372 | 1.1% | 3,459 | +2.6% | 1.6% | 3,484 | +3.3% | 2.1% |
| tape10k probe, off arm | 2,231 | 1.4% | 2,279 | +2.1% | 0.1% | 2,167 | -2.9% ~ | 5.8% |
| tape10k probe, on arm | 4,715 | 1.6% | 4,845 | +2.8% ~ | 6.3% | 4,615 | -2.1% ~ | 6.4% |
| tape10k probe, declared arm | 4,461 | 3.4% | 4,669 | +4.7% | 0.1% | 4,711 | +5.6% | 4.4% |
| pk-ops control, off / on / declared | 14,415 / 13,987 / 13,671 | 5.5 / 1.3 / 15.6% | 14,655 / 13,945 / 14,264 | +1.7 / -0.3 / +4.3% ~ | 7.1 / 17.0 / 0.2% | 13,809 / 13,483 / 14,357 | -4.2 / -3.6 / +5.0% ~ | 2.9 / 11.6 / 12.4% |
| open insert, off / on / declared | 625 / 628 / 697 | 1.4 / 9.4 / 9.2% | 695 / 645 / 679 | +11.3 / +2.7 ~ / -2.6 ~ | 5.5 / 6.9 / 13.3% | 647 / 673 / 634 | +3.5 / +7.3 ~ / -9.1 ~ | 2.6 / 6.3 / 0.3% |

s4 is flat to within +-5% with n=2 (spreads of 0-17%); the 2-5% "outside spread" deltas on 0.1-2% spreads are the size of the
positive-leaning drift CA-S0 saw between positions and are not read. The optimizer's serving win (on arm 1.66x the off arm on
the board probe in all three binaries) is intact.

## Where the server CPU goes: base vs c12 (one sampled pass each of s1 and s2)

`LookupCommit`'s share fell by 30x in s1 and 10x in s2. Leaf-function categories, share of CPU-time samples
(sampler at 500 Hz, no frame pointers, so leaf categories; the base column is CA-S0's `prof-base-r1`, the same sampler and sizes,
**measured earlier in the same session on `kds_server.base`**; the c12 column is `prof-c12-r1` here. One sampled pass each,
so shares carry about +-1 point of sampling noise at these sample counts).

| Category | s1 base | s1 c12 | s2 base | s2 c12 |
|---|---|---|---|---|
| samples | 1,881 | 1,753 | 6,038 | 6,138 |
| **mvcc visibility (`LookupCommit`)** | **6.9% (130)** | **0.2% (4)** | **5.7% (342)** | **0.6% (36)** |
| locks / latches (mutex, `LockTable`) | 9.5% | 2.7% | 7.4% | 4.4% |
| row accept / scan kernel / decode / btree | 28.1% | 35.1% | 36.7% | 41.2% |
| socket / wire encode | 18.1% | 21.9% | 10.4% | 9.9% |
| idle (epoll / yield) | 13.8% | 12.6% | 9.0% | 10.3% |
| WAL / durability | 2.0% | 2.7% | 4.7% | 5.3% |
| advisory (Waystone / access stats / signals) | 0.4% | 0.5% | 0.6% | 0.8% |
| other (unattributed) | 14.2% | 17.1% | 16.8% | 18.2% |

`locks / latches` falls with it (s1 9.5% -> 2.7%, s2 7.4% -> 4.4%): the lookup's latch acquisition is the mutex time of the base profile.
This sampled c12 pass does not separate C1 from C2; the throughput arms give the same answer (C2 adds nothing over C1).

## Correctness and warnings (per arm)

| Arm | s1 `--verify` | s2 verify failures / 400 (per rep) | s2 `TXN_CONFLICT` log lines | s2 `gave up after 5 attempts` | s1, s3, s4 server-log lines |
|---|---|---|---|---|---|
| base | OK x3 | 37, 40, 33 | 44, 43, 27 | 4, 2, 0 | 0 |
| c1 | OK x3 | 36, 43, 39 | 25, 34, 33 | 2, 4, 0 | 0 |
| c12 | OK x3 | 43, 35, 47 | 28, 27, 25 | 2, 2, 8 | 0 |
| c12 sampled | OK | 45 | 31 | 4 | 0 |

s1's read-back-through-the-join check passed in all 10 cells; s4's reply-identity check passed (every rc 0). **s2's I1 sample fails in
8-12% of checks in every arm including base** (33-47 of 400; the pre-existing defect CA-S0 recorded, unchanged by C1/C2; the arm means 36.7 /
39.3 / 41.7 are inside the rep-to-rep range of 33-47). The log lines are all s2's: `TXN_CONFLICT` UPDATE refusals (retried) and
the `[btree] ... gave up after 5 attempts` insert refusals, present in base too. **Anomaly: c12 rep 3 logged 8 of the latter against 0-4 in
every other cell.** One cell, n=1 above the range; the c12 arm's other two reps and the sampled pass are 2-4. Not a finding, but it is the one
count the candidates did not match, so repeat before C2 is accepted.

## What the run says about the engine

- **The per-row `LookupCommit` latch was a real, measurable cost on walked reads, and C1 removes it.** CA-S0 priced it at 6.9% (s1) and 5.7% (s2)
  of leaf CPU, the largest single item inside "locks / latches"; the c12 profile has it at 0.2% and 0.6%, and s1's scan throughput gains 13-26%.
  The throughput gain exceeds the CPU share removed (6.9%); a plausible reading is that the latch is one for the instance and eight cores contend on it
  (so the saving is less waiting as well as fewer instructions), but this run did not measure contention separately.
- **s2 does not move despite losing 5% of its CPU in the same call**: the scenario is `group`-fsync-bound (WAL 5% of CPU, idle 10%, committed TPS
  ~1,500), so a read-path saving has no lever on its headline. Do not use s2 to judge read-path candidates.
- **C2 (burn threshold 1) shows no effect in these four scenarios.** c12 vs c1 differ by less than the spreads on every row, which these workloads
  (no idle core pinning the floor during a measured window) would predict. C2 targets a condition this quad does not create; it needs a scenario with
  a long-lived idle core to be measured, and its one oddity here (8 gave-up inserts in c12 rep 3) should be repeated.
- **`load-bars` -9% on c1** (outside the floor, inside it on c12) is the only flag against the candidates; the s1 load is an insert path, which a
  walk memo should not touch. Repeat with more reps before reading it.
- Noise floor of this round (same arm, interleaved): s1 scans 1-6% (base `backtest-replay` 14.5% from one slow rep), s2 1-6%, s3 1-8% with c1
  outliers at 15-29%, s4 0-17% (n=2). Deltas smaller than these are marked `~` and are not findings.

## Reproduction

    bench/v3.2.0/archive/ca-r1-v3.2.0-154-g7112d4a1/ca-r1-run.sh <OUTDIR>     # needs kds_server.{base,c1,c12} and bin/kds_sampler.so in /home/cdkbs/bench-runs/bz
    python3 bench/v3.2.0/archive/ca-harness/compare.py <OUTDIR> base c1 c12
