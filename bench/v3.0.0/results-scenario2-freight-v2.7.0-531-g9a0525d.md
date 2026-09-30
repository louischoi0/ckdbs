# Scenario 2 (freight) at `v2.7.0-531-g9a0525d`

**One line.** Eight contended bookers now scale with cores: `cores = 8`
under `group` runs 1,519.0 tps (median of three) against 537.6 tps at
`cores = 1`, 2.83x, where the f6ed10c run had eight cores at half the speed
of one (308.5 vs 589.0). The one-core cells are 8.7 % and 12.7 % below
f6ed10c. **The invariant check fails in every cell (13 to 31 of 400), and
this run isolates the cause**: with the driver's `--isolation
repeatable-read` the same workload verifies 400 of 400 in both probes at
unchanged throughput.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then
`--load-only`, then the run on the same server, as in the f6ed10c BTREE
run. Arguments and cell order are that run's
(`bench/v3.0.0/archive/scenario0-btree-v2.7.0-157-gf6ed10c/run_all.sh` at
`9a0525d`).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; run 1 06:41:28 to 06:43:37 UTC, run 2 06:44:27 to 06:46:33, run 3 06:47:00 to 06:49:09 (per-cell times in section 8); the isolation probe 07:11:15 to 07:11:58 |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`, which over `9a0525d` only deletes `bench/v3.0.0/` and edits `bench/README.md`; `src/`, `include/`, `tools/` and `CMakeLists.txt` are identical |
| Engine commit measured | `9a0525d`, `git describe --tags` = `v2.7.0-531-g9a0525d` ("AY-S11: AY closed") |
| Tree cleanliness | Clean at the start; nothing edited or built during the cells |
| Binary provenance | `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, copy of `build-release/kds_server`, **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, source binary mtime 2026-09-30 06:30:43 UTC (after `9a0525d` at 06:19:05 and `fc2d343` at 06:29:21); every server started from the copy |
| Device | data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/`, `/dev/root`, `ext4` (`df -T`). Not tmpfs. Fresh data file per cell, deleted after |
| Build type | Release (`CMakeCache.txt`), built to completion before the first cell |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket x 4 cores x 2 threads, Linux 7.0.0-1014-azure. **The `cores = 8` cells put eight reactors, eight booker processes and a reporter on those eight CPUs** |
| Server config | `log_level = warn`, `cores` and `durability` per cell, all else default; `placement` and `peer_listeners` are retired and not set. Default `isolation` (read-committed; the driver reports it as `server default`) except the probe in section 6 |
| Ports | 15604 to 15607 (probe: 15604, 15605); never 15432 |
| PostgreSQL | not installed on this host when this file was written and **not measured here**; the PostgreSQL 18.6 floor for this shape was measured afterwards, at `v2.7.0-545-gf2f1ee7`, and is in `results-scenario2-freight-pg18-v2.7.0-545-gf2f1ee7.md` (summary: `results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md`) |

## 2. What was run

The four cells of the f6ed10c matrix, three times each (twelve cells),
plus two probe cells (`rr-s2-c1-g`, `rr-s2-c8-g`, `--isolation
repeatable-read`, one run each). Arguments: `--organizations 300 --ships
30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100
--seed 1 --sync`, with `--txn`, `--contend` and `--manifest` at their
defaults (on) and `--capacity-mode cached`. A run is 3,000 committed
bookings, 375 per booker, contended on 6 hot routes; the eight relations
hold 24,644 to 24,664 rows at the end of a run (24,653 in run 1 of
`s2-c1-g`: 300 organizations, 30 ships, 300 operations, 12 fees, 93
recipes and 4,000 cargos loaded, then 3,000 freights and about 16,900
charges written by the bookings).

Rule 9's 200 / 1K / 10K sweep is **not executed** for this driver: the
sizes above mirror the predecessor and the measured unit (an eight-statement
booking on a pk-addressed voyage and customer) does not scale with the
row count of the ledgers it appends to.

Load and build check per cell: section 8. **No build or test process was
live at any cell.**

## 3. Throughput

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | f6ed10c |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 542.7 tps | 537.6 tps | 537.0 tps | **537.6 tps** | 1.1 % | 589.0 tps |
| `s2-c8-g` | 8 | group | 1,477.0 tps | 1,519.0 tps | 1,542.8 tps | **1,519.0 tps** | 4.5 % | 308.5 tps |
| `s2-c1-s` | 1 | strict | 482.5 tps | 477.7 tps | 498.0 tps | **482.5 tps** | 4.3 % | 553.0 tps |
| `s2-c8-s` | 8 | strict | 864.2 tps | 741.1 tps | 1,052.6 tps | **864.2 tps** | 42.0 % | 294.0 tps |

Every cell committed its 3,000 bookings (`abandoned 1` once, in run 1 of
`s2-c8-s`: one booking gave up after five retries and the driver drew
another).

