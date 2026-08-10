# Assertions on the write path, measured

`docs/workplan-assertion.md` AST10's benchmark half: what an enabled
`CREATE ASSERTION` costs on the INSERT path, against the same workload with no
assertion, across the three durability classes.

The thesis, and every number below is in service of it: **an assertion's
admission check is a constant ~5 µs per statement, at every relation size and
every durability class — so whether it matters is purely a question of what
the statement already costs.** Under `strict` and `group`, where an
autocommitted INSERT is a ~1,030 µs fsync with a statement attached, one
assertion is +0.4–1.1% at p50 and vanishes inside the run's own noise in the
mean. Under `relaxed`, where the fsync leaves the latency path, the same
5 µs is +4.7% at p50, and two assertions on one relation cost 6.4% of insert
throughput. A batch of one is a batch: the fsync does not make the check
free, it makes it irrelevant.

| | |
|---|---|
| **Run** | 2026-08-09, 05:55:15 – 05:58:52 UTC (nine configurations, sequential) |
| **Branch / commit** | `feat-assertion` / `cd5b5a3` ("assertion: end-to-end close-out (AST10)") |
| **Binary provenance** | `build-release/kds_server`, mtime 05:54:15 UTC, built from `cd5b5a3` (committed 05:50:55). HEAD at run time was `26821c1` (05:54:58), which touches `README.md` only — no engine source differs, so the binary measures the engine at HEAD. |
| **Tree** | No tracked file modified. Untracked: `.claude/worktrees/`, `cls/` — neither is engine source and neither is built. |
| **Build type** | Release (`-O3 -DNDEBUG`). `./build` is Debug and is not used here. |
| **Device** | `/dev/nvme0n1` (Amazon EBS, `ROTA=0`), xfs, data files under `/home/ec2-user/bench-assertion/` — one **fresh data file and fresh server process per configuration** (ports 15471–15479). Not tmpfs. |
| **Machine** | AMD EPYC 7571, 2 vCPU, 7.6 GiB, kernel 6.18.38. 1-minute load 1.21–1.54 across the run window, recorded before and after every configuration; no compiler running. Several resident agent processes held ~30% of one core throughout — the block-interleaved design and the in-run twin floor are what absorb that, and §1 reports the floor they produced. |
| **Server config** | `cores = 1`, **`durability = strict \| group \| relaxed` is the one key varied**; everything else at defaults (`isolation = read-committed`, `waystone_recording/replay = on`, `access_statistics = on`, `indexes = on`, `checkpoint_interval_ms = 5000`). |
| **Enforcement stamp** | `enforcing=on` observed via `SHOW ASSERTIONS` in **all nine runs** — the driver refuses to measure a mislabelled engine (`--expect-enforcing on`). |
| **Correctness** | `--verify` passed in all nine runs: the assertion's own aggregate (`GROUP BY grp: COUNT(*), SUM(amount)`) answers identically across all five comparison relations. Zero unexpected error replies in any measured phase. The violation phase refused **200/200** attempts in every run with `ERR ASSERTION_VIOLATION retryable=0 … COUNT(*) would exceed bound 8`. |
| **Baseline** | No PostgreSQL twin exists for this workload — §6 says why and what would build one. |

Driver, flags and exact invocation: `bench/docs/README.md`
(`assertion_benchmark.py`). This file states findings; it does not re-explain
how to run the tool. The driver was staged before enforcement existed and ran
against the enforcing server **unchanged** — it reads the `enforcing=` stamp
from the server rather than assuming it, which is what let the same tool
measure this engine state without modification.

**The size sweep.** `--rows 200 / 1000 / 10000` sets the setup rows per
relation; `--ops 200 / 1000 / 2000` measured statements per phase per
relation, identical across the three durability classes at each size, so
every comparison is equal work rather than equal time. `--groups` is 64 at
every size. The relations are 4-column BTREE
(`id int64, grp int64, amount int64, note varchar`).

---

## 0. How the comparison is kept honest

Six relations per run, five of them loaded with **byte-identical rows in the
same order** and differing only in what is declared over them:

| relation | declared |
|---|---|
| `ast_none` | nothing — the control |
| `ast_twin` | nothing — the noise floor: `none` vs `twin` is the same configuration by construction |
| `ast_cnt` | `CHECK COUNT(*) <= 2·(rows+ops)` `GROUP BY (grp)` |
| `ast_sum` | `CHECK SUM(amount) <= 2·(rows+ops)·1000` `GROUP BY (grp)` |
| `ast_multi` | both of the above — per-assertion scaling |
| `ast_cap` | `COUNT(*) <= 8` on a tiny relation, for the violation phase only; excluded from every comparison |

Ceilings carry 2× headroom over the loosest bound the workload can reach, so
the comparison phases measure the **pass path** only; the refusal path is its
own phase on its own relation (§4). Statements are block-interleaved across
the relations (25 per relation per round) with identical argument draws, so a
machine that gets busier partway through costs every column equally.

## 1. The noise floor, from inside the run

`twin` against `none` on the insert phase, per configuration:

| durability | rows | twin Δmean | twin Δp50 |
|---|---:|---:|---:|
| strict | 200 | −1.7% | −1.6% |
| strict | 1,000 | −0.1% | +0.1% |
| strict | 10,000 | +1.4% | +0.3% |
| group | 200 | −2.0% | −1.4% |
| group | 1,000 | −0.8% | −0.2% |
| group | 10,000 | +1.7% | −0.3% |
| relaxed | 200 | +1.2% | +0.2% |
| relaxed | 1,000 | −0.2% | +0.2% |
| relaxed | 10,000 | −1.2% | −0.5% |

**The floor is ±2% of the mean under `strict`/`group` (±3 µs at p50 on the
1,000-sample-and-up runs) and ±1.2% mean / ±0.5 µs p50 under `relaxed`.**
Nothing smaller is reported as a result below. The 200-row cells carry only
200 samples each and are the widest — read the 1,000/10,000 rows first. Tail
maxima (one 72.9 ms stall in a `strict` read phase, one 36 ms in a `group`
insert) are the resident load showing up; the interleaving spreads them, and
the p50 columns are where the signal is.

## 2. The headline: INSERT with an assertion, against without

Full distributions, all nine configurations. `ins/s` is the phase's
single-connection throughput.

### durability = strict

| rows | relation | ops | ins/s | mean µs | p0 | p25 | p50 | p95 | p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `none` | 200 | 925 | 1070.5 | 909.8 | 1015.3 | 1042.1 | 1165.3 | 1817.7 |
| 200 | `twin` | 200 | 942 | 1052.3 | 947.1 | 1006.8 | 1025.5 | 1097.2 | 1223.8 |
| 200 | `cnt` | 200 | 948 | 1044.8 | 926.5 | 1010.9 | 1038.1 | 1149.2 | 1229.1 |
| 200 | `sum` | 200 | 922 | 1074.7 | 941.2 | 1013.6 | 1036.4 | 1148.1 | 1703.4 |
| 200 | `multi` | 200 | 924 | 1072.5 | 972.9 | 1020.7 | 1046.4 | 1136.6 | 1239.8 |
| 1,000 | `none` | 1000 | 949 | 1044.0 | 378.3 | 1001.9 | 1027.3 | 1135.0 | 1991.9 |
| 1,000 | `twin` | 1000 | 950 | 1042.7 | 379.5 | 1003.9 | 1028.2 | 1129.2 | 1463.0 |
| 1,000 | `cnt` | 1000 | 934 | 1061.1 | 406.1 | 1012.4 | 1038.2 | 1159.8 | 1615.5 |
| 1,000 | `sum` | 1000 | 916 | 1082.3 | 829.0 | 1008.6 | 1034.0 | 1137.8 | 1783.3 |
| 1,000 | `multi` | 1000 | 941 | 1053.5 | 912.4 | 1015.3 | 1037.8 | 1134.8 | 1264.8 |
| 10,000 | `none` | 2000 | 955 | 1038.0 | 770.4 | 1002.8 | 1025.8 | 1115.8 | 1244.2 |
| 10,000 | `twin` | 2000 | 942 | 1053.0 | 792.8 | 1005.4 | 1029.0 | 1126.7 | 1309.8 |
| 10,000 | `cnt` | 2000 | 948 | 1046.1 | 798.2 | 1006.5 | 1032.0 | 1118.9 | 1233.8 |
| 10,000 | `sum` | 2000 | 936 | 1059.5 | 904.5 | 1010.0 | 1033.7 | 1111.8 | 1253.3 |
| 10,000 | `multi` | 2000 | 944 | 1050.2 | 759.8 | 1012.6 | 1037.3 | 1124.6 | 1270.1 |

