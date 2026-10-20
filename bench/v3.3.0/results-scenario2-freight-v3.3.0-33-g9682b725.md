# Scenario 2 (freight) at `v3.3.0-33-g9682b725`

**One line.** The eight-statement freight booking runs as fast as it did at v3.1.0 or a little faster, and the one place it moved
beyond its spreads is where the commit dominates. Eight contended bookers at `cores = 8` run 1,965.0 tps under `group` and
1,674.2 tps under `strict` (medians of three, spreads 0.5 % and 3.4 %), against 1,902.7 tps and 1,584.9 tps at v3.1.0
(cross-version, +3.3 % and +5.6 %), and `cores = 1` is 649.8 tps (`group`) and 587.6 tps (`strict`),
inside the noise (-0.3 %, +0.8 %). **One durable booker equals PostgreSQL 18.6, re-measured in the same window**:
414.8 tps (`group`) and 417.1 tps (`strict`) against 419.5 tps (0.99x and 0.99x); the commit is
56 % of a booking on both engines and its mean is the same to the microsecond
(1,343.9 µs against 1,343.4 µs). The booker sweep at `cores = 8` is written up this time (section 7): `group` climbs from 394 tps with one booker to
3,768 tps with 32, `strict` to 3,071 tps, and the invariant check still fails under the default isolation and passes 400 of 400 under `repeatable-read`.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then `--load-only`, then the run on the same server, with the arguments and cell order of the v3.1.0
run (`run_all.sh` in the archive). Measured by `ck-tester`. Comparator (cross-version): `bench/v3.1.0/results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md`, re-derived from its
archived JSON. PostgreSQL 18.6: `tools/pg_scenario2_freight.py` with `synchronous_commit = on`, **fresh, three runs**.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; the four core cells x 3 runs 03:46:07 to 03:50:59 UTC (interleaved with each other, `c8-s`, `c1-s`, `c1-g`, `c8-g` per round, before any other cell of this scenario); isolation probe, booker sweep `x-b*` 03:51:03 to 03:55:14 UTC; single-booker cells 03:55:18 to 03:58:05 UTC; saturation cells 04:20:52 to 04:22:20 UTC; PostgreSQL 04:22:24 to 04:30:28 UTC |
| Worktree / branch | `bench-scenarios-1-4` / `bench-scenarios-1-4` (the main checkout is not used) |
| HEAD | `9682b725`, `git describe --tags` = `v3.3.0-33-g9682b725` ("Merge fix-fk-self-reference-refused: a self-referencing foreign key is refused NotImplemented at both doors"), committed 2026-10-20 03:33:40 UTC; equal to `origin/main` when the run began |
| Tree cleanliness | Clean (`git status` empty) at the start and at the end of the measurement; nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts. `build-release/` is ignored by git. The results files under `bench/v3.3.0/` are uncommitted additions until the commit that carries them |
| Binary provenance | `build-release/kds_server`, built in this worktree at HEAD with `cmake -DCMAKE_BUILD_TYPE=Release` (`CMakeCache.txt`: `Release`), target `kds_server`, `-j4`, finished 2026-10-20 03:36:57 UTC, 3 min 17 s **after** the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server` before the first cell; **copy sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (equal to the source binary's, checked again after the run); source binary mtime 2026-10-20 03:36:57.38 UTC. Every server started from the copy; no build ran during any reported cell (the only build of the session finished before the first cell) |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM; `/` was 95 % full (14 GB free) |
| Time budget | **Budget lifted by the operator for this run** (order of 2026-10-20: "lift the budget, re-measure PG too"). Every cell of the v3.1.0 rebaseline's plan ran, with its repeat counts, and PostgreSQL was re-measured fresh in the same window. Nothing was cut |
| Prior run of this shape | `bench/v3.1.0/results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md` (KDS binary `cd7c7c4e`, sha256 `4752f562...`, measured 2026-10-13 23:38 UTC to 2026-10-14 01:39 UTC). The two commits are 240 commits apart (`git rev-list --count cd7c7c4e..HEAD`), 101 of them first-parent and 23 of those merges, with 178 files changed under `src/`, `include/` and `CMakeLists.txt` (milestones BV to CD among them), so **every KDS delta below is cross-version and attributable to no single change**. The host and its device state are also a week-old comparison: v3.1.0's cells ran on the same VM, about 6.2 days earlier |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 (held by an unrelated `kds_server`) |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive); `/proc/loadavg` and the process list are in each cell's precheck. **A 1-second monitor ran through the whole run** (`monitor.log`: 1,500+ samples of foreign processes above 10 % CPU) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. **No cell was discarded**: the monitor saw only a `claude` process at 11 to 23 % of one CPU in 23 samples of the 1,518, all outside the 50 % line. One batch of PostgreSQL cells failed on a connection error and is listed under contamination below |
| PostgreSQL | PostgreSQL 18.6 (`/home/cdkbs/pg-bench-ba`, port 15434), defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), not started or stopped by this run. **Re-measured fresh in this window** (the twin runs follow the KDS cells, never concurrent with them) |
| Driver | **unmodified** `tools/scenario2_freight.py`, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`; exit 0 in every cell |

## 2. What was run

| cells | cores | durability | bookers | runs | extra |
|---|---|---|---|---|---|
| `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s2-c8-g` (+ `r2-`, `r3-`) | 8 / 1 / 1 / 8 | strict / strict / group / group | 8 | 3 each | - |
| `rr-s2-c1-g`, `rr-s2-c8-g` | 1 / 8 | group | 8 | 1 each | `--isolation repeatable-read` |
| `x-b{1,2,4,16}-s2-c8-{g,s}` | 8 | group and strict | 1, 2, 4, 16 | 1 each | `--bookers N` (the booker sweep) |
| `sat-b{16,32}-s2-c8-{g,s}` | 8 | group and strict | 16, 32 | 1 each | the same plus a 1-second whole-host CPU sampler |
| `b1-s2-c1-g`, `b1-s2-c1-s` (+ `r2-`, `r3-`) | 1 | group / strict | 1 | 3 each | `--bookers 1`, the PostgreSQL twin's shape |
| `pg-s2-b1` (+ `r2-`, `r3-`) | - | `synchronous_commit = on` | 1 | 3 | `pg_scenario2_freight.py` with the loaded sizes below |