**Cores now help, and durability now costs.** `cores = 8` over `cores = 1`
is 2.83x under `group` (1,519.0 / 537.6) and 1.79x under `strict` (864.2 /
482.5); `group` over `strict` is 1.11x at one core and 1.76x at eight. The
f6ed10c run had the opposite pair, cores costing -48 % and durability 5 to
7 %. The `strict` `cores = 8` cell has a 42.0 % spread over three runs
(741.1, 864.2, 1,052.6 tps) and its median is the least reliable number in
this file.

## 4. Percentiles

The booking (the eight-statement transaction, client-perceived) and its
constituent phases, run 1 in full. `ops` is the phase's count; `booking`
exceeds 3,000 because a rejected or conflicted attempt opens and closes a
span. Values in microseconds.

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 3,349 ops | 718.3 µs | 11,041.9 µs | 12,869.2 µs | 20,845.8 µs | 31,102.2 µs | 66,453.2 µs | 0 errors |
| `s2-c1-g` | commit | 3,000 ops | 1,326.9 µs | 1,616.5 µs | 1,762.6 µs | 4,103.2 µs | 7,977.5 µs | 33,632.8 µs | 0 errors |
| `s2-c1-g` | freight-insert | 3,000 ops | 64.3 µs | 272.0 µs | 1,321.9 µs | 2,047.0 µs | 4,903.2 µs | 33,119.0 µs | 0 errors |
| `s2-c1-g` | charge-insert | 16,918 ops | 44.5 µs | 176.3 µs | 261.1 µs | 1,854.4 µs | 3,572.7 µs | 22,943.5 µs | 0 errors |
| `s2-c1-g` | operation-update | 3,000 ops | 50.3 µs | 170.7 µs | 210.3 µs | 1,794.8 µs | 3,535.5 µs | 33,350.3 µs | 0 errors |
| `s2-c1-g` | org-update | 3,000 ops | 53.4 µs | 169.6 µs | 206.3 µs | 1,782.8 µs | 3,003.3 µs | 33,366.5 µs | 0 errors |
| `s2-c1-g` | cargo-lookup | 3,349 ops | 99.8 µs | 313.3 µs | 419.0 µs | 1,917.7 µs | 4,436.1 µs | 23,157.4 µs | 0 errors |
| `s2-c1-g` | credit-lookup | 3,349 ops | 81.1 µs | 278.3 µs | 372.8 µs | 1,951.3 µs | 3,782.3 µs | 33,594.8 µs | 0 errors |
| `s2-c1-g` | capacity-read | 3,349 ops | 69.0 µs | 256.5 µs | 379.5 µs | 1,971.9 µs | 4,370.1 µs | 33,359.3 µs | 0 errors |
| `s2-c1-g` | recipe-read | 3,349 ops | 189.7 µs | 372.9 µs | 525.4 µs | 2,172.1 µs | 4,276.9 µs | 20,415.9 µs | 0 errors |
| `s2-c1-g` | manifest-scan | 120 ops | 81.9 µs | 409.0 µs | 688.6 µs | 2,173.1 µs | 2,438.0 µs | 3,730.5 µs | 0 errors |
| `s2-c8-g` | booking | 3,360 ops | 571.9 µs | 3,808.9 µs | 4,234.6 µs | 7,996.1 µs | 15,314.4 µs | 119,362.5 µs | 0 errors |
| `s2-c8-g` | commit | 3,000 ops | 1,149.1 µs | 1,913.6 µs | 2,312.3 µs | 5,760.4 µs | 13,375.0 µs | 61,345.4 µs | 0 errors |
| `s2-c8-g` | freight-insert | 3,000 ops | 60.0 µs | 104.8 µs | 132.1 µs | 315.8 µs | 485.1 µs | 6,112.9 µs | 0 errors |
| `s2-c8-g` | charge-insert | 16,926 ops | 39.0 µs | 96.5 µs | 123.2 µs | 244.1 µs | 389.8 µs | 115,683.2 µs | 0 errors |
| `s2-c8-g` | operation-update | 3,000 ops | 60.8 µs | 103.8 µs | 122.2 µs | 237.1 µs | 364.0 µs | 707.2 µs | 0 errors |
| `s2-c8-g` | org-update | 3,000 ops | 55.7 µs | 102.5 µs | 122.3 µs | 241.6 µs | 392.6 µs | 17,880.8 µs | 0 errors |
| `s2-c8-g` | cargo-lookup | 3,360 ops | 80.2 µs | 126.7 µs | 140.4 µs | 238.7 µs | 346.6 µs | 1,633.8 µs | 0 errors |
| `s2-c8-g` | credit-lookup | 3,360 ops | 67.6 µs | 109.3 µs | 125.2 µs | 229.8 µs | 368.6 µs | 3,212.6 µs | 0 errors |
| `s2-c8-g` | capacity-read | 3,360 ops | 60.0 µs | 104.8 µs | 121.0 µs | 240.6 µs | 426.6 µs | 1,704.4 µs | 0 errors |
| `s2-c8-g` | recipe-read | 3,360 ops | 177.2 µs | 271.3 µs | 298.8 µs | 476.1 µs | 593.8 µs | 6,461.1 µs | 0 errors |
| `s2-c8-g` | manifest-scan | 60 ops | 85.3 µs | 135.8 µs | 398.2 µs | 669.3 µs | 710.9 µs | 710.9 µs | 0 errors |
| `s2-c1-s` | booking | 3,339 ops | 509.7 µs | 11,631.0 µs | 14,325.9 µs | 24,570.0 µs | 32,571.8 µs | 89,082.5 µs | 0 errors |
| `s2-c1-s` | commit | 3,000 ops | 1,163.8 µs | 1,470.9 µs | 1,634.7 µs | 4,597.8 µs | 8,906.4 µs | 21,151.5 µs | 0 errors |
| `s2-c1-s` | freight-insert | 3,000 ops | 62.3 µs | 265.9 µs | 1,443.9 µs | 3,648.3 µs | 6,981.3 µs | 21,296.5 µs | 0 errors |
| `s2-c1-s` | charge-insert | 16,919 ops | 43.1 µs | 201.4 µs | 318.1 µs | 3,061.1 µs | 5,525.1 µs | 69,612.2 µs | 0 errors |
| `s2-c1-s` | operation-update | 3,000 ops | 58.3 µs | 174.5 µs | 276.4 µs | 2,992.0 µs | 5,244.5 µs | 69,611.1 µs | 0 errors |
| `s2-c1-s` | org-update | 3,000 ops | 55.1 µs | 168.1 µs | 255.6 µs | 2,843.4 µs | 5,889.7 µs | 21,315.6 µs | 0 errors |
| `s2-c1-s` | cargo-lookup | 3,339 ops | 79.2 µs | 197.0 µs | 252.8 µs | 2,685.0 µs | 5,229.4 µs | 68,467.1 µs | 0 errors |
| `s2-c1-s` | credit-lookup | 3,339 ops | 64.2 µs | 195.3 µs | 308.8 µs | 2,907.1 µs | 5,540.3 µs | 11,768.9 µs | 0 errors |
| `s2-c1-s` | capacity-read | 3,339 ops | 65.4 µs | 184.1 µs | 310.9 µs | 2,931.8 µs | 5,037.1 µs | 69,797.7 µs | 0 errors |
| `s2-c1-s` | recipe-read | 3,339 ops | 171.9 µs | 333.5 µs | 466.7 µs | 3,189.9 µs | 5,909.5 µs | 21,520.9 µs | 0 errors |
| `s2-c1-s` | manifest-scan | 140 ops | 67.5 µs | 591.1 µs | 1,739.1 µs | 3,741.7 µs | 6,764.8 µs | 11,336.3 µs | 0 errors |
| `s2-c8-s` | booking | 3,321 ops | 498.7 µs | 4,124.2 µs | 5,864.2 µs | 14,072.5 µs | 19,624.9 µs | 94,914.9 µs | 0 errors |
| `s2-c8-s` | commit | 3,000 ops | 1,149.3 µs | 2,145.5 µs | 2,994.1 µs | 8,141.6 µs | 13,750.6 µs | 20,424.2 µs | 0 errors |
| `s2-c8-s` | freight-insert | 3,125 ops | 41.2 µs | 79.9 µs | 94.4 µs | 2,262.2 µs | 6,003.5 µs | 11,630.3 µs | 0 errors |
| `s2-c8-s` | charge-insert | 17,597 ops | 34.2 µs | 63.4 µs | 78.5 µs | 193.6 µs | 2,782.4 µs | 18,061.6 µs | 0 errors |
| `s2-c8-s` | operation-update | 3,125 ops | 44.2 µs | 72.8 µs | 86.3 µs | 233.2 µs | 3,183.2 µs | 14,018.1 µs | 41 errors |
| `s2-c8-s` | org-update | 3,084 ops | 41.9 µs | 70.4 µs | 85.6 µs | 304.6 µs | 4,309.0 µs | 15,262.4 µs | 84 errors |
| `s2-c8-s` | cargo-lookup | 3,446 ops | 72.4 µs | 119.1 µs | 134.0 µs | 264.1 µs | 3,972.8 µs | 10,128.4 µs | 0 errors |
| `s2-c8-s` | credit-lookup | 3,446 ops | 62.7 µs | 99.8 µs | 116.4 µs | 273.8 µs | 3,415.8 µs | 13,828.0 µs | 0 errors |
| `s2-c8-s` | capacity-read | 3,446 ops | 57.6 µs | 93.7 µs | 111.0 µs | 251.3 µs | 3,601.9 µs | 75,849.6 µs | 0 errors |
| `s2-c8-s` | recipe-read | 3,446 ops | 169.2 µs | 224.1 µs | 268.1 µs | 424.1 µs | 3,458.8 µs | 9,272.6 µs | 0 errors |
| `s2-c8-s` | manifest-scan | 80 ops | 87.9 µs | 257.9 µs | 428.9 µs | 2,045.1 µs | 8,744.2 µs | 8,744.2 µs | 0 errors |

