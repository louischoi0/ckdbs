# PostgreSQL control run of scenarios 1 to 4 at `v3.4.17-202-g481d101aa`: the same-host floor

**One line.** PostgreSQL 18.7, in a fresh container at its defaults with its processes pinned to this host's eight P-cores, runs the
scenario-2 booking at **2,087.1 tps** (three runs, spread 3.9 %), the 10,000-loan unindexed equality of scenario 3 at 4,924 qps (spread 7.7 %), scenario 4's
hot-board equality at 3,994 qps (2.3 %) and scenario 1's 60,480-bar backtest join at 778 qps (11.6 %), all 36 cells exit 0 and every
verification line passes. **This file is a control group, not a result about KDS**: it is the PostgreSQL floor that a later pinned
KDS run on this host is read against, and it replaces the v3.3.0 PostgreSQL numbers for that purpose, because those came from another
host, another PostgreSQL minor version and a native install, and they differ from these by 1.2x to 9.7x depending on which wait a cell is
made of (section 9: commit 9.7x, round trip 2.6x, per examined row 1.9x, per returned row 1.2x to 1.4x). No KDS cell ran in this window.

The thesis, in the order the file shows it: (1) the cells are repeatable on this host, with a median spread of 2.9 % to 3.8 % per scenario and
a few small cells above 20 % that carry no claim (section 4); (2) a booking is 479 µs, of which the durable commit is 29 % and its floor
(p0) is 125.4 µs (section 5); (3) an unindexed equality costs a fixed 22 to 27 µs round trip plus about 18 ns per row examined, and an index
removes the per-row term only from 1,000 loans up, 1.3x to 1.7x there and 4.7x to 7.6x on the `loans` equalities at 10,000 (sections 6 and 8); (4) the shapes that return many rows are bounded by a per-returned-row
cost of 2.8 µs that moved least against v3.3.0, which is the cell family where a later KDS ratio is least sensitive to the host (section 8).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time executed | **2026-11-28 00:18:31 to 00:32:03 UTC** (attempt 2, the measurement; 36 cells, `timeline` in the archive). 812 s of wall clock, of which **580 s were quiet-gate holds** (390 s before `pg-s2-b1`, 190 s before `pg-s3-n200-none`) and 232 s ran the cells. Attempt 1 (00:16:21 to 00:17:23 UTC) ran 0 cells: section 3 |
| Worktree / branch | `pg-control-scenarios` / `worktree-pg-control-scenarios`, at `origin/main` `0d6a8c526` (`v3.4.17-238-g0d6a8c526`), where this file was written. The measured commit is **`481d101aa`** (`v3.4.17-202-g481d101aa`, committed 2026-11-27 06:55:30 -0500), an ancestor of `0d6a8c526`; the measurement ran from the main checkout's session, not from this worktree |
| Drivers | a `tools/` snapshot of `481d101aa` copied to the run directory (`src/tools`, not archived; `src/COMMIT` = `481d101aa69df4be7173fe609576a09378edda76`). **Verified at writing time identical to `git archive 481d101aa tools`** with `diff -r`, `__pycache__` ignored, so the tree measured was clean. The drivers are `tools/pg_scenario1_backtest.py`, `pg_scenario2_freight.py`, `pg_scenario3_library.py`, `pg_scenario4_cabinopt_days.py` over `pg_wire.py` and `bench_common.py`. **Between `481d101aa` and `0d6a8c526` seven files of `tools/` differ** (`cp_s0_fence.py`, `ea_close.py`, `ea_s0_premise.py`, `eb_close.py`, `eb_s0_premise.py`, `eb_s4_difftest.py`, `ee_s0_premise.py`); none is a scenario driver, `pg_wire.py` or `bench_common.py`. Between `9682b725` (v3.3.0's PostgreSQL cells) and `481d101aa`, `git diff --stat` over the four `pg_scenario*` drivers, `pg_wire.py`, `bench_common.py` and `pg_setup.sh` is empty |
| Build type, KDS binary | not applicable: no KDS binary ran. The version of record at the measured commit is `v3.4.17`, `git describe --tags` = `v3.4.17-202-g481d101aa` |
| Server | PostgreSQL **18.7** (Debian 18.7-1.pgdg13+5, gcc 14.2.0, x86_64) in a fresh `docker.io/library/postgres:18` container (image created 2026-11-24 04:30:03 UTC, id `71351f08f4be...`), started by `podman` 6.2.0 for this run, container id `a7177958b26a...`, run as `podman run -d --network host`, so **no rootless port proxy** sits between the driver and the postmaster. `initdb --encoding=UTF8 --locale=C`, `trust` authentication, database `bench`, role `postgres` |
| Server configuration | **PostgreSQL defaults**. The run set exactly two keys, both on the command line: `port = 15434` and `listen_addresses = 127.0.0.1`. Every other non-default line of `conf/pg-nondefault.txt` is the image's or `initdb`'s (`shared_buffers = 128MB`, `max_connections = 100`, `max_wal_size = 1GB`, `min_wal_size = 80MB`, `autovacuum_worker_slots = 16`, `dynamic_shared_memory_type = posix`); `synchronous_commit = on`, `fsync = on` (`conf/pg-settings.txt`, 400 settings). No tuning |
| Device | the container's storage under `/home/ukjin/.local/share/containers/storage` on `/` = **`/dev/mapper/fedora-root`, xfs** (`df -T /home/ukjin` at writing time; an LVM volume on an NVMe device, per the operator's record of this host); **never tmpfs**. A fresh cluster for this run (ready to accept connections at 00:18:31.97 UTC, `logs/pg.serverlog`) |
| Host | Intel Core Ultra 7 270K Plus, 24 logical CPUs (8 P-cores = CPUs 0-7, 16 E-cores = CPUs 8-23), `powersave` governor, Linux 7.3.6-300.fc45 (`uname -r` at writing time). **Not the host of any earlier PostgreSQL number in `bench/`** |
| Pinning | the postmaster and every process under it on **CPUs 0-7**: `podman --cpuset-cpus` is unavailable in rootless mode on this host, so `taskset -a -cp 0-7` was applied to the postmaster and its children after start, and new backends inherit it (`conf/pg-affinity.txt`: nine processes, all `0-7`). Every driver ran under **`taskset -c 8-23`** |
| Quiet host | a gate before every cell waits for no `cc1plus`, `cmake --build`, `ctest` or `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh`), **ignoring `kds_server` by name**: another project's `kds_server` (pid 1386) and containers (`sql-tuning-*`: postgres 17, mssql, two oracle and a `kds` image) run on this host throughout, so the gate sees their CPU only as the 8 % busy ceiling, not by process. `/proc/loadavg` and the foreign-process list are in every cell's `.precheck` (1-minute load 0.50 to 1.20 over the 36 cells). Section 4 states what the gate did and did not catch |
| Time budget | within the 20-minute stage budget of `CLAUDE.md` once the gate holds are set aside: **580 s of quiet-gate hold, outside the budget**; 232 s of cells |
| Suite | not executed; no code was changed |

## 2. Role, and the KDS cells each PostgreSQL cell is the floor for

Every cell is the PostgreSQL cell of `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/run_all.sh` (lines 226 to 239), with the same arguments, three rounds (`""`, `r2-`, `r3-`)
in the same order. A later KDS run on this host that reads itself against PostgreSQL should pair its cells as follows and quote
**these** numbers; the v3.3.0 PostgreSQL numbers stay what they were, a number from another host.

