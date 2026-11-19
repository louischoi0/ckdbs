# Scenario 2 (freight) at `v3.4.14`

**One line.** A booking is a commit and eight statements, and on this device the commit is 559 µs where the recorded run's was 1,294 µs, so every cell reads 28 % to 88 % above the recorded `v3.3.0-38-ga40882af` numbers (`cores = 1`, 8 bookers, `group`: 1,183 tps against 681 tps); **an interleaved A/B of the old engine against this one on the same host reads 1.02x and 1.00x, inside the spreads: the engine's booking path did not change and the whole delta is the device and CPU.** At the extreme (20,000 committed bookings, one process of 32 bookers or two processes of 16) the engine tops out at 3,079 to 3,411 tps with the host 88 % to 93 % busy at its peak seconds, and a second client process adds nothing, so what bounds it on this 8-vCPU VM is the machine's CPU shared by server and clients, not the fsync and not one Python client.

Driver: `tools/scenario2_freight.py` from the `35086008` extraction, unmodified, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`, each cell run as `--schema-only`, `--load-only` and then the measured run on the same file. Run by `ck-tester`.
Version measured: **`v3.4.14` = `35086008`** (`git describe --tags 35086008`). **`origin/main` is ahead of the tag** (`12887a14`, `v3.4.14-22-g12887a14`); nothing after the tag is measured.
Comparator: `bench/v3.3.0/results-scenario2-freight-v3.3.0-38-ga40882af.md` (another host, device and CPU, see `results-scenario1-backtest-v3.4.14.md` section 1); section 8's A/B is the same-host reading. PostgreSQL (the floor): **not measured**, no server on this host; the other-host PostgreSQL 18.6 figures are shown in section 9 and are not a league reading on a different fsync.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-19; the four core cells x 3 runs 19:04:41 to 19:07:02 UTC; booker sweep `x-b*` 19:07:02 to 19:08:45; single-booker cells x 3 runs 19:08:45 to 19:10:10; 32-booker cells 19:10:10 to 19:10:33; isolation probe and 16-booker saturation cells 19:24:11 to 19:24:56; the extreme cells (20,000 bookings) 19:26:53 to 19:32:36; engine A/B (6 cells of this scenario, 2 per cell shape) inside 19:14:53 to 19:22:28 |
| Branch / commit | extraction of tag `v3.4.14` = `35086008` under `/home/louis/bench-runs/scenarios-v3.4.14/src/`; **`origin/main` (`12887a14`) is ahead of the tag**; the result files were written in worktree `scenarios-v3-4-14` |
| Tree cleanliness | an extraction has no working tree; drivers unmodified |
| Binary provenance | Release build of the extraction, source binary mtime 2026-11-19 18:56:53.09 UTC; measured a copy, **sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`**. A/B arm: `a40882af` built Release, sha256 `42041f0b8d681dcffeb271495b1626e06b9b4feb39d5561a7615619845d1e2a8` |
| Device | `/dev/nvme0n1p1`, ext4; data files under `/home/louis/bench-runs/scenarios-v3.4.14/data/`; never tmpfs; fresh server and data file per cell |
| Build type | Release |
| Host | 8 logical CPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116, shared VM |
| Server config | `cores` (1 or 8) and `durability` (`group` or `strict`) per cell, `log_level = warn`, `buffer_pool_frames = 65536`, all else default; ports 15604 to 15607 |
| Quiet host | gate before each cell (no build/test process, at most 8 % busy over 3 s): every wait 0 s; 1-second monitor throughout, largest foreign sample 27.7 % of one CPU (this session's agent), no cell discarded |
| 60 s watchdog | status-file and PID poll; no orphan, no stall |

## 2. What was run

| cells | cores | durability | bookers | runs | extra |
|---|---|---|---|---|---|
| `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s2-c8-g` (+ `r2-`, `r3-`) | 8 / 1 / 1 / 8 | strict / strict / group / group | 8 | 3 each | - |
| `rr-s2-c1-g`, `rr-s2-c8-g` | 1 / 8 | group | 8 | 1 each | `--isolation repeatable-read` |
| `x-b{1,2,4,16}-s2-c8-{g,s}`, `sat-b32-s2-c8-{g,s}` | 8 | group and strict | 1, 2, 4, 16, 32 | 1 each | `--bookers N` (the booker sweep); the 32-booker cells run with a 1-second whole-host CPU sampler |
| `sat-b16-s2-c8-{g,s}` | 8 | group and strict | 16 | 1 each | the same plus the sampler |
| `b1-s2-c1-g`, `b1-s2-c1-s` (+ `r2-`, `r3-`) | 1 | group / strict | 1 | 3 each | `--bookers 1`, the PostgreSQL twin's shape |
| `big-b32-c8-{g,s}` | 8 | group and strict | 32 | 1 each | **the extreme:** `--bookings 20000 --organizations 3000 --operations 3000 --cargos 40000 --seconds 120` (a larger relation set so capacity does not run out) with the sampler |
| `big2p-c8-{g,s}` | 8 | group and strict | 2 processes x 16 | 1 each | two driver processes on one server, each 10,000 bookings on its own table set (`--suffix sa` / `sb`), started together after both loads; the aggregate is the sum of their tps |
| `ab-old{1,2,3}-s2-c1-g`, `ab-new{1,2,3}-s2-c1-g`; `ab-old{1,2,3}-b1-s2-c1-s`, `ab-new{1,2,3}-b1-s2-c1-s` | 1 | group (8 bookers), strict (1 booker) | 8, 1 | 3 pairs each, old, new, old, new, old, new | old = the `a40882af` binary, one driver |
| PostgreSQL | - | - | - | not run | no server on this host |

Each cell: a fresh server and data file. A booking is the driver's eight statements in one transaction (cargo, credit, capacity and recipe reads; freight and charge inserts; operation and organization updates) and a commit; 3,000 bookings per cell at the default size, with `--verify 100` invariant checks (400 checks) after. **The row-set axis (rule 9) is not swept, and that is a fact about the workload, not an omission:** loaded sizes are fixed by the arguments (300 organizations, 30 ships, 300 operations, 4,000 cargos; 3,000 freights and about 17,000 charges accumulate), the reads are pk descents and one 93-row recipe scan that do not scale with the booking count, and the writes grow `freights` and `charges` by 3,000 and about 17,000 rows inside every cell; the extreme cells grow them to 20,000 and about 110,000. There is no 200 / 1K / 10K axis and none is claimed.

Cut: the v3.3.0-38 run's old-client A/B (the client is unchanged, see scenario 1 section 2) was replaced by the engine A/B; the repeats of the booker sweep and the `x-b16` / `sat-b16` duplicate were kept as single runs as before (the sweep's 16-booker point appears once as `x-b16` and once as `sat-b16`).