Runs 2 and 3, booking, commit and the two appends only (the rest is in the
archive). The `errors` of `s2-c8-g`'s appends in runs 2 and 3 are the btree
re-descent refusal of scenario0's section 6 (run 2: `freights` key 107 page
145 and `charges` key 135 page 147; run 3: `charges` at the same key and
page), retried by the driver. Run 2:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 3,353 ops | 497.2 µs | 11,072.3 µs | 13,013.2 µs | 21,726.2 µs | 26,907.5 µs | 37,890.7 µs | 0 errors |
| `s2-c1-g` | commit | 3,000 ops | 1,233.1 µs | 1,613.2 µs | 1,770.5 µs | 3,999.8 µs | 7,686.6 µs | 22,033.4 µs | 0 errors |
| `s2-c1-g` | freight-insert | 3,000 ops | 61.8 µs | 269.8 µs | 1,337.0 µs | 2,024.5 µs | 5,078.7 µs | 20,557.8 µs | 0 errors |
| `s2-c1-g` | charge-insert | 16,920 ops | 49.6 µs | 178.3 µs | 264.3 µs | 1,868.5 µs | 3,919.2 µs | 20,555.2 µs | 0 errors |
| `s2-c8-g` | booking | 3,342 ops | 508.8 µs | 3,961.8 µs | 4,301.5 µs | 7,983.9 µs | 13,958.6 µs | 84,574.7 µs | 0 errors |
| `s2-c8-g` | commit | 3,000 ops | 1,281.0 µs | 1,924.0 µs | 2,289.1 µs | 5,815.8 µs | 11,754.7 µs | 64,443.1 µs | 0 errors |
| `s2-c8-g` | freight-insert | 3,003 ops | 48.7 µs | 108.2 µs | 137.7 µs | 342.7 µs | 492.7 µs | 80,848.5 µs | 1 error |
| `s2-c8-g` | charge-insert | 16,936 ops | 38.3 µs | 98.4 µs | 127.9 µs | 256.3 µs | 390.0 µs | 4,359.7 µs | 1 error |
| `s2-c1-s` | booking | 3,315 ops | 541.4 µs | 11,793.5 µs | 14,535.2 µs | 25,217.9 µs | 34,369.5 µs | 52,501.9 µs | 0 errors |
| `s2-c1-s` | commit | 3,000 ops | 1,214.4 µs | 1,483.6 µs | 1,669.0 µs | 4,554.6 µs | 7,898.1 µs | 29,140.4 µs | 0 errors |
| `s2-c1-s` | freight-insert | 3,000 ops | 40.1 µs | 275.1 µs | 1,449.0 µs | 3,836.9 µs | 5,979.1 µs | 29,005.4 µs | 0 errors |
| `s2-c1-s` | charge-insert | 16,925 ops | 33.9 µs | 212.4 µs | 342.3 µs | 3,160.3 µs | 5,635.8 µs | 29,414.0 µs | 0 errors |
| `s2-c8-s` | booking | 3,347 ops | 519.0 µs | 3,988.2 µs | 6,085.5 µs | 14,718.8 µs | 21,744.7 µs | 62,573.8 µs | 0 errors |
| `s2-c8-s` | commit | 3,000 ops | 1,121.7 µs | 1,612.6 µs | 2,193.1 µs | 5,525.6 µs | 10,148.9 µs | 21,003.9 µs | 0 errors |
| `s2-c8-s` | freight-insert | 3,050 ops | 40.7 µs | 77.4 µs | 101.2 µs | 2,566.7 µs | 5,248.4 µs | 13,088.4 µs | 0 errors |
| `s2-c8-s` | charge-insert | 17,203 ops | 34.7 µs | 64.0 µs | 84.4 µs | 1,777.9 µs | 3,499.9 µs | 16,130.0 µs | 0 errors |