Each cell: a fresh server and data file, `--schema-only`, `--load-only`, then the measured run on the same file. A booking is the driver's eight statements in
one transaction (cargo, credit, capacity and recipe reads; freight and charge inserts; operation and organization updates) and a commit; 3,000 bookings
per cell, with `--verify 100` invariant checks (400 checks) after. **The row-set axis is not swept** (rule 9): the loaded sizes are fixed by the arguments
(300 organizations, 30 ships, 300 operations, 4,000 cargos; 3,000 freights and about 17,000 charges accumulate) and the v3.1.0 plan it repeats never swept them. The
reads are pk descents and one 93-row recipe scan, which do not scale with the booking count; the writes grow `freights` and `charges` by 3,000 and 17,000 rows
inside every cell, which is a 17,000-row sweep of the insert path in one run, but there is no 200 / 1K / 10K axis and none is claimed.

Cut: nothing from the v3.1.0 plan; the booker sweep and the saturation cells that v3.1.0 left unwritten ran with the same arguments and are in section 7.

## 3. Throughput

Eight contended bookers. Each cell ran three times; the spread beside a median is this run's noise floor, and a delta smaller than the larger of the two spreads is not a finding.

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | v3.1.0 median (cross-version) | v3.1.0 spread | delta of medians |
|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 669.5 tps | 649.8 tps | 642.9 tps | **649.8 tps** | 4.1 % | 652.0 tps | 2.2 % | -0.3 % |
| `s2-c8-g` | 8 | group | 1,960.6 tps | 1,965.0 tps | 1,970.0 tps | **1,965.0 tps** | 0.5 % | 1,902.7 tps | 1.5 % | +3.3 % |
| `s2-c1-s` | 1 | strict | 596.2 tps | 587.6 tps | 586.3 tps | **587.6 tps** | 1.7 % | 583.1 tps | 8.0 % | +0.8 % |
| `s2-c8-s` | 8 | strict | 1,648.3 tps | 1,674.2 tps | 1,703.9 tps | **1,674.2 tps** | 3.4 % | 1,584.9 tps | 3.2 % | +5.6 % |

| ratio of medians | now | v3.1.0 (cross-version) |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | 3.02x | 2.92x |
| `cores = 8` over `cores = 1`, `strict` | 2.85x | 2.72x |
| `group` over `strict`, `cores = 1` | 1.11x | 1.12x |
| `group` over `strict`, `cores = 8` | 1.17x | 1.20x |

Every cell committed its 3,000 bookings. The two eight-core cells are above both spreads (`group` +3.3 % against spreads 0.5 % and 1.5 %; `strict` +5.6 %
against 3.4 % and 3.2 %), small but real; the two one-core cells are inside the noise. Section 5 shows where the eight-core gain went: the commit.

The single-booker cells (the matched shape for PostgreSQL, section 9):

| cell | run 1 | run 2 | run 3 | median | spread | v3.1.0 median (cross-version) | delta | commit p50 (run 1) | commit share of booking |
|---|---|---|---|---|---|---|---|---|---|
| `b1-s2-c1-g` | 412.7 tps | 422.1 tps | 414.8 tps | **414.8 tps** | 2.3 % | 374.0 tps | +10.9 % | 1,252.3 µs | 55.6 % |
| `b1-s2-c1-s` | 416.4 tps | 417.1 tps | 428.8 tps | **417.1 tps** | 3.0 % | 382.0 tps | +9.2 % | 1,244.5 µs | 55.9 % |

## 4. Percentiles

