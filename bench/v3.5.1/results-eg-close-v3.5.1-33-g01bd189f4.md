# EG close: the overhead of the `commit.wal_flush` wait scope on a strict commit

**One line.** The change's intended effect is in the data and nothing else is: NEW books **500 waits over 500 `strict` one-core inserts (about 121 µs a wait, 83 % of the insert's 146 µs mean)** where OLD books 0, and no cost of the two clock reads and the relaxed stores can be told from the run's noise on any cell. One client's insert reads 146.1 µs NEW against 156.5 µs OLD at the median of five rounds, **but both arms' spreads are above 100 % because round 2 ran 2.4x slow in both arms**, so the cell carries no claim; with that round set aside (a post-hoc view, labelled) the two arms are 146.0 µs and 153.8 µs on spreads of 0.5 % and 12.1 %, inside the larger spread. The booking rate at one core `strict` is 2,108 NEW against 2,098 OLD bookings a second (+0.5 %, spreads 0.8 % and 2.2 %, inside the spread); the `strict` path above one core that EG-R3 says must not move reads 9,636 against 9,507 (OLD spread 54.5 %: two of its five rounds ran at half speed; no claim); and the control the change cannot reach, scenario 3 at `group`, is inside the spread on its `loans-by-user` and `count-by-user` reads and carries no claim on `pk-user`.

Measured by `ck-tester` for milestone EG's close. The comparator is this engine's own previous state, the OLD binary, in the same window, interleaved. PostgreSQL is not part of this run. The change measured is one `sched::WaitScope` on `commit.wal_flush` around the `Sync()` that `WalManager::Commit`'s `strict` arm runs on the caller's stack (`src/wal/manager.cpp`); work order `instructions/v3.2.0/workorder-eg-one-client-durable-write.md`.

## Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-28 03:23:46 UTC to 03:29:18 UTC (about 5.5 minutes of measurement; 40 cells; the ordered rounds ended 03:27:53, and rounds 4 and 5 of cells 2 to 4 were added after them) |
| Branch, commit | worktree `eg-one-client-durable-write`, branch `worktree-eg-one-client-durable-write`, at `01bd189f4` (`v3.5.1-33-g01bd189f4`), tree clean at the start of the run |
| OLD | Release, built from `git archive 72580aa04` outside every worktree: the engine the milestone opened from (`git diff 72580aa04 d50d304f8 -- src include tests CMakeLists.txt scripts` is empty, `d50d304f8` being the opening commit). Source binary mtime 2026-11-27 22:09:37 -0500; the copy run is `kds_server.old`, sha256 `325a5af1db29268228e22a912daeadbe44baaab70e3bac404810638c49809310` |
| NEW | Release, built from `git archive 9830e6bb0`. **`git diff 9830e6bb0 01bd189f4 -- src include CMakeLists.txt` is empty**, so the binary builds the engine at `01bd189f4` too. Source binary mtime 2026-11-27 22:22:22 -0500, after the commit time of `9830e6bb0` (22:21:26) and before that of `01bd189f4` (22:22:42); the copy run is `kds_server.new`, sha256 `3560324f74422c0b103344bd16dd89c208afbfa09182222f738df7f7e9146487` |
| Copies | both binaries copied into the run directory `/home/ukjin/bench-runs/eg-close-v3.5.1-33-g01bd189f4/` before the first cell and every server started from the copy; hashes of the binaries, the drivers and `bench_common.py` are in the archive's `hashes.txt` |
| Build type | Release (both) |
| Device | `/dev/mapper/fedora-root`, xfs, under `/home/ukjin/bench-runs/` (`df -T` at run time in the archive's `device.txt`); the data file and the log are there, never tmpfs |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs, P/E hybrid (P-cores 0 to 7); every server `taskset -c 0-7`, every driver `taskset -c 8-23`; kernel 7.3.6-300.fc45; another project's server runs on the host throughout |
| Server configuration | `buffer_pool_frames = 65536`, `log_level = warn`, everything else at its default; cells 1 and 2 `cores = 1`, `durability = strict`; cell 3 `cores = 8`, `strict`; cell 4 `cores = 1`, `group`. A fresh server and a fresh data file a cell, deleted afterwards |
| Quiet gate | before every cell: no `cc1plus`, `cmake --build`, `ctest` or `kds_tests` process and at most 8 % of all CPUs busy over 3 s. **Every one of the 40 cells passed at once (gate wait 0 s)**, busy over the gate's 3 s 0 % to 1 %, 1-minute load 0.44 to 1.61 at the gate, above 1.0 only in round 1 (the archive's `logs/*.precheck`) |
| Order | cells run round by round in the order 1, 2, 3, 4; OLD first in odd rounds, NEW first in even ones |
| Exit status | all 40 runs exit 0 (`logs/*.rc`) |
| Verdict rule | spread = (max - min) / median over a cell's rounds, one per arm; a difference of medians smaller than the larger spread is **inside the spread** and no finding; a spread over 20 % **carries no claim** |
| Suite | not executed by this agent: the run changes no build input |