### durability = group

| rows | relation | ops | ins/s | mean µs | p0 | p25 | p50 | p95 | p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `none` | 200 | 936 | 1058.8 | 943.9 | 1017.6 | 1045.0 | 1180.8 | 1274.9 |
| 200 | `twin` | 200 | 956 | 1037.5 | 917.9 | 1003.3 | 1029.9 | 1110.0 | 1212.5 |
| 200 | `cnt` | 200 | 946 | 1047.7 | 950.2 | 1018.6 | 1042.1 | 1124.8 | 1203.2 |
| 200 | `sum` | 200 | 946 | 1048.0 | 942.1 | 1014.9 | 1037.1 | 1119.7 | 1186.0 |
| 200 | `multi` | 200 | 930 | 1065.5 | 963.4 | 1023.6 | 1047.0 | 1182.7 | 1250.7 |
| 1,000 | `none` | 1000 | 1,001 | 989.1 | 383.5 | 998.9 | 1034.4 | 1132.6 | 1397.2 |
| 1,000 | `twin` | 1000 | 1,010 | 980.8 | 368.9 | 999.7 | 1032.2 | 1127.5 | 1313.2 |
| 1,000 | `cnt` | 1000 | 998 | 992.9 | 386.8 | 1004.2 | 1039.3 | 1172.1 | 1574.1 |
| 1,000 | `sum` | 1000 | 975 | 1016.0 | 378.0 | 1003.3 | 1036.6 | 1159.8 | 1302.1 |
| 1,000 | `multi` | 1000 | 990 | 1000.6 | 393.0 | 1005.5 | 1041.9 | 1160.9 | 1317.8 |
| 10,000 | `none` | 2000 | 924 | 1070.9 | 926.1 | 1019.1 | 1047.4 | 1178.4 | 1528.7 |
| 10,000 | `twin` | 2000 | 910 | 1088.6 | 927.5 | 1019.7 | 1044.7 | 1198.2 | 2357.4 |
| 10,000 | `cnt` | 2000 | 907 | 1092.5 | 926.2 | 1024.5 | 1051.8 | 1203.4 | 2370.8 |
| 10,000 | `sum` | 2000 | 909 | 1087.7 | 890.8 | 1025.4 | 1051.6 | 1183.9 | 2116.5 |
| 10,000 | `multi` | 2000 | 906 | 1093.3 | 924.8 | 1028.0 | 1053.2 | 1188.1 | 1484.9 |

### durability = relaxed

| rows | relation | ops | ins/s | mean µs | p0 | p25 | p50 | p95 | p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `none` | 200 | 8,783 | 106.8 | 64.7 | 105.9 | 107.7 | 124.9 | 138.2 |
| 200 | `twin` | 200 | 8,714 | 108.1 | 63.8 | 103.7 | 107.9 | 130.4 | 162.9 |
| 200 | `cnt` | 200 | 8,400 | 111.9 | 62.5 | 108.3 | 112.7 | 144.4 | 172.1 |
| 200 | `sum` | 200 | 7,823 | 120.8 | 61.1 | 107.2 | 112.2 | 135.6 | 254.2 |
| 200 | `multi` | 200 | 8,163 | 115.8 | 72.4 | 113.5 | 114.8 | 133.4 | 179.8 |
| 1,000 | `none` | 1000 | 8,788 | 106.4 | 56.6 | 100.3 | 109.1 | 133.4 | 176.8 |
| 1,000 | `twin` | 1000 | 8,830 | 106.2 | 57.0 | 103.9 | 109.3 | 129.9 | 152.5 |
| 1,000 | `cnt` | 1000 | 8,612 | 108.8 | 61.0 | 97.4 | 114.0 | 135.2 | 157.0 |
| 1,000 | `sum` | 1000 | 8,236 | 114.1 | 61.0 | 94.5 | 113.6 | 141.5 | 231.8 |
| 1,000 | `multi` | 1000 | 8,090 | 116.0 | 62.7 | 98.7 | 115.9 | 144.3 | 201.9 |
| 10,000 | `none` | 2000 | 8,602 | 108.6 | 56.2 | 96.7 | 107.5 | 136.4 | 229.8 |
| 10,000 | `twin` | 2000 | 8,696 | 107.3 | 55.7 | 88.7 | 107.0 | 131.0 | 204.5 |
| 10,000 | `cnt` | 2000 | 8,440 | 110.9 | 60.6 | 95.9 | 112.5 | 136.6 | 177.7 |
| 10,000 | `sum` | 2000 | 8,243 | 114.1 | 61.4 | 96.8 | 112.6 | 131.6 | 158.9 |
| 10,000 | `multi` | 2000 | 8,051 | 116.7 | 63.1 | 102.7 | 114.8 | 142.7 | 253.3 |