Run 3:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 3,343 ops | 1,086.9 µs | 11,052.4 µs | 12,892.6 µs | 21,677.0 µs | 29,235.4 µs | 49,255.0 µs | 0 errors |
| `s2-c1-g` | commit | 3,000 ops | 1,217.6 µs | 1,614.8 µs | 1,794.3 µs | 4,165.3 µs | 8,535.3 µs | 17,026.5 µs | 0 errors |
| `s2-c1-g` | freight-insert | 3,000 ops | 56.7 µs | 270.1 µs | 1,326.5 µs | 2,134.4 µs | 5,303.5 µs | 15,394.6 µs | 0 errors |
| `s2-c1-g` | charge-insert | 16,917 ops | 35.5 µs | 172.1 µs | 258.2 µs | 1,915.6 µs | 4,717.0 µs | 19,903.3 µs | 0 errors |
| `s2-c8-g` | booking | 3,338 ops | 505.2 µs | 3,984.7 µs | 4,409.7 µs | 7,628.9 µs | 12,643.8 µs | 69,476.7 µs | 0 errors |
| `s2-c8-g` | commit | 3,000 ops | 1,250.6 µs | 2,119.9 µs | 2,499.3 µs | 5,705.6 µs | 10,653.9 µs | 67,941.1 µs | 0 errors |
| `s2-c8-g` | freight-insert | 3,002 ops | 52.6 µs | 99.9 µs | 123.5 µs | 290.1 µs | 390.1 µs | 2,068.3 µs | 0 errors |
| `s2-c8-g` | charge-insert | 16,929 ops | 34.4 µs | 91.4 µs | 116.2 µs | 220.6 µs | 329.3 µs | 2,416.8 µs | 1 error |
| `s2-c1-s` | booking | 3,335 ops | 553.1 µs | 11,224.1 µs | 14,040.5 µs | 23,872.7 µs | 31,832.4 µs | 44,165.0 µs | 0 errors |
| `s2-c1-s` | commit | 3,000 ops | 1,180.4 µs | 1,425.8 µs | 1,585.4 µs | 4,324.4 µs | 8,934.6 µs | 21,915.0 µs | 0 errors |
| `s2-c1-s` | freight-insert | 3,000 ops | 63.5 µs | 271.1 µs | 1,394.3 µs | 3,682.1 µs | 5,842.4 µs | 21,976.7 µs | 0 errors |
| `s2-c1-s` | charge-insert | 16,915 ops | 49.9 µs | 200.4 µs | 317.9 µs | 2,982.7 µs | 5,187.9 µs | 26,969.5 µs | 0 errors |
| `s2-c8-s` | booking | 3,280 ops | 507.2 µs | 4,123.4 µs | 5,515.2 µs | 11,862.9 µs | 17,400.6 µs | 119,515.0 µs | 0 errors |
| `s2-c8-s` | commit | 3,000 ops | 1,200.3 µs | 1,899.3 µs | 2,415.5 µs | 6,659.7 µs | 11,278.3 µs | 36,141.7 µs | 0 errors |
| `s2-c8-s` | freight-insert | 3,077 ops | 43.4 µs | 75.3 µs | 92.3 µs | 2,879.1 µs | 6,481.1 µs | 114,154.0 µs | 0 errors |
| `s2-c8-s` | charge-insert | 17,369 ops | 33.9 µs | 61.8 µs | 77.2 µs | 196.4 µs | 3,004.8 µs | 36,172.3 µs | 0 errors |

