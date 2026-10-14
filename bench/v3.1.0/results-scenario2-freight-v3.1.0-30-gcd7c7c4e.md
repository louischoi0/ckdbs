# Scenario 2 (freight) at `v3.1.0-30-gcd7c7c4e`

**One line.** The eight-statement freight booking runs faster at every
core count and durability than at `9a0525d`, and the commit path no longer
bounds `strict`: eight contended bookers at `cores = 8` run
1,902.7 tps under `group` and 1,584.9 tps under `strict` (medians of three,
spreads 1.5 % and 3.2 %), against 1,519.0 and 864.2 tps in the
comparator (cross-version, +25.3 % and +83.4 %), and `cores = 1` is
652.0 tps (`group`, +21.3 %) and 583.1 tps (`strict`, +20.8 %).
What did **not** move is the single durable client: one booker is
374.0 tps (`group`) and 382.0 tps (`strict`), the same as the comparator's
378.2 and 377.7 tps, with the commit fsync 62 % of every booking. The
invariant check still fails under the default isolation (13 to 34 of 400)
and passes 400 of 400 under `--isolation repeatable-read`, as at the
comparator.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then
`--load-only`, then the run on the same server, with the 2026-09-30
rebaseline's arguments and cell order. Measured by `ck-tester`; the
operator lifted the time budget for this run. Comparator (cross-version):
`bench/v3.0.0/results-scenario2-freight-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**: from
`results-scenario2-freight-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`,
measured 2026-09-30, **14 days before this run**).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-13 23:38 UTC to 2026-10-14 00:48 UTC. Runs 1 and 2 of the four cells ran 23:38 to 23:41; run 3 was delayed by another session's builds and `ctest` and its cells ran 23:58 to 00:08 (three of them re-run, see section 9); the isolation probe 23:52 to 23:53; the matched single-booker cells 00:44 to 00:48. Per-cell windows: section 9 |
| Worktree / branch | `bench-v3.1.0-scenarios` / `worktree-bench-v3.1.0-scenarios` |
| HEAD | `cd7c7c4e`, `git describe --tags` = `v3.1.0-30-gcd7c7c4e` ("Merge sysview-strict-compare: a catalog view's WHERE takes the relation's literal rule, compared under each column's declared type"), committed 2026-10-13 12:24:08 UTC |
| Tree cleanliness | Clean at the start (`git status` empty); nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts (the one exception is stated where it applies). The results files under `bench/v3.1.0/` are uncommitted additions |
| Binary provenance | `build-release/kds_server` built from this tree at HEAD (`cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-O3 -DNDEBUG`), finished 2026-10-13 23:26:41 UTC, 14 hours after the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/kds_server` before the first cell; **copy sha256 `4752f56285aba5e7b1adbd881facc8a7a7dadfab6269b468d30f0b29ce53befa`** (equal to the source binary's), source binary mtime 2026-10-13 23:26:41.5 UTC. Every server started from the copy; no build ran during any reported cell |
| Device | data files under `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server, in scenario 4), deleted after |
| Build type | Release (`CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`) |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM: other sessions build and run `ctest` on it |
| Time budget | **Lifted by the operator for this run** (order of 2026-10-13: "this work is an exception to the time-limit rule"); the rebaseline's arguments and repeat counts ran in full, no cell was cut |
| Prior run of this shape | the 2026-09-30 rebaseline at `9a0525d` (`v2.7.0-531-g9a0525d`, "AY closed"), file held at `36449d28` (`git show 36449d28:bench/v3.0.0/results-scenario*-v2.7.0-531-g9a0525d.md`). **495 commits and 176 first-parent merges apart** (`git rev-list --count 9a0525d..HEAD`), with 256 files changed under `src/`, `include/` and `CMakeLists.txt`, so **no delta below is attributable to one change** |
| Server config | `log_level = warn`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key and ran without it), `cores` and `durability` per cell, all else default. Default `isolation` (read-committed; the driver reports `server default`) except the probe in section 6. Ports 15604 to 15607, never 15432 |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`/proc/loadavg` and the process list are in each cell's precheck, tables below). **A 2-second monitor ran through the whole run** (`monitor.log`) and a cell with a build/test process or a foreign process at 50 % CPU or more inside its window was **discarded and re-run** (up to 8 attempts, 8 happened, `contaminated/` in the archive). Foreign `claude` processes at 10 to 30 % of one CPU were present in many windows and are counted as minor, not excluded. The monitor's limit: it samples every 2 to 3 s, so a shorter burst is not seen |
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, one booking connection, `results-scenario2-freight-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

The four cells of the comparator's matrix, **three runs each (twelve
cells)**, plus two `--isolation repeatable-read` probe cells (one run each),
plus six extra cells that are not in the comparator's matrix: `--bookers 1`
at `cores = 1`, `group` and `strict`, three runs each, because the PostgreSQL
twin drives one booking connection and the 8-booker cells have no matched
PostgreSQL counterpart (section 8). Arguments: `--organizations 300 --ships 30
--operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1
--sync`, with `--txn`, `--contend` and `--manifest` at their defaults (on) and
`--capacity-mode cached`. A run is 3,000 committed bookings, 375 per booker,
contended on 6 hot routes; the eight relations hold 24,644 to 24,664 rows at
the end of a run.

Cell order within a run: `c8-s`, `c1-s`, `c1-g`, `c8-g` (the comparator's
order). Commands (`run_all3.sh` in the archive; `$P` is the run prefix):

    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --schema-only
    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --load-only
    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --json <cell>.json

Rule 9's 200 / 1K / 10K sweep is **not executed** for this driver, as in the
comparator: the measured unit (an eight-statement booking on a pk-addressed
voyage and customer) does not scale with the row count of the ledgers it
appends to, and the mapping from flags to rows is fixed (`--organizations 300`,
`--cargos 4000`, `--bookings 3000` give the 24.6 k rows above). Rows loaded
per run are in the driver's JSON `meta.loaded`.

Load and build check per cell: section 9. **No reported cell has a build or
test process in its window.**

## 3. Throughput

Eight contended bookers: throughput is up in every cell, and the one that
moved most is the one the comparator could not read. `strict` at eight cores
is 1,584.9 tps with a 3.2 % spread, where the comparator's median
(864.2 tps) carried a 42.0 % spread and "is the least reliable number in
this file".

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | 9a0525d median (cross-version) | 9a0525d spread | delta of medians (cross-version) |
|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 663.2 tps | 652.0 tps | 648.7 tps | **652.0 tps** | 2.2 % | 537.6 tps | 1.1 % | +21.3 % |
| `s2-c8-g` | 8 | group | 1,875.8 tps | 1,902.7 tps | 1,903.2 tps | **1,902.7 tps** | 1.5 % | 1,519.0 tps | 4.5 % | +25.3 % |
| `s2-c1-s` | 1 | strict | 598.2 tps | 553.6 tps | 583.1 tps | **583.1 tps** | 8.0 % | 482.5 tps | 4.3 % | +20.8 % |
| `s2-c8-s` | 8 | strict | 1,584.9 tps | 1,616.5 tps | 1,567.0 tps | **1,584.9 tps** | 3.2 % | 864.2 tps | 42.0 % | +83.4 % |

| ratio of medians | now | 9a0525d (cross-version) |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | 2.92x | 2.83x |
| `cores = 8` over `cores = 1`, `strict` | 2.72x | 1.79x |
| `group` over `strict`, `cores = 1` | 1.12x | 1.11x |
| `group` over `strict`, `cores = 8` | 1.20x | 1.76x |

