# CF-S0: does Waystone replay pay?

Work order: `instructions/v3.2.0/workorder-cf-waystone-premise.md` (§1.2, §4 CF-Q1/Q2, §5 CF-S0).
Harness: `bench/v3.3.0/archive/cf-harness/` (`cf_s0.py`, `run.sh`, `compare.py`). Raw files:
`/home/cdkbs/bench-runs/cf/s0/` (18 `<arm>-r<rep>.json`, configs, server logs, the watchdog log).

**Finding.** This is the first v3 measurement in which a replay demonstrably ran (`ANALYZE`'s
`replays=` non-zero in arm A, zero in B and C, in every run).

- **Tuple-key point reads and tuple-key join chains** (`pk`, `chain3`, `chain4`) gain nothing from
  replay. Replay on against replay off is -0.2 % to -1.1 %, inside the spread in every cell.
- **What Waystone costs on those cells.** Arm A (the default) against arm C (both off) is 2.0-3.0 %
  fewer statements per second and 2.7-3.9 % more server CPU per statement, outside the spread on
  `chain3` and `pk`. The recording is that cost: B against C costs 2.0-3.7 % of server CPU per
  statement, with replay adding nothing.
- **A scan-driven chain with many tuple-key probes** (`scanprobe`, scenario 1's backtest
  cross-section shape: a `FilterScan`, then 200 probe rows per statement) is the one shape replay pays
  on. Replay against no replay is +5.7 % throughput and 7.2 % less server CPU. The net, A against C,
  is +5.0 % and 6.4 % less CPU. Both are outside the spread. Recording costs it nothing measurable
  (B against C is inside the spread).
- **The position control** (`control`, a `FilterScan` with no recordable step) moves 0.1-0.6 %
  between arms, inside its spread. Rotation balances position across arms, and the control shows no
  arm or position effect here.
- **Every reply matches across arms.** Every cell's reply digest is identical across all 18 runs
  (invariant 8).

**Against CF-Q1 (a)**, as marked: **the bar is not met.** It is one conjunction, and its last clause
fails.
- **Its two conditions hold on `scanprobe` alone.** Replay's gain and the net are both outside the
  larger spread, in throughput and in server CPU, and `replays=200`. They hold on no `pk`, `chain3` or
  `chain4` cell.
- **Whether `scanprobe` is "a tuple-key chain cell" is a reading, not a fact.** The order's §1.1 names
  "a tuple-key join chain" and "a scan that probes by tuple key" as two shapes, and `scanprobe` is
  the second. The bar's wording does not say which it admits.
- **"No cell regresses outside its spread" fails.** With Waystone on everywhere (arm A), `chain3` and
  `pk` regress against C outside their spreads, in throughput and in server CPU.

So the bar does not support the present default, and it does not by itself pick a branch. Beside the
bar, the numbers say two things. First, replay pays on one shape, which is §2.3's condition for
Branch O to be admissible, if the operator counts that shape. Second, the default costs point reads
and short tuple-key chains 2.7-3.9 % of server CPU for nothing. Branch D keeps the gain reachable only
instance-wide, where turning Waystone on brings that cost back. **CF-Q0 (a) leaves the branch to the
operator.**

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-21. Driver launched 07:46:39 UTC. The quiet gate held run 1 until 08:01:40 for another session's `ctest` (`ddl-build-yield`). That is the gate's 900 s cap (`--quiet-wait`), reached with the competing list empty at its last sample; the gate samples only before each run. The 18 runs then took 08:01:40-08:04:06 UTC |
| Worktree / branch | `cf-waystone-premise` / `worktree-cf-waystone-premise` |
| `git describe --tags` | `v3.3.0-97-g3d628df4` (HEAD at the run). The harness files were uncommitted then and are committed with this file |
| Engine code | identical to `a1809988` (`v3.3.0-96-ga1809988`, `origin/main` merged into the branch). `3d628df4` changes `instructions/` only |
| Binary | `build-release/kds_server` (mtime 07:45:13 UTC, after `3d628df4`), copied to `/home/cdkbs/bench-runs/cf/kds_server.cf`, sha256 `ee793c2d21b1755d7ee1d1ee65c0f35806506a83de52e9dda6a60058cef4f664`. Every server started from the copy |
| Build type | Release (`cmake -DCMAKE_BUILD_TYPE=Release`), no frame pointers |
| Device | `/dev/root`, ext4; fresh data file per run under `/home/cdkbs/bench-runs/cf/data`, deleted after it. Not tmpfs |
| Host | AMD EPYC 9V74, 8 CPUs; loadavg 2.2-2.6 at every run's start. `pgrep cc1plus\|cmake --build\|ctest ` was empty at every run's start (the gate waited for it); other long-lived servers on the host (`autotrade`, `web-ckdbs` sandbox) were idle and not stopped |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, plus the arm's two keys. Server pinned to CPU 2 (`taskset`), driver to CPU 3 |
| Arms | **A** `waystone_recording = true`, `waystone_replay = true` (the default); **B** recording true, replay false; **C** both false (CF-Q2 (a)) |
| Order | ABC, BCA, CAB, ABC, BCA, CAB: 6 reps per arm, each arm first, second and third twice |
| Watchdog | `run.sh`'s detached 60 s watchdog (progress-file age, driver liveness) plus a session relay. No relaunch was needed |