**One core queues; eight cores mostly do not.** The statements do the same
work at both core counts (p0 of `cargo-lookup` 99.8 us at `s2-c1-g`, 80.2
us at `s2-c8-g`; `freight-insert` 64.3 and 60.0 us; `charge-insert` 44.5 and
39.0 us) but their p50 falls going from one core to eight: `booking` 3.04x
(12,869.2 to 4,234.6 us), the four reads 1.76x to 3.14x, `operation-update`
and `org-update` 1.7x, `charge-insert` 2.12x, **`freight-insert` 10.0x**
(1,321.9 to 132.1 us). The difference at equal p0 is the wait behind seven
other sessions on the one reactor, not the statement. `commit` goes the
other way: p50 1,762.6 us at one core, 2,312.3 us at eight. Why
`freight-insert`, the first write of a booking, falls furthest is not
explained by this run.

## 5. Where the time goes

Each phase's mean times its ops over the booking's mean times its ops, run
1. What remains is `BEGIN`, refused attempts, the client and the socket.

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed (BEGIN, refused attempts, client, socket) | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 7.6 % | 7.3 % | 7.3 % | 8.4 % | 7.9 % | 29.6 % | 4.6 % | 4.4 % | 16.0 % | 7.1 % | 13,109 µs |
| `s2-c8-g` | 3.3 % | 3.1 % | 3.0 % | 6.8 % | 3.2 % | 15.9 % | 2.6 % | 2.9 % | 55.4 % | 3.8 % | 4,645 µs |
| `s2-c1-s` | 5.4 % | 6.2 % | 6.3 % | 7.6 % | 8.9 % | 34.2 % | 5.6 % | 5.1 % | 13.7 % | 6.9 % | 14,718 µs |
| `s2-c8-s` | 3.8 % | 3.7 % | 3.7 % | 5.6 % | 5.9 % | 13.4 % | 2.9 % | 3.2 % | 52.3 % | 5.5 % | 6,491 µs |

