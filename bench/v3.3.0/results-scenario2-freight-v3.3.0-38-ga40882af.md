# Scenario 2 (freight) at `v3.3.0-38-ga40882af`

**One line.** The eight-statement freight booking is not a many-row workload, so the new client decoder does not change its throughput: in three interleaved old/new pairs the one-core `group` cell reads 676 tps with the old client and 680 tps with the new
(1.006x, spreads 5.0 % and 3.6 %) and the one-booker cell 430 against 443 tps (1.032x, spreads 5.3 % and 1.8 %), both inside their spreads. What moved is one statement, the
93-row `recipe-read` scan, which falls 27 % in p50 (180 to 132 µs at one booker) and is 7 % of a booking. Against the morning every cell of the matrix and the sweep is
+1.3 % to +10.3 % (all 20 of 20 comparable cells positive, median +3.7 %), which is larger than the client explains: the old client reads 676 tps in this window against
the morning's 650, so the +4.8 % of `s2-c1-g` is a host difference of the same sign and size, and no cell of this scenario is read as a client effect. **One durable booker is
1.01x (`group`) and 1.04x (`strict`) of PostgreSQL 18.6**, reused from the morning's run (419.5 tps): the commit is 54 % of a KDS booking and its mean is 1,294 µs against PostgreSQL's 1,343 µs.
The booker sweep at `cores = 8` climbs from 410 tps (1 booker) to 3,930 tps (32, `group`), and the invariant check fails under the default isolation and passes 400 of 400 under `repeatable-read`, as in the morning.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then `--load-only`, then the run on the same server, with the morning's arguments and cell order (`run_all.sh` in the archive). Measured by `ck-tester`.
Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario2-freight-v3.3.0-33-g9682b725.md`. PostgreSQL 18.6: that file's twin cells (`tools/pg_scenario2_freight.py`, `synchronous_commit = on`, three runs), **not re-run**: budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; the four core cells x 3 runs 06:53:41 to 06:58:53 UTC; isolation probe and booker sweep `x-b*` 06:58:57 to 07:03:02 UTC; single-booker cells 07:03:06 to 07:05:48 UTC; saturation cells 07:28:09 to 07:29:36 UTC; A/B pairs (twelve cells) 08:18:05 to 08:23:18 UTC |
| Worktree / branch | `kwp-decoder-fast-path` / `kwp-decoder-fast-path` (the main checkout is not used) |
| HEAD | `a40882af`, `git describe --tags` = `v3.3.0-38-ga40882af` ("kwp: decode a row batch in one offset loop and resolve each column's renderer once"); it changes `tools/kwp.py` and `tools/ckdbs_cli.py` and nothing else; the engine source is that of `9682b725` |
| Engine measured | the morning's binary copy, byte for byte: `/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/kds_server` was copied from `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (checked equal before the first cell and after the last), source binary mtime 2026-10-20 03:36:57.38 UTC; nothing was rebuilt |
| Client | this worktree's `tools/` (`kwp.py`, `ckdbs_cli.py` as of `a40882af`) drove every cell of the matrix. The A/B arm `old` ran `a40882af^`'s (`f829964c`) `tools/` from a separate directory, `bench-runs/v3.3.0-38-ga40882af/oldtools/tools/`, put first on `PYTHONPATH`; the tree was not modified |
| Comparator | this engine's own morning run, `v3.3.0-33-g9682b725` (`bench/v3.3.0/results-scenario*-v3.3.0-33-g9682b725.md`): **same binary, old client**, 3 h earlier. PostgreSQL 18.6 is **reused** from that run; it was not re-run |
| Time budget | **Budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.** Every KDS cell of the morning's plan ran with its repeat counts |
| Driver | **unmodified** `tools/scenario2_freight.py`, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`; exit 0 in every cell |
| Unchanged from the morning | Everything else (build type, host, device, server config, quiet-host gate, monitor, PostgreSQL setup) is unchanged from the morning and is stamped once in `summary-scenarios-v3.3.0-38-ga40882af.md`, section Stamp. |

## 2. What was run

| cells | cores | durability | bookers | runs | extra |
|---|---|---|---|---|---|
| `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s2-c8-g` (+ `r2-`, `r3-`) | 8 / 1 / 1 / 8 cores | strict / strict / group / group | 8 bookers | 3 runs each | - |
| `rr-s2-c1-g`, `rr-s2-c8-g` | 1 / 8 cores | group | 8 bookers | 1 run each | `--isolation repeatable-read` |
| `x-b{1,2,4,16}-s2-c8-{g,s}` | 8 cores | group and strict | 1, 2, 4, 16 bookers | 1 run each | `--bookers N` (the booker sweep) |
| `sat-b{16,32}-s2-c8-{g,s}` | 8 cores | group and strict | 16, 32 bookers | 1 run each | the same plus a 1-second whole-host CPU sampler |
| `b1-s2-c1-g`, `b1-s2-c1-s` (+ `r2-`, `r3-`) | 1 core | group / strict | 1 booker | 3 runs each | `--bookers 1`, the PostgreSQL twin's shape |
| `ab-old{1,2,3}-s2-c1-g`, `ab-new{1,2,3}-s2-c1-g`; `ab-old{1,2,3}-b1-s2-c1-g`, `ab-new{1,2,3}-b1-s2-c1-g` | 1 core | group | 8 and 1 bookers | 3 pairs each, interleaved old, new, old, new | old = `a40882af^`'s client from `oldtools/` |
| PostgreSQL | - | `synchronous_commit = on` | 1 booker | not run | the morning's `pg-s2-b1`, `r2-`, `r3-` |

Each cell: a fresh server and data file, `--schema-only`, `--load-only`, then the measured run on the same file. A booking is the driver's eight statements in one transaction (cargo, credit, capacity and recipe reads; freight and charge inserts;
operation and organization updates) and a commit; 3,000 bookings per cell, with `--verify 100` invariant checks (400 checks) after. **The row-set axis is not swept:** the loaded sizes are fixed by the arguments
(300 organizations, 30 ships, 300 operations, 4,000 cargos; 3,000 freights and about 17,000 charges accumulate), the reads are pk descents and one 93-row recipe scan, which do not scale with the booking count, and the writes grow `freights` and
`charges` by 3,000 and 17,000 rows inside every cell, a 17,000-row sweep of the insert path in one run; there is no 200 / 1K / 10K axis and none is claimed. The one row-returning statement, `recipe-read` (93 rows), is the one this change reaches.

Cut: nothing from the morning's KDS plan.

## 3. Throughput

Findings are read by the rule of `summary-scenarios-v3.3.0-38-ga40882af.md` ("What counts as a finding"); a cell whose spread is above 20 % is reported and carries no delta claim. **A delta against the morning is client plus host; section 8 separates them.**

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | morning median | morning spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 core | group | 681.3 tps | 679.4 tps | 684.3 tps | **681.3 tps** | 0.7 % | 649.8 tps | 4.1 % | +4.8 % | **finding** |
| `s2-c8-g` | 8 cores | group | 1,990.6 tps | 1,987.9 tps | 2,047.5 tps | **1,990.6 tps** | 3.0 % | 1,965.0 tps | 0.5 % | +1.3 % | inside the spread |
| `s2-c1-s` | 1 core | strict | 596.0 tps | 593.6 tps | 612.2 tps | **596.0 tps** | 3.1 % | 587.6 tps | 1.7 % | +1.4 % | inside the spread |
| `s2-c8-s` | 8 cores | strict | 1,716.7 tps | 1,696.0 tps | 1,738.2 tps | **1,716.7 tps** | 2.5 % | 1,674.2 tps | 3.3 % | +2.5 % | inside the spread |
| `b1-s2-c1-g` | 1 core | group | 418.9 tps | 436.0 tps | 425.4 tps | **425.4 tps** | 4.0 % | 414.8 tps | 2.3 % | +2.6 % | inside the spread |
| `b1-s2-c1-s` | 1 core | strict | 433.1 tps | 434.3 tps | 443.1 tps | **434.3 tps** | 2.3 % | 417.1 tps | 3.0 % | +4.1 % | **finding** |

| ratio of medians | now | morning |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | 2.92x | 3.02x |
| `cores = 8` over `cores = 1`, `strict` | 2.88x | 2.85x |
| `group` over `strict`, `cores = 1` | 1.14x | 1.11x |
| `group` over `strict`, `cores = 8` | 1.16x | 1.17x |

Every cell committed its 3,000 bookings. All six matrix and single-booker cells are positive (+1.3 % to +4.8 %); two are findings by their spreads (`s2-c1-g`, `b1-s2-c1-s`) and four inside them. The pattern is a slight, uniform gain, and section 8 attributes it to the host, not the client.

## 4. Percentiles

Run 1 of each of the four core cells, every phase of the driver (microseconds; the five percentiles are the driver's). The `ddl` and load phases are in the archive's JSON:

#### `s2-c1-g` run 1 (1 core, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,342 ops | 525.0 µs | 9,256.9 µs | 10,643.3 µs | 14,742.6 µs | 18,708.2 µs | 25,993.2 µs | 0 errors |
| cargo-lookup | 3,342 ops | 72.0 µs | 262.9 µs | 353.4 µs | 1,593.1 µs | 2,041.8 µs | 13,548.1 µs | 0 errors |
| credit-lookup | 3,342 ops | 65.4 µs | 223.2 µs | 322.9 µs | 1,592.2 µs | 2,032.4 µs | 7,601.1 µs | 0 errors |
| capacity-read | 3,342 ops | 64.8 µs | 198.4 µs | 307.4 µs | 1,616.7 µs | 2,320.6 µs | 6,542.4 µs | 0 errors |
| recipe-read | 3,342 ops | 123.8 µs | 259.1 µs | 371.7 µs | 1,679.4 µs | 2,154.9 µs | 7,662.4 µs | 0 errors |
| freight-insert | 3,000 ops | 51.1 µs | 216.1 µs | 1,136.8 µs | 1,575.5 µs | 2,444.8 µs | 13,410.3 µs | 0 errors |
| charge-insert | 16,916 ops | 35.7 µs | 147.3 µs | 209.3 µs | 1,520.0 µs | 1,899.9 µs | 13,528.4 µs | 0 errors |
| operation-update | 3,000 ops | 38.9 µs | 139.0 µs | 180.8 µs | 1,485.7 µs | 2,184.5 µs | 7,105.4 µs | 0 errors |
| org-update | 3,000 ops | 43.9 µs | 139.6 µs | 177.7 µs | 1,474.4 µs | 1,810.7 µs | 8,116.4 µs | 0 errors |
| commit | 3,000 ops | 1,046.8 µs | 1,373.9 µs | 1,472.4 µs | 2,870.4 µs | 4,265.3 µs | 14,992.0 µs | 0 errors |
| manifest-scan | 100 ops | 109.3 µs | 236.1 µs | 344.2 µs | 1,620.3 µs | 1,993.7 µs | 2,268.6 µs | 0 errors |
| voyage-rollup | 100 ops | 98.1 µs | 214.5 µs | 333.7 µs | 1,515.1 µs | 1,588.2 µs | 1,756.4 µs | 0 errors |
| customer-statement | 50 ops | 138.5 µs | 825.6 µs | 1,012.2 µs | 2,565.6 µs | 3,085.4 µs | 3,085.4 µs | 0 errors |

#### `s2-c8-g` run 1 (8 cores, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,346 ops | 458.3 µs | 3,286.8 µs | 3,613.7 µs | 4,739.3 µs | 8,435.1 µs | 21,854.1 µs | 0 errors |
| cargo-lookup | 3,347 ops | 76.8 µs | 124.9 µs | 135.2 µs | 204.1 µs | 292.6 µs | 2,397.6 µs | 0 errors |
| credit-lookup | 3,347 ops | 66.6 µs | 107.9 µs | 120.4 µs | 198.1 µs | 272.0 µs | 3,371.9 µs | 0 errors |
| capacity-read | 3,347 ops | 63.5 µs | 103.7 µs | 116.8 µs | 193.0 µs | 277.4 µs | 2,302.5 µs | 0 errors |
| recipe-read | 3,347 ops | 125.8 µs | 185.2 µs | 207.5 µs | 318.2 µs | 428.0 µs | 2,398.9 µs | 0 errors |
| freight-insert | 3,001 ops | 52.8 µs | 83.0 µs | 93.2 µs | 191.2 µs | 282.5 µs | 1,838.7 µs | 0 errors |
| charge-insert | 16,926 ops | 36.4 µs | 67.1 µs | 78.7 µs | 146.6 µs | 216.4 µs | 2,491.5 µs | 0 errors |
| operation-update | 3,001 ops | 45.5 µs | 77.6 µs | 88.2 µs | 160.5 µs | 221.3 µs | 1,452.3 µs | 0 errors |
| org-update | 3,001 ops | 42.7 µs | 72.6 µs | 84.0 µs | 153.7 µs | 219.6 µs | 633.9 µs | 1 errors |
| commit | 3,000 ops | 1,119.0 µs | 1,813.7 µs | 2,088.5 µs | 3,143.6 µs | 6,737.6 µs | 20,381.2 µs | 0 errors |
| manifest-scan | 40 ops | 95.4 µs | 128.9 µs | 223.9 µs | 431.2 µs | 596.1 µs | 596.1 µs | 0 errors |
| voyage-rollup | 40 ops | 89.3 µs | 108.5 µs | 186.2 µs | 320.1 µs | 397.3 µs | 397.3 µs | 0 errors |
| customer-statement | 20 ops | 105.5 µs | 142.9 µs | 183.1 µs | 2,386.8 µs | 2,509.1 µs | 2,509.1 µs | 0 errors |

#### `s2-c1-s` run 1 (1 core, strict)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,312 ops | 665.5 µs | 9,551.0 µs | 11,944.4 µs | 18,573.1 µs | 24,473.5 µs | 41,790.8 µs | 0 errors |
| cargo-lookup | 3,312 ops | 86.1 µs | 174.2 µs | 216.1 µs | 1,836.1 µs | 2,876.7 µs | 12,049.4 µs | 0 errors |
| credit-lookup | 3,312 ops | 64.2 µs | 147.5 µs | 243.7 µs | 2,423.1 µs | 3,138.7 µs | 14,101.3 µs | 0 errors |
| capacity-read | 3,312 ops | 71.4 µs | 151.9 µs | 245.4 µs | 2,458.2 µs | 3,275.7 µs | 10,769.0 µs | 0 errors |
| recipe-read | 3,312 ops | 139.2 µs | 231.5 µs | 338.0 µs | 2,683.0 µs | 3,768.8 µs | 10,335.3 µs | 0 errors |
| freight-insert | 3,000 ops | 56.7 µs | 229.4 µs | 1,265.7 µs | 2,805.0 µs | 4,100.7 µs | 17,479.3 µs | 0 errors |
| charge-insert | 16,923 ops | 39.7 µs | 165.1 µs | 280.4 µs | 2,614.0 µs | 3,718.1 µs | 19,015.9 µs | 0 errors |
| operation-update | 3,000 ops | 54.4 µs | 149.2 µs | 224.0 µs | 2,329.5 µs | 3,164.8 µs | 14,119.1 µs | 0 errors |
| org-update | 3,000 ops | 49.7 µs | 142.3 µs | 193.4 µs | 2,275.2 µs | 2,956.0 µs | 17,567.1 µs | 0 errors |
| commit | 3,000 ops | 1,050.1 µs | 1,297.1 µs | 1,414.1 µs | 3,486.8 µs | 5,362.8 µs | 17,538.6 µs | 0 errors |
| manifest-scan | 120 ops | 101.8 µs | 353.5 µs | 600.6 µs | 2,891.0 µs | 3,107.8 µs | 4,008.7 µs | 0 errors |
| voyage-rollup | 120 ops | 98.9 µs | 339.7 µs | 1,390.2 µs | 3,310.6 µs | 4,392.7 µs | 4,606.2 µs | 0 errors |
| customer-statement | 60 ops | 322.9 µs | 770.0 µs | 1,624.0 µs | 3,521.7 µs | 10,406.7 µs | 10,406.7 µs | 0 errors |

#### `s2-c8-s` run 1 (8 cores, strict)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,350 ops | 443.2 µs | 3,502.7 µs | 4,098.9 µs | 5,815.6 µs | 9,866.7 µs | 22,344.4 µs | 0 errors |
| cargo-lookup | 3,399 ops | 78.0 µs | 129.3 µs | 140.5 µs | 224.2 µs | 298.8 µs | 1,938.9 µs | 0 errors |
| credit-lookup | 3,399 ops | 67.1 µs | 111.8 µs | 125.6 µs | 231.4 µs | 307.2 µs | 1,307.8 µs | 0 errors |
| capacity-read | 3,399 ops | 63.9 µs | 107.2 µs | 120.5 µs | 221.2 µs | 362.5 µs | 2,686.0 µs | 0 errors |
| recipe-read | 3,399 ops | 122.7 µs | 201.1 µs | 220.3 µs | 379.3 µs | 486.5 µs | 1,420.2 µs | 0 errors |
| freight-insert | 3,049 ops | 55.2 µs | 85.5 µs | 96.7 µs | 253.9 µs | 344.3 µs | 1,578.2 µs | 0 errors |
| charge-insert | 17,183 ops | 37.5 µs | 67.2 µs | 78.0 µs | 146.8 µs | 224.3 µs | 3,092.2 µs | 0 errors |
| operation-update | 3,049 ops | 51.8 µs | 76.3 µs | 85.1 µs | 151.7 µs | 1,351.7 µs | 3,113.5 µs | 26 errors |
| org-update | 3,023 ops | 45.8 µs | 70.8 µs | 80.5 µs | 139.6 µs | 1,235.2 µs | 2,814.2 µs | 23 errors |
| commit | 3,000 ops | 1,059.0 µs | 1,899.0 µs | 2,285.7 µs | 2,944.8 µs | 7,926.7 µs | 12,636.2 µs | 0 errors |
| manifest-scan | 40 ops | 98.9 µs | 137.1 µs | 219.8 µs | 364.8 µs | 567.1 µs | 567.1 µs | 0 errors |
| voyage-rollup | 40 ops | 95.9 µs | 126.0 µs | 186.9 µs | 362.3 µs | 409.3 µs | 409.3 µs | 0 errors |
| customer-statement | 20 ops | 84.4 µs | 130.4 µs | 229.2 µs | 2,454.4 µs | 2,522.8 µs | 2,522.8 µs | 0 errors |


## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN, refused attempts, the client and the socket). Each phase's mean times its ops over the booking's mean times its ops, run 1, now and in the morning:

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` (now) | 7.8 % | 7.5 % | 7.3 % | 7.8 % | 7.6 % | 29.9 % | 4.6 % | 4.5 % | 15.6 % | 7.4 % | 10,455 µs |
| `s2-c1-g` (morning) | 7.5 % | 7.4 % | 7.0 % | 8.3 % | 8.1 % | 29.5 % | 4.5 % | 4.4 % | 16.0 % | 7.3 % | 10,647 µs |
| `s2-c8-g` (now) | 4.1 % | 3.8 % | 3.6 % | 6.2 % | 2.7 % | 12.5 % | 2.5 % | 2.4 % | 57.6 % | 4.7 % | 3,497 µs |
| `s2-c8-g` (morning) | 4.0 % | 3.6 % | 3.5 % | 7.8 % | 2.7 % | 12.2 % | 2.4 % | 2.3 % | 57.2 % | 4.4 % | 3,550 µs |
| `s2-c1-s` (now) | 5.1 % | 6.0 % | 6.0 % | 7.2 % | 8.9 % | 36.1 % | 5.3 % | 4.8 % | 14.1 % | 6.5 % | 11,966 µs |
| `s2-c1-s` (morning) | 5.1 % | 5.8 % | 5.9 % | 7.3 % | 9.8 % | 35.5 % | 5.3 % | 4.8 % | 13.9 % | 6.6 % | 11,983 µs |
| `s2-c8-s` (now) | 3.8 % | 3.5 % | 3.4 % | 5.9 % | 2.6 % | 11.0 % | 2.6 % | 2.4 % | 51.1 % | 13.7 % | 4,064 µs |
| `s2-c8-s` (morning) | 3.7 % | 3.4 % | 3.4 % | 7.5 % | 2.7 % | 10.9 % | 2.6 % | 2.3 % | 50.5 % | 13.1 % | 4,215 µs |
| `b1-s2-c1-g` (now) | 4.7 % | 4.0 % | 3.9 % | 7.0 % | 3.0 % | 13.0 % | 2.8 % | 2.6 % | 54.4 % | 4.7 % | 2,100 µs |
| `b1-s2-c1-g` (morning) | 4.4 % | 3.7 % | 3.6 % | 8.6 % | 2.8 % | 12.0 % | 2.6 % | 2.4 % | 55.6 % | 4.4 % | 2,132 µs |