| PostgreSQL cell (here) | driver arguments | the KDS cell it is the floor for |
|---|---|---|
| `pg-s2-b1` | `pg_scenario2_freight.py --organizations 300 --ships 30 --operations 300 --cargos 4000 --bookings 3000 --verify 100 --seed 1 --synchronous-commit on` | `tools/scenario2_freight.py` with the same sizes and **`--bookers 1`** (the v3.3.0 cells `b1-s2-c1-g` and `b1-s2-c1-s`: `cores = 1`, `group` and `strict`; PostgreSQL's `synchronous_commit = on` was paired with both at v3.3.0) |
| `pg-s3-n{200,1000,10000}-{none,all}` | `pg_scenario3_library.py --user postgres --loans N --index-mode {none,all} --seed 1` | `tools/scenario3_library.py` at the same `--loans` and `--index-mode`, **`cores = 1`** (v3.3.0's `s3-c1-n{N}-{M}`) |
| `pg-s1` | `pg_scenario1_backtest.py --seed 1 --verify` | `tools/scenario1_backtest.py`, `cores = 1`: `c1-g` and `c1-s` (v3.3.0's `s1-c1-g`, `s1-c1-s`) |
| `pg-s1s-n252`, `-n1008`, `-n10080` | the same plus `--years 1 --symbols 1 --exchanges 1` / `--years 1 --symbols 4 --exchanges 2` / `--years 5 --symbols 8 --exchanges 2` | the size sweep `s1s-n252-c1-g`, `s1s-n1008-c1-g`, `s1s-n10080-c1-g` |
| `pg-s4` | `pg_scenario4_cabinopt_days.py --seed 1` | scenario 4's **day 1** (`tools/scenario4_cabinopt_days.py`, the `off` arm; the twin runs day 1 only, unpaced) |

The pinning to compare against is the one used here: KDS server on CPUs 0-7 and its driver on CPUs 8-23, which is what the pinned series of
`bench/v3.5.0/interim-scenarios-v3.4.17-166-g6c98f7c32.md` used on this host.

**Row-set sizes (rule 9)** and the flag mapping. Scenario 3: `--loans N` sets the relation sizes (`sizes` in the JSON): 200 loans = 40 users, 40 books, 100 reservations;
1,000 loans = 200, 200, 500; 10,000 loans = 2,000, 2,000, 5,000; three sizes, two index modes. Scenario 1: bars = `--years` x 252 x `--symbols`; `n252` = 252 bars (765 rows resident), `n1008` = 1,008 (2,280 rows), `n10080` = 10,080
(21,436 rows) and the default run 30 x 252 x 8 = 60,480 bars (128,536 rows). Scenario 4: tapes of 200, 1,000 and 10,000 rows beside two 10,000-row boards. **Scenario 2 does not sweep rows**: its loaded sizes are fixed by the
arguments (24,624 rows resident, 3,000 freights and 16,889 charges accumulate in a cell) and the booking's reads are pk descents and one 93-row recipe scan.

## 3. How the run ran, and attempt 1

| Item | State |
|---|---|
| Cells | 36 = 12 distinct cells x 3 rounds: `pg-s2-b1`, `pg-s3-n{200,1000,10000}-{none,all}` (6), `pg-s4`, `pg-s1`, `pg-s1s-n{252,1008,10080}` (3). Order within a round as in `run_all.sh`; rounds `""`, `r2-`, `r3-`. **All 36 `.rc` files are 0** |
| Fresh state | one fresh container and cluster for the whole run; every cell creates its own tables under a `--suffix` (scenario 1 drops them at its end), so a cell does not read a predecessor's rows; the cluster itself is shared by all 36 cells, as the v3.3.0 twin's was |
| Verification | scenario 2: `verify (§4) 400 checks, 0 failure(s)` in **3 of 3** runs. Scenario 1: `verify: OK - all 8 models' P&L read back through the comparison join matches what was accumulated` in **12 of 12** runs (`verify_problems` empty in every JSON). Scenarios 3 and 4 have no `--verify` mode; their cells report `client_errors 0` / `err 0` in every phase |
| Watchdog | a 60 s watchdog polled the run's `status` file (`watchdog.sh`, `watchdog.log`). It logged `ORPHAN: master gone, launching` at 00:18:31 UTC, which started attempt 2; it found no stall during attempt 2 and exited on `all.done` at 00:32:31 UTC |

**Attempt 1 failed and ran no cell.** A user systemd timer set about 10 h out fired the master at 00:16:21 UTC instead of its scheduled 00:01:53 UTC; the cause of the delay is **unverified**. The postmaster then could not bind `127.0.0.1:15433`: the container `sql-tuning-kdbs` of another project had held that port since 2026-11-27 14:02 UTC. `pg_up` failed at 00:17:23 UTC (`PG FAILED TO START`)
and nothing ran. Attempt 2 moved the run to **port 15434** (the port v3.3.0's PostgreSQL also used) and added a port-occupancy precheck to `pg_up`. Attempt 1's logs are in `attempt1-port15433-conflict/`
of the archive. The 15433 of `tools/pg_setup.sh` is therefore not usable on this host while that container lives.

## 4. Validity and the noise floor, from inside the run

Three rounds of every cell are the noise floor. **Spread = (max - min) / median over the three runs of a cell**, the definition of `bench/v3.3.0/summary-scenarios-v3.3.0-33-g9682b725.md`, and a spread above 20 % carries no
claim (marked `†` below). A cell is a finding against another only if the difference of medians exceeds the larger of the two spreads.

| Scenario | cell-metrics | median spread | cells above 20 % | highest |
|---|---|---|---|---|
| 2 booking | 1 (tps) | 3.9 % | 0 | 3.9 % |
| 3 library | 72 (12 shapes x 6 cells, qps derived from the mean) | 3.8 % | 9 | 119.9 % (`pk-user`, 10,000 loans, unindexed) |
| 4 days | 8 phases | 3.7 % | 1 (`d1-open`, 66.2 %) | 66.2 % |
| 1 backtest | 88 (22 phases x 4 sizes) | 2.9 % | 8 | 41.7 % |
| 1 sweeps | 88 (the warm/cold/index/dropped cases of 7 shapes x 4 sizes) | not tabulated | 11 | - |

What explains the spreads that are large, as far as the run shows:

* **The first read phase of a scenario-3 cell is slow to start.** `pk-user` runs first (200 operations, about 5 ms) and carries 5 of the 9 scenario-3 cells above 20 %: in `pg-s3-n10000-none` its three runs have means of 38.4 µs, 62.7 µs and 21.2 µs (p50 28.7, 56.5, 20.3 µs). The
  pattern (an early stretch of a short phase several times slower, then the steady state) is what a CPU that has not yet ramped up would show; the host's CPUs report `powersave`
  and about 51 % of maximum frequency at an idle sample. **The cause is unverified** (no frequency was sampled during a cell). Consequence: `pk-user` is not usable as the
  control that cannot be affected, and a later KDS run's first phase will carry the same start-up term.
* **A time-triggered checkpoint overlapped round 1.** The server log has `checkpoint starting: time` at 00:23:32 (before any cell) and again at **00:28:32**, completing at 00:29:26 (543 buffers written over 54.4 s, as the default spreading
  gives). It overlapped the round-1 cells from `pg-s3-n1000-none` to `pg-s1s-n10080`; rounds 2 and 3 saw none. This is PostgreSQL's own default behaviour and was not controlled. `pg-s4` run 1's `d1-open` (240 autocommit inserts, mean 383.8 µs, p99 12,986.6 µs, max 13,414.3 µs against means of
  about 150 µs in rounds 2 and 3) is the one cell in this run where the slowest operations are 100x the median; it coincides with that checkpoint, and **the attribution is unverified**.
* **One cell overlapped another session's test process.** The `.precheck` of **`pg-s2-b1` (round 1)** lists a foreign `build/tests/kds_tests` process
  (`ContentionCountersTest..., --gtest_repeat=5`, pid 1663701) that appeared between the gate's release (after 390 s) and the precheck's listing at 00:25:07, so it may have overlapped that cell, which
  ran for a few seconds. Round 1's 2,087.1 tps is the **median** of the three runs (2,128.7 and 2,047.5 tps beside it), so no effect is visible, and none can be excluded. The next cell's gate then held 190 s for the same process. Every other precheck lists no foreign process and a gate wait of 0 s.
* **The `FATAL: role "root" does not exist` lines in `logs/pg.serverlog`** (one per cell) are the readiness probe `pg_isready` run through `podman exec` as root; they are not driver connections.

The shared-host caveat is the one the gate cannot remove: the other project's containers and `kds_server` ran throughout; the gate's 8 % busy ceiling bounds them in aggregate (the loads it sampled during waits were 0 to 1 %), and
the drivers' and server's CPU sets are disjoint from each other but not from those processes.

## 5. Scenario 2: one durable booker

The booking (eight statements and a commit, one connection, 3,000 bookings, capacity cached) runs at **2,087.1 tps, spread 3.9 %**, and its commit is 29 % of it.

| Run | committed | rejected (capacity / credit) | conflicted | tps | verify |
|---|---|---|---|---|---|
| `pg-s2-b1` | 3,000 | 289 / 51 | 0 | 2,087.1 tps | 400 checks, 0 failures |
| `r2-pg-s2-b1` | 3,000 | 289 / 51 | 0 | 2,128.7 tps | 400 checks, 0 failures |
| `r3-pg-s2-b1` | 3,000 | 289 / 51 | 0 | 2,047.5 tps | 400 checks, 0 failures |
| **median** | | | | **2,087.1 tps**, spread 3.9 % | |

`tps` is the driver's committed bookings over elapsed time. One booker, so no conflict or lock wait can occur by construction (`conflicted 0`); the manifest reporter's 50 reads (20 scans, 20 roll-ups, 10 customer statements) ran inside the same window.

**Where a booking's time goes (rule 3).** A booking is 479 µs of wall clock (1 / tps: 479.1, 469.8 and 488.4 µs in the three runs). The shares below are each phase's total time over the `booking` phase's total time (3,340 attempts, 340 of them rejected and rolled back);
the wait types that apply to a single connection are the commit, the statements and the client's round trips. Lock or conflict wait does not apply (one booker); a separate client/socket round trip is not measured by the driver and sits inside every statement's mean.

| component of a booking | phase | executions | mean | share of booking time, median of 3 | range over 3 runs |
|---|---|---|---|---|---|
| commit (durable WAL flush) | `commit` | 1 per committed booking | 138.2 µs | 29.0 % | 28.8 % to 29.1 % |
| read: cargo | `cargo-lookup` | 1 per attempt (3,340) | 25.1 µs | 5.9 % | 5.8 % to 6.0 % |
| read: credit | `credit-lookup` | 1 per attempt (3,340) | 22.6 µs | 5.3 % | 5.3 % to 5.5 % |
| read: capacity | `capacity-read` | 1 per attempt (3,340) | 21.9 µs | 5.2 % | 5.1 % to 5.2 % |
| read: recipe (93-row scan) | `recipe-read` | 1 per attempt (3,340) | 38.8 µs | 9.1 % | 9.1 % to 9.1 % |
| write: freight insert | `freight-insert` | 1 per committed booking | 29.6 µs | 6.2 % | 6.1 % to 6.2 % |
| write: charge inserts | `charge-insert` | 5.6 per committed booking (16,889 / 3,000) | 19.6 µs | 23.1 % | 22.8 % to 23.1 % |
| write: operation update | `operation-update` | 1 per committed booking | 23.2 µs | 4.9 % | 4.9 % to 4.9 % |
| write: organization update | `org-update` | 1 per committed booking | 20.9 µs | 4.5 % | 4.4 % to 4.5 % |
| not in any statement phase (the candidates, not separated: `BEGIN`, the `ROLLBACK` of 340 rejected attempts, client work between statements, the manifest reporter's contention) | - | - | - | 7.1 % | 6.8 % to 7.1 % |

The commit is the one durable wait: 138.2 µs mean, **p0 125.4 µs and p50 132.8 µs**, so 91 % of its mean is its floor, which is the cost of one WAL flush on this device. The 5.6 charge inserts per booking (19.6 µs each, 23.1 %)
are the largest single statement family, and every statement is a round trip with a mean of 19.6 to 38.8 µs (p0 of the reads: 17.9 to 33.6 µs).

Latency distribution per phase (median over the three runs of each percentile; the operation count is one run's):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `booking` | 3,340 ops | 428.2 µs | 126.7 µs | 425.8 µs | 446.7 µs | 499.8 µs | 779.0 µs |
| `cargo-lookup` | 3,340 ops | 25.1 µs | 20.3 µs | 23.7 µs | 24.1 µs | 27.4 µs | 46.5 µs |
| `credit-lookup` | 3,340 ops | 22.6 µs | 18.5 µs | 21.4 µs | 22.2 µs | 24.0 µs | 30.2 µs |
| `capacity-read` | 3,340 ops | 21.9 µs | 17.9 µs | 20.9 µs | 21.3 µs | 23.0 µs | 30.2 µs |
| `recipe-read` | 3,340 ops | 38.8 µs | 33.6 µs | 36.9 µs | 38.0 µs | 41.2 µs | 62.7 µs |
| `freight-insert` | 3,000 ops | 29.6 µs | 25.3 µs | 27.3 µs | 27.8 µs | 33.8 µs | 60.3 µs |
| `charge-insert` | 16,889 ops | 19.6 µs | 16.8 µs | 18.4 µs | 18.6 µs | 20.4 µs | 34.4 µs |
| `operation-update` | 3,000 ops | 23.2 µs | 20.6 µs | 22.0 µs | 22.3 µs | 25.2 µs | 33.7 µs |
| `org-update` | 3,000 ops | 20.9 µs | 19.0 µs | 20.3 µs | 20.6 µs | 22.2 µs | 26.6 µs |
| `commit` | 3,000 ops | 138.2 µs | 125.4 µs | 131.7 µs | 132.8 µs | 158.9 µs | 181.5 µs |
| `manifest-scan` | 20 ops | 41.4 µs | 24.7 µs | 37.3 µs | 40.6 µs | 59.5 µs | 62.3 µs |
| `voyage-rollup` | 20 ops | 42.5 µs | 27.8 µs | 29.6 µs | 30.4 µs | 46.2 µs | 230.4 µs |
| `customer-statement` | 10 ops | 234.6 µs | 158.8 µs | 169.1 µs | 236.6 µs | 339.6 µs | 339.6 µs |

The loading phases of this cell (autocommit inserts, one durable commit each) have a mean of about 150 µs (`load-cargos` 151.8 µs, p0 133.8 µs, 4,000 ops), the same floor as the commit.

## 6. Scenario 3: the library, 200 / 1,000 / 10,000 loans, unindexed and indexed

Median queries per second, three runs per cell, 200 operations per shape, one connection, derived as 1,000,000 / mean µs; the spread is in brackets. `none` is a sequential scan (`ANALYZE` on, no index after load), `all` is the driver's full index set (single, composite and covering).

| shape | 200 loans, none | 200 loans, all | 1,000 loans, none | 1,000 loans, all | 10,000 loans, none | 10,000 loans, all |
|---|---|---|---|---|---|---|
| `pk-user` | 40,650 qps (59.1 %) | 30,581 qps (63.7 %) | 30,211 qps (11.6 %) | 26,455 qps (41.8 %) | 26,042 qps (119.9 %) | 22,727 qps (39.9 %) |
| `loans-by-user` | 31,949 qps (3.5 %) | 27,624 qps (5.3 %) | 21,231 qps (3.7 %) | 31,056 qps (13.0 %) | 4,924 qps (7.7 %) | 29,674 qps (20.7 %) |
| `loans-by-book` | 36,496 qps (6.6 %) | 33,670 qps (1.7 %) | 23,810 qps (5.5 %) | 34,965 qps (3.6 %) | 4,852 qps (19.4 %) | 35,336 qps (1.8 %) |
| `resv-by-user` | 43,668 qps (15.8 %) | 41,667 qps (3.0 %) | 32,468 qps (0.3 %) | 41,152 qps (3.8 %) | 8,873 qps (13.7 %) | 41,667 qps (2.1 %) |
| `books-by-author` | 42,017 qps (1.3 %) | 38,911 qps (11.2 %) | 37,313 qps (1.5 %) | 34,602 qps (3.5 %) | 16,287 qps (10.6 %) | 37,175 qps (1.5 %) |
| `books-by-genre` | 45,249 qps (1.8 %) | 40,323 qps (6.6 %) | 28,902 qps (2.0 %) | 26,810 qps (2.5 %) | 5,266 qps (2.4 %) | 6,863 qps (3.1 %) |
| `loans-by-daterange` | 30,120 qps (1.2 %) | 28,490 qps (0.0 %) | 12,240 qps (1.7 %) | 11,862 qps (2.2 %) | 1,506 qps (4.3 %) | 1,526 qps (5.5 %) |
| `overdue` | 32,468 qps (1.0 %) | 22,422 qps (38.0 %) | 14,164 qps (3.7 %) | 17,921 qps (2.0 %) | 1,720 qps (3.0 %) | 2,396 qps (1.7 %) |
| `join-loan-user` | 28,090 qps (0.6 %) | 25,510 qps (0.8 %) | 19,305 qps (1.4 %) | 24,390 qps (2.7 %) | 5,152 qps (1.0 %) | 24,155 qps (14.4 %) |
| `join-no-literal` | 9,470 qps (1.1 %) | 8,703 qps (26.9 %) | 6,748 qps (12.3 %) | 6,447 qps (2.5 %) | 3,044 qps (8.6 %) | 2,923 qps (13.6 %) |
| `exists-correlated` | 15,038 qps (3.6 %) | 13,928 qps (12.0 %) | 6,798 qps (6.8 %) | 11,507 qps (4.7 %) | 1,484 qps (7.5 %) | 11,287 qps (12.1 %) |
| `count-by-user` | 42,017 qps (9.8 %) | 37,037 qps (22.6 %) | 26,525 qps (1.3 %) | 38,610 qps (5.5 %) | 5,198 qps (11.3 %) | 39,216 qps (3.1 %) |

**An index pays only from 1,000 loans up, and mostly at 10,000, by removing the per-row term.** Throughput of the `all` cell over the `none` cell, median of three (`†` = either spread above 20 %, no claim, its larger spread in brackets):

| shape | 200 loans | 1,000 loans | 10,000 loans |
|---|---|---|---|
| `pk-user` | 0.75x † (64 %) | 0.88x † (42 %) | 0.87x † (120 %) |
| `loans-by-user` | 0.86x | 1.46x | 6.03x † (21 %) |
| `loans-by-book` | 0.92x | 1.47x | 7.28x |
| `resv-by-user` | 0.95x | 1.27x | 4.70x |
| `books-by-author` | 0.93x | 0.93x | 2.28x |
| `books-by-genre` | 0.89x | 0.93x | 1.30x |
| `loans-by-daterange` | 0.95x | 0.97x | 1.01x |
| `overdue` | 0.69x † (38 %) | 1.27x | 1.39x |
| `join-loan-user` | 0.91x | 1.26x | 4.69x |
| `join-no-literal` | 0.92x † (27 %) | 0.96x | 0.96x |
| `exists-correlated` | 0.93x | 1.69x | 7.60x |
| `count-by-user` | 0.88x † (23 %) | 1.46x | 7.55x |

At 10,000 loans the equalities on `loans` and `reservations` gain 4.7x to 7.6x (`loans-by-book` 7.28x, `exists-correlated` 7.60x, `count-by-user` 7.55x, `join-loan-user` 4.69x; `loans-by-user` 6.03x with a 20.7 % spread on the indexed side), `books-by-author` 2.28x,
`overdue` 1.39x and `books-by-genre` 1.30x; `loans-by-daterange` and `join-no-literal` do not move (1.01x, 0.96x), and these two are this scenario's **controls that an index cannot affect**: `EXPLAIN` in the cells'
JSON shows `join-no-literal` as the same Hash Join over a sequential scan of `loans` in both modes, and `loans-by-daterange` is a control by its ratio alone (its plan is not among the five archived `EXPLAIN`s). Their `all`/`none` ratios, 0.92x, 0.96x, 0.96x and 0.95x, 0.97x, 1.01x over the
three sizes, are the floor of this table: **a ratio from 0.92x to 1.01x is inside the controls and is not an index effect**. At 200 loans there is no gain: every shape reads 0.86x to 0.95x or carries no claim, and the two controls read 0.92x and 0.95x, so 5 to 8 points of the offset are common to shapes the index cannot affect; the remainder, up to 14 % (`loans-by-user` 0.86x, spreads 3.5 % and 5.3 %), is a cost of the indexed cell that this run does not explain. Whether the common part is an ordering effect (the `all` cell always runs 3 to 4 s after the `none` cell) is unverified.

The access arms (`EXPLAIN`, `plans` in each cell's JSON) at 10,000 loans: unindexed, `loans-by-user`, `books-by-genre` and `overdue` are sequential scans and the two joins hash joins (`exists-correlated` a hash join over a `HashAggregate`); indexed, the equalities are Bitmap Heap Scans over the driver's covering indexes, `overdue` a Bitmap Heap Scan
and `exists-correlated` a Nested Loop Semi Join over an index-only scan of the pk. At 200 loans the unindexed plans are sequential scans (`cost=0.00..4.50` for `loans-by-user`) and the joins hash joins.

**Fixed term and per-row term (rule 9).** The fixed term is the round trip and plan: the 200-loan `pk-user` p0 is 22.9 µs and `loans-by-user` 31.3 µs mean. The per-row term is the scan: the unindexed `loans-by-user` equality is 203.1 µs
mean at 10,000 loans and 31.3 µs at 200, so (203.1 - 27.1) µs over 10,000 rows is **17.6 ns per row examined** (`loans-by-book`: 17.9 ns), where 27.1 µs is scenario 1's `read-bar-lookup` mean in this run, used as the fixed term. The three sizes of the unindexed equality, 31,949 qps, 21,231 qps and 4,924 qps,
are that line, and an indexed equality stays at 27,624 qps, 31,056 qps and 29,674 qps: it does not scale with rows, which is the evidence for a fixed cost.

Against the v3.3.0 PostgreSQL 18.6 numbers (a different host, section 9), medians of the archived three runs per cell:

| shape | 200 loans, none | 200 loans, all | 1,000 loans, none | 1,000 loans, all | 10,000 loans, none | 10,000 loans, all |
|---|---|---|---|---|---|---|
| `pk-user` | 15,480 qps (1.4 %) | 15,748 qps (27.5 %) | 14,388 qps (2.4 %) | 14,347 qps (0.7 %) | 14,925 qps (22.7 %) | 14,925 qps (26.4 %) |
| `loans-by-user` | 13,263 qps (11.1 %) | 10,142 qps (27.6 %) | 9,643 qps (1.7 %) | 11,261 qps (17.4 %) | 2,421 qps (1.5 %) | 10,834 qps (25.4 %) |
| `loans-by-book` | 13,532 qps (25.2 %) | 9,542 qps (22.5 %) | 9,606 qps (2.3 %) | 11,669 qps (24.0 %) | 2,307 qps (2.7 %) | 11,751 qps (26.2 %) |
| `resv-by-user` | 15,198 qps (23.9 %) | 10,707 qps (0.4 %) | 12,330 qps (1.5 %) | 13,228 qps (25.7 %) | 4,259 qps (6.8 %) | 12,953 qps (27.6 %) |
| `books-by-author` | 15,129 qps (0.9 %) | 10,341 qps (21.3 %) | 13,495 qps (4.9 %) | 11,682 qps (21.6 %) | 6,734 qps (7.0 %) | 12,107 qps (22.8 %) |
| `books-by-genre` | 15,601 qps (1.4 %) | 10,638 qps (33.6 %) | 11,696 qps (1.4 %) | 10,309 qps (7.3 %) | 3,041 qps (3.3 %) | 3,755 qps (10.5 %) |
| `loans-by-daterange` | 11,710 qps (1.4 %) | 11,223 qps (24.0 %) | 5,984 qps (2.6 %) | 5,669 qps (4.2 %) | 1,063 qps (12.4 %) | 1,065 qps (13.3 %) |
| `overdue` | 12,225 qps (1.8 %) | 10,741 qps (22.5 %) | 6,540 qps (3.9 %) | 7,273 qps (16.9 %) | 1,135 qps (4.3 %) | 1,911 qps (5.5 %) |
| `join-loan-user` | 9,833 qps (2.2 %) | 8,780 qps (10.5 %) | 7,375 qps (3.0 %) | 8,347 qps (12.7 %) | 2,327 qps (0.3 %) | 8,929 qps (3.3 %) |
| `join-no-literal` | 4,380 qps (2.8 %) | 4,062 qps (12.2 %) | 3,452 qps (5.1 %) | 3,135 qps (0.5 %) | 1,504 qps (1.3 %) | 1,478 qps (141.5 %) |
| `exists-correlated` | 5,653 qps (1.1 %) | 4,632 qps (16.0 %) | 3,322 qps (3.8 %) | 4,836 qps (1.7 %) | 768 qps (2.2 %) | 5,128 qps (13.9 %) |
| `count-by-user` | 13,351 qps (0.8 %) | 9,524 qps (28.1 %) | 9,662 qps (0.9 %) | 11,820 qps (3.1 %) | 2,434 qps (3.1 %) | 11,521 qps (27.0 %) |

Ratio now over v3.3.0 (`†` = either side's spread above 20 %, no claim; every other cell is outside both spreads; the host, the PostgreSQL minor version and native-to-container are changed together and the ratio is attributable to none of them alone):

| shape | 200 loans, none | 200 loans, all | 1,000 loans, none | 1,000 loans, all | 10,000 loans, none | 10,000 loans, all |
|---|---|---|---|---|---|---|
| `pk-user` | 2.63x † | 1.94x † | 2.10x | 1.84x † | 1.74x † | 1.52x † |
| `loans-by-user` | 2.41x | 2.72x † | 2.20x | 2.76x | 2.03x | 2.74x † |
| `loans-by-book` | 2.70x † | 3.53x † | 2.48x | 3.00x † | 2.10x | 3.01x † |
| `resv-by-user` | 2.87x † | 3.89x | 2.63x | 3.11x † | 2.08x | 3.22x † |
| `books-by-author` | 2.78x | 3.76x † | 2.76x | 2.96x † | 2.42x | 3.07x † |
| `books-by-genre` | 2.90x | 3.79x † | 2.47x | 2.60x | 1.73x | 1.83x |
| `loans-by-daterange` | 2.57x | 2.54x † | 2.05x | 2.09x | 1.42x | 1.43x |
| `overdue` | 2.66x | 2.09x † | 2.17x | 2.46x | 1.52x | 1.25x |
| `join-loan-user` | 2.86x | 2.91x | 2.62x | 2.92x | 2.21x | 2.71x |
| `join-no-literal` | 2.16x | 2.14x † | 1.95x | 2.06x | 2.02x | 1.98x † |
| `exists-correlated` | 2.66x | 3.01x | 2.05x | 2.38x | 1.93x | 2.20x |
| `count-by-user` | 3.15x | 3.89x † | 2.75x | 3.27x | 2.14x | 3.40x † |

The geometric mean of the twelve shapes' ratios is 2.68x / 2.93x at 200 loans (`none` / `all`), 2.33x / 2.58x at 1,000 and 1.92x / 2.24x at 10,000: the ratio falls as the per-row term takes a larger share of a shape, because the per-row term moved less (section 9).

Latency distributions, per cell (median over the three runs of each percentile; `load` is the 380 / 1,900 / 19,000-row autocommit load of all four relations, where the count is stated):

### 200 loans, `none`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 24.6 µs | 22.9 µs | 23.4 µs | 23.7 µs | 25.5 µs | 87.1 µs |
| `loans-by-user` | 200 ops | 31.3 µs | 21.4 µs | 26.4 µs | 29.3 µs | 39.6 µs | 52.2 µs |
| `loans-by-book` | 200 ops | 27.4 µs | 22.2 µs | 25.3 µs | 27.5 µs | 31.2 µs | 31.5 µs |
| `resv-by-user` | 200 ops | 22.9 µs | 19.2 µs | 21.7 µs | 22.7 µs | 25.2 µs | 25.8 µs |
| `books-by-author` | 200 ops | 23.8 µs | 19.4 µs | 21.7 µs | 23.0 µs | 26.7 µs | 28.0 µs |
| `books-by-genre` | 200 ops | 22.1 µs | 19.3 µs | 20.8 µs | 22.2 µs | 24.4 µs | 25.9 µs |
| `loans-by-daterange` | 200 ops | 33.2 µs | 26.3 µs | 30.5 µs | 32.5 µs | 38.9 µs | 40.7 µs |
| `overdue` | 200 ops | 30.8 µs | 24.1 µs | 29.3 µs | 31.2 µs | 34.7 µs | 35.8 µs |
| `join-loan-user` | 200 ops | 35.6 µs | 28.9 µs | 33.6 µs | 35.3 µs | 39.2 µs | 41.5 µs |
| `join-no-literal` | 200 ops | 105.6 µs | 102.4 µs | 104.4 µs | 105.0 µs | 107.2 µs | 111.8 µs |
| `exists-correlated` | 200 ops | 66.5 µs | 64.0 µs | 65.6 µs | 66.1 µs | 68.8 µs | 76.0 µs |
| `count-by-user` | 200 ops | 23.8 µs | 22.5 µs | 23.1 µs | 23.8 µs | 24.9 µs | 28.3 µs |
| `load` | 380 ops | 156.8 µs | 133.0 µs | 137.1 µs | 140.9 µs | 234.8 µs | 352.6 µs |

### 200 loans, `all`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 32.7 µs | 23.2 µs | 28.9 µs | 32.3 µs | 36.8 µs | 42.1 µs |
| `loans-by-user` | 200 ops | 36.2 µs | 24.6 µs | 30.1 µs | 34.1 µs | 52.2 µs | 56.9 µs |
| `loans-by-book` | 200 ops | 29.7 µs | 24.5 µs | 27.7 µs | 29.7 µs | 33.2 µs | 34.0 µs |
| `resv-by-user` | 200 ops | 24.0 µs | 20.9 µs | 23.1 µs | 23.7 µs | 26.0 µs | 26.7 µs |
| `books-by-author` | 200 ops | 25.7 µs | 21.4 µs | 23.5 µs | 25.2 µs | 28.8 µs | 33.1 µs |
| `books-by-genre` | 200 ops | 24.8 µs | 21.6 µs | 23.2 µs | 24.7 µs | 26.9 µs | 30.8 µs |
| `loans-by-daterange` | 200 ops | 35.1 µs | 28.1 µs | 32.3 µs | 34.3 µs | 40.8 µs | 42.9 µs |
| `overdue` | 200 ops | 44.6 µs | 28.1 µs | 33.6 µs | 35.4 µs | 107.4 µs | 242.6 µs |
| `join-loan-user` | 200 ops | 39.2 µs | 31.8 µs | 37.0 µs | 38.7 µs | 43.0 µs | 45.1 µs |
| `join-no-literal` | 200 ops | 114.9 µs | 107.2 µs | 109.3 µs | 110.2 µs | 116.8 µs | 280.5 µs |
| `exists-correlated` | 200 ops | 71.8 µs | 69.1 µs | 70.6 µs | 71.3 µs | 73.9 µs | 80.0 µs |
| `count-by-user` | 200 ops | 27.0 µs | 24.8 µs | 25.9 µs | 26.4 µs | 27.8 µs | 29.0 µs |
| `load` | 380 ops | 151.3 µs | 131.0 µs | 137.1 µs | 140.0 µs | 212.8 µs | 359.9 µs |

### 1,000 loans, `none`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 33.1 µs | 29.3 µs | 30.4 µs | 30.6 µs | 36.1 µs | 43.2 µs |
| `loans-by-user` | 200 ops | 47.1 µs | 35.7 µs | 41.2 µs | 44.3 µs | 59.6 µs | 68.4 µs |
| `loans-by-book` | 200 ops | 42.0 µs | 36.1 µs | 39.9 µs | 41.9 µs | 46.4 µs | 48.3 µs |
| `resv-by-user` | 200 ops | 30.8 µs | 26.6 µs | 29.3 µs | 30.5 µs | 33.9 µs | 35.6 µs |
| `books-by-author` | 200 ops | 26.8 µs | 22.3 µs | 25.1 µs | 26.7 µs | 30.6 µs | 32.1 µs |
| `books-by-genre` | 200 ops | 34.6 µs | 28.5 µs | 33.2 µs | 34.3 µs | 41.6 µs | 42.7 µs |
| `loans-by-daterange` | 200 ops | 81.7 µs | 42.3 µs | 78.0 µs | 81.2 µs | 93.5 µs | 97.7 µs |
| `overdue` | 200 ops | 70.6 µs | 41.6 µs | 67.8 µs | 71.1 µs | 77.5 µs | 80.3 µs |
| `join-loan-user` | 200 ops | 51.8 µs | 46.1 µs | 49.8 µs | 51.4 µs | 56.4 µs | 59.3 µs |
| `join-no-literal` | 200 ops | 148.2 µs | 136.7 µs | 139.4 µs | 140.4 µs | 196.3 µs | 346.0 µs |
| `exists-correlated` | 200 ops | 147.1 µs | 133.1 µs | 135.0 µs | 136.0 µs | 175.0 µs | 357.4 µs |
| `count-by-user` | 200 ops | 37.7 µs | 35.5 µs | 37.1 µs | 37.4 µs | 38.8 µs | 42.1 µs |
| `load` | 1,900 ops | 152.9 µs | 133.7 µs | 139.7 µs | 142.3 µs | 174.8 µs | 277.9 µs |

### 1,000 loans, `all`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 37.8 µs | 28.9 µs | 34.7 µs | 35.0 µs | 40.3 µs | 43.5 µs |
| `loans-by-user` | 200 ops | 32.2 µs | 23.0 µs | 29.0 µs | 31.3 µs | 41.6 µs | 53.6 µs |
| `loans-by-book` | 200 ops | 28.6 µs | 23.4 µs | 26.6 µs | 28.6 µs | 33.0 µs | 35.1 µs |
| `resv-by-user` | 200 ops | 24.3 µs | 20.1 µs | 22.5 µs | 24.5 µs | 28.2 µs | 29.9 µs |
| `books-by-author` | 200 ops | 28.9 µs | 24.0 µs | 27.2 µs | 28.6 µs | 32.5 µs | 34.7 µs |
| `books-by-genre` | 200 ops | 37.3 µs | 31.5 µs | 35.8 µs | 36.9 µs | 44.1 µs | 45.3 µs |
| `loans-by-daterange` | 200 ops | 84.3 µs | 44.6 µs | 80.4 µs | 83.7 µs | 95.3 µs | 98.7 µs |
| `overdue` | 200 ops | 55.8 µs | 26.9 µs | 52.9 µs | 55.9 µs | 63.8 µs | 65.7 µs |
| `join-loan-user` | 200 ops | 41.0 µs | 34.5 µs | 38.9 µs | 40.3 µs | 45.7 µs | 48.7 µs |
| `join-no-literal` | 200 ops | 155.1 µs | 150.8 µs | 153.2 µs | 154.2 µs | 157.8 µs | 170.5 µs |
| `exists-correlated` | 200 ops | 86.9 µs | 84.0 µs | 85.7 µs | 86.1 µs | 89.2 µs | 101.9 µs |
| `count-by-user` | 200 ops | 25.9 µs | 23.1 µs | 25.3 µs | 25.6 µs | 26.5 µs | 28.9 µs |
| `load` | 1,900 ops | 151.2 µs | 132.9 µs | 139.7 µs | 141.4 µs | 177.4 µs | 283.2 µs |

### 10,000 loans, `none`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 38.4 µs | 27.4 µs | 28.2 µs | 28.7 µs | 50.6 µs | 71.7 µs |
| `loans-by-user` | 200 ops | 203.1 µs | 176.5 µs | 186.5 µs | 197.0 µs | 238.2 µs | 343.4 µs |
| `loans-by-book` | 200 ops | 206.1 µs | 183.3 µs | 197.9 µs | 206.4 µs | 221.6 µs | 233.2 µs |
| `resv-by-user` | 200 ops | 112.7 µs | 98.0 µs | 105.8 µs | 110.7 µs | 126.0 µs | 156.6 µs |
| `books-by-author` | 200 ops | 61.4 µs | 53.7 µs | 58.8 µs | 60.7 µs | 67.8 µs | 76.7 µs |
| `books-by-genre` | 200 ops | 189.9 µs | 158.4 µs | 170.3 µs | 175.8 µs | 324.9 µs | 372.4 µs |
| `loans-by-daterange` | 200 ops | 663.9 µs | 370.6 µs | 572.1 µs | 683.5 µs | 762.4 µs | 823.9 µs |
| `overdue` | 200 ops | 581.3 µs | 214.2 µs | 511.3 µs | 554.4 µs | 705.6 µs | 741.2 µs |
| `join-loan-user` | 200 ops | 194.1 µs | 183.9 µs | 189.8 µs | 192.1 µs | 205.5 µs | 217.5 µs |
| `join-no-literal` | 200 ops | 328.5 µs | 306.1 µs | 311.1 µs | 314.4 µs | 478.2 µs | 517.5 µs |
| `exists-correlated` | 200 ops | 673.7 µs | 624.4 µs | 651.6 µs | 674.8 µs | 699.4 µs | 867.2 µs |
| `count-by-user` | 200 ops | 192.4 µs | 183.4 µs | 188.4 µs | 191.4 µs | 201.8 µs | 209.0 µs |
| `load` | 19,000 ops | 151.6 µs | 127.7 µs | 139.8 µs | 142.3 µs | 174.2 µs | 277.9 µs |

### 10,000 loans, `all`
| shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 44.0 µs | 24.0 µs | 32.3 µs | 33.2 µs | 62.7 µs | 76.2 µs |
| `loans-by-user` | 200 ops | 33.7 µs | 23.1 µs | 28.5 µs | 30.9 µs | 50.2 µs | 55.7 µs |
| `loans-by-book` | 200 ops | 28.3 µs | 20.7 µs | 25.8 µs | 27.8 µs | 34.6 µs | 37.1 µs |
| `resv-by-user` | 200 ops | 24.0 µs | 19.0 µs | 22.0 µs | 23.3 µs | 27.9 µs | 30.4 µs |
| `books-by-author` | 200 ops | 26.9 µs | 19.8 µs | 24.9 µs | 26.3 µs | 32.2 µs | 35.9 µs |
| `books-by-genre` | 200 ops | 145.7 µs | 128.6 µs | 137.6 µs | 142.2 µs | 154.2 µs | 170.8 µs |
| `loans-by-daterange` | 200 ops | 655.2 µs | 318.0 µs | 572.1 µs | 682.2 µs | 776.6 µs | 804.8 µs |
| `overdue` | 200 ops | 417.3 µs | 37.0 µs | 336.7 µs | 460.9 µs | 525.0 µs | 557.1 µs |
| `join-loan-user` | 200 ops | 41.4 µs | 32.1 µs | 35.9 µs | 38.0 µs | 44.4 µs | 114.9 µs |
| `join-no-literal` | 200 ops | 342.1 µs | 318.3 µs | 323.8 µs | 327.5 µs | 509.1 µs | 543.7 µs |
| `exists-correlated` | 200 ops | 88.6 µs | 82.5 µs | 84.4 µs | 85.1 µs | 90.9 µs | 179.0 µs |
| `count-by-user` | 200 ops | 25.5 µs | 23.7 µs | 24.7 µs | 25.2 µs | 26.3 µs | 27.9 µs |
| `load` | 19,000 ops | 152.2 µs | 127.1 µs | 140.2 µs | 142.4 µs | 175.3 µs | 295.6 µs |

## 7. Scenario 4: the hot-predicate day, PostgreSQL with no structure

PostgreSQL at its defaults creates no structure for the hot predicate, so day 1 stands for every day: the hot equality is a sequential scan whatever happens (`EXPLAIN (ANALYZE, BUFFERS)` in the JSON: board `Seq Scan ... Rows Removed by Filter: 10206`, 76 buffers, **Execution Time 0.210 ms**; `tape_10k` 74 buffers, 0.203 ms). **No s4 PostgreSQL run with an
index on `symbol` exists: the twin never builds one, a known gap of the twin and not of PostgreSQL.** Qps is derived as 1,000,000 / mean µs for every phase (a serial single connection), for uniformity with the other scenarios.

| phase | what | ops | run 1 | run 2 | run 3 | median | spread | v3.3.0 median (spread) | ratio |
|---|---|---|---|---|---|---|---|---|---|
| `load[pg]` | setup load, txn batches of 500 | 31,200 | 54,945 qps | 54,945 qps | 55,249 qps | **54,945 qps** | 0.6 % | 17,452 qps (2.3 %) | 3.15x |
| `d1-open[pg]` | 240 autocommit inserts | 240 | 2,606 qps | 7,022 qps | 6,676 qps | **6,676 qps** | 66.2 % | 831 qps (10.9 %) | 8.03x † |
| `d1-board[pg]` | hot equality on a 10,240-row board | 2,400 | 4,021 qps | 3,931 qps | 3,994 qps | **3,994 qps** | 2.3 % | 1,976 qps (2.7 %) | 2.02x |
| `d1-tape200[pg]` | equality on a 200-row tape | 396 | 30,675 qps | 27,397 qps | 29,940 qps | **29,940 qps** | 10.9 % | 12,484 qps (6.7 %) | 2.40x |
| `d1-tape1k[pg]` | 1,000-row tape | 396 | 20,704 qps | 20,161 qps | 20,450 qps | **20,450 qps** | 2.7 % | 8,726 qps (2.5 %) | 2.34x |
| `d1-tape10k[pg]` | 10,000-row tape | 396 | 4,314 qps | 4,177 qps | 4,380 qps | **4,314 qps** | 4.7 % | 2,100 qps (1.4 %) | 2.05x |
| `d1-pk[pg]` | pk control | 240 | 42,553 qps | 46,512 qps | 43,103 qps | **43,103 qps** | 9.2 % | 14,409 qps (4.0 %) | 2.99x |
| `d1-close[pg]` | COUNT / SUM / GROUP BY full scans | 12 | 2,124 qps | 2,108 qps | 2,098 qps | **2,108 qps** | 1.2 % | 1,185 qps (0.2 %) | 1.78x |

The scan cost is linear in rows: the 200-row, 1,000-row and 10,000-row tapes have p50 31.7, 47.8 and 227.1 µs, so about **20 ns per row** from 1,000 to 10,000 ((227.1 - 47.8) µs / 9,000) and 20 ns from 200 to 1,000 ((47.8 - 31.7) µs / 800), against scenario 3's 17.6 ns per row; the predicate differs (text here, integer there) and the run does not separate that from the rest. The hot board
(10,240 rows) at 248.3 µs p50 is that scan plus the fixed term, and `EXPLAIN`'s 0.210 ms is 85 % of it, so the round trip beside it is about 38 µs. The `pk` control is 43,103 qps with a 9.2 % spread and p0 18.9 µs: the floor of a round trip in this scenario.

`d1-open` (240 autocommit inserts, one durable commit each) is 6,676 qps by its median but **carries no claim** (spread 66.2 %): run 1 has a 13 ms stall that rounds 2 and 3 do not (section 4). Its p50, 135.4 µs, agrees with the commit p50 of scenario 2 (132.8 µs); the mean does not, because of the outlier.

Latency distribution per phase (median over three runs of each percentile):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load[pg]` | 31,200 ops | 18.2 µs | 16.1 µs | 17.5 µs | 17.7 µs | 18.6 µs | 28.0 µs |
| `d1-open[pg]` | 240 ops | 149.8 µs | 131.5 µs | 134.0 µs | 135.4 µs | 238.2 µs | 306.7 µs |
| `d1-board[pg]` | 2,400 ops | 250.4 µs | 226.1 µs | 243.4 µs | 248.3 µs | 269.8 µs | 426.1 µs |
| `d1-tape200[pg]` | 396 ops | 33.4 µs | 29.8 µs | 31.5 µs | 31.7 µs | 39.0 µs | 42.1 µs |
| `d1-tape1k[pg]` | 396 ops | 48.9 µs | 46.0 µs | 47.5 µs | 47.8 µs | 54.7 µs | 56.7 µs |
| `d1-tape10k[pg]` | 396 ops | 231.8 µs | 221.3 µs | 225.5 µs | 227.1 µs | 279.4 µs | 376.0 µs |
| `d1-pk[pg]` | 240 ops | 23.2 µs | 18.9 µs | 20.4 µs | 20.6 µs | 25.2 µs | 84.4 µs |
| `d1-close[pg]` | 12 ops | 474.3 µs | 164.6 µs | 169.9 µs | 206.6 µs | 1,501.2 µs | 1,501.2 µs |

## 8. Scenario 1: the backtest at four sizes

One connection, the driver's 22 measured phases, then the warm/cold/index/dropped sweep over seven read shapes, the write sweep and the connection sweep. The sweep sizes are the full 30-year run and three smaller ones; phase qps is derived as 1,000,000 / mean µs.

| phase | 60,480 bars (30 years x 8 symbols) | 252 bars (1 year x 1 symbol) | 1,008 bars (1 year x 4 symbols) | 10,080 bars (5 years x 8 symbols) |
|---|---|---|---|---|
| `load-exchanges` | 2,928 qps (41.7 %) | 3,195 qps (20.2 %) | 4,290 qps (5.6 %) | 3,706 qps (38.9 %) |
| `load-symbols` | 5,914 qps (13.6 %) | 3,861 qps (33.2 %) | 5,828 qps (7.3 %) | 6,414 qps (8.6 %) |
| `load-sessions` | 46,948 qps (4.2 %) | 45,872 qps (1.9 %) | 47,170 qps (4.6 %) | 46,948 qps (9.0 %) |
| `load-bars` | 38,023 qps (2.6 %) | 36,101 qps (2.2 %) | 33,557 qps (12.7 %) | 33,784 qps (2.4 %) |
| `load-stats` | 38,168 qps (1.1 %) | 38,462 qps (2.3 %) | 38,911 qps (2.3 %) | 38,314 qps (3.4 %) |
| `load-models` | 5,886 qps (26.5 %) | 6,146 qps (36.8 %) | 6,523 qps (9.6 %) | 6,061 qps (0.8 %) |
| `backtest-read` | 778 qps (11.6 %) | 8,873 qps (2.5 %) | 6,161 qps (13.4 %) | 3,175 qps (3.0 %) |
| `result-insert` | 5,981 qps (4.7 %) | 6,803 qps (5.1 %) | 6,854 qps (10.9 %) | 6,859 qps (4.5 %) |
| `backtest-replay` | 771 qps (19.8 %) | 13,441 qps (2.0 %) | 8,403 qps (0.3 %) | 3,568 qps (2.1 %) |
| `compare-all` | 105 qps (14.9 %) | 3,814 qps (2.2 %) | 3,799 qps (1.4 %) | 909 qps (2.6 %) |
| `compare-one` | 1,433 qps (0.8 %) | 19,724 qps (1.2 %) | 19,685 qps (1.0 %) | 6,329 qps (3.6 %) |
| `read-bar-lookup` | 36,900 qps (3.0 %) | 36,496 qps (2.2 %) | 41,494 qps (10.2 %) | 42,194 qps (0.8 %) |
| `read-bar-range` | 1,723 qps (3.5 %) | 1,755 qps (2.6 %) | 1,739 qps (0.7 %) | 1,740 qps (1.3 %) |
| `read-symbol-history` | 47 qps (0.7 %) | 1,262 qps (3.7 %) | 1,209 qps (3.7 %) | 277 qps (0.5 %) |
| `read-day-slice` | 799 qps (16.3 %) | 32,468 qps (2.6 %) | 19,305 qps (21.2 %) | 4,275 qps (5.3 %) |
| `read-join-point` | 14,205 qps (37.0 %) | 21,978 qps (1.5 %) | 20,619 qps (1.6 %) | 20,284 qps (1.0 %) |
| `read-join-exists` | 236 qps (16.5 %) | 31,847 qps (1.6 %) | 14,641 qps (2.0 %) | 1,407 qps (2.3 %) |
| `agg-global` | 439 qps (7.2 %) | 23,148 qps (2.5 %) | 14,925 qps (1.5 %) | 2,853 qps (0.8 %) |
| `agg-by-symbol` | 281 qps (2.4 %) | 24,038 qps (1.7 %) | 11,614 qps (0.7 %) | 1,761 qps (2.8 %) |
| `agg-by-session` | 81 qps (1.9 %) | 2,585 qps (16.8 %) | 2,249 qps (5.9 %) | 450 qps (1.1 %) |
| `agg-day-slice` | 807 qps (15.5 %) | 32,468 qps (3.2 %) | 20,619 qps (6.1 %) | 4,602 qps (6.9 %) |
| `agg-distinct` | 492 qps (4.3 %) | 31,056 qps (1.5 %) | 17,730 qps (2.8 %) | 2,862 qps (2.6 %) |

Everything that returns or aggregates over whole relations scales with rows (`read-symbol-history` 1,262 qps at 252 bars to 47 qps at 60,480), and the fixed-cost shapes do not: `read-bar-lookup` is 36,496 qps (252 bars), 41,494 qps (1,008), 42,194 qps (10,080) and 36,900 qps (60,480), and `read-bar-range`
(200 rows returned, a pk range) is 1,723 to 1,755 qps at all four. Its 580.5 µs mean at 60,480 bars is a **per-returned-row** cost: (580.5 - 27.1) µs / 200 rows = 2.77 µs per row, and `read-symbol-history` at 60,480 bars gives (21,436.1 - 27.1) µs / 7,560 rows = 2.83 µs per row,
where 27.1 µs is that run's `read-bar-lookup` mean, taken as the fixed term.
Two independent shapes agree on about 2.8 µs per returned row.

The full-size backtest join runs at 778 qps (`backtest-read`, 360 cross-sections, 11.6 % spread) and the replay at 771 qps (19.8 %, no claim); `compare-all` is one statement per run (105 qps, 14.9 %).

Latency distributions, one table per size (median over three runs of each percentile):

### 60,480 bars
| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load-exchanges` | 2 ops | 341.5 µs | 280.8 µs | 280.8 µs | 280.8 µs | 367.8 µs | 367.8 µs |
| `load-symbols` | 8 ops | 169.1 µs | 138.1 µs | 139.3 µs | 143.8 µs | 257.1 µs | 257.1 µs |
| `load-sessions` | 7,560 ops | 21.3 µs | 18.9 µs | 20.3 µs | 20.5 µs | 21.5 µs | 27.0 µs |
| `load-bars` | 60,480 ops | 26.3 µs | 22.7 µs | 24.2 µs | 24.5 µs | 27.0 µs | 62.3 µs |
| `load-stats` | 60,480 ops | 26.2 µs | 23.7 µs | 25.3 µs | 25.5 µs | 26.9 µs | 33.4 µs |
| `load-models` | 8 ops | 169.9 µs | 138.7 µs | 139.2 µs | 141.6 µs | 320.3 µs | 320.3 µs |
| `backtest-read` | 360 ops | 1,285.3 µs | 1,196.0 µs | 1,261.3 µs | 1,278.8 µs | 1,338.2 µs | 1,461.2 µs |
| `result-insert` | 2,872 ops | 167.2 µs | 134.5 µs | 144.2 µs | 148.8 µs | 260.0 µs | 468.9 µs |
| `backtest-replay` | 360 ops | 1,296.3 µs | 1,211.3 µs | 1,261.8 µs | 1,275.0 µs | 1,467.5 µs | 1,541.6 µs |
| `compare-all` | 1 ops | 9,512.6 µs | 9,512.6 µs | 9,512.6 µs | 9,512.6 µs | 9,512.6 µs | 9,512.6 µs |
| `compare-one` | 32 ops | 697.8 µs | 569.7 µs | 688.2 µs | 713.6 µs | 775.4 µs | 838.1 µs |
| `read-bar-lookup` | 200 ops | 27.1 µs | 22.7 µs | 24.0 µs | 24.3 µs | 25.5 µs | 94.4 µs |
| `read-bar-range` | 200 ops | 580.5 µs | 460.1 µs | 574.9 µs | 599.7 µs | 650.9 µs | 672.3 µs |
| `read-symbol-history` | 10 ops | 21,436.1 µs | 20,446.0 µs | 21,092.3 µs | 21,272.7 µs | 22,259.1 µs | 22,259.1 µs |
| `read-day-slice` | 200 ops | 1,251.1 µs | 1,151.7 µs | 1,197.3 µs | 1,212.5 µs | 1,423.2 µs | 1,488.2 µs |
| `read-join-point` | 200 ops | 70.4 µs | 50.3 µs | 52.5 µs | 58.3 µs | 87.0 µs | 117.5 µs |
| `read-join-exists` | 10 ops | 4,242.1 µs | 4,077.6 µs | 4,091.8 µs | 4,226.0 µs | 4,500.8 µs | 4,500.8 µs |
| `agg-global` | 10 ops | 2,277.2 µs | 2,155.4 µs | 2,169.7 µs | 2,189.0 µs | 2,452.6 µs | 2,452.6 µs |
| `agg-by-symbol` | 10 ops | 3,556.1 µs | 3,418.0 µs | 3,483.5 µs | 3,533.3 µs | 3,717.1 µs | 3,717.1 µs |
| `agg-by-session` | 10 ops | 12,385.1 µs | 11,692.7 µs | 11,752.8 µs | 11,833.3 µs | 17,199.4 µs | 17,199.4 µs |
| `agg-day-slice` | 200 ops | 1,239.7 µs | 1,128.8 µs | 1,169.2 µs | 1,190.9 µs | 1,412.4 µs | 1,456.7 µs |
| `agg-distinct` | 10 ops | 2,032.9 µs | 1,940.4 µs | 1,955.4 µs | 1,970.1 µs | 2,318.7 µs | 2,318.7 µs |

### 252 bars
| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load-exchanges` | 1 ops | 313.0 µs | 313.0 µs | 313.0 µs | 313.0 µs | 313.0 µs | 313.0 µs |
| `load-symbols` | 1 ops | 259.0 µs | 259.0 µs | 259.0 µs | 259.0 µs | 259.0 µs | 259.0 µs |
| `load-sessions` | 252 ops | 21.8 µs | 19.2 µs | 20.4 µs | 20.7 µs | 23.4 µs | 34.8 µs |
| `load-bars` | 252 ops | 27.7 µs | 23.1 µs | 24.2 µs | 24.5 µs | 28.3 µs | 166.0 µs |
| `load-stats` | 252 ops | 26.0 µs | 24.0 µs | 25.3 µs | 25.5 µs | 26.9 µs | 32.3 µs |
| `load-models` | 8 ops | 162.7 µs | 141.0 µs | 141.0 µs | 144.0 µs | 223.1 µs | 223.1 µs |
| `backtest-read` | 12 ops | 112.7 µs | 92.9 µs | 94.1 µs | 95.8 µs | 270.5 µs | 270.5 µs |
| `result-insert` | 88 ops | 147.0 µs | 134.5 µs | 136.8 µs | 139.5 µs | 166.3 µs | 209.1 µs |
| `backtest-replay` | 12 ops | 74.4 µs | 67.2 µs | 68.9 µs | 71.0 µs | 94.7 µs | 94.7 µs |
| `compare-all` | 1 ops | 262.2 µs | 262.2 µs | 262.2 µs | 262.2 µs | 262.2 µs | 262.2 µs |
| `compare-one` | 32 ops | 50.7 µs | 48.2 µs | 49.0 µs | 49.4 µs | 56.3 µs | 78.0 µs |
| `read-bar-lookup` | 200 ops | 27.4 µs | 26.3 µs | 27.1 µs | 27.4 µs | 28.3 µs | 29.5 µs |
| `read-bar-range` | 200 ops | 569.9 µs | 457.6 µs | 485.9 µs | 594.8 µs | 640.9 µs | 666.8 µs |
| `read-symbol-history` | 10 ops | 792.5 µs | 696.8 µs | 730.6 µs | 811.3 µs | 865.0 µs | 865.0 µs |
| `read-day-slice` | 200 ops | 30.8 µs | 26.7 µs | 27.5 µs | 28.0 µs | 30.5 µs | 91.9 µs |
| `read-join-point` | 200 ops | 45.5 µs | 43.2 µs | 44.2 µs | 44.6 µs | 46.4 µs | 52.3 µs |
| `read-join-exists` | 10 ops | 31.4 µs | 29.0 µs | 29.2 µs | 29.4 µs | 43.4 µs | 43.4 µs |
| `agg-global` | 10 ops | 43.2 µs | 32.9 µs | 33.3 µs | 33.8 µs | 121.0 µs | 121.0 µs |
| `agg-by-symbol` | 10 ops | 41.6 µs | 39.4 µs | 39.9 µs | 40.1 µs | 50.0 µs | 50.0 µs |
| `agg-by-session` | 10 ops | 386.8 µs | 331.6 µs | 339.4 µs | 346.4 µs | 515.3 µs | 515.3 µs |
| `agg-day-slice` | 200 ops | 30.8 µs | 28.0 µs | 28.9 µs | 29.3 µs | 30.8 µs | 53.7 µs |
| `agg-distinct` | 10 ops | 32.2 µs | 28.9 µs | 29.2 µs | 29.6 µs | 54.6 µs | 54.6 µs |

### 1,008 bars
| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load-exchanges` | 2 ops | 233.1 µs | 168.4 µs | 168.4 µs | 168.4 µs | 287.3 µs | 287.3 µs |
| `load-symbols` | 4 ops | 171.6 µs | 145.0 µs | 145.0 µs | 150.3 µs | 230.0 µs | 230.0 µs |
| `load-sessions` | 252 ops | 21.2 µs | 19.0 µs | 20.1 µs | 20.3 µs | 22.3 µs | 30.7 µs |
| `load-bars` | 1,008 ops | 29.8 µs | 23.0 µs | 24.3 µs | 24.6 µs | 31.0 µs | 243.8 µs |
| `load-stats` | 1,008 ops | 25.7 µs | 23.9 µs | 25.3 µs | 25.5 µs | 26.8 µs | 31.5 µs |
| `load-models` | 8 ops | 153.3 µs | 138.7 µs | 139.4 µs | 141.4 µs | 226.9 µs | 226.9 µs |
| `backtest-read` | 12 ops | 162.3 µs | 137.8 µs | 139.7 µs | 144.9 µs | 338.4 µs | 338.4 µs |
| `result-insert` | 88 ops | 145.9 µs | 133.5 µs | 137.1 µs | 139.0 µs | 165.1 µs | 207.6 µs |
| `backtest-replay` | 12 ops | 119.0 µs | 112.5 µs | 114.0 µs | 116.7 µs | 138.6 µs | 138.6 µs |
| `compare-all` | 1 ops | 263.2 µs | 263.2 µs | 263.2 µs | 263.2 µs | 263.2 µs | 263.2 µs |
| `compare-one` | 32 ops | 50.8 µs | 48.8 µs | 49.2 µs | 49.9 µs | 55.4 µs | 77.8 µs |
| `read-bar-lookup` | 200 ops | 24.1 µs | 22.5 µs | 23.6 µs | 23.8 µs | 24.6 µs | 25.4 µs |
| `read-bar-range` | 200 ops | 575.0 µs | 463.2 µs | 490.9 µs | 598.2 µs | 652.1 µs | 671.2 µs |
| `read-symbol-history` | 10 ops | 827.3 µs | 704.3 µs | 818.8 µs | 838.8 µs | 898.2 µs | 898.2 µs |
| `read-day-slice` | 200 ops | 51.8 µs | 48.8 µs | 49.8 µs | 50.4 µs | 53.6 µs | 57.7 µs |
| `read-join-point` | 200 ops | 48.5 µs | 46.5 µs | 47.3 µs | 48.0 µs | 49.5 µs | 59.2 µs |
| `read-join-exists` | 10 ops | 68.3 µs | 64.0 µs | 65.0 µs | 65.4 µs | 80.7 µs | 80.7 µs |
| `agg-global` | 10 ops | 67.0 µs | 56.5 µs | 57.1 µs | 57.5 µs | 143.7 µs | 143.7 µs |
| `agg-by-symbol` | 10 ops | 86.1 µs | 82.3 µs | 83.3 µs | 84.1 µs | 96.0 µs | 96.0 µs |
| `agg-by-session` | 10 ops | 444.6 µs | 375.6 µs | 385.5 µs | 391.4 µs | 588.6 µs | 588.6 µs |
| `agg-day-slice` | 200 ops | 48.5 µs | 45.8 µs | 46.7 µs | 47.4 µs | 50.7 µs | 69.9 µs |
| `agg-distinct` | 10 ops | 56.4 µs | 53.0 µs | 53.2 µs | 54.1 µs | 77.1 µs | 77.1 µs |

### 10,080 bars
| phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load-exchanges` | 2 ops | 269.8 µs | 253.8 µs | 253.8 µs | 253.8 µs | 293.8 µs | 293.8 µs |
| `load-symbols` | 8 ops | 155.9 µs | 139.7 µs | 140.6 µs | 143.4 µs | 231.8 µs | 231.8 µs |
| `load-sessions` | 1,260 ops | 21.3 µs | 18.9 µs | 20.2 µs | 20.4 µs | 21.7 µs | 46.6 µs |
| `load-bars` | 10,080 ops | 29.6 µs | 22.9 µs | 24.4 µs | 24.7 µs | 51.6 µs | 91.6 µs |
| `load-stats` | 10,080 ops | 26.1 µs | 23.9 µs | 25.3 µs | 25.5 µs | 26.9 µs | 32.1 µs |
| `load-models` | 8 ops | 165.0 µs | 139.2 µs | 140.3 µs | 151.1 µs | 255.8 µs | 255.8 µs |
| `backtest-read` | 60 ops | 315.0 µs | 299.8 µs | 304.3 µs | 308.0 µs | 328.9 µs | 523.6 µs |
| `result-insert` | 472 ops | 145.8 µs | 135.3 µs | 138.9 µs | 141.0 µs | 167.2 µs | 197.5 µs |
| `backtest-replay` | 60 ops | 280.3 µs | 270.4 µs | 276.0 µs | 278.9 µs | 291.5 µs | 318.6 µs |
| `compare-all` | 1 ops | 1,100.7 µs | 1,100.7 µs | 1,100.7 µs | 1,100.7 µs | 1,100.7 µs | 1,100.7 µs |
| `compare-one` | 32 ops | 158.0 µs | 122.1 µs | 124.4 µs | 130.1 µs | 309.2 µs | 319.6 µs |
| `read-bar-lookup` | 200 ops | 23.7 µs | 22.2 µs | 23.3 µs | 23.5 µs | 24.4 µs | 26.1 µs |
| `read-bar-range` | 200 ops | 574.6 µs | 463.6 µs | 490.3 µs | 594.7 µs | 643.5 µs | 658.2 µs |
| `read-symbol-history` | 10 ops | 3,607.4 µs | 3,522.5 µs | 3,563.3 µs | 3,603.9 µs | 3,719.6 µs | 3,719.6 µs |
| `read-day-slice` | 200 ops | 233.9 µs | 212.9 µs | 218.5 µs | 221.1 µs | 297.9 µs | 490.5 µs |
| `read-join-point` | 200 ops | 49.3 µs | 46.8 µs | 47.9 µs | 48.7 µs | 50.1 µs | 60.9 µs |
| `read-join-exists` | 10 ops | 710.8 µs | 691.9 µs | 697.7 µs | 703.8 µs | 747.7 µs | 747.7 µs |
| `agg-global` | 10 ops | 350.5 µs | 327.9 µs | 336.2 µs | 339.8 µs | 440.9 µs | 440.9 µs |
| `agg-by-symbol` | 10 ops | 567.8 µs | 551.1 µs | 557.6 µs | 565.4 µs | 582.9 µs | 582.9 µs |
| `agg-by-session` | 10 ops | 2,221.0 µs | 2,044.8 µs | 2,192.4 µs | 2,220.5 µs | 2,313.5 µs | 2,313.5 µs |
| `agg-day-slice` | 200 ops | 217.3 µs | 205.1 µs | 211.3 µs | 215.1 µs | 223.3 µs | 288.2 µs |
| `agg-distinct` | 10 ops | 349.4 µs | 340.5 µs | 341.9 µs | 345.4 µs | 388.8 µs | 388.8 µs |

**Read shapes by case** (100 statements per cell, 8 distinct arguments in the warm cases, median qps of three runs and the spread; `index` is warm with a btree on the filter column created at runtime and `ANALYZE`d, `-` where the filter is the pk; `†` = a spread above 20 % on either side of the ratio):

### 60,480 bars
| shape | cold | warm | with index | after drop | index / warm | v3.3.0 warm (spread) | warm ratio |
|---|---|---|---|---|---|---|---|
| `bar-lookup` | 13,973 qps (7.5 %) | 32,466 qps (30.8 %) | - | - | - | 14,673 qps (1.7 %) | 2.21x † |
| `bar-range` | 1,670 qps (4.4 %) | 1,709 qps (2.0 %) | - | - | - | 1,400 qps (6.0 %) | 1.22x |
| `day-slice` | 807 qps (14.6 %) | 812 qps (10.4 %) | 6,588 qps (39.5 %) | 771 qps (21.8 %) | 8.12x | 414 qps (17.8 %) | 1.96x |
| `symbol-history` | 46 qps (0.2 %) | 46 qps (0.2 %) | 48 qps (0.8 %) | 46 qps (0.4 %) | 1.05x | 32 qps (8.0 %) | 1.42x |
| `cross-join` | 715 qps (18.8 %) | 716 qps (27.0 %) | 4,873 qps (18.9 %) | 726 qps (18.6 %) | 6.81x | 393 qps (19.5 %) | 1.82x † |
| `point-join` | 11,889 qps (45.8 %) | 15,772 qps (14.2 %) | - | - | - | 7,071 qps (2.4 %) | 2.23x |
| `model-join` | 1,415 qps (15.4 %) | 1,446 qps (5.1 %) | 1,473 qps (8.4 %) | 1,451 qps (4.2 %) | 1.02x | 1,092 qps (0.6 %) | 1.32x |

### 252 bars
| shape | cold | warm | with index | after drop | index / warm | v3.3.0 warm (spread) | warm ratio |
|---|---|---|---|---|---|---|---|
| `bar-lookup` | 35,594 qps (3.3 %) | 36,321 qps (2.6 %) | - | - | - | 9,990 qps (36.9 %) | 3.64x † |
| `bar-range` | 1,750 qps (5.2 %) | 1,708 qps (1.2 %) | - | - | - | 1,319 qps (10.3 %) | 1.30x |
| `day-slice` | 32,048 qps (7.6 %) | 35,637 qps (2.3 %) | 33,608 qps (2.0 %) | 34,885 qps (1.9 %) | 0.94x | 13,722 qps (26.7 %) | 2.60x † |
| `symbol-history` | 1,406 qps (1.1 %) | 1,223 qps (2.8 %) | 1,229 qps (1.0 %) | 1,228 qps (2.1 %) | 1.00x | 927 qps (5.1 %) | 1.32x |
| `cross-join` | 13,880 qps (2.9 %) | 14,868 qps (1.7 %) | 14,285 qps (1.2 %) | 14,551 qps (1.5 %) | 0.96x | 5,667 qps (15.3 %) | 2.62x |
| `point-join` | 21,935 qps (2.0 %) | 22,394 qps (2.4 %) | - | - | - | 6,354 qps (24.7 %) | 3.52x † |
| `model-join` | 18,882 qps (2.9 %) | 20,364 qps (0.8 %) | 21,221 qps (1.2 %) | 21,513 qps (1.8 %) | 1.04x | 6,597 qps (23.5 %) | 3.09x † |

### 1,008 bars
| shape | cold | warm | with index | after drop | index / warm | v3.3.0 warm (spread) | warm ratio |
|---|---|---|---|---|---|---|---|
| `bar-lookup` | 41,899 qps (2.5 %) | 42,624 qps (3.2 %) | - | - | - | 11,446 qps (26.5 %) | 3.72x † |
| `bar-range` | 1,703 qps (5.5 %) | 1,663 qps (3.7 %) | - | - | - | 1,279 qps (12.1 %) | 1.30x |
| `day-slice` | 15,228 qps (21.0 %) | 19,827 qps (1.2 %) | 27,501 qps (1.5 %) | 19,425 qps (1.2 %) | 1.39x | 7,652 qps (18.5 %) | 2.59x |
| `symbol-history` | 1,315 qps (12.5 %) | 1,204 qps (1.0 %) | 1,230 qps (5.0 %) | 1,224 qps (1.5 %) | 1.02x | 880 qps (13.7 %) | 1.37x |
| `cross-join` | 7,977 qps (17.0 %) | 8,849 qps (16.4 %) | 9,971 qps (1.1 %) | 8,736 qps (0.8 %) | 1.13x | 3,399 qps (13.5 %) | 2.60x |
| `point-join` | 20,244 qps (1.4 %) | 20,430 qps (7.0 %) | - | - | - | 7,120 qps (17.9 %) | 2.87x |
| `model-join` | 18,429 qps (3.0 %) | 19,196 qps (14.8 %) | 21,013 qps (1.7 %) | 21,164 qps (6.5 %) | 1.09x | 8,068 qps (21.0 %) | 2.38x † |

### 10,080 bars
| shape | cold | warm | with index | after drop | index / warm | v3.3.0 warm (spread) | warm ratio |
|---|---|---|---|---|---|---|---|
| `bar-lookup` | 38,887 qps (7.7 %) | 37,806 qps (6.2 %) | - | - | - | 14,171 qps (30.9 %) | 2.67x † |
| `bar-range` | 1,724 qps (2.2 %) | 1,681 qps (1.7 %) | - | - | - | 1,355 qps (6.9 %) | 1.24x |
| `day-slice` | 3,809 qps (19.4 %) | 4,474 qps (11.8 %) | 15,211 qps (24.9 %) | 4,369 qps (20.5 %) | 3.40x | 2,058 qps (6.1 %) | 2.17x |
| `symbol-history` | 277 qps (0.2 %) | 271 qps (1.5 %) | 273 qps (1.8 %) | 271 qps (1.6 %) | 1.01x | 195 qps (10.8 %) | 1.39x |
| `cross-join` | 2,965 qps (8.1 %) | 3,656 qps (23.8 %) | 8,704 qps (9.6 %) | 3,572 qps (11.5 %) | 2.38x | 1,682 qps (11.7 %) | 2.17x † |
| `point-join` | 20,056 qps (1.7 %) | 20,266 qps (1.1 %) | - | - | - | 7,010 qps (2.6 %) | 2.89x |
| `model-join` | 5,513 qps (41.7 %) | 7,702 qps (6.3 %) | 8,081 qps (2.5 %) | 7,491 qps (22.6 %) | 1.05x | 4,190 qps (2.6 %) | 1.84x |

**An index on the filter column pays only where there are rows to skip**: at 60,480 bars `day-slice` gains 8.12x (a 39.5 % spread on the indexed side: no claim) and `cross-join` 6.81x (18.9 %), at 10,080 bars 3.40x (24.9 %, no claim) and 2.38x, at 1,008 bars 1.39x and 1.13x, at 252 bars 0.94x and 0.96x,
the last two inside the noise. `symbol-history` and `model-join` do not gain (1.05x and 1.02x at full size); `symbol-history` returns 7,560 rows and its time is the per-returned-row cost (2.83 µs per row from this mean, 2.77 µs from `bar-range`, measured independently), so the filter is not where its time goes. The cold and warm cells of the scans are within their spreads at full size (`day-slice` 807 and 812 qps, `symbol-history` 46 and 46 qps, `cross-join` 715 and 716 qps); `bar-lookup` at full size is the one shape where a cold argument is slower (13,973 qps cold, 32,466 qps warm, 30.8 % spread on the warm side). Whether the data is resident in `shared_buffers` was not checked by a buffer-hit count, and the cause of the `bar-lookup` gap is not separated here.

**Write sweep (rule 3: the durability wait).** Rows per second inserting 2,000 rows, one round trip per row, by rows per transaction; the sweep is independent of the bars loaded, and its four columns are four more measurements of the same thing (the spread of the autocommit row across them, 5,915 to 6,039 rows/s, is the engine's noise floor):

| rows per transaction | 60,480 bars | 252 bars | 1,008 bars | 10,080 bars |
|---|---|---|---|---|
| 1 (autocommit) | 5,988 rows/s (5.7 %) | 5,915 rows/s (3.4 %) | 6,039 rows/s (4.1 %) | 5,961 rows/s (1.4 %) |
| 10 | 29,046 rows/s (0.7 %) | 28,866 rows/s (2.0 %) | 29,180 rows/s (12.5 %) | 28,874 rows/s (1.9 %) |
| 100 | 47,792 rows/s (3.4 %) | 47,677 rows/s (2.5 %) | 47,691 rows/s (2.8 %) | 47,293 rows/s (6.6 %) |
| 1000 | 49,428 rows/s (8.7 %) | 50,072 rows/s (2.4 %) | 48,963 rows/s (8.4 %) | 47,815 rows/s (5.3 %) |

An autocommit insert costs 1,000,000 / 5,988 = 167.0 µs and a 1,000-row batch 20.2 µs per row, so one durable commit costs 146.8 µs; scenario 2's `commit` mean is 138.2 µs, p0 125.4 µs. The `load-*` autocommit phases of the matrix
are the same floor (the `load-models` rows, 8 inserts: 142 to 152 µs p50).

**Connection sweep.** The backtest's 3-relation join, 200 operations per connection count, PostgreSQL forking one backend per connection (the one place where PostgreSQL's aggregate rate is genuine parallel execution and not only round-trip overlap), the backends confined to CPUs 0-7:

| connections | 60,480 bars | 252 bars | 1,008 bars | 10,080 bars |
|---|---|---|---|---|
| 1 | 746 qps (24.0 %) † | 11,846 qps (8.0 %) | 7,600 qps (5.3 %) | 3,450 qps (10.2 %) |
| 2 | 1,375 qps (8.2 %) | 16,466 qps (17.3 %) | 9,675 qps (64.5 %) † | 5,845 qps (18.5 %) |
| 4 | 2,453 qps (22.6 %) † | 20,234 qps (16.7 %) | 13,190 qps (75.1 %) † | 8,803 qps (9.7 %) |
| 8 | 4,626 qps (11.4 %) | 18,849 qps (45.5 %) † | 14,612 qps (29.1 %) † | 11,836 qps (87.8 %) † |

At 60,480 bars the curve climbs: 746, 1,375, 2,453, 4,626 qps for 1, 2, 4 and 8 connections (6.2x at eight, 11.4 % spread at 8), which is the shape's own scaling because every connection runs a 1.3 ms join. At 252, 1,008 and 10,080 bars a 200-operation point lasts 5 to 60 ms and the run's
own length and client start-up dominate: 252 bars rises to 20,234 qps at 4 and then falls to 18,849 qps at 8 with a 45.5 % spread. **Nothing in this run found the bound on the connection sweep**: the server's CPU was not sampled, and the driver, one Python process per connection count on the E-cores, is a candidate
the run cannot exclude. The 4- and 8-connection points of the smaller sizes carry no claim.

## 9. Against v3.3.0's PostgreSQL 18.6: what the host change moved

This section is the only comparison in the file, and it is labelled for what it is: **the host (an 8-vCPU AMD EPYC 9V74 cloud VM on `/dev/root` ext4, unpinned, against this 24-CPU hybrid desktop on xfs/NVMe, pinned), the PostgreSQL minor version (18.6 against 18.7) and the install (native, `/home/cdkbs/pg-bench-ba`, against a container with `--network host`) changed together**, and no ratio below is attributable to any one of them.
The v3.3.0 numbers are re-derived from that run's archived JSON (`bench/v3.3.0/archive/scenario{1,2,3,4}-v3.3.0-33-g9682b725/json/`, three runs per cell) by the same spread rule. Its tools differ from this run's by nothing in the PostgreSQL drivers (section 1).

What the ratios show is that the move did not scale PostgreSQL uniformly: **it moved each kind of wait by a different factor**, in the order the wait is made of fewer CPU cycles.

| wait | measure here | measure at v3.3.0 | ratio |
|---|---|---|---|
| durable commit | scenario 2 `commit` mean 138.2 µs (p0 125.4 µs) | 1,343.4 µs (p0 not tabulated; p50 1,278.8 µs) | **9.7x lower** |
| durable commit | autocommit insert, scenario 1 write sweep, 5,988 rows/s | 739 rows/s | 8.10x |
| booking, commit included | 2,087.1 tps | 419.5 tps | 4.98x |
| fixed round trip | `read-bar-lookup` mean 27.1 µs (p0 22.7 µs) | 70.8 µs (p0 65.0 µs) | 2.6x lower (p0 2.9x) |
| per row examined | unindexed equality, 10,000 loans, 17.6 ns per row | 34.2 ns per row | 1.94x lower |
| per row returned | `read-bar-range` 2.77 µs per row; `read-symbol-history` 2.83 µs per row | 3.26 and 4.04 µs per row | 1.18x and 1.43x lower |

The commit moved by the device (an fsync on an NVMe volume against the cloud VM's disk), the round trip by the CPU and the loopback path, and the per-row costs the least. Practical consequence for a later KDS run: **a ratio of KDS to PostgreSQL computed against the v3.3.0 PostgreSQL numbers
would have been misstated by 1.2x to 9.7x depending on the shape**, and the shapes bounded by durability are the ones where it would have been most misstated. The ratios of the matrices are above: scenario 2 at 4.98x; scenario 3 at 1.4x to 3.9x (geometric means 1.92x to 2.93x per cell); scenario 4's `board` 2.02x, `tape10k` 2.05x, `pk` 2.99x, `close` 1.78x and the load 3.15x;
scenario 1's bulk loads (`load-sessions`, `load-bars`, `load-stats`) 2.5x to 3.5x, the autocommit-dominated `result-insert` 7.7x to 9.0x, and the reads and aggregates 1.2x to 4.0x, with one cell (`compare-all` at 60,480 bars, 0.88x) inside its spread.

## 10. What the run teaches

1. **A durable booking on this host is 29 % commit and 64 % statements.** The commit floor (p0 125.4 µs) is what a durable single-connection KDS booker also pays on this device; whatever a later KDS run adds above that floor in the commit
   is KDS's. The v3.3.0 summary read one durable KDS booker as equal to PostgreSQL because both wait on the same fsync, at a commit of 1,343 µs; here the commit is a tenth of that and the statements, 19.6 to 38.8 µs each, are 64 % of the booking, so
   that reading does not carry over to this host's numbers. Whether it holds for KDS on this host is the later run's question, and this file does not predict it.
2. **The per-row terms are linear and small**: about 18 to 20 ns per row examined, 2.8 µs per row returned and 22 to 27 µs fixed. They give a line for each of scenario 3's and scenario 4's unindexed cells, and a KDS cell that sits off the line says which term it differs in.
3. **PostgreSQL's index-gain curve against rows is the control for any KDS index or Cabin gain curve**: nothing at 200 loans, 1.3x to 1.7x on the equalities at 1,000, and 4.7x to 7.6x on the `loans` equalities at 10,000.
4. **The first phase of a short cell is not a measurement of the engine** (section 4, `pk-user`): on this host a 200-operation first phase has a 12 % to 120 % spread. A later KDS run that uses a pk lookup as its control in a first phase has the same problem.
5. No decision recorded in `CLAUDE.md` or a spec is contradicted or informed by this run: it measures PostgreSQL only. The run supports no insight about KDS's engine, and none is manufactured.

## 11. What is cut, absent or unverified

| Item | State |
|---|---|
| KDS side | **not run in this window**; no KDS binary, no `cores` axis, no `strict`/`group` pairing measured here. The pairing of PostgreSQL's `synchronous_commit = on` with both rungs follows v3.3.0 |
| Scenario 4 with an index on `symbol` | **absent**: the twin builds none (a known gap of the twin). Only day 1 runs, unpaced |
| PostgreSQL with several bookers | **absent**: `pg-s2-b1` is one connection; the v3.3.0 booker sweep has no PostgreSQL counterpart |
| Saturation (rule 4b) | not reached: the connection sweep's bound was not found (section 8) and `synchronous_commit = on` is the durability extreme, so `strict`'s counterpart is measured; no CPU sampler ran |
| PostgreSQL 17 | not measured (another series' scope) |
| Cause of attempt 1's delayed timer | unverified |
| Cause of the slow first phase; of `pg-s4` run 1's 13 ms stall | unverified (a frequency trace and a `log_checkpoints` correlation would settle them) |
| Whether `pg-s2-b1` run 1 was affected by the foreign `kds_tests` | not excludable (section 4); no effect visible |
| Whether the E-core pinning of the drivers sets the 2.8 µs per returned row | unverified; a driver profile on P-cores would say |
| Suite | not executed; no code changed |

## 12. Archive and re-run

Raw files: `bench/v3.5.0/archive/pg-control-v3.4.17-202-g481d101aa/` holds the 36 JSON summaries (`json/`), the stdout, return code, precheck and gate wait of every cell (`logs/`), the server log and container output, the server's settings and affinity (`conf/`), `timeline`,
the watchdog's log and the three scripts that ran the cells, and attempt 1's records. It holds no data file. The tools snapshot is not archived: it is `481d101aa`'s `tools/`.

To re-run on this host: start `docker.io/library/postgres:18` with `--network host`, `-e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=bench -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C"` and the two command-line keys of section 1 on a free port; apply `taskset -a -cp 0-7` to the postmaster and its children;
then run `run_all.sh` of the archive with the drivers from `tools/` under `taskset -c 8-23`. The drivers' flags are in each driver's own `--help` and header (`bench/docs/` does not exist in this tree; `bench/README.md` carries the rules). Every cell is one `--suffix` over one `--json` path.