| Wait | Reading |
|---|---|
| Durability / commit | `commit` is 16.0 % of a booking at `s2-c1-g` and **55.4 %** at `s2-c8-g` (52.3 % at `s2-c8-s`). Its p50 rises from 1,762.6 to 2,312.3 us going from one core to eight under `group` while every other phase's p50 falls by 1.7x to 10x, so at eight cores the commit path, not the statements, bounds the booking. That this is the instance's single WAL stream (AR0 M0) shared by eight cores is the likely reading, not isolated by this run (no server-side breakdown) |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | at one core 29.6 % for `charge-insert` alone (about 5.6 charges per booking) and 7.9 / 4.6 / 4.4 % for the three others; at eight cores 15.9 / 3.2 / 2.6 / 2.9 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.3 to 8.4 % each at one core, 3.0 to 6.8 % at eight; `recipe-read` is the FilterScan and the largest |
| Lock or conflict wait | 0 client-visible conflicts at any `cores = 1` cell and 0 / 3 / 2 at `c8-g`, of which 3 (two in run 2, one in run 3) are the btree re-descent refusal on a `freights` or `charges` insert (scenario0 section 6), which the driver files under its `read` axis, and 2 are held rows; **125 / 50 / 77 at `c8-s`** (`TXN_CONFLICT retryable=1 row id=... was written by transaction ...`, all on `operations` and `organizations` rows), 0.02 to 0.04 retries per booking, all retried by the driver. The server logs also record 5 to 15 `row id=...` refusals per one-core cell that never reached the driver (a statement that parks is re-run after the holder decides), so rows are waited for at one core too. Their time is inside the phase that met them and is not separable |
| Client and socket round trip | the unattributed column, 3.8 to 7.1 % |

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | axes (op / org / read / commit) | verify failures / checks | error replies (all phases) | exit |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | 3,000 bookings | 294 bookings | 55 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 22 / 400 checks | 0 replies | exit 0 |
| `s2-c8-g` | 1 | 3,000 bookings | 300 bookings | 60 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 19 / 400 checks | 0 replies | exit 0 |
| `s2-c1-s` | 1 | 3,000 bookings | 289 bookings | 50 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 30 / 400 checks | 0 replies | exit 0 |
| `s2-c8-s` | 1 | 3,000 bookings | 280 bookings | 41 bookings | 125 conflicts | 41 / 84 / 0 / 0 conflicts | 27 / 400 checks | 125 replies | exit 0 |
| `r2-s2-c1-g` | 2 | 3,000 bookings | 297 bookings | 56 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 27 / 400 checks | 0 replies | exit 0 |
| `r2-s2-c8-g` | 2 | 3,000 bookings | 285 bookings | 57 bookings | 3 conflicts | 1 / 0 / 2 / 0 conflicts | 27 / 400 checks | 3 replies | exit 0 |
| `r2-s2-c1-s` | 2 | 3,000 bookings | 262 bookings | 53 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 30 / 400 checks | 0 replies | exit 0 |
| `r2-s2-c8-s` | 2 | 3,000 bookings | 298 bookings | 49 bookings | 50 conflicts | 22 / 28 / 0 / 0 conflicts | 23 / 400 checks | 50 replies | exit 0 |
| `r3-s2-c1-g` | 3 | 3,000 bookings | 291 bookings | 52 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 31 / 400 checks | 0 replies | exit 0 |
| `r3-s2-c8-g` | 3 | 3,000 bookings | 281 bookings | 57 bookings | 2 conflicts | 0 / 1 / 1 / 0 conflicts | 13 / 400 checks | 2 replies | exit 0 |
| `r3-s2-c1-s` | 3 | 3,000 bookings | 282 bookings | 53 bookings | 0 conflicts | 0 / 0 / 0 / 0 conflicts | 27 / 400 checks | 0 replies | exit 0 |
| `r3-s2-c8-s` | 3 | 3,000 bookings | 239 bookings | 41 bookings | 77 conflicts | 29 / 48 / 0 / 0 conflicts | 25 / 400 checks | 77 replies | exit 0 |

## 6. Correctness: what the invariant failures are

`--verify 100` runs 400 invariant checks per cell (I1 booked capacity
against the sum of freight volumes, I2 within ship capacity, I3 outstanding
against recomputed charges, I4 charge rows against rules). All twelve
default-isolation cells report failures, 13 to 31 of 400, always led by I1
(`booked_cbm` below `SUM(freights.cbm)`: for example `I1 operation 537:
booked_cbm=1836000, SUM(freights.cbm)=1975000` in `s2-c1-g`), **including
at `cores = 1` where no two statements execute at once**.