| Wait | Reading |
|---|---|
| Durability / commit | `commit` is 15.6 % of a booking at `s2-c1-g`, 57.6 % at `s2-c8-g` and 51.1 % at `s2-c8-s`; with one booker it is 54.4 % and its p50 (1,227.5 µs) is the device's sync: PostgreSQL's is 1,278.8 µs. At eight cores the commit, not the statements, bounds the booking |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: about 30 % for `charge-insert` alone (about 5.6 charges per booking) and 4.5 % to 8 % for the three others; eight cores 12.5 / 2.7 / 2.5 / 2.4 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.3 to 7.8 % each at one core, 3.6 to 6.2 % at eight; `recipe-read` is the FilterScan and the only one that moved (8.3 % to 7.8 % at `s2-c1-g`, 7.8 % to 6.2 % at `s2-c8-g`) |
| Lock or conflict wait | zero client-visible conflicts at every `cores = 1` cell, 1 / 3 / 1 at `c8-g`, and **49 / 67 / 39 at `c8-s`** (all on `operations` and `organizations` rows: `TXN_CONFLICT retryable=1`), retried by the driver (morning: 54 / 42 / 39), 2 bookings abandoned in `r2-s2-c8-s` (morning: 0). Their cost sits in the unattributed column: 13.7 % at `s2-c8-s` against 4.7 % at `s2-c8-g` |
| Client and socket round trip | the unattributed column at the cells with no conflicts, 4.7 to 7.4 % |