### The deltas, and what clears the floor

Insert-phase deltas against `ast_none` (p50, the robust column; mean beside
it):

| durability | rows | cnt Δp50 | sum Δp50 | multi Δp50 | cnt Δmean | sum Δmean | multi Δmean |
|---|---:|---:|---:|---:|---:|---:|---:|
| strict | 200 | −0.4% | −0.5% | +0.4% | −2.4% | +0.4% | +0.2% |
| strict | 1,000 | **+10.9 µs (+1.1%)** | +6.7 µs (+0.7%) | +10.5 µs (+1.0%) | +1.6% | +3.7% | +0.9% |
| strict | 10,000 | **+6.2 µs (+0.6%)** | +7.9 µs (+0.8%) | +11.5 µs (+1.1%) | +0.8% | +2.1% | +1.2% |
| group | 200 | −0.3% | −0.8% | +0.2% | −1.0% | −1.0% | +0.6% |
| group | 1,000 | **+4.9 µs (+0.5%)** | +2.2 µs (+0.2%) | +7.5 µs (+0.7%) | +0.4% | +2.7% | +1.2% |
| group | 10,000 | **+4.4 µs (+0.4%)** | +4.2 µs (+0.4%) | +5.8 µs (+0.6%) | +2.0% | +1.6% | +2.1% |
| relaxed | 200 | **+5.0 µs (+4.6%)** | +4.5 µs (+4.2%) | +7.1 µs (+6.6%) | +4.8% | +13.1%¹ | +8.4% |
| relaxed | 1,000 | **+4.9 µs (+4.5%)** | +4.5 µs (+4.1%) | +6.8 µs (+6.2%) | +2.3% | +7.2% | +9.0% |
| relaxed | 10,000 | **+5.0 µs (+4.7%)** | +5.1 µs (+4.7%) | +7.3 µs (+6.8%) | +2.1% | +5.1% | +7.5% |

¹ the `relaxed`/200 `sum` mean carries a single 2.3 ms outlier over 200
samples; its p50 (+4.2%) is the readable number.

Three things this table says.

**The p50 delta of one assertion is +4.4 to +5.1 µs at every size where the
sample is large enough to resolve it — under `relaxed`, `group` and (at
+6–11 µs) `strict` alike.** It is a fixed per-statement cost: relation size
moved 50× (200 → 10,000) and the delta did not move. That is the admission
design doing what `docs/feat-assertion.md` §6.2 says it does — an O(1) check
against one group header, one 32-byte Bound Cabin entry append, one
`ASSERT_RESERVE` record ahead of the `HEAP_INSERT` — and none of it scales
with rows incorporated. (§5's declare table is the O(rows) counterpart that
makes the contrast measurable.)

**Under `strict` and `group` the cost is real but irrelevant.** The mean
deltas sit inside the ±2% twin floor — *not a finding* — while the p50s show
the same ~5 µs the `relaxed` runs measure, now against a ~1,030 µs statement:
+0.4–1.1%. A batch of one is a batch: the fsync is 90% of the statement
(§3), and 5 µs under a 930 µs wait is priced at its ratio, not its absence.
Insert throughput under both classes is flat within the floor (`strict`
10,000: 955 → 948 ins/s with one COUNT assertion, −0.7%).