The driver writes `SET booked_cbm = <literal>` with a value it read earlier
in the same transaction and says in its source that this "is exactly the
lost-update shape". Two bookers that read one voyage before either commits
both write `read + their cbm`, and the second overwrites the first. The
probe cells run the same workload under `--isolation repeatable-read`:

| Cell | isolation | tps | committed | conflicted (retried) | axes (op / org) | verify failures / checks |
|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | repeatable-read | 541.8 tps | 3,000 bookings | 101 conflicts | 51 / 50 conflicts | **0 / 400 checks** |
| `rr-s2-c8-g` | repeatable-read | 1,579.5 tps | 3,000 bookings | 58 conflicts | 35 / 23 conflicts | **0 / 400 checks** |
| `s2-c1-g` (median of 3) | server default | 537.6 tps | 3,000 bookings | 0 conflicts | 0 / 0 conflicts | 22, 27, 31 / 400 checks |
| `s2-c8-g` (median of 3) | server default | 1,519.0 tps | 3,000 bookings | 0, 3, 2 conflicts | 0 / 0 conflicts | 19, 27, 13 / 400 checks |

Percentiles of the probe cells, microseconds:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | errors |
|---|---|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | booking | 3,354 ops | 625.3 µs | 10,566.4 µs | 12,599.6 µs | 22,138.2 µs | 29,724.5 µs | 0 errors |
| `rr-s2-c1-g` | commit | 3,000 ops | 1,251.3 µs | 1,564.5 µs | 1,727.1 µs | 4,250.8 µs | 7,449.3 µs | 0 errors |
| `rr-s2-c1-g` | operation-update | 3,101 ops | 68.1 µs | 168.7 µs | 206.5 µs | 1,749.0 µs | 4,003.1 µs | 51 errors |
| `rr-s2-c1-g` | org-update | 3,050 ops | 65.3 µs | 169.6 µs | 208.8 µs | 1,773.2 µs | 3,640.0 µs | 50 errors |
| `rr-s2-c8-g` | booking | 3,355 ops | 472.0 µs | 3,873.9 µs | 4,179.2 µs | 7,337.8 µs | 12,492.2 µs | 0 errors |
| `rr-s2-c8-g` | commit | 3,000 ops | 1,313.3 µs | 1,990.1 µs | 2,329.6 µs | 5,486.8 µs | 10,377.1 µs | 0 errors |
| `rr-s2-c8-g` | operation-update | 3,058 ops | 50.1 µs | 96.1 µs | 114.4 µs | 206.6 µs | 337.4 µs | 35 errors |
| `rr-s2-c8-g` | org-update | 3,023 ops | 46.8 µs | 95.0 µs | 112.5 µs | 198.2 µs | 312.6 µs | 23 errors |

**Finding.** The invariant failures are the read-committed lost update of a
client-side read-modify-write, not corruption of the engine: under
repeatable-read the engine refuses the second writer of a row
(`TXN_CONFLICT`, 101 at one core, 58 at eight), the driver retries, and the
check passes 400 of 400 in both probes. The price of the correct answer is
inside the noise of this file: 541.8 vs 537.6 tps at one core (+0.8 %, the
run-to-run spread being 1.1 %) and 1,579.5 vs 1,519.0 tps at eight (+4.0 %,
spread 4.5 %). One probe each, so the equality of throughput is a single
draw. `docs/spec/txn.md` owns what `isolation`
guarantees; whether a read-committed default is the wanted default for a
workload of this shape is not a decision this run makes.

## 7. Delta against the previous run of this shape

The comparator is
`bench/v3.0.0/results-scenario2-freight-btree-v2.7.0-157-gf6ed10c.md`
section 3, read at `9a0525d`. **The two runs are 374 commits and many
milestones apart (`v2.7.0-157-gf6ed10c` to `v2.7.0-531-g9a0525d`), so
no difference below is attributable to one change.** The f6ed10c run is one
draw per cell.

| cell | median | f6ed10c | delta |
|---|---|---|---|
| `s2-c1-g` | 537.6 tps | 589.0 tps | -8.7 % |
| `s2-c8-g` | 1,519.0 tps | 308.5 tps | +392.4 % |
| `s2-c1-s` | 482.5 tps | 553.0 tps | -12.7 % |
| `s2-c8-s` | 864.2 tps | 294.0 tps | +193.9 % |

- **`cores = 1`: -8.7 % (group) and -12.7 % (strict)**, outside this run's
  1.1 % and 4.3 % spreads and outside AM-S6's 3.5 to 4.1 % for these cells at
  f6ed10c. The one-core path is slower, by less than in scenario0.