## 3. Throughput

Findings are read by the rule of the v3.3.0-38 summary ("What counts as a finding"); a spread above 20 % carries no claim. **A delta against v3.3.0-38 is engine plus host; section 8 separates them.**

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.3.0-38 median | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 core | group | 1,184.3 tps | 1,173.0 tps | 1,183.3 tps | **1,183.3 tps** | 1.0 % | 681.3 tps | 0.7 % | +73.7 % | **finding** |
| `s2-c8-g` | 8 cores | group | 2,582.5 tps | 2,547.1 tps | 2,548.8 tps | **2,548.8 tps** | 1.4 % | 1,990.6 tps | 3.0 % | +28.0 % | **finding** |
| `s2-c1-s` | 1 core | strict | 1,123.5 tps | 1,132.6 tps | 1,075.2 tps | **1,123.5 tps** | 5.1 % | 596.0 tps | 3.1 % | +88.5 % | **finding** |
| `s2-c8-s` | 8 cores | strict | 2,321.0 tps | 2,369.6 tps | 2,306.8 tps | **2,321.0 tps** | 2.7 % | 1,716.7 tps | 2.5 % | +35.2 % | **finding** |
| `b1-s2-c1-g` | 1 core | group | 633.5 tps | 610.1 tps | 628.7 tps | **628.7 tps** | 3.7 % | 425.4 tps | 4.0 % | +47.8 % | **finding** |
| `b1-s2-c1-s` | 1 core | strict | 625.8 tps | 636.0 tps | 593.8 tps | **625.8 tps** | 6.7 % | 434.3 tps | 2.3 % | +44.1 % | **finding** |

| ratio of medians | now | v3.3.0-38 recorded |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | 2.15x | 2.92x |
| `cores = 8` over `cores = 1`, `strict` | 2.07x | 2.88x |
| `group` over `strict`, `cores = 1` | 1.05x | 1.14x |
| `group` over `strict`, `cores = 8` | 1.10x | 1.16x |

**Every cell is 28 % to 88 % above the recorded number, and the one-core cells gain most.** The gain is the commit: with one booker a booking is 1,386 µs against 2,100 µs, and the commit is 493 µs of it against 1,141 µs (section 5); everything but the commit is 893 µs against 958 µs. The one-booker cells have no queue and the attribution is clean: 1,386 µs against 2,100 µs a booking is 714 µs, of which 648 µs is the commit, and they read +44 % to +48 % in tps. The eight-booker one-core cells (+74 % to +88 %) are more than that: eight bookers queue on one reactor, so the booking's mean (6,002 µs) is mostly waiting for the core and everything on the path, including the sync, compounds; the A/B of section 8 shows the engine is the same, so that gain is the host's. The scale-out ratios fall because at 8 cores the host's CPU, not the sync, becomes the bound (section 7).

## 4. Percentiles

