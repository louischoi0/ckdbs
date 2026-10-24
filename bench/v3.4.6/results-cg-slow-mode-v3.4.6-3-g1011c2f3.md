# CG's walking-UPDATE slow mode: not the host and not phase order - B's binary spends more server CPU per walked row

**Thesis.** This repeats CG's close cell on a quiet, pinned host. It adds
the server's on-CPU time, its run-queue wait, and the busy share of its SMT
sibling, and a third arm that runs B's binary through A's phases.

- **The host is not the cause.**
  - No build or test process (`kds_tests`, `cc1plus`, `ctest`, the only
    names the watcher counts) was seen in any of the 30 cells.
  - The 1-minute load stayed at or below 1.4, which is the harness itself
    (the server and the driver).
  - The server's run-queue wait is 3.2-6.1 us per statement in every B and
    Ba rep, slow and fast alike (A's sibling-loaded reps r3, r4, r8:
    8.9-16.5 us).
  - The server's SMT sibling (CPU 5) is 0-2.4 % busy in 12 of the 13 slow
    reps; Ba-r2 is 8.1 %, and four slow reps read 0.0 %. One 10 ms tick is
    about 1.2 % of the phase.
  - A, interleaved with B within seconds, never entered the slow mode.
    This, not the CPU-time column, is what excludes a slower host CPU:
    on-CPU time rises the same whether the work grew or the CPU slowed,
    and this guest sees neither its clock (no cpufreq) nor steal (CPU 4's
    steal column is 0 for its lifetime). All 13 slow cells landing on B's
    binary and none on A's has a chance of about 6.5 in 10,000 under a
    binary-blind host cause.
- **Phase order is not the cause.** This was the confound CG's close could
  not exclude. B's binary run through A's phases (arm **Ba**) has the slow
  mode in 7 of 10 reps; B run through its own phases has it in 6 of 10.
- **The slow mode is server CPU, and it is in B's binary.**
  - At 10,000 rows, the slow reps' median server CPU is about 825 us per
    statement (815-840 per rep); the fast reps' and A's median is about
    762 us. The difference is +62 us, about +8 %, about 6 ns per walked
    row.
  - At 1,000 rows, B's walking `UPDATE` costs 108.5-115 us of server CPU
    in 19 of the 20 B and Ba reps (median 110), against A's 99.7-105.3
    (median 100.5). That is +9.5 %, about 9.5 ns per walked row. The
    exception is B-r5, at 102.0 us - A's level - in a run whose 10,000-row
    walk was slow. CG's close read this cell as -5.3 % and +6.1 % on the
    wire; pinned, the wire shows it too (p50 medians 120.3 against 128.9).
  - The tuple-key point `UPDATE` and the `INSERT` are within 1.2 us
    (5 %) of A in every arm and mode: medians 26.5-27.6 us against 26.3-26.4,
    and 24.2-24.8 against 23.8.