## Which client each driver times

The comparison is KDS against KDS, so both arms pay the same client and no ratio here names another engine. Cell 1's driver, `tools/eg_s0_premise.py`, times `kwp.Connection.query` per insert (the KWP client, one autocommit statement a call). Cells 2 to 4's drivers, `scenario2_freight.py` and `scenario3_library.py`, record `meta.timed_client = {"reads": "rows", "writes": "count"}`: a read is timed through to its decoded rows and a write through to its count reply (`bench/README.md`, "A ratio against another engine names both clients"). The client's own CPU is in both arms identically and cancels in a delta; it is not subtracted from any absolute figure here.

## Cell 1: the changed path, one client, one core, `strict`, autocommit one-row inserts

The cell's finding is the booking. NEW books every one of 500 commits under `commit.wal_flush` and OLD books none; `d_wal_syncs` is 500 in both arms, so the work is the same and only the accounting differs. A 500-insert round books about 60.6 ms of wait, 121 µs a commit, out of a 146 µs mean insert: **the insert is a sync of about 121 µs, about 14 µs of server CPU (all server threads) and a remaining roughly 11 µs of client and socket round trip** (derived by subtraction from the mean, not measured separately).

The overhead claim is about the mean, the percentiles and the server CPU, and the rounds do not carry one: **in round 2 both arms ran slow in every state** (idle mean 361.8 µs OLD and 368.7 µs NEW against 138.8 µs to 157.5 µs in the other rounds; after-load 409.0 and 415.5 µs against 202.5 to 222.9 µs), in the window 03:24:57 to 03:25:22 UTC, with the gate reading 0 % to 1 % busy and a load of 0.82 to 0.96 at its start. NEW's `recycled` state shows the same slow round 2 (404.7 µs) where OLD's does not. The cause is not established: it hit both arms in one window, which points at the host and not the binary, but nothing in the run names it. With one round in five at 2.4x, the median-based spreads of the mean and the derived rate are 53 % to 163 % (the larger arm's, by state) and the rule withholds the claim on every mean and on every percentile above p0.

The post-hoc four-round view (rounds 1, 3, 4, 5, chosen only because round 2 is the slow one in both arms; not the rule's, a view and not a result) follows the full tables.

#### Cell 1, `idle` (5 rounds a binary, 500 inserts a round)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 6389 | 6843 | +7.1 % | 69.5 % / 60.6 % | no claim (spread >20%) |
| mean_us | 156.5 | 146.1 | -6.6 % | 142.4 % / 152.5 % | no claim (spread >20%) |
| p0_us | 130.9 | 130.5 | -0.3 % | 6.3 % / 8.3 % | inside the spread |
| p25_us | 140.5 | 134.7 | -4.1 % | 16.5 % / 23.5 % | no claim (spread >20%) |
| p50_us | 146.1 | 137.7 | -5.7 % | 27.0 % / 31.5 % | no claim (spread >20%) |
| p95_us | 202.9 | 171.2 | -15.6 % | 78.5 % / 119.0 % | no claim (spread >20%) |
| p99_us | 342.1 | 270.1 | -21.0 % | 3135.8 % / 3905.1 % | no claim (spread >20%) |
| server CPU us/insert | 14.66 | 14.54 | -0.9 % | 97.4 % / 137.5 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [60837, 151049, 60583, 59814, 60059] | | | |
| booked us per wait (NEW) | | [121.7, 302.1, 121.2, 119.6, 120.1] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [138.8, 361.8, 157.5, 151.1, 156.5] | [146.6, 368.7, 145.8, 146.1, 145.8] | | | |
| per-round server CPU us/insert | [12.1, 26.38, 18.09, 13.84, 14.66] | [11.99, 31.98, 12.85, 14.54, 14.88] | | | |

#### Cell 1, `after-load` (5 rounds a binary, 500 inserts a round)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 4665 | 4820 | +3.3 % | 53.4 % / 52.8 % | no claim (spread >20%) |
| mean_us | 214.4 | 207.4 | -3.2 % | 96.3 % / 103.0 % | no claim (spread >20%) |
| p0_us | 183.6 | 183.2 | -0.3 % | 3.3 % / 4.4 % | inside the spread |
| p25_us | 198.5 | 193.7 | -2.4 % | 18.9 % / 23.6 % | no claim (spread >20%) |
| p50_us | 205.8 | 197.9 | -3.9 % | 25.1 % / 27.5 % | no claim (spread >20%) |
| p95_us | 242.4 | 240.5 | -0.8 % | 62.6 % / 79.8 % | no claim (spread >20%) |
| p99_us | 454.4 | 367.5 | -19.1 % | 2311.4 % / 2846.5 % | no claim (spread >20%) |
| server CPU us/insert | 16.02 | 12.27 | -23.4 % | 107.2 % / 168.1 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [87975, 171535, 89648, 89549, 88945] | | | |
| booked us per wait (NEW) | | [175.9, 343.1, 179.3, 179.1, 177.9] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [202.5, 409.0, 222.9, 214.4, 208.2] | [201.9, 415.5, 208.5, 202.5, 207.4] | | | |
| per-round server CPU us/insert | [14.55, 29.52, 16.02, 17.79, 12.34] | [11.05, 31.56, 13.64, 10.93, 12.27] | | | |

#### Cell 1, `recycled` (5 rounds a binary, 500 inserts a round)

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| inserts/s (derived, 1e6/mean_us) | 6525 | 6348 | -2.7 % | 7.1 % / 67.3 % | no claim (spread >20%) |
| mean_us | 153.3 | 157.5 | +2.8 % | 7.3 % / 162.7 % | no claim (spread >20%) |
| p0_us | 134.2 | 132.2 | -1.5 % | 8.0 % / 11.0 % | inside the spread |
| p25_us | 138.1 | 136.9 | -0.8 % | 8.1 % / 27.7 % | no claim (spread >20%) |
| p50_us | 142.3 | 143.4 | +0.8 % | 7.1 % / 41.8 % | no claim (spread >20%) |
| p95_us | 182.5 | 189.7 | +3.9 % | 36.3 % / 120.3 % | no claim (spread >20%) |
| p99_us | 380.1 | 393.7 | +3.6 % | 25.2 % / 3383.9 % | no claim (spread >20%) |
| server CPU us/insert | 14.08 | 12.89 | -8.4 % | 17.9 % / 149.4 % | no claim (spread >20%) |
| d_commit_wal_flush_count per round | [0, 0, 0, 0, 0] | [500, 500, 500, 500, 500] | | | |
| d_commit_wal_flush_us per round | [0, 0, 0, 0, 0] | [61993, 163789, 60615, 60756, 59892] | | | |
| booked us per wait (NEW) | | [124.0, 327.6, 121.2, 121.5, 119.8] | | | |
| d_wal_syncs per round | [500, 500, 500, 500, 500] | [500, 500, 500, 500, 500] | | | |
| per-round mean_us | [150.4, 154.5, 161.1, 149.9, 153.3] | [157.5, 404.7, 159.6, 151.5, 148.4] | | | |
| per-round server CPU us/insert | [12.69, 14.25, 13.56, 14.08, 15.21] | [15.72, 31.81, 12.89, 12.73, 12.55] | | | |

**The four-round view with round 2 set aside, labelled post hoc.**

| state | metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|---|
| idle | mean_us | 153.84 | 146.00 | -5.1 % | 12.1 % / 0.5 % | inside the spread |
| idle | p0_us | 130.66 | 130.21 | -0.3 % | 0.6 % / 3.2 % | inside the spread |
| idle | p25_us | 137.27 | 134.32 | -2.1 % | 5.7 % / 2.3 % | inside the spread |
| idle | p50_us | 141.30 | 136.65 | -3.3 % | 10.9 % / 2.5 % | inside the spread |
| idle | p95_us | 191.73 | 170.97 | -10.8 % | 29.9 % / 5.7 % | no claim (spread >20%) |
| idle | p99_us | 339.53 | 263.15 | -22.5 % | 40.8 % / 27.1 % | no claim (spread >20%) |
| idle | server CPU us/insert | 14.25 | 13.69 | -3.9 % | 42.0 % / 21.1 % | no claim (spread >20%) |
| after-load | mean_us | 211.27 | 205.00 | -3.0 % | 9.7 % / 3.2 % | inside the spread |
| after-load | p0_us | 183.51 | 183.18 | -0.2 % | 1.7 % / 0.8 % | inside the spread |
| after-load | p25_us | 196.29 | 192.47 | -1.9 % | 5.6 % / 3.9 % | inside the spread |
| after-load | p50_us | 202.22 | 196.94 | -2.6 % | 7.5 % / 3.4 % | inside the spread |
| after-load | p95_us | 239.78 | 239.46 | -0.1 % | 38.0 % / 4.3 % | no claim (spread >20%) |
| after-load | p99_us | 453.74 | 331.10 | -27.0 % | 37.8 % / 65.7 % | no claim (spread >20%) |
| after-load | server CPU us/insert | 15.29 | 11.66 | -23.7 % | 35.7 % / 23.2 % | no claim (spread >20%) |
| recycled | mean_us | 151.81 | 154.49 | +1.8 % | 7.4 % / 7.3 % | inside the spread |
| recycled | p0_us | 133.13 | 131.94 | -0.9 % | 8.0 % / 3.3 % | inside the spread |
| recycled | p25_us | 136.38 | 136.89 | +0.4 % | 8.2 % / 3.0 % | inside the spread |
| recycled | p50_us | 140.90 | 141.35 | +0.3 % | 7.2 % / 4.9 % | inside the spread |
| recycled | p95_us | 186.11 | 179.96 | -3.3 % | 35.6 % / 42.7 % | no claim (spread >20%) |
| recycled | p99_us | 385.19 | 387.67 | +0.6 % | 24.9 % / 41.9 % | no claim (spread >20%) |
| recycled | server CPU us/insert | 13.82 | 12.81 | -7.3 % | 18.2 % / 24.8 % | no claim (spread >20%) |

Read that way, the mean and p0 to p50 of every state are inside the spread (idle mean 146.0 µs NEW against 153.8 µs OLD, -5.1 % on spreads of 0.5 % and 12.1 %); what stays outside the rule is the upper tail (p95, p99) and the server CPU, whose spreads are 21 % to 66 %. **No cell shows NEW slower by more than a spread**; most signs are negative, the opposite of an overhead, and a median 5 % faster for NEW on the idle mean is inside OLD's own 12 % spread and is not a finding. What the run supports is that the scope's cost is below what five rounds of a 14 µs-CPU, 146 µs insert can resolve; a bound tighter than OLD's 12 % spread on this cell is not available from it.

## Cell 2: a booking at one core, `strict` (scenario 2, one booker)

Command: `scenario2_freight.py --port P --suffix <tag> --organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1 --bookers 1 --sync --json out.json`. Five rounds a binary (three were asked for; rounds 4 and 5 were added). **The booking rate and every phase are inside the spread**, on spreads of 0.5 % to 4 % at the body percentiles. This is the cell with the tightest rounds, so the best evidence for the scope's cost on the path it touches: the `commit` phase (one `strict` sync a booking) is 134.6 µs OLD and 134.8 µs NEW on 1.6 % and 2.4 % spreads, a +0.1 % difference, so any cost on a commit's mean is below about 2 % of 135 µs, roughly 3 µs. The `--verify 100` pass reports 400 checks and 0 failures in all 10 runs. Throughputs below are derived (`1e6 / mean_us`) except `meta.tps`, which the driver reports.

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| booking rate meta.tps (bookings/s) | 2098 | 2108 | +0.5 % | 2.2 % / 0.8 % | inside the spread |
| per-round tps | [2079, 2073, 2120, 2100, 2098] | [2103, 2115, 2101, 2108, 2117] | | | |

**phase `commit`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7429 | 7418 | -0.1 % | 1.5 % / 2.5 % | inside the spread |
| mean_us | 134.6 | 134.8 | +0.1 % | 1.6 % / 2.4 % | inside the spread |
| p0_us | 121.1 | 121.5 | +0.3 % | 5.8 % / 5.1 % | inside the spread |
| p25_us | 128.6 | 128.9 | +0.2 % | 0.9 % / 0.5 % | inside the spread |
| p50_us | 129.4 | 129.7 | +0.2 % | 0.7 % / 0.5 % | inside the spread |
| p95_us | 154.4 | 153.6 | -0.5 % | 1.2 % / 1.1 % | inside the spread |
| p99_us | 170.1 | 167.3 | -1.6 % | 8.4 % / 4.4 % | inside the spread |

**phase `freight-insert`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 47393 | 48077 | +1.4 % | 3.7 % / 2.9 % | inside the spread |
| mean_us | 21.1 | 20.8 | -1.4 % | 3.8 % / 2.9 % | inside the spread |
| p0_us | 18.4 | 18.4 | +0.0 % | 2.2 % / 1.1 % | inside the spread |
| p25_us | 20.0 | 19.9 | -0.5 % | 2.0 % / 3.0 % | inside the spread |
| p50_us | 20.3 | 20.2 | -0.5 % | 2.5 % / 1.0 % | inside the spread |
| p95_us | 21.9 | 21.5 | -1.8 % | 10.0 % / 5.1 % | inside the spread |
| p99_us | 31.0 | 30.7 | -1.0 % | 15.8 % / 11.1 % | inside the spread |

**phase `cargo-lookup`** (ops/round 3402; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 39216 | 39841 | +1.6 % | 2.8 % / 2.8 % | inside the spread |
| mean_us | 25.5 | 25.1 | -1.6 % | 2.7 % / 2.8 % | inside the spread |
| p0_us | 20.8 | 20.9 | +0.5 % | 1.0 % / 1.4 % | inside the spread |
| p25_us | 24.3 | 24.2 | -0.4 % | 1.6 % / 3.3 % | inside the spread |
| p50_us | 24.6 | 24.6 | +0.0 % | 2.0 % / 1.2 % | inside the spread |
| p95_us | 26.6 | 26.3 | -1.1 % | 8.6 % / 4.9 % | inside the spread |
| p99_us | 38.6 | 35.0 | -9.3 % | 24.9 % / 43.4 % | no claim (spread >20%) |

## Cell 3: above one core, `strict`, `cores = 8`, eight bookers (EG-R3)

Command: the cell 2 arguments with `--bookers 8`. Five rounds a binary. EG-R3 says this path must not move: the scope sits on the caller's stack at the inline `Sync()`, which a `strict` commit above one core does not take (it parks and publishes after the writer's sync). The result **agrees with no movement but cannot show it by the rule**: the booking rate is 9,636 NEW against 9,507 OLD at the median, but OLD's rounds 1 and 5 ran at 4,725 and 4,610 a second while its other three ran at 9,507 to 9,786, and all five NEW rounds ran at 9,572 to 9,860. OLD's 54.5 % spread withholds the claim. The two slow OLD rounds were both OLD-first rounds (1 and 5) and the gate showed a quiet host at each; a half-speed mode is the shape the project's hybrid-CPU note describes for unpinned work, but both sides were pinned here, so that explanation is unverified. The full-speed rounds overlap between the arms (OLD 9,507 to 9,786, NEW 9,572 to 9,860), which is what no movement looks like and is not a measurement of it.

Two things beside the rates are not the change's. **Scenario 2's `--verify` line reports client-side lost-update failures under READ COMMITTED with eight bookers**, known and not the engine: OLD 27, 29, 15, 12, 19 and NEW 22, 23, 25, 20, 20 failures of 400 checks (rounds 1 to 5). They are neither a pass nor a failure of the change. And the driver reports **retryable `TXN_CONFLICT` error replies from its contended `operation-update` and `org-update` phases, in both arms**: OLD 44, 50, 47, 35, 45 and NEW 53, 45, 55, 36, 53 a run (rounds 1 to 5). The three phases tabled below show 0 errors in every round, but the brief's "0 errors" does not hold for this cell in either arm; the exit status is 0 in all runs.

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| booking rate meta.tps (bookings/s) | 9507 | 9636 | +1.4 % | 54.5 % / 3.0 % | no claim (spread >20%) |
| per-round tps | [4725, 9660, 9786, 9507, 4610] | [9636, 9860, 9638, 9572, 9607] | | | |

**phase `commit`** (ops/round 3000; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 4810 | 4900 | +1.9 % | 81.0 % / 3.8 % | no claim (spread >20%) |
| mean_us | 207.9 | 204.1 | -1.8 % | 338.7 % / 3.8 % | no claim (spread >20%) |
| p0_us | 126.9 | 127.0 | +0.1 % | 2.5 % / 2.0 % | inside the spread |
| p25_us | 169.3 | 165.0 | -2.5 % | 5.4 % / 1.9 % | inside the spread |
| p50_us | 206.2 | 202.0 | -2.0 % | 7.2 % / 2.6 % | inside the spread |
| p95_us | 288.0 | 279.3 | -3.0 % | 3649.1 % / 9.3 % | no claim (spread >20%) |
| p99_us | 341.7 | 337.1 | -1.3 % | 3204.9 % / 13.2 % | no claim (spread >20%) |

**phase `freight-insert`** (ops/round 3044; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 33784 | 36630 | +8.4 % | 23.9 % / 4.7 % | no claim (spread >20%) |
| mean_us | 29.6 | 27.3 | -7.8 % | 24.3 % / 4.8 % | no claim (spread >20%) |
| p0_us | 17.0 | 16.8 | -1.2 % | 14.1 % / 4.2 % | inside the spread |
| p25_us | 21.2 | 21.1 | -0.5 % | 4.2 % / 1.4 % | inside the spread |
| p50_us | 22.7 | 22.5 | -0.9 % | 6.2 % / 2.7 % | inside the spread |
| p95_us | 41.5 | 36.6 | -11.8 % | 24.1 % / 25.4 % | no claim (spread >20%) |
| p99_us | 143.6 | 149.8 | +4.3 % | 138.0 % / 63.8 % | no claim (spread >20%) |

**phase `cargo-lookup`** (ops/round 3389; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 30303 | 30675 | +1.2 % | 34.0 % / 4.4 % | no claim (spread >20%) |
| mean_us | 33.0 | 32.6 | -1.2 % | 45.2 % / 4.3 % | no claim (spread >20%) |
| p0_us | 21.4 | 21.4 | +0.0 % | 5.6 % / 7.0 % | inside the spread |
| p25_us | 25.8 | 25.7 | -0.4 % | 5.4 % / 1.2 % | inside the spread |
| p50_us | 27.2 | 27.2 | +0.0 % | 8.5 % / 0.7 % | inside the spread |
| p95_us | 43.2 | 40.8 | -5.6 % | 372.5 % / 19.9 % | no claim (spread >20%) |
| p99_us | 215.5 | 199.9 | -7.2 % | 132.4 % / 65.1 % | no claim (spread >20%) |

## Cell 4: the control the change cannot reach (scenario 3, `group`, `cores = 1`)

Command: `scenario3_library.py --port P --suffix <tag> --loans 10000 --index-mode none --server-indexes on --seed 1 --json out.json`; the row counts are the driver's (2,000 users, 2,000 books, 5,000 reservations, 10,000 loans), 200 operations a shape, five rounds a binary. `group` does not take the `strict` inline sync, so the scope is not on this cell's path, and the table reads as the run's noise floor on an unreachable path. **`loans-by-user` and `count-by-user` (a scan and a count, 128 µs to 137 µs) are inside the spread on 0.5 % to 5 % spreads; `pk-user` (about 27 µs, 200 operations) spreads 20 % to 39 % and carries no claim.** That a 27 µs point read spreads 21 % to 28 % between rounds of identical engines is the floor for a short read at 200 operations here.

**phase `pk-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 36900 | 35714 | -3.2 % | 20.5 % / 27.7 % | no claim (spread >20%) |
| mean_us | 27.1 | 28.0 | +3.3 % | 22.9 % / 28.2 % | no claim (spread >20%) |
| p0_us | 22.8 | 25.1 | +10.1 % | 21.5 % / 38.6 % | no claim (spread >20%) |
| p25_us | 25.3 | 25.8 | +2.0 % | 19.8 % / 36.4 % | no claim (spread >20%) |
| p50_us | 26.1 | 26.2 | +0.4 % | 26.8 % / 34.4 % | no claim (spread >20%) |
| p95_us | 28.0 | 28.4 | +1.4 % | 30.7 % / 28.9 % | no claim (spread >20%) |
| p99_us | 33.9 | 33.2 | -2.1 % | 33.3 % / 26.5 % | no claim (spread >20%) |

**phase `loans-by-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7337 | 7321 | -0.2 % | 2.6 % / 5.1 % | inside the spread |
| mean_us | 136.3 | 136.6 | +0.2 % | 2.6 % / 5.2 % | inside the spread |
| p0_us | 128.6 | 128.0 | -0.5 % | 1.3 % / 1.6 % | inside the spread |
| p25_us | 132.8 | 132.3 | -0.4 % | 0.8 % / 1.0 % | inside the spread |
| p50_us | 135.2 | 134.4 | -0.6 % | 0.5 % / 0.8 % | inside the spread |
| p95_us | 143.1 | 143.4 | +0.2 % | 25.1 % / 44.1 % | no claim (spread >20%) |
| p99_us | 161.5 | 166.9 | +3.3 % | 27.6 % / 38.3 % | no claim (spread >20%) |

**phase `count-by-user`** (ops/round 200; errors OLD [0, 0, 0, 0, 0] NEW [0, 0, 0, 0, 0])

| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |
|---|---|---|---|---|---|
| ops/s (derived, 1e6/mean_us) | 7628 | 7686 | +0.8 % | 2.4 % / 3.4 % | inside the spread |
| mean_us | 131.1 | 130.1 | -0.8 % | 2.4 % / 3.4 % | inside the spread |
| p0_us | 126.3 | 125.6 | -0.6 % | 2.2 % / 3.6 % | inside the spread |
| p25_us | 128.9 | 128.3 | -0.5 % | 1.7 % / 3.6 % | inside the spread |
| p50_us | 129.9 | 129.6 | -0.2 % | 2.0 % / 3.8 % | inside the spread |
| p95_us | 135.8 | 134.7 | -0.8 % | 4.8 % / 3.7 % | inside the spread |
| p99_us | 139.3 | 138.7 | -0.4 % | 71.4 % / 1.8 % | no claim (spread >20%) |

## What the run teaches

1. **The scope's cost is not resolvable.** On the path it touches (cell 1, cell 2's `commit` phase) the mean difference is -6.6 % to +2.8 % across the three cell 1 states and +0.1 % on the commit phase, none outside a spread that passes the rule, and cell 2's tight rounds bound a commit's mean difference to about 2 % of 135 µs. Two clock reads and a few relaxed stores are tens of nanoseconds against a 121 µs sync, about 0.05 % of a commit (an estimate from the operation count, not a measurement; confidence: medium).
2. **The wait booking is now the sync.** Where a one-core `strict` commit's roughly 120 µs of the 146 µs insert appeared in no wait statistic, NEW's `commit.wal_flush` carries it, 121 µs a wait in the four ordinary rounds and 302 µs in the slow one. That is the order's second matter on the one-core arm, and it is the part of this A/B that is a difference and not a null.
3. **The machine bound two claims, not the engine.** Cell 1's slow round and cell 3's two half-speed OLD rounds each cost a claim, and neither was announced by the gate, the load or the driver's output. A round of cell 1 is about 70 ms of timed work, so one disturbance spoils it; more rounds of the same cell, not longer ones, is what would tighten it.

## What was cut or is unverified

- **Row-count sweep (200, 1K, 10K): not run.** The change is a fixed cost on a commit's sync and does not scale with rows; cell 1 loads 90,000 rows before its `after-load` state, cell 2's scenario is fixed at 3,000 bookings over 4,000 cargos and cell 4 at 10,000 loans. The brief set the shapes.
- **Rounds.** Cell 1 five rounds as asked; cells 2 to 4 five rounds, two more than asked, added because the three asked for took under 4 minutes in all. Nothing was cut for time.
- **Cell 1's round 2 and cell 3's OLD rounds 1 and 5** are unexplained; all are in their headline medians and tables, none removed. The four-round view of cell 1 is post hoc.
- **PostgreSQL** is not part of this run.
- **Cell 3's `--verify` failures and `TXN_CONFLICT` error replies** are in both arms at comparable sizes; their cause (READ COMMITTED lost updates under contention) is the brief's and was not investigated.
- **The correctness suite was not executed** by this agent; the run changes no build input.
- **The scope's cost on `sys.wait_stats` itself** was not isolated; it is bounded only through the insert and commit means above.

## Reproduce

Cell 1: `eg_s0_premise.py --engine kds --port P --pid <server pid> --json out.json`; cells 2 to 4 use the commands above. The orchestration (`run.sh`, the 60 s watchdog, the quiet gate, the interleave), the drivers as run, every raw JSON and log are in `bench/v3.5.1/archive/eg-close-v3.5.1-33-g01bd189f4/`; `analyze.py` there regenerates the tables from the JSON.
