# DA-S0 premise: the price of today's `sys.access_stats` write

`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md` §1.1, DA-Q7 (a), §5 DA-S0.

**Finding.** At `cores = 1` the write costs +0.60 us of server CPU a point `SELECT` (2.7 %), which is at
the edge of the same-binary spread (within-arm range 0.75-0.78 us) and not outside it. At `cores = 8`
with every core loaded it costs +8.19 us (+24 %) and 11.6 % of throughput, in 6 of 6 paired lifetimes
and about five times the within-arm range: outside the spread. The cell §1.1 names as unmeasured
(page 11's exclusive hold above one core under short statements) is where the cost is. An
`INSERT` into a child relation with a foreign key shows no delta (+0.08 us, 3 of 6).

## Stamp

| item | value |
|---|---|
| executed | 2026-11-19 22:46-22:53 UTC (driver start 22:46:22, DONE 22:52:50) |
| branch / commit | `worktree-da-access-stats-memory-resident` at `673f025d`, `git describe --tags` = `v3.4.14-30-g673f025d`; measured from `git archive 673f025d` extracted to `/home/louis/ck-tmp/da-s0-base`, so the tree is clean by construction (the DA worktree was being edited in parallel and was not built) |
| binary | `build-release/kds_server` built from that archive (CMake Release), mtime 2026-11-19 22:45:51 UTC, after the commit; copy at `/home/louis/bench-runs/da-s0/kds_server`, sha256 `dc66d5dccab3bd713b64ef063eb85dcbf48613942adf7a0be8499091101b90cc`; every server started from the copy |
| device | data files under `/home/louis/bench-runs/da-s0/run` on `/dev/nvme0n1p1` (ext4); host 8 vCPU Intel Xeon Platinum 8581C |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 or 8, `access_statistics = on` or `off` (boot key; everything else default) |
| quiet gate | load average 0.20 at the start, no `cc1plus`, no `ctest`; no hold. The load average read 4.0 during the runs is this run's own clients. The build ran before the measurement (about 1 minute) |
| budget | build + smoke + run about 9 minutes of the 20 |

## Method

Driver `da_s0.py` (archived beside the raw data; not yet entered in `bench/docs/`, which a scenario driver would require). One server lifetime per
arm per round, fresh data file, 6 rounds, arms alternated per round (on first in odd rounds, off first in even), 2 s warm then 8 s measured.
Server CPU is `utime + stime` of the server process from `/proc/<pid>/stat` over the measured window divided by completed statements, so it
includes the server's kernel socket time. Tables: `t(id int64, v int64, w int64) BTREE`, 10,000 rows, `SELECT * FROM t WHERE id = <random>`
(a tuple key point read; does not scale with rows, one size measured, which departs from bench rule 9 for this premise cell).

| cell | cores | clients | placement |
|---|---|---|---|
| select1 | 1 | 1 serial Python process | server pinned to CPU 2, client off CPU 2 |
| select8 | 8 | 16 closed-loop Python processes | unpinned; 8 vCPU shared by server and clients, server used about 4 CPUs on average (12.7 CPU-s in 3 s in the smoke run) |
| fk1 | 1 | 1 serial | `INSERT INTO ch VALUES (k, 1..1000, k)`, `ch.pid REFERENCES par` (1,000 rows), named keys, server pinned |

## Results

CPU per statement and QPS per lifetime (`analysis.txt`, `raw.jsonl`). Spread = max - min over the 6 lifetimes of one arm. QPS is client-side completed statements per second.

| cell | arm | server CPU us/stmt, mean (per lifetime) | spread us | QPS mean | QPS spread | statements |
|---|---|---|---|---|---|---|
| select1 | on | 22.72 (22.99 22.90 22.24 22.79 22.82 22.60) | 0.75 | 23,632 | 728 | 1,135,205 |
| select1 | off | 22.12 (21.90 22.14 22.66 22.04 21.87 22.10) | 0.78 | 24,059 | 865 | 1,155,728 |
| select8 | on | 42.18 (41.75 41.08 42.62 42.71 42.68 42.20) | 1.63 | 97,720 | 4,907 | 4,703,857 |
| select8 | off | 33.98 (33.89 33.51 34.71 33.95 33.54 34.30) | 1.20 | 110,595 | 3,990 | 5,323,378 |
| fk1 insert | on | 23.00 (22.87 23.30 22.81 22.99 23.06 22.99) | 0.49 | 23,359 | 490 | 1,122,081 |
| fk1 insert | off | 22.92 (22.50 22.98 22.93 23.10 23.49 22.52) | 0.98 | 23,321 | 999 | 1,120,279 |

Paired on minus off (same round):

| cell | CPU delta us/stmt, per round | mean | rounds on > off | QPS delta, mean | outside own spread |
|---|---|---|---|---|---|
| select1 | +1.08 +0.76 -0.42 +0.75 +0.94 +0.50 | +0.60 | 5 / 6 | -1.8 % (+2.9 in round 3) | no: delta is below the 0.78 us within-arm range |
| select8 | +7.87 +7.57 +7.91 +8.76 +9.14 +7.91 | +8.19 | 6 / 6 | -11.6 % (-10.3 to -12.6) | yes: about 5x the 1.63 us range, arm ranges do not overlap |
| fk1 insert | +0.37 +0.32 -0.12 -0.10 -0.43 +0.47 | +0.08 | 3 / 6 | +0.2 % | no |

## Latency (client round trip, us; median of the six per-lifetime percentiles per arm)

Includes the Python client and the socket; it is not server time. No control cell was run, so the within-arm spread above is the noise floor.

| cell | arm | n | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|
| select1 | on | 1,135,205 | 34 | 39 | 40 | 48 | 58 |
| select1 | off | 1,155,728 | 34 | 38 | 39 | 44.5 | 55 |
| select8 | on | 4,703,857 | 31 | 95.5 | 137 | 268.5 | 534.5 |
| select8 | off | 5,323,378 | 29 | 85.5 | 123 | 236.5 | 420 |
| fk1 insert | on | 1,122,081 | 32 | 37 | 38 | 44 | 61.5 |
| fk1 insert | off | 1,120,279 | 31.5 | 37 | 38 | 45 | 64.5 |

## What it says

- Under the shape the work order could not previously price, the write is not "about 0.5 us": it is 8 us a statement and 12 % of throughput on this host. The hold of page 11 is serialising 16 clients' statements; DA's memory store would be a speed-up there, not merely "no cell slower". The mechanism (wait on the page latch versus cache-line traffic) was not separated here; `sys.wait_stats` was not read.
- At `cores = 1` the delta (+0.60 us, 5 of 6) sits at the top of the spread and matches BO's 0.35-0.52 us for its two records (`sys-views.md` §6). DA-Q7's bar "no cell slower outside its own spread" can only be tested at this cell with a larger lifetime count.
- The foreign-key insert shows nothing at `cores = 1`; the extra access record is not distinguishable there.
- The CPU-per-statement figure at `cores = 8` (34-42 us) is higher than at `cores = 1` (22 us) in both arms: server CPU there includes cross-core costs and the client processes compete for the same 8 vCPUs. Compare arms, not cores.

## Limits

One cardinality (10,000 rows) and one durability (`relaxed`); no `strict`. At `cores = 8` the clients are Python processes sharing the vCPUs, so the server was not saturated on all eight (about 4 CPUs used); a harsher load would widen the gap, not shrink it, but that was not measured. No PMU. 6 lifetimes per arm; nothing was cut for the budget (the run fit in 9 minutes). Not run: the DA-S0 census items (borrow read, page-11 walkers, clock, test pins), which are reading, not measurement.

Raw data: `bench/v3.4.14/archive/da-s0-premise-v3.4.14-30-g673f025d/` (`raw.jsonl`, `da_s0.py`, `analyze.py`, `analysis.txt`, `driver.log`, `build.log`).
