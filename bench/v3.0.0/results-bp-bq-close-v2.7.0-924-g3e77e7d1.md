# The BP and BQ close: BP-S6's fence cost falls 6 %, the point statements and the four-core cells do not move, and the every-row UPDATE carries a +3-6 % that follows BQ's per-append instruments

**Thesis.** BP (`workorder-bp-per-core-residue.md`) and BQ (`workorder-bq-wal-segment-roll.md`) close together on
`worktree-bp-per-core-residue` at `3e77e7d1` (`v2.7.0-924-g3e77e7d1`), the merge that brought BR's work order into BP's branch;
the later commit `1fb71d1a` changes comments and texts only. CLAUDE.md step 3 asks for the overhead of a milestone
measured once, over its whole code change, with the opening commit as A and the closing commit as B. This file is that
measurement for both milestones, on data that already existed: **A** = `cf889b63` (`v2.7.0-902-gcf889b63`, BP's opening;
its binary is byte-identical to BQ's opening `59c4139f`), **Q** = `4cf30270` (BQ's instruments, BQ's only engine change),
**B** = `3e77e7d1` (the close), and **S6A** = `ccf9cfc1` (BP before BP-S6). All are Release builds from `git archive`
exports (`-O3 -DNDEBUG`). What the data say:

- **Nothing moved at `cores = 4`.** `point` (reads) and `insert1` (the monotonic insert) at eight sessions, `relaxed`:
  57,621 / 57,599 / 58,916 stmt/s and 70,445 / 70,251 / 70,600 stmt/s for A / Q / B, every percentile p0-p99 inside the
  run-to-run spread (0.1-8.1 % of throughput). The 0.37-0.40 s maximum of `insert1` tracks the longest transaction-id carve and is the same
  on all three binaries (§5).
- **Point statements at `cores = 1` did not move past the instrument's floor.** A to B: point `SELECT` -1.4 / +0.4 %,
  point `UPDATE` -2.5 / -2.5 % (literal / prepared server CPU), every interval over three runs contains zero. A to Q:
  +1.2..+2.9 %, also inside (§2).
- **BP-S6 is the one change that clears its floor, and only where it was aimed.** At `cores = 8` with one held range fence the
  server's CPU per statement falls **51.6 to 48.4 µs (-6.2 %)**, every B run below every S6A run; the fence's premium over a
  plain cell falls from **+13.6 % to +5.1 %**; lock-partition waits per statement in that cell fall 25 %. In the plain cell
  nothing moves (45.4 to 46.1 µs, +1.5 %, inside a 0.9 µs spread), and update-disjoint throughput does not move in either (§6).
- **The every-row UPDATE (10,000 rows) is +3 to +6 % above `ccf9cfc1` in every binary that carries BQ's instruments, and
  +1 to +3 % above the shared opening, which is the size of the instrument's floor.** Four pairs against `ccf9cfc1` (one
  on aligned builds, one with the lazy `RollRecord`), the pooled sessions of five binaries and a single-server profile
  pass agree on the first; the second is not separable from noise (the same three-of-three-runs-positive pattern appears
  in S6, a pair whose executed code is identical). About half of the +3-6 % sits in
  `WalManager::Append`'s own frame (1.05 % of samples in `ccf9cfc1`, 2.97-3.60 % in the three binaries with BQ's code,
  z = 3.9-4.8), and making the per-append `RollRecord` lazy did not remove it. The cause inside that frame is not found; this
  stays an **open item of the close** (§3).
- **Two instrument findings change how earlier files read.** The second server of every `bn_census` pair has a longest
  statement of 0.59-1.22 s in each of its 60-statement every-row arms (60 of 60 arms), the first server never above
  270 ms (0 of 60), and the binary does not matter: `6139c2de`, `d555cb55` and `3e77e7d1` each show it as the second server and not as
  the first (§4). And three runs replicate a session, not a binary: the same binary `cf889b63` reads 5.4 % higher in the
  three AQ sessions than in the six AB and S2S1 sessions (§3.2).

The floor was not measured: PostgreSQL is not part of this file. `group` and `strict` were not run; the durability axis of
this close is covered by the commit loops of `results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md` and by BQ's own files, not here (§10).