**Under `relaxed` it is visible and honest: −1.9% throughput for one COUNT
assertion, −4.2% for one SUM, −6.4% for two** (10,000 rows: 8,602 → 8,440 /
8,243 / 8,051 ins/s). The second assertion on one relation costs ~+2.3 µs at
p50 on top of the first's ~+5 µs — sub-linear, because the directory lookup
and the statement-end reservation cleanup are shared and only the entry
append and record repeat.

## 3. Where a measured INSERT's time goes

The measured unit is one autocommitted INSERT observed from the client, at
10,000 rows. The decomposition is by subtraction across the run's own phases;
each wait type is named per the documentation rules, including the two this
driver cannot resolve.

| wait type | strict | group | relaxed | how obtained |
|---|---:|---:|---:|---|
| durability/commit (fsync) | ~929 µs (90%) | ~962 µs (90%) | 0 (deferred to the drain cadence) | `insert[none]` mean, this class − `relaxed` |
| client + socket round trip | ≲ 108 µs | ≲ 108 µs | ≲ 108 µs (majority) | bounded by the whole `relaxed` insert; the driver has no engine-free `PING` phase, so client and server shares cannot be split further here — `bench/docs/README.md` puts the Python client at most of a ~130 µs statement |
| write-statement work (parse, compile, chain insert, WAL append) | inside the 108 µs above | inside the 108 µs above | inside the 108 µs above | not separable from the round trip by this driver; the `/proc` CPU meter at a 10 ms tick cannot resolve ~2.7 ms sampling blocks and its per-op numbers are quantization noise, so they are not reported |
| **assertion admission + reservation** | **+6–8 µs p50** | **+4 µs p50** | **+5 µs p50** | the `cnt` column of §2's delta table |
| read wait | none on this path — the Bound Cabin page is memory-resident (nothing evicts) | | | |
| lock / conflict wait | zero by construction — one connection, admission serialized on the relation's home core (`feat-assertion.md` §6.1: core-local, no latches) | | | |

`pk-select`, the read control, is flat in every configuration (`relaxed`
10,000: p50 119.0 `none` vs 119.0/118.6/118.7 across the asserted relations)
— assertions live on the write path and the read path did not move.

## 4. The refusal path: a violation costs a read, not a write

200 inserts past `ast_cap`'s `COUNT(*) <= 8` ceiling, all refused, at 10,000
rows:

| durability | ops | refused | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| strict | 200 | 200 | 106.7 | 61.8 | 79.1 | 107.5 | 133.1 | 189.4 |
| group | 200 | 200 | 134.6 | 62.2 | 84.4 | 108.2 | 164.8 | 1368.6 |
| relaxed | 200 | 200 | 115.9 | 62.5 | 112.3 | 113.3 | 125.9 | 140.8 |

The p50 is ~108–113 µs in **all three classes** — a refused INSERT costs the
same whatever the durability setting, because the admission check runs before
anything durable happens: no `ASSERT_RESERVE`, no `HEAP_INSERT`, no fsync.
Under `strict` that makes a refusal **9.5× cheaper than an admitted insert**
(107.5 vs 1025.8 µs p50). A workload that leans on assertions to bounce
over-limit writes pays read prices for the bounces.

## 5. The other statement shapes, and what each one confirms

`relaxed` at 10,000 rows (the highest-resolution configuration; under
`strict`/`group` every delta below is inside the ±2–2.5% floor):

