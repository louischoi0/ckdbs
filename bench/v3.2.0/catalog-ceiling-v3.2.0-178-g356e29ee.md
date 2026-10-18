# Catalog ceiling: CREATE TABLE costs 15% less server CPU, the insert and the tuple-key read did not move

The branch HEAD (`356e29ee`, catalog chains grown from the general page supply, superblock 31)
against its base `7e256216` (origin/main). The single finding is on the catalog write path: 500
`CREATE TABLE`s of eight `int64` columns cost 105.3 us of server CPU each on B against 123.8 us on A
(0.850x), and run at 7,526 tables/s against 6,662 (derived, 1.13x). The control arm, A's binary
started a second time, differs from A by 0.7% (CPU) and 0.5% (rate), so the CREATE delta is
outside the noise floor. A single-row `INSERT` and a point `SELECT` by tuple key at 200, 1,000 and
10,000 rows are inside the floor: no regression on any shape measured.

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-18, 14:01:21-14:03:58 UTC (20 runs, ~2.6 min), after a 5-run pilot at 14:00-14:01 whose numbers are not used |
| Worktree / branch | `fix-catalog-page-ceiling` / `worktree-fix-catalog-page-ceiling` |
| `git describe --tags` | B: `v3.2.0-178-g356e29ee`. A: `v3.2.0-172-g7e256216`. A is B's merge-base, 6 commits behind |
| Tree | B built from a clean tree (`git status` empty); A from a detached scratch worktree at `7e256216`, removed afterwards |
| Build type | Release (`-DCMAKE_BUILD_TYPE=Release`), each in its own `build-release`, built before the run and never overlapping it |
| Binaries | copies in `/home/cdkbs/kds-bench-scratch/catalog-ceiling/`, every server started from the copy |
| A (and A2) | sha256 `555d909765977439882592ca9d17847da7fe737bdfb2dc891d015e84e10a0e94`, source mtime 2026-10-18 13:58:04 UTC |
| B | sha256 `0f9ad1fb82416390ab87e9a6cd5fdb3ab1226429ff60d0fe6fd5edeeec63d574`, source mtime 2026-10-18 13:59:56 UTC (HEAD committed 13:55:15 UTC, so the binary is newer than HEAD) |
| Device | `/dev/root`, ext4 (`df -T`), data files and WAL under `$HOME/kds-bench-scratch/catalog-ceiling`, fresh per server per run |
| Host | 8 CPUs (AMD EPYC 9V74), kernel 7.0.0-1014-azure. **Not quiet:** another session's `ctest -j8` (`cc-stats-planner`) ran throughout; loadavg at cell starts 4.7-8.1, 11-13 at launch. No compiler was running during the run. Server pinned to CPU 2, driver to CPU 4 (taskset); the load shows in the wall columns more than in the CPU column |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, port 31970+arm |
| Tests | no code change by this run; the correctness suite was not executed here |

