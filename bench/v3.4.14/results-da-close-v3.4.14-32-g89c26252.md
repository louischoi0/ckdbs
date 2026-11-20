# DA close: the memory-resident `sys.access_stats` against the page-resident one

`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md` DA-S2, DA-Q7 (a): no cell slower outside its own spread.

**Finding.** The bar is met. With `access_statistics = on`, DA-S1 is not slower in any cell: `select1` and `fk1` move by less than their within-arm spread (-0.22 and -0.45 us of server CPU a statement, 3 of 6 and 2 of 6 lifetimes slower), and the `cores = 8` point read, the one cell where S0 found a cost, is **8.32 us (19 %) cheaper and 13.5 % faster in 6 of 6 paired lifetimes**, about six times its within-arm range. The control (both binaries, `access_statistics = off`, `cores = 8`) differs by 0.06 us and +0.4 %, inside its spread, so the code path that is skipped is equal on both.

## Stamp

| item | value |
|---|---|
| executed | 2026-11-19 23:17-23:26 UTC (driver start about 23:17:40, DONE 23:26:13) |
| branch | `worktree-da-access-stats-memory-resident` |
| A | commit `673f025d`, `v3.4.14-30-g673f025d` (the commit DA opened from) |
| B | commit `89c26252`, `v3.4.14-32-g89c26252` (DA-S1, the closing code commit) |
| tree | each arm built from its own clean `git archive` (`/home/louis/ck-tmp/da-close-a`, `-b`, since removed), so clean by construction; the DA worktree carries uncommitted document edits and was not built |
| binaries | both built Release, 2026-11-19 23:16:40 UTC, after the commits. A copy `kds_A` sha256 `dc66d5dccab3bd713b64ef063eb85dcbf48613942adf7a0be8499091101b90cc` (identical to S0's binary, so the S0 numbers are of the same engine); B copy `kds_B` sha256 `b743a60ce0d685061ee63d73ed71c2ae0bb7667782ff453a03938471f1ac42cd`; both in `/home/louis/bench-runs/da-close/`, every server started from a copy |
| device | data files under `/home/louis/bench-runs/da-close/run` on `/dev/nvme0n1p1` (ext4, checked with `df -T`); host 8 vCPU |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 or 8, `access_statistics = on` (the shipped default) except the control cell, `off`; all else default |
| quiet gate | load average 3.3 when the driver started (7.5 just after the two parallel builds, 45 s settled before the start); no `cc1plus`. A resident `build-sandbox/kds_server` of another project (pid 560863, about 10 % of one CPU) was running throughout, on both arms alike. The load of 8 during the run is its own clients. No hold |
| budget | build 2 min, run 9 min, well inside the 1 hour; nothing cut: 6 lifetimes per arm per cell, 4 cells |

## Method

Driver `da_close.py`, the S0 driver `da_s0.py` with the two arms changed from on/off to binary A/B (archived beside the raw data; not yet in `bench/docs/`). One server lifetime per arm per round, fresh data file, arms alternated per round (A first in odd rounds, B first in even), 2 s warm then 8 s measured, `SELECT * FROM t WHERE id = <random>` over a 10,000-row table or a child `INSERT` with an FK into a 1,000-row parent. Server CPU is `utime + stime` of the server process over the measured window divided by completed statements. Only 10,000 rows were run, as in S0 (a point read and a keyed insert do not scale with rows; a departure from bench rule 9 stated rather than hidden). Statement-level waits are not decomposed (rule 3 does not apply: the unit is a CPU cost compared between two binaries, not a latency to be broken down).

| cell | cores | clients | placement | `access_statistics` |
|---|---|---|---|---|
| select1 | 1 | 1 serial | server pinned to CPU 2 | on |
| select8 | 8 | 16 closed-loop processes | unpinned | on |
| fk1 | 1 | 1 serial | server pinned, `INSERT INTO ch` with `pid REFERENCES par` | on |
| select8off (control) | 8 | 16 | unpinned | off |

## Results

Spread = max - min of the 6 lifetimes of one arm. QPS is client-side completed statements per second.

| cell | arm | CPU us/stmt, mean | CPU spread | QPS mean | QPS spread | statements |
|---|---|---|---|---|---|---|
| select1 | A | 22.53 | 1.01 | 23,755 | 1,180 | 1,141,070 |
| select1 | B | 22.30 | 0.91 | 23,957 | 998 | 1,150,780 |
| select8 | A | 42.71 | 2.23 | 97,370 | 5,643 | 4,686,400 |
| select8 | B | 34.39 | 1.34 | 110,509 | 3,151 | 5,319,180 |
| fk1 | A | 23.21 | 1.26 | 23,086 | 1,250 | 1,108,920 |
| fk1 | B | 22.76 | 0.75 | 23,440 | 709 | 1,125,932 |
| select8 off (control) | A | 34.51 | 1.11 | 108,849 | 7,131 | 5,238,470 |
| select8 off (control) | B | 34.45 | 1.72 | 109,280 | 5,600 | 5,259,672 |

Paired B minus A, same round:

| cell | CPU delta us/stmt per round | mean | B slower (CPU) in | QPS delta mean (range) | outside own spread |
|---|---|---|---|---|---|
| select1 | +0.17 -0.54 +0.13 -0.37 -1.33 +0.59 | -0.22 | 3 / 6 | +0.9 % (-2.6 to +5.8) | no (inside the 1.0 us range) |
| select8 | -8.46 -8.52 -8.54 -7.64 -7.32 -9.43 | -8.32 | 0 / 6 | +13.5 % (+9.7 to +17.9) | yes, a gain; arm ranges do not overlap |
| fk1 | +0.11 -1.17 -0.03 +0.23 -1.29 -0.58 | -0.45 | 2 / 6 | +1.6 % (-1.3 to +5.9) | no |
| select8 off (control) | +1.01 -0.49 +1.13 -0.07 -0.53 -1.39 | -0.06 | 2 / 6 | +0.4 % (-4.8 to +4.1) | no; sets the floor, about 1.7 us / 5 % |

## Latency (client round trip, us; median of the six per-lifetime percentiles)

Includes the Python client and the socket; it is not server time.

| cell | arm | n | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|
| select1 | A | 1,141,070 | 35 | 39 | 40 | 45.5 | 55.5 |
| select1 | B | 1,150,780 | 34.5 | 39 | 39 | 46 | 54.5 |
| select8 | A | 4,686,400 | 33.5 | 98 | 139.5 | 271 | 513 |
| select8 | B | 5,319,180 | 30.5 | 86.5 | 126.5 | 237.5 | 399.5 |
| fk1 | A | 1,108,920 | 33 | 37 | 38 | 44.5 | 62 |
| fk1 | B | 1,125,932 | 32.5 | 37 | 38 | 43.5 | 62 |
| select8 off | A | 5,238,470 | 30.5 | 88.5 | 129.5 | 238 | 402.5 |
| select8 off | B | 5,259,672 | 31.5 | 87.5 | 126.5 | 242 | 422.5 |

## Against S0's numbers

Same host, same driver shape, and A is S0's binary (same sha256). Reading S0 (`results-da-s0-premise-v3.4.14-30-g673f025d.md`) beside this run:

| cell | S0 on, CPU / QPS | A on, this run | S0 off | B on, this run |
|---|---|---|---|---|
| select1 | 22.72 us / 23,632 | 22.53 / 23,755 | 22.12 / 24,059 | 22.30 / 23,957 |
| select8 | 42.18 / 97,720 | 42.71 / 97,370 | 33.98 / 110,595 | 34.39 / 110,509 |
| fk1 | 23.00 / 23,359 | 23.21 / 23,086 | 22.92 / 23,321 | 22.76 / 23,440 |

A reproduces S0's on arm in every cell (within 0.5 us and 1.5 %), and B with the write on lands where S0's engine with the write off did: at `cores = 8`, +0.41 us and -0.1 % against S0's off arm, both inside the spread. S0 priced the write at +0.60 us (`select1`), +8.19 us / -11.6 % (`select8`) and +0.08 us (`fk1`); in B that price is not measurable at any of the three, and at `select8` it is gone.

## What it says

- **The `cores = 8` cost was the page-11 hold, and moving the store off the page removes it.** The cost that S0 found is exactly the one B recovers: B with statistics on is indistinguishable from S0's statistics off. The cost was serialisation of short statements across cores on one exclusive page, not the work of counting; a per-core memory store has no shared page to hold. This confirms the DA work order's premise (§1.1) with a second data point beyond S0, and it settles the question S0 left open about mechanism only in outcome: the hold, not the cache-line traffic of the counter, is what throttled (the memory store still counts, and costs nothing visible).
- **At `cores = 1` the +0.60 us S0 priced is not distinguishable now** (B is 0.22 us cheaper, 3 of 6). With a floor of about 1 us, the bar can be called met there, not that the write has been shown free: a cost under roughly 1 us a statement is below what 6 lifetimes resolve on this host.
- **The control holds**: with the write off on both binaries, the two engines are equal (-0.06 us, +0.4 %), so DA-S1 changed nothing else on the read path; the whole select8 delta is the access-statistics path.
- The foreign-key insert, which records two access rows, shows no delta in either direction.

## Limits

One cardinality and one durability (`relaxed`, not `strict`). At `cores = 8` the 16 Python clients share the 8 vCPUs with the server (about 4 CPUs of server use), so the server was not saturated on every core; a harsher load would widen the gap, not narrow it, but that was not measured. Only the access-statistics write was varied; the read side (`SHOW ACCESS` / `sys.access_stats` scan cost, the folded counts' correctness, a crash, a restart) is not covered here and is the test suite's. The suite was not run by this measurement. No PMU. The `access_statistics = on` arm on B was measured at the shipped default; the off arm on B only in the control cell.

Raw data: `bench/v3.4.14/archive/da-close-v3.4.14-32-g89c26252/` (`raw.jsonl`, `da_close.py`, `analyze.py`, `analysis.txt`, `driver.log`, `wd.log`, `run.status`, build logs).