| phase | relation | ops | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| update-sum | `none` | 2000 | 121.3 | 64.4 | 101.0 | 116.1 | 135.1 | 203.3 |
| update-sum | `twin` | 2000 | 114.2 | 63.9 | 110.0 | 116.2 | 137.7 | 164.4 |
| update-sum | `cnt` | 2000 | 112.9 | 66.6 | 100.4 | 117.1 | 139.9 | 191.2 |
| update-sum | `sum` | 2000 | 115.8 | 69.9 | 102.5 | 121.4 | 141.3 | 175.8 |
| update-sum | `multi` | 2000 | 128.7 | 71.2 | 109.9 | 122.1 | 150.3 | 230.7 |
| update-grp | `none` | 2000 | 116.5 | 64.0 | 103.8 | 116.3 | 142.6 | 229.2 |
| update-grp | `twin` | 2000 | 121.4 | 63.5 | 110.4 | 116.4 | 135.7 | 190.6 |
| update-grp | `cnt` | 2000 | 137.7 | 70.8 | 109.2 | 122.0 | 143.1 | 180.6 |
| update-grp | `sum` | 2000 | 128.9 | 72.1 | 111.6 | 122.2 | 147.6 | 221.9 |
| update-grp | `multi` | 2000 | 124.3 | 73.9 | 113.3 | 124.9 | 149.0 | 215.0 |
| update-note | `none` | 2000 | 112.0 | 63.7 | 95.0 | 116.4 | 141.1 | 192.7 |
| update-note | `twin` | 2000 | 110.0 | 64.4 | 91.3 | 115.3 | 138.7 | 183.0 |
| update-note | `cnt` | 2000 | 113.6 | 65.5 | 98.4 | 117.5 | 143.4 | 184.5 |
| update-note | `sum` | 2000 | 120.6 | 65.4 | 107.0 | 118.0 | 136.9 | 165.0 |
| update-note | `multi` | 2000 | 121.8 | 66.3 | 99.4 | 118.4 | 141.9 | 212.6 |
| pk-select | `none` | 2000 | 119.3 | 67.6 | 107.0 | 119.0 | 138.9 | 207.3 |
| pk-select | `twin` | 2000 | 114.3 | 66.6 | 102.8 | 118.6 | 138.5 | 168.4 |
| pk-select | `cnt` | 2000 | 118.3 | 67.8 | 113.2 | 119.0 | 139.8 | 190.3 |
| pk-select | `sum` | 2000 | 114.5 | 68.4 | 107.5 | 118.6 | 135.9 | 175.3 |
| pk-select | `multi` | 2000 | 115.1 | 68.2 | 106.4 | 118.7 | 136.7 | 179.1 |
| delete | `none` | 2000 | 114.2 | 59.2 | 90.5 | 111.1 | 131.0 | 165.8 |
| delete | `twin` | 2000 | 113.6 | 59.2 | 92.2 | 111.1 | 130.6 | 157.8 |
| delete | `cnt` | 2000 | 120.8 | 64.0 | 94.9 | 115.5 | 138.6 | 186.1 |
| delete | `sum` | 2000 | 111.7 | 64.5 | 94.7 | 115.3 | 137.2 | 179.9 |
| delete | `multi` | 2000 | 113.4 | 68.0 | 101.4 | 117.8 | 139.4 | 181.1 |

Read against `assertion_check.hpp`'s departure/arrival table, each row
confirms a structural claim:

- **`update-grp` (moves a row between groups): +5.7 to +8.6 µs p50** — the
  most expensive shape, as it should be: a departure entry *and* a checked
  arrival, two 32 B appends. Still the same order as an insert's one.
- **`update-sum` under the SUM assertion: +5.3 µs p50; under the COUNT
  assertion: +1.0 µs — at the floor.** A `SET amount` changes SUM but not
  COUNT, and the COUNT relation's write hook correctly does nothing
  ("aggregate invariant → no entry, no delta"). The asymmetry is the
  enforcement discriminating by aggregate, measured.
- **`update-note` (touches no asserted column): +1.1 to +2.0 µs p50 against
  a −1.1 µs twin** — at or within ~2 µs of the floor. The must-stay-free rule
  the index and Cabin hooks obey holds here within measurement resolution.
- **`delete`: +4.2 to +6.7 µs p50 — check-free is not work-free.** AS11's
  dividend is real: no admission check, no refusal path, and no delta *under
  `strict`/`group`* (−2.6% to +1.3%, inside the floor). But `relaxed` shows
  the departure entry §5 of `feat-assertion.md` requires for the coverage
  contract — one 32 B append so the aggregates come down and the ceiling
  does not refuse valid writes forever. The driver's docstring predicted
  "the delta here must be zero even under enforcement"; that expectation was
  written against the check and missed the coverage entry, and the data
  corrects it: DELETE pays the same entry-append price as an arrival, minus
  the check.

## 6. Versus PostgreSQL