Arms: **A** (base binary), **B** (branch binary), **A2** (A's binary, started separately) as the
in-run control. Each run starts three fresh servers (fresh data file, fresh WAL), loads the read
tables, then runs each shape in 10 interleaved blocks per arm with the arm order rotating per block
and per run. The driver is `archive/catalog-ceiling-v3.2.0-178-g356e29ee/cc_ab.py`, a scratch
script that imports `tools/kwp.py` and `tools/bn_census.py` unmodified; it is archived with the raw
`out.json` because it is not in `tools/`. Invocation: `python3 -I cc_ab.py --arm A=<A> --arm B=<B>
--arm A2=<A copy> --workdir <dir> --runs 20 --out out.json --progress progress`.

## What was measured

| Shape | Statement | Per run, per arm | Rows |
|---|---|---|---|
| create | `CREATE TABLE ctN (c0 int64, ..., c7 int64) BTREE`, autocommit | 500 | does not scale with rows |
| insert | `INSERT INTO ins VALUES (k, v)`, 2-column btree, one row per statement | 2,000 | table grows 0 to 2,000 rows |
| select-200 / 1000 / 10000 | `SELECT * FROM tN WHERE id = k`, tuple-key point read | 2,000 each | 200 / 1,000 / 10,000 |

All statements are literal text over KWP/1 on one connection. The driver has no `--verify`; instead
every statement's tag is checked (`INSERT`/`CREATE`, exactly one row for a SELECT): 0 failures in
every cell.

## Results

Rate is derived, `1,000,000 / mean wall us` per run, then the median over 20 runs; the bracket is
the min-max of the 20 runs. Server CPU is the on-CPU time of all server threads per statement
(`/proc/<pid>/task/*/schedstat`). Percentiles are wall latency as the client sees it, the median
over 20 runs of each run's percentile.

| Shape | Arm | Rate (derived, median [min-max]) | Server CPU / stmt | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|---|
| create | A | 6,662 tables/s [1,941-8,015] | 123.8 us | 75 us | 109 us | 137 us | 240 us | 434 us |
| create | B | 7,526 tables/s [2,060-9,310] | 105.3 us | 75 us | 105 us | 131 us | 175 us | 260 us |
| create | A2 | 6,631 tables/s [1,639-8,004] | 124.7 us | 75 us | 111 us | 137 us | 235 us | 431 us |
| insert | A | 14,328 stmt/s [3,078-22,197] | 42.8 us | 46 us | 62 us | 68 us | 86 us | 137 us |
| insert | B | 14,596 stmt/s [3,497-22,193] | 42.0 us | 44 us | 57 us | 68 us | 86 us | 129 us |
| insert | A2 | 13,954 stmt/s [3,433-22,222] | 43.2 us | 46 us | 61 us | 68 us | 88 us | 146 us |
| select-200 | A | 13,593 stmt/s [3,036-18,028] | 42.4 us | 54 us | 70 us | 74 us | 91 us | 125 us |
| select-200 | B | 13,700 stmt/s [2,536-17,694] | 41.5 us | 56 us | 69 us | 73 us | 89 us | 119 us |
| select-200 | A2 | 13,653 stmt/s [2,797-17,601] | 43.6 us | 55 us | 70 us | 75 us | 93 us | 116 us |
| select-1000 | A | 14,751 stmt/s [3,416-16,911] | 37.4 us | 52 us | 60 us | 65 us | 79 us | 108 us |
| select-1000 | B | 14,699 stmt/s [2,677-16,763] | 37.6 us | 56 us | 61 us | 66 us | 79 us | 113 us |
| select-1000 | A2 | 14,677 stmt/s [2,951-16,393] | 36.9 us | 55 us | 60 us | 66 us | 79 us | 109 us |
| select-10000 | A | 15,224 stmt/s [2,372-17,834] | 36.8 us | 55 us | 61 us | 62 us | 77 us | 106 us |
| select-10000 | B | 15,331 stmt/s [2,976-17,737] | 36.4 us | 53 us | 60 us | 62 us | 76 us | 103 us |
| select-10000 | A2 | 15,277 stmt/s [2,835-17,688] | 36.2 us | 52 us | 60 us | 62 us | 76 us | 111 us |

Operation counts are 500 (create) and 2,000 (every other shape) per arm per run, 20 runs.

| Shape | B / A rate | B / A server CPU | A2 / A rate (noise floor) | A2 / A server CPU (noise floor) | Reading |
|---|---|---|---|---|---|
| create | 1.130x | 0.850x | 0.995x | 1.007x | B cheaper, outside the floor |
| insert | 1.019x | 0.981x | 0.974x | 1.009x | inside the floor |
| select-200 | 1.008x | 0.980x | 1.004x | 1.028x | inside the floor |
| select-1000 | 0.996x | 1.004x | 0.995x | 0.985x | inside the floor |
| select-10000 | 1.007x | 0.991x | 1.003x | 0.985x | inside the floor |

## Reading

**No shape regressed.** The floor, from A2 against A, is 0.5-2.6% on the rate and 0.7-2.8% on CPU;
the insert and the three tuple-key reads sit inside it. The read shapes are flat in rows (42 to 37
us of CPU across 200 to 10,000 rows), as a tuple-key descent should be.

**CREATE TABLE is cheaper on B by 15% of CPU and shows a shorter tail** (p90 175 against 240 us,
p99 260 against 434 us). The cause was not isolated here; a sampled profile of the two binaries
would be needed, and none was taken. The shape creates 500 tables on a fresh volume, so it prices
the catalog write path for a catalog of about 500 relations only.

**What this run did not reach (rule 4b).** The change is a catalog page ceiling, and nothing here
approached it: 500 tables is far from the limit, so no cell characterised the ceiling itself or
the cost of growing a catalog chain from the general supply at scale. Cut for the 45-minute
budget: a create sweep to thousands of tables, `durability = strict`, `cores > 1`, and a sampled
CPU split. The PostgreSQL floor was not measured for these shapes. There is no prior kdbs results
file for this workload and shape, so A, the base binary measured in the same run, is the baseline;
this file becomes the baseline the next run is read against.

## Waits

Does not apply as a decomposition: a statement is one serial round trip at `durability = relaxed`
(no commit wait or fsync on the statement path), with one client and so no lock or conflict wait.
What can be separated is in the table: the gap between the client's p50 (62-137 us) and the server
CPU per statement (36-124 us) is the loopback round trip and the driver.

## Host note

The run shared the machine with another session's test suite for its whole length. Wall-clock
rates carry that (per-run minimum rates are 2-3x below the medians) and are therefore reported as
medians of 20 interleaved runs; the in-run A2 control is what makes the deltas readable. Server CPU
per statement for A on create ranged 101-190 us across runs, against a median of 123.8 us.