## Method and every cut

- **One server per (arm, rep).** Each starts on a fresh volume and loads the same relations:
  - `a` and `b`: 20,000 rows each; `c` and `d`: 2,000 each. Each row names the next relation's tuple
    key through a scrambled column, so a chain does not walk adjacent leaves.
  - `t`: 10,000 rows, 100 sessions of 100 rows; `session_no` is unindexed.
  - `bar`: 20,000 rows; `sym`: 100 rows.
  - All relations are `BTREE`, loaded by 500-row `INSERT`s.
- **Cells**, all literal SQL over one connection:
  - `chain3`: `a JOIN b JOIN c ... WHERE a.id = k`, which `ANALYZE` shows as Lookup a, Probe b,
    Probe c.
  - `chain4`: `chain3` plus Probe d.
  - `pk`: `SELECT * FROM a WHERE id = k`.
  - `scanprobe`: `t JOIN bar JOIN sym ... WHERE t.session_no = s`, shown as FilterScan t, Probe bar,
    Probe sym. It returns 100 rows, so 200 recorded entries, inside the one-page trail of 253.
  - `control`: `SELECT ... FROM t WHERE t.session_no = s`, a FilterScan alone, which never touches a
    trail.
- **Working set and passes.** The working set is 1,000 keys for the first three cells and 50 sessions
  for the last two, below the 4,096-instance sighting table. Each cell runs 22 passes:
  - Passes 1 and 2 are warm-up and excluded. Pass 1 is the first sighting, pass 2 records.
  - Passes 3-22 are measured: 20,000 statements per lookup cell and 1,000 per scan cell, per run.
- **Metrics.**
  - Throughput is measured statements over measured wall time, from a serial driver.
  - Server CPU per statement is the sum of every server thread's `/proc/<pid>/task/*/schedstat`
    on-CPU time over the measured passes, divided by statements.
  - Each cell's median is taken over its 6 reps. Spread is (max - min) / median. `~` marks a delta
    inside the larger of the two arms' spreads.
- **Checks.**
  - **Reply digest:** a sha256 of the last pass's sorted rows, compared across all 18 runs per cell.
  - **Replays:** one `ANALYZE` of the cell's first key after the sweep, summed `replays=`.