**There is no PostgreSQL twin for this workload, and none is silently
missing: no released PostgreSQL parses `CREATE ASSERTION`** (the statement is
in the SQL standard; PostgreSQL rejects it as unimplemented). The nearest
emulation — an `AFTER INSERT` constraint trigger re-running a per-group
aggregate — measures a different mechanism with a different complexity class
(a per-row re-aggregation or a maintained side table, against this engine's
O(1) header check), and calling it a baseline would compare the feature to a
workaround. The task that would add the comparison anyway, honestly labelled:
a `tools/pg_assertion_benchmark.py` importing this driver's schema and phase
list and enforcing the same ceilings with a constraint trigger on the port
15433 scratch cluster, its results marked as pricing PostgreSQL's *available
substitute*, not `CREATE ASSERTION`. It does not exist today, and no number
in this file has a PostgreSQL column.

## 7. Side measurements

**CREATE ASSERTION is O(rows), ~1 µs/row.** The declare phase times the
build — the scan that incorporates every live row into the Bound Cabin
(AST06). One statement each, `relaxed` runs shown; `strict`/`group` are
within a few hundred µs of the same (DDL is unlogged, so no fsync rides it):

| rows | `cnt` µs | `sum` µs | `multi` µs (2 statements, mean) |
|---:|---:|---:|---:|
| 200 | 416 | 340 | 368 |
| 1,000 | 1,082 | 1,021 | 1,064 |
| 10,000 | 9,306 | 9,465 | 11,325 |

The contrast with §2 is the design in two numbers: declaring pays the
relation size **once**, so that every admission afterwards pays a constant.

**The production counters tie out to the workload arithmetic exactly**
(`SHOW ASSERTIONS` after the `group`/10,000 run, AST09's surface):
`cnt_lim checks=3968` is 2,000 insert admissions + ~1,968 group-moving
updates (2,000 draws × 63/64 expected to move); `reserved=7936` is one
arrival per insert + two entries per group move + one departure per delete
(2000 + 3936 + 2000); `sum_lim checks=4966` adds the ~half of `update-sum`
statements whose delta was positive and checkable. `cap_lim violations=200`
matches the 200 refusals. The enforcer counts what §4.2's table says it
does, statement for statement.

## 8. What this run teaches about the engine

1. **Enforced assertions are effectively free at the durability classes a
   financial workload would run.** Under `strict` and `group`, the classes
   with a durability point per transaction, one assertion is +0.4–1.1% at
   p50 on INSERT and unmeasurable in throughput. The write-amplification
   budget AST10 asked to be checked is met with an order of magnitude to
   spare — the added bytes (one 32 B entry, one `ASSERT_RESERVE` record) ride
   a WAL flush that was already happening. The proviso from
   `bench/results-index.md`'s write-side finding applies verbatim: a batch of
   one is a batch, this is a single connection, and under concurrency the
   fsync amortizes across transactions while the per-statement 5 µs does not
   — the concurrent measurement does not exist yet and would be the next
   thing to buy with this driver.
2. **The admission check is a fixed cost, measured as one.** ~5 µs per
   statement for the first assertion, ~2.3 µs for the second, invariant
   across a 50× relation-size sweep and across durability classes. The Bound
   Cabin's directory does what AST04 built it to do: admission reads one
   header, never entries, never the relation.
3. **The refusal path is cheaper than the success path — much cheaper under
   fsync.** ~108 µs p50 at every durability class against 1,026 µs for an
   admitted `strict` insert. Declarative bounds refuse before durability, so
   an over-limit burst costs read-path resources, not write-path ones. No
   other constraint mechanism in this engine (FK's reverse scan included)
   has that property this cleanly.
4. **DELETE's price is the coverage entry, not the check.** AS11 bought the
   absent check; §5's "100% of live rows" still costs a departure append
   (+4–7 µs, `relaxed` only in practice). The driver docstring's zero-delta
   expectation was wrong about enforcement's shape and this file supersedes
   it.
5. **Where the cost will actually be noticed is `relaxed` ingest.** −6.4%
   insert throughput with two assertions on one relation is real money on a
   bulk-load path. The mitigation is already in the design (the build-scoped
   delta batching AST06 reserved as a measured optimization); this is its
   first justifying data point.