Outcomes, conflicts and the invariant check, every cell of the matrix:

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | run 1 | 3,000 | 292 | 50 | 0 | 0 | 0 / 0 / 0 / 0 | 28 / 400 |
| `r2-s2-c1-g` | run 2 | 3,000 | 287 | 50 | 0 | 0 | 0 / 0 / 0 / 0 | 30 / 400 |
| `r3-s2-c1-g` | run 3 | 3,000 | 300 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 24 / 400 |
| `s2-c8-g` | run 1 | 3,000 | 295 | 51 | 1 | 0 | 0 / 1 / 0 / 0 | 21 / 400 |
| `r2-s2-c8-g` | run 2 | 3,000 | 292 | 53 | 3 | 0 | 1 / 2 / 0 / 0 | 17 / 400 |
| `r3-s2-c8-g` | run 3 | 3,000 | 284 | 59 | 1 | 0 | 1 / 0 / 0 / 0 | 15 / 400 |
| `s2-c1-s` | run 1 | 3,000 | 259 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 29 / 400 |
| `r2-s2-c1-s` | run 2 | 3,000 | 282 | 49 | 0 | 0 | 0 / 0 / 0 / 0 | 37 / 400 |
| `r3-s2-c1-s` | run 3 | 3,000 | 295 | 51 | 0 | 0 | 0 / 0 / 0 / 0 | 32 / 400 |
| `s2-c8-s` | run 1 | 3,000 | 293 | 57 | 49 | 0 | 26 / 23 / 0 / 0 | 18 / 400 |
| `r2-s2-c8-s` | run 2 | 3,000 | 339 | 50 | 67 | 2 | 23 / 44 / 0 / 0 | 23 / 400 |
| `r3-s2-c8-s` | run 3 | 3,000 | 326 | 60 | 39 | 0 | 20 / 19 / 0 / 0 | 15 / 400 |
| `b1-s2-c1-g` | run 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r2-b1-s2-c1-g` | run 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r3-b1-s2-c1-g` | run 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `b1-s2-c1-s` | run 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r2-b1-s2-c1-s` | run 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r3-b1-s2-c1-s` | run 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |

## 6. Correctness: what the invariant failures are

The `--verify 100` pass checks four invariants (booked volume against the sum of freights, ship capacity, outstanding credit against recomputed charges, charge rows against rules) 400 times while the bookers run. Under the
**default isolation** with more than one booker it fails 15 to 37 of 400 in every cell, `cores = 1` included where no conflict is ever reported; with **one booker it fails 0 of 400 in all six cells**; under **`--isolation repeatable-read`
it passes 400 of 400** at both core counts. The driver writes `SET booked_cbm = <literal>` from a value it read earlier in the same transaction, the lost-update shape its source names: under read committed two bookers that read one voyage
before either commits both write `read + their cbm`, and the second overwrites the first. The failures are that client-side lost update, not engine corruption: under repeatable-read the engine refuses the second writer (`TXN_CONFLICT`), the
driver retries, and every check passes. The decoder has no part in it.

| cell | isolation | tps now | tps morning | verify failures / checks | conflicted |
|---|---|---|---|---|---|
| `s2-c1-g` | server default, median of 3 | 681.3 tps | 649.8 tps | 28/30/24 of 400 | 0/0/0 |
| `rr-s2-c1-g` | repeatable-read, 1 run | 672.0 tps | 651.2 tps | 0 of 400 | 116 |
| `s2-c8-g` | server default, median of 3 | 1,990.6 tps | 1,965.0 tps | 21/17/15 of 400 | 1/3/1 |
| `rr-s2-c8-g` | repeatable-read, 1 run | 2,036.8 tps | 1,932.3 tps | 0 of 400 | 60 |