- **`cores = 8`: +392.4 % (group) and +193.9 % (strict).** These are not
  the same configuration: f6ed10c ran `peer_listeners = on` with
  `placement = namespace`, where each booking's relations belonged to one
  owner core and statements were shipped to it. Both are retired; a
  session runs where it is accepted and every core writes. AM-S6 recorded
  those cells as unstable to 80 %, but 5x and 3x are beyond that. This is
  a change of architecture, and it is the largest movement in this file.
- Statement-level: `commit` p50 at `s2-c8-g` is 2,312.3 us against
  f6ed10c's 5,976.1 us; `freight-insert` p50 at `s2-c1-g` is 1,321.9 us
  against 1,235.2 us.

## 8. Noise floor and host stamps

Three runs per cell: the spread column of section 3 is the floor: 1.1 %
(`c1-g`), 4.5 % (`c8-g`), 4.3 % (`c1-s`) and **42.0 % (`c8-s`)**. A
difference below those in the matching cell is not a result. The spread
of `c8-s` is 741.1 (run 2, 50 conflicts), 864.2 (run 1, 125) and 1,052.6 tps
(run 3, 77): throughput does not follow the conflict count, and the spread
is not explained here. The device thermometer,
`load-cargos` p50, is 1.30 to 1.38 ms across all fourteen cells, so no
device stall is indicated (`recovery_checkpoint_us` was not sampled).

| cell | run | precheck UTC | loadavg 1/5/15 | build/ctest processes |
|---|---|---|---|---|
| `s2-c1-g` | 1 | 2026-09-30T06:42:14Z | 1.48 / 1.37 / 1.41 | none |
| `s2-c8-g` | 1 | 2026-09-30T06:43:17Z | 1.85 / 1.51 / 1.46 | none |
| `s2-c1-s` | 1 | 2026-09-30T06:41:50Z | 0.86 / 1.26 / 1.38 | none |
| `s2-c8-s` | 1 | 2026-09-30T06:41:28Z | 0.36 / 1.20 / 1.37 | none |
| `r2-s2-c1-g` | 2 | 2026-09-30T06:45:11Z | 1.27 / 1.39 / 1.42 | none |
| `r2-s2-c8-g` | 2 | 2026-09-30T06:46:13Z | 1.48 / 1.44 / 1.43 | none |
| `r2-s2-c1-s` | 2 | 2026-09-30T06:44:48Z | 1.30 / 1.41 / 1.42 | none |
| `r2-s2-c8-s` | 2 | 2026-09-30T06:44:27Z | 1.11 / 1.38 / 1.42 | none |
| `r3-s2-c1-g` | 3 | 2026-09-30T06:47:43Z | 1.68 / 1.56 / 1.48 | none |
| `r3-s2-c8-g` | 3 | 2026-09-30T06:48:48Z | 1.78 / 1.60 / 1.50 | none |
| `r3-s2-c1-s` | 3 | 2026-09-30T06:47:20Z | 1.72 / 1.56 / 1.48 | none |
| `r3-s2-c8-s` | 3 | 2026-09-30T06:47:00Z | 2.01 / 1.59 / 1.49 | none |

The probe cells: loadavg 0.12 / 0.64 / 0.91 and 0.64 / 0.72 / 0.93, no
build or test process.

## 9. What the run says about the engine

1. **The cross-core design pays here.** Retiring ownership (AT-S5, AT-S9)
   turned the worst scenario in the f6ed10c matrix, eight cores at half of
   one, into the best: 1,519.0 tps at eight cores under `group`. The bound
   moved from a shipped statement to the commit path, which is 55 % of a
   booking at eight cores and the only phase that got slower.
2. **One reactor is a queue.** At one core every statement but `commit` is
   1.7x to 10x slower at p50 than at eight cores for the same p0. Any
   one-core number from this driver is a saturation number: it measures the
   reactor, not the statement.
3. **`strict` conflicts.** At eight cores under `strict` 50 to 125 bookings
   in 3,000 meet a row held by an undecided writer and are refused
   `TXN_CONFLICT`, against 0 to 1 held-row refusals per run under `group`
   (whose other 3 refusals are the btree re-descent, section 5). Row locks held across the
   longer commit wait are the likely mechanism; this run does not show it.
4. **The invariant failures are a property of the read-modify-write under
   the default isolation, and repeatable-read removes them at no visible
   cost on this shape** (section 6). The f6ed10c file left them open and
   undiagnosed; this file has one probe of each core count.
5. **The floor is host CPU at eight cores.** The `cores = 8` cells share
   eight CPUs among eight reactors and nine client processes, so how much of
   the 2.83x is the engine and how much the client's own CPU is not
   separable here.

Archive: `bench/v3.0.0/archive/scenario2-v2.7.0-531-g9a0525d/`: per-cell
JSON, driver stdout, load and schema logs, prechecks, server logs, configs,
run scripts (`run_s02.sh`, `run_s2rr.sh`), the timeline and the binary hash.