Every cell committed its 3,000 bookings (`abandoned` bookings: section 5).
**Cores help as they did, and durability now costs little extra at eight
cores**: `group` over `strict` fell from 1.76x to 1.20x. At eight cores `strict`
runs 83 % of `group`'s rate; the comparator had it at 57 %.

## 4. Percentiles

The booking (the eight-statement transaction, client-perceived) and its
constituent phases. `ops` is the phase's count; `booking` exceeds 3,000
because a rejected or conflicted attempt opens and closes a span. Run 1 in full:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 3,350 ops | 464.2 µs | 8,742.0 µs | 10,306.6 µs | 18,277.9 µs | 24,691.4 µs | 35,659.6 µs | 0 errors |
| `s2-c1-g` | commit | 3,000 ops | 1,030.0 µs | 1,256.4 µs | 1,371.1 µs | 3,602.7 µs | 7,027.7 µs | 18,408.9 µs | 0 errors |
| `s2-c1-g` | freight-insert | 3,000 ops | 46.8 µs | 233.4 µs | 1,038.5 µs | 2,001.2 µs | 5,002.8 µs | 17,090.2 µs | 0 errors |
| `s2-c1-g` | charge-insert | 16,913 ops | 38.3 µs | 138.7 µs | 196.5 µs | 1,504.7 µs | 3,854.1 µs | 17,208.8 µs | 0 errors |
| `s2-c1-g` | operation-update | 3,000 ops | 49.0 µs | 129.4 µs | 166.1 µs | 1,386.5 µs | 3,273.8 µs | 15,886.2 µs | 0 errors |
| `s2-c1-g` | org-update | 3,000 ops | 45.9 µs | 128.1 µs | 163.2 µs | 1,406.6 µs | 2,804.5 µs | 15,890.0 µs | 0 errors |
| `s2-c1-g` | cargo-lookup | 3,350 ops | 76.7 µs | 238.9 µs | 320.4 µs | 1,579.3 µs | 4,326.5 µs | 10,498.8 µs | 0 errors |
| `s2-c1-g` | credit-lookup | 3,350 ops | 54.1 µs | 205.8 µs | 283.9 µs | 1,564.7 µs | 4,281.0 µs | 11,091.8 µs | 0 errors |
| `s2-c1-g` | capacity-read | 3,350 ops | 62.1 µs | 192.8 µs | 288.2 µs | 1,677.8 µs | 4,303.6 µs | 17,230.7 µs | 0 errors |
| `s2-c1-g` | recipe-read | 3,350 ops | 156.9 µs | 312.4 µs | 418.1 µs | 1,742.9 µs | 4,000.8 µs | 12,161.7 µs | 0 errors |
| `s2-c1-g` | manifest-scan | 100 ops | 93.2 µs | 253.1 µs | 488.6 µs | 1,702.5 µs | 2,959.4 µs | 3,802.3 µs | 0 errors |
| `s2-c8-g` | booking | 3,350 ops | 484.6 µs | 3,126.4 µs | 3,486.2 µs | 7,141.6 µs | 11,175.1 µs | 23,555.0 µs | 0 errors |
| `s2-c8-g` | commit | 3,000 ops | 1,100.8 µs | 1,730.4 µs | 2,030.3 µs | 6,154.7 µs | 9,864.7 µs | 18,720.8 µs | 0 errors |
| `s2-c8-g` | freight-insert | 3,000 ops | 50.3 µs | 77.7 µs | 87.5 µs | 201.5 µs | 354.9 µs | 1,886.5 µs | 0 errors |
| `s2-c8-g` | charge-insert | 16,919 ops | 36.6 µs | 61.9 µs | 73.1 µs | 137.3 µs | 203.6 µs | 3,557.2 µs | 0 errors |
| `s2-c8-g` | operation-update | 3,000 ops | 47.5 µs | 72.4 µs | 81.8 µs | 149.3 µs | 235.0 µs | 1,592.9 µs | 0 errors |
| `s2-c8-g` | org-update | 3,000 ops | 44.0 µs | 68.1 µs | 78.5 µs | 149.4 µs | 217.0 µs | 545.7 µs | 0 errors |
| `s2-c8-g` | cargo-lookup | 3,350 ops | 72.8 µs | 118.4 µs | 129.5 µs | 198.1 µs | 268.0 µs | 2,138.4 µs | 0 errors |
| `s2-c8-g` | credit-lookup | 3,350 ops | 63.1 µs | 102.5 µs | 115.6 µs | 189.3 µs | 258.4 µs | 2,059.9 µs | 0 errors |
| `s2-c8-g` | capacity-read | 3,350 ops | 61.3 µs | 98.7 µs | 111.4 µs | 188.4 µs | 286.4 µs | 2,302.1 µs | 0 errors |
| `s2-c8-g` | recipe-read | 3,350 ops | 156.7 µs | 223.3 µs | 263.0 µs | 384.1 µs | 531.9 µs | 2,370.5 µs | 0 errors |
| `s2-c8-g` | manifest-scan | 40 ops | 82.8 µs | 123.4 µs | 216.3 µs | 421.5 µs | 805.9 µs | 805.9 µs | 0 errors |
| `s2-c1-s` | booking | 3,327 ops | 502.7 µs | 9,101.6 µs | 11,381.7 µs | 20,791.2 µs | 29,169.9 µs | 44,031.7 µs | 0 errors |
| `s2-c1-s` | commit | 3,000 ops | 985.1 µs | 1,147.0 µs | 1,277.2 µs | 3,940.0 µs | 8,587.4 µs | 31,358.8 µs | 0 errors |
| `s2-c1-s` | freight-insert | 3,000 ops | 56.3 µs | 234.2 µs | 1,145.0 µs | 3,177.4 µs | 5,974.3 µs | 31,398.4 µs | 0 errors |
| `s2-c1-s` | charge-insert | 16,918 ops | 41.5 µs | 153.7 µs | 250.5 µs | 2,406.3 µs | 4,972.7 µs | 31,476.6 µs | 0 errors |
| `s2-c1-s` | operation-update | 3,000 ops | 51.9 µs | 140.0 µs | 200.7 µs | 2,267.6 µs | 4,446.6 µs | 17,330.4 µs | 0 errors |
| `s2-c1-s` | org-update | 3,000 ops | 47.1 µs | 131.0 µs | 185.4 µs | 2,223.6 µs | 3,855.9 µs | 17,320.5 µs | 0 errors |
| `s2-c1-s` | cargo-lookup | 3,327 ops | 72.1 µs | 165.7 µs | 208.7 µs | 2,069.2 µs | 3,497.8 µs | 17,369.2 µs | 0 errors |
| `s2-c1-s` | credit-lookup | 3,327 ops | 64.3 µs | 145.9 µs | 237.8 µs | 2,274.3 µs | 4,125.2 µs | 14,348.2 µs | 0 errors |
| `s2-c1-s` | capacity-read | 3,327 ops | 62.2 µs | 147.3 µs | 237.2 µs | 2,325.6 µs | 4,550.4 µs | 17,093.9 µs | 0 errors |
| `s2-c1-s` | recipe-read | 3,327 ops | 157.2 µs | 278.4 µs | 380.1 µs | 2,569.2 µs | 5,248.8 µs | 30,363.8 µs | 0 errors |
| `s2-c1-s` | manifest-scan | 118 ops | 85.5 µs | 268.4 µs | 453.3 µs | 3,441.6 µs | 9,815.6 µs | 17,692.8 µs | 0 errors |
| `s2-c8-s` | booking | 3,355 ops | 470.7 µs | 3,501.2 µs | 4,075.9 µs | 8,092.4 µs | 13,264.0 µs | 53,201.2 µs | 0 errors |
| `s2-c8-s` | commit | 3,000 ops | 1,066.2 µs | 1,719.4 µs | 2,098.1 µs | 5,142.8 µs | 10,712.0 µs | 50,491.0 µs | 0 errors |
| `s2-c8-s` | freight-insert | 3,090 ops | 55.3 µs | 82.6 µs | 97.5 µs | 311.7 µs | 433.2 µs | 1,567.3 µs | 0 errors |
| `s2-c8-s` | charge-insert | 17,393 ops | 37.9 µs | 64.8 µs | 77.4 µs | 154.2 µs | 276.5 µs | 3,821.3 µs | 0 errors |
| `s2-c8-s` | operation-update | 3,090 ops | 42.8 µs | 73.1 µs | 84.0 µs | 151.2 µs | 351.0 µs | 12,608.6 µs | 14 errors |
| `s2-c8-s` | org-update | 3,076 ops | 44.5 µs | 68.5 µs | 78.6 µs | 145.8 µs | 531.5 µs | 6,215.0 µs | 76 errors |
| `s2-c8-s` | cargo-lookup | 3,445 ops | 69.4 µs | 125.5 µs | 138.5 µs | 229.1 µs | 296.8 µs | 1,773.7 µs | 0 errors |
| `s2-c8-s` | credit-lookup | 3,445 ops | 62.3 µs | 109.3 µs | 125.3 µs | 234.9 µs | 330.9 µs | 1,386.9 µs | 0 errors |
| `s2-c8-s` | capacity-read | 3,445 ops | 60.9 µs | 105.5 µs | 122.4 µs | 255.9 µs | 441.6 µs | 1,664.1 µs | 0 errors |
| `s2-c8-s` | recipe-read | 3,445 ops | 160.4 µs | 256.8 µs | 292.0 µs | 524.6 µs | 658.6 µs | 2,253.2 µs | 0 errors |
| `s2-c8-s` | manifest-scan | 40 ops | 66.4 µs | 103.3 µs | 169.5 µs | 398.6 µs | 433.5 µs | 433.5 µs | 0 errors |