The repeatable-read probe costs no throughput and turns the failed checks into client-visible conflicts, retried by the driver: a refused write instead of a wrong read.

## 7. The booker sweep at `cores = 8`

Bookers 1, 2, 4, 8, 16 and 32 (the saturation cells), `cores = 8`, one run each except 8 (three). `--bookings 3000` in every cell, so every cell does the same work (equal work, not equal time):

| bookers | durability | now: run 1 | run 2 | run 3 | median | spread | morning median | delta | conflicted (run 1) | verify failures (run 1) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | group | 410.2 tps | n/a | n/a | **410.2 tps** | n/a | 394.3 tps | +4.0 % | 0 | 0 / 400 |
| 1 | strict | 416.5 tps | n/a | n/a | **416.5 tps** | n/a | 401.8 tps | +3.7 % | 0 | 0 / 400 |
| 2 | group | 728.5 tps | n/a | n/a | **728.5 tps** | n/a | 696.7 tps | +4.6 % | 0 | 1 / 400 |
| 2 | strict | 580.2 tps | n/a | n/a | **580.2 tps** | n/a | 564.0 tps | +2.9 % | 0 | 7 / 400 |
| 4 | group | 1,177.1 tps | n/a | n/a | **1,177.1 tps** | n/a | 1,154.9 tps | +1.9 % | 0 | 3 / 400 |
| 4 | strict | 994.0 tps | n/a | n/a | **994.0 tps** | n/a | 964.6 tps | +3.0 % | 32 | 4 / 400 |
| 8 | group | 1,990.6 tps | 1,987.9 tps | 2,047.5 tps | **1,990.6 tps** | 3.0 % | 1,965.0 tps | +1.3 % | 1 | 21 / 400 |
| 8 | strict | 1,716.7 tps | 1,696.0 tps | 1,738.2 tps | **1,716.7 tps** | 2.5 % | 1,674.2 tps | +2.5 % | 49 | 18 / 400 |
| 16 | group | 3,284.4 tps | n/a | n/a | **3,284.4 tps** | n/a | 3,095.1 tps | +6.1 % | 1 | 38 / 400 |
| 16 | strict | 2,507.0 tps | n/a | n/a | **2,507.0 tps** | n/a | 2,417.2 tps | +3.7 % | 148 | 30 / 400 |
| 32 | group | 3,929.6 tps | n/a | n/a | **3,929.6 tps** | n/a | 3,767.7 tps | +4.3 % | 1 | 60 / 400 |
| 32 | strict | 3,387.4 tps | n/a | n/a | **3,387.4 tps** | n/a | 3,071.4 tps | +10.3 % | 342 | 53 / 400 |

The 16-booker point was run twice, as the sweep's `x-b16` and as the saturation cell `sat-b16` (same arguments, plus the sampler):

| cell | now | morning | delta |
|---|---|---|---|
| `sat-b16-s2-c8-g` | 3,219.9 tps | 3,057.5 tps | +5.3 % |
| `sat-b16-s2-c8-s` | 2,546.4 tps | 2,382.1 tps | +6.9 % |

**Throughput climbs with bookers on both durability rungs and the gap between them opens**: `group` 410 tps to 3,930 tps (9.6x for 32x the clients) and `strict` 417 tps to 3,387 tps
(8.1x). A single booker is not slower under `strict`: it waits for its own sync either way. From two bookers on `strict` pays for the staged commit that parks and publishes after its sync, plus
the retried conflicts (342 at 32 bookers, 148 at 16; none up to two bookers). The sweep's deltas against the morning are +1.3 % to +10.3 % on 12 of 12 points, the same sign and size as the matrix's, and single runs carry no spread: they are not read.

What bounds it. The 1-second whole-host sampler around the saturation cells:

| cell | 1-s samples | mean busy | peak busy | mean steal |
|---|---|---|---|---|
| `sat-b16-s2-c8-g` | 19 samples | 5 % | 64 % | 0.0 % |
| `sat-b16-s2-c8-s` | 19 samples | 6 % | 54 % | 0.0 % |
| `sat-b32-s2-c8-g` | 18 samples | 6 % | 55 % | 0.0 % |
| `sat-b32-s2-c8-s` | 19 samples | 6 % | 65 % | 0.0 % |

No limit of the engine was reached in these cells: the host never exceeded 65 % busy in any one-second sample, steal was zero, and each cell lasts under two seconds of measured booking time (3,000 bookings at 3,000+ tps), so a one-second sampler sees two
or three samples of it. The driver is Python threads in one process, so the client (the GIL) is a candidate bound that this run did not rule out. **That is the open item of this scenario**: it has not found the ceiling, and a saturation measurement needs
more bookings (`--bookings` larger) and a second client process (rule 4b).

## 8. The client, isolated: old against new, interleaved

Three pairs per cell on the same host, old, new, old, new, old, new, a fresh server and data file each, the same binary copy. Throughput:

| cell (tps) | old client: pair 1 / 2 / 3 | old median | old spread | new client: pair 1 / 2 / 3 | new median | new spread | new over old | read |
|---|---|---|---|---|---|---|---|---|
| tps | 649 / 676 / 682 | 676 tps | 5.0 % | 693 / 668 / 680 | 680 tps | 3.6 % | 1.006x | inside the spread |

| cell (tps) | old client: pair 1 / 2 / 3 | old median | old spread | new client: pair 1 / 2 / 3 | new median | new spread | new over old | read |
|---|---|---|---|---|---|---|---|---|
| tps | 407 / 430 / 430 | 430 tps | 5.3 % | 440 / 443 / 448 | 443 tps | 1.8 % | 1.032x | inside the spread |