Run 1 of each of the four core cells, every phase of the driver (microseconds; the five percentiles are the driver's):

#### `s2-c1-g` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,327 ops | 495.6 µs | 9,277.4 µs | 10,812.5 µs | 15,005.8 µs | 21,361.4 µs | 29,277.0 µs | 0 errors |
| cargo-lookup | 3,327 ops | 81.1 µs | 258.2 µs | 344.8 µs | 1,644.6 µs | 2,197.3 µs | 7,183.5 µs | 0 errors |
| credit-lookup | 3,327 ops | 66.2 µs | 219.3 µs | 312.7 µs | 1,685.8 µs | 2,476.4 µs | 16,995.5 µs | 0 errors |
| capacity-read | 3,327 ops | 60.0 µs | 192.6 µs | 293.9 µs | 1,667.1 µs | 2,194.0 µs | 12,860.2 µs | 0 errors |
| recipe-read | 3,327 ops | 149.5 µs | 312.6 µs | 427.7 µs | 1,797.4 µs | 2,279.4 µs | 17,121.2 µs | 0 errors |
| freight-insert | 3,000 ops | 56.0 µs | 239.0 µs | 1,151.9 µs | 1,653.9 µs | 2,712.2 µs | 12,942.5 µs | 0 errors |
| charge-insert | 16,919 ops | 40.9 µs | 146.5 µs | 205.1 µs | 1,598.3 µs | 2,053.6 µs | 17,017.4 µs | 0 errors |
| operation-update | 3,000 ops | 48.8 µs | 141.4 µs | 180.9 µs | 1,573.7 µs | 1,911.5 µs | 12,871.4 µs | 0 errors |
| org-update | 3,000 ops | 45.1 µs | 138.7 µs | 174.0 µs | 1,570.8 µs | 1,951.4 µs | 4,647.3 µs | 0 errors |
| commit | 3,000 ops | 1,067.1 µs | 1,381.2 µs | 1,510.3 µs | 3,018.4 µs | 3,929.9 µs | 18,222.7 µs | 0 errors |
| manifest-scan | 100 ops | 93.7 µs | 292.3 µs | 409.8 µs | 1,780.8 µs | 2,708.0 µs | 4,067.5 µs | 0 errors |
| voyage-rollup | 100 ops | 66.2 µs | 277.1 µs | 1,192.8 µs | 1,852.6 µs | 2,388.9 µs | 3,589.1 µs | 0 errors |
| customer-statement | 50 ops | 209.7 µs | 892.6 µs | 1,208.5 µs | 2,564.5 µs | 2,682.0 µs | 2,682.0 µs | 0 errors |

#### `s2-c8-g` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,356 ops | 500.0 µs | 3,308.9 µs | 3,730.8 µs | 4,818.8 µs | 7,879.1 µs | 22,453.1 µs | 0 errors |
| cargo-lookup | 3,357 ops | 78.4 µs | 124.0 µs | 134.4 µs | 197.8 µs | 254.2 µs | 1,388.8 µs | 0 errors |
| credit-lookup | 3,357 ops | 68.5 µs | 107.0 µs | 119.1 µs | 189.1 µs | 260.8 µs | 2,256.4 µs | 0 errors |
| capacity-read | 3,357 ops | 64.8 µs | 102.2 µs | 115.4 µs | 192.0 µs | 269.5 µs | 979.3 µs | 0 errors |
| recipe-read | 3,357 ops | 159.5 µs | 230.2 µs | 268.3 µs | 392.5 µs | 559.2 µs | 3,194.1 µs | 0 errors |
| freight-insert | 3,001 ops | 50.9 µs | 82.6 µs | 91.8 µs | 191.6 µs | 338.7 µs | 676.1 µs | 0 errors |
| charge-insert | 16,931 ops | 37.2 µs | 66.3 µs | 77.2 µs | 143.9 µs | 215.5 µs | 12,649.7 µs | 0 errors |
| operation-update | 3,001 ops | 42.9 µs | 76.9 µs | 86.9 µs | 151.1 µs | 238.2 µs | 2,461.2 µs | 1 errors |
| org-update | 3,000 ops | 44.7 µs | 71.6 µs | 83.0 µs | 146.2 µs | 204.1 µs | 1,496.4 µs | 0 errors |
| commit | 3,000 ops | 1,134.3 µs | 1,859.8 µs | 2,184.2 µs | 3,160.3 µs | 6,012.8 µs | 12,416.7 µs | 0 errors |
| manifest-scan | 40 ops | 85.8 µs | 125.4 µs | 251.0 µs | 448.8 µs | 548.6 µs | 548.6 µs | 0 errors |
| voyage-rollup | 40 ops | 85.4 µs | 124.2 µs | 183.5 µs | 284.5 µs | 303.8 µs | 303.8 µs | 0 errors |
| customer-statement | 20 ops | 103.4 µs | 166.6 µs | 368.9 µs | 2,454.3 µs | 2,529.3 µs | 2,529.3 µs | 0 errors |

#### `s2-c1-s` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,309 ops | 476.4 µs | 9,804.8 µs | 12,182.0 µs | 18,144.4 µs | 22,314.3 µs | 34,785.9 µs | 0 errors |
| cargo-lookup | 3,309 ops | 62.3 µs | 171.0 µs | 213.8 µs | 1,844.8 µs | 2,818.5 µs | 10,793.6 µs | 0 errors |
| credit-lookup | 3,309 ops | 58.2 µs | 144.9 µs | 238.0 µs | 2,380.5 µs | 3,088.8 µs | 5,782.1 µs | 0 errors |
| capacity-read | 3,309 ops | 53.7 µs | 146.8 µs | 228.4 µs | 2,485.6 µs | 3,534.6 µs | 7,825.7 µs | 0 errors |
| recipe-read | 3,309 ops | 161.0 µs | 277.6 µs | 376.3 µs | 2,667.5 µs | 3,683.7 µs | 10,918.6 µs | 0 errors |
| freight-insert | 3,000 ops | 55.9 µs | 255.6 µs | 1,304.3 µs | 3,004.3 µs | 4,208.8 µs | 14,820.6 µs | 0 errors |
| charge-insert | 16,917 ops | 39.3 µs | 163.3 µs | 276.1 µs | 2,642.3 µs | 3,730.6 µs | 14,825.4 µs | 0 errors |
| operation-update | 3,000 ops | 55.0 µs | 145.3 µs | 207.1 µs | 2,487.7 µs | 3,635.1 µs | 14,802.8 µs | 0 errors |
| org-update | 3,000 ops | 48.0 µs | 141.3 µs | 195.2 µs | 2,305.9 µs | 3,011.0 µs | 14,805.7 µs | 0 errors |
| commit | 3,000 ops | 1,074.2 µs | 1,298.8 µs | 1,442.0 µs | 3,496.3 µs | 4,677.0 µs | 14,697.1 µs | 0 errors |
| manifest-scan | 120 ops | 87.3 µs | 288.8 µs | 464.6 µs | 2,794.4 µs | 3,637.5 µs | 6,766.5 µs | 0 errors |
| voyage-rollup | 120 ops | 71.4 µs | 209.1 µs | 382.7 µs | 3,481.2 µs | 3,958.0 µs | 4,037.2 µs | 0 errors |
| customer-statement | 60 ops | 144.6 µs | 691.4 µs | 1,278.1 µs | 3,341.2 µs | 8,713.9 µs | 8,713.9 µs | 0 errors |

#### `s2-c8-s` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| booking | 3,370 ops | 495.4 µs | 3,675.1 µs | 4,273.8 µs | 6,163.8 µs | 9,806.7 µs | 14,567.9 µs | 0 errors |
| cargo-lookup | 3,424 ops | 73.1 µs | 129.5 µs | 140.9 µs | 228.8 µs | 294.2 µs | 656.9 µs | 0 errors |
| credit-lookup | 3,424 ops | 63.6 µs | 112.2 µs | 125.6 µs | 236.7 µs | 312.7 µs | 1,146.5 µs | 0 errors |
| capacity-read | 3,424 ops | 63.0 µs | 107.9 µs | 122.4 µs | 248.5 µs | 427.1 µs | 1,078.9 µs | 0 errors |
| recipe-read | 3,424 ops | 161.6 µs | 264.8 µs | 294.3 µs | 510.3 µs | 624.4 µs | 1,074.7 µs | 0 errors |
| freight-insert | 3,054 ops | 51.3 µs | 87.3 µs | 99.2 µs | 297.1 µs | 398.5 µs | 1,156.4 µs | 0 errors |
| charge-insert | 17,227 ops | 40.3 µs | 68.9 µs | 80.5 µs | 150.1 µs | 260.6 µs | 1,645.4 µs | 0 errors |
| operation-update | 3,054 ops | 44.9 µs | 77.3 µs | 87.2 µs | 157.3 µs | 1,511.1 µs | 4,735.3 µs | 20 errors |
| org-update | 3,034 ops | 39.1 µs | 72.3 µs | 82.6 µs | 149.6 µs | 490.1 µs | 4,124.3 µs | 34 errors |
| commit | 3,000 ops | 1,142.2 µs | 1,966.4 µs | 2,341.8 µs | 3,207.1 µs | 6,724.7 µs | 11,267.6 µs | 0 errors |
| manifest-scan | 40 ops | 86.9 µs | 127.7 µs | 189.4 µs | 350.7 µs | 455.8 µs | 455.8 µs | 0 errors |
| voyage-rollup | 40 ops | 93.8 µs | 124.7 µs | 174.6 µs | 360.6 µs | 2,197.9 µs | 2,197.9 µs | 0 errors |
| customer-statement | 20 ops | 111.1 µs | 166.5 µs | 197.7 µs | 1,287.6 µs | 2,185.9 µs | 2,185.9 µs | 0 errors |

## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN, refused attempts, the client and the socket). Each phase's mean times its ops over the
booking's mean times its ops, run 1, now and at v3.1.0 (cross-version):

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed (BEGIN, refused attempts, client, socket) | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` (now) | 7.5 % | 7.4 % | 7.0 % | 8.3 % | 8.1 % | 29.5 % | 4.5 % | 4.4 % | 16.0 % | 7.3 % | 10,647 µs |
| `s2-c1-g` (v3.1.0) | 7.4 % | 7.0 % | 7.2 % | 8.3 % | 8.3 % | 29.8 % | 4.3 % | 4.3 % | 15.8 % | 7.6 % | 10,662 µs |
| `s2-c8-g` (now) | 4.0 % | 3.6 % | 3.5 % | 7.8 % | 2.7 % | 12.2 % | 2.4 % | 2.3 % | 57.2 % | 4.4 % | 3,550 µs |
| `s2-c8-g` (v3.1.0) | 3.7 % | 3.3 % | 3.3 % | 7.3 % | 2.5 % | 11.1 % | 2.2 % | 2.1 % | 60.5 % | 4.0 % | 3,699 µs |
| `s2-c1-s` (now) | 5.1 % | 5.8 % | 5.9 % | 7.3 % | 9.8 % | 35.5 % | 5.3 % | 4.8 % | 13.9 % | 6.6 % | 11,983 µs |
| `s2-c1-s` (v3.1.0) | 5.0 % | 5.8 % | 6.1 % | 7.9 % | 9.7 % | 34.4 % | 5.0 % | 4.9 % | 14.3 % | 6.9 % | 11,870 µs |
| `s2-c8-s` (now) | 3.7 % | 3.4 % | 3.4 % | 7.5 % | 2.7 % | 10.9 % | 2.6 % | 2.3 % | 50.5 % | 13.1 % | 4,215 µs |
| `s2-c8-s` (v3.1.0) | 3.5 % | 3.3 % | 3.3 % | 7.3 % | 2.7 % | 10.3 % | 2.3 % | 2.1 % | 50.7 % | 14.5 % | 4,416 µs |

| Wait | Reading |
|---|---|
| Durability / commit | `commit` is 16.0 % of a booking at `s2-c1-g`, **57.2 % at `s2-c8-g`** and 50.5 % at `s2-c8-s`; with one booker it is 55.6 % and its p50 (1,252.3 µs) is the device's sync: PostgreSQL's is 1,278.8 µs. At eight cores the commit, not the statements, bounds the booking |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: 29.5 % for `charge-insert` alone (about 5.6 charges per booking) and 8.1 / 4.5 / 4.4 % for the three others; eight cores 12.2 / 2.7 / 2.4 / 2.3 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.0 to 8.3 % each at one core, 3.5 to 7.8 % at eight; `recipe-read` is the FilterScan and the largest |
| Lock or conflict wait | zero client-visible conflicts at every `cores = 1` cell and at `c8-g` 1 / 0 / 0; **54 / 42 / 39 at `c8-s`** (all on `operations` and `organizations` rows: `TXN_CONFLICT retryable=1`), all retried by the driver (v3.1.0: 90 / 63 / 45). Their cost sits in the unattributed column: 13.1 % at `s2-c8-s` against 4.4 % at `s2-c8-g` |
| Client and socket round trip | the unattributed column at the cells with no conflicts, 4.4 to 7.3 % |

Outcomes, conflicts and the invariant check, every cell of the matrix, with the conflict lines the server logged (`TXN_CONFLICT`; the second number is the btree
re-descent refusal `gave up after 5 attempts`, which a statement that wrote nothing re-runs under BA-S14 and which **reached the driver in no cell**):

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks | log conflict lines (row / descent) |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | 3,000 | 275 | 52 | 0 | 0 | 0 / 0 / 0 / 0 | 24 / 400 | 12 (12 / 0) |
| `r2-s2-c1-g` | 2 | 3,000 | 269 | 51 | 0 | 0 | 0 / 0 / 0 / 0 | 26 / 400 | 9 (9 / 0) |
| `r3-s2-c1-g` | 3 | 3,000 | 274 | 49 | 0 | 0 | 0 / 0 / 0 / 0 | 32 / 400 | 11 (11 / 0) |
| `s2-c8-g` | 1 | 3,000 | 299 | 57 | 1 | 0 | 1 / 0 / 0 / 0 | 13 / 400 | 9 (7 / 2) |
| `r2-s2-c8-g` | 2 | 3,000 | 293 | 57 | 0 | 0 | 0 / 0 / 0 / 0 | 21 / 400 | 9 (9 / 0) |
| `r3-s2-c8-g` | 3 | 3,000 | 296 | 57 | 0 | 0 | 0 / 0 / 0 / 0 | 14 / 400 | 14 (13 / 1) |
| `s2-c1-s` | 1 | 3,000 | 263 | 46 | 0 | 0 | 0 / 0 / 0 / 0 | 35 / 400 | 9 (9 / 0) |
| `r2-s2-c1-s` | 2 | 3,000 | 275 | 53 | 0 | 0 | 0 / 0 / 0 / 0 | 29 / 400 | 11 (11 / 0) |
| `r3-s2-c1-s` | 3 | 3,000 | 306 | 43 | 0 | 0 | 0 / 0 / 0 / 0 | 35 / 400 | 7 (7 / 0) |
| `s2-c8-s` | 1 | 3,000 | 316 | 54 | 54 | 0 | 20 / 34 / 0 / 0 | 13 / 400 | 129 (126 / 3) |
| `r2-s2-c8-s` | 2 | 3,000 | 311 | 58 | 42 | 0 | 15 / 27 / 0 / 0 | 10 / 400 | 97 (96 / 1) |
| `r3-s2-c8-s` | 3 | 3,000 | 248 | 56 | 39 | 0 | 14 / 25 / 0 / 0 | 16 / 400 | 106 (104 / 2) |
| `b1-s2-c1-g` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |
| `r2-b1-s2-c1-g` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |
| `r3-b1-s2-c1-g` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |
| `b1-s2-c1-s` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |
| `r2-b1-s2-c1-s` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |
| `r3-b1-s2-c1-s` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 0 / 0 / 0 | 0 / 400 | 0 (0 / 0) |

## 6. Correctness: what the invariant failures are

The `--verify 100` pass checks four invariants (booked volume against the sum of freights, ship capacity, outstanding credit against recomputed charges, charge rows against
rules) 400 times while the bookers run. Under the **default isolation** with more than one booker it fails 10 to 35 of 400 in every cell, `cores = 1` included
where no conflict is ever reported; with **one booker it fails 0 of 400 in all six cells**; under **`--isolation repeatable-read` it passes 400 of 400** at both core counts. The
driver writes `SET booked_cbm = <literal>` from a value it read earlier in the same transaction, which its source names "exactly the lost-update shape": under
read committed two bookers that read one voyage before either commits both write `read + their cbm`, and the second overwrites the first. The failures are
that client-side lost update, not engine corruption: under repeatable-read the engine refuses the second writer (`TXN_CONFLICT`), the driver retries, and every check
passes. This is the reading the v3.1.0 file made of its own failures (13 to 34 of 400), and no evidence here says otherwise.

| cell | isolation | tps | verify failures / checks | conflicted | v3.1.0 tps (cross-version) |
|---|---|---|---|---|---|
| `s2-c1-g` | server default (read-committed), median of 3 | 649.8 tps | 24/26/32 of 400 | 0/0/0 | 652.0 tps |
| `rr-s2-c1-g` | repeatable-read, 1 run | 651.2 tps | 0 of 400 | 119 | 639.4 tps |
| `s2-c8-g` | server default (read-committed), median of 3 | 1,965.0 tps | 13/21/14 of 400 | 1/0/0 | 1,902.7 tps |
| `rr-s2-c8-g` | repeatable-read, 1 run | 1,932.3 tps | 0 of 400 | 62 | 1,805.2 tps |

The repeatable-read probe costs no throughput (651.2 tps at `cores = 1`, 1,932.3 tps at `cores = 8` against 649.8 and 1,965.0 under the default), and it turns the failed checks into
client-visible conflicts (119 and 62 `conflicted` replies), retried by the driver: a refused write instead of a wrong read.

## 7. The booker sweep at `cores = 8`, written up this time

Bookers 1, 2, 4, 8, 16 (the v3.1.0 plan) and 32 (the saturation cells), `cores = 8`, one run each except 8 (three). `--bookings 3000` in every cell, so every cell does the same work
(equal work, not equal time). The v3.1.0 column is its single `x-b*` run (cross-version, one run, so a delta below the 3 % to 12 % single-run spread is not read; only the
`cores = 8, 8 bookers` pair has a measured spread):

| bookers | durability | now: run 1 | run 2 | run 3 | median | spread | v3.1.0 (1 run, cross-version) | delta | conflicted (run 1) | abandoned (run 1) | verify failures (run 1) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | group | 394.3 tps | n/a | n/a | **394.3 tps** | n/a | 361.7 tps | +9.0 % | 0 | 0 | 0 / 400 |
| 1 | strict | 401.8 tps | n/a | n/a | **401.8 tps** | n/a | 324.4 tps | +23.9 % | 0 | 0 | 0 / 400 |
| 2 | group | 696.7 tps | n/a | n/a | **696.7 tps** | n/a | 704.1 tps | -1.1 % | 0 | 0 | 1 / 400 |
| 2 | strict | 564.0 tps | n/a | n/a | **564.0 tps** | n/a | 556.7 tps | +1.3 % | 0 | 0 | 8 / 400 |
| 4 | group | 1,154.9 tps | n/a | n/a | **1,154.9 tps** | n/a | 1,136.8 tps | +1.6 % | 0 | 0 | 9 / 400 |
| 4 | strict | 964.6 tps | n/a | n/a | **964.6 tps** | n/a | 997.7 tps | -3.3 % | 11 | 0 | 7 / 400 |
| 8 | group | 1,960.6 tps | 1,965.0 tps | 1,970.0 tps | **1,965.0 tps** | 0.5 % | 1,902.7 tps | +3.3 % | 1 | 0 | 13 / 400 |
| 8 | strict | 1,648.3 tps | 1,674.2 tps | 1,703.9 tps | **1,674.2 tps** | 3.4 % | 1,584.9 tps | +5.6 % | 54 | 0 | 13 / 400 |
| 16 | group | 3,095.1 tps | n/a | n/a | **3,095.1 tps** | n/a | 2,759.2 tps | +12.2 % | 0 | 0 | 28 / 400 |
| 16 | strict | 2,417.2 tps | n/a | n/a | **2,417.2 tps** | n/a | 2,296.0 tps | +5.3 % | 141 | 0 | 25 / 400 |
| 32 | group | 3,767.7 tps | n/a | n/a | **3,767.7 tps** | n/a | n/a | n/a | 1 | 0 | 69 / 400 |
| 32 | strict | 3,071.4 tps | n/a | n/a | **3,071.4 tps** | n/a | n/a | n/a | 351 | 0 | 75 / 400 |

**Throughput climbs with bookers on both durability rungs and the gap between them opens**: `group` 394 tps to
3,768 tps (9.6x for 32x the clients) and `strict` 402 tps to 3,071 tps (7.6x), with `group` over `strict` going 0.98x at one booker, 1.24x
at two, 1.20x at four, 1.17x at eight, 1.28x at 16 and 1.23x at 32. The one-booker rows are the fsync-bound pair: their cross-version deltas (+9.0 % and +23.9 %; v3.1.0's `strict` single run is the slow one) are carried by the commit, as section 8 shows for the matched `cores = 1` cells; whether by the device or the engine, this run does not separate. **A single booker is not slower under `strict`**: it waits for its own sync either way. From two bookers on `strict` pays for the
staged commit that parks and publishes after its sync, plus the retried conflicts (54 at 8 bookers, 141 at 16, 351 at 32; zero up to two bookers and 11 at four).

What bounds it. The 1-second whole-host sampler around the saturation cells:

| cell | 1-s samples | mean busy | peak busy | mean steal |
|---|---|---|---|---|
| `sat-b16-s2-c8-g` | 19 | 6 % | 38 % | 0.0 % |
| `sat-b16-s2-c8-s` | 19 | 6 % | 40 % | 0.0 % |
| `sat-b32-s2-c8-g` | 19 | 6 % | 67 % | 0.0 % |
| `sat-b32-s2-c8-s` | 19 | 6 % | 53 % | 0.0 % |

No limit of the engine was reached in these cells: the host never exceeded 67 % busy (peak of any one-second sample), steal was zero, and each cell lasts under two seconds of measured booking time
(3,000 bookings at 3,000+ tps), so a one-second sampler sees two or three samples of it. The driver is Python threads in one process, so the client side (the GIL) is a candidate bound that this run
did not rule out, and the 32-booker invariant failures (69 and 75 of 400, against 13 at eight) say the contention, not the hardware, is rising. The cells are fixed at 3,000 bookings; a
saturation measurement needs a longer run (`--bookings` larger) and a second client process. **That is the open item of this scenario**: it has not found the ceiling.

## 8. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.1.0/results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md` (three runs per matrix cell), 240 commits apart. Throughput deltas: section 3 and 7. Statement level, run 1
against run 1, `cores = 8`, `strict` and the single-booker cell:

| cell | phase | p0 now | p0 v3.1.0 | p50 now | p50 v3.1.0 | p50 now/then | mean now | mean v3.1.0 |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 495.6 µs | 464.2 µs | 10,812.5 µs | 10,306.6 µs | 1.05x | 10,647.4 µs | 10,662.0 µs |
| `s2-c1-g` | commit | 1,067.1 µs | 1,030.0 µs | 1,510.3 µs | 1,371.1 µs | 1.10x | 1,889.0 µs | 1,883.6 µs |
| `s2-c1-g` | freight-insert | 56.0 µs | 46.8 µs | 1,151.9 µs | 1,038.5 µs | 1.11x | 960.1 µs | 993.3 µs |
| `s2-c1-g` | charge-insert | 40.9 µs | 38.3 µs | 205.1 µs | 196.5 µs | 1.04x | 617.7 µs | 628.9 µs |
| `s2-c1-g` | operation-update | 48.8 µs | 49.0 µs | 180.9 µs | 166.1 µs | 1.09x | 531.3 µs | 506.4 µs |
| `s2-c1-g` | cargo-lookup | 81.1 µs | 76.7 µs | 344.8 µs | 320.4 µs | 1.08x | 803.8 µs | 790.6 µs |
| `s2-c1-g` | recipe-read | 149.5 µs | 156.9 µs | 427.7 µs | 418.1 µs | 1.02x | 887.6 µs | 880.7 µs |
| `s2-c8-g` | booking | 500.0 µs | 484.6 µs | 3,730.8 µs | 3,486.2 µs | 1.07x | 3,550.3 µs | 3,699.2 µs |
| `s2-c8-g` | commit | 1,134.3 µs | 1,100.8 µs | 2,184.2 µs | 2,030.3 µs | 1.08x | 2,271.1 µs | 2,498.1 µs |
| `s2-c8-g` | freight-insert | 50.9 µs | 50.3 µs | 91.8 µs | 87.5 µs | 1.05x | 105.8 µs | 103.8 µs |
| `s2-c8-g` | charge-insert | 37.2 µs | 36.6 µs | 77.2 µs | 73.1 µs | 1.06x | 86.0 µs | 81.0 µs |
| `s2-c8-g` | operation-update | 42.9 µs | 47.5 µs | 86.9 µs | 81.8 µs | 1.06x | 95.5 µs | 90.6 µs |
| `s2-c8-g` | cargo-lookup | 78.4 µs | 72.8 µs | 134.4 µs | 129.5 µs | 1.04x | 141.9 µs | 137.7 µs |
| `s2-c8-g` | recipe-read | 159.5 µs | 156.7 µs | 268.3 µs | 263.0 µs | 1.02x | 276.2 µs | 270.0 µs |
| `s2-c8-s` | booking | 495.4 µs | 470.7 µs | 4,273.8 µs | 4,075.9 µs | 1.05x | 4,215.0 µs | 4,416.5 µs |
| `s2-c8-s` | commit | 1,142.2 µs | 1,066.2 µs | 2,341.8 µs | 2,098.1 µs | 1.12x | 2,389.2 µs | 2,502.6 µs |
| `s2-c8-s` | freight-insert | 51.3 µs | 55.3 µs | 99.2 µs | 97.5 µs | 1.02x | 127.2 µs | 129.6 µs |
| `s2-c8-s` | charge-insert | 40.3 µs | 37.9 µs | 80.5 µs | 77.4 µs | 1.04x | 89.5 µs | 88.0 µs |
| `s2-c8-s` | operation-update | 44.9 µs | 42.8 µs | 87.2 µs | 84.0 µs | 1.04x | 121.1 µs | 111.0 µs |
| `s2-c8-s` | cargo-lookup | 73.1 µs | 69.4 µs | 140.9 µs | 138.5 µs | 1.02x | 152.4 µs | 150.2 µs |
| `s2-c8-s` | recipe-read | 161.6 µs | 160.4 µs | 294.3 µs | 292.0 µs | 1.01x | 310.3 µs | 312.3 µs |
| `b1-s2-c1-g` | booking | 443.9 µs | 437.7 µs | 2,186.2 µs | 2,236.5 µs | 0.98x | 2,131.5 µs | 2,352.4 µs |
| `b1-s2-c1-g` | commit | 1,014.5 µs | 1,101.9 µs | 1,252.3 µs | 1,349.8 µs | 0.93x | 1,343.9 µs | 1,645.9 µs |
| `b1-s2-c1-g` | freight-insert | 56.3 µs | 46.6 µs | 62.2 µs | 58.4 µs | 1.07x | 68.5 µs | 64.5 µs |
| `b1-s2-c1-g` | charge-insert | 42.1 µs | 40.4 µs | 45.2 µs | 43.0 µs | 1.05x | 51.6 µs | 48.4 µs |
| `b1-s2-c1-g` | operation-update | 52.3 µs | 49.7 µs | 55.2 µs | 52.3 µs | 1.06x | 61.8 µs | 57.6 µs |
| `b1-s2-c1-g` | cargo-lookup | 68.6 µs | 65.8 µs | 87.2 µs | 84.5 µs | 1.03x | 93.0 µs | 89.2 µs |
| `b1-s2-c1-g` | recipe-read | 151.4 µs | 150.6 µs | 176.5 µs | 173.2 µs | 1.02x | 183.2 µs | 181.0 µs |

The statements did not get faster, and per statement they are slightly slower: every statement p50 in this table (the 20 rows other than `booking` and `commit`) is 1.01x to 1.11x of v3.1.0's (median about 1.04x), the same sign in all 20, and the `pk` control of scenario 4 reads 73.4 µs
against 70.1 µs (+4.7 %). Whether that 3 % to 7 % is the engine (240 commits) or the host (the same VM, a week later) is **not separated by this run**. The booking still comes out level or faster because the **commit** fell:
the single-booker row's movement (+10.9 %) is the commit, whose mean fell from 1,645.9 to 1,343.9 µs (-18 %) while its p50 fell 7 % (1,349.8 to 1,252.3 µs)
and its p0 from 1,101.9 to 1,014.5 µs, so most of the fall is the tail. At `cores = 8` the commit
mean also fell (2,271 against 2,498 µs `group`, 2,389 against 2,503 µs `strict`), which is where the +3.3 % and +5.6 % come from; at `s2-c1-g` it did not (1,889.0 against 1,883.6 µs).
PostgreSQL's commit mean in this window is 1,343.4 µs, equal to KDS's. **Whether the fall is the device or the engine is not separated by this run**: PostgreSQL was not
measured in v3.1.0's window (that file reused PostgreSQL from 2026-09-30, commit mean 1,543.0 µs), and scenario 1's durable autocommit insert did not get faster in the same
comparison (744 against 773 rows/s).

## 9. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor and answers "is the engine in the right league" and nothing finer. The twin drives **one** booking connection with its manifest reporter interleaved, so the matched KDS
cell is `--bookers 1`; the eight-booker cells have no PostgreSQL counterpart. Three runs each, same window, PostgreSQL after the KDS cells:

| engine / cell | run 1 | run 2 | run 3 | median | spread | KDS/PG |
|---|---|---|---|---|---|---|
| KDS `b1-s2-c1-g` (this run) | 412.7 tps | 422.1 tps | 414.8 tps | 414.8 tps | 2.3 % | 0.99x |
| KDS `b1-s2-c1-s` (this run) | 416.4 tps | 417.1 tps | 428.8 tps | 417.1 tps | 3.0 % | 0.99x |
| PostgreSQL 18.6, `synchronous_commit = on`, fresh | 419.5 tps | 427.1 tps | 414.4 tps | 419.5 tps | 3.1 % | 1.00x |

Statement level, run 1 of each (the write statements are the engine's strength, the recipe scan its weakness):

| statement | KDS `b1-s2-c1-g` p50 | PostgreSQL p50 | KDS/PG (below 1.00x = KDS faster) | KDS mean | PG mean |
|---|---|---|---|---|---|
| booking | 2,186.2 µs | 2,206.4 µs | 0.99x | 2,131.5 µs | 2,123.2 µs |
| cargo-lookup | 87.2 µs | 78.4 µs | 1.11x | 93.0 µs | 82.2 µs |
| credit-lookup | 69.7 µs | 67.9 µs | 1.03x | 78.3 µs | 72.6 µs |
| capacity-read | 66.2 µs | 65.5 µs | 1.01x | 75.8 µs | 70.6 µs |
| recipe-read | 176.5 µs | 91.0 µs | 1.94x | 183.2 µs | 96.3 µs |
| freight-insert | 62.2 µs | 81.0 µs | 0.77x | 68.5 µs | 87.7 µs |
| charge-insert | 45.2 µs | 54.1 µs | 0.84x | 51.6 µs | 59.8 µs |
| operation-update | 55.2 µs | 72.5 µs | 0.76x | 61.8 µs | 79.3 µs |
| org-update | 51.0 µs | 69.5 µs | 0.73x | 57.8 µs | 75.9 µs |
| commit | 1,252.3 µs | 1,278.8 µs | 0.98x | 1,343.9 µs | 1,343.4 µs |

**KDS and PostgreSQL are the same machine on one durable client**: the booking mean is 2,132 µs against 2,123 µs. The four write statements run at 0.73x to 0.84x of PostgreSQL's p50 (a
KDS write is cheaper than a PostgreSQL write by a quarter) and `recipe-read`, a 93-row FilterScan, runs at 1.94x (KDS slower), which is the per-row result cost scenario 1 isolates. The two roughly
cancel in the booking, and the commit is the same fsync.

## 10. Noise floor, contamination and host

The 3-run spreads of section 3 are the noise floor: 0.5 % to 4.1 % at the matrix cells and 2.3 % to 3.0 % at the single-booker cells; the PostgreSQL cells' is 3.1 %. The booker sweep's
single runs have no spread of their own; their floor is the 8-booker pair's 0.5 % (`group`) to 3.4 % (`strict`), and the repeat is the 16-booker pair, run twice (`x-b16` and `sat-b16`): 3,095.1 against 3,057.5 tps under `group` (1.2 %) and 2,417.2 against 2,382.1 tps under `strict` (1.5 %). A single-run delta in the sweep below about 3 % is therefore not read; the cross-version deltas of 12.2 % (16 `group`) and 5.3 % (16 `strict`) are single against single.

**Contamination.** No KDS cell was discarded (140 validity evaluations, none contaminated; `contaminated/` in the run archive holds only the failed PostgreSQL scenario-3 attempt, which is not scenario 2's).

Per-cell host stamps:

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s2-c8-s` | 03:46:10 | 03:46:26 | 1.04 / 1.19 / 2.06 | 0 s | none | 0 of 9 samples | 0 |
| `r2-s2-c8-s` | 03:47:49 | 03:48:05 | 0.95 / 1.10 / 1.93 | 0 s | none | 0 of 10 samples | 0 |
| `r3-s2-c8-s` | 03:49:27 | 03:49:44 | 1.04 / 1.13 / 1.87 | 0 s | none | 0 of 10 samples | 0 |
| `s2-c1-s` | 03:46:34 | 03:46:53 | 0.87 / 1.14 / 2.02 | 0 s | none | 0 of 10 samples | 0 |
| `r2-s2-c1-s` | 03:48:12 | 03:48:31 | 1.40 / 1.19 / 1.95 | 0 s | none | 1 of 10 samples | 0 |
| `r3-s2-c1-s` | 03:49:51 | 03:50:10 | 0.95 / 1.10 / 1.84 | 0 s | none | 0 of 11 samples | 0 |
| `s2-c1-g` | 03:47:00 | 03:47:19 | 0.76 / 1.09 / 1.98 | 0 s | none | 0 of 11 samples | 0 |
| `r2-s2-c1-g` | 03:48:38 | 03:48:57 | 1.22 / 1.17 / 1.91 | 0 s | none | 1 of 11 samples | 0 |
| `r3-s2-c1-g` | 03:50:17 | 03:50:36 | 1.23 / 1.16 / 1.83 | 0 s | none | 0 of 10 samples | 0 |
| `s2-c8-g` | 03:47:26 | 03:47:42 | 0.98 / 1.12 / 1.96 | 0 s | none | 0 of 9 samples | 0 |
| `r2-s2-c8-g` | 03:49:04 | 03:49:20 | 0.99 / 1.13 / 1.88 | 0 s | none | 0 of 9 samples | 0 |
| `r3-s2-c8-g` | 03:50:43 | 03:50:59 | 1.07 / 1.13 / 1.80 | 0 s | none | 1 of 10 samples | 0 |
| `rr-s2-c1-g` | 03:51:06 | 03:51:25 | 1.17 / 1.15 / 1.79 | 0 s | none | 0 of 10 samples | 0 |
| `rr-s2-c8-g` | 03:51:32 | 03:51:48 | 1.30 / 1.19 / 1.79 | 0 s | none | 0 of 10 samples | 0 |
| `x-b1-s2-c8-g` | 03:51:55 | 03:52:17 | 1.04 / 1.14 / 1.76 | 0 s | none | 0 of 12 samples | 0 |
| `x-b1-s2-c8-s` | 03:52:24 | 03:52:46 | 0.87 / 1.09 / 1.72 | 0 s | none | 1 of 12 samples | 0 |
| `x-b2-s2-c8-g` | 03:52:53 | 03:53:12 | 0.84 / 1.06 / 1.69 | 0 s | none | 0 of 11 samples | 0 |
| `x-b2-s2-c8-s` | 03:53:19 | 03:53:39 | 0.99 / 1.09 / 1.68 | 0 s | none | 0 of 12 samples | 0 |
| `x-b4-s2-c8-g` | 03:53:46 | 03:54:04 | 1.10 / 1.11 / 1.68 | 0 s | none | 0 of 10 samples | 0 |
| `x-b4-s2-c8-s` | 03:54:11 | 03:54:28 | 0.91 / 1.07 / 1.65 | 0 s | none | 0 of 10 samples | 0 |
| `x-b16-s2-c8-g` | 03:54:35 | 03:54:51 | 0.86 / 1.05 / 1.62 | 0 s | none | 0 of 9 samples | 0 |
| `x-b16-s2-c8-s` | 03:54:58 | 03:55:14 | 0.90 / 1.04 / 1.61 | 0 s | none | 0 of 9 samples | 0 |
| `b1-s2-c1-g` | 03:55:21 | 03:55:43 | 1.03 / 1.07 / 1.60 | 0 s | none | 0 of 12 samples | 0 |
| `r2-b1-s2-c1-g` | 03:56:18 | 03:56:40 | 0.83 / 1.01 / 1.55 | 0 s | none | 0 of 12 samples | 0 |
| `r3-b1-s2-c1-g` | 03:57:15 | 03:57:37 | 0.91 / 1.00 / 1.52 | 0 s | none | 0 of 12 samples | 0 |
| `b1-s2-c1-s` | 03:55:50 | 03:56:11 | 0.86 / 1.03 / 1.57 | 0 s | none | 0 of 12 samples | 0 |
| `r2-b1-s2-c1-s` | 03:56:47 | 03:57:08 | 0.82 / 0.99 / 1.53 | 0 s | none | 0 of 11 samples | 0 |
| `r3-b1-s2-c1-s` | 03:57:44 | 03:58:05 | 0.85 / 0.98 / 1.49 | 0 s | none | 0 of 12 samples | 0 |
| `sat-b16-s2-c8-g` | 04:20:55 | 04:21:10 | 0.56 / 0.51 / 0.78 | 0 s | none | 0 of 9 samples | 0 |
| `sat-b16-s2-c8-s` | 04:21:18 | 04:21:34 | 0.90 / 0.59 / 0.80 | 0 s | none | 0 of 10 samples | 0 |
| `sat-b32-s2-c8-g` | 04:21:41 | 04:21:56 | 0.84 / 0.61 / 0.80 | 0 s | none | 0 of 9 samples | 0 |
| `sat-b32-s2-c8-s` | 04:22:04 | 04:22:20 | 0.88 / 0.64 / 0.81 | 0 s | none | 0 of 9 samples | 0 |
| `pg-s2-b1` | 04:22:27 | 04:22:41 | 0.84 / 0.65 / 0.81 | 0 s | none | 0 of 9 samples | 0 |
| `r2-pg-s2-b1` | 04:26:36 | 04:26:50 | 0.72 / 0.72 / 0.81 | 0 s | none | 0 of 9 samples | 0 |
| `r3-pg-s2-b1` | 04:30:14 | 04:30:28 | 1.01 / 0.85 / 0.84 | 0 s | none | 0 of 8 samples | 0 |

## 11. What the run says about the engine

1. **The commit is the whole story at both ends.** One booker: commit 55.6 % of the booking, identical to PostgreSQL's to the microsecond; eight bookers at `cores = 8`: 57.2 % under `group`, 50.5 % under
   `strict`. The statements around it (reads at 66 to 87 µs, writes at 45 to 62 µs p50) are not what a booking waits for. A faster engine cannot show in this scenario without a faster device or a cheaper group commit.
2. **Eight cores do their job on commit-bound work**: `cores = 8` over `cores = 1` is 3.02x under `group` and 2.85x under `strict` for 8 bookers, and `group` over `strict` is 1.17x, the staged
   `strict` commit costing little over `group` once clients overlap (BA-S7).
3. **The invariant check measures the driver's read-modify-write under read committed, not engine corruption** (section 6): the lost update it produces is refused under repeatable-read, which passes 400 of 400 at no throughput cost.
4. **This run did not find the engine's limit under booking load** (section 7): 3,768 tps at 32 bookers with the host at most 67 % busy. Finding it needs a longer cell and a client that is not one Python process; that is the
   next measurement, not a conclusion.

## 12. Archive

`bench/v3.3.0/archive/scenario2-v3.3.0-33-g9682b725/`: per-cell JSON (KDS and PostgreSQL), driver stdout, schema and load logs, server configs, the 1-second CPU samples of the saturation cells, the server logs. What the four
scenarios share is in `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`.