Runs 2 and 3, booking, commit and the two appends only (the rest is in the
archive's JSON):

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `r2-s2-c1-g` | booking | 3,364 ops | 673.9 µs | 8,851.6 µs | 10,440.0 µs | 17,930.0 µs | 24,417.7 µs | 34,996.4 µs | 0 errors |
| `r2-s2-c1-g` | commit | 3,000 ops | 1,044.5 µs | 1,261.1 µs | 1,381.0 µs | 3,766.7 µs | 6,955.5 µs | 23,616.7 µs | 0 errors |
| `r2-s2-c1-g` | freight-insert | 3,000 ops | 60.5 µs | 233.2 µs | 1,051.1 µs | 2,118.5 µs | 4,894.4 µs | 13,485.5 µs | 0 errors |
| `r2-s2-c1-g` | charge-insert | 16,922 ops | 39.5 µs | 139.6 µs | 196.5 µs | 1,569.1 µs | 4,185.7 µs | 23,736.8 µs | 0 errors |
| `r2-s2-c8-g` | booking | 3,352 ops | 505.4 µs | 3,138.8 µs | 3,427.9 µs | 6,406.7 µs | 11,871.4 µs | 23,598.9 µs | 0 errors |
| `r2-s2-c8-g` | commit | 3,000 ops | 1,077.6 µs | 1,706.6 µs | 1,999.0 µs | 5,121.8 µs | 10,465.4 µs | 15,281.1 µs | 0 errors |
| `r2-s2-c8-g` | freight-insert | 3,000 ops | 53.2 µs | 77.6 µs | 87.1 µs | 195.0 µs | 349.4 µs | 704.4 µs | 0 errors |
| `r2-s2-c8-g` | charge-insert | 16,918 ops | 37.2 µs | 61.5 µs | 72.0 µs | 135.6 µs | 206.6 µs | 6,346.4 µs | 0 errors |
| `r2-s2-c1-s` | booking | 3,326 ops | 460.5 µs | 9,633.7 µs | 12,051.8 µs | 24,245.6 µs | 33,036.9 µs | 50,113.5 µs | 0 errors |
| `r2-s2-c1-s` | commit | 3,000 ops | 952.9 µs | 1,179.5 µs | 1,327.1 µs | 4,707.0 µs | 9,695.0 µs | 21,135.5 µs | 0 errors |
| `r2-s2-c1-s` | freight-insert | 3,000 ops | 51.0 µs | 246.7 µs | 1,199.5 µs | 3,650.7 µs | 9,154.3 µs | 21,094.1 µs | 0 errors |
| `r2-s2-c1-s` | charge-insert | 16,920 ops | 38.8 µs | 156.3 µs | 255.7 µs | 2,752.1 µs | 5,874.6 µs | 21,186.3 µs | 0 errors |
| `r2-s2-c8-s` | booking | 3,353 ops | 463.7 µs | 3,560.1 µs | 4,071.4 µs | 7,737.9 µs | 12,114.9 µs | 21,022.7 µs | 0 errors |
| `r2-s2-c8-s` | commit | 3,000 ops | 1,011.4 µs | 1,838.5 µs | 2,171.6 µs | 5,317.5 µs | 9,889.3 µs | 14,108.3 µs | 0 errors |
| `r2-s2-c8-s` | freight-insert | 3,063 ops | 42.1 µs | 82.4 µs | 94.9 µs | 299.8 µs | 432.2 µs | 1,440.1 µs | 0 errors |
| `r2-s2-c8-s` | charge-insert | 17,290 ops | 37.4 µs | 64.7 µs | 75.6 µs | 150.0 µs | 277.2 µs | 2,055.4 µs | 0 errors |

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `r3-s2-c1-g` | booking | 3,334 ops | 483.0 µs | 9,055.5 µs | 10,678.4 µs | 18,410.3 µs | 23,870.5 µs | 30,390.3 µs | 0 errors |
| `r3-s2-c1-g` | commit | 3,000 ops | 1,108.8 µs | 1,307.9 µs | 1,424.6 µs | 3,748.4 µs | 6,729.1 µs | 15,820.5 µs | 0 errors |
| `r3-s2-c1-g` | freight-insert | 3,000 ops | 49.2 µs | 235.1 µs | 1,083.8 µs | 2,181.9 µs | 5,069.0 µs | 14,259.0 µs | 0 errors |
| `r3-s2-c1-g` | charge-insert | 16,921 ops | 36.1 µs | 138.3 µs | 196.3 µs | 1,550.3 µs | 3,606.0 µs | 14,482.4 µs | 0 errors |
| `r3-s2-c8-g` | booking | 3,354 ops | 501.1 µs | 3,196.2 µs | 3,514.2 µs | 6,438.1 µs | 9,648.0 µs | 20,359.8 µs | 0 errors |
| `r3-s2-c8-g` | commit | 3,000 ops | 1,056.3 µs | 1,749.8 µs | 2,055.9 µs | 5,086.3 µs | 8,309.8 µs | 15,503.1 µs | 0 errors |
| `r3-s2-c8-g` | freight-insert | 3,002 ops | 52.2 µs | 77.8 µs | 87.8 µs | 213.4 µs | 365.3 µs | 1,616.9 µs | 0 errors |
| `r3-s2-c8-g` | charge-insert | 16,932 ops | 36.4 µs | 61.9 µs | 72.5 µs | 138.4 µs | 216.6 µs | 3,445.7 µs | 0 errors |
| `r3-s2-c1-s` | booking | 3,310 ops | 472.3 µs | 9,403.7 µs | 11,671.4 µs | 21,411.6 µs | 27,859.3 µs | 44,405.6 µs | 0 errors |
| `r3-s2-c1-s` | commit | 3,000 ops | 988.5 µs | 1,185.4 µs | 1,316.2 µs | 4,030.7 µs | 9,098.8 µs | 17,738.2 µs | 0 errors |
| `r3-s2-c1-s` | freight-insert | 3,000 ops | 41.4 µs | 235.6 µs | 1,196.3 µs | 3,342.9 µs | 6,444.3 µs | 19,335.2 µs | 0 errors |
| `r3-s2-c1-s` | charge-insert | 16,921 ops | 36.3 µs | 152.7 µs | 250.8 µs | 2,514.8 µs | 5,077.4 µs | 17,766.5 µs | 0 errors |
| `r3-s2-c8-s` | booking | 3,328 ops | 493.0 µs | 3,631.5 µs | 4,158.0 µs | 8,331.3 µs | 14,482.3 µs | 25,772.7 µs | 0 errors |
| `r3-s2-c8-s` | commit | 3,000 ops | 1,022.9 µs | 1,795.4 µs | 2,180.4 µs | 5,447.7 µs | 12,576.4 µs | 17,995.0 µs | 0 errors |
| `r3-s2-c8-s` | freight-insert | 3,045 ops | 57.4 µs | 82.4 µs | 95.5 µs | 308.2 µs | 422.2 µs | 3,285.4 µs | 0 errors |
| `r3-s2-c8-s` | charge-insert | 17,170 ops | 33.7 µs | 64.5 µs | 76.8 µs | 150.4 µs | 279.2 µs | 2,007.5 µs | 0 errors |

**One core queues; eight cores mostly do not.** Statement p0 is the same
at both core counts (`cargo-lookup` 76.7 µs at `s2-c1-g`, 72.8 µs at
`s2-c8-g`; `freight-insert` 46.8 and 50.3 µs) but p50 falls going from
one core to eight: `booking` 2.96x
(10,306.6 to 3,486.2 µs), the four reads 1.59x to 2.59x,
`charge-insert` 2.69x and **`freight-insert` 11.9x**
(1,038.5 to 87.5 µs). The difference at equal p0 is the wait behind
seven other sessions on the one reactor, not the statement. `commit` goes
the other way: p50 1,371.1 µs at one core, 2,030.3 µs at eight under `group`. Why
`freight-insert`, the first write of a booking, falls furthest is not
explained by this run (the comparator said the same of its 10.0x).

## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN,
refused attempts, the client and the socket). Each phase's mean times its ops
over the booking's mean times its ops, run 1, now and at the comparator:

| cell | cargo-lookup | credit-lookup | capacity-read | recipe-read | freight-insert | charge-insert | operation-update | org-update | commit | unattributed (BEGIN, refused attempts, client, socket) | booking mean |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` (now) | 7.4 % | 7.0 % | 7.2 % | 8.3 % | 8.3 % | 29.8 % | 4.3 % | 4.3 % | 15.8 % | 7.6 % | 10,662 µs |
| `s2-c1-g` (9a0525d) | 7.6 % | 7.3 % | 7.3 % | 8.4 % | 7.9 % | 29.6 % | 4.6 % | 4.4 % | 16.0 % | 7.1 % | 13,109 µs |
| `s2-c8-g` (now) | 3.7 % | 3.3 % | 3.3 % | 7.3 % | 2.5 % | 11.1 % | 2.2 % | 2.1 % | 60.5 % | 4.0 % | 3,699 µs |
| `s2-c8-g` (9a0525d) | 3.3 % | 3.1 % | 3.0 % | 6.8 % | 3.2 % | 15.9 % | 2.6 % | 2.9 % | 55.4 % | 3.8 % | 4,645 µs |
| `s2-c1-s` (now) | 5.0 % | 5.8 % | 6.1 % | 7.9 % | 9.7 % | 34.4 % | 5.0 % | 4.9 % | 14.3 % | 6.9 % | 11,870 µs |
| `s2-c1-s` (9a0525d) | 5.4 % | 6.2 % | 6.3 % | 7.6 % | 8.9 % | 34.2 % | 5.6 % | 5.1 % | 13.7 % | 6.9 % | 14,718 µs |
| `s2-c8-s` (now) | 3.5 % | 3.3 % | 3.3 % | 7.3 % | 2.7 % | 10.3 % | 2.3 % | 2.1 % | 50.7 % | 14.5 % | 4,416 µs |
| `s2-c8-s` (9a0525d) | 3.8 % | 3.7 % | 3.7 % | 5.6 % | 5.9 % | 13.4 % | 2.9 % | 3.2 % | 52.3 % | 5.5 % | 6,491 µs |

| Wait | Reading |
|---|---|
| Durability / commit | `commit` is 15.8 % of a booking at `s2-c1-g`, **60.5 % at `s2-c8-g`** and 50.7 % at `s2-c8-s` (comparator 55.4 % and 52.3 %). At eight cores the commit, not the statements, bounds the booking: its p50 rises 1.48x going from one core to eight while every statement's p50 falls. With one booker (section 8) the commit is 62 % and its p50 1,349.8 µs equals PostgreSQL's 1,345.4 µs, which is the device's sync. A `group` and a `strict` commit now cost the same at eight cores (p50 2,030.3 and 2,098.1 µs) |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: 29.8 % for `charge-insert` alone (about 5.6 charges per booking) and 8.3 / 4.3 / 4.3 % for the three others; eight cores 11.1 / 2.5 / 2.2 / 2.1 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.4 to 8.3 % each at one core, 3.7 to 7.3 % at eight; `recipe-read` is the FilterScan and the largest |
| Lock or conflict wait | zero client-visible conflicts at any `cores = 1` cell and at `c8-g` 0 / 0 / 2 (the two in run 3 are held rows); **90 / 63 / 45 at `c8-s`** (`TXN_CONFLICT retryable=1 row id=... was written by transaction ...`, all on `operations` and `organizations` rows; comparator 125 / 50 / 77), all retried by the driver. The server log also records conflict lines that never reached the driver (table below): a statement that parks is re-run after the holder decides. Their time is inside the phase that met them and is not separable. The `unattributed` column of `s2-c8-s` (14.5 % against 5.5 % in the comparator) is where refused attempts land |
| Client and socket round trip | the unattributed column at the cells with no conflicts, 4.0 to 6.9 % |

Conflict lines in the server log (`TXN_CONFLICT`, split into held-row and
btree-descent lines). The btree re-descent refusal of a `freights` or
`charges` insert (`gave up after 5 attempts`) appears only at `cores = 8`, 2 to 6
times per cell, and **reached the driver in none of them** (no `freight-insert`
or `charge-insert` error in any of the twelve cells, and the driver counts a refused
charge insert as a conflict, so it would show). This is consistent with BA-S14 (a statement that wrote
nothing before the clustered tree refused it is run again after a yield), which this run did not
isolate; the server still logs the refusal. In the comparator the refusal reached the driver
three times in two of three `c8-g` runs.

| cell | run 1 | run 2 | run 3 |
|---|---|---|---|
| `s2-c1-g` | 5 conflict lines (5 row, 0 descent) | 6 conflict lines (6 row, 0 descent) | 7 conflict lines (7 row, 0 descent) |
| `s2-c8-g` | 11 conflict lines (9 row, 4 descent) | 14 conflict lines (12 row, 4 descent) | 17 conflict lines (14 row, 6 descent) |
| `s2-c1-s` | 9 conflict lines (9 row, 0 descent) | 9 conflict lines (9 row, 0 descent) | 10 conflict lines (10 row, 0 descent) |
| `s2-c8-s` | 154 conflict lines (151 row, 4 descent) | 132 conflict lines (131 row, 2 descent) | 117 conflict lines (117 row, 0 descent) |

Outcomes, conflicts and the invariant check, every cell:

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks | error replies (all phases) | exit |
|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | 3,000 bookings | 295 bookings | 55 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 28 / 400 checks | 0 replies | exit 0 |
| `s2-c8-g` | 1 | 3,000 bookings | 294 bookings | 56 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 13 / 400 checks | 0 replies | exit 0 |
| `s2-c1-s` | 1 | 3,000 bookings | 268 bookings | 59 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 30 / 400 checks | 0 replies | exit 0 |
| `s2-c8-s` | 1 | 3,000 bookings | 299 bookings | 56 bookings | 90 conflicts | 4 bookings | 14 / 76 / 0 / 0 conflicts | 13 / 400 checks | 90 replies | exit 0 |
| `r2-s2-c1-g` | 2 | 3,000 bookings | 309 bookings | 55 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 24 / 400 checks | 0 replies | exit 0 |
| `r2-s2-c8-g` | 2 | 3,000 bookings | 298 bookings | 54 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 22 / 400 checks | 0 replies | exit 0 |
| `r2-s2-c1-s` | 2 | 3,000 bookings | 278 bookings | 48 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 28 / 400 checks | 0 replies | exit 0 |
| `r2-s2-c8-s` | 2 | 3,000 bookings | 297 bookings | 56 bookings | 63 conflicts | 1 bookings | 23 / 40 / 0 / 0 conflicts | 14 / 400 checks | 63 replies | exit 0 |
| `r3-s2-c1-g` | 3 | 3,000 bookings | 291 bookings | 43 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 34 / 400 checks | 0 replies | exit 0 |
| `r3-s2-c8-g` | 3 | 3,000 bookings | 305 bookings | 49 bookings | 2 conflicts | 0 bookings | 1 / 1 / 0 / 0 conflicts | 20 / 400 checks | 2 replies | exit 0 |
| `r3-s2-c1-s` | 3 | 3,000 bookings | 258 bookings | 52 bookings | 0 conflicts | 0 bookings | 0 / 0 / 0 / 0 conflicts | 32 / 400 checks | 0 replies | exit 0 |
| `r3-s2-c8-s` | 3 | 3,000 bookings | 278 bookings | 50 bookings | 45 conflicts | 0 bookings | 27 / 18 / 0 / 0 conflicts | 15 / 400 checks | 45 replies | exit 0 |

## 6. Correctness: what the invariant failures are

`--verify 100` runs 400 invariant checks per cell (I1 booked capacity against
the sum of freight volumes, I2 within ship capacity, I3 outstanding against
recomputed charges, I4 charge rows against rules). All twelve default-isolation
cells report failures, 13 to 34 of 400, **including at `cores = 1` where no two
statements execute at once**. The driver writes `SET booked_cbm = <literal>`
from a value it read earlier in the same transaction and says in its source
that this "is exactly the lost-update shape": two bookers that read one voyage
before either commits both write `read + their cbm`, and the second overwrites
the first. The probe cells run the same workload under `--isolation
repeatable-read`:

| cell | isolation | tps | committed | conflicted (retried) | axes (op / org) | verify failures / checks |
|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | repeatable-read | 639.4 tps | 3,000 bookings | 112 conflicts | 54 / 58 conflicts | **0 / 400 checks** |
| `s2-c1-g` (median of 3) | server default (read-committed) | 652.0 tps | 3,000 bookings | 0, 0, 0 conflicts | - | 28, 24, 34 / 400 checks |
| `rr-s2-c8-g` | repeatable-read | 1,805.2 tps | 3,000 bookings | 58 conflicts | 37 / 21 conflicts | **0 / 400 checks** |
| `s2-c8-g` (median of 3) | server default (read-committed) | 1,902.7 tps | 3,000 bookings | 0, 0, 2 conflicts | - | 13, 22, 20 / 400 checks |

Percentiles of the probe cells:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | booking | 3,353 ops | 913.0 µs | 8,857.7 µs | 10,525.6 µs | 19,874.7 µs | 25,924.7 µs | 35,090.8 µs | 0 errors |
| `rr-s2-c1-g` | commit | 3,000 ops | 1,098.9 µs | 1,284.9 µs | 1,415.3 µs | 3,771.1 µs | 7,168.7 µs | 17,898.0 µs | 0 errors |
| `rr-s2-c1-g` | operation-update | 3,112 ops | 58.0 µs | 133.0 µs | 170.9 µs | 1,488.6 µs | 3,724.3 µs | 11,267.4 µs | 54 errors |
| `rr-s2-c1-g` | org-update | 3,058 ops | 55.4 µs | 129.6 µs | 167.3 µs | 1,505.0 µs | 2,906.9 µs | 11,262.1 µs | 58 errors |
| `rr-s2-c8-g` | booking | 3,340 ops | 487.2 µs | 3,185.9 µs | 3,522.9 µs | 7,452.5 µs | 13,815.2 µs | 19,906.6 µs | 0 errors |
| `rr-s2-c8-g` | commit | 3,000 ops | 1,065.3 µs | 1,734.0 µs | 2,047.9 µs | 5,967.7 µs | 12,163.3 µs | 18,270.3 µs | 0 errors |
| `rr-s2-c8-g` | operation-update | 3,058 ops | 48.9 µs | 73.1 µs | 83.3 µs | 152.2 µs | 214.1 µs | 1,476.7 µs | 37 errors |
| `rr-s2-c8-g` | org-update | 3,021 ops | 44.5 µs | 68.8 µs | 80.1 µs | 153.2 µs | 280.2 µs | 1,274.6 µs | 21 errors |

**Finding.** The failures are the read-committed lost update of a client-side
read-modify-write, not corruption of the engine: under repeatable-read the
engine refuses the second writer of a row (`TXN_CONFLICT`, 112 at one core,
58 at eight), the driver retries, and the check passes 400 of 400 in both probes.
The price of the correct answer is -1.9 % at one core (639.4 against 652.0 tps, inside
the 2.2 % run-to-run spread) and **-5.1 % at eight cores** (1,805.2 against 1,902.7 tps, outside the 1.5 %
spread, one probe run). The comparator's probes showed no cost (+0.8 % and +4.0 %);
this run's eight-core probe does, once. `docs/spec/txn.md` owns what
`isolation` guarantees; whether a read-committed default is the wanted default for
a workload of this shape is not a decision this run makes.

## 7. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.0.0/results-scenario2-freight-v2.7.0-531-g9a0525d.md`
(three runs per cell, held at `36449d28`). The comparator's describe string says `v2.7.0` because that was the version of record on 2026-09-30; it is the same architecture series (the AR0 single-WAL engine) and `bench/README.md` names it as the comparator for every later delta of the same driver and shape. The version label changed between the two runs, so every delta in this file is **cross-version** and is labelled so.
**495 commits and 176 first-parent merges separate the two runs**, so no
difference below is attributable to one change. Throughput deltas: section 3.
Statement-level, run 1 of each against run 1 of the comparator:

| cell | phase | p0 now | p0 9a0525d | p50 now | p50 9a0525d | p50 ratio now/then | mean now | mean 9a0525d |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | booking | 464.2 µs | 718.3 µs | 10,306.6 µs | 12,869.2 µs | 0.80x | 10,662.0 µs | 13,109.0 µs |
| `s2-c1-g` | commit | 1,030.0 µs | 1,326.9 µs | 1,371.1 µs | 1,762.6 µs | 0.78x | 1,883.6 µs | 2,335.7 µs |
| `s2-c1-g` | freight-insert | 46.8 µs | 64.3 µs | 1,038.5 µs | 1,321.9 µs | 0.79x | 993.3 µs | 1,149.2 µs |
| `s2-c1-g` | charge-insert | 38.3 µs | 44.5 µs | 196.5 µs | 261.1 µs | 0.75x | 628.9 µs | 768.4 µs |
| `s2-c1-g` | operation-update | 49.0 µs | 50.3 µs | 166.1 µs | 210.3 µs | 0.79x | 506.4 µs | 678.8 µs |
| `s2-c1-g` | cargo-lookup | 76.7 µs | 99.8 µs | 320.4 µs | 419.0 µs | 0.76x | 790.6 µs | 992.7 µs |
| `s2-c1-g` | recipe-read | 156.9 µs | 189.7 µs | 418.1 µs | 525.4 µs | 0.80x | 880.7 µs | 1,094.9 µs |
| `s2-c8-g` | booking | 484.6 µs | 571.9 µs | 3,486.2 µs | 4,234.6 µs | 0.82x | 3,699.2 µs | 4,644.9 µs |
| `s2-c8-g` | commit | 1,100.8 µs | 1,149.1 µs | 2,030.3 µs | 2,312.3 µs | 0.88x | 2,498.1 µs | 2,883.0 µs |
| `s2-c8-g` | freight-insert | 50.3 µs | 60.0 µs | 87.5 µs | 132.1 µs | 0.66x | 103.8 µs | 164.3 µs |
| `s2-c8-g` | charge-insert | 36.6 µs | 39.0 µs | 73.1 µs | 123.2 µs | 0.59x | 81.0 µs | 147.0 µs |
| `s2-c8-g` | operation-update | 47.5 µs | 60.8 µs | 81.8 µs | 122.2 µs | 0.67x | 90.6 µs | 136.9 µs |
| `s2-c8-g` | cargo-lookup | 72.8 µs | 80.2 µs | 129.5 µs | 140.4 µs | 0.92x | 137.7 µs | 155.4 µs |
| `s2-c8-g` | recipe-read | 156.7 µs | 177.2 µs | 263.0 µs | 298.8 µs | 0.88x | 270.0 µs | 314.6 µs |
| `s2-c1-s` | booking | 502.7 µs | 509.7 µs | 11,381.7 µs | 14,325.9 µs | 0.79x | 11,870.1 µs | 14,718.5 µs |
| `s2-c1-s` | commit | 985.1 µs | 1,163.8 µs | 1,277.2 µs | 1,634.7 µs | 0.78x | 1,883.2 µs | 2,244.8 µs |
| `s2-c1-s` | freight-insert | 56.3 µs | 62.3 µs | 1,145.0 µs | 1,443.9 µs | 0.79x | 1,282.6 µs | 1,450.8 µs |
| `s2-c1-s` | charge-insert | 41.5 µs | 43.1 µs | 250.5 µs | 318.1 µs | 0.79x | 802.4 µs | 994.5 µs |
| `s2-c1-s` | operation-update | 51.9 µs | 58.3 µs | 200.7 µs | 276.4 µs | 0.73x | 660.9 µs | 915.0 µs |
| `s2-c1-s` | cargo-lookup | 72.1 µs | 79.2 µs | 208.7 µs | 252.8 µs | 0.83x | 595.8 µs | 799.4 µs |
| `s2-c1-s` | recipe-read | 157.2 µs | 171.9 µs | 380.1 µs | 466.7 µs | 0.81x | 932.8 µs | 1,120.0 µs |
| `s2-c8-s` | booking | 470.7 µs | 498.7 µs | 4,075.9 µs | 5,864.2 µs | 0.70x | 4,416.5 µs | 6,491.3 µs |
| `s2-c8-s` | commit | 1,066.2 µs | 1,149.3 µs | 2,098.1 µs | 2,994.1 µs | 0.70x | 2,502.6 µs | 3,754.9 µs |
| `s2-c8-s` | freight-insert | 55.3 µs | 41.2 µs | 97.5 µs | 94.4 µs | 1.03x | 129.6 µs | 407.2 µs |
| `s2-c8-s` | charge-insert | 37.9 µs | 34.2 µs | 77.4 µs | 78.5 µs | 0.99x | 88.0 µs | 163.6 µs |
| `s2-c8-s` | operation-update | 42.8 µs | 44.2 µs | 84.0 µs | 86.3 µs | 0.97x | 111.0 µs | 201.4 µs |
| `s2-c8-s` | cargo-lookup | 69.4 µs | 72.4 µs | 138.5 µs | 134.0 µs | 1.03x | 150.2 µs | 240.8 µs |
| `s2-c8-s` | recipe-read | 160.4 µs | 169.2 µs | 292.0 µs | 268.1 µs | 1.09x | 312.3 µs | 351.7 µs |