Statement p50 of each cell (every statement of the driver; the three pairs side by side). First the eight-booker `s2-c1-g` cell, then the one-booker cell:

| statement | old: p50 pair 1 / 2 / 3 | old median | new: p50 pair 1 / 2 / 3 | new median | new / old |
|---|---|---|---|---|---|
| booking | 11,090.5 / 10,846.3 / 10,652.7 | 10,846.3 µs | 10,543.5 / 10,773.3 / 10,686.4 | 10,686.4 µs | 0.985x |
| commit | 1,508.5 / 1,489.6 / 1,461.0 | 1,489.6 µs | 1,465.0 / 1,512.9 / 1,496.5 | 1,496.5 µs | 1.005x |
| cargo-lookup | 347.0 / 343.7 / 347.3 | 347.0 µs | 374.1 / 373.5 / 363.3 | 373.5 µs | 1.076x |
| credit-lookup | 310.6 / 316.2 / 313.3 | 313.3 µs | 311.9 / 309.8 / 309.4 | 309.8 µs | 0.989x |
| capacity-read | 301.3 / 304.9 / 298.0 | 301.3 µs | 313.6 / 309.0 / 299.2 | 309.0 µs | 1.026x |
| recipe-read | 432.7 / 440.3 / 440.9 | 440.3 µs | 393.0 / 375.5 / 365.7 | 375.5 µs | 0.853x |
| freight-insert | 1,180.7 / 1,145.1 / 1,126.5 | 1,145.1 µs | 1,110.0 / 1,151.3 / 1,149.6 | 1,149.6 µs | 1.004x |
| charge-insert | 210.2 / 208.9 / 209.8 | 209.8 µs | 207.0 / 208.4 / 210.6 | 208.4 µs | 0.993x |
| operation-update | 180.4 / 179.9 / 179.4 | 179.9 µs | 180.3 / 180.0 / 180.8 | 180.3 µs | 1.002x |
| org-update | 180.7 / 176.0 / 177.4 | 177.4 µs | 179.8 / 178.5 / 179.7 | 179.7 µs | 1.013x |
| manifest-scan | 361.9 / 492.9 / 520.2 | 492.9 µs | 393.3 / 345.5 / 427.8 | 393.3 µs | 0.798x |
| voyage-rollup | 384.0 / 350.3 / 341.1 | 350.3 µs | 363.6 / 399.2 / 351.0 | 363.6 µs | 1.038x |
| customer-statement | 1,153.3 / 1,248.4 / 1,041.2 | 1,153.3 µs | 1,258.2 / 895.9 / 897.4 | 897.4 µs | 0.778x |

| statement | old: p50 pair 1 / 2 / 3 | old median | new: p50 pair 1 / 2 / 3 | new median | new / old |
|---|---|---|---|---|---|
| booking | 2,282.0 / 2,132.2 / 2,118.7 | 2,132.2 µs | 2,082.3 / 2,063.4 / 2,063.3 | 2,063.4 µs | 0.968x |
| commit | 1,191.0 / 1,189.2 / 1,203.3 | 1,191.0 µs | 1,200.0 / 1,197.6 / 1,196.2 | 1,197.6 µs | 1.006x |
| cargo-lookup | 109.1 / 87.2 / 86.3 | 87.2 µs | 86.0 / 85.4 / 85.6 | 85.6 µs | 0.982x |
| credit-lookup | 97.1 / 69.6 / 68.3 | 69.6 µs | 68.9 / 68.4 / 69.0 | 68.9 µs | 0.990x |
| capacity-read | 93.9 / 65.4 / 64.5 | 65.4 µs | 65.0 / 64.5 / 65.1 | 65.0 µs | 0.994x |
| recipe-read | 196.6 / 180.1 / 175.3 | 180.1 µs | 132.3 / 131.4 / 132.2 | 132.2 µs | 0.734x |
| freight-insert | 80.4 / 62.9 / 61.4 | 62.9 µs | 61.6 / 60.9 / 60.8 | 60.9 µs | 0.968x |
| charge-insert | 61.5 / 46.5 / 45.3 | 46.5 µs | 45.9 / 45.4 / 45.1 | 45.4 µs | 0.976x |
| operation-update | 74.5 / 55.9 / 55.3 | 55.9 µs | 56.1 / 55.3 / 55.1 | 55.3 µs | 0.989x |
| org-update | 71.4 / 51.9 / 51.1 | 51.9 µs | 52.4 / 51.3 / 51.4 | 51.4 µs | 0.990x |
| manifest-scan | 188.7 / 161.2 / 163.9 | 163.9 µs | 194.9 / 192.0 / 160.2 | 192.0 µs | 1.171x |
| voyage-rollup | 160.7 / 130.5 / 127.6 | 130.5 µs | 163.8 / 166.6 / 133.6 | 163.8 µs | 1.255x |
| customer-statement | 705.6 / 687.0 / 677.7 | 687.0 µs | 696.1 / 697.5 / 682.4 | 696.1 µs | 1.013x |

**The decoder moves one statement and no throughput.** `recipe-read` falls to 0.73x of its p50 at one booker ; every statement that returns one row or none is within 0.97x to 1.08x, and the three reporter statements (`manifest-scan`, `voyage-rollup`, `customer-statement`, 50 to 120 operations per cell) scatter by up to 25 % between pairs and carry no claim. A booking is 2.1 ms at one booker and `recipe-read` is 7.0 % of it, so a 27 % saving on it is about 1.9 % of the booking, below the 3.6 % to 5.3 % spreads of the
cells: the A/B reads +0.6 % and +3.2 % and both are inside the spread. **The +1 % to +10 % against the morning is therefore not the client**: the old client reads 676 tps on `s2-c1-g` and 430 tps on the one-booker cell
here, against 650 and 415 in the morning. Host, in the opposite direction to scenario 1's scan effect (a gain here, a loss there), and not separated into cause.

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor and answers "is the engine in the right league" and nothing finer. The twin drives **one** booking connection with its manifest reporter interleaved, so the matched KDS cell is `--bookers 1`; the eight-booker cells have no
PostgreSQL counterpart. Its cells are the morning's three runs (04:22 to 04:30 UTC), not re-run:

| engine / cell | run 1 | run 2 | run 3 | median | spread | KDS now / PG |
|---|---|---|---|---|---|---|
| KDS `b1-s2-c1-g` (now) | 418.9 tps | 436.0 tps | 425.4 tps | 425.4 tps | 4.0 % | 1.01x |
| KDS `b1-s2-c1-g` (morning) | 412.7 tps | 422.1 tps | 414.8 tps | 414.8 tps | 2.3 % | 0.99x |
| KDS `b1-s2-c1-s` (now) | 433.1 tps | 434.3 tps | 443.1 tps | 434.3 tps | 2.3 % | 1.04x |
| KDS `b1-s2-c1-s` (morning) | 416.4 tps | 417.1 tps | 428.8 tps | 417.1 tps | 3.0 % | 0.99x |
| PostgreSQL 18.6 (morning's run, reused) | 419.5 tps | 427.1 tps | 414.4 tps | 419.5 tps | 3.0 % | 1.00x |

| statement | KDS `b1-s2-c1-g` p50 | PostgreSQL p50 | KDS/PG p50 | KDS mean | PG mean |
|---|---|---|---|---|---|
| booking | 2,142.2 µs | 2,206.4 µs | 0.97x | 2,099.6 µs | 2,123.2 µs |
| cargo-lookup | 89.3 µs | 78.4 µs | 1.14x | 97.7 µs | 82.2 µs |
| credit-lookup | 75.5 µs | 67.9 µs | 1.11x | 84.5 µs | 72.6 µs |
| capacity-read | 71.5 µs | 65.5 µs | 1.09x | 81.1 µs | 70.6 µs |
| recipe-read | 138.1 µs | 91.0 µs | 1.52x | 147.9 µs | 96.3 µs |
| freight-insert | 63.9 µs | 81.0 µs | 0.79x | 71.3 µs | 87.7 µs |
| charge-insert | 47.6 µs | 54.1 µs | 0.88x | 55.0 µs | 59.8 µs |
| operation-update | 55.7 µs | 72.5 µs | 0.77x | 65.6 µs | 79.3 µs |
| org-update | 51.9 µs | 69.5 µs | 0.75x | 61.8 µs | 75.9 µs |
| commit | 1,227.5 µs | 1,278.8 µs | 0.96x | 1,294.2 µs | 1,343.4 µs |

**KDS and PostgreSQL are the same machine on one durable client**: the booking mean is 2,100 µs against 2,123 µs. The four write statements run at 0.75x to 0.88x of PostgreSQL's p50 (a KDS write is cheaper than a
PostgreSQL write by an eighth to a quarter) and `recipe-read`, a 93-row FilterScan, is 1.52x (KDS slower), from 1.94x: the per-row result cost scenario 1 isolates is what is left of it.
The commit is the same fsync: KDS's p50 is 0.96x of PostgreSQL's.

## 10. Noise floor, contamination and host

The 3-run spreads of section 3 are the noise floor: 0.7 % to 3.1 % at the matrix cells and 2.3 % to 4.0 % at the single-booker cells; the PostgreSQL cells' is 3.0 %. The A/B spreads are 1.8 % to 5.3 %.
The booker sweep's single-run points have none and are read only for shape.