- **Cuts to fit the 20-minute budget.** The run itself took 2.5 minutes; the time went to the quiet
  gate. What was not measured:
  - **CF-S0's cell 4 is not scenario 1's driver.** Its `backtest-replay` shape is reproduced on
    synthetic relations as `scanprobe`, and the driver was not run. **Cell 5's `read-day-slice` is
    likewise the synthetic `control`.** The shapes are the same chain kinds, but the sizes and data are
    not scenario 1's.
  - No relation-size sweep: every relation fits the pool and every tree is shallow (at most 20,000 rows), so a
    descent is as cheap as it gets.
  - No cold pool and no `cores > 1`, and one connection only (sightings are per core, trails are the
    instance's).
  - Latency is reported as p50 only; p99 is in the raw files. This is a cost-and-gain measurement, not
    a latency study, so no wait decomposition was made.

## Per arm (median of 6 reps)

| cell | arm | reps | qps | spread | server CPU us/stmt | spread | p50 us | replays (ANALYZE) |
|---|---|---|---|---|---|---|---|---|
| chain3 | A | 6 | 10,084 | 1.8% | 57.20 | 3.0% | 95.9 | 3 |
| chain3 | B | 6 | 10,134 | 1.9% | 56.95 | 2.2% | 95.7 | 0 |
| chain3 | C | 6 | 10,298 | 1.6% | 55.53 | 1.4% | 94.0 | 0 |
| chain4 | A | 6 | 9,565 | 2.5% | 60.72 | 2.9% | 101.2 | 4 |
| chain4 | B | 6 | 9,628 | 1.4% | 60.25 | 2.0% | 100.8 | 0 |
| chain4 | C | 6 | 9,760 | 1.2% | 59.10 | 2.4% | 99.2 | 0 |
| pk | A | 6 | 12,167 | 2.8% | 42.28 | 2.2% | 78.9 | 1 |
| pk | B | 6 | 12,308 | 0.8% | 42.19 | 2.5% | 78.0 | 0 |
| pk | C | 6 | 12,545 | 2.5% | 40.68 | 2.3% | 76.8 | 0 |
| scanprobe | A | 6 | 1,858 | 2.5% | 407.49 | 1.4% | 534.4 | 200 |
| scanprobe | B | 6 | 1,758 | 3.7% | 438.94 | 2.8% | 566.2 | 0 |
| scanprobe | C | 6 | 1,769 | 1.2% | 435.50 | 1.6% | 562.4 | 0 |
| control | A | 6 | 2,429 | 3.0% | 318.98 | 1.9% | 404.6 | 0 |
| control | B | 6 | 2,434 | 3.1% | 317.59 | 3.8% | 402.2 | 0 |
| control | C | 6 | 2,430 | 2.4% | 319.52 | 3.1% | 404.3 | 0 |

## The three comparisons

A gain is positive when the first arm is better (more qps, less CPU). `~` means inside the larger
spread.

| cell | replay gain A/B qps | A/B CPU | net A/C qps | A/C CPU | recording B/C qps | B/C CPU |
|---|---|---|---|---|---|---|
| chain3 | -0.5%~ | -0.4%~ | -2.1% | -3.0% | -1.6%~ | -2.6% |
| chain4 | -0.7%~ | -0.8%~ | -2.0%~ | -2.7%~ | -1.4%~ | -2.0%~ |
| pk | -1.1%~ | -0.2%~ | -3.0% | -3.9% | -1.9%~ | -3.7% |
| scanprobe | +5.7% | +7.2% | +5.0% | +6.4% | -0.6%~ | -0.8%~ |
| control | -0.2%~ | -0.4%~ | -0.1%~ | +0.2%~ | +0.1%~ | +0.6%~ |

Reply digests are identical across all 18 runs in every cell. Arm A's `replays=` is 3, 4, 1, 200
and 0 by cell; B's and C's are 0 everywhere.

## What the numbers say about the engine

- **Replay saves too little per entry to pay for reading a short trail.** On `scanprobe`, A against B
  saves about 31 us of server CPU per statement over 200 replayed probes, about 0.16 us per probe.
  That is net: what the skipped descents save less what reading and indexing a 200-entry trail
  costs, so it is a lower bound on the gross saving per probe. On the one-to-four-entry cells A
  against B is +0.1 to +0.5 us, inside the spread. Whatever those entries save, the fixed cost of
  reading them (walk the directory, read the trail page, build the replay index) takes about all of
  it. Only a statement with hundreds of probes gets ahead.
- **Recording's cost is mostly fixed per statement.** B against C is about 1.2-1.5 us per statement
  on the lookup cells, 2-4 % of a 40-60 us statement. On `scanprobe`, with 200 entries, it is 3.4 us,
  inside that cell's spread, so the per-entry part is small. Every execution from the second
  re-records (walks the directory and reads the trail page); it writes only when the entries changed.
  That fixed cost is what makes the default a loss on point reads and short chains.
- **This is the shallowest case for a descent.** A deeper tree or a colder pool raises what a descent
  costs, and so what a replay can save. But a replay still reads the leaf, so it saves no leaf I/O.
  This run did not measure either; the gain on `scanprobe` is a floor for warm, shallow trees, not a
  ceiling.
- **CA-S0's scenario-1 movement is not explained by this run.** CA-S0's walked shapes could not
  replay (§1.2 of the order), and the rotated `control` here moves nothing. That is consistent with
  position or noise but does not show it: CA-S0 also turned `access_statistics` off, which this run
  does not touch.