- **Not explained here.** This run does not show why the cost is bimodal at
  10,000 rows (fast reps at A's level) but present in 19 of 20 reps at
  1,000.
  It also does not show which code in B's walk pays it. CG-S1 did not
  change the walk's loop or `apply`, but it split `UpdateInner` into its
  parse and `UpdateParsed`, so the compiled walk can differ. A per-run state
  that depends on memory addresses (heap and page placement under ASLR) is
  the leading candidate for the bimodality. It is untested.

| Item | Value |
|---|---|
| Executed | 2026-10-24 03:58:11-03:59:16 UTC; no quiet-gate wait |
| Worktree / describe | `cg-slow-mode-attribution`, `v3.4.6-3-g1011c2f3` (only `tools/cg_close_benchmark.py` and this bench tree changed; no engine build input) |
| A | `f44846ef` (`v3.4.3`), binary `kds_server.f44846ef` sha256 `6e6c2ac19c5e7ecb5b2526628a02ffc3a96b1a54fef0a3b4eacf28114defbd35`, `--engine a` |
| B | `6775aec1` (`v3.4.3-8-g6775aec1`, CG-S1), binary `kds_server.6775aec1` sha256 `3d300d01f53a2e88c9b110d8d08971223cdcd64646b6abdd80519c4df1135d36`, `--engine b` |
| Ba | B's binary, `--engine a` (A's five phases, so the 10,000-row walk runs at A's statement position) |
| Binaries | Release, the same copies CG's close ran (`/home/cdkbs/bench-runs/cg/bin/`) |
| Host | 8 vCPU AMD EPYC 9V74 (4 cores x SMT 2), Azure (no guest cpufreq; cpuidle driver `none`), ext4 on `/dev/root` |
| Pinning | server `taskset -c 4` (core 4-5), driver `taskset -c 6` |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`; one fresh server and data file per cell |
| Order | 10 reps, rotated A B Ba / B Ba A / Ba A B |
| Driver | `tools/cg_close_benchmark.py` with `--server-pid` (per-phase schedstat on-CPU time and run-queue wait per statement) and `--watch-cpus 4,5` (per-phase /proc/stat busy share); `archive/cg-slow-mode-v3.4.6-3-g1011c2f3/` holds `matrix.sh`, `run.sh`, `tables.py`, `tables.md` and `raw/` (per-cell JSON, a 0.5 s competitor watch per cell, the progress and watchdog logs) |
| Watchdog | a detached 60 s watchdog (`run.sh`) plus a session relay; no relaunch |
| Cut | `cores > 1`, other durabilities, a profile of where the extra CPU goes, an ASLR-off arm |

## 1. The 10,000-row walking UPDATE, per arm (medians of 10 reps)

| arm | slow reps (p50 > 820 us) | p50 | server CPU us/stmt | run-queue wait us/stmt | CPU 5 (sibling) busy | p50 per rep, sorted |
|---|---|---|---|---|---|---|
| A | 0 / 10 | 768.9 | 762.4 | 4.3 | 3.1 % | 756.3 757.7 758.5 765.8 768.7 769.1 776.5 782.7 785.7 802.5 |
| B | 6 / 10 | 838.5 | 820.4 | 4.8 | 1.2 % | 761.1 765.1 776.4 782.5 838.3 838.6 839.5 848.5 851.5 851.6 |
| Ba | 7 / 10 | 836.4 | 815.8 | 4.4 | 0.6 % | 757.5 766.1 783.7 834.0 835.3 837.5 838.7 840.5 841.5 847.0 |

Every rep is in `archive/.../tables.md`. In a slow rep the whole
distribution moves: p0 is 794-805 us, against 717-760 us in fast and A reps.

**The sibling-loaded reps.**
- A-r4 is the one rep where the sibling was busy (41 %). Its server CPU
  rose to 891 us per statement and its mean to 927 us, while its p50
  stayed at 769 us.
- A-r8 (sibling 9.5 %) is A's highest p50, at 802.5 us; A-r3 (8.5 %) and
  Ba-r5 (12.3 %) stayed fast, Ba-r2 (8.1 %) is slow.

**Effect.** Sibling load slows part of a phase (SMT contention): it raises
the server's CPU time and the tail. It does not make the slow band, which
appears with the sibling at 0.0 % (B-r6, Ba-r6, Ba-r9, Ba-r10).

## 2. Server CPU per statement by mode, other phases

| arm / mode | reps | walking UPDATE, 1,000 rows | point UPDATE, 1,000 | point UPDATE, 10,000 | INSERT, 10,000 | walking UPDATE, 10,000 |
|---|---|---|---|---|---|---|
| A | 10 | 100.5 | 26.3 | 26.4 | 23.8 | 762.4 |
| B fast | 4 | 109.9 | 26.5 | 26.7 | 24.2 | 762.2 |
| B slow | 6 | 110.3 | 27.1 | 27.1 | 24.8 | 824.9 |
| Ba fast | 3 | 110.2 | 27.2 | 27.6 | 24.4 | 762.1 |
| Ba slow | 7 | 109.4 | 26.8 | 26.6 | 24.2 | 825.1 |

**What the table shows.**
- The extra cost belongs to the walking `UPDATE` alone.
- At 1,000 rows B pays it in 19 runs of 20.
- At 10,000 rows B pays it in about two runs of three.

## 3. What this says about the engine

- **CG-S1 made walking `UPDATE`s dearer in CPU.** The cost is about 6-10 ns
  per walked row, roughly +8-10 %. It is not a host artefact and not a
  measurement-order artefact. CG's close recorded it as unexplained
  wall-clock noise; it is server on-CPU time that follows B's binary. It
  does not touch the tuple-key point `UPDATE` or `INSERT`. The upsert arms
  have no A baseline (A cannot parse them); B's slow and fast reps agree
  on them within 0.4 us.
- **Why it switches off in some 10,000-row runs is open.**
  - A fast B rep costs exactly what A costs. So the extra cost is not
    unconditional per row.
  - The mode is not fixed per server process: B-r5 was at A's level at
    1,000 rows and slow at 10,000, and B's four fast 10,000-row reps paid
    the cost at 1,000. Whatever selects it is chosen per relation (or per
    size) within one run.
  - It may be something that depends on where the walked pages or the
    statement's state land in memory. A cache-line or set conflict, or 4K
    aliasing, would fit a per-run, address-dependent switch.
- **Next steps** (each one stage):
  1. Sample where the extra CPU goes: `tools/kds_sampler.c` under
     `LD_PRELOAD` (perf is refused here), on B slow, B fast and A at
     10,000 rows.
  2. Run B with ASLR off (`setarch -R`) to test whether the 10,000-row
     bimodality follows the address layout.
  3. Diff the compiled walk loop between the two binaries (`objdump` of the
     `UpdateParsed` walk and its `apply`).