- **`cores = 1`: every phase is 17 to 27 % faster at p50 and the floor moved
  with it.** `group`: `booking` 0.80x of the comparator's p50, `commit` 0.78x, `freight-insert`
  0.79x, `cargo-lookup` 0.76x; p0 of `cargo-lookup` 76.7 against 99.8 µs. The reactor queue is
  shorter because the statements are cheaper; this run does not apportion it between
  milestones (BM's scan kernel and BN's read/update path are candidates for the reads
  and updates; BA's commit changes for `commit`).
- **`cores = 8`, `group`: the write statements are 33 to 41 % faster at p50** (`charge-insert`
  0.59x, `freight-insert` 0.66x), the reads 8 to 12 % and `commit` 0.88x.
- **`cores = 8`, `strict`: the write statements did not change** (0.92x to 1.03x) **but
  `commit` and `booking` are 30 % faster** (2,098.1 against 2,994.1 µs): the whole
  gain is in the commit, which is where BA-S7 (a `strict` commit stages its record, parks with
  its marker held and publishes after its sync) acts; this run does not isolate it. The commit is still
  half of a `strict` booking at eight cores (52.3 % in the comparator, 50.7 % now), only shorter.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL stays the floor and answers "is the engine in the right league", not
"is it good". The PG18 twin (`tools/pg_scenario2_freight.py`) drives **one**
booking connection with its manifest reporter interleaved, so the matched KDS
cell is `--bookers 1`; the eight-booker cells above have no PostgreSQL
counterpart and are not compared with it. The PostgreSQL figures are the three
`synchronous_commit = on` runs measured at `f2f1ee7` on 2026-09-30, 14 days
before these cells, on a host that has since run other work; the KDS
`--bookers 1` cells were re-run here on this binary, three runs each, and the
comparator's own `--bookers 1` cells (also reused from that PG18 file) are
shown beside them.

| engine / cell | clients | durability | run 1 | run 2 | run 3 | median | spread | vs PG18 |
|---|---|---|---|---|---|---|---|---|
| PostgreSQL 18.6 `synchronous_commit = on` (reused, `f2f1ee7`, 2026-09-30) | 1 | durable | 391.6 tps | 392.2 tps | 400.9 tps | **392.2 tps** | 2.4 % | 1.00x |
| KDS `cores = 1`, `--bookers 1`, this run | 1 | group | 374.0 tps | 364.8 tps | 374.7 tps | **374.0 tps** | 2.7 % | 0.95x |
| KDS `cores = 1`, `--bookers 1`, this run | 1 | strict | 383.6 tps | 382.0 tps | 375.5 tps | **382.0 tps** | 2.1 % | 0.97x |
| KDS `cores = 1`, `--bookers 1`, 9a0525d binary (reused from the PG18 file) | 1 | group | 378.2 tps | 378.2 tps | 367.1 tps | **378.2 tps** | 3.0 % | 0.96x |
| KDS `cores = 1`, `--bookers 1`, 9a0525d binary (reused from the PG18 file) | 1 | strict | 377.7 tps | 369.2 tps | 382.1 tps | **377.7 tps** | 3.5 % | 0.96x |

Matched, one client, durable: KDS is **0.95x** (`group`) and **0.97x** (`strict`) of
PostgreSQL's rate; the comparator's pair was 0.96x and 0.96x. The KDS cells' own change since
`9a0525d` (-1.1 % and +1.1 %) is inside their spreads (2.7 % and 2.1 %), so **the
single durable client did not change** and PostgreSQL's three runs (spread 2.4 %) are
1.05x of KDS. The reason is in the decomposition: the commit's fsync is 62 % of a PostgreSQL booking and
62 % of a KDS one, and its p50 is the same on both engines. Per-phase p50, medians of three (the
comparator's KDS and the PG18 columns are reused):

| phase | PG18 p50 (reused) | KDS group p50 now | KDS group p50 9a0525d | KDS strict p50 now | KDS strict p50 9a0525d | KDS group mean now | PG18 mean |
|---|---|---|---|---|---|---|---|
| booking | 2,255.3 µs | 2,246.6 µs | 2,307.3 µs | 2,219.0 µs | 2,266.9 µs | 2,352.4 µs | 2,272.6 µs |
| commit | 1,345.4 µs | 1,349.8 µs | 1,325.5 µs | 1,322.6 µs | 1,283.8 µs | 1,645.9 µs | 1,543.0 µs |
| freight-insert | 78.5 µs | 58.6 µs | 63.9 µs | 58.0 µs | 63.9 µs | 64.5 µs | 82.3 µs |
| charge-insert | 51.0 µs | 43.7 µs | 51.5 µs | 43.2 µs | 51.7 µs | 48.8 µs | 54.9 µs |
| operation-update | 70.9 µs | 53.1 µs | 58.7 µs | 52.7 µs | 58.6 µs | 57.6 µs | 73.8 µs |
| org-update | 68.9 µs | 49.2 µs | 56.2 µs | 49.0 µs | 56.4 µs | 53.7 µs | 71.5 µs |
| cargo-lookup | 76.9 µs | 85.0 µs | 88.9 µs | 84.3 µs | 88.8 µs | 89.2 µs | 78.7 µs |
| credit-lookup | 66.6 µs | 67.4 µs | 73.4 µs | 67.3 µs | 73.3 µs | 74.0 µs | 67.8 µs |
| capacity-read | 64.0 µs | 63.6 µs | 69.8 µs | 63.7 µs | 69.6 µs | 70.3 µs | 65.9 µs |
| recipe-read | 88.9 µs | 175.0 µs | 186.8 µs | 174.0 µs | 186.8 µs | 181.0 µs | 91.5 µs |

KDS's write statements are 0.71x to 0.86x of PostgreSQL's p50 (faster), its three pk reads
0.99x to 1.11x, and its `recipe-read` FilterScan 1.97x (the PG twin serves it from an
index it creates; scenario 3's PG file shows the same class of gap). Percentiles
of the matched KDS cells, run 1:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `b1-s2-c1-g` | booking | 3,402 ops | 437.7 µs | 2,119.0 µs | 2,236.5 µs | 4,041.7 µs | 8,409.4 µs | 37,016.1 µs | 0 errors |
| `b1-s2-c1-g` | commit | 3,000 ops | 1,101.9 µs | 1,272.0 µs | 1,349.8 µs | 3,294.9 µs | 7,412.6 µs | 33,977.5 µs | 0 errors |
| `b1-s2-c1-g` | freight-insert | 3,000 ops | 46.6 µs | 56.4 µs | 58.4 µs | 82.2 µs | 103.0 µs | 1,857.6 µs | 0 errors |
| `b1-s2-c1-g` | charge-insert | 16,907 ops | 40.4 µs | 42.1 µs | 43.0 µs | 64.4 µs | 85.4 µs | 1,493.9 µs | 0 errors |
| `b1-s2-c1-g` | operation-update | 3,000 ops | 49.7 µs | 51.5 µs | 52.3 µs | 72.8 µs | 90.3 µs | 1,566.5 µs | 0 errors |
| `b1-s2-c1-g` | org-update | 3,000 ops | 45.5 µs | 47.7 µs | 48.4 µs | 70.1 µs | 84.7 µs | 1,214.9 µs | 0 errors |
| `b1-s2-c1-g` | cargo-lookup | 3,402 ops | 65.8 µs | 82.6 µs | 84.5 µs | 113.0 µs | 131.7 µs | 1,455.8 µs | 0 errors |
| `b1-s2-c1-g` | credit-lookup | 3,402 ops | 51.0 µs | 65.8 µs | 67.3 µs | 98.2 µs | 114.8 µs | 1,433.6 µs | 0 errors |
| `b1-s2-c1-g` | capacity-read | 3,402 ops | 58.5 µs | 61.7 µs | 63.2 µs | 95.1 µs | 118.7 µs | 1,453.4 µs | 0 errors |
| `b1-s2-c1-g` | recipe-read | 3,402 ops | 150.6 µs | 167.8 µs | 173.2 µs | 225.7 µs | 282.0 µs | 1,568.5 µs | 0 errors |
| `b1-s2-c1-g` | manifest-scan | 180 ops | 51.0 µs | 108.9 µs | 183.6 µs | 1,496.3 µs | 2,965.1 µs | 3,971.1 µs | 0 errors |
| `b1-s2-c1-s` | booking | 3,402 ops | 435.0 µs | 2,087.8 µs | 2,197.4 µs | 3,752.0 µs | 7,972.5 µs | 19,528.3 µs | 0 errors |
| `b1-s2-c1-s` | commit | 3,000 ops | 1,079.0 µs | 1,237.3 µs | 1,314.2 µs | 2,944.8 µs | 6,792.4 µs | 18,724.4 µs | 0 errors |
| `b1-s2-c1-s` | freight-insert | 3,000 ops | 50.8 µs | 56.1 µs | 58.0 µs | 78.8 µs | 94.7 µs | 1,350.5 µs | 0 errors |
| `b1-s2-c1-s` | charge-insert | 16,907 ops | 37.5 µs | 41.7 µs | 42.6 µs | 63.2 µs | 82.1 µs | 1,541.6 µs | 0 errors |
| `b1-s2-c1-s` | operation-update | 3,000 ops | 49.4 µs | 51.3 µs | 52.1 µs | 72.7 µs | 87.4 µs | 2,424.7 µs | 0 errors |
| `b1-s2-c1-s` | org-update | 3,000 ops | 44.8 µs | 47.1 µs | 47.9 µs | 69.8 µs | 88.9 µs | 1,285.6 µs | 0 errors |
| `b1-s2-c1-s` | cargo-lookup | 3,402 ops | 66.2 µs | 82.0 µs | 83.9 µs | 107.8 µs | 124.0 µs | 281.2 µs | 0 errors |
| `b1-s2-c1-s` | credit-lookup | 3,402 ops | 54.8 µs | 65.3 µs | 66.8 µs | 95.6 µs | 114.4 µs | 1,481.2 µs | 0 errors |
| `b1-s2-c1-s` | capacity-read | 3,402 ops | 53.8 µs | 61.7 µs | 63.1 µs | 92.0 µs | 108.5 µs | 1,459.5 µs | 0 errors |
| `b1-s2-c1-s` | recipe-read | 3,402 ops | 151.5 µs | 168.2 µs | 173.6 µs | 206.2 µs | 232.9 µs | 1,589.1 µs | 0 errors |
| `b1-s2-c1-s` | manifest-scan | 160 ops | 61.0 µs | 109.6 µs | 172.2 µs | 1,399.5 µs | 3,393.1 µs | 3,451.2 µs | 0 errors |

## 9. Noise floor and host stamps

Three runs per cell: the spread column of section 3 is the floor, 2.2 % (`c1-g`), 1.5 % (`c8-g`),
8.0 % (`c1-s`) and 3.2 % (`c8-s`). A difference below those in the matching cell is not a
result. The `c1-s` spread is the widest of the uncontended cells; the comparator's
42 % `c8-s` spread did not repeat here (three runs within 3.2 %), so the comparator's median for
that cell is the one to distrust. The device thermometer, `load-cargos` p50, is
1.01 to 1.32 ms across all 20 cells (the comparator's was 1.30 to 1.38 ms): no device stall is
indicated (`recovery_checkpoint_us` was not sampled).

**Contamination and re-runs.** Another session's builds and test suite ran on this host
during the measurement. Three of run 3's cells were discarded and re-run: `r3-s2-c1-g` three times (the third
discard had a `ctest` running `kds_tests` in 44 of its 54 monitor samples), `r3-s2-c8-s` twice and
`r3-s2-c1-s` once. The discarded outputs are in `contaminated/` in the archive
(`r3-s2-c1-g.attempt0..2`, `r3-s2-c1-s.attempt0`, `r3-s2-c8-s.attempt0..1`). A cell is discarded for
a build or test process in its window, never for its result.
The first attempt of the whole run did not start: the server refused the comparator's
config because `buffer_pool_frames` is now required (BE), which is why that key is in
section 1's config row (`contaminated/attempt0-no-buffer-pool-key`).

| cell | window UTC | loadavg 1/5/15 at precheck | gate wait | build/test processes at precheck | monitor samples | with build/test | with foreign >= 50 % | with minor 10-50 % |
|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 2026-10-13 23:39:13 to 23:39:31 | 1.93 / 2.36 / 2.94 | 0 s | none | 8 samples | 0 | 0 | 1 |
| `s2-c8-g` | 2026-10-13 23:39:34 to 23:39:50 | 1.66 / 2.27 / 2.90 | 0 s | none | 8 samples | 0 | 0 | 2 |
| `s2-c1-s` | 2026-10-13 23:38:51 to 23:39:10 | 2.44 / 2.48 / 3.00 | 0 s | none | 9 samples | 0 | 0 | 2 |
| `s2-c8-s` | 2026-10-13 23:38:32 to 23:38:48 | 1.62 / 2.35 / 2.96 | 0 s | none | 7 samples | 0 | 0 | 2 |
| `r2-s2-c1-g` | 2026-10-13 23:40:35 to 23:40:53 | 1.25 / 2.05 / 2.78 | 0 s | none | 8 samples | 0 | 0 | 2 |
| `r2-s2-c8-g` | 2026-10-13 23:40:56 to 23:41:11 | 1.24 / 1.99 / 2.75 | 0 s | none | 7 samples | 0 | 0 | 1 |
| `r2-s2-c1-s` | 2026-10-13 23:40:13 to 23:40:32 | 1.36 / 2.12 / 2.82 | 0 s | none | 9 samples | 0 | 0 | 4 |
| `r2-s2-c8-s` | 2026-10-13 23:39:53 to 23:40:10 | 1.61 / 2.22 / 2.87 | 0 s | none | 8 samples | 0 | 0 | 5 |
| `r3-s2-c1-g` | 2026-10-14 00:07:25 to 00:07:44 | 7.05 / 7.50 / 6.32 | 190 s | none | 9 samples | 0 | 0 | 3 |
| `r3-s2-c8-g` | 2026-10-13 23:52:32 to 23:52:48 | 7.25 / 8.26 / 5.97 | 200 s | none | 8 samples | 0 | 0 | 2 |
| `r3-s2-c1-s` | 2026-10-13 23:58:48 to 23:59:08 | 2.22 / 5.19 / 5.37 | 0 s | none | 10 samples | 0 | 0 | 3 |
| `r3-s2-c8-s` | 2026-10-13 23:58:25 to 23:58:41 | 3.09 / 5.59 / 5.51 | 10 s | none | 7 samples | 0 | 0 | 2 |
| `rr-s2-c1-g` | 2026-10-13 23:52:51 to 23:53:09 | 5.94 / 7.92 / 5.90 | 0 s | none | 8 samples | 0 | 0 | 3 |
| `rr-s2-c8-g` | 2026-10-13 23:53:12 to 23:53:29 | 4.60 / 7.49 / 5.80 | 0 s | none | 8 samples | 0 | 0 | 0 |
| `b1-s2-c1-g` | 2026-10-14 00:44:43 to 00:45:07 | 0.08 / 0.33 / 1.54 | 0 s | none | 11 samples | 0 | 0 | 0 |
| `b1-s2-c1-s` | 2026-10-14 00:45:14 to 00:45:38 | 0.37 / 0.38 / 1.51 | 0 s | none | 11 samples | 0 | 0 | 0 |
| `r2-b1-s2-c1-g` | 2026-10-14 00:45:45 to 00:46:10 | 0.76 / 0.49 / 1.51 | 0 s | none | 11 samples | 0 | 0 | 0 |
| `r2-b1-s2-c1-s` | 2026-10-14 00:46:17 to 00:46:41 | 0.78 / 0.52 / 1.49 | 0 s | none | 11 samples | 0 | 0 | 0 |
| `r3-b1-s2-c1-g` | 2026-10-14 00:46:48 to 00:47:12 | 0.72 / 0.54 / 1.46 | 0 s | none | 11 samples | 0 | 0 | 0 |
| `r3-b1-s2-c1-s` | 2026-10-14 00:47:19 to 00:47:43 | 0.90 / 0.60 / 1.45 | 0 s | none | 11 samples | 0 | 0 | 0 |

## 10. What the run says about the engine

1. **The commit path no longer bounds `strict` at eight cores.** `group` over `strict`
   fell from 1.76x to 1.20x and `strict` at eight cores runs 1,584.9 tps; `commit` p50 is 2,098.1
   against 2,030.3 µs (`group`). This is the data point for `docs/spec/wal.md` §8 and §11's
   claim that a `strict` commit parks instead of blocking its reactor.
2. **A single durable client is device-bound and unchanged.** One booker: 374.0 /
   382.0 tps now, 378.2 / 377.7 at the comparator, and the commit fsync is
   62 % of the booking; the statements around it are 4 to 15 % faster at p50 and show
   only as the 38 % that is not commit. The statement side of the one-client path
   can gain at most that share while the commit is 1.35 ms of a 2.24 ms booking (p50).
3. **Cores help, and the host is the next bound.** 2.92x under `group` for
   eight cores; the `cores = 8` cells put eight reactors, eight booker processes and a
   reporter on the host's eight logical CPUs, so how much of the headroom above 2.9x is
   the engine and how much the clients' own CPU is not separable here. No limit was
   found by adding load: the 8-core `group` cell spread is 1.5 % and nothing in it
   saturates but the commit.
4. **Strict still conflicts, less.** `TXN_CONFLICT` replies reached the driver 90 / 63 / 45 times in
   the three `c8-s` runs, against 0 / 0 / 2 at `c8-g` (comparator 125 / 50 / 77 and 0 / 3 / 2): row locks held
   across the longer commit wait remain the likely mechanism; this run does not show it.
5. **The invariant failures are the driver's read-committed lost update** and
   repeatable-read removes them (0 of 400 in both probes); the price at eight cores is
   5.1 % in this run's single probe.

Archive: `bench/v3.1.0/archive/scenario2-v3.1.0-30-gcd7c7c4e/`: per-cell JSON, driver
stdout, load and schema logs, prechecks, server logs, configs, run scripts
(`run_all3.sh`, `lib.sh`), `monitor.log`, the timeline, the binary hash, the
contaminated attempts.
