# BP-S1's census: what the per-core engine's shared structures cost above one core

**Thesis.** Of the five Group B premises of `instructions/v3.0.0/workorder-bp-per-core-residue.md`, one clears
BP-R2's bar and it is the one nobody had priced: **BP-H10, the `64 x cores` lock partitions, at `cores = 8`**. With one range
fence held on the written relation a tuple ask walks 512 latched partitions, and the server's CPU per statement rises from
46.8 to 52.4 us (+12 %); paired by run the rise is +5.4 to +5.7 us in all three runs, and every fence run sits above every plain
run. At `cores = 4` (30.3 us plain, 30.0 us fence, medians) and `cores = 1` (31.2 / 31.5 us) the second pass shows nothing. The
other three measured premises fall under their bars. **BP-H6** (the whole-memo drop on a root
move): a root move costs at most `cores - 1` drops, a drop costs 30 to 97 us on the statement that meets it, and the catalog's
resolve functions are under 1 % of server CPU even at `cores = 4`. **BP-H7** (N checkpoint runs per period): the runs scale
with cores (1 / 2 / 3.9 per 5 s), the pages written per 1,000 rows do not (21.0 to 23.4 on the run log), and cutting the runs 4x moved
neither throughput nor the probe's tail past the repeat spread. **BP-H9** (the in-flight table's miss scan): `PURGE`
asks once per row, each miss reads one id, and the scan is 1 sample of 695 in the profile. **BP-H8** was not measured: the cell
was not run, and `SHOW META` carries no counter for the window's size, its burns or `LookupCommit`. The verdicts drawn before this file
was written stand, with the qualifications listed after the table (the mechanism behind pass 1's `cores = 4` fence cost, the reading
of `PURGE`'s `cores = 4` step, and what the H7 throughput comparison can carry). Two gaps bear on how far they reach: the row-set sweep of
`bench/README.md` rule 9 was not run, and the `cores = 8` cell shares its eight vCPUs, four physical cores, with the client.

| Item | Value |
|---|---|
| Executed | **Pass 1**: 2026-10-12 08:49-12:27 UTC (H6 cells, the PURGE profile, pass-1 H7/H9/H10 cells). **Pass 2**: 2026-10-12 14:31-16:57 UTC (H7, H9, H10; the chain's BP-S3 A/B cells, 14:11-14:31 and 16:59, are not in this file) |
| Branch / worktree | `worktree-bp-per-core-residue`, worktree `bp-per-core-residue` (the file was written there at `d555cb55`, `v2.7.0-920-gd555cb55`, whose working tree has uncommitted changes; **no binary of that tree was measured**) |
| Pass 1 commit | `1592007e`, `git describe --tags` = `v2.7.0-904-g1592007e`, committed 2026-10-12 08:42:17 UTC. BP-S1's instrumented commit |
| Pass 2 commit | `ccf9cfc1`, `git describe --tags ccf9cfc1` in the worktree = **`v2.7.0-909-gccf9cfc1`** (the request for this file said 911; the file is named by what the tool prints), committed 2026-10-12 13:46:17 UTC. BP-S3's head: `676d1d7d` (the BP-S1 counter fixes), `230c4453` (H1-H3), `998903ab` (H4), `ccf9cfc1` (H5) sit between the two passes |
| Tree cleanliness | Both binaries were built from an export of the commit (`v2/src-1592007e`, `v2/src-ccf9cfc1`, tarballs beside them), so the measured tree is clean by construction |
| Binary provenance | Pass 1: `/home/cdkbs/bench-runs/bp-s1/bin/kds_server`, mtime 08:42:54 UTC (after the commit), sha256 `44b7c2e5e2bcd4093b62c83201e65d17bc17afb8add9264e6ed0d5abc22dfd8a`; a rebuild from the export (`v2/build-1592007e`) has the identical hash. Pass 2: `/home/cdkbs/bench-runs/bp-s1/v2/bin/kds_server.b`, copy mtime 14:11:04 UTC (the build 14:09:30, after the commit), sha256 `a6fc3c04ef2bf2e19e3715ca1943c689959c97e9aa59a57a205029da1aea111f`. Every server ran from these copies. The growth, lock, in-flight and CABIN AUTO cells' JSON carry the hash; the checkpoint driver's JSON carries the path and the one-drop probe's nothing, so those two files' hashes were re-read from the copies for this file |
| Build | Release (`CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`), both |
| Device | `/dev/root`, ext4 on `nvme0n1p1` (`df -T`, 258.9 GB, 49-52 % used during pass 2), data files and WAL under `/home/cdkbs/bench-runs/bp-s1/work/` and `.../v2/work/`; **not tmpfs** |
| Machine | 8 vCPU AMD EPYC 9V74 = **4 physical cores x 2 SMT** (`thread_siblings_list`: 0-1, 2-3, 4-5, 6-7), Linux 7.0.0-1014-azure |
| Server config | Per driver, all fresh server and fresh data file per run. Growth, lock, in-flight, CABIN AUTO cells: `buffer_pool_frames = 65536`, `log_level = warn`, `reactor_cpus` pinned, `cores`/`durability` per row. Checkpoint cell: `buffer_pool_frames = 786432`, `checkpoint_interval_ms = 5000`, `log_level = info`, `durability = group`, reactors on CPUs 0..k-1 (the driver's choice). `cores` 1/2/4(/8) |
| Drivers | Unmodified `tools/ba_census.py` (`insertN`), `tools/scenario4_cabinopt_days.py`, `tools/bi_checkpoint_stall.py`, `tools/lock_contention_benchmark.py`. Four new wrappers outside `tools/` start the servers and read `SHOW META`: `lockcell.py` (lock cell with an optional held fence), `inflight_cell.py` (`PURGE` and named-key loops), `cell2.py` (CABIN AUTO with readers), `reresolve.py` (one memo drop's cost). All archived; the exact commands are the schedules `scripts/cell*.sh`, `scripts/prof.sh` (pass 1) and `scripts/pass2-chain.sh` (pass 2); `bench/docs/` stays closed (`bench/README.md`) |
| Raw files | `archive/bp-s1-census-v2.7.0-909-gccf9cfc1/`: `pass1/`, `pass2/`, `scripts/` (every table below is `scripts/bp_s1_tables.py` over those directories) |
| Suite | **Not executed.** This file changes no engine code. The stage commits carry their own suite claims (`676d1d7d`: 3603/3603 Debug, at the stack's head) |

## Verdicts

| Premise | BP-R2 bar | What the census measured | Verdict | Follows |
|---|---|---|---|---|
| **BP-H6** whole-memo drops on word moves | 3 % of server CPU | `<= 2.72e-4` drops per statement (`group`, `cores = 4`), 3.3e-5 at `relaxed`; drops = root moves x (`cores` - 1) at `relaxed` (0.98-1.00), 0.64-0.67 at `group`; 0 structural reruns in 66 cells; a drop costs +30 us (point `SELECT`), +79 us (point `UPDATE`), 0 (pk join), 97 us at worst on a 900-table catalog; resolve functions 0.90 % of server CPU at `cores = 4` (0.63 % at `cores = 1`, where no drop happens) | **Rejected**, 0.08 % worst-case share | BP-Q4 -> (c) keep; BP-S5 not run |
| **BP-H7** N checkpoint runs per period | 5 % throughput or p99 vs the cost removed | runs per 5 s period 1.00 / 1.98 / 3.94 (`cores` 1 / 2 / 4); pages written per 1,000 rows 21.0 -> 22.5-22.9 -> 23.3-23.4 (run log; 22.8 -> 25.5-25.6 -> 26.9-27.0 on the counter), i.e. x1.11-1.18, not xN; 4x fewer runs (cadence 20 s against 5 s, `cores = 4`, 8 repeats each) moved throughput -1.5 % and left the probe's p99 inside the repeat spread (23.8 vs 26.8 ms, spreads 18.0-37.3 and 23.4-35.2) | **Rejected**: not material | BP-Q5 -> (a) keep |
| **BP-H8** the id window's machinery | 5 % of throughput or p99 | **Not measured.** No skew cell was run, and `SHOW META` has no counter for the window's size, burns or `LookupCommit` | **Unmeasured** (BP-Q6 is report only) | BP-S7 reports it as such |
| **BP-H9** the in-flight table's miss scan | 3 % of server CPU | `PURGE` of 100,000 rows: 100,000 asks, 100,000 misses, **1.00 id read per miss** at `cores` 1 and 4; profile: `InFlight` 1 sample of 695 (0.14 %) at `cores = 4`, 0 of 679 at `cores = 1`; the named-key loops ask 0 times on the uncontended path; no cell holds more than about 10 concurrent transactions against the 1,024-slot cap | **Rejected** for the scan; the cap is not approached, and the case it could bind (BL's session skew) was not run | BP-Q7 -> (a) keep |
| **BP-H10** `64 x cores` lock partitions | 3 % of server CPU on any cell | relaxed, one held fence: **+12 %** server CPU per statement at `cores = 8` (46.8 -> 52.4 us, pairs +5.4 to +5.7 us), -0.8 % at `cores = 4`, +1.0 % at `cores = 1`; throughput -3.6 % and p99 +2.9 % on `update-disjoint` at `cores = 8`, **inside the noise floor** | **Held at `cores = 8`, on the CPU leg only** | BP-Q15 -> (a); BP-S6 runs |

**Qualifications to the verdicts drawn before this file.**
1. **Naming.** `ccf9cfc1` describes as `v2.7.0-909-gccf9cfc1` in the worktree, not `-911`.
2. **BP-H10's `cores = 4` figure in pass 1 (+11 %).** It was attributed to the per-partition counter on the fence walk. The data do not support that as the whole story (§5): the `cores = 8` pair moved only from +6.0 to +5.7 us when the counter went. `cores = 8` is
   established by pass 2; `cores = 4` is split between the passes. The 5 % throughput and p99 legs of BP-R2 are not met on any arm; only the 3 % CPU leg is.
3. **BP-H9 "inside the spread".** `PURGE` at `cores = 4` is 5.3-5.7 % above `cores = 1` in both passes, and in pass 1 the runs do not overlap. The rejection stands on three other grounds (§4): a `DELETE` that asks nothing steps up 34-39 %, the profile puts
   `InFlight` at 1 sample of 695, and 1.00 id is read per miss.
4. **BP-H7's flat throughput between `cores` 2 and 4** cannot carry the verdict: the driver pins `cores = 2` onto SMT siblings and records no loader CPU. The cadence instrument and the pages-per-row ratio carry it (§2). The `k1` cells of pass 2 are host-stalled; "about 1 run per period" at `cores = 1` is pass 1's 1.00.
5. **The pass-1 summary written at the time** still lists the discarded `cell2 k4 r2` (a compile ran beside it); this file uses the re-run.

## How the numbers were taken, and what each pass is used for

**Pass 1** (`1592007e`) ran BP-S1's cells with its first counters. Four of them were wrong: the in-flight tally counted tables read
and not ids; `checkpoint_pages_written` counted the publish path only (30,886 pages in `SHOW META` against 373,480 on the run log at
`cores = 1`); `checkpoint_data_syncs` counted parks, not syncs; and `fence_partitions_read` was a counter on the fence walk's hot path,
one `Add` per partition visited. `676d1d7d` fixed the first three and deleted the fourth. **Pass 1 is therefore used only for what its
counters did not touch**: H6's word moves and memo drops, the sampler profiles, the one-drop probe, the cadence instrument (runs, rows and
probe latencies are not counters of that kind), and the delta column. **Pass 2** (`ccf9cfc1`) re-ran H7, H9 and H10 on the fixed counters.
It carries BP-S3's H1-H5 as well, so a pass-1 to pass-2 difference at `cores = 1` is the counter fix plus BP-S3's removal of fences, and the two
passes are two to eight hours apart and not interleaved: **the delta column is not an A/B.**

**Where the work order's §6.1 was not followed.** Three runs per cell, not eight (the checkpoint cell: two in pass 2, one in pass 1, plus eight
cadence repeats); cells run in series with `plain` and `fence` adjacent in time, not interleaved block by block (the lock benchmark itself
interleaves its arms in four blocks); no skew cell and no skew profile; no reader against the growth cell, so the "reader's p99 against the same cell
with no inserting cores" was not taken (the counters and the sampler price the premise from the other side); the profile ran on the growth and
`PURGE` cells only. **Pinning** differs by cell and matters: the lock cell put the client on CPU 1, the SMT sibling of the first reactor's CPU 0, at
`cores` 1 and 4, and on all eight CPUs at `cores = 8`, so eight reactors and the client share four physical cores; the checkpoint driver pins reactors to CPUs
0..k-1 (`cores = 2` is one physical core's two siblings) and its loaders and probes to 4-7; the in-flight cell's client sat on CPU 5, the sibling of
the reactor on CPU 4 whenever the session landed on core 2.

**Host.** The pass-2 chain's gate (1-minute load at most 1.6, none of `cc1plus`, `cmake --build`, `ctest`, `kds_tests`, the BL and BQ drivers running) admitted every pass-2 cell, and
pass 2's stamps record load and filesystem only; the drivers' own before/after lists (`cc1plus|cmake --build|ctest`) were empty in every cell. **The gate does not
see I/O.** The checkpoint driver recorded the kernel's I/O pressure: `bp2-k1-cp5000-a` started with `full avg10 = 74.7 %`, and both `k1` cells then stalled for tens of seconds
(a probe p99 of 28 s and 95 s); four of the 18 `group` lock runs (`k1 plain r2`, `k4 fence r2`, `k8 plain r2`, `k8 fence r2`, all the third run of their row) fell to 276-771 stmt/s on some arm against about
2,700 for their siblings, a device stall of the same kind. `recovery_checkpoint_us` of the fresh server flagged two of the four beforehand (319 and 831 ms) and was also high (326 ms) before one run that then ran normally (`k8 plain r1`).
Pass 1 stamps carry the competing-process list and CPU pressure per cell; a compile and a `ctest` run landed during two pass-1 cells (`cell2 k4 r2`, `cell3 k1 600 s`), both discarded and re-run, and the
CABIN AUTO table below is the re-run.

## 1. BP-H6: a root move costs at most `cores - 1` memo drops, and a drop is cheap

**Finding.** The growth cell (every session inserting single rows into its own btree relation from empty, two relations per core, 20 s) moves the schema word
twice per relation at `relaxed` (about 324,000 rows each at `cores = 1`) and once per relation under `group` (about 6,900 rows each), on a level growth, and nothing else: no Cabin, schema or resolve move occurs.
Each move makes each other core drop its memo once at its next statement head, so drops per run equal root moves x (`cores` - 1) at `relaxed` (ratio 0.98-1.00) and 0.64-0.67 of it at `group`, where a core that has
not yet resolved the relation has nothing to drop. Structural reruns are zero in every cell. The rate, 9.5e-5 drops per statement at `cores = 2` and 2.72e-4 at `cores = 4` under `group`, looks higher than `relaxed`'s only because a `group` statement takes 2.4 ms and the cell issues few of them; per second it is 0.13 and 0.77 drops under `group` against 0.4 and 2.4 under `relaxed`.

*Table: growth cell (pass 1): statements and latency*

| durability | cores | sessions (relations) | runs | statements per run | stmt/s median (min-max) | p0 | p25 | p50 | p95 | p99 | max (largest of runs) | client CPU share (max over sessions) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `group` | 1 | 2 | 3 | 13,825 stmt | 691 stmt/s (674-707) | 1,169.5 µs | 2,367.7 µs | 2,519.8 µs | 5,291.6 µs | 10,236.3 µs | 31,069.1 µs | 0.01 of a CPU |
| `group` | 2 | 4 | 3 | 28,516 stmt | 1,426 stmt/s (1,365-1,430) | 1,127.9 µs | 2,284.5 µs | 2,417.5 µs | 5,047.1 µs | 10,064.6 µs | 528,268.3 µs | 0.02 of a CPU |
| `group` | 4 | 8 | 3 | 56,360 stmt | 2,817 stmt/s (2,808-2,828) | 1,220.7 µs | 2,296.3 µs | 2,447.8 µs | 5,123.0 µs | 10,165.3 µs | 34,098.5 µs | 0.02 of a CPU |
| `relaxed` | 1 | 2 | 3 | 648,516 stmt | 32,426 stmt/s (32,237-32,744) | 38.0 µs | 50.6 µs | 51.4 µs | 67.0 µs | 78.3 µs | 374,070.2 µs | 0.54 of a CPU |
| `relaxed` | 2 | 4 | 3 | 1,073,867 stmt | 53,469 stmt/s (53,387-58,440) | 32.3 µs | 54.1 µs | 60.0 µs | 77.4 µs | 95.9 µs | 423,957.4 µs | 0.49 of a CPU |
| `relaxed` | 4 | 8 | 3 | 1,421,774 stmt | 71,076 stmt/s (69,942-72,528) | 30.8 µs | 70.1 µs | 78.2 µs | 147.8 µs | 198.7 µs | 426,562.4 µs | 0.38 of a CPU |

*Table: growth cell (pass 1): word moves by cause and the memo drops they cost*

| durability | cores | root moves per run | cabin | schema | resolve | memo drops per run | drops / (root moves x (cores-1)) | memo drops per statement | structural reruns | carves per run |
|---|---|---|---|---|---|---|---|---|---|---|
| `group` | 1 | [2, 2, 2] moves | 0 moves | 0 moves | 0 moves | [0, 0, 0] drops | - (ratio) | 0.00e+00 drops/stmt | 0 reruns | [3, 3, 3] carves |
| `group` | 2 | [4, 4, 4] moves | 0 moves | 0 moves | 0 moves | [2, 3, 3] drops | 0.67 (ratio) | 9.48e-05 drops/stmt | 0 reruns | [6, 9, 7] carves |
| `group` | 4 | [8, 8, 8] moves | 0 moves | 0 moves | 0 moves | [17, 15, 14] drops | 0.64 (ratio) | 2.72e-04 drops/stmt | 0 reruns | [17, 19, 19] carves |
| `relaxed` | 1 | [4, 4, 4] moves | 0 moves | 0 moves | 0 moves | [0, 0, 0] drops | - (ratio) | 0.00e+00 drops/stmt | 0 reruns | [157, 158, 160] carves |
| `relaxed` | 2 | [8, 8, 8] moves | 0 moves | 0 moves | 0 moves | [8, 8, 8] drops | 1.00 (ratio) | 7.25e-06 drops/stmt | 0 reruns | [288, 263, 265] carves |
| `relaxed` | 4 | [16, 16, 16] moves | 0 moves | 0 moves | 0 moves | [47, 46, 48] drops | 0.98 (ratio) | 3.30e-05 drops/stmt | 0 reruns | [392, 392, 395] carves |

CABIN AUTO adds a second source. At `cores = 4` a run counts five Cabin moves, 42 schema moves and 16 resolve moves (the scenario's own DDL), 8 root moves, and
**50-57 memo drops over 277 s**, 41-48 of them before the first Cabin move. Readers (three free-running point `SELECT` sessions, one per core 1-3) read the same in all three runs:
p50 133-138 us, p99 244-299 us over 5.9-6.1 million statements. There is no control arm without Cabin moves, so this is a statement that nothing diverges, not a measurement of an effect.

*Table: CABIN AUTO cell (pass 1)*

| cores | run | readers | driver wall | load 1 min before -> after | root | cabin | schema | resolve | memo drops (total) | drops before the first cabin move | drops after it | structural reruns | reader statements | on-arm statements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | r0 | no | 275.9 s | 3.29 -> 0.24 load | 5 moves | 5 moves | 30 moves | 10 moves | 5 drops | 5 drops | 0 drops | 0 reruns | 0 stmt | 43,440 stmt |
| 1 | r1 | no | 275.7 s | 2.80 -> 0.22 load | 5 moves | 5 moves | 30 moves | 10 moves | 5 drops | 5 drops | 0 drops | 0 reruns | 0 stmt | 43,440 stmt |
| 1 | r2 | no | 275.7 s | 3.00 -> 1.63 load | 5 moves | 5 moves | 30 moves | 10 moves | 5 drops | 5 drops | 0 drops | 0 reruns | 0 stmt | 43,440 stmt |
| 4 | r0 | yes | 277.6 s | 0.24 -> 2.80 load | 8 moves | 5 moves | 42 moves | 16 moves | 50 drops | 43 drops | 7 drops | 0 reruns | 5,964,462 stmt | 43,440 stmt |
| 4 | r1 | yes | 277.9 s | 0.22 -> 3.00 load | 8 moves | 5 moves | 42 moves | 16 moves | 57 drops | 48 drops | 9 drops | 0 reruns | 6,054,547 stmt | 43,440 stmt |
| 4 | r2 | yes | 277.6 s | 0.32 -> 2.77 load | 8 moves | 5 moves | 42 moves | 16 moves | 50 drops | 41 drops | 9 drops | 0 reruns | 6,113,133 stmt | 43,440 stmt |

Readers (point `SELECT`, free-running, one session per core 1..3, same server as the scenario's session on core 0):
| run | reader core | statements | errors | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| r0 | 1 | 1,956,415 stmt | 0 errors | 42.1 µs | 116.6 µs | 138.2 µs | 207.0 µs | 284.0 µs |
| r0 | 2 | 2,018,423 stmt | 0 errors | 42.0 µs | 111.7 µs | 136.8 µs | 202.9 µs | 248.6 µs |
| r0 | 3 | 1,989,624 stmt | 0 errors | 50.5 µs | 113.5 µs | 137.2 µs | 204.7 µs | 262.4 µs |
| r1 | 1 | 2,001,000 stmt | 0 errors | 50.5 µs | 111.6 µs | 135.4 µs | 204.2 µs | 270.7 µs |
| r1 | 2 | 2,034,613 stmt | 0 errors | 52.5 µs | 111.2 µs | 135.2 µs | 201.5 µs | 248.2 µs |
| r1 | 3 | 2,018,934 stmt | 0 errors | 51.9 µs | 112.1 µs | 135.5 µs | 202.4 µs | 254.6 µs |
| r2 | 1 | 2,045,357 stmt | 0 errors | 51.4 µs | 110.6 µs | 134.2 µs | 198.4 µs | 253.4 µs |
| r2 | 2 | 2,078,701 stmt | 0 errors | 42.5 µs | 107.1 µs | 133.0 µs | 196.6 µs | 244.3 µs |
| r2 | 3 | 1,989,075 stmt | 0 errors | 41.6 µs | 114.2 µs | 135.1 µs | 202.9 µs | 298.8 µs |

**What one drop costs.** A probe on a two-core server alternates a schema change on core 1 with a statement on core 0 and subtracts the same statement run warm and a control
(a word-neutral `SHOW META` on core 1). The net cost lands on the statement that meets the drop, is CPU and not wait (server CPU 41.7 us against 43.1 us wall for the point `SELECT`), and grows with the
catalog (last third of the run, 900 junk tables: +46 and +97 us). The maxima are not recorded by this driver.

*Table: one memo drop's cost (pass 1, `cores = 2`, reactors pinned, 300 iterations per statement, 900 `CREATE TABLE junk_i` over the run)*

| statement | arm | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| point SELECT (1 relation) | first statement after the schema word moved (drop) | 300 stmt | 94.4 µs | 110.7 µs | 123.8 µs | 157.8 µs | 186.6 µs |
| point SELECT (1 relation) | same statement, warm | 300 stmt | 72.1 µs | 75.4 µs | 78.5 µs | 90.4 µs | 117.6 µs |
| point SELECT (1 relation) | control: first statement after a word-neutral `SHOW META` on the other core | 300 stmt | 86.4 µs | 95.9 µs | 99.5 µs | 121.9 µs | 138.1 µs |
| point SELECT (1 relation) | control: same statement, rest | 300 stmt | 74.0 µs | 82.1 µs | 85.9 µs | 101.2 µs | 115.3 µs |
| point UPDATE (1 relation) | first statement after the schema word moved (drop) | 300 stmt | 122.1 µs | 142.0 µs | 153.7 µs | 195.3 µs | 218.5 µs |
| point UPDATE (1 relation) | same statement, warm | 300 stmt | 58.4 µs | 61.2 µs | 63.9 µs | 72.7 µs | 81.2 µs |
| point UPDATE (1 relation) | control: first statement after a word-neutral `SHOW META` on the other core | 300 stmt | 66.6 µs | 70.5 µs | 73.0 µs | 89.9 µs | 109.1 µs |
| point UPDATE (1 relation) | control: same statement, rest | 300 stmt | 56.7 µs | 59.1 µs | 61.3 µs | 74.3 µs | 92.4 µs |
| pk join SELECT (2 relations) | first statement after the schema word moved (drop) | 300 stmt | 63.0 µs | 69.9 µs | 72.0 µs | 89.0 µs | 102.8 µs |
| pk join SELECT (2 relations) | same statement, warm | 300 stmt | 57.4 µs | 60.7 µs | 63.8 µs | 74.6 µs | 79.8 µs |
| pk join SELECT (2 relations) | control: first statement after a word-neutral `SHOW META` on the other core | 300 stmt | 66.8 µs | 69.9 µs | 72.0 µs | 90.9 µs | 106.9 µs |
| pk join SELECT (2 relations) | control: same statement, rest | 300 stmt | 56.6 µs | 60.5 µs | 62.8 µs | 73.8 µs | 77.3 µs |

| statement | cold - warm, median (wall) | control first - rest (wall) | net of control (wall) | server CPU cold - warm | control (server CPU) | net of control (server CPU) | net wall by third of the run (catalog grows 0 -> 900 junk tables) |
|---|---|---|---|---|---|---|---|
| point SELECT (1 relation) | 43.1 µs | 14.6 µs | 28.5 µs | 41.7 µs | 11.4 µs | 30.3 µs | [12.7, 28.4, 46.4] µs |
| point UPDATE (1 relation) | 90.6 µs | 11.9 µs | 78.7 µs | 87.4 µs | 8.4 µs | 79.0 µs | [61.8, 75.6, 97.4] µs |
| pk join SELECT (2 relations) | 9.6 µs | 10.0 µs | -0.4 µs | 6.9 µs | 6.2 µs | 0.7 µs | [-0.5, -0.6, 0.5] µs |

**What the sampler attributes.** At 1 kHz over a 14 s window of the relaxed growth cell, the catalog's resolve functions (inclusive) are `SysTableRow::Decode` 0.67 %, `FindTableAccess` 0.12 %,
`Revalidate` 0.07 %, `InitTableAccess` 0.04 % of 21,713 samples at `cores = 4` (server CPU 34.2 s for about 1.01 million statements, 33.8 us each), and 0.50 / 0.08 / - / 0.05 % at `cores = 1`, where
no drop ever happens (8,450 samples, 18.2 us per statement). Charging every one of those samples to drops gives 0.90 %, under the 3 % bar, and the `cores = 1` figure shows most of it is not drops. The statement
itself is 35 % `send(2)` at `cores = 4` (7,524 samples), the cost BN-S1 found.

**The bound.** Worst case, `group` `cores = 4`: 2.72e-4 drops per statement x 97 us = 0.026 us per statement, 0.08 % of 33.8 us (0.15 % of the `cores = 1` figure of 18.2 us). Even a drop of 1 ms would
be 0.27 us, 0.8 % of 33.8 us. In pass 2 the same counters run in the checkpoint cells: a single relation grown to 20 million rows by four loaders moves the root 4 times, with 14 drops at `cores = 4` over 115,953
statements, and no structural rerun anywhere.

*Table: word moves and memo drops in pass 2's checkpoint cells (ccf9cfc1): four loaders into ONE relation `load`, 240 s, SHOW META after the load*

| cell | cores | rows inserted into `load` | root moves | cabin | schema | resolve | memo drops | structural reruns | statements the cell issued (loader INSERTs + probes) |
|---|---|---|---|---|---|---|---|---|---|
| k1-a | 1 | 15,235,072 rows | 4 moves | 0 moves | 10 moves | 4 moves | 2 drops | 0 reruns | 38,878 stmt |
| k1-b | 1 | 7,811,072 rows | 3 moves | 0 moves | 10 moves | 4 moves | 2 drops | 0 reruns | 31,628 stmt |
| k2-a | 2 | 20,256,768 rows | 4 moves | 0 moves | 10 moves | 4 moves | 6 drops | 0 reruns | 67,782 stmt |
| k2-b | 2 | 20,776,960 rows | 4 moves | 0 moves | 10 moves | 4 moves | 6 drops | 0 reruns | 68,290 stmt |
| k4-a | 4 | 20,431,872 rows | 4 moves | 0 moves | 10 moves | 4 moves | 14 drops | 0 reruns | 115,953 stmt |
| k4-b | 4 | 20,086,784 rows | 4 moves | 0 moves | 10 moves | 4 moves | 14 drops | 0 reruns | 115,616 stmt |

**Verdict.** Rejected on its own V: the premise fails at the cell it names (growth) and at CABIN AUTO. The bound does not depend on which of the
two denominators is used. BP-Q4 takes (c), keep; BP-S5 is not run. The `[source-read]` leg of H6 (options (a) and (d)) was not needed and was not done.

## 2. BP-H7: per-core checkpoints write the same pages per row; what they multiply is the runs and their syncs

**Finding.** At a 5 s cadence the instance runs `cores` checkpoint runs per period (1.00, 1.98, 3.94 per period in the clean cells; pass 2's `k4-b` read 3.19, with fewer runs through its first 100 s).
The pages those runs write per 1,000 rows rise 11-18 % between `cores = 1` and 4 and not 4x, because each core writes back the pages its own sessions dirtied; the cost of N runs is more runs and more syncs
(7.6-10.3 data syncs per run at `cores` 2 and 4), not more I/O per row. What the extra runs buy is a younger anchor: the longest gap between two completed runs falls 5.6 s -> 3.4-4.0 s -> 1.9 s with
`cores` 1 -> 2 -> 4.

*Table: checkpoint cell, pass 2 (ccf9cfc1): one row per cell*

| cell | cores | rep | load 1 min before | io pressure `full` avg10 before / after | rows inserted | rows/s | checkpoint runs (load span) | runs per 5 s period | pages written (counter) | pages by run log, load span | pages per 1,000 rows (counter) | pages per 1,000 rows (run log) | share of core-seconds inside a run | MB/s written back | data syncs (counter) | syncs per run | run wall p50 / max | longest anchor gap | read as |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| k1-a | 1 | a | 1.27 load | 74.66 % / 49.37 % | 15,235,072 rows | 63,479 rows/s | 47 runs | 0.98 runs/period | 347,663 pages | 319,870 pages | 22.8 pages/1,000 rows | 21.0 pages/1,000 rows | 18.7 % | 11.9 MB/s | 1,631 syncs | 34.7 syncs/run | 634 ms / 7499 ms | 11.83 s | stalled by host I/O: not usable for rate or latency |
| k1-b | 1 | b | 1.39 load | 0.00 % / 65.15 % | 7,811,072 rows | 32,546 rows/s | 43 runs | 0.90 runs/period | 179,984 pages | 165,063 pages | 23.0 pages/1,000 rows | 21.1 pages/1,000 rows | 26.0 % | 6.1 MB/s | 1,133 syncs | 26.3 syncs/run | 731 ms / 9307 ms | 15.28 s | stalled by host I/O: not usable for rate or latency |
| k2-a | 2 | a | 1.45 load | 10.21 % / 21.27 % | 20,256,768 rows | 84,403 rows/s | 95 runs | 1.98 runs/period | 517,243 pages | 454,945 pages | 25.5 pages/1,000 rows | 22.5 pages/1,000 rows | 15.2 % | 17.7 MB/s | 784 syncs | 8.3 syncs/run | 782 ms / 1740 ms | 3.99 s | host I/O pressure at start |
| k2-b | 2 | b | 1.60 load | 0.00 % / 25.21 % | 20,776,960 rows | 86,571 rows/s | 95 runs | 1.98 runs/period | 531,349 pages | 476,754 pages | 25.6 pages/1,000 rows | 22.9 pages/1,000 rows | 16.7 % | 18.1 MB/s | 978 syncs | 10.3 syncs/run | 821 ms / 1663 ms | 3.45 s | clean |
| k4-a | 4 | a | 1.53 load | 0.00 % / 19.70 % | 20,431,872 rows | 85,133 rows/s | 189 runs | 3.94 runs/period | 551,127 pages | 477,081 pages | 27.0 pages/1,000 rows | 23.3 pages/1,000 rows | 11.0 % | 18.8 MB/s | 1,478 syncs | 7.8 syncs/run | 521 ms / 966 ms | 1.93 s | clean |
| k4-b | 4 | b | 1.46 load | 0.17 % / 18.63 % | 20,086,784 rows | 83,695 rows/s | 153 runs | 3.19 runs/period | 541,159 pages | 469,812 pages | 26.9 pages/1,000 rows | 23.4 pages/1,000 rows | 9.3 % | 18.5 MB/s | 1,167 syncs | 7.6 syncs/run | 541 ms / 2104 ms | 6.61 s | one 1.6 s stall seen by all four loaders |

Reading this table: `k1` is not usable for rate or latency in either rep (host I/O stall); its page counts are still sound, since 23.0 pages per 1,000 rows held at half the rows. `k2-a` started under mild I/O pressure
(its probe p99, 82 ms, against 18 ms in `k2-b`). `k4-b` ran 3-13 runs per 20 s instead of 16 through its first 100 s, the window in which all four loaders met one 1.6 s stall. `k2-b` and `k4-a` are the clean cells. Bytes: 10.8-11.5 thousand pages per 5 s period at `cores` 2-4, 88-94 MB per period,
17.6-18.7 MB/s (counter x 8 KiB over the 240 s load). The two page columns differ because the counter also catches writes outside a run's logged pages (the 9-16 % gap grows with `cores`); neither is the ideal of one write per page, which this census cannot
state.

**The foreground probe.** A point `SELECT` of a 1,000-row relation on each core, sent open-loop every 20 ms with latency counted from the scheduled time, under the four-loader insert load:

*Table: the foreground probe (point `SELECT` of a 1,000-row relation, one session per core, open loop at 20 ms, latency from the scheduled time), all cores pooled, over the load span*

| cell | n | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| k1-a | 12,000 samples | 0.15 ms | 16.94 ms | 279.12 ms | 26,418.83 ms | 28,070.59 ms | 28,709.42 ms |
| k1-b | 12,000 samples | 0.16 ms | 10,494.19 ms | 64,861.76 ms | 87,688.59 ms | 94,838.84 ms | 97,057.42 ms |
| k2-a | 24,000 samples | 0.17 ms | 0.44 ms | 0.89 ms | 15.09 ms | 82.19 ms | 354.63 ms |
| k2-b | 24,000 samples | 0.10 ms | 0.30 ms | 0.36 ms | 8.56 ms | 18.39 ms | 413.79 ms |
| k4-a | 48,000 samples | 0.16 ms | 0.50 ms | 0.62 ms | 12.37 ms | 25.39 ms | 382.69 ms |
| k4-b | 48,000 samples | 0.16 ms | 0.49 ms | 0.62 ms | 14.04 ms | 51.43 ms | 1,142.57 ms |

Probe inside and outside the probed core's own checkpoint runs (clean cells):
| cell | class | n | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| k2-a | inside a run of its core | 3,651 samples | 0.20 ms | 0.40 ms | 0.76 ms | 33.24 ms | 130.65 ms | 354.63 ms |
| k2-a | outside | 20,349 samples | 0.17 ms | 0.44 ms | 0.91 ms | 13.64 ms | 70.25 ms | 354.54 ms |
| k2-b | inside a run of its core | 4,017 samples | 0.16 ms | 0.29 ms | 0.33 ms | 9.48 ms | 41.74 ms | 413.73 ms |
| k2-b | outside | 19,983 samples | 0.10 ms | 0.30 ms | 0.36 ms | 8.47 ms | 16.59 ms | 413.79 ms |
| k4-a | inside a run of its core | 5,235 samples | 0.23 ms | 0.50 ms | 0.61 ms | 13.45 ms | 37.77 ms | 382.63 ms |
| k4-a | outside | 42,765 samples | 0.16 ms | 0.50 ms | 0.62 ms | 12.25 ms | 24.53 ms | 382.69 ms |
| k4-b | inside a run of its core | 4,486 samples | 0.19 ms | 0.48 ms | 0.61 ms | 16.05 ms | 49.61 ms | 216.13 ms |
| k4-b | outside | 43,514 samples | 0.16 ms | 0.49 ms | 0.62 ms | 13.93 ms | 51.77 ms | 1,142.57 ms |

`C_PING` probe (no statement path) pooled:
| cell | n | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| k1-a | 12,000 samples | 0.12 ms | 5.23 ms | 122.46 ms | 20,035.36 ms | 24,473.77 ms | 25,527.78 ms |
| k1-b | 12,000 samples | 0.10 ms | 9,641.61 ms | 46,448.10 ms | 60,672.99 ms | 62,315.04 ms | 63,084.47 ms |
| k2-a | 24,000 samples | 0.08 ms | 0.32 ms | 0.67 ms | 13.36 ms | 76.82 ms | 334.18 ms |
| k2-b | 24,000 samples | 0.08 ms | 0.25 ms | 0.33 ms | 8.17 ms | 17.09 ms | 393.84 ms |
| k4-a | 48,000 samples | 0.09 ms | 0.47 ms | 0.61 ms | 12.03 ms | 23.76 ms | 431.25 ms |
| k4-b | 48,000 samples | 0.08 ms | 0.46 ms | 0.61 ms | 13.58 ms | 54.22 ms | 1,142.48 ms |

At the clean cells the probe's body does not move with `cores` (p50 0.36-0.62 ms) and its tail is the checkpoint's: p99 inside a run of the probed core's own is 1.5-2.5x its p99 outside in three of four cells
(`k2-b` 41.7 against 16.6 ms; `k4-a` 37.8 against 24.5 ms), about 40 samples deep, so a direction rather than a number. A core is inside a run 9-17 % of the time at every `cores`; N runs do not put any core inside a run more
than one run does.

**Where a run's wall time goes.** Of a 555-845 ms run at `cores` 2-4, **77-84 % is parked on the log gate**, 7-10 % on a data sync, and page writes are 1-2 %:

*Table: where a checkpoint run's wall time goes (sum over the run records in the load span, per run; the phases are the server's own run line)*

| cell | runs | wall per run | start | page writes | publish writeback | parked on the log (gate/end) | parked on a data sync | the rest (steps between, yields) |
|---|---|---|---|---|---|---|---|---|
| k1-a | 47 runs | 957.0 ms | 5.9 ms (1 %) | 71.4 ms (7 %) | 11.9 ms (1 %) | 75.1 ms (8 %) | 154.3 ms (16 %) | 638.5 ms (67 %) |
| k1-b | 43 runs | 1,450.1 ms | 4.6 ms (0 %) | 41.9 ms (3 %) | 1.6 ms (0 %) | 421.0 ms (29 %) | 319.0 ms (22 %) | 662.1 ms (46 %) |
| k2-a | 95 runs | 768.1 ms | 10.1 ms (1 %) | 18.9 ms (2 %) | 11.3 ms (1 %) | 591.3 ms (77 %) | 77.6 ms (10 %) | 59.0 ms (8 %) |
| k2-b | 95 runs | 844.5 ms | 9.0 ms (1 %) | 13.2 ms (2 %) | 13.1 ms (2 %) | 706.2 ms (84 %) | 56.1 ms (7 %) | 46.8 ms (6 %) |
| k4-a | 189 runs | 556.5 ms | 10.3 ms (2 %) | 6.1 ms (1 %) | 8.5 ms (2 %) | 464.0 ms (83 %) | 40.6 ms (7 %) | 27.1 ms (5 %) |
| k4-b | 153 runs | 584.4 ms | 10.4 ms (2 %) | 11.4 ms (2 %) | 7.4 ms (1 %) | 467.0 ms (80 %) | 57.3 ms (10 %) | 30.7 ms (5 %) |

**Cadence as the instrument.** The census has no build that runs one instance-wide checkpoint, so the cost-removed arm BP-R2 asks for is approximated by cutting the same cell's runs 4x (pass 1, `cores = 4`,
150 s, eight repeats each, one fresh server per repeat). It removes three quarters of the runs and the syncs they issue and moves throughput **-1.5 %**: the 5 s cadence is not slower. The p99 reads 12.6 % worse at the 5 s cadence
(23.8 against 26.8 ms), but the 20 s cadence's own eight repeats span 18.0-37.3 ms, so by the noise rule it is not a finding.

*Table: cadence as an instrument (pass 1, `cores = 4`, 150 s cells, eight repeats each): checkpoint runs per unit time cut 4x by `checkpoint_interval_ms` 5,000 -> 20,000*

| cadence | repeats | checkpoint runs per cell | rows/s median (min-max) | probe p50 median (min-max) | probe p99 median (min-max) | probe max median (min-max) | longest anchor gap median (min-max) |
|---|---|---|---|---|---|---|---|
| 20 s | 8 repeats | [26, 27] runs | 93,430 rows/s (90,057-94,003) | 0.58 ms (0.54-0.59) | 23.8 ms (18.0-37.3) | 384.8 ms (167.5-894.2) | 5.55 s (5.45-6.11) |
| 5 s | 8 repeats | [113, 117] runs | 94,891 rows/s (93,942-104,987) | 0.59 ms (0.57-0.60) | 26.8 ms (23.4-35.2) | 296.6 ms (127.0-404.3) | 1.85 s (1.75-5.87) |

*Table: pass 1 (1592007e), the same cell: `bi_checkpoint_stall.py`, group, 240 s, 5 s cadence, one run per core count (counters of pages and syncs are the flawed ones and are not shown)*

| cores | rows inserted | rows/s | checkpoint runs | runs per 5 s period | run wall p50 / max | share of core-seconds inside a run | pages per 1,000 rows (run log) | longest anchor gap | io pressure `full` avg10 before | probe n | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 17,727,488 rows | 73,865 rows/s | 48 runs | 1.00 runs/period | 623 ms / 1155 ms | 14.3 % | 21.1 pages/1,000 rows | 5.60 s | 0.00 % | 12,000 samples | 0.18 ms | 5.64 ms | 24.64 ms | 316.28 ms | 378.09 ms | 462.80 ms |
| 2 | 21,303,296 rows | 88,764 rows/s | 95 runs | 1.98 runs/period | 775 ms / 1354 ms | 15.3 % | 23.0 pages/1,000 rows | 3.35 s | 0.34 % | 24,000 samples | 0.15 ms | 0.30 ms | 0.36 ms | 9.11 ms | 27.22 ms | 404.57 ms |
| 4 | 20,401,152 rows | 85,005 rows/s | 189 runs | 3.94 runs/period | 535 ms / 1012 ms | 11.0 % | 23.4 pages/1,000 rows | 1.91 s | 0.13 % | 48,000 samples | 0.13 ms | 0.48 ms | 0.59 ms | 11.68 ms | 22.94 ms | 348.33 ms |

**Verdict.** Rejected as not material, with three qualifications. (1) The pages-per-row comparison uses `k1` cells that stalled; the count is robust to the stall (the ratio held at half the rows) but the rate and
latency columns of `k1` are pass 1's. (2) "Throughput flat between `k2` and `k4`" (84-87k rows/s) is not evidence about checkpoints: this driver pins `cores = 2` onto two SMT siblings of one physical core and runs four Python loaders
on CPUs 4-7, and records no loader CPU, so a client ceiling cannot be excluded. The cadence instrument is the stronger evidence. (3) Runs per period of 3.19 belong to the stalled `k4-b`; the clean value is 3.94. BP-Q5 takes (a), keep. The
instance-wide alternative would have to publish undo heads beside AX's in-flight ids, and the census gives that change little to buy: taking `cores = 1`'s pages per row as the floor, the I/O it could save is the 11-18 % above.

## 3. BP-H8: not measured

The order's V takes the window's size, its burns and `LookupCommit` per statement from counters that already exist, in the skew cell. **No skew cell was run, and `SHOW META` carries none of the three** (its window counters are
the latch's waits, `contention_window_waits`, and the carve counter). The order's own fallback applies: H8 is reported unmeasured and BP-Q6 stays report-only. What the census does hold is indirect and says nothing about burns: carves run
about once per 3,600 statements in the growth cell (`relaxed`, `cores = 4`: 392 carves per 1.42 million statements), the window latch waited 0.5-1.2 s over 240 s in the checkpoint cell at `cores` 2-4, and the lock cells land eight sessions on
only 4-7 of 8 cores (3-4 of 4) because `SO_REUSEPORT` hashes connections, so idle cores are the normal case in these cells even without a skew cell. A later order that wants H8 needs a counter first.

## 4. BP-H9: the scan is one id per miss, and only `PURGE` asks per row

**Finding.** `PURGE` asks `InFlight` once per key, 100,000 times for 100,000 rows, and every ask is a miss. A miss reads each attached core's table (one count each, four at `cores = 4`) and every live id in them: **1.00 id per miss at both core
counts**, because the only live id is the `PURGE`'s own transaction. The named-key `INSERT` asks 0 times on its uncontended path (20,000 fresh and 20,000 duplicate statements) and 20 times, all hits, when it meets another session's undecided insert (20 asks for 20 waits of 200 ms, so a waiter does not ask repeatedly while it waits).

*Table: PURGE of 100,000 delete-marked rows, `relaxed`, pass 2 (ccf9cfc1), counters fixed*

| cores | run | session on core | load 1 min before | PURGE wall | PURGE rows/s (derived: 100,000 / wall) | PURGE server CPU | CPU per row | asks | misses | ids read per miss (pass 2 only) | `DELETE` of the same rows: wall / server CPU / asks |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | r0 | core 0 | 1.55 load | 797.4 ms | 125,408 rows/s | 797.7 ms | 7.98 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 85.4 ms / 85.3 ms / 0 asks |
| 1 | r1 | core 0 | 1.44 load | 673.1 ms | 148,566 rows/s | 673.5 ms | 6.74 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 49.5 ms / 49.4 ms / 0 asks |
| 1 | r2 | core 0 | 1.39 load | 681.3 ms | 146,778 rows/s | 681.6 ms | 6.82 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 49.6 ms / 49.5 ms / 0 asks |
| 4 | r0 | core 3 | 1.52 load | 694.9 ms | 143,906 rows/s | 715.5 ms | 7.16 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 110.3 ms / 107.6 ms / 0 asks |
| 4 | r1 | core 2 | 1.37 load | 822.4 ms | 121,595 rows/s | 842.2 ms | 8.42 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 64.3 ms / 66.0 ms / 0 asks |
| 4 | r2 | core 2 | 1.44 load | 698.4 ms | 143,184 rows/s | 717.6 ms | 7.18 µs | 100,000 asks | 100,000 misses | 1.00 ids/miss | 64.5 ms / 66.2 ms / 0 asks |

| cores | PURGE CPU, median (min-max) | PURGE CPU, the two lowest runs | DELETE CPU, median (min-max) | k4 - k1, PURGE | k4 - k1, DELETE (no in-flight ask) |
|---|---|---|---|---|---|
| 1 | 681.6 ms (673.5-797.7) | [673.5, 681.6] | 49.5 ms (49.4-85.3) | - | - |
| 4 | 717.6 ms (715.5-842.2) | [715.5, 717.6] | 66.2 ms (66.0-107.6) | - | - |
| | | | | +36.0 ms (+5.3 %) | +16.7 ms (+33.7 %) |

Named-key `INSERT` loops (20,000 statements per loop, one session, a fresh table):
| cores | loop | runs | stmt/s median (min-max) | server CPU per stmt median (min-max) | p0 | p25 | p50 | p95 | p99 | max | in-flight asks | errors per run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | fresh keys | 3 | 20,505 stmt/s (19,924-22,381) | 26.4 µs (21.4-26.8) | 36.0 µs | 45.8 µs | 46.6 µs | 58.0 µs | 68.7 µs | n/r | 0 asks | [0, 0, 0] errors |
| 1 | the same keys again (duplicates) | 3 | 20,065 stmt/s (17,314-20,092) | 23.3 µs (22.7-29.2) | 39.5 µs | 45.2 µs | 45.7 µs | 62.5 µs | 120.4 µs | n/r | 0 asks | [20000, 20000, 20000] errors |
| 4 | fresh keys | 3 | 15,867 stmt/s (15,828-21,854) | 35.0 µs (24.2-35.3) | 53.9 µs | 57.4 µs | 58.1 µs | 75.3 µs | 90.1 µs | n/r | 0 asks | [0, 0, 0] errors |
| 4 | the same keys again (duplicates) | 3 | 14,029 stmt/s (13,875-19,598) | 38.6 µs (25.3-38.6) | 62.5 µs | 65.2 µs | 66.2 µs | 87.8 µs | 146.3 µs | n/r | 0 asks | [20000, 20000, 20000] errors |

A named key meeting another session's undecided insert of the same key (20 waits of 200 ms per run):
| cores | in-flight asks per run | misses | ids read | session cores |
|---|---|---|---|---|
| 1 | [20, 20, 20] asks | [0, 0, 0] misses | [0, 0, 0] ids or tables read | [0, 0, 0] core |
| 4 | [20, 20, 20] asks | [0, 0, 0] misses | [0, 0, 0] ids or tables read | [3, 2, 2] core |

**Is the scan material?** At `cores = 4` the `PURGE` costs 36 ms more CPU than at `cores = 1` (+5.3 %, pass 2; +38.6 ms, +5.7 %, pass 1 whose runs do not overlap: 671-682 against 711-718 ms). "Inside the spread" is therefore not why it is rejected: the
two clean runs of each core count sit 5-6 % apart. Three things reject it. (1) **A control that asks nothing.** `DELETE FROM p WHERE id >= 1` over the same rows asks 0 times and costs +16.7 ms (+33.7 %, pass 2) and +18.7 ms (+38.8 %, pass 1) more at
`cores = 4`; the `cores = 1` to `4` step is in the engine around the scan, and on this statement it is larger in relative terms than the `PURGE`'s. (2) **The profile** (pass 1, 1 kHz, the `PURGE` alone). 94-95 % of its samples are four functions: `PayloadAt` 51 / 48 %, `SearchLeaf` 18 / 23 %, `memcpy` 14 / 15 %, `SlotKeystoneId` 11 / 9 % at `cores` 4 / 1.
`InFlight` is **1 sample of 695** at `cores = 4` and none of 679 at `cores = 1` (resolution 0.14 %). (3) **Arithmetic from the counters**: three more relaxed loads per row at `cores = 4`, at an assumed 5 ns each, about 1.5 ms of 717 (0.2 %), an estimate and not a measurement.

*Table: PURGE of 100,000 delete-marked rows, `relaxed`, pass 1 (1592007e)*

| cores | run | session on core | load 1 min before | PURGE wall | PURGE rows/s (derived: 100,000 / wall) | PURGE server CPU | CPU per row | asks | misses | ids read per miss (pass 2 only) | `DELETE` of the same rows: wall / server CPU / asks |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | r0 | core 0 | 1.48 load | 670.9 ms | 149,054 rows/s | 671.1 ms | 6.71 µs | 100,000 asks | 100,000 misses | - | 48.2 ms / 48.2 ms / 0 asks |
| 1 | r1 | core 0 | 1.13 load | 674.2 ms | 148,324 rows/s | 674.5 ms | 6.75 µs | 100,000 asks | 100,000 misses | - | 48.6 ms / 48.6 ms / 0 asks |
| 1 | r2 | core 0 | 0.81 load | 682.0 ms | 146,628 rows/s | 682.2 ms | 6.82 µs | 100,000 asks | 100,000 misses | - | 48.2 ms / 48.1 ms / 0 asks |
| 4 | r0 | core 3 | 1.34 load | 699.2 ms | 143,021 rows/s | 717.6 ms | 7.18 µs | 100,000 asks | 100,000 misses | - | 64.3 ms / 65.8 ms / 0 asks |
| 4 | r1 | core 1 | 0.96 load | 693.3 ms | 144,238 rows/s | 710.8 ms | 7.11 µs | 100,000 asks | 100,000 misses | - | 66.9 ms / 68.5 ms / 0 asks |
| 4 | r2 | core 2 | 0.99 load | 695.1 ms | 143,864 rows/s | 713.1 ms | 7.13 µs | 100,000 asks | 100,000 misses | - | 65.3 ms / 66.9 ms / 0 asks |

| cores | PURGE CPU, median (min-max) | PURGE CPU, the two lowest runs | DELETE CPU, median (min-max) | k4 - k1, PURGE | k4 - k1, DELETE (no in-flight ask) |
|---|---|---|---|---|---|
| 1 | 674.5 ms (671.1-682.2) | [671.1, 674.5] | 48.2 ms (48.1-48.6) | - | - |
| 4 | 713.1 ms (710.8-717.6) | [710.8, 713.1] | 66.9 ms (65.8-68.5) | - | - |
| | | | | +38.6 ms (+5.7 %) | +18.7 ms (+38.8 %) |

Named-key `INSERT` loops (20,000 statements per loop, one session, a fresh table):
| cores | loop | runs | stmt/s median (min-max) | server CPU per stmt median (min-max) | p0 | p25 | p50 | p95 | p99 | max | in-flight asks | errors per run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | fresh keys | 3 | 22,812 stmt/s (22,498-22,916) | 20.9 µs (20.8-21.4) | 33.8 µs | 40.8 µs | 41.2 µs | 53.3 µs | 64.5 µs | n/r | 0 asks | [0, 0, 0] errors |
| 1 | the same keys again (duplicates) | 3 | 19,796 stmt/s (19,793-20,196) | 23.4 µs (23.1-23.6) | 37.7 µs | 45.6 µs | 46.1 µs | 62.9 µs | 121.7 µs | n/r | 0 asks | [20000, 20000, 20000] errors |
| 4 | fresh keys | 3 | 21,807 stmt/s (16,199-22,084) | 24.3 µs (23.8-34.7) | 36.7 µs | 42.0 µs | 42.5 µs | 57.5 µs | 68.7 µs | n/r | 0 asks | [0, 0, 0] errors |
| 4 | the same keys again (duplicates) | 3 | 19,470 stmt/s (13,882-19,497) | 25.9 µs (25.9-38.3) | 38.7 µs | 46.1 µs | 46.9 µs | 65.3 µs | 121.6 µs | n/r | 0 asks | [20000, 20000, 20000] errors |

A named key meeting another session's undecided insert of the same key (20 waits of 200 ms per run):
| cores | in-flight asks per run | misses | ids read | session cores |
|---|---|---|---|---|
| 1 | [20, 20, 20] asks | [0, 0, 0] misses | [20, 20, 20] ids or tables read | [0, 0, 0] core |
| 4 | [20, 20, 20] asks | [0, 0, 0] misses | [40, 40, 20] ids or tables read | [3, 1, 2] core |

The last column of the contended-insert table counts what each pass's counter counts: pass 2 counts ids read on a miss, so a hit reads none (0); pass 1 counted tables read (1.0 per ask at `cores = 1`, 1.0-2.0 per ask at `cores = 4`, depending on which core the session landed on).

**The 1,024-slot cap.** No cell exceeds about ten concurrent transactions on an instance (eight sessions, a fence holder, the control). The case that could bind, many sessions on one core (BL's session skew), is the cell that was not run, so the cap leg of H9 is neither
approached nor settled. **Verdict**: rejected for the scan; BP-Q7 takes (a), keep.

## 5. BP-H10: a held fence makes every tuple ask walk `64 x cores` latched partitions, and at `cores = 8` that is +12 % CPU

**Finding.** `FenceCoversKey` runs on 0.65-0.67 of the statements in a fence cell (the fence is on the benchmark's relation; the control arms `select-hot` and `ping` do not take a tuple unit) and latches every partition: 64 at `cores = 1` (where the latches are null and
the walk is 64 reads), 256 at `cores = 4`, 512 at `cores = 8`. The plain cells make no walk at all (fence walks 0), so the pair isolates the walk. Server CPU per statement, paired by run:

*Table: pass 2 (ccf9cfc1): lock cell, server CPU per statement (3 runs per row; r0, r1, r2) and the fence's cost, paired by run*

| durability | cores | partitions | plain: CPU per stmt (r0, r1, r2) | fence: CPU per stmt (r0, r1, r2) | paired delta fence - plain (r0, r1, r2) | delta of medians | fence walks per stmt | latch acquisitions a walk makes | bench wall (plain / fence, median) |
|---|---|---|---|---|---|---|---|---|---|
| `relaxed` | 1 | 64 partitions | [31.1, 31.4, 31.2] µs | [31.8, 31.5, 31.4] µs | [0.6, 0.1, 0.2] µs ([2.0, 0.3, 0.7] %) | +0.3 µs (+1.0 %) | 0.65 walks/stmt | 64 reads, no latch | 3.8 s / 3.9 s |
| `relaxed` | 4 | 256 partitions | [30.0, 33.7, 30.3] µs | [30.0, 35.9, 29.8] µs | [0.0, 2.2, -0.5] µs ([0.1, 6.6, -1.5] %) | -0.2 µs (-0.8 %) | 0.65 walks/stmt | 256 latches | 3.0 s / 2.8 s |
| `relaxed` | 8 | 512 partitions | [46.8, 49.9, 46.6] µs | [52.4, 55.3, 52.0] µs | [5.7, 5.4, 5.4] µs ([12.2, 10.7, 11.6] %) | +5.7 µs (+12.2 %) | 0.66 walks/stmt | 512 latches | 5.8 s / 5.9 s |
| `group` | 1 | 64 partitions | [37.2, 36.7, 37.0] µs | [37.8, 37.2, 37.3] µs | [0.6, 0.5, 0.3] µs ([1.7, 1.3, 0.8] %) | +0.3 µs (+0.8 %) | 0.67 walks/stmt | 64 reads, no latch | 28.0 s / 23.8 s |
| `group` | 4 | 256 partitions | [60.9, 62.5, 54.4] µs | [59.3, 59.1, 126.8] µs | [-1.6, -3.4, 72.4] µs ([-2.6, -5.5, 133.1] %) | -1.6 µs (-2.6 %) | 0.67 walks/stmt | 256 latches | 21.9 s / 24.7 s |
| `group` | 8 | 512 partitions | [88.6, 103.1, 374.7] µs | [87.6, 96.5, 301.5] µs | [-1.0, -6.6, -73.2] µs ([-1.2, -6.4, -19.5] %) | -6.6 µs (-6.4 %) | 0.67 walks/stmt | 512 latches | 25.9 s / 25.9 s |

At `cores = 8` `relaxed` the rise is +5.4 to +5.7 us (+10.7 to +12.2 %) in each of the three pairs; the largest plain run (49.9 us) is below the smallest fence run (52.0 us). That is 0.66 walks x 512 latches = 338 latch acquisitions per statement for
+5.7 us, about 17 ns each. At `cores = 4` the same walk is 167 acquisitions per statement and the pairs read +0.0, +2.2 and -0.5 us: **not seen**, and not reconciled with `cores = 8` by a constant per-acquisition cost (that would predict +2.8 us). `group` carries the same pairs and
nothing readable, because its durability wait buries them and a device stall hit four of its runs (CPU per statement 126.8, 374.7 and 301.5 us in three of them).

**Throughput and tail do not clear the floor.** The 5 % leg of BP-R2 asks for a throughput or p99 change against the cost removed. At `cores = 8` `relaxed` `update-disjoint` reads -3.6 % and +2.9 %; the same arm run twice in one cell
differs by 0.8 % (plain) and 0.1 % (fence), but between runs the arm spans 14,712-18,406 stmt/s, the control arm `ping`, which never touches the fence, moves -2.8 % in throughput and +8.8 % at p99, and at `cores = 4` the fence cell is 7-11 % *faster* on every arm including the
controls. At `cores` 1 and 4 one client CPU drives all eight sessions, so a client ceiling cannot be excluded and throughput there is not a server measure. **Only the CPU leg is met.**

*Table: pass 2: fence - plain, per arm, relaxed (ratio of medians over the 3 runs; the spread column is the same arm repeated inside one run, `update-disjoint` vs `update-disjoint-again`)*

| cores | arm | throughput plain -> fence | p99 plain -> fence | per-run throughput ratio fence/plain (r0, r1, r2) |
|---|---|---|---|---|
| 1 | update-hot | 29,770 -> 29,859 stmt/s (+0.3 %) | 503 -> 515 µs (+2.4 %) | [0.983, 1.003, 1.157] |
| 1 | update-disjoint | 29,846 -> 29,448 stmt/s (-1.3 %) | 502 -> 516 µs (+2.8 %) | [0.984, 1.799, 0.712] |
| 1 | update-disjoint-again | 29,776 -> 29,669 stmt/s (-0.4 %) | 498 -> 522 µs (+4.7 %) | [0.996, 1.022, 0.637] |
| 1 | insert-omitted | 30,471 -> 30,174 stmt/s (-1.0 %) | 491 -> 509 µs (+3.6 %) | [1.036, 0.739, 0.98] |
| 1 | select-hot | 19,952 -> 19,721 stmt/s (-1.2 %) | 770 -> 759 µs (-1.5 %) | [1.008, 0.986, 1.0] |
| 1 | ping | 28,485 -> 28,614 stmt/s (+0.5 %) | 496 -> 495 µs (-0.1 %) | [0.992, 1.012, 1.0] |
| 4 | update-hot | 35,164 -> 38,543 stmt/s (+9.6 %) | 488 -> 445 µs (-8.8 %) | [1.028, 0.972, 1.119] |
| 4 | update-disjoint | 36,542 -> 40,407 stmt/s (+10.6 %) | 429 -> 425 µs (-0.9 %) | [0.991, 1.173, 1.113] |
| 4 | update-disjoint-again | 36,622 -> 39,996 stmt/s (+9.2 %) | 438 -> 448 µs (+2.2 %) | [0.986, 1.408, 1.104] |
| 4 | insert-omitted | 37,994 -> 40,629 stmt/s (+6.9 %) | 431 -> 467 µs (+8.2 %) | [0.977, 0.985, 1.078] |
| 4 | select-hot | 27,581 -> 29,818 stmt/s (+8.1 %) | 616 -> 606 µs (-1.6 %) | [1.002, 1.104, 1.09] |
| 4 | ping | 34,139 -> 36,994 stmt/s (+8.4 %) | 461 -> 452 µs (-2.0 %) | [1.084, 0.967, 1.113] |
| 8 | update-hot | 17,692 -> 17,502 stmt/s (-1.1 %) | 1,382 -> 1,372 µs (-0.7 %) | [0.989, 0.987, 0.965] |
| 8 | update-disjoint | 18,022 -> 17,376 stmt/s (-3.6 %) | 1,290 -> 1,328 µs (+2.9 %) | [0.964, 1.15, 0.978] |
| 8 | update-disjoint-again | 18,169 -> 17,363 stmt/s (-4.4 %) | 1,354 -> 1,493 µs (+10.3 %) | [0.956, 0.967, 0.976] |
| 8 | insert-omitted | 18,336 -> 17,938 stmt/s (-2.2 %) | 1,431 -> 1,427 µs (-0.3 %) | [0.978, 0.988, 0.97] |
| 8 | select-hot | 14,096 -> 14,052 stmt/s (-0.3 %) | 1,802 -> 1,760 µs (-2.3 %) | [0.997, 1.042, 0.988] |
| 8 | ping | 17,406 -> 16,911 stmt/s (-2.8 %) | 1,297 -> 1,412 µs (+8.8 %) | [1.005, 0.965, 0.997] |

**The distributions.** `relaxed`, three runs per row, medians of each percentile (the `update-hot` arm's conflicts are its design):

*Table: pass 2: `relaxed`, the statement distributions per arm (median over 3 runs of each percentile; max is the largest of the runs; the p99 spread is min-max over runs)*

| cores | fence | arm | runs | stmt/s median (min-max) | p0 | p25 | p50 | p95 | p99 (min-max) | max | errors per run |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | plain | update-hot | 3 runs | 29,770 stmt/s (25,980-29,838) | 46.8 µs | 222.2 µs | 250.8 µs | 384.4 µs | 503.0 µs (499-510) | 85,793 µs | [0, 0, 0] errors |
| 1 | plain | update-disjoint | 3 runs | 29,846 stmt/s (16,365-30,125) | 46.0 µs | 222.3 µs | 251.2 µs | 383.0 µs | 502.2 µs (495-509) | 247,686 µs | [0, 0, 0] errors |
| 1 | plain | update-disjoint-again | 3 runs | 29,776 stmt/s (29,492-30,059) | 47.2 µs | 223.0 µs | 252.1 µs | 386.7 µs | 498.4 µs (498-512) | 6,822 µs | [0, 0, 0] errors |
| 1 | plain | insert-omitted | 3 runs | 30,471 stmt/s (29,677-30,794) | 43.9 µs | 214.5 µs | 245.2 µs | 372.0 µs | 491.4 µs (489-500) | 8,564 µs | [0, 0, 0] errors |
| 1 | plain | select-hot | 3 runs | 19,952 stmt/s (19,574-19,998) | 69.9 µs | 359.7 µs | 393.6 µs | 543.6 µs | 770.3 µs (747-778) | 1,665 µs | [0, 0, 0] errors |
| 1 | plain | ping | 3 runs | 28,485 stmt/s (27,712-28,838) | 58.0 µs | 211.0 µs | 235.8 µs | 477.7 µs | 496.0 µs (490-524) | 1,057 µs | [0, 0, 0] errors |
| 1 | fence | update-hot | 3 runs | 29,859 stmt/s (29,317-30,071) | 45.9 µs | 220.1 µs | 249.0 µs | 390.2 µs | 515.3 µs (501-526) | 9,777 µs | [0, 0, 0] errors |
| 1 | fence | update-disjoint | 3 runs | 29,448 stmt/s (21,241-29,649) | 47.2 µs | 221.4 µs | 251.1 µs | 387.1 µs | 516.4 µs (508-518) | 225,422 µs | [0, 0, 0] errors |
| 1 | fence | update-disjoint-again | 3 runs | 29,669 stmt/s (19,143-30,152) | 47.1 µs | 220.4 µs | 250.0 µs | 394.8 µs | 521.6 µs (514-534) | 302,302 µs | [0, 0, 0] errors |
| 1 | fence | insert-omitted | 3 runs | 30,174 stmt/s (22,513-30,758) | 41.7 µs | 214.2 µs | 245.5 µs | 381.9 µs | 509.2 µs (498-511) | 191,077 µs | [0, 0, 0] errors |
| 1 | fence | select-hot | 3 runs | 19,721 stmt/s (19,710-19,946) | 69.1 µs | 362.0 µs | 397.9 µs | 553.0 µs | 758.6 µs (755-759) | 1,837 µs | [0, 0, 0] errors |
| 1 | fence | ping | 3 runs | 28,614 stmt/s (27,704-28,837) | 57.7 µs | 208.8 µs | 234.5 µs | 474.5 µs | 495.3 µs (492-522) | 1,492 µs | [0, 0, 0] errors |
| 4 | plain | update-hot | 3 runs | 35,164 stmt/s (35,119-37,480) | 31.0 µs | 158.4 µs | 200.0 µs | 357.1 µs | 488.4 µs (460-494) | 6,848 µs | [251, 1585, 478] errors |
| 4 | plain | update-disjoint | 3 runs | 36,542 stmt/s (30,581-40,756) | 28.9 µs | 144.9 µs | 178.6 µs | 317.4 µs | 429.1 µs (418-461) | 143,275 µs | [0, 0, 0] errors |
| 4 | plain | update-disjoint-again | 3 runs | 36,622 stmt/s (25,393-40,544) | 29.9 µs | 145.5 µs | 180.4 µs | 332.6 µs | 437.9 µs (414-1,023) | 201,460 µs | [0, 0, 0] errors |
| 4 | plain | insert-omitted | 3 runs | 37,994 stmt/s (37,368-41,606) | 29.6 µs | 161.3 µs | 196.8 µs | 332.9 µs | 431.4 µs (411-436) | 6,926 µs | [0, 0, 0] errors |
| 4 | plain | select-hot | 3 runs | 27,581 stmt/s (26,894-29,770) | 44.3 µs | 224.9 µs | 271.1 µs | 460.0 µs | 616.2 µs (601-619) | 3,480 µs | [0, 0, 0] errors |
| 4 | plain | ping | 3 runs | 34,139 stmt/s (33,294-35,110) | 44.6 µs | 182.7 µs | 222.9 µs | 359.6 µs | 460.9 µs (460-467) | 4,142 µs | [0, 0, 0] errors |
| 4 | fence | update-hot | 3 runs | 38,543 stmt/s (34,143-39,351) | 29.7 µs | 153.0 µs | 188.3 µs | 334.8 µs | 445.4 µs (438-518) | 6,711 µs | [284, 1966, 235] errors |
| 4 | fence | update-disjoint | 3 runs | 40,407 stmt/s (35,871-40,685) | 33.0 µs | 148.7 µs | 184.1 µs | 321.8 µs | 425.2 µs (409-462) | 8,224 µs | [0, 0, 0] errors |
| 4 | fence | update-disjoint-again | 3 runs | 39,996 stmt/s (35,764-40,434) | 33.9 µs | 148.3 µs | 185.2 µs | 326.7 µs | 447.5 µs (437-471) | 7,304 µs | [0, 0, 0] errors |
| 4 | fence | insert-omitted | 3 runs | 40,629 stmt/s (36,809-40,955) | 30.0 µs | 146.3 µs | 182.2 µs | 322.2 µs | 466.7 µs (446-501) | 9,056 µs | [0, 0, 0] errors |
| 4 | fence | select-hot | 3 runs | 29,818 stmt/s (29,686-30,071) | 43.9 µs | 209.6 µs | 251.1 µs | 435.1 µs | 606.2 µs (567-959) | 4,341 µs | [0, 0, 0] errors |
| 4 | fence | ping | 3 runs | 36,994 stmt/s (33,944-37,040) | 44.3 µs | 162.2 µs | 201.7 µs | 342.8 µs | 451.8 µs (443-911) | 3,642 µs | [0, 0, 0] errors |
| 8 | plain | update-hot | 3 runs | 17,692 stmt/s (14,825-18,264) | 44.8 µs | 245.6 µs | 378.7 µs | 962.8 µs | 1,381.9 µs (1,277-1,500) | 12,306 µs | [1079, 1495, 1013] errors |
| 8 | plain | update-disjoint | 3 runs | 18,022 stmt/s (14,712-18,406) | 45.1 µs | 238.2 µs | 366.4 µs | 907.6 µs | 1,290.5 µs (1,255-1,338) | 266,203 µs | [0, 0, 0] errors |
| 8 | plain | update-disjoint-again | 3 runs | 18,169 stmt/s (16,382-18,363) | 46.4 µs | 237.5 µs | 365.5 µs | 921.2 µs | 1,353.8 µs (1,275-1,418) | 13,076 µs | [0, 0, 0] errors |
| 8 | plain | insert-omitted | 3 runs | 18,336 stmt/s (16,490-18,879) | 42.3 µs | 231.7 µs | 354.3 µs | 926.9 µs | 1,431.0 µs (1,264-1,467) | 8,092 µs | [0, 0, 0] errors |
| 8 | plain | select-hot | 3 runs | 14,096 stmt/s (12,666-14,720) | 53.7 µs | 306.5 µs | 462.6 µs | 1,194.4 µs | 1,801.8 µs (1,582-2,296) | 4,970 µs | [0, 0, 0] errors |
| 8 | plain | ping | 3 runs | 17,406 stmt/s (16,820-17,697) | 58.2 µs | 254.8 µs | 384.6 µs | 954.8 µs | 1,297.0 µs (1,286-2,004) | 4,322 µs | [0, 0, 0] errors |
| 8 | fence | update-hot | 3 runs | 17,502 stmt/s (14,629-17,615) | 47.9 µs | 249.0 µs | 385.2 µs | 963.8 µs | 1,371.9 µs (1,312-1,538) | 5,879 µs | [1400, 1687, 1377] errors |
| 8 | fence | update-disjoint | 3 runs | 17,376 stmt/s (16,914-17,994) | 47.8 µs | 234.2 µs | 363.5 µs | 948.0 µs | 1,328.0 µs (1,290-2,264) | 148,946 µs | [0, 0, 0] errors |
| 8 | fence | update-disjoint-again | 3 runs | 17,363 stmt/s (15,833-17,931) | 47.8 µs | 247.6 µs | 377.8 µs | 988.4 µs | 1,493.4 µs (1,314-2,008) | 5,586 µs | [0, 0, 0] errors |
| 8 | fence | insert-omitted | 3 runs | 17,938 stmt/s (16,293-18,322) | 48.3 µs | 238.8 µs | 369.4 µs | 984.0 µs | 1,427.4 µs (1,281-1,839) | 6,703 µs | [0, 0, 0] errors |
| 8 | fence | select-hot | 3 runs | 14,052 stmt/s (13,200-14,550) | 53.5 µs | 303.6 µs | 465.8 µs | 1,213.6 µs | 1,760.4 µs (1,644-2,351) | 4,464 µs | [0, 0, 0] errors |
| 8 | fence | ping | 3 runs | 16,911 stmt/s (16,789-17,637) | 58.3 µs | 257.4 µs | 387.9 µs | 991.4 µs | 1,411.7 µs (1,297-1,724) | 4,565 µs | [0, 0, 0] errors |

`group`, same layout. The statement is its commit wait: p50 of `update-disjoint` is 2.0-2.2 ms against 0.18-0.37 ms at `relaxed`.

*Table: pass 2: `group`, the statement distributions per arm (median over 3 runs of each percentile; max is the largest of the runs; the p99 spread is min-max over runs)*

| cores | fence | arm | runs | stmt/s median (min-max) | p0 | p25 | p50 | p95 | p99 (min-max) | max | errors per run |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | plain | update-hot | 3 runs | 2,483 stmt/s (342-2,652) | 1,069.3 µs | 2,104.8 µs | 2,182.6 µs | 5,527.8 µs | 11,758.9 µs (10,060-597,675) | 918,759 µs | [0, 0, 0] errors |
| 1 | plain | update-disjoint | 3 runs | 2,696 stmt/s (2,540-3,290) | 1,122.2 µs | 2,070.1 µs | 2,137.4 µs | 5,129.4 µs | 10,111.5 µs (8,978-10,971) | 290,281 µs | [0, 0, 0] errors |
| 1 | plain | update-disjoint-again | 3 runs | 2,520 stmt/s (675-2,672) | 1,066.9 µs | 2,074.8 µs | 2,145.8 µs | 5,488.5 µs | 11,227.7 µs (10,561-167,776) | 802,486 µs | [0, 0, 0] errors |
| 1 | plain | insert-omitted | 3 runs | 2,486 stmt/s (771-2,766) | 1,094.5 µs | 2,044.6 µs | 2,115.4 µs | 4,964.4 µs | 10,725.9 µs (8,258-199,843) | 839,970 µs | [0, 0, 0] errors |
| 1 | plain | select-hot | 3 runs | 19,762 stmt/s (19,708-19,797) | 69.4 µs | 363.2 µs | 399.0 µs | 545.6 µs | 758.1 µs (752-762) | 1,676 µs | [0, 0, 0] errors |
| 1 | plain | ping | 3 runs | 28,656 stmt/s (28,428-28,712) | 58.2 µs | 211.5 µs | 235.4 µs | 476.3 µs | 494.9 µs (494-497) | 946 µs | [0, 0, 0] errors |
| 1 | fence | update-hot | 3 runs | 3,097 stmt/s (2,646-3,149) | 1,118.3 µs | 2,125.1 µs | 2,197.9 µs | 4,964.8 µs | 9,822.8 µs (9,189-10,672) | 24,135 µs | [0, 0, 0] errors |
| 1 | fence | update-disjoint | 3 runs | 2,769 stmt/s (2,715-3,158) | 1,134.0 µs | 2,154.4 µs | 2,230.2 µs | 4,666.1 µs | 9,563.3 µs (8,690-10,312) | 282,115 µs | [0, 0, 0] errors |
| 1 | fence | update-disjoint-again | 3 runs | 2,770 stmt/s (2,649-3,264) | 1,119.4 µs | 2,158.2 µs | 2,238.2 µs | 4,546.4 µs | 9,494.4 µs (8,442-10,282) | 286,725 µs | [0, 0, 0] errors |
| 1 | fence | insert-omitted | 3 runs | 3,082 stmt/s (2,779-3,172) | 1,110.9 µs | 2,133.8 µs | 2,214.8 µs | 4,523.5 µs | 8,686.5 µs (8,388-8,824) | 31,402 µs | [0, 0, 0] errors |
| 1 | fence | select-hot | 3 runs | 19,791 stmt/s (19,750-19,913) | 69.4 µs | 362.3 µs | 397.1 µs | 549.2 µs | 757.3 µs (736-771) | 1,866 µs | [0, 0, 0] errors |
| 1 | fence | ping | 3 runs | 28,555 stmt/s (28,519-28,736) | 59.7 µs | 211.4 µs | 234.8 µs | 475.5 µs | 493.5 µs (493-494) | 825 µs | [0, 0, 0] errors |
| 4 | plain | update-hot | 3 runs | 3,160 stmt/s (2,824-3,595) | 55.4 µs | 1,952.6 µs | 2,041.6 µs | 4,367.8 µs | 8,833.3 µs (7,692-8,987) | 320,685 µs | [242, 477, 227] errors |
| 4 | plain | update-disjoint | 3 runs | 3,059 stmt/s (2,715-3,567) | 1,015.6 µs | 1,948.5 µs | 2,038.0 µs | 4,408.0 µs | 9,971.7 µs (8,150-10,311) | 190,840 µs | [0, 0, 0] errors |
| 4 | plain | update-disjoint-again | 3 runs | 2,956 stmt/s (2,709-3,439) | 1,094.7 µs | 1,954.5 µs | 2,040.6 µs | 4,468.1 µs | 9,828.0 µs (9,766-9,931) | 228,679 µs | [0, 0, 0] errors |
| 4 | plain | insert-omitted | 3 runs | 3,391 stmt/s (2,759-3,466) | 1,026.4 µs | 1,926.1 µs | 2,010.4 µs | 4,296.1 µs | 9,464.2 µs (8,425-9,540) | 29,628 µs | [0, 0, 0] errors |
| 4 | plain | select-hot | 3 runs | 29,634 stmt/s (27,280-29,639) | 44.0 µs | 216.3 µs | 252.5 µs | 435.4 µs | 593.2 µs (578-618) | 3,891 µs | [0, 0, 0] errors |
| 4 | plain | ping | 3 runs | 36,441 stmt/s (34,700-36,690) | 44.3 µs | 165.0 µs | 203.8 µs | 345.3 µs | 448.0 µs (438-463) | 3,335 µs | [0, 0, 0] errors |
| 4 | fence | update-hot | 3 runs | 2,805 stmt/s (450-3,029) | 50.7 µs | 1,938.4 µs | 2,021.9 µs | 5,169.0 µs | 9,650.7 µs (9,403-299,251) | 760,368 µs | [504, 533, 556] errors |
| 4 | fence | update-disjoint | 3 runs | 2,717 stmt/s (409-3,088) | 1,113.3 µs | 1,963.6 µs | 2,048.9 µs | 5,465.0 µs | 10,245.9 µs (10,016-299,953) | 1,140,149 µs | [0, 0, 0] errors |
| 4 | fence | update-disjoint-again | 3 runs | 2,724 stmt/s (400-3,194) | 1,050.3 µs | 1,938.3 µs | 2,038.3 µs | 5,124.2 µs | 10,040.0 µs (8,750-302,627) | 611,098 µs | [0, 0, 0] errors |
| 4 | fence | insert-omitted | 3 runs | 2,820 stmt/s (432-2,967) | 1,045.4 µs | 1,929.4 µs | 2,043.5 µs | 4,974.3 µs | 9,993.5 µs (9,811-266,647) | 601,044 µs | [0, 0, 0] errors |
| 4 | fence | select-hot | 3 runs | 28,904 stmt/s (28,818-29,579) | 42.9 µs | 208.9 µs | 251.4 µs | 438.4 µs | 612.9 µs (608-657) | 4,331 µs | [0, 0, 0] errors |
| 4 | fence | ping | 3 runs | 34,516 stmt/s (33,774-35,713) | 43.5 µs | 180.3 µs | 216.0 µs | 362.0 µs | 461.4 µs (461-467) | 3,946 µs | [0, 0, 0] errors |
| 8 | plain | update-hot | 3 runs | 2,949 stmt/s (442-3,225) | 70.3 µs | 1,986.6 µs | 2,106.1 µs | 4,733.6 µs | 10,300.2 µs (10,193-321,939) | 393,374 µs | [580, 732, 978] errors |
| 8 | plain | update-disjoint | 3 runs | 2,796 stmt/s (356-2,829) | 1,001.7 µs | 2,039.2 µs | 2,173.0 µs | 4,972.8 µs | 11,435.9 µs (9,601-333,022) | 606,268 µs | [0, 0, 0] errors |
| 8 | plain | update-disjoint-again | 3 runs | 2,668 stmt/s (381-3,214) | 1,072.4 µs | 2,022.2 µs | 2,166.8 µs | 5,631.6 µs | 10,110.1 µs (8,866-298,574) | 609,447 µs | [0, 0, 0] errors |
| 8 | plain | insert-omitted | 3 runs | 2,612 stmt/s (276-2,870) | 1,009.7 µs | 2,013.0 µs | 2,327.3 µs | 6,062.6 µs | 10,182.2 µs (10,085-335,209) | 398,432 µs | [0, 0, 0] errors |
| 8 | plain | select-hot | 3 runs | 14,189 stmt/s (13,789-14,447) | 53.7 µs | 303.6 µs | 462.6 µs | 1,209.6 µs | 1,707.4 µs (1,629-2,240) | 18,752 µs | [0, 0, 0] errors |
| 8 | plain | ping | 3 runs | 16,996 stmt/s (16,318-17,285) | 59.5 µs | 253.0 µs | 383.9 µs | 1,003.9 µs | 1,434.8 µs (1,425-1,784) | 4,949 µs | [0, 0, 0] errors |
| 8 | fence | update-hot | 3 runs | 2,840 stmt/s (606-3,606) | 70.9 µs | 1,931.6 µs | 2,075.2 µs | 5,447.6 µs | 9,606.2 µs (7,806-256,755) | 756,358 µs | [955, 961, 1110] errors |
| 8 | fence | update-disjoint | 3 runs | 3,080 stmt/s (2,705-3,322) | 984.1 µs | 1,916.9 µs | 2,026.1 µs | 4,274.2 µs | 10,732.4 µs (9,700-11,077) | 242,665 µs | [0, 0, 0] errors |
| 8 | fence | update-disjoint-again | 3 runs | 2,723 stmt/s (339-3,553) | 972.3 µs | 2,008.4 µs | 2,223.1 µs | 5,078.3 µs | 10,206.7 µs (8,834-319,628) | 2,246,385 µs | [0, 0, 0] errors |
| 8 | fence | insert-omitted | 3 runs | 2,736 stmt/s (328-3,076) | 981.4 µs | 2,068.1 µs | 2,348.4 µs | 5,067.4 µs | 10,128.2 µs (8,869-331,734) | 510,052 µs | [0, 0, 0] errors |
| 8 | fence | select-hot | 3 runs | 14,091 stmt/s (13,933-14,553) | 54.1 µs | 299.7 µs | 458.9 µs | 1,206.0 µs | 1,688.6 µs (1,639-2,501) | 5,960 µs | [0, 0, 0] errors |
| 8 | fence | ping | 3 runs | 16,894 stmt/s (16,808-17,161) | 58.6 µs | 255.0 µs | 384.0 µs | 984.1 µs | 1,376.2 µs (1,344-1,884) | 5,162 µs | [0, 0, 0] errors |

*Table: pass 2: waits the lock cell can name (per statement; SHOW META deltas over the cell)*

| durability | cores | fence | partition latch waits per stmt | partition latch wait per stmt | window latch waits | structural reruns | memo drops per run | sessions' cores (distinct, of cores) |
|---|---|---|---|---|---|---|---|---|
| `relaxed` | 1 | plain | 0.00e+00 waits/stmt | 0.000 µs | [0, 0, 0] waits | 0 reruns | [1, 1, 1] drops | [1, 1, 1] cores of 1 |
| `relaxed` | 1 | fence | 0.00e+00 waits/stmt | 0.000 µs | [0, 0, 0] waits | 0 reruns | [1, 1, 1] drops | [1, 1, 1] cores of 1 |
| `relaxed` | 4 | plain | 1.40e-03 waits/stmt | 0.001 µs | [133, 364, 133] waits | 0 reruns | [4, 4, 4] drops | [3, 3, 4] cores of 4 |
| `relaxed` | 4 | fence | 3.20e-03 waits/stmt | 0.001 µs | [157, 677, 126] waits | 0 reruns | [5, 4, 5] drops | [3, 3, 4] cores of 4 |
| `relaxed` | 8 | plain | 1.73e-02 waits/stmt | 0.012 µs | [445, 502, 558] waits | 0 reruns | [8, 8, 8] drops | [5, 6, 5] cores of 8 |
| `relaxed` | 8 | fence | 2.31e-02 waits/stmt | 0.027 µs | [774, 760, 787] waits | 0 reruns | [9, 9, 9] drops | [5, 6, 7] cores of 8 |
| `group` | 1 | plain | 0.00e+00 waits/stmt | 0.000 µs | [0, 0, 0] waits | 0 reruns | [1, 1, 1] drops | [1, 1, 1] cores of 1 |
| `group` | 1 | fence | 0.00e+00 waits/stmt | 0.000 µs | [0, 0, 0] waits | 0 reruns | [1, 1, 1] drops | [1, 1, 1] cores of 1 |
| `group` | 4 | plain | 2.28e-03 waits/stmt | 0.001 µs | [257, 404, 161] waits | 0 reruns | [4, 4, 4] drops | [3, 3, 4] cores of 4 |
| `group` | 4 | fence | 3.89e-03 waits/stmt | 0.001 µs | [292, 390, 343] waits | 0 reruns | [7, 7, 7] drops | [3, 3, 3] cores of 4 |
| `group` | 8 | plain | 1.44e-02 waits/stmt | 0.012 µs | [386, 438, 721] waits | 0 reruns | [8, 8, 8] drops | [5, 6, 5] cores of 8 |
| `group` | 8 | fence | 1.58e-02 waits/stmt | 0.008 µs | [475, 588, 710] waits | 0 reruns | [11, 11, 14] drops | [5, 5, 4] cores of 8 |

The latch wait the partitions themselves cost is small: 0.012 us per statement plain and 0.027 us with the fence at `cores = 8` (medians; 1,665 and 2,218 waits per 96,000 statements). The cost is the walk, not the contention. The sessions land on 4-7 of the 8 cores, so the table is read
with one to four reactors idle.

**Against pass 1.** The fence's cost at `cores = 8` reproduces (pass 1: +6.0 us, +13.2 %; pass 2: +5.7 us, +12.2 %); at `cores = 4` it does not (pass 1: +3.1 us, +11.1 %, all three pairs positive; pass 2: -0.2 us).

*Table: pass 1 (1592007e): lock cell, server CPU per statement (3 runs per row; r0, r1, r2) and the fence's cost, paired by run*

| durability | cores | partitions | plain: CPU per stmt (r0, r1, r2) | fence: CPU per stmt (r0, r1, r2) | paired delta fence - plain (r0, r1, r2) | delta of medians | fence walks per stmt | latch acquisitions a walk makes | bench wall (plain / fence, median) |
|---|---|---|---|---|---|---|---|---|---|
| `relaxed` | 1 | 64 partitions | [31.7, 31.5, 31.2] µs | [31.6, 31.6, 31.8] µs | [-0.2, 0.2, 0.6] µs ([-0.5, 0.5, 1.8] %) | +0.2 µs (+0.5 %) | 0.65 walks/stmt | 64 reads, no latch | 3.7 s / 3.7 s |
| `relaxed` | 4 | 256 partitions | [27.9, 28.0, 29.0] µs | [31.1, 32.9, 31.1] µs | [3.2, 4.9, 2.0] µs ([11.4, 17.6, 7.0] %) | +3.1 µs (+11.1 %) | 0.65 walks/stmt | 256 latches | 2.9 s / 2.9 s |
| `relaxed` | 8 | 512 partitions | [45.9, 45.6, 45.2] µs | [51.8, 51.7, 51.6] µs | [5.9, 6.0, 6.4] µs ([12.9, 13.2, 14.2] %) | +6.0 µs (+13.2 %) | 0.66 walks/stmt | 512 latches | 5.6 s / 5.7 s |
| `group` | 1 | 64 partitions | [37.2, 37.2, 37.2] µs | [38.0, 37.8, 37.9] µs | [0.7, 0.6, 0.7] µs ([2.0, 1.7, 1.8] %) | +0.7 µs (+1.8 %) | 0.67 walks/stmt | 64 reads, no latch | 26.5 s / 26.4 s |
| `group` | 4 | 256 partitions | [59.2, 58.1, 60.2] µs | [62.8, 59.9, 59.4] µs | [3.6, 1.8, -0.8] µs ([6.1, 3.1, -1.3] %) | +0.7 µs (+1.2 %) | 0.67 walks/stmt | 256 latches | 26.0 s / 25.6 s |
| `group` | 8 | 512 partitions | [84.8, 83.3, 82.8] µs | [83.8, 84.6, 89.4] µs | [-1.0, 1.3, 6.6] µs ([-1.2, 1.5, 8.0] %) | +1.3 µs (+1.5 %) | 0.67 walks/stmt | 512 latches | 25.7 s / 25.4 s |

**On the hot-path counter.** Pass 1 had a counter on the walk, one `Add` per partition visited (512 per walk at `cores = 8`, 256 at `cores = 4`), deleted at `676d1d7d`. It has been offered as the reason pass 1 read +11 % at `cores = 4`. The data do not support it as the whole reason: if the counter cost what
would explain +3.1 us at `cores = 4`, deleting it should have taken about twice that off the `cores = 8` pair, and the pair moved from +6.0 to +5.7 us. And of the 3.3 us by which the `cores = 4` pair changed between the passes (+3.1 to -0.2), 2.3 us is the plain cell, which was *lower* in pass 1 (28.0
against 30.3 us); only 1.1 us is the fence cell (31.1 to 30.0 us). What the second pass settles is `cores = 8`; `cores = 4` is split between the passes and stays open. BP-H10 is held on the pass-2 `cores = 8` pairs, which do not depend on that question.

**Verdict.** Held at `cores = 8` on BP-R2's 3 % CPU leg, with a caveat BP-R2 as written does not weigh: the cell holds a fence for its whole duration, and how often a production relation holds one is not measured. BP-Q15 (a), a fixed count of 64, is adopted; BP-S6 runs, and its A/B is the
measurement. BP-Q15's figure for partition contention at `cores = 8` (0.015 us of latch wait per statement) is confirmed (0.012 us median, 0.0146 us mean, plain).

## 6. Where the time goes

| Unit | Wait types and their shares | Source |
|---|---|---|
| A single-row `INSERT` under `group` (growth cell) | The commit wait is 97-98 % of p50: 2,418-2,520 us against 51-78 us at `relaxed` for the same statement, `cores` 1-4. Socket send is 35 % of the `relaxed` statement's CPU (7,524 of 21,713 samples at `cores = 4`) | growth tables, sampler |
| A point `UPDATE` in the lock cell, `group` | Commit wait 83-91 % of p50 (2,137 / 2,038 / 2,173 us against 251 / 179 / 366 us at `relaxed`, `cores` 1 / 4 / 8). At `relaxed` the control arm (`SHOW META`, no relation) is 235.8 us at `cores = 1` against the update's 251.2 us, so the round trip and the common statement path are about 94 % and the update's own part about 15 us | lock tables |
| The same, lock and conflict wait | Partition latch wait 0.012 us per statement at `cores = 8` (0.03 % of 47 us); the `update-hot` arm's conflicts are errors, not waits | lock waits table |
| A checkpoint run | Parked on the log gate 77-84 %, on a data sync 7-10 %, page writes 1-2 %, publish 1-2 %, start 1-2 %, the rest 5-8 % at `cores` 2-4; the run is cooperative, so the parks do not hold the reactor | run decomposition |
| The checkpoint cell's insert load, instance-wide, `cores = 4` | Frame-table latch 43.7 s (4.5 % of 968 core-seconds, 52 million waits, about 2.5 per inserted row), WAL stream 13.4-16.8 s, WAL sync gate 2.1 s, window 1.1 s, **lock partition 0.02 s** | checkpoint waits table |
| `PURGE` of 100,000 rows, `relaxed` | CPU-bound: wall 673 ms, CPU 673.5 ms at `cores = 1`; no durability wait (`relaxed`) and no lock wait measured | H9 table |
| One memo drop | CPU, not wait: +41.7 us server CPU against +43.1 us wall | one-drop table |
| The foreground probe | No decomposition applies: it is a point read queued behind whatever its core is running; the in-run and out-of-run split is the only attribution | probe tables |

`SHOW META` has no counter for client or socket time, so "client and socket round trip" is read only from the control arm above, and the write-statement and read shares inside a statement's CPU come from the sampler where one ran.

*Table: the instance's waits over the cell (SHOW META `contention`, whole life of the server)*

| cell | frame table | WAL stream | WAL sync gate | window | lock partition | page latch spin turns | share of core-seconds in frame-table waits |
|---|---|---|---|---|---|---|---|
| k1-a | 0 waits, 0.00 s | 0 waits, 0.00 s | 107 waits, 2.10 s | 0 waits, 0.000 s | 0 waits, 0.0000 s | 0 turns | 0.00 % |
| k1-b | 0 waits, 0.00 s | 0 waits, 0.00 s | 90 waits, 13.11 s | 0 waits, 0.000 s | 0 waits, 0.0000 s | 0 turns | 0.00 % |
| k2-a | 26,695,153 waits, 10.91 s | 502,554 waits, 17.94 s | 210 waits, 2.48 s | 6,562 waits, 1.115 s | 126,296 waits, 0.0776 s | 15,159,512 turns | 2.25 % |
| k2-b | 39,643,991 waits, 12.38 s | 627,129 waits, 7.59 s | 285 waits, 3.12 s | 7,811 waits, 0.479 s | 200,377 waits, 0.0271 s | 21,125,117 turns | 2.56 % |
| k4-a | 52,075,461 waits, 43.66 s | 1,125,696 waits, 13.38 s | 221 waits, 2.12 s | 15,065 waits, 1.105 s | 130,795 waits, 0.0207 s | 67,180,847 turns | 4.51 % |
| k4-b | 52,669,371 waits, 43.69 s | 1,112,699 waits, 16.77 s | 212 waits, 2.12 s | 14,891 waits, 1.184 s | 129,212 waits, 0.0191 s | 66,889,228 turns | 4.51 % |

## 7. Against the first pass, and against the engine's last files

The census has no earlier file of its own shape, so it is the baseline the next run is read against. For the shapes the older results files measured, the nearest numbers are: `PURGE` at `cores = 1`, `relaxed`, a range window, per key
6.00 / 5.82 / 5.71 us for 200 / 1,000 / 10,000-key windows (`bench/v3.0.0/results-bh-close-v2.7.0-712-g4015736d.md`, elapsed time per key, a 30,000-row relation), against **6.74-6.82 us per key here at a 100,000-key window** (CPU, the two
clean runs of pass 2), so the per-key cost does not stay flat to 100,000 keys; `bi_checkpoint_stall.py` `cores = 4`, `group`, 600 s cells (`results-bi-s5-checkpoint-stall-v2.7.0-784-gec827a19.md`, B side): 469 / 401 / 425 runs per cell
(472 / 383 per 600 s here), statement probe p99 in a run 177-265 ms and worst anchor gap 15.5-20.8 s, against 37.8 ms and 1.9 s here at 240 s. Those files are 125 (`-784`) and 197 (`-712`) commits earlier by describe count, with other cell lengths and loads;
they are context and not a delta.

| Shape | Metric | Pass 1 `1592007e` | Pass 2 `ccf9cfc1` | Read as |
|---|---|---|---|---|
| Checkpoint, `cores = 2` | rows/s; runs per 5 s; longest gap | 88,764 rows/s; 1.98; 3.35 s | 84,403 / 86,571 rows/s; 1.98 / 1.98; 3.99 / 3.45 s | same within the repeat spread |
| Checkpoint, `cores = 4` | rows/s; runs per 5 s; longest gap | 85,005 rows/s; 3.94; 1.91 s | 85,133 / 83,695 rows/s; 3.94 / 3.19 (stall); 1.93 / 6.61 s (stall) | same, clean cell |
| Checkpoint, `cores = 1` | rows/s | 73,865 rows/s; gap 5.60 s | 63,479 / 32,546 rows/s, gap 11.8 / 15.3 s | pass 2 stalled; not a delta |
| `PURGE` 100,000 rows, `cores` 1 / 4 | rows/s (derived: 100,000 / wall); CPU per row | 148,300 / 143,900 rows/s (median wall); 6.75 / 7.13 us | 146,800-148,600 / 143,200-143,900 rows/s (the clean runs); 6.74-6.82 / 7.16-7.18 us (outlier runs 7.98 and 8.42 us) | same within 1 % on the clean runs |
| `DELETE` of those rows | CPU, `cores` 1 / 4 | 48.2 / 66.9 ms | 49.5 / 66.2 ms (outlier runs 85.3 / 107.6 ms) | same |
| Lock cell, `relaxed`, plain, `update-disjoint` | stmt/s; CPU per stmt (`cores` 1 / 4 / 8) | see the pass-1 table above | 29,846 / 36,542 / 18,022 stmt/s; 31.2 / 30.3 / 46.8 us | plain +2.3 us at `cores = 4` against pass 1; others within 1.2 us |
| Lock cell fence cost | CPU per stmt, fence - plain (`cores` 1 / 4 / 8) | +0.2 / +3.1 / +6.0 us | +0.3 / -0.2 / +5.7 us | `cores = 8` reproduces, `cores = 4` does not |
| Named-key loop, fresh keys, `cores = 1` | stmt/s | 22,812 stmt/s | 20,505 / 19,924 / 22,381 stmt/s | pass 2's loop is noisy (1 s per loop); not a finding |

## 8. What the run teaches about the engine

- **The memo's invalidation is a rare event.** A relation grows a level once within its first 6,900 rows and a second time before 324,000, and each growth costs the other cores one re-resolve. The structure CLAUDE.md's Transactional DDL row describes ("every core's
  catalog asks the instance's schema version word at its task boundaries") pays at most `cores - 1` drops per move, and a drop costs 30-97 us once, on the statement that meets it. Nothing in this census asks for the (b) or (d) options.
- **Per-core checkpoints are cheap in I/O and expensive only in coordination.** Each core writes the pages its sessions dirtied, so pages per row move by at most 18 % from one to four cores, while the runs and their syncs scale with `cores`. A run is mostly a wait for the log, not
  work; the reactor is not held. What BP-H7 proposed to save is therefore small, and what it would cost (publishing undo heads so recovery can start from one instance-wide anchor) is a recovery-correctness change.
- **The ratified in-flight table is not on any hot path this census could find.** `InFlight` is a rounding error in `PURGE`, the only per-row asker; the named-key path asks only under conflict. AX's trade (a table per core, a scan on a miss) is not paid for in any cell here.
- **The lock partition count is the one ratified sizing whose cost shows here, and it shows through the walk, not through contention.** `64 x cores` was sized under ownership on AO-S7's evidence. Partition latch waits are 0.012 us per statement; the cost is the walk, each latch an
  acquisition of about 17 ns at `cores = 8`, paid only while a fence is held, because the plain path skips the walk. The cost is a property of one state multiplied by the core count, which would explain why `cores = 4` hides it and `cores = 8` shows it.
- **The cost of a fence is not linear in partitions on this machine.** 167 acquisitions per statement cost nothing measurable at `cores = 4` in pass 2 while 338 cost 5.7 us at `cores = 8`; the difference between the two cells is eight reactors on four physical cores with the client on the same CPUs. Whether the walk is dear because
  of the count or because of the SMT sharing is not separated by this census, and BP-S6's A/B at the fixed count of 64 is the cell that separates it.
- **The next contention on the insert path is not in BP's list.** At `cores = 4` four loaders inserting into one relation spend 4.5 % of all core-seconds waiting for the frame-table latch (52 million waits, about 2.5 per row) and the WAL stream latch another 1.4 %; the lock partitions spend 0.002 %. That is an observation from
  one cell, not an experiment, but it says where the next gain on this workload is.
- **The largest tails in this census were host stalls, and the chain's gate did not see them.** The two `k1` checkpoint cells (I/O pressure recorded at 75 % before one of them), four `group` lock runs (two flagged beforehand by a slow fresh-server checkpoint) and one 1.6 s stall in a `k4` checkpoint cell (nothing flagged it)
  all passed a gate on load average and compile processes. A census that measures at the extreme should record `/proc/pressure/io` per cell, as the checkpoint driver does, and gate on it.

## 9. What this census did not do

- **Rule 9's row-set sweep (200 / 1,000 / 10,000 rows) was not run.** No cell here varies its cardinality; the growth cell passes through all of them within a run and records only totals. H9's `PURGE` asks once per key by construction, so its ask count is cardinality-free,
  and the nearest sweep of the per-key cost, three window sizes at `cores = 1`, is BH's close (above); H10's walk is per tuple ask, not per row, and the lock relation holds 256 rows. These are arguments and not evidence.
- No skew cell, so H8 and H9's cap are open. No reader against the growth cell. No eight-run interleaved A/B; no measurement with the structure removed by instrument other than the cadence for H7.
- The `cores = 8` cell oversubscribes four physical cores, so the +12 % is measured on a machine where eight reactors and their client contend for the same caches; this host has no more physical cores to test it on.
- BP-G1's cells (point `SELECT` and `UPDATE`, every-row `UPDATE`, Cabin `SELECT`, `group` and `strict` commit loops) are BP-S3's own A/B and are not in this file. The `cores = 1` baselines this census happens to carry are the growth cell (`relaxed`: 32,426 stmt/s, p50 51.4 us;
  `group`: 691 stmt/s, p50 2,520 us), the lock cell's plain `relaxed` update (29,846 stmt/s, 31.2 us of server CPU per statement) and the `PURGE` rows above.
- No test suite was run for this file.

## 10. Host, per cell

The top CPU consumers on every pass-1 stamp were three or four `claude` sessions (each under 7 % of a CPU) and the unrelated `kds_server` on port 15432 under `/home/cdkbs/autotrade` (0.7 %), which this census never connected to. CPU pressure `some` before a lock cell reads
14-30 % where the previous cell ran seconds earlier on the same CPUs: that is the census's own load. Pass 2's stamps carry load and filesystem; the kernel's I/O pressure is recorded for the checkpoint cells only (table in §2).

*Table: pass 1 host stamps (one per cell, from `hoststamps.log`)*

| cell | UTC | load 1 / 5 / 15 min | cpu pressure `some` avg10 / avg60 | competing compile/test processes |
|---|---|---|---|---|
| cell1 group k1 s2 | 2026-10-12 08:49:19 | 1.84 / 4.27 / 5.28 load | 0.65 / 3.21 % | none |
| cell1 group k2 s4 | 2026-10-12 08:50:31 | 1.25 / 3.57 / 4.96 load | 0.32 / 1.08 % | none |
| cell1 group k4 s8 | 2026-10-12 08:51:43 | 1.41 / 3.14 / 4.70 load | 0.01 / 0.43 % | none |
| cell1 relaxed k1 s2 | 2026-10-12 08:52:59 | 1.61 / 2.77 / 4.44 load | 1.02 / 0.92 % | none |
| cell1 relaxed k2 s4 | 2026-10-12 08:54:13 | 2.36 / 2.76 / 4.31 load | 1.67 / 1.30 % | none |
| cell1 relaxed k4 s8 | 2026-10-12 08:55:49 | 3.33 / 2.90 / 4.21 load | 1.80 / 1.35 % | none |
| prof relaxed insertN k4 | 2026-10-12 09:01:25 | 0.79 / 2.00 / 3.46 load | 0.02 / 0.95 % | none |
| prof relaxed insertN k1 | 2026-10-12 09:01:52 | 3.63 / 2.56 / 3.60 load | 15.49 / 5.69 % | none |
| reresolve probe | 2026-10-12 09:03:08 | 1.57 / 2.16 / 3.38 load | 0.11 / 1.97 % | none |
| cell4 inflight relaxed k1 r0 | 2026-10-12 09:03:15 | 1.48 / 2.12 / 3.36 load | 0.20 / 1.76 % | none |
| cell4 inflight relaxed k4 r0 | 2026-10-12 09:03:26 | 1.34 / 2.07 / 3.33 load | 0.45 / 1.58 % | none |
| cell4 inflight relaxed k1 r1 | 2026-10-12 09:03:37 | 1.13 / 2.00 / 3.29 load | 0.52 / 1.38 % | none |
| cell4 inflight relaxed k4 r1 | 2026-10-12 09:03:47 | 0.96 / 1.93 / 3.25 load | 0.61 / 1.26 % | none |
| cell4 inflight relaxed k1 r2 | 2026-10-12 09:03:58 | 0.81 / 1.87 / 3.22 load | 0.52 / 1.13 % | none |
| cell4 inflight relaxed k4 r2 | 2026-10-12 09:04:08 | 0.99 / 1.87 / 3.20 load | 0.39 / 0.99 % | none |
| cell4 purge under sampler k1 | 2026-10-12 09:04:20 | 1.33 / 1.91 / 3.20 load | 0.64 / 0.96 % | none |
| cell4 purge under sampler k4 | 2026-10-12 09:04:25 | 1.30 / 1.90 / 3.18 load | 0.35 / 0.87 % | none |
| cell5 locks group k1 plain r0 | 2026-10-12 09:04:30 | 1.28 / 1.88 / 3.17 load | 0.23 / 0.81 % | none |
| cell5 locks group k1 fence r0 | 2026-10-12 09:04:57 | 1.36 / 1.86 / 3.13 load | 2.19 / 1.07 % | none |
| cell5 locks group k4 plain r0 | 2026-10-12 09:05:25 | 2.39 / 2.05 / 3.15 load | 3.61 / 1.76 % | none |
| cell5 locks group k4 fence r0 | 2026-10-12 09:05:52 | 2.82 / 2.17 / 3.16 load | 6.31 / 3.48 % | none |
| cell5 locks group k8 plain r0 | 2026-10-12 09:06:18 | 2.51 / 2.16 / 3.13 load | 7.88 / 5.36 % | none |
| cell5 locks group k8 fence r0 | 2026-10-12 09:06:44 | 2.52 / 2.18 / 3.11 load | 2.71 / 4.51 % | none |
| cell5 locks relaxed k1 plain r0 | 2026-10-12 09:07:10 | 2.41 / 2.18 / 3.08 load | 1.85 / 3.40 % | none |
| cell5 locks relaxed k1 fence r0 | 2026-10-12 09:07:15 | 2.94 / 2.29 / 3.11 load | 14.04 / 5.61 % | none |
| cell5 locks relaxed k4 plain r0 | 2026-10-12 09:07:19 | 2.79 / 2.27 / 3.10 load | 22.83 / 7.81 % | none |
| cell5 locks relaxed k4 fence r0 | 2026-10-12 09:07:23 | 2.79 / 2.27 / 3.10 load | 26.86 / 9.48 % | none |
| cell5 locks relaxed k8 plain r0 | 2026-10-12 09:07:26 | 3.36 / 2.40 / 3.14 load | 30.01 / 11.16 % | none |
| cell5 locks relaxed k8 fence r0 | 2026-10-12 09:07:33 | 3.25 / 2.39 / 3.13 load | 18.64 / 10.55 % | none |
| cell5 locks group k1 plain r1 | 2026-10-12 09:07:39 | 2.98 / 2.36 / 3.11 load | 12.57 / 10.02 % | none |
| cell5 locks group k1 fence r1 | 2026-10-12 09:08:06 | 2.46 / 2.28 / 3.07 load | 1.87 / 6.87 % | none |
| cell5 locks group k4 plain r1 | 2026-10-12 09:08:33 | 2.14 / 2.23 / 3.03 load | 2.45 / 5.39 % | none |
| cell5 locks group k4 fence r1 | 2026-10-12 09:09:00 | 2.66 / 2.32 / 3.03 load | 7.02 / 6.26 % | none |
| cell5 locks group k8 plain r1 | 2026-10-12 09:09:26 | 2.22 / 2.25 / 2.99 load | 11.34 / 7.68 % | none |
| cell5 locks group k8 fence r1 | 2026-10-12 09:09:53 | 2.19 / 2.23 / 2.96 load | 2.49 / 5.46 % | none |
| cell5 locks relaxed k1 plain r1 | 2026-10-12 09:10:20 | 2.27 / 2.24 / 2.94 load | 1.83 / 3.95 % | none |
| cell5 locks relaxed k1 fence r1 | 2026-10-12 09:10:25 | 2.25 / 2.23 / 2.93 load | 14.56 / 6.25 % | none |
| cell5 locks relaxed k4 plain r1 | 2026-10-12 09:10:29 | 2.14 / 2.21 / 2.92 load | 22.56 / 8.27 % | none |
| cell5 locks relaxed k4 fence r1 | 2026-10-12 09:10:33 | 2.14 / 2.21 / 2.92 load | 26.75 / 9.94 % | none |
| cell5 locks relaxed k8 plain r1 | 2026-10-12 09:10:36 | 2.69 / 2.33 / 2.96 load | 28.20 / 11.27 % | none |
| cell5 locks relaxed k8 fence r1 | 2026-10-12 09:10:43 | 2.96 / 2.39 / 2.97 load | 18.09 / 10.74 % | none |
| cell5 locks group k1 plain r2 | 2026-10-12 09:10:49 | 3.12 / 2.43 / 2.98 load | 12.39 / 10.23 % | none |
| cell5 locks group k1 fence r2 | 2026-10-12 09:11:17 | 2.29 / 2.29 / 2.92 load | 4.78 / 7.37 % | none |
| cell5 locks group k4 plain r2 | 2026-10-12 09:11:44 | 1.85 / 2.19 / 2.87 load | 1.34 / 5.22 % | none |
| cell5 locks group k4 fence r2 | 2026-10-12 09:12:11 | 1.75 / 2.14 / 2.83 load | 10.72 / 6.97 % | none |
| cell5 locks group k8 plain r2 | 2026-10-12 09:12:37 | 1.64 / 2.08 / 2.79 load | 11.89 / 7.91 % | none |
| cell5 locks group k8 fence r2 | 2026-10-12 09:13:02 | 1.61 / 2.04 / 2.76 load | 2.78 / 5.67 % | none |
| cell5 locks relaxed k1 plain r2 | 2026-10-12 09:13:28 | 1.40 / 1.95 / 2.71 load | 1.85 / 4.16 % | none |
| cell5 locks relaxed k1 fence r2 | 2026-10-12 09:13:33 | 1.29 / 1.92 / 2.69 load | 13.91 / 6.31 % | none |
| cell5 locks relaxed k4 plain r2 | 2026-10-12 09:13:37 | 1.90 / 2.04 / 2.73 load | 22.63 / 8.43 % | none |
| cell5 locks relaxed k4 fence r2 | 2026-10-12 09:13:41 | 2.47 / 2.15 / 2.76 load | 25.98 / 9.90 % | none |
| cell5 locks relaxed k8 plain r2 | 2026-10-12 09:13:44 | 3.08 / 2.28 / 2.80 load | 29.54 / 11.59 % | none |
| cell5 locks relaxed k8 fence r2 | 2026-10-12 09:13:51 | 3.23 / 2.33 / 2.81 load | 18.68 / 11.00 % | none |
| cell2 cabinauto group k1 r0 | 2026-10-12 09:13:57 | 3.29 / 2.36 / 2.82 load | 12.59 / 10.43 % | none |
| cell2 cabinauto group k4 r0 | 2026-10-12 09:18:34 | 0.24 / 1.07 / 2.15 load | 0.00 / 0.13 % | none |
| cell2 cabinauto group k1 r1 | 2026-10-12 09:23:18 | 2.80 / 2.17 / 2.34 load | 21.67 / 7.86 % | none |
| cell2 cabinauto group k4 r1 | 2026-10-12 09:27:55 | 0.22 / 1.00 / 1.78 load | 0.00 / 0.15 % | none |
| cell2 cabinauto group k1 r2 | 2026-10-12 09:32:40 | 3.00 / 2.26 / 2.12 load | 1.82 / 2.33 % | none |
| cell2 cabinauto group k4 r2 | 2026-10-12 09:37:17 | 1.63 / 1.38 / 1.75 load | 0.00 / 0.04 % | 504342 cmake --build /home/cdkbs/ckdbs/.claude/worktrees/wal-segment-roll-order/ |
| cell3 checkpoint group k1 600s run0 | 2026-10-12 09:42:10 | 9.23 / 6.70 / 3.99 load | 3.54 / 15.28 % | 507967 ctest --test-dir /home/cdkbs/ckdbs/.claude/worktrees/wal-segment-roll-ord |
| cell3 checkpoint group k1 600s run0 attempt1 | 2026-10-12 09:48:32 | 1.28 / 4.84 / 4.28 load | 0.00 / 0.02 % | none |
| cell3 checkpoint group k1 600s run0 attempt2 | 2026-10-12 09:54:40 | 1.53 / 3.65 / 3.99 load | 0.00 / 0.00 % | none |
| cell3 checkpoint group k1 600s run0 attempt3 | 2026-10-12 09:55:16 | 1.40 / 3.38 / 3.89 load | 0.02 / 0.05 % | none |
| cell3 checkpoint group k1 600s run0 attempt4 | 2026-10-12 10:02:03 | 1.26 / 3.83 / 4.19 load | 0.00 / 0.06 % | none |
| cell3 checkpoint group k1 240s run0 attempt1 | 2026-10-12 10:35:54 | 1.14 / 3.80 / 4.10 load | 0.00 / 0.00 % | none |
| cell3 checkpoint group k2 240s run0 attempt1 | 2026-10-12 10:41:02 | 1.13 / 2.65 / 3.55 load | 0.00 / 0.24 % | none |
| cell3 checkpoint group k4 240s run0 attempt1 | 2026-10-12 10:46:10 | 1.13 / 2.37 / 3.21 load | 0.00 / 0.05 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep a 240s attempt1 | 2026-10-12 10:52:33 | 0.59 / 1.98 / 2.85 load | 0.00 / 0.00 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep a 240s attempt1 | 2026-10-12 10:57:30 | 1.10 / 1.96 / 2.64 load | 0.00 / 0.09 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep b 240s attempt1 | 2026-10-12 11:02:33 | 1.20 / 1.97 / 2.49 load | 0.00 / 0.30 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep b 240s attempt1 | 2026-10-12 11:07:31 | 1.30 / 2.12 / 2.46 load | 0.00 / 0.27 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e1 150s attempt1 | 2026-10-12 11:12:37 | 1.10 / 1.98 / 2.34 load | 0.00 / 0.24 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e1 150s attempt1 | 2026-10-12 11:16:19 | 1.07 / 2.03 / 2.32 load | 0.00 / 0.20 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e2 150s attempt1 | 2026-10-12 11:19:47 | 1.10 / 1.97 / 2.26 load | 0.00 / 0.31 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e2 150s attempt1 | 2026-10-12 11:23:14 | 1.18 / 1.90 / 2.19 load | 0.00 / 0.32 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e3 150s attempt1 | 2026-10-12 11:26:46 | 1.24 / 1.96 / 2.18 load | 0.00 / 0.74 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e3 150s attempt1 | 2026-10-12 11:30:13 | 1.15 / 2.01 / 2.19 load | 0.00 / 0.27 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e4 150s attempt1 | 2026-10-12 11:33:35 | 1.20 / 1.90 / 2.12 load | 0.00 / 0.26 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e4 150s attempt1 | 2026-10-12 11:37:03 | 1.08 / 1.79 / 2.05 load | 0.00 / 0.27 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e5 150s attempt1 | 2026-10-12 11:40:40 | 1.19 / 1.91 / 2.07 load | 0.00 / 0.20 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e5 150s attempt1 | 2026-10-12 11:44:27 | 1.13 / 2.06 / 2.14 load | 0.00 / 0.22 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e6 150s attempt1 | 2026-10-12 11:47:59 | 1.42 / 2.06 / 2.15 load | 0.00 / 0.28 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e6 150s attempt1 | 2026-10-12 11:51:37 | 1.09 / 2.03 / 2.15 load | 0.00 / 0.30 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e7 150s attempt1 | 2026-10-12 11:55:14 | 1.11 / 2.08 / 2.18 load | 0.00 / 0.28 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e7 150s attempt1 | 2026-10-12 11:58:51 | 1.11 / 2.07 / 2.19 load | 0.00 / 0.26 % | none |
| cell3c checkpoint group k4 cadence 20000ms rep e8 150s attempt1 | 2026-10-12 12:02:38 | 1.08 / 2.04 / 2.18 load | 0.00 / 0.22 % | none |
| cell3c checkpoint group k4 cadence 5000ms rep e8 150s attempt1 | 2026-10-12 12:06:16 | 1.11 / 1.98 / 2.16 load | 0.00 / 0.29 % | none |
| cell2 rerun cabinauto group k4 r2 attempt1 | 2026-10-12 12:09:58 | 1.13 / 2.00 / 2.15 load | 0.00 / 0.24 % | none |
| cell2 rerun cabinauto group k4 r2 attempt2 | 2026-10-12 12:22:42 | 0.32 / 0.57 / 1.35 load | 0.00 / 0.00 % | none |

*Table: pass 2 host stamps (one per cell, from `hoststamps.log`; the cell's driver wrote its own before/after load and competing list)*

| cell | UTC at start | load 1 / 5 / 15 min | fs |
|---|---|---|---|
| h7 bp2-k1-cp5000-a | 2026-10-12 14:31:26 | 1.27 / 2.40 / 5.77 load | ext4 |
| h7 bp2-k2-cp5000-a | 2026-10-12 14:35:48 | 1.45 / 2.31 / 4.93 load | ext4 |
| h7 bp2-k4-cp5000-a | 2026-10-12 14:42:31 | 1.53 / 3.68 / 4.96 load | ext4 |
| h7 bp2-k1-cp5000-b | 2026-10-12 14:48:24 | 1.39 / 3.13 / 4.39 load | ext4 |
| h7 bp2-k2-cp5000-b | 2026-10-12 15:03:13 | 1.60 / 5.67 / 5.96 load | ext4 |
| h7 bp2-k4-cp5000-b | 2026-10-12 15:08:15 | 1.46 / 3.68 / 5.05 load | ext4 |
| h9 k1 r0 | 2026-10-12 15:14:49 | 1.55 / 3.04 / 4.38 load | ext4 |
| h9 k4 r0 | 2026-10-12 15:15:10 | 1.52 / 2.94 / 4.32 load | ext4 |
| h9 k1 r1 | 2026-10-12 15:15:20 | 1.44 / 2.87 / 4.28 load | ext4 |
| h9 k4 r1 | 2026-10-12 15:15:31 | 1.37 / 2.81 / 4.25 load | ext4 |
| h9 k1 r2 | 2026-10-12 15:15:43 | 1.39 / 2.77 / 4.22 load | ext4 |
| h9 k4 r2 | 2026-10-12 15:16:33 | 1.44 / 2.59 / 4.08 load | ext4 |
| h10 relaxed k1 plain r0 | 2026-10-12 15:16:45 | 1.33 / 2.51 / 4.03 load | ext4 |
| h10 relaxed k1 fence r0 | 2026-10-12 15:16:49 | 1.47 / 2.52 / 4.03 load | ext4 |
| h10 relaxed k4 plain r0 | 2026-10-12 15:16:54 | 1.43 / 2.50 / 4.01 load | ext4 |
| h10 relaxed k4 fence r0 | 2026-10-12 15:16:57 | 1.43 / 2.50 / 4.01 load | ext4 |
| h10 relaxed k8 plain r0 | 2026-10-12 15:21:11 | 1.55 / 2.71 / 3.81 load | ext4 |
| h10 relaxed k8 fence r0 | 2026-10-12 15:22:28 | 1.52 / 2.48 / 3.64 load | ext4 |
| h10 group k1 plain r0 | 2026-10-12 15:23:15 | 1.60 / 2.38 / 3.55 load | ext4 |
| h10 group k1 fence r0 | 2026-10-12 15:24:31 | 1.57 / 2.25 / 3.41 load | ext4 |
| h10 group k4 plain r0 | 2026-10-12 15:25:17 | 1.51 / 2.15 / 3.32 load | ext4 |
| h10 group k4 fence r0 | 2026-10-12 15:26:23 | 1.57 / 2.08 / 3.22 load | ext4 |
| h10 group k8 plain r0 | 2026-10-12 15:27:08 | 1.60 / 2.02 / 3.14 load | ext4 |
| h10 group k8 fence r0 | 2026-10-12 15:28:25 | 1.58 / 1.95 / 3.03 load | ext4 |
| h10 relaxed k1 plain r1 | 2026-10-12 15:55:23 | 1.60 / 5.21 / 6.59 load | ext4 |
| h10 relaxed k1 fence r1 | 2026-10-12 15:55:28 | 1.55 / 5.14 / 6.56 load | ext4 |
| h10 relaxed k4 plain r1 | 2026-10-12 15:55:53 | 1.60 / 4.87 / 6.43 load | ext4 |
| h10 relaxed k4 fence r1 | 2026-10-12 15:57:57 | 1.43 / 3.77 / 5.84 load | ext4 |
| h10 relaxed k8 plain r1 | 2026-10-12 16:01:11 | 1.53 / 2.84 / 5.08 load | ext4 |
| h10 relaxed k8 fence r1 | 2026-10-12 16:01:49 | 1.59 / 2.72 / 4.96 load | ext4 |
| h10 group k1 plain r1 | 2026-10-12 16:03:26 | 1.55 / 2.45 / 4.64 load | ext4 |
| h10 group k1 fence r1 | 2026-10-12 16:06:55 | 1.54 / 2.08 / 4.05 load | ext4 |
| h10 group k4 plain r1 | 2026-10-12 16:07:50 | 1.60 / 2.02 / 3.91 load | ext4 |
| h10 group k4 fence r1 | 2026-10-12 16:09:22 | 1.52 / 1.96 / 3.72 load | ext4 |
| h10 group k8 plain r1 | 2026-10-12 16:11:06 | 1.58 / 1.95 / 3.53 load | ext4 |
| h10 group k8 fence r1 | 2026-10-12 16:13:01 | 1.57 / 1.87 / 3.32 load | ext4 |
| h10 relaxed k1 plain r2 | 2026-10-12 16:13:44 | 1.54 / 1.84 / 3.24 load | ext4 |
| h10 relaxed k1 fence r2 | 2026-10-12 16:14:18 | 1.52 / 1.81 / 3.18 load | ext4 |
| h10 relaxed k4 plain r2 | 2026-10-12 16:15:13 | 1.59 / 1.82 / 3.11 load | ext4 |
| h10 relaxed k4 fence r2 | 2026-10-12 16:28:44 | 1.50 / 2.31 / 2.76 load | ext4 |
| h10 relaxed k8 plain r2 | 2026-10-12 16:28:48 | 1.50 / 2.31 / 2.76 load | ext4 |
| h10 relaxed k8 fence r2 | 2026-10-12 16:29:14 | 1.54 / 2.26 / 2.73 load | ext4 |
| h10 group k1 plain r2 | 2026-10-12 16:35:54 | 1.52 / 2.16 / 2.54 load | ext4 |
| h10 group k1 fence r2 | 2026-10-12 16:44:06 | 1.60 / 2.43 / 2.58 load | ext4 |
| h10 group k4 plain r2 | 2026-10-12 16:44:29 | 1.51 / 2.35 / 2.55 load | ext4 |
| h10 group k4 fence r2 | 2026-10-12 16:44:59 | 1.48 / 2.27 / 2.51 load | ext4 |
| h10 group k8 plain r2 | 2026-10-12 16:49:20 | 1.56 / 2.12 / 2.41 load | ext4 |
| h10 group k8 fence r2 | 2026-10-12 16:55:02 | 1.55 / 2.01 / 2.30 load | ext4 |