Run 1 of each cell, every phase of the driver (microseconds; the five percentiles are the driver's; booking and commit are the units of section 5). The `ddl` and load phases are included.

#### `s2-c1-g` run 1 (1 core, group, 8 bookers)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 377.6 µs | 440.3 µs | 525.9 µs | 709.7 µs | 709.7 µs | 709.7 µs | 0 errors |
| load-organizations | 300 ops | 299.8 µs | 398.4 µs | 454.7 µs | 634.6 µs | 720.6 µs | 839.2 µs | 0 errors |
| load-ships | 30 ops | 291.1 µs | 417.3 µs | 460.8 µs | 681.7 µs | 731.8 µs | 731.8 µs | 0 errors |
| load-operations | 300 ops | 264.8 µs | 406.1 µs | 456.2 µs | 658.4 µs | 751.8 µs | 918.8 µs | 0 errors |
| load-fees | 12 ops | 315.3 µs | 381.8 µs | 418.7 µs | 551.4 µs | 551.4 µs | 551.4 µs | 0 errors |
| load-recipes | 93 ops | 308.5 µs | 395.7 µs | 442.0 µs | 644.5 µs | 919.3 µs | 919.3 µs | 0 errors |
| load-cargos | 4,000 ops | 279.2 µs | 426.4 µs | 483.1 µs | 676.0 µs | 834.9 µs | 2,897.5 µs | 0 errors |
| booking | 3,339 ops | 427.0 µs | 5,421.8 µs | 6,187.7 µs | 8,008.5 µs | 9,186.6 µs | 23,494.7 µs | 0 errors |
| cargo-lookup | 3,339 ops | 71.3 µs | 224.1 µs | 293.3 µs | 887.5 µs | 1,064.0 µs | 3,644.0 µs | 0 errors |
| credit-lookup | 3,339 ops | 52.9 µs | 192.5 µs | 263.7 µs | 880.4 µs | 1,059.2 µs | 3,701.8 µs | 0 errors |
| capacity-read | 3,339 ops | 56.7 µs | 190.1 µs | 249.6 µs | 906.1 µs | 1,099.3 µs | 3,751.0 µs | 0 errors |
| recipe-read | 3,339 ops | 119.4 µs | 299.0 µs | 377.5 µs | 1,030.7 µs | 1,201.1 µs | 2,463.0 µs | 0 errors |
| freight-insert | 3,000 ops | 41.2 µs | 209.7 µs | 489.8 µs | 895.6 µs | 1,088.7 µs | 3,457.9 µs | 0 errors |
| charge-insert | 16,917 ops | 37.1 µs | 145.4 µs | 201.5 µs | 843.6 µs | 1,020.9 µs | 3,738.6 µs | 0 errors |
| operation-update | 3,000 ops | 50.0 µs | 148.6 µs | 201.9 µs | 859.5 µs | 1,028.6 µs | 2,372.1 µs | 0 errors |
| org-update | 3,000 ops | 41.7 µs | 142.5 µs | 198.2 µs | 862.6 µs | 1,048.1 µs | 2,243.7 µs | 0 errors |
| commit | 3,000 ops | 378.9 µs | 686.9 µs | 806.6 µs | 1,443.3 µs | 1,749.1 µs | 3,679.7 µs | 0 errors |
| manifest-scan | 60 ops | 165.7 µs | 295.3 µs | 619.0 µs | 1,003.8 µs | 1,053.5 µs | 1,053.5 µs | 0 errors |
| voyage-rollup | 60 ops | 151.8 µs | 252.4 µs | 350.2 µs | 950.8 µs | 2,073.7 µs | 2,073.7 µs | 0 errors |
| customer-statement | 30 ops | 219.9 µs | 865.5 µs | 1,304.4 µs | 2,346.7 µs | 2,369.5 µs | 2,369.5 µs | 0 errors |

#### `s2-c8-g` run 1 (8 cores, group, 8 bookers)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 337.5 µs | 423.2 µs | 473.2 µs | 911.5 µs | 911.5 µs | 911.5 µs | 0 errors |
| load-organizations | 300 ops | 345.1 µs | 488.5 µs | 553.4 µs | 711.0 µs | 827.3 µs | 899.9 µs | 0 errors |
| load-ships | 30 ops | 373.9 µs | 485.5 µs | 529.1 µs | 668.8 µs | 699.5 µs | 699.5 µs | 0 errors |
| load-operations | 300 ops | 321.7 µs | 468.0 µs | 544.1 µs | 787.2 µs | 1,219.1 µs | 1,386.5 µs | 0 errors |
| load-fees | 12 ops | 394.8 µs | 425.6 µs | 454.7 µs | 700.2 µs | 700.2 µs | 700.2 µs | 0 errors |
| load-recipes | 93 ops | 381.5 µs | 480.4 µs | 537.6 µs | 695.8 µs | 1,272.0 µs | 1,272.0 µs | 0 errors |
| load-cargos | 4,000 ops | 341.5 µs | 482.7 µs | 543.0 µs | 750.5 µs | 903.1 µs | 2,218.4 µs | 0 errors |
| booking | 3,349 ops | 502.7 µs | 2,399.8 µs | 2,739.4 µs | 3,528.2 µs | 4,570.2 µs | 12,183.1 µs | 0 errors |
| cargo-lookup | 3,349 ops | 63.4 µs | 152.0 µs | 173.8 µs | 305.9 µs | 439.4 µs | 624.8 µs | 0 errors |
| credit-lookup | 3,349 ops | 64.2 µs | 118.3 µs | 146.6 µs | 287.7 µs | 406.2 µs | 2,083.7 µs | 0 errors |
| capacity-read | 3,349 ops | 56.7 µs | 109.5 µs | 138.0 µs | 271.5 µs | 378.3 µs | 2,339.7 µs | 0 errors |
| recipe-read | 3,349 ops | 135.5 µs | 247.1 µs | 275.6 µs | 463.4 µs | 571.9 µs | 6,222.0 µs | 0 errors |
| freight-insert | 3,000 ops | 54.0 µs | 102.3 µs | 127.7 µs | 277.8 µs | 360.6 µs | 3,554.4 µs | 0 errors |
| charge-insert | 16,913 ops | 31.3 µs | 68.0 µs | 85.1 µs | 210.2 µs | 330.0 µs | 4,333.6 µs | 0 errors |
| operation-update | 3,000 ops | 47.9 µs | 87.4 µs | 105.2 µs | 215.7 µs | 341.0 µs | 628.9 µs | 0 errors |
| org-update | 3,000 ops | 38.6 µs | 76.9 µs | 92.5 µs | 211.2 µs | 317.4 µs | 772.2 µs | 0 errors |
| commit | 3,000 ops | 388.6 µs | 681.1 µs | 826.5 µs | 1,279.9 µs | 2,138.5 µs | 6,327.5 µs | 0 errors |
| manifest-scan | 40 ops | 85.2 µs | 172.6 µs | 401.1 µs | 775.4 µs | 871.3 µs | 871.3 µs | 0 errors |
| voyage-rollup | 40 ops | 77.8 µs | 167.0 µs | 413.6 µs | 698.4 µs | 814.1 µs | 814.1 µs | 0 errors |
| customer-statement | 20 ops | 100.9 µs | 153.3 µs | 320.3 µs | 6,174.9 µs | 6,438.2 µs | 6,438.2 µs | 0 errors |

#### `s2-c1-s` run 1 (1 core, strict, 8 bookers)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 526.9 µs | 529.2 µs | 545.7 µs | 715.5 µs | 715.5 µs | 715.5 µs | 0 errors |
| load-organizations | 300 ops | 325.9 µs | 418.2 µs | 487.6 µs | 670.4 µs | 864.1 µs | 1,042.0 µs | 0 errors |
| load-ships | 30 ops | 335.7 µs | 397.2 µs | 450.3 µs | 620.7 µs | 743.7 µs | 743.7 µs | 0 errors |
| load-operations | 300 ops | 293.4 µs | 391.3 µs | 447.2 µs | 605.1 µs | 674.4 µs | 902.3 µs | 0 errors |
| load-fees | 12 ops | 367.1 µs | 397.2 µs | 448.4 µs | 699.6 µs | 699.6 µs | 699.6 µs | 0 errors |
| load-recipes | 93 ops | 290.8 µs | 404.5 µs | 447.8 µs | 674.3 µs | 756.2 µs | 756.2 µs | 0 errors |
| load-cargos | 4,000 ops | 280.4 µs | 402.6 µs | 455.9 µs | 642.3 µs | 764.8 µs | 2,172.7 µs | 0 errors |
| booking | 3,338 ops | 494.3 µs | 5,609.7 µs | 6,522.0 µs | 8,613.6 µs | 10,016.3 µs | 21,919.6 µs | 0 errors |
| cargo-lookup | 3,338 ops | 71.2 µs | 188.4 µs | 240.4 µs | 966.6 µs | 1,396.7 µs | 2,405.8 µs | 0 errors |
| credit-lookup | 3,338 ops | 59.5 µs | 185.4 µs | 252.2 µs | 1,103.8 µs | 1,463.3 µs | 3,360.5 µs | 0 errors |
| capacity-read | 3,338 ops | 58.2 µs | 182.0 µs | 246.3 µs | 1,140.7 µs | 1,523.6 µs | 2,973.8 µs | 0 errors |
| recipe-read | 3,338 ops | 120.6 µs | 302.7 µs | 382.4 µs | 1,298.2 µs | 1,689.6 µs | 3,110.0 µs | 0 errors |
| freight-insert | 3,000 ops | 52.6 µs | 193.2 µs | 559.4 µs | 1,276.6 µs | 1,798.3 µs | 3,837.1 µs | 0 errors |
| charge-insert | 16,917 ops | 37.7 µs | 150.7 µs | 215.0 µs | 1,119.9 µs | 1,513.6 µs | 4,034.1 µs | 0 errors |
| operation-update | 3,000 ops | 43.3 µs | 148.0 µs | 206.1 µs | 1,057.5 µs | 1,497.4 µs | 3,238.9 µs | 0 errors |
| org-update | 3,000 ops | 44.6 µs | 142.1 µs | 208.2 µs | 1,024.7 µs | 1,447.8 µs | 2,969.3 µs | 0 errors |
| commit | 3,000 ops | 372.2 µs | 593.4 µs | 700.4 µs | 1,411.2 µs | 1,933.2 µs | 3,867.6 µs | 0 errors |
| manifest-scan | 60 ops | 130.5 µs | 293.3 µs | 422.5 µs | 1,488.1 µs | 1,785.0 µs | 1,785.0 µs | 0 errors |
| voyage-rollup | 60 ops | 101.8 µs | 308.0 µs | 377.1 µs | 1,336.4 µs | 1,930.9 µs | 1,930.9 µs | 0 errors |
| customer-statement | 30 ops | 254.3 µs | 829.0 µs | 1,509.5 µs | 2,331.3 µs | 2,554.3 µs | 2,554.3 µs | 0 errors |

#### `s2-c8-s` run 1 (8 cores, strict, 8 bookers)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 364.0 µs | 403.3 µs | 467.5 µs | 699.9 µs | 699.9 µs | 699.9 µs | 0 errors |
| load-organizations | 300 ops | 329.4 µs | 450.6 µs | 511.6 µs | 741.6 µs | 880.1 µs | 1,286.3 µs | 0 errors |
| load-ships | 30 ops | 344.1 µs | 482.4 µs | 522.3 µs | 759.1 µs | 2,128.2 µs | 2,128.2 µs | 0 errors |
| load-operations | 300 ops | 349.8 µs | 466.8 µs | 525.7 µs | 699.3 µs | 920.9 µs | 4,481.8 µs | 0 errors |
| load-fees | 12 ops | 416.1 µs | 482.9 µs | 500.0 µs | 629.7 µs | 629.7 µs | 629.7 µs | 0 errors |
| load-recipes | 93 ops | 417.6 µs | 493.0 µs | 556.1 µs | 748.6 µs | 835.3 µs | 835.3 µs | 0 errors |
| load-cargos | 4,000 ops | 317.0 µs | 465.6 µs | 522.8 µs | 711.7 µs | 885.0 µs | 6,703.1 µs | 0 errors |
| booking | 3,323 ops | 488.6 µs | 2,603.0 µs | 3,027.6 µs | 4,068.3 µs | 6,052.1 µs | 38,258.4 µs | 0 errors |
| cargo-lookup | 3,360 ops | 73.2 µs | 163.3 µs | 186.5 µs | 320.4 µs | 422.1 µs | 4,268.3 µs | 0 errors |
| credit-lookup | 3,360 ops | 61.6 µs | 122.0 µs | 150.2 µs | 287.0 µs | 413.0 µs | 4,614.3 µs | 0 errors |
| capacity-read | 3,360 ops | 58.5 µs | 111.2 µs | 141.3 µs | 290.9 µs | 445.2 µs | 4,688.3 µs | 0 errors |
| recipe-read | 3,360 ops | 121.0 µs | 247.4 µs | 280.1 µs | 502.0 µs | 618.3 µs | 5,216.6 µs | 0 errors |
| freight-insert | 3,037 ops | 58.7 µs | 106.4 µs | 132.2 µs | 319.4 µs | 429.5 µs | 4,282.9 µs | 0 errors |
| charge-insert | 17,130 ops | 31.9 µs | 69.8 µs | 89.4 µs | 198.6 µs | 324.1 µs | 4,997.1 µs | 0 errors |
| operation-update | 3,037 ops | 49.1 µs | 88.0 µs | 105.2 µs | 202.8 µs | 392.2 µs | 4,418.6 µs | 18 errors |
| org-update | 3,019 ops | 40.6 µs | 75.4 µs | 93.5 µs | 198.2 µs | 349.8 µs | 1,383.6 µs | 19 errors |
| commit | 3,000 ops | 405.3 µs | 705.0 µs | 835.6 µs | 1,232.3 µs | 1,476.7 µs | 4,310.2 µs | 0 errors |
| manifest-scan | 40 ops | 82.9 µs | 104.4 µs | 333.8 µs | 640.3 µs | 728.4 µs | 728.4 µs | 0 errors |
| voyage-rollup | 40 ops | 69.0 µs | 118.6 µs | 248.2 µs | 616.0 µs | 806.4 µs | 806.4 µs | 0 errors |
| customer-statement | 20 ops | 100.4 µs | 130.8 µs | 279.2 µs | 5,064.5 µs | 5,184.0 µs | 5,184.0 µs | 0 errors |

#### `b1-s2-c1-g` run 1 (1 core, group, 1 booker)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 364.7 µs | 395.9 µs | 438.8 µs | 638.9 µs | 638.9 µs | 638.9 µs | 0 errors |
| load-organizations | 300 ops | 316.7 µs | 438.7 µs | 488.3 µs | 677.6 µs | 724.8 µs | 911.2 µs | 0 errors |
| load-ships | 30 ops | 378.2 µs | 451.0 µs | 503.6 µs | 688.3 µs | 1,035.4 µs | 1,035.4 µs | 0 errors |
| load-operations | 300 ops | 286.0 µs | 402.1 µs | 454.9 µs | 625.1 µs | 686.2 µs | 753.3 µs | 0 errors |
| load-fees | 12 ops | 364.5 µs | 378.8 µs | 424.1 µs | 544.7 µs | 544.7 µs | 544.7 µs | 0 errors |
| load-recipes | 93 ops | 350.8 µs | 432.6 µs | 467.6 µs | 631.1 µs | 666.6 µs | 666.6 µs | 0 errors |
| load-cargos | 4,000 ops | 283.3 µs | 413.2 µs | 464.0 µs | 640.1 µs | 745.4 µs | 1,830.3 µs | 0 errors |
| booking | 3,402 ops | 379.7 µs | 1,304.4 µs | 1,427.1 µs | 1,852.1 µs | 2,103.5 µs | 15,885.9 µs | 0 errors |
| cargo-lookup | 3,402 ops | 64.6 µs | 86.8 µs | 95.9 µs | 169.0 µs | 205.9 µs | 284.7 µs | 0 errors |
| credit-lookup | 3,402 ops | 57.3 µs | 63.2 µs | 66.2 µs | 125.5 µs | 181.8 µs | 1,595.9 µs | 0 errors |
| capacity-read | 3,402 ops | 56.0 µs | 60.2 µs | 62.1 µs | 94.4 µs | 125.3 µs | 1,686.2 µs | 0 errors |
| recipe-read | 3,402 ops | 113.7 µs | 132.1 µs | 138.2 µs | 197.4 µs | 228.2 µs | 1,389.2 µs | 0 errors |
| freight-insert | 3,000 ops | 49.9 µs | 64.8 µs | 69.0 µs | 160.7 µs | 192.7 µs | 1,527.1 µs | 0 errors |
| charge-insert | 16,907 ops | 37.6 µs | 41.1 µs | 42.4 µs | 57.6 µs | 82.0 µs | 1,683.4 µs | 0 errors |
| operation-update | 3,000 ops | 46.2 µs | 52.1 µs | 54.7 µs | 75.6 µs | 89.1 µs | 1,686.4 µs | 0 errors |
| org-update | 3,000 ops | 42.5 µs | 45.3 µs | 46.4 µs | 62.0 µs | 77.9 µs | 1,618.3 µs | 0 errors |
| commit | 3,000 ops | 293.6 µs | 482.4 µs | 543.6 µs | 751.0 µs | 888.1 µs | 3,175.0 µs | 0 errors |
| manifest-scan | 100 ops | 60.9 µs | 101.3 µs | 135.6 µs | 593.9 µs | 762.6 µs | 1,044.6 µs | 0 errors |
| voyage-rollup | 100 ops | 52.2 µs | 86.4 µs | 116.4 µs | 620.3 µs | 752.6 µs | 864.4 µs | 0 errors |
| customer-statement | 50 ops | 59.6 µs | 465.0 µs | 940.9 µs | 1,733.0 µs | 2,489.3 µs | 2,489.3 µs | 0 errors |

#### `b1-s2-c1-s` run 1 (1 core, strict, 1 booker)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 ops | 469.7 µs | 483.4 µs | 573.5 µs | 684.5 µs | 684.5 µs | 684.5 µs | 0 errors |
| load-organizations | 300 ops | 317.4 µs | 413.9 µs | 479.2 µs | 655.3 µs | 744.4 µs | 1,860.9 µs | 0 errors |
| load-ships | 30 ops | 376.8 µs | 440.3 µs | 493.0 µs | 635.9 µs | 636.7 µs | 636.7 µs | 0 errors |
| load-operations | 300 ops | 345.6 µs | 434.2 µs | 483.4 µs | 669.0 µs | 811.4 µs | 2,318.2 µs | 0 errors |
| load-fees | 12 ops | 412.3 µs | 448.0 µs | 520.8 µs | 673.5 µs | 673.5 µs | 673.5 µs | 0 errors |
| load-recipes | 93 ops | 353.7 µs | 424.7 µs | 483.0 µs | 645.2 µs | 772.0 µs | 772.0 µs | 0 errors |
| load-cargos | 4,000 ops | 285.4 µs | 418.8 µs | 471.5 µs | 643.2 µs | 752.2 µs | 2,153.1 µs | 0 errors |
| booking | 3,402 ops | 379.2 µs | 1,311.5 µs | 1,440.1 µs | 1,903.9 µs | 2,182.5 µs | 9,712.8 µs | 0 errors |
| cargo-lookup | 3,402 ops | 65.1 µs | 87.3 µs | 95.6 µs | 168.0 µs | 202.6 µs | 1,261.1 µs | 0 errors |
| credit-lookup | 3,402 ops | 57.2 µs | 63.0 µs | 66.3 µs | 128.9 µs | 175.0 µs | 1,591.0 µs | 0 errors |
| capacity-read | 3,402 ops | 56.0 µs | 60.3 µs | 62.4 µs | 105.0 µs | 144.1 µs | 1,674.1 µs | 0 errors |
| recipe-read | 3,402 ops | 114.8 µs | 133.4 µs | 139.9 µs | 211.4 µs | 242.4 µs | 1,558.2 µs | 0 errors |
| freight-insert | 3,000 ops | 50.6 µs | 65.5 µs | 70.3 µs | 161.8 µs | 198.3 µs | 2,233.8 µs | 0 errors |
| charge-insert | 16,907 ops | 36.6 µs | 41.3 µs | 42.7 µs | 59.6 µs | 99.8 µs | 1,501.7 µs | 0 errors |
| operation-update | 3,000 ops | 46.5 µs | 51.6 µs | 54.5 µs | 77.6 µs | 104.4 µs | 1,844.8 µs | 0 errors |
| org-update | 3,000 ops | 42.2 µs | 45.2 µs | 46.3 µs | 63.1 µs | 108.7 µs | 1,551.4 µs | 0 errors |
| commit | 3,000 ops | 292.6 µs | 484.3 µs | 544.3 µs | 739.6 µs | 901.8 µs | 2,776.1 µs | 0 errors |
| manifest-scan | 100 ops | 61.1 µs | 107.7 µs | 165.9 µs | 565.6 µs | 820.3 µs | 828.5 µs | 0 errors |
| voyage-rollup | 100 ops | 54.1 µs | 90.1 µs | 142.6 µs | 679.6 µs | 706.6 µs | 817.7 µs | 0 errors |
| customer-statement | 50 ops | 64.4 µs | 437.8 µs | 895.3 µs | 1,808.6 µs | 2,148.4 µs | 2,148.4 µs | 0 errors |

## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (`BEGIN`, refused attempts, the client and the socket). Each phase's mean times its ops over the booking's mean times its ops, run 1, now and recorded:

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` (now) | 7.4 % | 7.0 % | 6.8 % | 8.9 % | 7.2 % | 30.1 % | 5.2 % | 5.2 % | 13.5 % | 8.7 % | 6,002 µs |
| `s2-c1-g` (v3.3.0-38) | 7.8 % | 7.5 % | 7.3 % | 7.8 % | 7.6 % | 29.9 % | 4.6 % | 4.5 % | 15.6 % | 7.4 % | 10,455 µs |
| `s2-c8-g` (now) | 7.1 % | 6.2 % | 5.8 % | 11.2 % | 5.0 % | 20.1 % | 4.1 % | 3.7 % | 29.2 % | 7.6 % | 2,654 µs |
| `s2-c8-g` (v3.3.0-38) | 4.1 % | 3.8 % | 3.6 % | 6.2 % | 2.7 % | 12.5 % | 2.5 % | 2.4 % | 57.6 % | 4.7 % | 3,497 µs |
| `s2-c1-s` (now) | 6.1 % | 6.9 % | 6.9 % | 9.2 % | 8.1 % | 32.4 % | 5.4 % | 5.4 % | 11.5 % | 8.0 % | 6,331 µs |
| `s2-c1-s` (v3.3.0-38) | 5.1 % | 6.0 % | 6.0 % | 7.2 % | 8.9 % | 36.1 % | 5.3 % | 4.8 % | 14.1 % | 6.5 % | 11,966 µs |
| `s2-c8-s` (now) | 6.9 % | 5.7 % | 5.5 % | 10.4 % | 4.9 % | 18.6 % | 3.8 % | 3.4 % | 26.2 % | 14.4 % | 2,984 µs |
| `s2-c8-s` (v3.3.0-38) | 3.8 % | 3.5 % | 3.4 % | 5.9 % | 2.6 % | 11.0 % | 2.6 % | 2.4 % | 51.1 % | 13.7 % | 4,064 µs |
| `b1-s2-c1-g` (now) | 7.7 % | 5.5 % | 4.8 % | 10.4 % | 5.1 % | 16.5 % | 3.7 % | 3.2 % | 35.6 % | 7.5 % | 1,386 µs |
| `b1-s2-c1-g` (v3.3.0-38) | 4.7 % | 4.0 % | 3.9 % | 7.0 % | 3.0 % | 13.0 % | 2.8 % | 2.6 % | 54.4 % | 4.7 % | 2,100 µs |
| `b1-s2-c1-s` (now) | 7.6 % | 5.4 % | 4.9 % | 10.6 % | 5.2 % | 16.5 % | 3.7 % | 3.2 % | 35.3 % | 7.6 % | 1,404 µs |
| `b1-s2-c1-s` (v3.3.0-38) | 4.6 % | 3.8 % | 3.7 % | 6.9 % | 3.0 % | 12.7 % | 2.7 % | 2.5 % | 55.5 % | 4.5 % | 2,031 µs |

The booking split into the commit and the rest, mean per booking (the commit phase's mean times its ops, over the bookings):

| cell | booking mean | commit mean | everything but the commit |
|---|---|---|---|
| `b1-s2-c1-g` (now) | 1,386 µs | 493 µs per booking | 893 µs |
| `b1-s2-c1-g` (v3.3.0-38) | 2,100 µs | 1,141 µs per booking | 958 µs |
| `b1-s2-c1-s` (now) | 1,404 µs | 496 µs per booking | 908 µs |
| `b1-s2-c1-s` (v3.3.0-38) | 2,031 µs | 1,128 µs per booking | 903 µs |
| `s2-c1-g` (now) | 6,002 µs | 813 µs per booking | 5,189 µs |
| `s2-c1-g` (v3.3.0-38) | 10,455 µs | 1,633 µs per booking | 8,822 µs |
| `s2-c8-g` (now) | 2,654 µs | 776 µs per booking | 1,878 µs |
| `s2-c8-g` (v3.3.0-38) | 3,497 µs | 2,013 µs per booking | 1,484 µs |

| Wait | Reading |
|---|---|
| Durability / commit | 35.6 % of a one-booker booking (493 µs of 1,386 µs) and 29.2 % at `s2-c8-g`, against 54.4 % and 57.6 % recorded. The commit is the device's sync: 543.6 µs p50 with one booker under `group` (544.3 µs under `strict`; means 559.5 and 562.1 µs) because one booker waits for its own sync either way |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core, 8 bookers: 30.1 % for `charge-insert` alone (about 5.6 charges a booking) and 5.2 % to 7.2 % for the other three; one booker: 16.5 % and 3.2 % to 5.1 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 6.8 % to 8.9 % each at one core, 8 bookers; with one booker `recipe-read` (the 93-row filtered scan) is 10.4 % and the largest read |
| Lock or conflict wait | zero client-visible conflicts at every `cores = 1` cell, 0 or 1 at `cores = 8` `group`, and 37, 48 and 53 at `cores = 8` `strict` with 8 bookers (16 at 4 bookers) (`TXN_CONFLICT retryable=1`, on `operations` and `organizations`, retried by the driver); their cost is in the unattributed column (14.4 % at `s2-c8-s` against 7.6 % at `s2-c8-g`) |
| Client and socket round trip | the unattributed column at the cells with no conflicts: 7.5 % to 8.7 % |

Outcomes, conflicts and the invariant check, every cell of the matrix:

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | run 1 | 3,000 | 286 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 28 / 400 |
| `r2-s2-c1-g` | run 2 | 3,000 | 290 | 52 | 0 | 0 | 0 / 0 / 0 / 0 | 30 / 400 |
| `r3-s2-c1-g` | run 3 | 3,000 | 282 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 25 / 400 |
| `s2-c8-g` | run 1 | 3,000 | 296 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 15 / 400 |
| `r2-s2-c8-g` | run 2 | 3,000 | 290 | 57 | 1 | 0 | 0 / 1 / 0 / 0 | 18 / 400 |
| `r3-s2-c8-g` | run 3 | 3,000 | 302 | 53 | 1 | 0 | 1 / 0 / 0 / 0 | 18 / 400 |
| `s2-c1-s` | run 1 | 3,000 | 281 | 57 | 0 | 0 | 0 / 0 / 0 / 0 | 17 / 400 |
| `r2-s2-c1-s` | run 2 | 3,000 | 284 | 54 | 0 | 0 | 0 / 0 / 0 / 0 | 27 / 400 |
| `r3-s2-c1-s` | run 3 | 3,000 | 295 | 56 | 0 | 0 | 0 / 0 / 0 / 0 | 32 / 400 |
| `s2-c8-s` | run 1 | 3,000 | 270 | 53 | 37 | 0 | 18 / 19 / 0 / 0 | 20 / 400 |
| `r2-s2-c8-s` | run 2 | 3,000 | 254 | 50 | 48 | 0 | 23 / 25 / 0 / 0 | 13 / 400 |
| `r3-s2-c8-s` | run 3 | 3,000 | 274 | 58 | 53 | 0 | 32 / 21 / 0 / 0 | 10 / 400 |
| `b1-s2-c1-g` | run 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r2-b1-s2-c1-g` | run 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r3-b1-s2-c1-g` | run 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `b1-s2-c1-s` | run 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r2-b1-s2-c1-s` | run 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |
| `r3-b1-s2-c1-s` | run 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 |

## 6. Correctness: what the invariant failures are

Under the default isolation with more than one booker the `--verify 100` pass fails 5 to 87 of 400 checks (10 to 32 in the matrix cells), `cores = 1` included, and with **one booker it fails 0 of 400** (all six single-booker runs) and under **`--isolation repeatable-read` it passes 400 of 400** at both core counts: the driver writes `SET booked_cbm = <literal>` from a value it read earlier in the same transaction, the client-side lost update its source names. The failures are not engine corruption (the same account as the recorded run's section 6); under repeatable-read the engine refuses the second writer and the driver retries.

| cell | cores | bookers | tps now | v3.3.0-38 tps | delta | conflicted | verify failures / checks |
|---|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | 1 | 8 | 1,149.8 tps | 672.0 tps | +71.1 % | 115 | 0 / 400 |
| `rr-s2-c8-g` | 8 | 8 | 2,569.2 tps | 2,036.8 tps | +26.1 % | 92 | 0 / 400 |
| `s2-c1-g` | 1 | 8 | 1,184.3 tps | 681.3 tps | +73.8 % | 0 | 28 / 400 |
| `s2-c8-g` | 8 | 8 | 2,582.5 tps | 1,990.6 tps | +29.7 % | 0 | 15 / 400 |
| `sat-b16-s2-c8-g` | 8 | 16 | 3,427.8 tps | 3,219.9 tps | +6.5 % | 1 | 44 / 400 |
| `sat-b16-s2-c8-s` | 8 | 16 | 3,231.4 tps | 2,546.4 tps | +26.9 % | 116 | 41 / 400 |

## 7. The booker sweep, and the extreme

Bookers 1, 2, 4, 8, 16 and 32, `cores = 8`, one run each except 8 (three). `--bookings 3000` in every cell (equal work, not equal time):

| bookers | durability | median | runs | spread | v3.3.0-38 median | delta | conflicted (run 1) | verify failures (run 1) |
|---|---|---|---|---|---|---|---|---|
| 1 | group | **578.6 tps** | 1 | n/a | 410.2 tps | +41.1 % | 0 | 0 / 400 |
| 1 | strict | **575.7 tps** | 1 | n/a | 416.5 tps | +38.2 % | 0 | 0 / 400 |
| 2 | group | **1,044.5 tps** | 1 | n/a | 728.5 tps | +43.4 % | 0 | 5 / 400 |
| 2 | strict | **910.6 tps** | 1 | n/a | 580.2 tps | +57.0 % | 0 | 6 / 400 |
| 4 | group | **1,700.8 tps** | 1 | n/a | 1,177.1 tps | +44.5 % | 0 | 14 / 400 |
| 4 | strict | **1,532.4 tps** | 1 | n/a | 994.0 tps | +54.2 % | 16 | 8 / 400 |
| 8 | group | **2,548.8 tps** | 3 | 1.4 % | 1,990.6 tps | +28.0 % | 0 | 15 / 400 |
| 8 | strict | **2,321.0 tps** | 3 | 2.7 % | 1,716.7 tps | +35.2 % | 37 | 20 / 400 |
| 16 | group | **3,474.0 tps** | 1 | n/a | 3,284.4 tps | +5.8 % | 1 | 51 / 400 |
| 16 | strict | **3,165.1 tps** | 1 | n/a | 2,507.0 tps | +26.3 % | 108 | 40 / 400 |
| 32 | group | **3,841.6 tps** | 1 | n/a | 3,929.6 tps | -2.2 % | 2 | 87 / 400 |
| 32 | strict | **3,471.0 tps** | 1 | n/a | 3,387.4 tps | +2.5 % | 166 | 74 / 400 |

**Throughput climbs with bookers on both durability rungs and flattens toward 32**: `group` 579 tps to 3,842 tps (6.6x for 32x the clients) and `strict` 576 to 3,471 tps (6.0x); from 8 to 16 bookers the gain is 36 % on both rungs and from 16 to 32 only 11 % (`group`) and 10 % (`strict`), the shape of a bound. The 3,000-booking cells last 0.8 to 1.5 s, so the sampler sees one or two busy seconds (peak 41 % to 66 % busy at 16 and 32 bookers) and **they are not saturation measurements**. The extreme cells are:

| cell | client processes x bookers | durability | bookings | tps (aggregate) | booking mean | commit mean | conflicted (retried) | verify failures / checks | 1-s samples with host busy 20 % or more: count, mean, peak |
|---|---|---|---|---|---|---|---|---|---|
| `big-b32-c8-g` | 1 x 32 | group | 20,000 | **3,322 tps** | 8,429 µs | 1,920 µs | 1 (1) | 8 / 400 | 7, 71 %, 93 % |
| `big-b32-c8-s` | 1 x 32 | strict | 20,000 | **3,411 tps** | 8,675 µs | 1,804 µs | 231 (215) | 5 / 400 | 6, 80 %, 88 % |
| `big2p-c8-g` | 2 x 16 | group | 20,000 | **3,372 tps** | 8,573 µs | 1,939 µs | 1 (1) | 4 / 800 | 11, 58 %, 93 % |
| `big2p-c8-s` | 2 x 16 | strict | 20,000 | **3,079 tps** | 9,844 µs | 1,889 µs | 144 (128) | 2 / 800 | 12, 56 %, 90 % |

**What bounds it. The engine tops out at about 3.1k to 3.4k tps on this 8-vCPU VM, with the host 88 % to 93 % busy at its peak seconds in the middle seconds of the run, and the bound is the machine's CPU, not the fsync and not one client process.** Three facts support that. First, one process of 32 bookers (3,322 tps `group`) and two processes of 16 (3,372 tps) are the same within 2 %: a second Python client adds no throughput, so the GIL of one client is not the ceiling. Second, the commit is not the wait that grows: a commit costs 1.8 to 1.9 ms at 32 concurrent bookers against 0.56 ms alone, a queue of 32 bookers on 8 reactor cores, and the whole-host busy samples peak at 93 %. Third, `strict` is 2.7 % above `group` at 32 bookers on one process (3,411 against 3,322 tps) and 8.7 % below it at two processes (3,079 against 3,372 tps), single runs with opposite signs, so the durability rung is not what decides it at this concurrency. **Not separated:** how the 93 % splits between the server's 8 reactor threads and the client Python processes, because both run on the same 8 vCPUs; a ceiling of the engine alone needs the clients on another host. The 3,000-booking 32-booker cells read 3,842 tps (`group`), 14 % above the 20,000-booking cell's 3,322 tps: the relations are 6.7x larger in the extreme cell.

The measured extreme gives the open item of the recorded run a first answer: that run found no limit ("the host never exceeded 65 % busy") and named a second client process as the missing control; this run supplies it, and the limit is found at the host.

## 8. The engine, isolated: v3.3.0-38 against v3.4.14, interleaved on this host

The recorded-number deltas of section 3 are engine plus host. The same driver (`35086008`'s) against the `a40882af` binary and the measured binary, alternating old, new, old, new, old, new on this host, three pairs each, medians and spreads over three runs:

| measure | v3.3.0-38 engine | spread | v3.4.14 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s2 8 bookers, cores 1, group | 1,174.4 tps | 4.8 % | 1,197.6 tps | 2.4 % | 1.02x | inside the spread |
| s2 1 booker, cores 1, strict | 625.7 tps | 3.7 % | 627.1 tps | 3.6 % | 1.00x | inside the spread |
| s2 1 booker strict, commit mean (derived qps) | 1,809.6 qps | 2.6 % | 1,840.3 qps | 1.6 % | 1.02x | inside the spread |
| s2 1 booker strict, recipe-read mean (derived qps) | 6,485.1 qps | 5.6 % | 6,600.7 qps | 6.7 % | 1.02x | inside the spread |
| s2 1 booker strict, charge-insert mean (derived qps) | 21,739.1 qps | 4.6 % | 21,231.4 qps | 4.5 % | 0.98x | inside the spread |

**The booking path is the same engine inside its spreads.** 8 bookers on one core at `group` reads 1.02x (spreads 4.8 % and 2.4 %), one booker at `strict` 1.00x (3.7 % and 3.6 %), the commit's mean 1.02x, `charge-insert` 0.98x, `recipe-read` 1.02x. The noise floor of this A/B is about 5 % and nothing is outside it. So the whole of section 3's +28 % to +88 % is the host: the commit phase's mean is 559.5 µs here against 1,294.2 µs recorded (the same ratio as the booking's), and the non-commit part of a booking moved only 893 against 958 µs.

## 9. The floor (PostgreSQL), not measured

No PostgreSQL server exists on this host (`pg_ctl`, `initdb` absent; the twin's attempt exited 1 on every cell with "connection refused" and produced no result). The recorded PostgreSQL 18.6 numbers are the v3.3.0-33 run's (419.5 tps with one durable booker, commit p50 1,278.8 µs, on the other host) and **a ratio against them would divide a faster device by a slower one**: KDS's one-booker cells are 629 and 626 tps here, 1.50x and 1.49x of 419.5, and its commit p50 is 544 µs against 1,279 µs. The statements that are not the commit are comparable in kind (the other host's CPU also differs): `charge-insert` p50 42.4 µs against PostgreSQL's 54.1 µs, the four writes 0.67x to 0.85x of its p50, `recipe-read` 1.52x. These are listed, not read:

| statement | KDS now p50 (group) | v3.3.0-38 p50 | PG 18.6 p50 (other host) | KDS now mean | v3.3.0-38 mean | PG mean |
|---|---|---|---|---|---|---|
| booking | 1,427.1 µs | 2,142.2 µs | 2,206.4 µs | 1,386.4 µs | 2,099.6 µs | 2,123.2 µs |
| cargo-lookup | 95.9 µs | 89.3 µs | 78.4 µs | 106.2 µs | 97.7 µs | 82.2 µs |
| credit-lookup | 66.2 µs | 75.5 µs | 67.9 µs | 75.6 µs | 84.5 µs | 72.6 µs |
| capacity-read | 62.1 µs | 71.5 µs | 65.5 µs | 67.2 µs | 81.1 µs | 70.6 µs |
| recipe-read | 138.2 µs | 138.1 µs | 91.0 µs | 144.7 µs | 147.9 µs | 96.3 µs |
| freight-insert | 69.0 µs | 63.9 µs | 81.0 µs | 80.0 µs | 71.3 µs | 87.7 µs |
| charge-insert | 42.4 µs | 47.6 µs | 54.1 µs | 46.0 µs | 55.0 µs | 59.8 µs |
| operation-update | 54.7 µs | 55.7 µs | 72.5 µs | 58.8 µs | 65.6 µs | 79.3 µs |
| org-update | 46.4 µs | 51.9 µs | 69.5 µs | 50.0 µs | 61.8 µs | 75.9 µs |
| commit | 543.6 µs | 1,227.5 µs | 1,278.8 µs | 559.5 µs | 1,294.2 µs | 1,343.4 µs |

## 10. Noise floor, contamination and host

* Spread over three runs: 1.0 % to 6.7 % in the six matrix cells (`b1-s2-c1-s` 6.7 %, `s2-c1-s` 5.1 %); every delta against the recorded numbers is far outside it and is a host effect by section 8. The A/B's own spreads (1.6 % to 6.7 %) set the floor at about 5 %.
* The booker-sweep points, the isolation probe, the saturation and the extreme cells are single runs and carry no spread; no delta is read from them.
* **Contamination:** none; no cell discarded. **One disclosure:** the table generator (one thread, 1 to 3 s) ran while `x-b16-s2-c8-s` / `b1-s2-c1-g` (19:08:48) and `ab-old1-s1-c1-g` (19:15) were live, below the monitor's 10 % line; neither is an outlier of its group.
* The 3,000-booking saturation-labelled cells are not saturated (peak 41 % to 66 % busy); the extreme cells of section 7 are.

## 11. What the run says about the engine

1. **A write-and-commit workload on this engine is the device's sync plus about 0.9 ms of its own work per booking.** A one-booker booking is 1,386 µs: 493 µs of commit, and 893 µs of statements and client, which on the other device and CPU was 958 µs. The engine's own cost did not move between the two versions (A/B 1.00x to 1.02x), and an fsync that is 2.3x cheaper is a 1.5x faster booking.
2. **The saturation bound on one VM is shared CPU.** 3.1k to 3.4k bookings a second at 32 bookers on 8 cores with the host 88 % to 93 % busy at its peak seconds, the same with one client process or two; the next experiment that would separate the server's ceiling from the clients' is a second host for the clients, which this environment does not have.
3. **`strict` costs 5 % (one core) to 10 % (eight cores, 8 bookers) of `group` in the matrix, and nothing readable at the extreme** (+2.7 % and -8.7 % in two single runs: 3,411 against 3,322 tps with one process, 3,079 against 3,372 with two), and with one booker the two are the same. Under conflict `strict` retries far more (231 conflicts at 20,000 bookings and 32 bookers, 1 under `group`; 37 to 53 in the 8-booker matrix cells against 0 or 1): the lock wait of an open writer lasts through the sync.

## 12. Archive

`bench/v3.4.14/archive/scenario2-v3.4.14/`: per-cell JSON, driver stdout and logs, server configs, the 1-second CPU samples of the saturation and extreme cells (`logs/*.cpu`). What the scenarios share is in `bench/v3.4.14/archive/run-v3.4.14/`. No data file is kept.