Contamination: one KDS cell was discarded and re-run, `s2-c1-s` (first attempt 06:54:33 to 06:54:52 UTC): the 1-second monitor saw a `claude` process of another session at 52.5 % of a CPU inside its window (one sample in 11). The re-run is the cell reported;
the attempt is in `contaminated/` of the run archive. No other cell of this scenario was flagged (`logs/validity.log`).

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s2-c8-s` | 06:53:44 | 06:54:00 | 1.26 / 1.05 / 0.74 | 0 s | none | 0 of 9 samples | 0 |
| `s2-c1-s` | 06:54:33 | 06:54:52 | 1.14 / 1.06 / 0.76 | 0 s | none | 1 of 11 samples | 0 |
| `s2-c1-g` | 06:54:59 | 06:55:17 | 1.08 / 1.05 / 0.76 | 0 s | none | 0 of 10 samples | 0 |
| `s2-c8-g` | 06:55:24 | 06:55:40 | 1.20 / 1.08 / 0.78 | 0 s | none | 1 of 9 samples | 0 |
| `r2-s2-c8-s` | 06:55:47 | 06:56:03 | 1.06 / 1.06 / 0.78 | 0 s | none | 0 of 10 samples | 0 |
| `r2-s2-c1-s` | 06:56:10 | 06:56:28 | 1.33 / 1.12 / 0.81 | 0 s | none | 0 of 10 samples | 0 |
| `r2-s2-c1-g` | 06:56:35 | 06:56:54 | 1.14 / 1.09 / 0.81 | 0 s | none | 0 of 11 samples | 0 |
| `r2-s2-c8-g` | 06:57:01 | 06:57:16 | 0.98 / 1.06 / 0.81 | 0 s | none | 0 of 9 samples | 0 |
| `r3-s2-c8-s` | 06:57:23 | 06:57:39 | 0.91 / 1.04 / 0.81 | 0 s | none | 0 of 10 samples | 0 |
| `r3-s2-c1-s` | 06:57:46 | 06:58:05 | 0.85 / 1.02 / 0.81 | 0 s | none | 0 of 10 samples | 0 |
| `r3-s2-c1-g` | 06:58:12 | 06:58:30 | 0.94 / 1.03 / 0.82 | 0 s | none | 0 of 11 samples | 0 |
| `r3-s2-c8-g` | 06:58:37 | 06:58:53 | 0.80 / 1.00 / 0.81 | 0 s | none | 0 of 9 samples | 0 |
| `b1-s2-c1-g` | 07:03:09 | 07:03:30 | 1.13 / 1.13 / 0.93 | 0 s | none | 0 of 11 samples | 0 |
| `b1-s2-c1-s` | 07:03:37 | 07:03:57 | 0.99 / 1.10 / 0.92 | 0 s | none | 0 of 11 samples | 0 |
| `r2-b1-s2-c1-g` | 07:04:04 | 07:04:25 | 1.17 / 1.14 / 0.94 | 0 s | none | 0 of 12 samples | 0 |
| `r2-b1-s2-c1-s` | 07:04:32 | 07:04:53 | 0.95 / 1.09 / 0.93 | 0 s | none | 1 of 12 samples | 0 |
| `r3-b1-s2-c1-g` | 07:05:00 | 07:05:20 | 0.95 / 1.08 / 0.93 | 0 s | none | 0 of 12 samples | 0 |
| `r3-b1-s2-c1-s` | 07:05:27 | 07:05:48 | 0.88 / 1.06 / 0.93 | 0 s | none | 0 of 11 samples | 0 |
| `x-b1-s2-c8-g` | 06:59:47 | 07:00:09 | 0.96 / 1.05 / 0.85 | 0 s | none | 0 of 12 samples | 0 |
| `x-b1-s2-c8-s` | 07:00:16 | 07:00:38 | 0.93 / 1.04 / 0.85 | 0 s | none | 0 of 12 samples | 0 |
| `x-b2-s2-c8-g` | 07:00:45 | 07:01:03 | 0.96 / 1.03 / 0.86 | 0 s | none | 0 of 10 samples | 0 |
| `x-b2-s2-c8-s` | 07:01:10 | 07:01:29 | 1.15 / 1.08 / 0.88 | 0 s | none | 0 of 11 samples | 0 |
| `x-b4-s2-c8-g` | 07:01:36 | 07:01:53 | 0.94 / 1.04 / 0.87 | 0 s | none | 1 of 10 samples | 0 |
| `x-b4-s2-c8-s` | 07:02:00 | 07:02:17 | 1.32 / 1.12 / 0.90 | 0 s | none | 0 of 9 samples | 0 |
| `x-b16-s2-c8-g` | 07:02:24 | 07:02:40 | 1.48 / 1.17 / 0.93 | 0 s | none | 0 of 10 samples | 0 |
| `x-b16-s2-c8-s` | 07:02:47 | 07:03:02 | 1.30 / 1.16 / 0.93 | 0 s | none | 1 of 9 samples | 0 |
| `rr-s2-c1-g` | 06:59:00 | 06:59:18 | 1.30 / 1.10 / 0.85 | 0 s | none | 0 of 10 samples | 0 |
| `rr-s2-c8-g` | 06:59:25 | 06:59:40 | 1.18 / 1.09 / 0.86 | 0 s | none | 0 of 9 samples | 0 |
| `sat-b16-s2-c8-g` | 07:28:12 | 07:28:28 | 0.56 / 0.46 / 0.60 | 0 s | none | 0 of 10 samples | 0 |
| `sat-b16-s2-c8-s` | 07:28:35 | 07:28:51 | 0.69 / 0.50 / 0.61 | 0 s | none | 0 of 9 samples | 0 |
| `sat-b32-s2-c8-g` | 07:28:58 | 07:29:13 | 1.10 / 0.62 / 0.65 | 0 s | none | 0 of 9 samples | 0 |
| `sat-b32-s2-c8-s` | 07:29:21 | 07:29:36 | 1.12 / 0.66 / 0.66 | 0 s | none | 0 of 9 samples | 0 |
| `ab-old1-s2-c1-g` | 08:18:08 | 08:18:26 | 1.19 / 1.09 / 0.81 | 0 s | none | 0 of 10 samples | 0 |
| `ab-old1-b1-s2-c1-g` | 08:18:33 | 08:18:55 | 1.19 / 1.10 / 0.82 | 0 s | none | 0 of 12 samples | 0 |
| `ab-new1-s2-c1-g` | 08:19:02 | 08:19:20 | 1.35 / 1.14 / 0.84 | 0 s | none | 1 of 10 samples | 0 |
| `ab-new1-b1-s2-c1-g` | 08:19:27 | 08:19:47 | 1.20 / 1.13 / 0.85 | 0 s | none | 0 of 11 samples | 0 |
| `ab-old2-s2-c1-g` | 08:19:54 | 08:20:12 | 1.05 / 1.10 / 0.84 | 0 s | none | 0 of 10 samples | 0 |
| `ab-old2-b1-s2-c1-g` | 08:20:19 | 08:20:40 | 0.95 / 1.07 / 0.84 | 0 s | none | 0 of 12 samples | 0 |
| `ab-new2-s2-c1-g` | 08:20:47 | 08:21:05 | 1.05 / 1.08 / 0.85 | 0 s | none | 0 of 10 samples | 0 |
| `ab-new2-b1-s2-c1-g` | 08:21:12 | 08:21:33 | 1.02 / 1.07 / 0.86 | 0 s | none | 0 of 12 samples | 0 |
| `ab-old3-s2-c1-g` | 08:21:40 | 08:21:58 | 0.93 / 1.05 / 0.86 | 0 s | none | 0 of 10 samples | 0 |
| `ab-old3-b1-s2-c1-g` | 08:22:05 | 08:22:25 | 0.87 / 1.03 / 0.86 | 0 s | none | 0 of 11 samples | 0 |
| `ab-new3-s2-c1-g` | 08:22:32 | 08:22:51 | 0.84 / 1.01 / 0.86 | 0 s | none | 0 of 11 samples | 0 |
| `ab-new3-b1-s2-c1-g` | 08:22:58 | 08:23:18 | 1.00 / 1.04 / 0.87 | 0 s | none | 0 of 11 samples | 0 |


## 11. What the run says about the engine

1. **A write-and-commit workload does not see the decoder.** Seven of the eight statements of a booking return 0 or 1 row; the one that returns 93 rows is 7 % of a booking and its saving is about 2 % of it. The engine's booking cost is the commit (51 % to 58 % of a booking at one booker or at eight cores)
   and the four writes; both unchanged.
2. **One durable client is PostgreSQL's equal** (0.99x to 1.04x across the matched cells of both runs), because both engines wait on the same fsync (the commit p50 is 1,228 µs against 1,279 µs); the engine's write statements are cheaper (0.75x to 0.88x) and its 93-row read dearer (1.5x), and they cancel in the booking.
3. **A scenario this short cannot be compared across hours at the 5 % level**: every cell here is +1 % to +10 % against the morning with the old client reading the same gain in the same window. The same host moved scenario 1's scan shapes by -14 %, so the sign of host drift depends on the workload.

## 12. Archive

`bench/v3.3.0/archive/scenario2-v3.3.0-38-ga40882af/`: per-cell JSON, driver stdout and logs, server configs; the contaminated attempt is in `bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/contaminated/`. What the four scenarios share is in `bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/`.
The PostgreSQL twin's JSON is in the morning's archive, `bench/v3.3.0/archive/scenario2-v3.3.0-33-g9682b725/json/`. No data file is kept.