| Item | Value |
|---|---|
| Executed | 2026-10-12 UTC. Cells **19:25:42-20:23:56**: the close chain (`bn` pairs AB and AQ, `ba_census` at `cores = 4`, lock cells at `cores = 8`) 19:25:42-19:58:23; bisect 1 (S2S1, S4S6BQ) 19:58:56-20:00:44; bisect 2 (MERGE, S4, S6) 20:04:20-20:07:32; aligned builds 20:11:29-20:12:24; lazy-`RollRecord` build (PREMERGE_FIX, CLOSE_FIX) 20:19:16-20:21:24; four profile passes 20:22-20:23:56 (file times of the `.done` markers; the profile scripts keep no stamp file). **This file was written 2026-10-12 20:24-20:49 UTC from the existing data; no run, server, build or test was started in the session that wrote it** (only Python analysis under `nice`) |
| Branch / worktree | Measured commit `3e77e7d1` (`v2.7.0-924-g3e77e7d1`) on branch `worktree-bp-per-core-residue`. This file is written in worktree `bp-per-core-residue` at `1fb71d1a` (`v2.7.0-925-g1fb71d1a`), whose tree is **dirty**: uncommitted documents (CLAUDE.md, `docs/inflight/known-gaps.md`, `docs/spec/wal.md`, five files under `instructions/v3.0.0/`) and two header comments (`include/kds/wal/log_device.hpp`, `stream.hpp`); none of it is in a measured binary except `rollfix`, because every other measured binary was built from a `git archive` export (clean by construction). The later commit `1fb71d1a` changes comments (`latch.hpp`'s) and texts only |
| Binaries | Table below. Every server ran from a copy under `/home/cdkbs/bench-runs/` (`close/bin/`, `bq/bin/`, `bp-s1/v2/bin/`), never from a build tree; `ba_census` and the lock cell copy once more into their work directories and record `bin_sha256` in every JSON (A `09c82ae3`, Q `024ef4dc`, B `72e48fd3`, S6A `a6fc3c04`: all 18 + 12 cell JSONs carry the hash of the binary they were meant to run). **Every binary built from a commit is newer than that commit**, by 58 s (B) to 12 h (A, rebuilt from an export); in each case the export is byte-identical to `git archive` of the commit (sha256 prefixes in the table), so the binary is the commit's engine |
| Build | Release (`CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`) for every row but the two aligned builds, which add `-falign-functions=64 -falign-loops=64 -falign-jumps=16` |
| Device | `/dev/root`, **ext4** on `nvme0n1p1`; `df -T` of the work directory read `ext4` when this file was written, and the `ba_census` and lock JSONs record `/dev/root ext4` at run time (`bn_census` records none; its work directory is on the same filesystem). Data files and WAL directories under `/home/cdkbs/bench-runs/close/work/`; not tmpfs. 8 vCPU AMD EPYC 9V74 = 4 physical cores x 2 SMT (`thread_siblings_list` 0-1, 2-3, ...), Linux 7.0.0-1014-azure |
| Server configuration | `cores = 1` (`bn_census`), 4 (`ba_census`), 8 (lock cell); `durability = relaxed` everywhere; `buffer_pool_frames = 65536`; `log_level = warn`; every other key at its default (64 MiB segments, checkpoint cadence 5,000 ms, `waystone_recording` default). `bn_census`: both servers of a pair side by side, both pinned to CPU 2, the client on CPU 4, one busy at a time. `ba_census`: server CPUs 0,2,4,6, client CPUs 1,3,5,7. Lock cell: all eight CPUs |
| Cells | §0. Raw JSON and logs, the chain scripts and the scripts that produced every table: `bench/v3.0.0/archive/bp-bq-close-v2.7.0-924-g3e77e7d1/` (its `README.md` says how to re-run the tables) |
| Baseline | A of this file is the baseline of the milestone pair (CLAUDE.md step 3). The most recent same-shape numbers are in `results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md` (`bn_census`, `cores = 1`, 10,000 rows: `1592007e` and `ccf9cfc1`) and `results-bp-s1-census-v2.7.0-909-gccf9cfc1.md` (the `cores = 8` lock cell, `ccf9cfc1`); §9 sets this run against both. For `ba_census point` and `insert1` at `cores = 4` there is no earlier file for this shape and configuration: this file becomes the baseline the next is read against |
| Suite / overhead | **Suite not executed** (analysis of existing files; no engine code changed by this file). This file *is* the milestone-close overhead measurement of CLAUDE.md step 3, on A = `cf889b63` and B = `3e77e7d1` |

**Binaries (sha256 of the file measured; export = sha256 prefix of the `.tar` the binary was built from, equal to that of `git archive <commit>`)**

| Id | Commit and `git describe --tags` | Commit time | Binary mtime | Size | sha256 | Export |
|---|---|---|---|---|---|---|
| A | `cf889b63`, `v2.7.0-902-gcf889b63` | 07:21:31 | 19:23:22 | 5,766,400 B | `09c82ae38fd1ae4124b39263be51db23cb3322c97b75434aa41aa4cc5ee32892` | `8466a6e1...` |
| (BQ's opening) | `59c4139f`, `v2.7.0-906-g59c4139f` | 07:44:25 | 19:24:06 | 5,766,400 B | the same hash as A | `2761e0b0...` |
| `1592007e` | `v2.7.0-904-g1592007e` (BP-S1) | 08:42:17 | 14:11:04 | 5,760,672 B | `44b7c2e5e2bcd4093b62c83201e65d17bc17afb8add9264e6ed0d5abc22dfd8a` | `fb673f34...` |
| Q | `4cf30270`, `v2.7.0-909-g4cf30270` (BQ-S1) | 10:32:55 | 10:33:54 | 5,792,080 B | `024ef4dc863ebc6b164dd5a585a9fd6ccbfcf12817be1a0e1d29ef492d2e9540` | `0190fe9e...` (as recorded in `results-bq-s2-rung1-v2.7.0-909-g4cf30270.md`; the very copy BQ-S1 and S2 measured) |
| S6A = `ccf9cfc1` | `v2.7.0-909-gccf9cfc1` (BP-S3 head) | 13:46:17 | 14:11:04 | 5,756,304 B | `a6fc3c04ef2bf2e19e3715ca1943c689959c97e9aa59a57a205029da1aea111f` | `1eb04e67...` |
| `6139c2de` | `v2.7.0-917-g6139c2de` (BQ merged into BP) | 14:08:01 | 20:02:02 | 5,781,880 B | `cd75758c6e69db674cabe9db51e7411696a8254ae55bee42c7ae52ab677b8123` | `48cf1d58...` |
| `d555cb55` | `v2.7.0-920-gd555cb55` (BP-S4) | 17:12:32 | 20:02:46 | 5,782,048 B | `0a39e325fffea0d115c98a87f14dc7d4e4a0875ce53a833a3910f8284d7a826f` | `0475020d...` |
| B | `3e77e7d1`, `v2.7.0-924-g3e77e7d1` (the close) | 19:21:39 | 19:22:37 | 5,782,048 B | `72e48fd3f20b3f7a56130300e2cc2cb1de3128fc2a12cf4da47ad7146300f005` | `f589573b...` |
| `ccf9cfc1` aligned | as `ccf9cfc1`, `-falign-*` | | 20:09:17 | 5,957,008 B | `87aef827d7e853ca1dd888b7af4d7442d8ee98ab8e21b28bc61b57ac5f56e80a` | `1eb04e67...` |
| `6139c2de` aligned | as `6139c2de`, `-falign-*` | | 20:10:02 | 5,982,584 B | `c0db4fe2e75ee0e81bd939d09d8dad5ee7262cf4cba4b331314951a0f74109be` | `48cf1d58...` |
| `rollfix` | working tree of the worktree (HEAD `1fb71d1a`'s sources plus the two header comments above) with `WalManager::Append`'s per-append `RollRecord` made an `std::optional` filled only on a roll; **not a commit, reverted afterwards because it changed nothing; the patch is not archived** | | 20:17:30 | 5,782,072 B | `0d72c0d88487dfd6a3eed6204745b97bb623fbe6feefc00475ce987ef1ec1324` | none |

The `1592007e` export prefix is as recorded in `results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md`; the other prefixes were
recomputed for this file from the `.tar` and from `git archive` of the commit, and agree. The five binaries of the
`bn_census` lineage differ in size by at most 36 KB (0.6 %) from one another, and the two aligned builds are 3.5 %
larger than their unaligned twins.

## 0. What was measured, and how to read the tables

**Cells.** Every command is the one the chain scripts ran (archived under `archive/bp-bq-close-v2.7.0-924-g3e77e7d1/driver/`); the drivers are the unmodified
`tools/` scripts of the worktree (`tools/` carried no uncommitted change), plus `lockcell.py`, the wrapper BP-S1 wrote
outside `tools/` to start a lock cell and read `SHOW META` around it. `bench/docs/` stays closed (`bench/README.md`); the
drivers' flags are documented by their own docstrings.

```
# cores = 1: two servers side by side, every arm cut into 12 blocks taken alternately (3 runs per pair)
taskset -c 4 python3 tools/bn_census.py --bin <first> --bin-b <second> --workdir <w> --port 32111 --pin-server 2 \
    --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 --blocks 12 --runs 1 --only select-pk,update-lit,update-range
#   (the bisect pairs: --only select-pk,update-range)
# cores = 4: one server, 8 sessions, 20 s after 2 s of warm-up, the three binaries rotated A Q B / Q B A / B A Q
python3 tools/ba_census.py --shape {point|insert1} --durability relaxed --cores 4 --sessions 8 --seconds 20 --warmup 2 \
    --runs 1 --bin <b> --port 32121 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --workdir <w> --out <d> --max-wait 0
# cores = 8: BP-S6's cell, plain and with one range fence held on the benchmark's relation (3 runs each, S6A and B adjacent)
taskset -c 0-7 python3 lockcell.py --bin <b> --workdir <w> --port 32131 --cores 8 --server-cpus 0,1,2,3,4,5,6,7 \
    --durability relaxed --run <r> [--fence] --out <d>
# profile passes: one server under the LD_PRELOAD sampler, 120 every-row statements per path
taskset -c 4 python3 tools/bn_census.py --bin <b> --workdir <w> --port 32161 --pin-server 2 --pin-driver 4 --rows 10000 \
    --ops 3000 --range-ops 60 --runs 1 --sampler libkdssampler.so --sampler-dir <d> --profile-ops 3000 \
    --profile-range-ops 120 --profile-only update-range --only update-range
```

| Pair or cell | First server (`--bin`) | Second server (`--bin-b`) | What lies between | Runs | Arms |
|---|---|---|---|---|---|
| **AB** | A `cf889b63` | B `3e77e7d1` | BP's whole change and the merged BQ | 3 | 12 per run (3 shapes x 2 driver paths x 2 servers) |
| **AQ** | A `cf889b63` | Q `4cf30270` | BQ's instruments alone | 3 | 12 per run |
| S2S1 | A `cf889b63` | `1592007e` | BP-S2 (retired WAL kinds, dead code) and BP-S1 (counters) | 3 | 8 per run |
| S4S6BQ | `ccf9cfc1` | B `3e77e7d1` | the BQ merge, BP-S4, BP-S6 | 3 | 8 per run |
| MERGE | `ccf9cfc1` | `6139c2de` | BQ's engine change merged into BP's branch (12 files under `src/` and `include/`, all BQ's) | 3 | 8 per run |
| S4 | `6139c2de` | `d555cb55` | BP-S4, the delete-mark purge on core 0's tick (6 files: catalog, dispatcher, transaction manager, expeditor) | 3 | 8 per run |
| S6 | `d555cb55` | B `3e77e7d1` | BP-S6, 64 lock partitions at every core count (3 files; **no executed change at `cores = 1`**: the table is 64 x 1 = 64 partitions either way) and BR's work order merged | 3 | 8 per run |
| MERGEALIGNED | `ccf9cfc1` aligned | `6139c2de` aligned | the BQ merge, both built with aligned functions, loops and jumps | 3 | 8 per run |
| PREMERGE_FIX | `ccf9cfc1` | `rollfix` | the BQ merge with `RollRecord` made lazy, against BP-S3 | 3 | 8 per run |
| CLOSE_FIX | B `3e77e7d1` | `rollfix` | the lazy `RollRecord` alone | 3 | 8 per run |
| `ba_census point`, `insert1` | one server | A, Q, B | | 3 each | 18 cells |
| lock cell, plain and fence | one server | S6A, B | BP-S6 | 3 each | 12 cells |
| profile passes | one server under the sampler | `ccf9cfc1`, `6139c2de`, `rollfix`, B | | 1 each (2 dumps) | 4 passes |

`select-pk` (`SELECT * FROM t WHERE id = ?`) and `update-lit` (`UPDATE t SET v = ? WHERE id = ?`) run 3,000 statements per
arm, `update-range` (`UPDATE r SET v = ? WHERE id >= 1`, every row of a 10,000-row relation) 60. AB and AQ ran all three
over both driver paths (literal: an unnamed `C_PARSE` per execution; prepared: a named parse and a bind per execution),
12 arms per run (one per server); the bisect pairs ran `select-pk` and `update-range`, 8 arms per run. Every one of the 264
arms of 30 `bn_census` runs returned the tag its shape must produce (`bad = 0`), every every-row arm's end state read back correctly, and 10,000
further updates of an unspilled and a spilled row allocated 0 pages on every server. Every one of the 18 `ba_census`
cells recorded 0 errors and 0 refusals and `client_bound = false` (the busiest client thread was at 0.44 of a CPU).

**Rows.** The cores = 1 relations hold **10,000 rows** (`--rows 10000`); a pk point statement does not scale with the
row count, and the every-row `UPDATE` does (it writes 10,000 rows). **The 200 and 1,000-row sizes were not run**, so
the fixed and the per-row parts of the every-row figure are not separated; the per-row cost quoted in §3 divides a
whole statement by 10,000 and says so. `ba_census point` reads a 10,000-row relation; `insert1` grows its relation by
about 1.4 million rows in 20 s; the lock cell's relation has 256 rows. Rule 9's three sizes are therefore **not met by
this close**; a point statement is flat in rows by construction and the other shapes were taken at the one size their
source measurements used.

**Units.** A rate carries its unit (`stmt/s`, `QPS`, `stmt/CPU-s`); `QPS = 1,000,000 / µs` is derived where marked. Server
CPU per statement is a cost and stays in µs, with its rate form beside it in §2. The drivers record **p90** and not p95
for `bn_census` and **p95** for `ba_census` and the lock cell; each table says which. An every-row arm is 60 statements,
so its p90 is the 54th and its p99 is the maximum; only p0-p50 describe its typical statement.

**Server CPU** is the sum over every server thread of `/proc/<pid>/task/*/schedstat`'s on-CPU field, divided by the
arm's statements (the server's idle burn, 4.0-5.9 ms per second, about 0.5 % of a CPU, is measured and not subtracted).
For the lock cell it is the server's whole `schedstat` over the cell (setup included) divided by the 96,000 statements of
its six arms.

**The unit of independence is the run, and a run replicates a session, not a binary.** The two servers of a pair are
separate processes; a run's offset between them (memory placement, layout, which one started first) is shared by its 12
blocks, so the sign test over 36 blocks is shown and not used; and, because the same two binaries run in all three
runs, a binary-specific offset repeats in every run and the interval over three runs does not contain it. The
following measured floors say how large these offsets are; **a delta smaller than the floor of its shape is not a
finding**.

| Floor | Measured how | Point `SELECT` (literal / prepared) | Every-row `UPDATE` (literal / prepared) |
|---|---|---|---|
| A run-to-run, one binary, one session family | A's three arms in AB (max - min) | 1.01 µs (3.9 %) / 0.86 µs (3.4 %) | 58.6 µs (0.8 %) / 152.6 µs (2.2 %) |
| The same binary in two session families | `cf889b63`, AB and S2S1 sessions against AQ sessions | not separated | **+5.4 % / +5.6 %** (6,897 against 7,271 µs; 6,989 against 7,377 µs) |
| **An identical-code pair** (S6: the executed path at `cores = 1` is the same, the binary differs) | second against first, three runs | +1.6 / +4.2 / +4.1 % and +2.3 / +4.6 / +3.2 % | **+1.5 / +1.5 / +1.2 % and +2.6 / +2.3 / +1.7 %**: every run positive, interval excludes zero |
| Second server against first, same binary | three binaries each measured in both positions | +1.7 %, +2.2 %, -1.0 % (literal) | +0.7 %, +2.0 %, +0.1 % (literal) |
| Code layout | `-falign-*` build against the plain build of the same commit | `ccf9cfc1` -2.3 %, `6139c2de` +2.7 % (literal) | `ccf9cfc1` **-3.4 % / -3.6 %**, `6139c2de` -0.5 % / 0.0 % |
| Telescoping | five paired segments from A to B against the direct A to B | | **+5.8 % against +2.8 %** (literal), **+9.3 % against +2.1 %** (prepared) |

So a binary-to-binary difference below about **+-2 %** on the every-row `UPDATE` and **+-4 %** on a point statement is not
a finding in this instrument, however many of the three runs agree on its sign.

## 1. The verdicts

BP-R1 gates nothing and BP-R6 and BQ-S5 report; each row is read against the floors above, not against a bar.

| Cell | Movement (second against first) | Read against | Verdict |
|---|---|---|---|
| Point `SELECT`, A to B, `cores = 1` | -1.4 % literal / +0.4 % prepared (server CPU, median of 3 paired runs) | intervals [-1.38, +0.67] and [-0.57, +0.87] µs contain 0; S6's identical-code pair reads +4.1 / +3.2 % | **Within noise** |
| Point `UPDATE`, A to B | -2.5 % / -2.5 % | intervals [-2.08, +0.57] and [-1.80, +0.76] µs contain 0 | **Within noise** (`results-bp-s3-g1-...` read +2.9 / +2.3 % for `1592007e` to `ccf9cfc1`: the sign belongs to the run) |
| Point statements, A to Q | `SELECT` +2.9 / +1.2 %, `UPDATE` +1.7 / +2.4 % | every interval contains 0; the two `UPDATE` rows are mixed in sign across runs | **Within noise** |
| Every-row `UPDATE`, A to B | +2.8 % / +2.1 % paired (every run positive, interval over runs excludes 0); +1.1 % / +1.2 % against all nine A runs pooled | S6's identical-code pair +1.5 / +2.3 %, every run positive, interval excludes 0; the same A differs by 5.4 % between session families | **Inside the instrument's floor**; not a finding either way |
| Every-row `UPDATE`, A to Q | -1.7 % / -1.5 % paired | Q is **+3.5 % / +3.2 %** above A's level in the six AB and S2S1 sessions | **Undetermined**: the sign depends on which A is the reference (§3.2) |
| Every-row `UPDATE`, `ccf9cfc1` to a binary with BQ's code | MERGE +5.0 / +6.3 %; close +5.3 / +6.6 %; aligned builds +7.3 / +8.7 %; lazy `RollRecord` +6.5 / +8.0 %; pooled +4.1 / +4.9 % (merge), +5.1 / +6.3 % (close) | 2-3 times the identical-code pair; seen in nine unaligned sessions and in three aligned ones, in the single-server profile pass (+5.1..+5.7 % / +3.6..+5.5 %, no position effect) and in `WalManager::Append`'s own samples; `ccf9cfc1` was always the first server and the position floor is about +1 % (up to +2.4 %), hence "+3 to +6 %" | **A real movement, +3 to +6 %**, open (§3) |
| `cores = 4`, `point` (read), A / Q / B | 57,621 / 57,599 / 58,916 stmt/s; p50 117.0 / 116.2 / 116.0 µs; p99 238.0 / 238.3 / 231.0 µs | spreads of 4.0 / 8.1 / 4.1 % between runs | **Within noise** |
| `cores = 4`, `insert1`, A / Q / B | 70,445 / 70,251 / 70,600 stmt/s; p50 77.9 / 78.8 / 78.6 µs; p99 201.1 / 200.5 / 200.5 µs; max 392.6 / 386.7 / 374.4 ms | spreads of 3.1 / 0.1 / 2.7 % | **Within noise**; the maximum is the carve |
| BP-S6, `cores = 8`, one fence held, CPU per statement | S6A 51.59 µs, B 48.42 µs: **-6.2 %**, 3 of 3 runs, every B below every S6A | plain cell +1.5 % inside a 0.9 µs spread; S1's own repeat of the S6A fence cell read 52.4 µs | **A real reduction** |
| BP-S6, fence premium over the plain cell | +13.6 % (S6A) to +5.1 % (B), medians; per run +12.5..+13.6 % to +3.8..+6.7 % | | **Reduced, not closed** |
| BP-S6, lock-partition waits per statement | fence cell 22.3 to 16.7 per 1,000 (-25 %); plain cell 15.5 to 15.3 per 1,000 (-1.5 %) | | **Fell where the fence walk is** |
| BP-S6, update-disjoint throughput | plain 18,716 to 18,709 stmt/s; fence 18,470 to 18,556 stmt/s | the same arm repeated inside one cell differs by 0.2-3.7 % | **Within noise**: the CPU saved does not reach throughput |
| Longest statement of an every-row arm (60 statements) | second server 586-1,219 ms in 60 of 60 arms; first server 20-270 ms in 60 of 60 | the binary changes sides with its position | **A property of the position, not of any binary** (§4) |

## 2. Point statements at `cores = 1`

**Finding.** Neither BP's whole change nor BQ's instruments moves a point statement past the instrument's floor. A point
statement costs the server 22.8-27.4 µs and the client thread 22-34 µs; the wall time of a serial driver is the two
processes' CPU, overlapped, so the server's share of a 55 µs `SELECT` is 46 % and of a 45 µs `UPDATE` 53 %, and no
durability wait is on the path at `relaxed` (§7).

**Server on-CPU time per statement, A to B (BP's whole change and the merged BQ), `cores = 1`, `relaxed`, 10,000 rows**

| shape (driver path) | statements per arm | A = `cf889b63` r1 / r2 / r3 | B = `3e77e7d1` r1 / r2 / r3 | B - A r1 / r2 / r3 | B - A, % of A, r1 / r2 / r3 | median B - A | spread of B - A over runs | A, run to run (max - min) | 95 % interval of the mean B - A (run as unit) | contains 0 | A within a run: 12 blocks (max - min), median of runs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| point `SELECT`, literal | 3,000 stmt | 26.39 / 26.06 / 25.37 µs | 26.01 / 25.31 / 25.45 µs | -0.38 / -0.75 / 0.07 µs | -1.4 / -2.9 / +0.3 % | -0.38 µs (-1.4 %) | 0.82 µs | 1.01 µs | [-1.38, +0.67] µs | yes | 2.40 µs |
| point `SELECT`, prepared | 3,000 stmt | 24.98 / 25.28 / 24.42 µs | 25.08 / 25.17 / 24.88 µs | 0.10 / -0.11 / 0.46 µs | +0.4 / -0.4 / +1.9 % | +0.10 µs (+0.4 %) | 0.57 µs | 0.86 µs | [-0.57, +0.87] µs | yes | 1.70 µs |
| point `UPDATE`, literal | 3,000 stmt | 23.75 / 26.07 / 23.90 µs | 23.16 / 24.72 / 23.58 µs | -0.59 / -1.35 / -0.32 µs | -2.5 / -5.2 / -1.3 % | -0.59 µs (-2.5 %) | 1.03 µs | 2.32 µs | [-2.08, +0.57] µs | yes | 1.70 µs |
| point `UPDATE`, prepared | 3,000 stmt | 23.36 / 24.42 / 23.21 µs | 22.77 / 23.42 / 23.23 µs | -0.59 / -1.00 / 0.02 µs | -2.5 / -4.1 / +0.1 % | -0.59 µs (-2.5 %) | 1.02 µs | 1.21 µs | [-1.80, +0.76] µs | yes | 1.97 µs |
| every-row `UPDATE`, literal | 60 stmt | 6,921.1 / 6,880.9 / 6,862.5 µs | 7,087.5 / 7,123.2 / 7,057.4 µs | 166.4 / 242.3 / 194.9 µs | +2.4 / +3.5 / +2.8 % | +194.9 µs (+2.8 %) | 75.9 µs | 58.6 µs | [+105.9, +296.5] µs | **no** | 2,115.9 µs |
| every-row `UPDATE`, prepared | 60 stmt | 7,116.6 / 6,964.0 / 6,998.5 µs | 7,262.9 / 7,195.2 / 7,141.8 µs | 146.2 / 231.2 / 143.2 µs | +2.1 / +3.3 / +2.0 % | +146.2 µs (+2.1 %) | 87.9 µs | 152.6 µs | [+49.5, +297.6] µs | **no** | 2,297.9 µs |

**The same, A to Q (BQ's instruments on A's base)**

| shape (driver path) | statements per arm | A = `cf889b63` r1 / r2 / r3 | Q = `4cf30270` r1 / r2 / r3 | Q - A r1 / r2 / r3 | Q - A, % of A, r1 / r2 / r3 | median Q - A | spread of Q - A over runs | A, run to run (max - min) | 95 % interval of the mean Q - A (run as unit) | contains 0 | A within a run: 12 blocks (max - min), median of runs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| point `SELECT`, literal | 3,000 stmt | 26.09 / 25.72 / 25.84 µs | 27.40 / 25.82 / 26.58 µs | 1.31 / 0.10 / 0.74 µs | +5.0 / +0.4 / +2.9 % | +0.74 µs (+2.9 %) | 1.21 µs | 0.37 µs | [-0.78, +2.22] µs | yes | 1.74 µs |
| point `SELECT`, prepared | 3,000 stmt | 25.81 / 25.53 / 25.08 µs | 26.12 / 25.73 / 25.86 µs | 0.31 / 0.20 / 0.78 µs | +1.2 / +0.8 / +3.1 % | +0.31 µs (+1.2 %) | 0.58 µs | 0.73 µs | [-0.34, +1.20] µs | yes | 2.90 µs |
| point `UPDATE`, literal | 3,000 stmt | 24.33 / 25.35 / 24.13 µs | 24.74 / 24.84 / 24.81 µs | 0.41 / -0.50 / 0.68 µs | +1.7 / -2.0 / +2.8 % | +0.41 µs (+1.7 %) | 1.18 µs | 1.21 µs | [-1.35, +1.74] µs | yes | 1.25 µs |
| point `UPDATE`, prepared | 3,000 stmt | 23.20 / 25.04 / 23.40 µs | 23.76 / 24.44 / 24.36 µs | 0.56 / -0.60 / 0.97 µs | +2.4 / -2.4 / +4.1 % | +0.56 µs (+2.4 %) | 1.57 µs | 1.84 µs | [-1.71, +2.33] µs | yes | 1.67 µs |
| every-row `UPDATE`, literal | 60 stmt | 7,446.0 / 7,199.0 / 7,166.8 µs | 7,240.9 / 7,134.5 / 7,047.7 µs | -205.1 / -64.5 / -119.1 µs | -2.8 / -0.9 / -1.7 % | -119.1 µs (-1.7 %) | 140.6 µs | 279.2 µs | [-305.7, +46.6] µs | yes | 2,271.7 µs |
| every-row `UPDATE`, prepared | 60 stmt | 7,557.7 / 7,345.5 / 7,227.3 µs | 7,248.9 / 7,236.5 / 7,152.4 µs | -308.8 / -109.0 / -74.9 µs | -4.1 / -1.5 / -1.0 % | -109.0 µs (-1.5 %) | 233.8 µs | 330.4 µs | [-478.1, +149.6] µs | yes | 2,266.4 µs |

**Paired blocks.** The arm is cut into 12 blocks taken alternately, so block `i` of the two servers share a host moment.
Shown for completeness; the run, not the block, is the unit (§0). A to B, then A to Q:

| shape (driver path) | blocks with B < A, r1 / r2 / r3 (of 12 each) | all 36 | sign test, one-sided p (B lower) | mean of the 36 paired B - A |
|---|---|---|---|---|
| point `SELECT`, literal | 7 / 12 / 5 blocks | 24 of 36 blocks | 0.033 | -0.35 µs |
| point `SELECT`, prepared | 6 / 7 / 3 blocks | 16 of 36 blocks | 0.797 | +0.15 µs |
| point `UPDATE`, literal | 7 / 7 / 7 blocks | 21 of 36 blocks | 0.203 | -0.76 µs |
| point `UPDATE`, prepared | 8 / 12 / 6 blocks | 26 of 36 blocks | 0.006 | -0.52 µs |
| every-row `UPDATE`, literal | 1 / 2 / 2 blocks | 5 of 36 blocks | 1.000 | +201.2 µs |
| every-row `UPDATE`, prepared | 3 / 1 / 3 blocks | 7 of 36 blocks | 1.000 | +173.5 µs |

| shape (driver path) | blocks with Q < A, r1 / r2 / r3 (of 12 each) | all 36 | sign test, one-sided p (Q lower) | mean of the 36 paired Q - A |
|---|---|---|---|---|
| point `SELECT`, literal | 0 / 7 / 1 blocks | 8 of 36 blocks | 1.000 | +0.72 µs |
| point `SELECT`, prepared | 4 / 2 / 2 blocks | 8 of 36 blocks | 1.000 | +0.43 µs |
| point `UPDATE`, literal | 5 / 7 / 3 blocks | 15 of 36 blocks | 0.879 | +0.19 µs |
| point `UPDATE`, prepared | 3 / 9 / 1 blocks | 13 of 36 blocks | 0.967 | +0.31 µs |
| every-row `UPDATE`, literal | 10 / 9 / 10 blocks | 29 of 36 blocks | 0.000 | -129.6 µs |
| every-row `UPDATE`, prepared | 11 / 10 / 9 blocks | 30 of 36 blocks | 0.000 | -164.2 µs |

**As statements per server-CPU second (derived: 1,000,000 / µs per statement, median of the three runs).** A to B, then A to Q:

| shape (driver path) | A | B = `3e77e7d1` | B vs A |
|---|---|---|---|
| point `SELECT`, literal | 38,374 stmt/CPU-s | 39,296 stmt/CPU-s | +2.4 % |
| point `SELECT`, prepared | 40,030 stmt/CPU-s | 39,872 stmt/CPU-s | -0.4 % |
| point `UPDATE`, literal | 41,845 stmt/CPU-s | 42,416 stmt/CPU-s | +1.4 % |
| point `UPDATE`, prepared | 42,811 stmt/CPU-s | 43,043 stmt/CPU-s | +0.5 % |
| every-row `UPDATE`, literal | 145.3 stmt/CPU-s | 141.1 stmt/CPU-s | -2.9 % |
| every-row `UPDATE`, prepared | 142.9 stmt/CPU-s | 139.0 stmt/CPU-s | -2.7 % |

| shape (driver path) | A | Q = `4cf30270` | Q vs A |
|---|---|---|---|
| point `SELECT`, literal | 38,696 stmt/CPU-s | 37,616 stmt/CPU-s | -2.8 % |
| point `SELECT`, prepared | 39,168 stmt/CPU-s | 38,671 stmt/CPU-s | -1.3 % |
| point `UPDATE`, literal | 41,107 stmt/CPU-s | 40,300 stmt/CPU-s | -2.0 % |
| point `UPDATE`, prepared | 42,742 stmt/CPU-s | 41,047 stmt/CPU-s | -4.0 % |
| every-row `UPDATE`, literal | 138.9 stmt/CPU-s | 140.2 stmt/CPU-s | +0.9 % |
| every-row `UPDATE`, prepared | 136.1 stmt/CPU-s | 138.2 stmt/CPU-s | +1.5 % |

**Reading.** The point rows of A to B read -2.5 to +0.4 % and of A to Q +1.2 to +2.9 %, and for every one of them the
interval over three runs contains zero. The sign of a point row belongs to the run and to the pair: A's own
`select-pk` literal arm reads 25.37-26.39 µs across the three AB runs and 25.72-26.09 µs across the three AQ runs, and
the identical-code pair S6 reads +4.1 % on the same shape (§3.2). A change of tens of nanoseconds per statement is about
1e-3 of a 25 µs statement and is below anything this instrument can show, so a per-statement fixed cost of BQ's
instruments, if any, is smaller than the floor of 1-2 µs (4-8 %); §3 shows what is left once a statement logs ten
thousand rows.

**Wall-clock latency per statement (client side), median of the three runs per percentile; `p90` stands where the rule
asks for `p95`; the last two columns are derived QPS (the second from the arm's mean, which an every-row arm's longest
statement dominates, §4)**

| shape (driver path) | binary (pair) | statements per run | p0 | p25 | p50 | p90 | p99 | 1,000,000 / p50 | 1,000,000 / mean |
|---|---|---|---|---|---|---|---|---|---|
| point `SELECT`, literal | A `cf889b63` (AB) | 3,000 | 51.91 µs | 54.29 µs | 55.20 µs | 61.50 µs | 79.80 µs | 18,115 QPS | 17,334 QPS |
| point `SELECT`, literal | B `3e77e7d1` | 3,000 | 48.17 µs | 54.07 µs | 54.80 µs | 63.76 µs | 85.60 µs | 18,247 QPS | 17,382 QPS |
| point `SELECT`, literal | A `cf889b63` (AQ) | 3,000 | 52.33 µs | 54.57 µs | 55.23 µs | 63.14 µs | 80.53 µs | 18,105 QPS | 17,224 QPS |
| point `SELECT`, literal | Q `4cf30270` | 3,000 | 53.51 µs | 54.98 µs | 55.68 µs | 63.97 µs | 80.26 µs | 17,958 QPS | 17,127 QPS |
| point `SELECT`, prepared | A `cf889b63` (AB) | 3,000 | 49.42 µs | 50.95 µs | 51.56 µs | 59.25 µs | 75.29 µs | 19,396 QPS | 18,467 QPS |
| point `SELECT`, prepared | B `3e77e7d1` | 3,000 | 49.55 µs | 51.07 µs | 51.64 µs | 62.82 µs | 90.02 µs | 19,366 QPS | 17,638 QPS |
| point `SELECT`, prepared | A `cf889b63` (AQ) | 3,000 | 49.61 µs | 51.35 µs | 52.01 µs | 60.47 µs | 78.52 µs | 19,227 QPS | 18,237 QPS |
| point `SELECT`, prepared | Q `4cf30270` | 3,000 | 50.32 µs | 51.75 µs | 52.31 µs | 60.70 µs | 74.30 µs | 19,117 QPS | 18,186 QPS |
| point `UPDATE`, literal | A `cf889b63` (AB) | 3,000 | 36.79 µs | 44.61 µs | 45.06 µs | 54.32 µs | 71.52 µs | 22,194 QPS | 20,921 QPS |
| point `UPDATE`, literal | B `3e77e7d1` | 3,000 | 37.95 µs | 44.37 µs | 44.81 µs | 52.22 µs | 68.09 µs | 22,317 QPS | 21,219 QPS |
| point `UPDATE`, literal | A `cf889b63` (AQ) | 3,000 | 39.49 µs | 44.99 µs | 45.48 µs | 54.87 µs | 73.15 µs | 21,988 QPS | 20,575 QPS |
| point `UPDATE`, literal | Q `4cf30270` | 3,000 | 38.21 µs | 45.41 µs | 45.90 µs | 54.35 µs | 69.68 µs | 21,786 QPS | 20,507 QPS |
| point `UPDATE`, prepared | A `cf889b63` (AB) | 3,000 | 34.92 µs | 41.82 µs | 42.49 µs | 51.49 µs | 71.95 µs | 23,533 QPS | 21,691 QPS |
| point `UPDATE`, prepared | B `3e77e7d1` | 3,000 | 34.51 µs | 41.57 µs | 42.09 µs | 50.21 µs | 66.78 µs | 23,756 QPS | 22,273 QPS |
| point `UPDATE`, prepared | A `cf889b63` (AQ) | 3,000 | 34.55 µs | 42.10 µs | 42.56 µs | 49.52 µs | 67.54 µs | 23,494 QPS | 22,154 QPS |
| point `UPDATE`, prepared | Q `4cf30270` | 3,000 | 35.02 µs | 42.43 µs | 43.31 µs | 51.55 µs | 70.14 µs | 23,092 QPS | 21,580 QPS |
| every-row `UPDATE`, literal | A `cf889b63` (AB) | 60 | 6,516.3 µs | 6,627.7 µs | 6,720.8 µs | 7,541.0 µs | 77,034.9 µs | 148.8 QPS | 115.3 QPS |
| every-row `UPDATE`, literal | B `3e77e7d1` | 60 | 6,690.2 µs | 6,800.8 µs | 6,868.5 µs | 7,840.1 µs | 957,456.3 µs | 145.6 QPS | 38.5 QPS |
| every-row `UPDATE`, literal | A `cf889b63` (AQ) | 60 | 6,842.9 µs | 6,964.3 µs | 7,055.2 µs | 7,514.6 µs | 109,908.6 µs | 141.7 QPS | 108.9 QPS |
| every-row `UPDATE`, literal | Q `4cf30270` | 60 | 6,700.1 µs | 6,827.1 µs | 6,921.5 µs | 7,744.9 µs | 1,023,444.6 µs | 144.5 QPS | 38.1 QPS |
| every-row `UPDATE`, prepared | A `cf889b63` (AB) | 60 | 6,515.0 µs | 6,609.7 µs | 6,665.1 µs | 7,697.7 µs | 161,655.7 µs | 150.0 QPS | 93.3 QPS |
| every-row `UPDATE`, prepared | B `3e77e7d1` | 60 | 6,697.4 µs | 6,771.2 µs | 6,816.6 µs | 7,859.3 µs | 870,510.9 µs | 146.7 QPS | 40.0 QPS |
| every-row `UPDATE`, prepared | A `cf889b63` (AQ) | 60 | 6,839.9 µs | 6,972.2 µs | 7,058.6 µs | 7,736.5 µs | 164,055.8 µs | 141.7 QPS | 94.6 QPS |
| every-row `UPDATE`, prepared | Q `4cf30270` | 60 | 6,698.8 µs | 6,855.5 µs | 6,929.5 µs | 7,691.0 µs | 868,013.8 µs | 144.3 QPS | 40.3 QPS |

**Where a point statement's time goes (median of three runs; the two columns overlap in time and do not sum to the wall time)**

| shape (driver path) | binary (pair) | server on-CPU per statement | client thread on-CPU per statement | p50 wall | mean wall |
|---|---|---|---|---|---|
| point `SELECT`, literal | A (AB) | 26.06 µs | 32.03 µs | 55.20 µs | 57.69 µs |
| point `SELECT`, literal | B | 25.45 µs | 32.62 µs | 54.80 µs | 57.53 µs |
| point `SELECT`, literal | Q | 26.58 µs | 32.05 µs | 55.68 µs | 58.39 µs |
| point `SELECT`, prepared | A (AB) | 24.98 µs | 29.94 µs | 51.56 µs | 54.15 µs |
| point `SELECT`, prepared | B | 25.08 µs | 31.39 µs | 51.64 µs | 56.70 µs |
| point `SELECT`, prepared | Q | 25.86 µs | 29.61 µs | 52.31 µs | 54.99 µs |
| point `UPDATE`, literal | A (AB) | 23.90 µs | 25.09 µs | 45.06 µs | 47.80 µs |
| point `UPDATE`, literal | B | 23.58 µs | 24.83 µs | 44.81 µs | 47.13 µs |
| point `UPDATE`, literal | Q | 24.81 µs | 24.86 µs | 45.90 µs | 48.76 µs |
| point `UPDATE`, prepared | A (AB) | 23.36 µs | 22.63 µs | 42.49 µs | 46.10 µs |
| point `UPDATE`, prepared | B | 23.23 µs | 22.41 µs | 42.09 µs | 44.90 µs |
| point `UPDATE`, prepared | Q | 24.36 µs | 22.93 µs | 43.31 µs | 46.34 µs |

## 3. The every-row UPDATE: +3 to +6 % over `ccf9cfc1`, +1 to +3 % over the opening, and half of it in one function's frame

**Finding.** The every-row `UPDATE` of 10,000 rows is the one shape in which a per-record cost shows: it logs a record
per row and per change, so a cost of a few nanoseconds per `WalManager::Append` is multiplied by about ten thousand.
Measured as the server's CPU per statement, **`ccf9cfc1` (BP-S3's head) is the low point of the series, 6,753 µs literal
and 6,778 µs prepared (nine sessions each); every binary that carries BQ's instruments sits 3.4-6.8 % higher**
(§3.2), the opening `cf889b63` (7,022 / 7,118 µs pooled) sits with them, and the close is +1 to +3 % over the opening,
which is the size of the floor. The paired tables first, then the same data pooled by binary, then what the layout, the lazy `RollRecord` and the
profiles say about it.

### 3.1 The pairs

The ladder, every-row `UPDATE` (10,000 rows, `cores = 1`), server CPU per statement, second server against first,
three runs each (the interval is the mean of the three per-run deltas +- t(2) x its standard error, in µs):

| pair | from (first server) | to (second server) | what lies between | every-row `UPDATE`, literal: to - from per run, % | median | 95 % interval of the mean (µs) | prepared: per run, % | median | 95 % interval (µs) | blocks to < from, literal / prepared (of 36) |
|---|---|---|---|---|---|---|---|---|---|---|
| AB | `cf889b63` | `3e77e7d1` | BP + BQ, whole close | +2.4 / +3.5 / +2.8 % | +2.8 % | [+106, +297] µs | +2.1 / +3.3 / +2.0 % | +2.1 % | [+50, +298] µs | 5 / 7 blocks |
| AQ | `cf889b63` | `4cf30270` | BQ alone (its instruments) | -2.8 / -0.9 / -1.7 % | -1.7 % | [-306, +47] µs | -4.1 / -1.5 / -1.0 % | -1.5 % | [-478, +150] µs | 29 / 30 blocks |
| S2S1 | `cf889b63` | `1592007e` | BP-S2 + BP-S1 (retired kinds, counters) | +0.5 / +0.6 / -0.5 % | +0.5 % | [-85, +113] µs | -0.1 / +1.9 / +1.5 % | +1.5 % | [-103, +250] µs | 21 / 14 blocks |
| S4S6BQ | `ccf9cfc1` | `3e77e7d1` | BQ merge + BP-S4 + BP-S6 | +5.3 / +5.3 / +6.0 % | +5.3 % | [+306, +446] µs | +6.6 / +7.3 / +6.4 % | +6.6 % | [+387, +540] µs | 1 / 0 blocks |
| MERGE | `ccf9cfc1` | `6139c2de` | BQ merged onto BP-S3 | +5.0 / +3.6 / +6.0 % | +5.0 % | [+121, +539] µs | +6.3 / +4.5 / +7.9 % | +6.3 % | [+149, +695] µs | 2 / 0 blocks |
| S4 | `6139c2de` | `d555cb55` | BP-S4 (purge on core 0's tick) | +1.0 / +0.1 / -0.0 % | +0.1 % | [-74, +124] µs | +0.1 / +1.2 / +1.0 % | +1.0 % | [-52, +158] µs | 21 / 14 blocks |
| S6 | `d555cb55` | `3e77e7d1` | BP-S6 (64 lock partitions, no executed change at cores = 1) | +1.5 / +1.5 / +1.2 % | +1.5 % | [+74, +122] µs | +2.6 / +2.3 / +1.7 % | +2.3 % | [+72, +238] µs | 8 / 4 blocks |
| MERGEALIGNED | `ccf9cfc1 aligned` | `6139c2de aligned` | BQ merge, both aligned builds | +7.7 / +6.7 / +7.3 % | +7.3 % | [+390, +554] µs | +8.9 / +8.6 / +8.7 % | +8.7 % | [+544, +597] µs | 0 / 0 blocks |
| PREMERGE_FIX | `ccf9cfc1` | `rollfix` | rollfix against BP-S3 | +6.7 / +6.5 / +6.2 % | +6.5 % | [+390, +476] µs | +8.3 / +7.1 / +8.0 % | +8.0 % | [+418, +633] µs | 0 / 1 blocks |
| CLOSE_FIX | `3e77e7d1` | `rollfix` | rollfix against the close | +0.1 / +1.4 / +4.0 % | +1.4 % | [-225, +482] µs | +0.6 / +2.0 / +4.6 % | +2.0 % | [-187, +523] µs | 9 / 9 blocks |

The same pairs on the point `SELECT`, for the floor they put under it:

| pair | from -> to | point `SELECT`, literal: per run, % | median | 95 % interval of the mean (µs) | prepared: per run, % | median | 95 % interval (µs) |
|---|---|---|---|---|---|---|---|
| AB | `cf889b63` -> `3e77e7d1` | -1.4 / -2.9 / +0.3 % | -1.4 % | [-1.38, +0.67] µs | +0.4 / -0.4 / +1.9 % | +0.4 % | [-0.57, +0.87] µs |
| AQ | `cf889b63` -> `4cf30270` | +5.0 / +0.4 / +2.9 % | +2.9 % | [-0.78, +2.22] µs | +1.2 / +0.8 / +3.1 % | +1.2 % | [-0.34, +1.20] µs |
| S2S1 | `cf889b63` -> `1592007e` | +1.7 / -2.1 / +0.3 % | +0.3 % | [-1.21, +1.19] µs | +0.7 / -1.7 / -0.0 % | -0.0 % | [-0.85, +0.69] µs |
| S4S6BQ | `ccf9cfc1` -> `3e77e7d1` | -2.0 / -0.7 / +1.6 % | -0.7 % | [-1.28, +1.07] µs | -2.0 / -0.6 / +1.5 % | -0.6 % | [-1.21, +1.01] µs |
| MERGE | `ccf9cfc1` -> `6139c2de` | -5.5 / -1.7 / -1.2 % | -1.7 % | [-2.30, +0.82] µs | -4.5 / -2.3 / -0.7 % | -2.3 % | [-1.84, +0.55] µs |
| S4 | `6139c2de` -> `d555cb55` | -4.4 / +5.0 / -2.3 % | -2.3 % | [-3.24, +2.91] µs | -3.4 / +5.1 / -2.6 % | -2.6 % | [-2.93, +2.73] µs |
| S6 | `d555cb55` -> `3e77e7d1` | +1.6 / +4.2 / +4.1 % | +4.1 % | [-0.06, +1.72] µs | +2.3 / +4.6 / +3.2 % | +3.2 % | [+0.16, +1.48] µs |
| MERGEALIGNED | `ccf9cfc1 aligned` -> `6139c2de aligned` | +4.1 / +2.2 / +2.7 % | +2.7 % | [+0.17, +1.34] µs | +4.5 / +1.2 / +2.7 % | +2.7 % | [-0.29, +1.66] µs |
| PREMERGE_FIX | `ccf9cfc1` -> `rollfix` | -4.5 / -2.2 / -3.7 % | -3.7 % | [-1.61, -0.17] µs | -3.7 / -2.4 / -3.4 % | -3.4 % | [-1.21, -0.38] µs |
| CLOSE_FIX | `3e77e7d1` -> `rollfix` | -2.2 / +3.2 / +3.1 % | +3.1 % | [-1.60, +2.29] µs | -1.3 / +2.0 / +1.4 % | +1.4 % | [-0.92, +1.26] µs |

**The segments do not telescope.** Taking the segment medians in order, A to `1592007e` (+0.5 / +1.5 %), `1592007e` to
`ccf9cfc1` (-1.3 / -1.8 %, BP-S3's file), `ccf9cfc1` to `6139c2de` (+5.0 / +6.3 %), to `d555cb55` (+0.1 / +1.0 %) and to B
(+1.5 / +2.3 %) sum to +5.8 / +9.3 %, against +2.8 / +2.1 % measured directly from A to B. The 3.0 and 7.2 points of
disagreement are the sum of five pair offsets of the kind §0 lists, and it is why the next table pools by binary.

### 3.2 The same data, by binary

Each `bn_census` run measures two binaries; pooling every run in which a binary appeared, in either position, puts them
on one axis. `ccf9cfc1` (nine sessions) and the close (twelve) have a session-to-session standard deviation of 1.0-1.4 %
(65-103 µs), and the layout and position offsets average over sessions instead of repeating.

**Server CPU per every-row `UPDATE` statement by binary (mean over the sessions it appeared in, min - max in brackets; the two right-hand columns of each shape are against `ccf9cfc1` and against `cf889b63` pooled)**

| binary | role in the build lineage | sessions (runs) pooled | every-row `UPDATE`, literal: mean (min - max) | vs `ccf9cfc1` | vs `cf889b63` pooled | prepared: mean (min - max) | vs `ccf9cfc1` | vs `cf889b63` pooled |
|---|---|---|---|---|---|---|---|---|
| `cf889b63` | A, BP's and BQ's opening | 9 | 7,022 µs (6,819 - 7,446) = 142.4 stmt/CPU-s | +4.0 % | +0.0 % | 7,118 µs (6,830 - 7,558) = 140.5 stmt/CPU-s | +5.0 % | +0.0 % |
| `1592007e` | BP-S1 (counters) | 3 | 6,920 µs (6,860 - 6,987) = 144.5 stmt/CPU-s | +2.5 % | -1.5 % | 7,025 µs (6,957 - 7,074) = 142.3 stmt/CPU-s | +3.7 % | -1.3 % |
| `ccf9cfc1` | BP-S3 head, before BQ is merged | 9 | 6,753 µs (6,666 - 6,878) = 148.1 stmt/CPU-s | +0.0 % | -3.8 % | 6,778 µs (6,666 - 6,918) = 147.5 stmt/CPU-s | +0.0 % | -4.8 % |
| `4cf30270` | Q, BQ's instruments on A's base | 3 | 7,141 µs (7,048 - 7,241) = 140.0 stmt/CPU-s | +5.7 % | +1.7 % | 7,213 µs (7,152 - 7,249) = 138.6 stmt/CPU-s | +6.4 % | +1.3 % |
| `6139c2de` | BQ merged into BP | 6 | 7,032 µs (6,948 - 7,159) = 142.2 stmt/CPU-s | +4.1 % | +0.2 % | 7,111 µs (6,971 - 7,252) = 140.6 stmt/CPU-s | +4.9 % | -0.1 % |
| `d555cb55` | + BP-S4 | 6 | 6,984 µs (6,948 - 7,044) = 143.2 stmt/CPU-s | +3.4 % | -0.5 % | 7,075 µs (7,017 - 7,119) = 141.4 stmt/CPU-s | +4.4 % | -0.6 % |
| `3e77e7d1` | B, the close (+ BP-S6) | 12 | 7,100 µs (7,022 - 7,245) = 140.9 stmt/CPU-s | +5.1 % | +1.1 % | 7,201 µs (7,033 - 7,372) = 138.9 stmt/CPU-s | +6.3 % | +1.2 % |
| `rollfix` | B with the per-append RollRecord made lazy (uncommitted) | 6 | 7,159 µs (7,098 - 7,311) = 139.7 stmt/CPU-s | +6.0 % | +2.0 % | 7,240 µs (7,176 - 7,356) = 138.1 stmt/CPU-s | +6.8 % | +1.7 % |
| `ccf9cfc1 aligned` | aligned build | 3 | 6,523 µs (6,469 - 6,600) = 153.3 stmt/CPU-s | -3.4 % | -7.1 % | 6,537 µs (6,495 - 6,615) = 153.0 stmt/CPU-s | -3.6 % | -8.2 % |
| `6139c2de aligned` | aligned build | 3 | 6,995 µs (6,936 - 7,079) = 143.0 stmt/CPU-s | +3.6 % | -0.4 % | 7,108 µs (7,060 - 7,193) = 140.7 stmt/CPU-s | +4.9 % | -0.1 % |

**`cf889b63` is not one level.** The same binary, in the same interleaved design, read:

| `cf889b63` in | sessions | literal, per run | mean | prepared, per run | mean |
|---|---|---|---|---|---|
| the AB and S2S1 sessions (second server: `3e77e7d1`, `1592007e`) | 6 | 6,921 / 6,881 / 6,863 / 6,880 / 6,819 / 7,019 µs | 6,897 µs | 7,117 / 6,964 / 6,999 / 7,053 / 6,830 / 6,972 µs | 6,989 µs |
| the AQ sessions (second server: `4cf30270`) | 3 | 7,446 / 7,199 / 7,167 µs | 7,271 µs | 7,558 / 7,346 / 7,227 µs | 7,377 µs |

The three AQ sessions put A 5.4 % (literal) and 5.6 % (prepared) above the six AB and S2S1 sessions, and Q then reads
-1.7 % against that A. Against the other six sessions of A, Q (7,141 / 7,213 µs) is +3.5 % / +3.2 % above it, against all nine
+1.7 % / +1.3 %. The cause of A's level shifting with the other server in the pair is not known, so **A to Q on this
shape is undetermined between -2 % and +3.5 %**, and nothing here needs BQ to cost *less* on A's base than on BP's:
Q (7,141 µs), the merge (7,032), B (7,100) and `rollfix` (7,159) are all at 7.0-7.2 ms, and `ccf9cfc1` (6,753) and the
aligned `ccf9cfc1` (6,523) are below them.

**One binary in both positions.** Three binaries ran as the first server in one pair and the second in another:

| binary | first-server sessions (mean of n) | second-server sessions (mean of n) | second vs first |
|---|---|---|---|
| `3e77e7d1`, every-row `UPDATE`, literal | 7,061 µs (n=3) | 7,112 µs (n=9) | +0.7 % |
| `3e77e7d1`, every-row `UPDATE`, prepared | 7,073 µs (n=3) | 7,244 µs (n=9) | +2.4 % |
| `3e77e7d1`, point `SELECT`, literal | 25.48 µs (n=3) | 25.92 µs (n=9) | +1.7 % |
| `3e77e7d1`, point `SELECT`, prepared | 24.75 µs (n=3) | 25.18 µs (n=9) | +1.7 % |
| `6139c2de`, every-row `UPDATE`, literal | 6,964 µs (n=3) | 7,101 µs (n=3) | +2.0 % |
| `6139c2de`, every-row `UPDATE`, prepared | 7,035 µs (n=3) | 7,188 µs (n=3) | +2.2 % |
| `6139c2de`, point `SELECT`, literal | 25.25 µs (n=3) | 25.80 µs (n=3) | +2.2 % |
| `6139c2de`, point `SELECT`, prepared | 24.55 µs (n=3) | 25.05 µs (n=3) | +2.0 % |
| `d555cb55`, every-row `UPDATE`, literal | 6,979 µs (n=3) | 6,989 µs (n=3) | +0.1 % |
| `d555cb55`, every-row `UPDATE`, prepared | 7,061 µs (n=3) | 7,088 µs (n=3) | +0.4 % |
| `d555cb55`, point `SELECT`, literal | 25.34 µs (n=3) | 25.09 µs (n=3) | -1.0 % |
| `d555cb55`, point `SELECT`, prepared | 24.51 µs (n=3) | 24.45 µs (n=3) | -0.2 % |

The position moves a binary by -1.0 to +2.4 %, mean about +1 %.

**What BP's own work did, and what BQ's did to it.** `cf889b63` to `ccf9cfc1` is -3.8 % / -4.8 % pooled (-2.1 % / -3.0 % against
the six AB and S2S1 sessions of A; -0.8 % / -0.3 % from the two segments measured against each other), so BP-S1 to BP-S3
took between 0.3 and 5 % off this statement, most likely 2-3 %. `ccf9cfc1` to the merge is +4.1 % / +4.9 % pooled and
+5.0 % / +6.3 % paired: the cost of BQ's instruments on BP's branch, **+280-420 µs, 28-42 ns per row** of 10,000, if
the whole of it is per row. `ccf9cfc1` was the first server in all nine of its sessions and the binaries it is compared
with were mostly the second, so the position offset of the table above (about +1 %, up to +2.4 %) sits inside these
deltas; net of it they are about +3 to +5 %, and the single-server profile pass, which has no position, reads +3.6 to
+5.7 %. BP-S4 (+0.1 / +1.0 % paired) and BP-S6 (+1.5 / +2.3 % paired, no executed change) add
nothing beyond the floor.

### 3.3 It is not layout, and it is not the lazy `RollRecord`

**Aligned builds.** With functions, loops and jumps aligned, `ccf9cfc1` reads 6,523 / 6,537 µs (-3.4 % / -3.6 % against
its unaligned self) and the merge 6,995 / 7,108 µs (-0.5 % / 0.0 %), so the merge's delta from the aligned `ccf9cfc1` is
**+7.3 % / +8.7 %**, every run positive, 0 of 36 blocks lower. A layout accident that the merge happened to hit would
not survive a build that fixes the layout; the delta grew. (`ccf9cfc1` itself moves 3.4 % with alignment, which is the
layout floor in the table of §0.)

**The lazy `RollRecord` (`rollfix`).** Every `WalManager::Append` since BQ-S1 constructs a `RollRecord` (about 176 bytes by its members: a
struct of timings, default-initialised) and resets it after a roll; the build named `rollfix` made it an `std::optional`
filled only on a roll. Against `ccf9cfc1` it reads **+6.5 % / +8.0 %**, against the close **+1.4 % / +2.0 %** (intervals
[-225, +482] and [-187, +523] µs), i.e. at the merge's level or a little above it; pooled it is the highest binary of the
series (7,159 / 7,240 µs). **The change removed nothing.** It was reverted and is not archived.

### 3.4 Where in the profile

A sampler pass (the `LD_PRELOAD` sampler of `tools/kds_sampler.c`, ~800 samples per dump, one dump per driver path) of
`ccf9cfc1`, the merge, `rollfix` and the close, each on a quiet host (1-minute load 0.89-1.10), single server:

| profiled binary | dump | samples | `WalManager::Append` self (samples, share) | `WalManager::Append` inclusive | `WalStream::Append` (its callee) | server CPU per statement under the sampler (literal / prepared) |
|---|---|---|---|---|---|---|
| `ccf9cfc1` | dump.0 (literal) | 809 | 10 (1.24 %) | 29 (3.58 %) | 19 (2.35 %) | 6,892 µs |
| `ccf9cfc1` | dump.1 (prepared) | 812 | 7 (0.86 %) | 24 (2.96 %) | 18 (2.22 %) | 6,835 µs |
| **`ccf9cfc1` both dumps** | | **1621** | **17 (1.05 %)** | **53 (3.27 %)** | **37 (2.28 %)** | |
| `6139c2de` | dump.0 (literal) | 857 | 25 (2.92 %) | 45 (5.25 %) | 17 (1.98 %) | 7,256 µs |
| `6139c2de` | dump.1 (prepared) | 838 | 36 (4.30 %) | 54 (6.44 %) | 19 (2.27 %) | 7,081 µs |
| **`6139c2de` both dumps** | | **1695** | **61 (3.60 %)** | **99 (5.84 %)** | **36 (2.12 %)** | |
| rollfix | dump.0 (literal) | 860 | 25 (2.91 %) | 51 (5.93 %) | 25 (2.91 %) | 7,243 µs |
| rollfix | dump.1 (prepared) | 860 | 26 (3.02 %) | 44 (5.12 %) | 18 (2.09 %) | 7,206 µs |
| **rollfix both dumps** | | **1720** | **51 (2.97 %)** | **95 (5.52 %)** | **43 (2.50 %)** | |
| B `3e77e7d1` | dump.0 (literal) | 859 | 25 (2.91 %) | 46 (5.36 %) | 22 (2.56 %) | 7,283 µs |
| B `3e77e7d1` | dump.1 (prepared) | 858 | 31 (3.61 %) | 51 (5.94 %) | 18 (2.10 %) | 7,210 µs |
| **B `3e77e7d1` both dumps** | | **1717** | **56 (3.26 %)** | **97 (5.65 %)** | **40 (2.33 %)** | |

The server CPU per statement under the sampler (last column) reproduces the paired result with one server and no
interleaving: the merge is +5.3 % / +3.6 % above `ccf9cfc1`, `rollfix` +5.1 % / +5.4 %, the close +5.7 % / +5.5 %.
**`WalManager::Append`'s own samples rise from 17 of 1,621 (1.05 %) to 61 of 1,695 (3.60 %), 51 of 1,720 (2.97 %) and 56 of
1,717 (3.26 %)**: z = 4.8, 3.9 and 4.4 against `ccf9cfc1` by a pooled two-proportion test, and 1.0 between the merge and
`rollfix`, i.e. the lazy `RollRecord` did not change it. Its callee `WalStream::Append` does not move (2.28 % to 2.12 /
2.50 / 2.33 %), so the added cost is in `WalManager::Append`'s own frame, not below it. At 1.9-2.6 points of a 7.1 ms
statement that is **137-184 µs, 14-18 ns per row, between a third and two thirds, about half, of the +280-420 µs** the
merge added. The other half is
not resolved: the functions that enter or leave the top-25 self lists between the two sides do so at 1.4-1.7 %
(`UndoPageAppend`, `UndoLog::Append`, `StatusOr`'s destructor, `PageView::ReadTuple`), those lists are truncated at about
nine samples, and a function just under the cut on one side reads 0.

**What the code between `ccf9cfc1` and the merge adds per append** (read from the diff `59c4139f..4cf30270`, not measured
separately): in `WalManager::Append` the `RollRecord` and a branch on its `end_ns`; in `WalStream::Append` a
`StreamLatchHold` (a `waited_` word and `ChargeRollOverlap`'s early-out) in place of a `LatchGuard`; and
`Latch::lock()` now calls `LockReportingWait()`, which every latch in the engine takes. Which of the three costs the
rest, and what in `WalManager::Append`'s frame costs the half that the lazy `RollRecord` left, is `[inferred]` and
untested. The cheap next steps are a disassembly of `WalManager::Append`'s prologue in `ccf9cfc1` and B (the distribution
compiler's default hardening, `-fstack-protector-strong` and `-fstack-clash-protection`, is on in the compile lines this
host's GCC 13 produced, and both attach to a large address-taken local and stay with an `optional`), and a build of A with
`latch.hpp`'s change alone.

### 3.5 Disposition

An **open item of the close**, stated as the data support it: the every-row `UPDATE` costs +3-6 % more server CPU per
statement in every binary carrying BQ's instruments than in `ccf9cfc1`; against the shared opening the close is +1 to
+3 %, inside the floor; the cost scales with logged records, not with statements, which is why no point statement, no
`cores = 4` cell and no `insert1` throughput shows it; about half of it is in `WalManager::Append`'s frame and survives
the obvious fix; the other half is not localised. It is **not** an interaction between BQ and BP: the BQ-only binary Q is
at the same level as the merge.

## 4. The second server's stall

**Finding.** In every `update-range` arm of every `bn_census` pair, **the second server's longest statement of its 60 is
0.59-1.22 s; the first server's is 20-270 ms.** 60 of 60 second-server arms against 0 of 60 first-server arms exceed
300 ms, over ten pairs and ten binaries, and the same binary carries the stall in one position and not in the other (the
time a second-server arm spends above its median, summed over its 60 statements, is 652-1,357 ms, 1.01-1.42 times its
longest statement, median 1.05: one long statement per arm; the first server's excess is 30-486 ms, spread over several
statements):

| pair | first-server binary | first server: longest statement of an arm (6 arms: 2 paths x 3 runs) | second-server binary | second server: longest statement of an arm (6 arms) |
|---|---|---|---|---|
| AB | A `cf889b63` | 77-207 ms | B `3e77e7d1` | 866-1,075 ms |
| AQ | A `cf889b63` | 24-270 ms | Q `4cf30270` | 678-1,069 ms |
| S2S1 | A `cf889b63` | 33-126 ms | `1592007e` | 978-1,136 ms |
| S4S6BQ | `ccf9cfc1` | 40-116 ms | B `3e77e7d1` | 938-1,068 ms |
| MERGE | `ccf9cfc1` | 22-114 ms | `6139c2de` | 993-1,150 ms |
| S4 | `6139c2de` | 20-153 ms | `d555cb55` | 946-1,054 ms |
| S6 | `d555cb55` | 20-115 ms | B `3e77e7d1` | 868-1,219 ms |
| MERGEALIGNED | `ccf9cfc1` aligned | 24-213 ms | `6139c2de` aligned | 879-1,161 ms |
| PREMERGE_FIX | `ccf9cfc1` | 22-242 ms | rollfix | 586-1,180 ms |
| CLOSE_FIX | B `3e77e7d1` | 21-127 ms | rollfix | 979-1,055 ms |

`6139c2de` is the second server in MERGE (993-1,150 ms) and the first in S4 (20-153 ms); `d555cb55` the second in S4
(946-1,054 ms) and the first in S6 (20-115 ms); B the second in AB, S4S6BQ and S6 and the first in CLOSE_FIX (21-127 ms).
Nor is it the lazy `RollRecord` or BQ's code: `1592007e` (BP-S1, before BQ) shows it as the second server of S2S1, and Q
and `rollfix` show it too. The p99 of the
point shapes shows no such position effect (median 75 µs first, 74 µs second server over 60 arms). The stall is not CPU
(the second server's CPU per statement is the first's within the position floor of §0, 0.1-2.4 %), so it is a wait on
the second server's side; its mechanism is not found (`[inferred]` candidate: the second server's first write-heavy block lands
behind the first server's dirty pages on the same filesystem).

**Consequences.** (i) Any mean-based wall rate of an every-row arm in these files is a property of the pair's second
slot: the mean-derived QPS column of §2 reads 115.3 against 38.5 QPS for A against B and 108.9 against 38.1 for A against
Q, and the median-derived column 148.8 against 145.6 and 141.7 against 144.5 (the CPU and the median are the evidence).
(ii) **`results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md` §3, which reads the same stall as "B's tail the CPU does not explain"
(0.91-1.09 s in B's six `update-range` arms against 17-196 ms in A's), describes the second server of that pair, and its
claim that these files cannot say whether the stall is B's or the device's is answered: it is the position's.** Its
`group` commit-loop pauses (142 / 217 / 201 ms against 26 / 24 / 23 ms) are a different driver (`ba_census insert1`) and
are not addressed here.

## 5. `cores = 4`: reads and the monotonic insert

**Finding.** `point` and `insert1` at eight sessions, `relaxed`, do not distinguish A, Q and B. Throughput, p0-p99 and the
maximum agree to within the spread of one binary between its own runs, and the BQ and BP instruments added to the log
path cost less than a part in a hundred of a statement's latency in the waits the engine names (§7).

**Throughput and latency, median of three runs per binary (A, Q, B rotated per run so no binary owns an early or a late
slot)**

| shape | binary | runs | stmt/s median (min - max) | spread (max - min, % of median) | p0 | p25 | p50 | p95 | p99 (min - max) | max (min - max) | errors / refusals |
|---|---|---|---|---|---|---|---|---|---|---|---|
| point | A `cf889b63` | 3 | 57,621 stmt/s (56,906 - 59,183) | 4.0 % | 51.4 µs | 104.0 µs | 117.0 µs | 212.2 µs | 238.0 µs (229.3 - 243.6) | 6.1 ms (4.6 - 9.0) | 0 / 0 |
| point | Q `4cf30270` | 3 | 57,599 stmt/s (54,493 - 59,180) | 8.1 % | 47.8 µs | 103.9 µs | 116.2 µs | 210.1 µs | 238.3 µs (230.9 - 919.2) | 6.9 ms (6.8 - 7.0) | 0 / 0 |
| point | B `3e77e7d1` | 3 | 58,916 stmt/s (57,000 - 59,426) | 4.1 % | 50.6 µs | 104.2 µs | 116.0 µs | 207.1 µs | 231.0 µs (226.8 - 240.5) | 7.0 ms (6.0 - 9.2) | 0 / 0 |
| insert1 | A `cf889b63` | 3 | 70,445 stmt/s (68,960 - 71,137) | 3.1 % | 31.4 µs | 70.6 µs | 77.9 µs | 145.2 µs | 201.1 µs (196.2 - 204.8) | 392.6 ms (370.1 - 400.2) | 0 / 0 |
| insert1 | Q `4cf30270` | 3 | 70,251 stmt/s (70,240 - 70,325) | 0.1 % | 30.6 µs | 70.5 µs | 78.8 µs | 151.3 µs | 200.5 µs (197.3 - 205.0) | 386.7 ms (378.6 - 396.6) | 0 / 0 |
| insert1 | B `3e77e7d1` | 3 | 70,600 stmt/s (68,833 - 70,712) | 2.7 % | 31.7 µs | 71.3 µs | 78.6 µs | 147.9 µs | 200.5 µs (193.2 - 204.5) | 374.4 ms (367.1 - 384.0) | 0 / 0 |

**Every run** (the placement columns are the sessions the kernel's `SO_REUSEPORT` hash and BL-S3's balancer left on each
core at the end of the run, from the driver's `meta_session_core`):

| shape | binary | run | stmt/s | p0 | p25 | p50 | p95 | p99 | max | sessions per core (c0 / c1 / c2 / c3) | client CPU share (max of 8 sessions) | load before (1 / 5 / 15 min) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| point | A `cf889b63` | r1 | 56,906 stmt/s | 51.4 µs | 104.6 µs | 117.0 µs | 213.9 µs | 243.6 µs | 6.1 ms | 4 / 2 / 1 / 1 | 0.43 | 1.46 / 2.92 / 3.56 |
| point | A `cf889b63` | r2 | 57,621 stmt/s | 51.2 µs | 104.0 µs | 117.0 µs | 212.2 µs | 238.0 µs | 4.6 ms | 3 / 1 / 2 / 2 | 0.43 | 1.42 / 2.27 / 2.90 |
| point | A `cf889b63` | r3 | 59,183 stmt/s | 58.3 µs | 103.8 µs | 115.7 µs | 204.2 µs | 229.3 µs | 9.0 ms | 1 / 2 / 3 / 2 | 0.43 | 1.49 / 2.32 / 2.74 |
| point | Q `4cf30270` | r1 | 57,599 stmt/s | 49.8 µs | 105.3 µs | 117.6 µs | 210.1 µs | 238.3 µs | 6.8 ms | 1 / 3 / 2 / 2 | 0.43 | 1.36 / 2.72 / 3.44 |
| point | Q `4cf30270` | r2 | 54,493 stmt/s | 44.0 µs | 102.2 µs | 115.2 µs | 210.8 µs | 919.2 µs | 7.0 ms | 2 / 0 / 3 / 3 | 0.39 | 1.46 / 2.40 / 3.05 |
| point | Q `4cf30270` | r3 | 59,180 stmt/s | 47.8 µs | 103.9 µs | 116.2 µs | 204.3 µs | 230.9 µs | 6.9 ms | 2 / 2 / 1 / 3 | 0.43 | 1.37 / 2.27 / 2.69 |
| point | B `3e77e7d1` | r1 | 57,000 stmt/s | 47.6 µs | 104.4 µs | 116.7 µs | 215.6 µs | 240.5 µs | 7.0 ms | 1 / 5 / 1 / 1 | 0.43 | 1.38 / 2.61 / 3.35 |
| point | B `3e77e7d1` | r2 | 58,916 stmt/s | 50.6 µs | 104.2 µs | 116.0 µs | 207.1 µs | 231.0 µs | 9.2 ms | 1 / 2 / 1 / 4 | 0.43 | 1.36 / 2.33 / 2.97 |
| point | B `3e77e7d1` | r3 | 59,426 stmt/s | 57.1 µs | 104.0 µs | 115.9 µs | 204.3 µs | 226.8 µs | 6.0 ms | 3 / 2 / 2 / 1 | 0.44 | 1.47 / 2.25 / 2.75 |
| insert1 | A `cf889b63` | r1 | 68,960 stmt/s | 29.7 µs | 70.6 µs | 80.1 µs | 155.8 µs | 204.8 µs | 392.6 ms | 2 / 4 / 2 / 0 | 0.34 | 1.47 / 2.49 / 3.24 |
| insert1 | A `cf889b63` | r2 | 71,137 stmt/s | 40.6 µs | 70.9 µs | 77.9 µs | 145.2 µs | 196.2 µs | 400.2 ms | 0 / 3 / 5 / 0 | 0.37 | 1.57 / 2.35 / 2.81 |
| insert1 | A `cf889b63` | r3 | 70,445 stmt/s | 31.4 µs | 70.1 µs | 77.3 µs | 145.2 µs | 201.1 µs | 370.1 ms | 2 / 1 / 2 / 3 | 0.36 | 1.59 / 2.33 / 2.67 |
| insert1 | Q `4cf30270` | r1 | 70,240 stmt/s | 32.8 µs | 70.5 µs | 77.7 µs | 144.8 µs | 197.3 µs | 378.6 ms | 1 / 1 / 3 / 3 | 0.36 | 1.55 / 2.46 / 3.17 |
| insert1 | Q `4cf30270` | r2 | 70,325 stmt/s | 29.7 µs | 70.3 µs | 78.8 µs | 152.7 µs | 200.5 µs | 396.6 ms | 4 / 2 / 0 / 2 | 0.37 | 1.36 / 2.24 / 2.85 |
| insert1 | Q `4cf30270` | r3 | 70,251 stmt/s | 30.6 µs | 71.3 µs | 80.0 µs | 151.3 µs | 205.0 µs | 386.7 ms | 2 / 1 / 3 / 2 | 0.35 | 1.50 / 2.31 / 2.64 |
| insert1 | B `3e77e7d1` | r1 | 68,833 stmt/s | 31.7 µs | 71.3 µs | 79.5 µs | 147.9 µs | 204.5 µs | 374.4 ms | 3 / 2 / 1 / 2 | 0.35 | 1.48 / 2.39 / 3.10 |
| insert1 | B `3e77e7d1` | r2 | 70,712 stmt/s | 31.5 µs | 70.8 µs | 78.5 µs | 148.2 µs | 200.5 µs | 384.0 ms | 1 / 0 / 3 / 4 | 0.36 | 1.52 / 2.26 / 2.82 |
| insert1 | B `3e77e7d1` | r3 | 70,600 stmt/s | 32.1 µs | 71.3 µs | 78.6 µs | 146.3 µs | 193.2 µs | 367.1 ms | 5 / 2 / 0 / 1 | 0.36 | 1.55 / 2.32 / 2.69 |

**Reading.** The `point` medians differ by +2.2 % (B against A) inside A's own 4.0 % spread and B's 4.1 %; `insert1`
differs by -0.3 % (Q) and +0.2 % (B). The worst cell of the nine reads is Q's r2: 54,493 stmt/s and p99 919.2 µs against
227-244 µs in the other eight. Its sessions sat 2 / 0 / 3 / 3 on the four cores, one reactor empty and another carrying
three sessions, which is the skew BL (paused) was opened for; it is a placement outcome of one run, not Q's. The
`insert1` maximum statement of 367-400 ms tracks the longest transaction-id carve (360-402 ms) on every binary: in eight of
the nine runs the maximum is 0.3-18.5 ms above the carve's longest hold, in B's r3 it is 35 ms below it:

| binary | run | longest carve | longest out-of-run hold of a reactor | longest checkpoint step | max statement |
|---|---|---|---|---|---|
| A `cf889b63` | r1 | 392.3 ms | 310.3 ms | 687.4 ms | 392.6 ms |
| A `cf889b63` | r2 | 397.3 ms | 394.9 ms | 734.2 ms | 400.2 ms |
| A `cf889b63` | r3 | 369.2 ms | 353.6 ms | 737.6 ms | 370.1 ms |
| Q `4cf30270` | r1 | 360.1 ms | 349.6 ms | 813.6 ms | 378.6 ms |
| Q `4cf30270` | r2 | 396.3 ms | 394.1 ms | 681.0 ms | 396.6 ms |
| Q `4cf30270` | r3 | 373.9 ms | 374.0 ms | 694.1 ms | 386.7 ms |
| B `3e77e7d1` | r1 | 362.8 ms | 317.9 ms | 890.9 ms | 374.4 ms |
| B `3e77e7d1` | r2 | 383.7 ms | 383.8 ms | 735.7 ms | 384.0 ms |
| B `3e77e7d1` | r3 | 402.1 ms | 349.2 ms | 733.9 ms | 367.1 ms |

**The log's instruments, in the runs that have them** (`insert1`; A has none of BQ's counters). Eleven rolls per run, each
with the next segment's body already built ahead (`body hit` 11 of 11, none waited or inline):

| binary | run | rolls | summed hold | longest hold | sync gate | create (header + dir sync + rename) | body hit / waited / inline | max statement (any session) |
|---|---|---|---|---|---|---|---|---|
| Q `4cf30270` | r1 | 11 | 103.6 ms | 12.6 ms | 43.5 ms | 35.0 ms | 11 / 0 / 0 | 378.6 ms |
| Q `4cf30270` | r2 | 11 | 88.3 ms | 4.3 ms | 15.0 ms | 42.2 ms | 11 / 0 / 0 | 396.6 ms |
| Q `4cf30270` | r3 | 11 | 75.9 ms | 5.2 ms | 19.3 ms | 30.8 ms | 11 / 0 / 0 | 386.7 ms |
| B `3e77e7d1` | r1 | 11 | 123.6 ms | 26.1 ms | 40.9 ms | 34.2 ms | 11 / 0 / 0 | 374.4 ms |
| B `3e77e7d1` | r2 | 11 | 88.4 ms | 7.5 ms | 12.7 ms | 38.6 ms | 11 / 0 / 0 | 384.0 ms |
| B `3e77e7d1` | r3 | 11 | 95.7 ms | 1.4 ms | 18.1 ms | 43.0 ms | 11 / 0 / 0 | 367.1 ms |

The summed roll hold is 76-124 ms per run and the longest single hold 1.4-26.1 ms; nothing in this cell approaches the
300-330 ms holds BQ-S1 priced under a bulk writer at `relaxed` (`include/kds/wal/stream.hpp`'s comment), because here the
body is always ready and the sync gate is short. B's summed hold (88-124 ms) and Q's (76-104 ms) overlap; three runs
each do not separate them.

## 6. BP-S6: 64 lock partitions at every core count, at `cores = 8`

**Finding.** With one range fence held on the benchmark's relation, every tuple ask walks every partition
(`FenceCoversKey`). S6A latches 8 x 64 = 512 of them; B 64. The server's CPU per statement in that cell falls **from 51.59
to 48.42 µs (-3.18 µs, -6.2 %)**, the three B runs (47.89-48.75 µs) all below the three S6A runs (51.18-51.97 µs), and the
fence cell's lock-partition waits per statement fall by a quarter. In the plain cell, which makes no fence walk, nothing
moves.

**Server CPU per statement (setup included, over 96,000 statements), update-disjoint throughput and lock-partition waits, `cores = 8`, `relaxed`, 256 rows, 8 sessions, 3 runs**

| held fence | binary | server CPU per statement, r0 / r1 / r2 | median | update-disjoint stmt/s, r0 / r1 / r2 | median | lock-partition waits per statement, r0 / r1 / r2 | fence walks per statement |
|---|---|---|---|---|---|---|---|
| plain | S6A `ccf9cfc1` (64 x 8 partitions) | 45.41 / 46.21 / 45.30 µs | **45.41 µs** | 18,634 / 18,727 / 18,716 stmt/s | 18,716 stmt/s | 15.1 / 16.4 / 15.1 per 1,000 | 0.000 |
| plain | B `3e77e7d1` (64 partitions) | 45.38 / 46.11 / 46.07 µs | **46.07 µs** | 18,709 / 18,745 / 18,480 stmt/s | 18,709 stmt/s | 14.4 / 15.7 / 15.8 per 1,000 | 0.000 |
| fence | S6A `ccf9cfc1` (64 x 8 partitions) | 51.59 / 51.97 / 51.18 µs | **51.59 µs** | 18,813 / 18,340 / 18,470 stmt/s | 18,470 stmt/s | 21.7 / 23.1 / 22.0 per 1,000 | 0.659 |
| fence | B `3e77e7d1` (64 partitions) | 48.42 / 47.89 / 48.75 µs | **48.42 µs** | 18,556 / 18,556 / 18,669 stmt/s | 18,556 stmt/s | 16.3 / 16.6 / 17.2 per 1,000 | 0.659 |

**The fence premium over the plain cell, paired by run**

| binary | fence premium over plain, per run r0 / r1 / r2 (CPU per statement) | median | premium of the medians |
|---|---|---|---|
| S6A `ccf9cfc1` (64 x 8 partitions) | +13.6 / +12.5 / +13.0 % | +13.0 % | +13.6 % |
| B `3e77e7d1` (64 partitions) | +6.7 / +3.8 / +5.8 % | +5.8 % | +5.1 % |

**Latency of the arms that matter, median of three runs (the lock benchmark driver records p95, not p90); `ping` never takes a tuple unit and is the control**

| held fence | binary | arm | ops per run | p0 | p25 | p50 | p95 | p99 | stmt/s | errors |
|---|---|---|---|---|---|---|---|---|---|---|
| plain | S6A `ccf9cfc1` | update-disjoint | 16,000 | 44.5 µs | 234.8 µs | 360.2 µs | 900.8 µs | 1,257.6 µs | 18,716 stmt/s | 0 |
| plain | S6A `ccf9cfc1` | update-disjoint-again | 16,000 | 43.9 µs | 236.7 µs | 363.6 µs | 904.6 µs | 1,277.9 µs | 18,673 stmt/s | 0 |
| plain | S6A `ccf9cfc1` | ping (control, never touches a tuple unit) | 16,000 | 58.6 µs | 253.1 µs | 383.3 µs | 932.0 µs | 1,272.9 µs | 17,896 stmt/s | 0 |
| plain | B `3e77e7d1` | update-disjoint | 16,000 | 43.0 µs | 232.8 µs | 357.1 µs | 905.1 µs | 1,265.3 µs | 18,709 stmt/s | 0 |
| plain | B `3e77e7d1` | update-disjoint-again | 16,000 | 44.7 µs | 233.9 µs | 357.5 µs | 892.1 µs | 1,207.8 µs | 18,910 stmt/s | 0 |
| plain | B `3e77e7d1` | ping (control, never touches a tuple unit) | 16,000 | 59.1 µs | 255.3 µs | 382.3 µs | 920.7 µs | 1,253.4 µs | 17,866 stmt/s | 0 |
| fence | S6A `ccf9cfc1` | update-disjoint | 16,000 | 47.2 µs | 238.6 µs | 363.8 µs | 904.3 µs | 1,262.4 µs | 18,470 stmt/s | 0 |
| fence | S6A `ccf9cfc1` | update-disjoint-again | 16,000 | 48.2 µs | 241.4 µs | 368.5 µs | 899.3 µs | 1,237.1 µs | 18,484 stmt/s | 0 |
| fence | S6A `ccf9cfc1` | ping (control, never touches a tuple unit) | 16,000 | 58.0 µs | 252.5 µs | 380.6 µs | 915.7 µs | 1,270.2 µs | 17,940 stmt/s | 0 |
| fence | B `3e77e7d1` | update-disjoint | 16,000 | 44.5 µs | 236.4 µs | 362.8 µs | 904.3 µs | 1,258.7 µs | 18,556 stmt/s | 0 |
| fence | B `3e77e7d1` | update-disjoint-again | 16,000 | 44.7 µs | 241.3 µs | 370.5 µs | 922.3 µs | 1,250.5 µs | 18,373 stmt/s | 0 |
| fence | B `3e77e7d1` | ping (control, never touches a tuple unit) | 16,000 | 60.2 µs | 256.1 µs | 383.4 µs | 931.5 µs | 1,260.3 µs | 17,797 stmt/s | 0 |

**Reading.** (i) The premium fell from +13.6 % to +5.1 % of a statement (6.2 to 2.3 µs per statement of fence over
plain) and the S6A cell agrees with BP-S1's census of the same S6A binary, which read plain 46.8 and fence 52.4 µs
(§9). (ii) **The walk's cost per latch acquisition is not constant**: 0.659 walks per statement x 512 partitions is 337
acquisitions for 6.18 µs (18 ns each), and 0.659 x 64 is 42 for 2.35 µs (56 ns each). Cutting the acquisitions eightfold
cut the premium 2.6-fold; two points fit 11 ns per acquisition (9-15 ns over the nine pairings of runs) plus
**about 2.2 µs of fixed cost per statement under a held fence**, which partitioning cannot reach `[inferred]`, from two
points only. (iii) The lock-partition waits per statement in the fence cell fall from 22.3 to 16.7 per 1,000 (-25 %),
leaving the premium over plain at +1.4 per 1,000 against +6.7; the plain cell's 15.5 and 15.3 per 1,000 are the same.
(iv) **The CPU does not reach throughput.** Update-disjoint reads 18,716 and 18,709 stmt/s plain and 18,470 and 18,556
stmt/s with the fence, p99 1.26 ms in all four, the repeat arm differs from the first by 0.2-3.7 %; eight reactors and
the client share four physical cores and the cell is not server-CPU-bound, which is BP-S1's own reading of this cell
("only the CPU leg is met").

## 7. Where the time goes

**`cores = 1`, a point statement (relaxed, no fsync on the path).** Wall p50 55.2 µs for the literal `SELECT`: the server
on CPU 25.5-26.6 µs and the client thread 32.0-32.6 µs, which together (58 µs) exceed the wall time, so the two overlap and
the socket round trip and the wake-ups are inside both; no durability wait (`relaxed` makes the commit a log append), no
lock wait (one core, one session), no conflict wait. Rows of §2's last table.

**`cores = 4`, mean latency from Little's law (8 sessions / throughput) against the waits the engine names, per
statement, median of three runs.** The named waits are everything `SHOW META` keeps as a duration; page-latch waits keep
a count and no duration, so they appear as a count.

| shape | binary | mean latency, Little (8 sessions / stmt/s) | page-latch waits per statement | frame-table wait per statement | lock-partition wait per statement | WAL stream-latch wait per statement | WAL sync-gate wait per statement | sum of the named waits | share of mean latency |
|---|---|---|---|---|---|---|---|---|---|
| point | A `cf889b63` | 138.8 µs | 0.034 waits (no duration kept) | 0.753 µs | 0.018 µs | 0.000 µs | 0.000 µs | 0.770 µs | 0.55 % |
| point | Q `4cf30270` | 138.9 µs | 0.032 waits (no duration kept) | 0.727 µs | 0.016 µs | 0.000 µs | 0.000 µs | 0.743 µs | 0.54 % |
| point | B `3e77e7d1` | 135.8 µs | 0.036 waits (no duration kept) | 0.748 µs | 0.015 µs | 0.000 µs | 0.000 µs | 0.763 µs | 0.56 % |
| insert1 | A `cf889b63` | 113.6 µs | 0.269 waits (no duration kept) | 0.505 µs | 0.074 µs | 0.202 µs | 0.016 µs | 0.797 µs | 0.70 % |
| insert1 | Q `4cf30270` | 113.9 µs | 0.257 waits (no duration kept) | 0.483 µs | 0.069 µs | 0.193 µs | 0.014 µs | 0.759 µs | 0.67 % |
| insert1 | B `3e77e7d1` | 113.3 µs | 0.275 waits (no duration kept) | 0.453 µs | 0.061 µs | 0.239 µs | 0.013 µs | 0.766 µs | 0.68 % |

The named waits sum to 0.74-0.80 µs of a 113-139 µs mean latency, **0.54-0.70 %**, on every binary; the rest is CPU,
scheduling and the socket round trip on a box whose reactors and clients share physical cores. The WAL stream-latch wait
of `insert1` is 0.20 (A: 0.196 / 0.241 / 0.202), 0.19 (Q: 0.274 / 0.193 / 0.162) and 0.24 µs (B: 0.279 / 0.239 / 0.239)
per statement; B's three runs sit at or above A's, by 0.04 µs of a 113 µs latency, and overlap Q's, so the instruments held under
that latch add nothing this cell can separate. `strict` and `group` waits (fsync) do not apply: `relaxed` has none.

**`cores = 8`, lock cell.** Lock-partition waits are 0.0144-0.0231 per statement (14-23 per 1,000) and cost 0.004-0.011 µs of a
statement (§6's table); the wait-for graph never fired (`lock_wait_for_waits` 0 per statement in all 12 cells).

## 8. Host, load and the noise floor from inside the run

Every cell waited for a quiet gate before it started: a 1-minute load average at or under 1.6 and, in the chain and the
bisect scripts, no `cc1plus`, `cmake --build`, `ctest`, `kds_tests` or `bl_mixed` process (the chain also refused
`s2_matrix` and BP-S1's chain; the profile scripts gated on load alone). The driver's own
check (`pgrep -f 'cc1plus|cmake --build|ctest'`) found nothing at the start of any run, at the start of any of the 264
`bn_census` arms, or at the end of any run, nor before or after any of the 18 `ba_census` and 12 lock cells. The 1-minute
load at a cell's start still includes the previous cell's own server and client (0.94-1.60), and the 5- and 15-minute loads
fall through the window as the BQ-S2 matrix that ended at 19:08 and the builds at 19:22-19:24 age out.

| cell group | cells | first - last start (UTC) | 1-min load at start | 5-min load | 15-min load | resident non-benchmark `python3`/`kds_server` processes counted by the gate |
|---|---|---|---|---|---|---|
| cores = 1 pairs AB, AQ (`bn_census`) | 6 | 19:25:42 - 19:27:33 | 1.39 - 1.56 | 2.97 - 3.62 | 3.58 - 3.84 | 4, 5 |
| cores = 4 `point` | 9 | 19:27:45 - 19:48:57 | 1.36 - 1.49 | 2.25 - 2.92 | 2.69 - 3.56 | 4 |
| cores = 4 `insert1` | 9 | 19:32:36 - 19:53:39 | 1.36 - 1.59 | 2.24 - 2.49 | 2.64 - 3.24 | 4 |
| lock cells, cores = 8 | 12 | 19:54:57 - 19:58:16 | 1.38 - 1.59 | 1.94 - 2.30 | 2.42 - 2.62 | 4 |
| bisect: S2S1, S4S6BQ | 6 | 19:58:56 - 20:00:33 | 0.94 - 1.58 | 1.77 - 1.86 | 2.29 - 2.36 | not recorded by this script |
| bisect: MERGE, S4, S6 | 9 | 20:04:20 - 20:07:21 | 1.39 - 1.59 | 2.04 - 2.40 | 2.33 - 2.48 | not recorded by this script |
| MERGEALIGNED | 3 | 20:11:29 - 20:12:12 | 1.39 - 1.60 | 2.37 - 2.48 | 2.44 - 2.47 | not recorded by this script |
| PREMERGE_FIX, CLOSE_FIX | 6 | 20:19:16 - 20:21:13 | 1.39 - 1.56 | 2.59 - 3.10 | 2.66 - 2.81 | not recorded by this script |

The "resident processes" are the four idle `python3` daemons of the image (`networkd-dispatcher`,
`unattended-upgrade-shutdown`, two `waagent`), one more for a single cell (5 at `AQ r2`). Two other `kds_server`
processes were resident during the whole window and are excluded from the gate's count by pattern: an unrelated instance
under `/home/cdkbs/autotrade` on its own port (15432, `README.md` rule 5; never bound by a cell here) and a
`./kds_server --config kds.conf` started a day and a half earlier; their CPU during the window was not measured (`ps`
reports lifetime averages of 0.6-0.7 % each) and they are a standing, unquantified load on the 8 vCPUs.

**Noise floors from inside the run** are the table in §0 and the "spread" columns of every table. The two that decide
the verdicts: the identical-code pair S6 (+1.5 / +2.3 % on the every-row `UPDATE`, +4.1 / +3.2 % on the point `SELECT`) and
A's 5.4 % difference between session families. The `cores = 4` cells repeat each binary three times in rotated order;
their spread (0.1-8.1 % of throughput) is the floor for §5. The lock cell repeats `update-disjoint` inside every cell
(0.2-3.7 %).

## 9. Against this engine's own previous numbers

**BP-S3's file, same driver, same shapes, same 10,000 rows** (`results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md`, measured 14:21
and 19:10 UTC the same day; the update-lit rows were not re-run in the bisect pairs):

| shape (driver path) | `1592007e` in BP-S3's file (mean of 3) | `1592007e` here (mean of 3, S2S1) | here vs there | `ccf9cfc1` in BP-S3's file (mean of 3) | `ccf9cfc1` here (mean of 9) | here vs there |
|---|---|---|---|---|---|---|
| point `SELECT`, literal | 26.08 µs | 25.77 µs | -1.2 % | 26.06 µs | 26.06 µs | +0.0 % |
| point `SELECT`, prepared | 25.80 µs | 25.02 µs | -3.0 % | 25.46 µs | 25.28 µs | -0.7 % |
| every-row `UPDATE`, literal | 6,964.6 µs | 6,919.6 µs | -0.6 % | 6,866.6 µs | 6,753.3 µs | -1.7 % |
| every-row `UPDATE`, prepared | 7,090.9 µs | 7,025.4 µs | -0.9 % | 6,937.7 µs | 6,777.7 µs | -2.3 % |

The host reproduces the earlier absolute levels within 0.0-3.0 % (lower now in seven of eight rows, equal in one);
`ccf9cfc1`'s every-row figure reads 1.7-2.3 % lower now. That is a statement of the floor between sessions of one day, and it is the reason §3 pools.

**BP-S1's census, the `cores = 8` lock cell on the same binary** (`ccf9cfc1`, `kds_server.b`, sha256 `a6fc3c04...`;
`results-bp-s1-census-v2.7.0-909-gccf9cfc1.md` §5): plain 46.8 / 49.9 / 46.6 µs, fence 52.4 / 55.3 / 52.0 µs, premium +5.7 µs
(+12.2 %). Here, S6A: plain 45.4 / 46.2 / 45.3 µs, fence 51.6 / 52.0 / 51.2 µs, premium +6.2 µs (+13.6 %). The cell
reproduces (the earlier plain r1 was 49.9 µs, an outlier of the earlier session); B is measured against this, not against
a figure from another session.

**BQ's own previous numbers** (`results-bq-s1-...`, `results-bq-s2-...`) are `bl_mixed` cells (a bulk writer plus
readers, the roll's latched hold priced in hundreds of milliseconds); none of this file's cells is that shape, so no
delta is taken. Within `insert1` at `cores = 4` the roll holds are 1.4-26.1 ms with the body built ahead, a different
regime from the 300-330 ms BQ-S1 measured under the bulk writer (§5).

**`point` at `cores = 4`**: the nearest earlier number is `results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`'s
CPU-bound variant (4 x 8, kernel placement, 62,264 +- 2,726 stmt/s), measured 67 commits earlier **with
`waystone_recording = off`**; this file's `point` cell runs the default, so no delta is taken.

## 10. What the run teaches, and what it does not cover

- **A cost on the per-record path is paid per record.** BQ's instruments cost nothing a point statement or a four-core
  insert can show and +3-6 % of an every-row `UPDATE`, because the statement writes ten thousand records and the instrument
  sits in `WalManager::Append`. `src/wal/stream.cpp` says of the roll's clock reads "only here, so an append that does not
  roll reads no clock"; that holds for the clock and not for the struct, the extra argument and the latch hold that every
  append now carries, and this is its first per-record measurement: the instrument is free per roll and not free per
  append. About half of the cost is in the frame of `WalManager::Append` and survives making `RollRecord` lazy, which
  is evidence against the zero-initialisation being the cost; it is the first data point on that frame's contents.
- **BP-S6 spent the partition lever and left a fixed part.** At 64 partitions the held-fence premium is +5.1 %, 2.35 µs a
  statement, of which the walk's latches account for 0.45 µs at the 11 ns the two points fit; the rest does not scale with
  the partition count. Further reducing partitions would buy at most the 0.45 µs. This is `[inferred]` from two points.
- **The instrument cannot read a binary below about 2 % (every-row) or 4 % (point).** Three runs replicate a session. A
  close should carry a code-identical control pair (the same source built twice, or S6-like), run order swapped, and
  should pool by binary, as §3.2 does. An A that moves by 5.4 % with the other server in the pair makes every paired
  delta of that pair a statement about the pair.
- **The second-server stall is the harness's.** It changes how BP-S3's file reads (§4). Swapping which binary starts
  first in half of a pair's runs would spread it over both binaries instead of one.
- **`cores = 4` showed no regression**, but it has no per-statement CPU: `ba_census` records throughput and latency, and
  with eight sessions sharing four physical cores with their clients a 1-2 % server-CPU change is below it. The
  every-row finding is therefore a `cores = 1` finding that `cores = 4` does not contradict and cannot confirm.
- **Not measured.** `group` and `strict` (the durability axis of rule 4b): a roll at `group` and `strict` holds the stream latch
  through a sync and BQ's instruments sit on that path; BQ's and BP-S3's files cover those cells, this close does not. The
  200 and 1,000-row sizes (rule 9). The Cabin-served `SELECT` (BP-H3). Cores 2 and 8 outside the lock cell. A saturating
  every-row writer on many cores, where the instrument's cost would meet the stream latch's contention. PostgreSQL's floor.

