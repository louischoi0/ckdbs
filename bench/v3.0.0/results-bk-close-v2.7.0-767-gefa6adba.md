# BK's close: the 48-bit Keystone id costs no write or point read anything the host can resolve, and the whole-relation walk 1.2-1.8 % when the binary sits in a fair place

**Thesis.** BK widens the Keystone id from 40 to 48 bits inside the one 64-bit word: a mask constant, a compare bound, the Cabin
bound entry's shifts and the trail key's pack, nothing per row on a write. Measured on `worktree-bk-keystone-48` (B = `efa6adba`,
`v2.7.0-767-gefa6adba`, BK-S2) against the commit BK opened from (A = `5979b5e7`, `v2.7.0-763-g5979b5e7`), interleaved, in Release
builds, over the data sets below and five hours of other sessions' load:

- **No INSERT, SELECT-by-pk, UPDATE or DELETE shape is slower in B in any data set.** Their differences run from -0.85 to +0.10 µs
  at p50 (-1.6 to +0.2 %), mostly in B's favour, at 200, 1,000 and 10,000 rows and at `cores = 1` and `cores = 2`, and a
  byte-identical copy of B reads up to 0.80 µs away from B on SELECT (§4), so the spread between two files of one binary is as
  large as anything measured.
- **An INSERT whose named key sits in the upper eight bits of the id space** (`2^40 + i`, which A refuses `OutOfRange`) costs
  **+0.10 µs [+0.10, +0.15]** over the same INSERT below 2^40 in the same blocks (49.2 against 49.1 µs at p50, 10,000 rows; server
  CPU +0.12 µs per statement).
- **One shape moves: `COUNT/MIN/MAX` over a 100-id window, which walks the whole relation, by +1.2 % (+1.7 µs [+1.5, +2.2]) at
  1,000 rows and +1.5 % (+12.1 µs [+10.6, +16.0]) at 10,000 rows between fresh copies of the two binaries**, 1.2 ns a row of a
  74 ns walk, server work, and the same +1.7 to +1.8 % the two passes of the first matrix read (+14.05 µs at 04:46 and +14.75 µs at
  06:21, 10,000 rows). **It is not stable**: later in the day the B file read +36.9 µs (slots exchanged) and +42.5 µs (+5.3 %, the
  strict cell's walk arm), and **a copy of the same B bytes made at 09:32 UTC read 17.2 µs [16.4, 19.5] (2.1 %) faster than the B
  file, while two copies of A agree to 0.05 µs**: the walk, at this size, resolves where a binary's pages sit, and B's do not sit
  as well as A's.
- **The order expected flat (`instructions/v3.0.0/workorder-bk-keystone-48.md` §5, BK-Q4); the walk is reported, not resolved, and
  gates nothing (BK-Q4).** Its mechanism is not found: the disassembly of the two measured binaries shows B executing **one call
  fewer** per decoded row than A (§3), so the call structure BK-S2 changed cannot be what the walk pays.

- **A** = `5979b5e7`, `v2.7.0-763-g5979b5e7`, the commit BK opened from. Its binary is **byte-identical** to the BJ close's B'
  (`e458e272`, `v2.7.0-757-ge458e272`): same sha256, so the cross-file comparison of §6 sets one binary against itself.
- **B** = `efa6adba`, `v2.7.0-767-gefa6adba`, BK-S2 (the word), the last commit of BK that changes code. BK-S3 (`adff33be`,
  `v2.7.0-768-gadff33be`) changes documents only - `git diff efa6adba adff33be -- src include sim tests CMakeLists.txt tools` is
  empty - and BK-S4 did not exist when this ran, so B is the engine BK closes with. **The branch tip moved during this session**
  to `a45311b5` (`v2.7.0-772-ga45311b5`), a merge of `origin/main` that brings BJ's three review commits (`a1b06d5a`, `fdf882a9`,
  `e623fb5a`) into the branch; they change eight engine files relative to B (five sources, `set_expr.cpp`, `parser.cpp`,
  `fingerprint.cpp`, `step_compiler.cpp` and `command_dispatcher.cpp`, and three headers, `ast.hpp`, `step_compiler.hpp` and
  `command_dispatcher.hpp`) and **are not in this measurement**.
- Raw output: `archive/bk-close-v2.7.0-767-gefa6adba/`. The drivers that ran are copied there (measurement-stage copies;
  `tools/` is untouched); how to run them is in their docstrings, and `bench/docs/` stays closed (`bench/README.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-11, UTC: builds 04:38-04:40; first matrix 04:42-04:50 (`cores = 1`, pass 1); the rest, gated on a quiet host, between 05:00 and 09:56 |
| Branch / worktree | `worktree-bk-keystone-48`, worktree `bk-keystone-48`. Session start `adff33be` (`v2.7.0-768-gadff33be`), tip `a45311b5` at the end (see above). Only files under `bench/v3.0.0/` were added by this session; `git status` was clean before it started |
| A | `5979b5e7`, `v2.7.0-763-g5979b5e7`, committed 2026-10-11 01:34:23 UTC. Built from a fresh `git worktree add --detach` of that commit in the session scratchpad (clean by construction), removed afterwards. Binary mtime 2026-10-11 04:39:56 UTC, **newer than its commit** |
| B | `efa6adba`, `v2.7.0-767-gefa6adba`, committed 2026-10-11 04:34:56 UTC; the same way, a second detached worktree. Binary mtime 2026-10-11 04:39:56 UTC, **newer than its commit**. A to B under `src/ include/ sim/`: 35 files, 167 insertions, 151 deletions (BK-R1..R5, R7, R8: `keystone.hpp/.cpp`, the three duplicated constants, `PackKey`, `kSuperBlockVersion` 22, the refusal texts, the sim's constants, and `RowKeystoneId`'s deletion) |
| Build | Release (`CMAKE_BUILD_TYPE` read back from each `CMakeCache.txt`; `-O3 -DNDEBUG`, g++ 13.3.0, cmake 3.28.3), `-DKDS_WITH_TLS=ON -DKDS_BUILD_SIM=ON -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_BENCH=OFF`, **target `kds_server` only** (the sim and the tests were not built); A and B each in its own build directory, built niced on CPUs 0-5 and finished before the first statement was timed. No LTO |
| Measured copies (`archive/.../binaries.txt`) | A `164fa531cbb43273feae3aff544fe5625a4231263fae4fd830e7f9d850baa6b7` (copies `kds_server-A`, `-A2`, `-A3`), B `b4091b79fe0b6563b1c0d2226b3e1e8ebdef382715250168bd57895dd59600ee` (copies `kds_server-B`, `-B2`). Every server ran from a copy under the scratchpad's `run/bin/`; `-A2` (made with `-A` and `-B` at 04:40) is the null pairs' second slot, `-A3` and `-B2` were made at 09:32 UTC for the copy controls and the fresh pair (§4) |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads: siblings 0/1, 2/3, 4/5, 6/7), 15 GiB, Linux 7.0.0-1014-azure; data files and WAL under the scratchpad's `run/w-*` on `/dev/root`, **ext4** (`df -T`), not tmpfs (the volume was 98 % full when the session began, 7.0 GB free, and 32 % full at the end); driver on CPU 4; server on CPU 2 (`cores = 1`) or `taskset -c 2,3` (`cores = 2`, where the engine pins the second reactor to **CPU 1** itself: `src/server/expeditor.cpp:1929`, `reactor_cpus` empty means core k on CPU k, confirmed by a thread-affinity probe: `archive/.../evidence/thread-probe.txt`) |
| Server config | `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 or 2, **`durability = relaxed`** (the strict cell: `strict`), fresh server and data file per (cell, run); ports 32011 (slot A) and 32012 (slot B), chosen, not defaulted |
| Host load | **The host was shared and busy.** Other sessions built (`cc1plus` up to 8 at once, load average up to 20) and ran test suites for most of the session, and another session's benchmark held CPUs 6 and 7 for much of it. Per run: `/proc/loadavg`, the build/test process list, a per-CPU busy fraction over the run and the other session's CPUs 6,7 are in every JSON. After the first matrix the runner started a run only after three consecutive quiet 3 s windows (the CPUs outside the pinned set and outside 6,7 at or under 4 % busy, and no `cc1plus`, `cmake`, `ld`, `make` or `ninja` process); the discard rules were amended four times while it ran, each stated in the last appendix; runs kept / earlier attempts discarded and re-run: `cores = 1` 96 kept / 0 discarded; `cores = 2` 48 kept / 33 discarded; null pair 46 kept / 24 discarded; null pair at `cores = 2` 48 kept / 2 discarded; strict 18 kept / 18 discarded; slots exchanged 29 kept / 5 discarded; copy control B 16 kept / 0 discarded; copy control A 16 kept / 0 discarded; fresh pair 31 kept / 1 discarded (rules and reasons in the last appendix; the discards read the host and the control, never a statement's latency) |
| Suite | **Not executed in this session.** This measurement changes no code. The order's own record for BK-S2 is `ctest -j8` Debug 3500 of 3500 and `scripts/sim.sh` 190 runs, 0 failures |

## What was run, and what was not

Run, all by `archive/.../drivers/bk_overhead_ab.py` (a measurement-stage copy shaped like BH's `bh_overhead_ab.py` and BJ's
`bj_close_ab.py`; unmodified between cells) under `archive/.../drivers/runner.py`, with the discard rules of
`archive/.../drivers/analyze_bk.py`:

- **Overhead A/B**, relaxed: `cores = 1`, 200 / 1,000 / 10,000 rows, 32 runs per size (pass 1: 16 runs at 04:42-04:50 UTC on an
  idle host; pass 2: 16 runs under the quiet-gated runner); `cores = 2`, the same sizes, 16 runs per size. Every run is 3,000
  statements per arm, so 96,000 per arm and shape per size at `cores = 1` and 48,000 at `cores = 2`.
- **Null pairs**: the same A binary in both slots (`kds_server-A` and `-A2`), `cores = 1` (15-16 runs per size) and `cores = 2` (16
  per size), to measure what the method reads when nothing differs.
- **Flipped pair**: B's binary in slot A and A's in slot B, `cores = 1`, 1,000 rows (13 runs) and 10,000 rows (16 runs); read as
  new − old.
- **Copy controls and the fresh pair**, `cores = 1`, made at 09:32 UTC: the B bytes in both slots (`kds_server-B`, the file run
  all day, against `kds_server-B2`), the A bytes in both slots (`kds_server-A` against `-A3`), each at 10,000 rows, 16 runs; and **A
  (copy 3) against B (copy 2)**, both fresh files, 1,000 and 10,000 rows, 16 and 15 runs.
- **Strict**, `cores = 1`, 200 / 1,000 / 10,000 rows, 6 runs per size, 600 statements per arm and run (12 blocks of 50).
- **Refusal probe** in every run, on both servers (untimed): `INSERT` of `2^40`, `2^48` and `2^48 - 1` into a fresh relation, and
  a read-back by pk; the upper-bits keys of the named-pair arm are read back by pk; row counts of all nine relations are
  checked against the expected. A run that fails any of these is not kept.

Not run: the suite and the sim; `group` durability; more than two cores; more than one client; a PostgreSQL floor for these
shapes (not measured, and no longer owed: `CLAUDE.md`'s measurement baseline is this engine's own last run); other column types,
varchar spill, secondary indexes, foreign keys and assertions (the shapes BK touches by name - the Cabin bound entry word and the
trail key - are reached by `CREATE ASSERTION` and by repeated pattern replay, which no arm here exercises: they are pinned by
BK-S2's cells, not priced here); `cores = 2` with slots exchanged, and the walk at 200 rows with slots exchanged; a stage build or
`perf` to attribute the walk's increment (`perf_event_paranoid` = 4).

### The shapes, and the mapping from the driver to the row count

`--rows N` is the row count every relation holds when an arm starts (200 / 1,000 / 10,000, loaded with named ascending keys in
500-row `INSERT`s) and the size of the walk; each arm adds 3,000 statements per run (`--ops 3000`, 12 blocks of 250, A and B
alternating, the first server of each block alternating, and of each run by parity). Relations are `be_<t> (id int64, v int64, w
int64) BTREE`.

| arm | statement | relation | what it exercises |
|---|---|---|---|
| `insert` | `INSERT INTO be_a VALUES (n, n)` | `be_a` | pk omitted: the engine issues the id, `Encode` at the mark |
| `insert-again` | the same | `be_b` | replicate: same binary, same shape, second relation (the in-run noise floor) |
| `insert-named` | `INSERT INTO be_n VALUES (rows + i, i, i)` | `be_n` | pk named, ascending above the preload, keys below 2^40 (both binaries accept it) |
| `named-pair`, B only | `INSERT INTO be_m VALUES (rows + i, ...)` and `INSERT INTO be_h VALUES (2^40 + i, ...)`, blocks alternating which goes first | `be_m`, `be_h` | the same insert with keys below 2^40 and in the upper eight bits of the id space; A refuses the second (`OutOfRange`), so A runs nothing here |
| `select-pk` | `SELECT * FROM be_r WHERE id = k`, k spread by a stride of 7919 | `be_r` | one-row read, decode of the Keystone word |
| `update-pk` | `UPDATE be_u SET v = q WHERE id = k` | `be_u` | the Keystone word re-encoded in a new version, spread over the preload |
| `update-hot` | `UPDATE be_o SET v = q WHERE id = 1` | `be_o` | one row, its version chain growing by 3,000 per run |
| `delete-pk` | `DELETE FROM be_d WHERE id = k` (keys inserted untimed before each block) | `be_d` | delete-mark by pk |
| `range100` | `SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id >= lo AND id < lo + 100` | `be_r` | **walks the whole relation** (86 µs at 200 rows, 146 at 1,000, 805 at 10,000: linear, about 74 ns a row) |
| `ping` | `SHOW META` | none | control no Keystone path can touch |

## 1. The write path and the point reads are flat

At `cores = 1`, over 32 runs per size (two passes of 16), no INSERT, SELECT, UPDATE or DELETE shape moves by more than 0.5 µs at
p50, and each interval either sits on zero or lies inside the floor. The shapes B reads *faster* on - SELECT by pk and the two
UPDATEs, 0.1 to 0.5 µs - are the B slot's and the file's own bias, which the null pair reproduces at 1,000 rows and the copy control
reproduces on SELECT (§4), not a finding. Throughput at p50 is derived (`1,000,000 / p50`, A → B, median of the per-run changes):

### cores = 1

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 20,325 → 20,325 /s (+0.0 %) | 20,346 → 20,325 /s (+0.0 %) | 20,367 → 20,367 /s (+0.1 %) |
| INSERT again (replicate) | 20,325 → 20,325 /s (+0.0 %) | 20,325 → 20,325 /s (+0.0 %) | 20,367 → 20,367 /s (+0.0 %) |
| INSERT, pk named (keys below 2^40) | 20,325 → 20,408 /s (+0.2 %) | 20,325 → 20,367 /s (+0.0 %) | 20,408 → 20,367 /s (+0.0 %) |
| SELECT by pk | 15,279 → 15,408 /s (+0.7 %) | 14,793 → 14,881 /s (+0.7 %) | 15,408 → 15,480 /s (+0.4 %) |
| UPDATE by pk | 19,120 → 19,268 /s (+0.8 %) | 19,029 → 19,139 /s (+0.8 %) | 19,048 → 19,029 /s (+0.3 %) |
| UPDATE of one hot row | 19,194 → 19,361 /s (+0.6 %) | 19,212 → 19,286 /s (+0.5 %) | 19,268 → 19,268 /s (+0.2 %) |
| DELETE by pk | 20,367 → 20,367 /s (+0.2 %) | 20,367 → 20,346 /s (+0.0 %) | 20,325 → 20,367 /s (+0.0 %) |
| COUNT/MIN/MAX over 100 ids (walks the relation) | 11,561 → 11,507 /s (-0.7 %) | 6,870 → 6,796 /s (-1.1 %) | 1,242 → 1,221 /s (-1.8 %) |
| SHOW META (control) | 19,841 → 19,881 /s (-0.1 %) | 19,960 → 19,881 /s (-0.2 %) | 19,881 → 19,881 /s (+0.0 %) |

Δ p50, B − A (bold: the interval excludes zero **and** the median exceeds the floor, §4):

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.15, +0.10] | +0.00 µs [-0.05, +0.10] | -0.05 µs [-0.10, +0.00] |
| INSERT again (replicate) | +0.00 µs [-0.10, +0.10] | +0.00 µs [+0.00, +0.10] | +0.00 µs [-0.10, +0.10] |
| INSERT, pk named (keys below 2^40) | -0.10 µs [-0.20, +0.00] | +0.00 µs [-0.05, +0.05] | +0.00 µs [+0.00, +0.10] |
| SELECT by pk | -0.45 µs [-0.60, +0.00] | -0.50 µs [-0.75, -0.20] | -0.25 µs [-0.70, +0.00] |
| UPDATE by pk | -0.40 µs [-0.70, -0.30] | -0.40 µs [-0.50, +0.00] | -0.15 µs [-0.40, +0.30] |
| UPDATE of one hot row | -0.30 µs [-0.70, -0.20] | -0.25 µs [-0.40, +0.00] | -0.10 µs [-0.40, +0.20] |
| DELETE by pk | -0.10 µs [-0.20, +0.05] | +0.00 µs [-0.05, +0.10] | +0.00 µs [-0.15, +0.10] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.65 µs [+0.30, +0.90] | **+1.65 µs [+1.35, +1.90]** | **+14.35 µs [+11.60, +16.50]** |
| SHOW META (control) | +0.05 µs [-0.05, +0.10] | +0.10 µs [+0.00, +0.20] | +0.00 µs [-0.10, +0.10] |

### cores = 2

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | 20,161 → 20,243 /s (+0.3 %) | 20,161 → 20,202 /s (+0.2 %) | 20,284 → 20,346 /s (+0.2 %) |
| INSERT again (replicate) | 20,202 → 20,284 /s (+0.1 %) | 20,202 → 20,161 /s (+0.0 %) | 20,243 → 20,325 /s (+0.3 %) |
| INSERT, pk named (keys below 2^40) | 20,040 → 20,080 /s (+0.2 %) | 20,060 → 20,060 /s (+0.0 %) | 20,040 → 20,141 /s (+0.8 %) |
| SELECT by pk | 14,970 → 15,106 /s (+0.9 %) | 14,482 → 14,577 /s (+1.1 %) | 15,186 → 15,326 /s (+0.9 %) |
| UPDATE by pk | 18,570 → 18,657 /s (+0.6 %) | 18,450 → 18,657 /s (+1.1 %) | 18,433 → 18,587 /s (+0.7 %) |
| UPDATE of one hot row | 18,692 → 18,832 /s (+0.6 %) | 18,570 → 18,832 /s (+1.2 %) | 18,692 → 18,939 /s (+1.1 %) |
| DELETE by pk | 20,182 → 20,202 /s (-0.1 %) | 20,161 → 20,202 /s (+0.2 %) | 20,222 → 20,243 /s (+0.2 %) |
| COUNT/MIN/MAX over 100 ids (walks the relation) | 11,364 → 11,351 /s (-0.3 %) | 6,754 → 6,691 /s (-0.8 %) | 1,229 → 1,204 /s (-2.0 %) |
| SHOW META (control) | 19,782 → 19,763 /s (-0.2 %) | 19,724 → 19,646 /s (-0.4 %) | 19,822 → 19,704 /s (-0.9 %) |

Δ p50, B − A, `cores = 2` (16 runs per size; the floor is 0.60, 0.89 and 1.11 µs):

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.40, +0.05] | -0.10 µs [-0.40, +0.10] | -0.10 µs [-0.20, +0.20] |
| INSERT again (replicate) | -0.05 µs [-0.30, +0.10] | +0.00 µs [-0.20, +0.30] | -0.15 µs [-0.30, +0.10] |
| INSERT, pk named (keys below 2^40) | -0.10 µs [-0.30, +0.00] | +0.00 µs [-0.30, +0.30] | -0.40 µs [-0.50, -0.20] |
| SELECT by pk | -0.60 µs [-0.90, -0.40] | -0.75 µs [-1.00, +0.10] | -0.60 µs [-0.80, -0.20] |
| UPDATE by pk | -0.30 µs [-0.85, +0.30] | -0.60 µs [-1.40, +0.05] | -0.40 µs [-0.80, -0.20] |
| UPDATE of one hot row | -0.30 µs [-1.10, +0.25] | -0.65 µs [-1.40, +0.00] | -0.60 µs [-0.80, -0.35] |
| DELETE by pk | +0.05 µs [-0.40, +0.20] | -0.10 µs [-0.30, +0.15] | -0.10 µs [-0.30, +0.10] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.30 µs [+0.00, +0.50] | **+1.20 µs [+0.70, +1.80]** | **+16.95 µs [+13.90, +25.80]** |
| SHOW META (control) | +0.10 µs [+0.00, +0.30] | +0.20 µs [+0.00, +0.40] | +0.45 µs [+0.10, +0.60] |

At `cores = 2` the point shapes read up to 0.75 µs *lower* in B, with the `SHOW META` control +0.10 to +0.45 µs the other way.
Net of the `cores = 2` null pair (the same binary in both slots, §4), difference of medians with a bootstrap CI over both sets:

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.40, +0.20] | -0.10 µs [-0.45, +0.10] | -0.10 µs [-0.85, +0.20] |
| INSERT again (replicate) | +0.05 µs [-0.40, +0.25] | -0.10 µs [-0.30, +0.25] | -0.25 µs [-0.50, +0.05] |
| INSERT, pk named (keys below 2^40) | +0.00 µs [-0.30, +0.35] | +0.05 µs [-0.45, +0.40] | -0.50 µs [-0.80, -0.05] |
| SELECT by pk | -0.50 µs [-1.15, -0.15] | -0.65 µs [-1.00, +0.25] | -0.55 µs [-0.95, -0.05] |
| UPDATE by pk | -0.40 µs [-1.10, +0.20] | -0.55 µs [-1.40, -0.05] | **-0.70 µs [-1.15, -0.30]** |
| UPDATE of one hot row | -0.30 µs [-1.25, +0.30] | **-0.65 µs [-1.50, -0.05]** | **-0.85 µs [-1.35, -0.30]** |
| DELETE by pk | -0.15 µs [-0.60, +0.15] | -0.15 µs [-0.50, +0.15] | -0.35 µs [-0.95, -0.15] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.35 µs [-0.50, +0.80] | **+1.15 µs [+0.55, +2.20]** | **+18.50 µs [+14.10, +26.95]** |
| SHOW META (control) | +0.30 µs [+0.10, +0.50] | +0.50 µs [+0.00, +0.75] | +0.35 µs [-0.05, +0.50] |

**Three cells resolve, all in B's favour**: the two UPDATE shapes at 10,000 rows (-0.70 [-1.15, -0.30] and -0.85 [-1.35, -0.30] µs)
and the hot-row UPDATE at 1,000 rows (-0.65 [-1.50, -0.05]), 1.2-1.6 % of the statement. They are of the size by which byte-identical
copies of B differ on point shapes (§4: SELECT -0.80, UPDATE -0.45 µs) and are not seen at `cores = 1` (the same shapes net of
that null pair: -0.35 to +0.15 µs, none resolved); BK-S2 changes nothing on the UPDATE path that would favour two cores, and
this is reported as measured, not attributed.

## 2. The Keystone shapes: a named pk in the upper eight bits costs what one below 2^40 costs

BK's one visible behaviour is that `2^40` through `2^48 - 1` are valid keys. The named-pair arm inserts `2^40 + i` and `rows + i`
in alternating blocks on the same server, so the comparison is within one run and one binary:

| cores | rows | lo p50 | hi p50 | Δ p50, hi − lo | Δ mean | Δ server CPU per statement | hi against A's low-key named insert (cross-arm) |
|---|---|---|---|---|---|---|---|
| cores = 1 | 200 | 49.1 µs | 49.2 µs | +0.10 µs [+0.00, +0.10] | +0.09 µs [-0.57, +0.68] | +0.12 µs [+0.00, +0.17] | +0.00 µs [-0.05, +0.00] |
| cores = 1 | 1,000 | 49.2 µs | 49.3 µs | +0.10 µs [+0.00, +0.10] | +0.44 µs [-0.27, +1.02] | +0.09 µs [+0.03, +0.15] | +0.10 µs [+0.00, +0.20] |
| cores = 1 | 10,000 | 49.1 µs | 49.2 µs | +0.10 µs [+0.10, +0.15] | +0.09 µs [-0.47, +0.68] | +0.12 µs [+0.04, +0.17] | +0.20 µs [+0.00, +0.30] |
| cores = 2 | 200 | 49.5 µs | 49.8 µs | +0.25 µs [+0.10, +0.35] | -0.14 µs [-0.73, +0.46] | +0.16 µs [+0.13, +0.23] | +0.00 µs [-0.40, +0.20] |
| cores = 2 | 1,000 | 49.7 µs | 49.7 µs | +0.10 µs [+0.05, +0.20] | +0.27 µs [-0.31, +0.86] | +0.08 µs [-0.06, +0.20] | -0.10 µs [-0.30, +0.20] |
| cores = 2 | 10,000 | 49.5 µs | 49.6 µs | +0.10 µs [+0.10, +0.15] | -0.26 µs [-0.99, +0.44] | -0.10 µs [-0.39, +0.04] | -0.30 µs [-0.40, -0.10] |

The upper-bits insert is **+0.10 µs at p50** (0.2 %) over the lower one in five of the six cells (every size at `cores = 1`, and
1,000 and 10,000 rows at `cores = 2`), the clock's 0.1 µs resolution, and **+0.25 µs [+0.10, +0.35]** (0.5 %) in the sixth,
`cores = 2` at 200 rows. The interval excludes zero at 10,000 rows at `cores = 1` (`[+0.10, +0.15]`) and at all three sizes at
`cores = 2`; every cell is inside the floor (0.6 µs at the least, §4); its mean differs by less than the mean's own interval
everywhere, and server CPU by -0.10 to +0.16 µs. Against A's low-key named insert (the cross-arm column) it is -0.30 to
+0.20 µs. **A 48-bit id costs an INSERT nothing above the floor (+0.10 to +0.25 µs at p50) whichever bits the key uses.** Evidence that
the arm ran the behaviour it claims, from every kept run: A refused `2^40`, `2^48` and `2^48 - 1` (`ERR primary key ... is
outside the Keystone id space [1, 1099511627775]`); B placed `2^40` and `2^48 - 1` (read back by pk), refused `2^48` naming
`[1, 281474976710655]` (BK-R5), and read back the first and last of the 3,000 upper-bits rows it had placed.

## 3. The walk: 1.2-1.8 % slower between fair placements, 5 % in the worst one seen, and no call to blame

`range100` is the only shape that moves, and it moves with the row count; it is the one shape here that touches every row
(about 74 ns a row: 86.5, 145.6 and 805.2 µs at 200, 1,000 and 10,000 rows in A). Every data set that measured it, in the
order it ran, absolute slot p50s (median of the per-run p50s) and the difference slot B − slot A (95 % CI of the median over runs;
in the exchanged-slots rows slot A holds B's binary, so a negative difference there is B's walk being slower):

| data set | window (UTC) | cores | rows | binary in slot A → slot B | slot A p50 | slot B p50 | slot B − slot A (95 % CI) | % of slot A | runs |
|---|---|---|---|---|---|---|---|---|---|
| main, pass 1 | 04:42-04:44 | 1 | 200 | A → B | 86.5 µs | 87.1 µs | +0.65 µs [+0.20, +1.10] | +0.8 % | 16 |
| main, pass 1 | 04:44-04:46 | 1 | 1,000 | A → B | 145.4 µs | 147.1 µs | +1.75 µs [+1.30, +2.20] | +1.2 % | 16 |
| main, pass 1 | 04:46-04:50 | 1 | 10,000 | A → B | 804.2 µs | 817.9 µs | +14.05 µs [+7.30, +15.80] | +1.7 % | 16 |
| main, pass 2 | 06:11-06:16 | 1 | 200 | A → B | 86.2 µs | 86.7 µs | +0.65 µs [+0.00, +0.90] | +0.8 % | 16 |
| main, pass 2 | 06:16-06:21 | 1 | 1,000 | A → B | 145.7 µs | 147.2 µs | +1.55 µs [+1.10, +1.90] | +1.1 % | 16 |
| main, pass 2 | 06:21-06:38 | 1 | 10,000 | A → B | 806.1 µs | 820.4 µs | +14.75 µs [+11.70, +24.90] | +1.8 % | 16 |
| null pair | 04:50-07:12 | 1 | 200 | A → A (copy 2) | 86.5 µs | 86.2 µs | -0.20 µs [-0.40, +0.20] | -0.2 % | 15 |
| null pair | 05:01-05:56 | 1 | 1,000 | A → A (copy 2) | 146.1 µs | 145.9 µs | +0.00 µs [-0.70, +0.80] | +0.0 % | 15 |
| null pair | 05:06-05:09 | 1 | 10,000 | A → A (copy 2) | 799.7 µs | 804.0 µs | +5.65 µs [+3.10, +9.10] | +0.7 % | 16 |
| main, `cores = 2` | 05:09-06:00 | 2 | 200 | A → B | 88.0 µs | 88.1 µs | +0.30 µs [+0.00, +0.50] | +0.3 % | 16 |
| main, `cores = 2` | 05:11-06:00 | 2 | 1,000 | A → B | 148.1 µs | 149.4 µs | +1.20 µs [+0.70, +1.80] | +0.8 % | 16 |
| main, `cores = 2` | 05:14-06:11 | 2 | 10,000 | A → B | 813.5 µs | 830.5 µs | +16.95 µs [+13.90, +25.80] | +2.1 % | 16 |
| null pair, `cores = 2` | 09:09-09:14 | 2 | 200 | A → A (copy 2) | 87.4 µs | 87.5 µs | -0.05 µs [-0.50, +0.70] | -0.1 % | 16 |
| null pair, `cores = 2` | 09:15-09:19 | 2 | 1,000 | A → A (copy 2) | 147.7 µs | 147.4 µs | +0.05 µs [-0.80, +0.50] | +0.0 % | 16 |
| null pair, `cores = 2` | 09:19-09:26 | 2 | 10,000 | A → A (copy 2) | 815.5 µs | 813.8 µs | -1.55 µs [-3.70, +1.40] | -0.2 % | 16 |
| strict cell's walk arm (600 statements) | 06:39-08:13 | 1 | 200 | A → B | 87.0 µs | 87.8 µs | +0.60 µs [+0.15, +1.40] | +0.7 % | 6 |
| strict cell's walk arm (600 statements) | 08:13-08:18 | 1 | 1,000 | A → B | 146.9 µs | 151.6 µs | +4.70 µs [+4.30, +5.25] | +3.2 % | 6 |
| strict cell's walk arm (600 statements) | 08:21-09:26 | 1 | 10,000 | A → B | 807.4 µs | 854.2 µs | +42.50 µs [+37.15, +51.00] | +5.3 % | 6 |
| slots exchanged | 08:58-09:27 | 1 | 1,000 | B → A | 150.9 µs | 147.0 µs | -4.00 µs [-5.00, -2.90] | -2.7 % | 13 |
| slots exchanged | 09:03-09:09 | 1 | 10,000 | B → A | 839.3 µs | 804.5 µs | -36.90 µs [-41.10, -29.30] | -4.4 % | 16 |
| copy control: B bytes in both slots | 09:33-09:39 | 1 | 10,000 | B → B (copy 2) | 834.8 µs | 818.0 µs | -17.20 µs [-19.50, -16.40] | -2.1 % | 16 |
| copy control: A bytes in both slots | 09:39-09:45 | 1 | 10,000 | A → A (copy 3) | 797.8 µs | 800.9 µs | -0.05 µs [-2.70, +8.20] | -0.0 % | 16 |
| fresh copies: A (copy 3) against B (copy 2) | 09:45-09:50 | 1 | 1,000 | A (copy 3) → B (copy 2) | 145.2 µs | 147.1 µs | +1.70 µs [+1.50, +2.20] | +1.2 % | 16 |
| fresh copies: A (copy 3) against B (copy 2) | 09:51-09:56 | 1 | 10,000 | A (copy 3) → B (copy 2) | 798.2 µs | 813.0 µs | +12.10 µs [+10.60, +16.00] | +1.5 % | 15 |

How to read it. **At `cores = 1` A is stable**: it reads 798-807 µs at 10,000 rows in every cell it occupies, in either slot, in any
copy (A against its copy 3: -0.05 µs [-2.7, +8.2]). **B is not**: it reads 817-821 µs early in the day (pass 1, pass 2), 813 in a
fresh copy at 09:50, and 835-854 µs from the original file later (the strict cell, the exchanged slots, the copy control's first
slot), and **the same B bytes in a copy made at 09:32 read 17.2 µs [16.4, 19.5] lower than the B file** (-2.1 %). So the walk resolves
where a binary's pages sit (the page cache's physical placement of the file, whose pages the other sessions' builds evict and
refill), and B's placement matters where A's does not. **The fair figure is the fresh-against-fresh pair: +1.7 µs [+1.5, +2.2]
(+1.2 %) at 1,000 rows and +12.1 µs [+10.6, +16.0] (+1.5 %) at 10,000**, which the 04:46 and 06:21 passes bracket (+1.55 to +1.75
and +14.05 to +14.75 µs); the +37 and +43 µs are B's file in a poor place. The increment is **server work** (server CPU per
statement +1.65 and +15.85 µs in the fresh pair), linear in rows (1.2 ns a row), and small at 200 rows (+0.65 µs, 0.8 %). At
`cores = 2` the main order reads +0.30, +1.20 and +16.95 µs against a `cores = 2` null pair of -0.05, +0.05 and -1.55 µs.

What the code says, from the two binaries that ran (`nm -S -C` and `objdump -d` on the measured copies;
`archive/.../evidence/callees.txt` and `disasm-*.txt`):

| per decoded row | A (`5979b5e7`) | B (`efa6adba`) |
|---|---|---|
| `DecodeRowInto` calls | `RowKeystoneId` (`row_codec.cpp`) | `KeystoneIdOfPayload` (`keystone.cpp`) |
| that function calls | `Keystone::Decode`, out of line | nothing: `Decode` is inlined |
| calls per decoded row | **2** | **1** |
| the id mask | `movabs $0xffffffffff,%rdx; and` | `movabs $0xffffffffffff,%rdx; and` |

(The other `call` in each reading function is `operator new` on its short-payload error path.) BK-S2's cut (`RowKeystoneId`
deleted, its callers calling `KeystoneIdOfPayload`) therefore removed a call from the per-row path of every decode; the mask is one
`movabs` and one `and` at either width. **The walk got slower while the code on its row path got shorter**, so the increment is
not attributed to anything BK-S2 wrote. The hypothesis that fits is code placement of the walk's hot loop (B's binary is 16 KB
larger, 5,701,520 against 5,685,056 bytes, so every function after the first changed one shifts, and BH's close recorded this same
shape moving between -8 % and +13 % from build to build with its own code unchanged: `results-bh-close-v2.7.0-712-g4015736d.md`
§1, quoting the 711 file's §2), and the copy control shows B's walk is placement-sensitive in the one way a placement hypothesis
predicts. It remains a hypothesis: a stage build (A with an unrelated size change) would price the placement effect and `perf`
(locked out) would show where the time goes; neither was run.

## 4. The noise floor, and the controls

Eight independent measures of what this method reads when nothing differs (point shapes, then the walk):

| measure | cores = 1 | cores = 2 |
|---|---|---|
| replicate arm (INSERT again − INSERT, p50 per run), median / robust sd, A | +0.00 µs / 0.30 µs (200), +0.05 / 0.22 (1,000), -0.10 / 0.15 (10,000) | +0.00 / 0.15, +0.00 / 0.30, +0.05 / 0.22 |
| replicate arm, B | +0.00 / 0.22, +0.00 / 0.15, +0.00 / 0.15 | -0.10 / 0.15, +0.00 / 0.30, +0.10 / 0.37 |
| control `SHOW META`, Δ p50 B − A | +0.05 [-0.05, +0.10], +0.10 [0.00, +0.20], +0.00 [-0.10, +0.10] | +0.10 [0.00, +0.30], +0.20 [0.00, +0.40], +0.45 [+0.10, +0.60] |
| **null pair** (A and A copy 2), point shapes, slot B − slot A | -0.50 to +0.20 µs (SELECT -0.10 / -0.50 / +0.05, UPDATE -0.20 / -0.50 / +0.20) | -0.10 to +0.30 µs (UPDATE +0.10 / -0.05 / +0.30, DELETE +0.20 / +0.05 / +0.25) |
| null pair, the walk | -0.20 [-0.40, +0.20], +0.00 [-0.70, +0.80], **+5.65 [+3.10, +9.10]** µs | -0.05 [-0.50, +0.70], +0.05 [-0.80, +0.50], -1.55 [-3.70, +1.40] |
| **copy control, B bytes in both slots** (10,000 rows) | SELECT **-0.80 [-1.20, -0.60]**, UPDATE -0.45 [-0.80, -0.30], INSERT named -0.10, `SHOW META` -0.30; the walk **-17.20 [-19.50, -16.40]** | not run |
| copy control, A bytes in both slots (10,000 rows) | SELECT -0.20 [-0.50, +0.10], UPDATE +0.00 [-0.30, +0.30]; the walk -0.05 [-2.70, +8.20] | not run |
| flipped pair (new − old), point shapes | 1,000 rows -0.40 to +0.00 µs, 10,000 rows -0.10 to +0.10 µs | not run |

**A delta is read as a finding only if its 95 % CI excludes zero and its median exceeds the floor**: the larger of 0.6 µs, three
times the replicate arms' robust sd, and (where a null pair exists) the null pair's median and CI half-width for that shape and
size. **The copy control is the stricter floor and it is not in the formula**: two files of the same B bytes differ by 0.8 µs on
SELECT and by 2.1 % on the walk, so a delta of that size between two *different* binaries is not an engine fact until a
fresh-against-fresh pair repeats it (§3 does that for the walk; the point shapes' deltas reach -0.85 µs, §1 at `cores = 2` net of the
null pair, the size of that spread and not inside it, so they are reported as measured, not as engine facts). The null pair, per size:

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, -0.10] | +0.00 µs [-0.20, +0.20] | -0.10 µs [-0.15, +0.20] |
| INSERT again (replicate) | -0.20 µs [-0.30, +0.00] | +0.00 µs [-0.10, +0.10] | +0.00 µs [+0.00, +0.10] |
| INSERT, pk named (keys below 2^40) | -0.10 µs [-0.20, +0.00] | -0.20 µs [-0.30, +0.10] | +0.00 µs [-0.20, +0.15] |
| SELECT by pk | +0.00 µs [-0.60, +0.20] | -0.50 µs [-0.90, -0.30] | +0.05 µs [-0.30, +0.40] |
| UPDATE by pk | -0.20 µs [-0.50, +0.10] | -0.50 µs [-1.00, +0.10] | +0.20 µs [-0.20, +0.50] |
| UPDATE of one hot row | -0.30 µs [-0.40, +0.10] | -0.40 µs [-0.90, +0.00] | +0.15 µs [-0.20, +0.50] |
| DELETE by pk | -0.20 µs [-0.30, +0.10] | -0.10 µs [-0.20, +0.00] | +0.00 µs [-0.10, +0.20] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | -0.20 µs [-0.40, +0.20] | +0.00 µs [-0.70, +0.80] | +5.65 µs [+3.10, +9.10] |
| SHOW META (control) | +0.20 µs [+0.00, +0.30] | +0.00 µs [-0.30, +0.20] | +0.10 µs [-0.10, +0.20] |

The flipped pair, new − old (the slots are exchanged, so the slot's bias changes sign; every point shape reads within -0.4 and
+0.1 µs, and the walk, which the file's placement moves, +4.00 µs and +36.90 µs):

| shape | 1,000 rows | 10,000 rows |
|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, +0.50] | +0.00 µs [-0.20, +0.20] |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | +0.00 µs [-0.20, +0.10] |
| INSERT, pk named (keys below 2^40) | -0.10 µs [-0.30, +0.20] | +0.10 µs [-0.10, +0.15] |
| SELECT by pk | -0.40 µs [-1.30, +0.00] | -0.10 µs [-0.40, +0.15] |
| UPDATE by pk | -0.10 µs [-0.50, +0.50] | +0.00 µs [-0.45, +0.35] |
| UPDATE of one hot row | +0.00 µs [-0.40, +0.50] | -0.05 µs [-0.60, +0.30] |
| DELETE by pk | -0.10 µs [-0.20, +0.00] | +0.00 µs [-0.10, +0.10] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +4.00 µs [+2.90, +5.00] | +36.90 µs [+29.30, +41.10] |
| SHOW META (control) | +0.20 µs [+0.00, +0.30] | -0.10 µs [-0.20, +0.00] |

The two passes of `cores = 1` (16 runs each; pass 1 ran on an idle host before any other session's load, pass 2 under the
quiet-gated runner while another session's benchmark held CPUs 6,7 in 8 of its 16 runs at 200 rows and 5 at 1,000):

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.20] | +0.10 µs [+0.00, +0.20] | -0.05 µs [-0.15, +0.05] |
| INSERT again (replicate) | +0.05 µs [-0.10, +0.20] | +0.05 µs [-0.10, +0.20] | -0.10 µs [-0.10, +0.10] |
| INSERT, pk named (keys below 2^40) | +0.00 µs [-0.10, +0.00] | +0.00 µs [-0.10, +0.10] | +0.00 µs [-0.05, +0.10] |
| SELECT by pk | -0.10 µs [-0.90, +0.10] | -0.55 µs [-0.75, -0.10] | -0.45 µs [-0.80, +0.00] |
| UPDATE by pk | -0.30 µs [-0.40, +0.00] | -0.05 µs [-0.55, +0.20] | -0.05 µs [-0.45, +0.60] |
| UPDATE of one hot row | -0.20 µs [-0.35, +0.00] | -0.10 µs [-0.40, +0.30] | -0.10 µs [-0.50, +0.50] |
| DELETE by pk | -0.15 µs [-0.30, +0.30] | +0.00 µs [-0.10, +0.20] | -0.10 µs [-0.20, +0.00] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.65 µs [+0.20, +1.10] | +1.75 µs [+1.30, +2.20] | +14.05 µs [+7.30, +15.80] |
| SHOW META (control) | +0.00 µs [-0.50, +0.10] | +0.05 µs [+0.00, +0.20] | -0.10 µs [-0.40, +0.10] |

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.25, +0.10] | -0.05 µs [-0.20, +0.10] | -0.05 µs [-0.15, +0.10] |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.05] | +0.00 µs [-0.10, +0.20] | +0.10 µs [-0.10, +0.20] |
| INSERT, pk named (keys below 2^40) | -0.20 µs [-0.35, -0.10] | +0.00 µs [-0.20, +0.10] | +0.10 µs [-0.05, +0.20] |
| SELECT by pk | -0.60 µs [-0.70, -0.10] | -0.40 µs [-1.00, +0.00] | -0.10 µs [-0.75, +0.10] |
| UPDATE by pk | -0.70 µs [-0.85, -0.25] | -0.50 µs [-0.75, -0.10] | -0.20 µs [-0.50, +0.20] |
| UPDATE of one hot row | -0.70 µs [-0.90, -0.30] | -0.40 µs [-0.80, +0.00] | -0.10 µs [-0.40, +0.05] |
| DELETE by pk | -0.10 µs [-0.20, +0.10] | +0.05 µs [-0.10, +0.15] | +0.05 µs [-0.20, +0.20] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.65 µs [+0.00, +0.90] | +1.55 µs [+1.10, +1.90] | +14.75 µs [+11.70, +24.90] |
| SHOW META (control) | +0.10 µs [+0.00, +0.30] | +0.15 µs [+0.00, +0.30] | +0.10 µs [-0.10, +0.20] |

**Sensitivity to the other session's benchmark.** Splitting the kept runs by whether CPUs 6,7 were busy over 10 % moves no
point-shape median by more than 0.7 µs in either direction and the `ping` p50 not at all (50.1-50.7 µs in both halves); the walk
reads +0.50/+0.85 (200 rows), +1.70/+1.10 (1,000) and +14.35 / n.a. (10,000) at `cores = 1` (quiet / busy) and +14.40 / +17.10 at
`cores = 2`, 10,000 rows (`archive/.../evidence/sens.txt`). CPUs 6 and 7 are one physical core that shares only the L3 and memory
with the pinned CPUs. **The mount-time device-stall thermometer** (`recovery_checkpoint_us`, median 7-9 ms): 7 of 96 `cores = 1`
runs and 1 of 46 null runs read more than 100 ms in one arm at mount; dropping them moves no median by more than 0.15 µs
(`archive/.../evidence/stalls.txt`).

## 5. Waits

Every statement here is one client, one connection, serial: there is **no lock or conflict wait** (0 error replies in every kept
run) and, at `durability = relaxed`, **no commit wait**. What a statement's p50 is made of, `cores = 1`, 1,000 rows, and what it
becomes under `strict` (6 runs per size, 600 statements per arm; the A arm; the commit wait is strict p50 less the relaxed p50 at
the same row count):

| shape | relaxed p50 (A) | server CPU per statement (A) | outside the server's CPU (client, loopback, wake-up) | strict p50 (A), rows as stated | commit wait: strict − relaxed p50 | commit wait as a share of the strict p50 |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 28.5 µs | 20.6 µs | 1004 µs (200 rows); 1007 µs (1,000 rows); 1008 µs (10,000 rows) | +955 µs; +958 µs; +958 µs | 95 %; 95 %; 95 % |
| INSERT, pk named | 49.2 µs | 28.8 µs | 20.4 µs | 1006 µs (200 rows); 1011 µs (1,000 rows); 1004 µs (10,000 rows) | +957 µs; +962 µs; +955 µs | 95 %; 95 %; 95 % |
| UPDATE by pk | 52.5 µs | 31.0 µs | 21.6 µs | 1011 µs (200 rows); 1013 µs (1,000 rows); 1008 µs (10,000 rows) | +959 µs; +961 µs; +956 µs | 95 %; 95 %; 95 % |
| UPDATE of one hot row | 52.0 µs | 30.7 µs | 21.3 µs | 1005 µs (200 rows); 1012 µs (1,000 rows); 1008 µs (10,000 rows) | +953 µs; +960 µs; +956 µs | 95 %; 95 %; 95 % |
| DELETE by pk | 49.1 µs | 28.1 µs | 21.0 µs | 994 µs (200 rows); 995 µs (1,000 rows); 999 µs (10,000 rows) | +945 µs; +946 µs; +950 µs | 95 %; 95 %; 95 % |
| SELECT by pk | 67.6 µs | 35.7 µs | 31.9 µs | 69 µs (200 rows); 66 µs (1,000 rows); 66 µs (10,000 rows) | +4 µs; -2 µs; +1 µs | 6 %; -3 %; 2 % |
| COUNT/MIN/MAX over 100 ids | 145.6 µs | 112.6 µs | 32.9 µs | 87 µs (200 rows); 147 µs (1,000 rows); 807 µs (10,000 rows) | +0 µs; +1 µs; +2 µs | 1 %; 1 %; 0 % |
| SHOW META (control) | 50.1 µs | 32.9 µs | 17.2 µs | 52 µs (200 rows); 52 µs (1,000 rows); 51 µs (10,000 rows) | +1 µs; +2 µs; +1 µs | 2 %; 4 %; 1 % |

The client and the socket are about 17-21 µs of every write's p50 (the part outside the server's CPU: the Python client's frame
encode and decode, the loopback round trip and the reactor's wake-up) and 32 µs of a `SELECT` (it also renders the row).
**Durability is the wait that dominates when it is switched on**: an INSERT, UPDATE or DELETE p50 is 994-1,013 µs against 49-53 µs
relaxed, **a commit wait of 945-962 µs, 95 % of the statement, at every size**; reads pay none (SELECT, `SHOW META` and the walk
move by -2 to +4 µs, noise). The commit wait is the device's fsync and it is the same in A and B: strict Δ p50, B − A,

| shape | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|
| INSERT, pk omitted | -0.55 µs [-6.75, +8.50] | +6.80 µs [-4.55, +15.15] | +2.15 µs [-4.70, +6.35] |
| INSERT again (replicate) | +0.10 µs [-3.15, +14.45] | +0.70 µs [-6.45, +3.35] | -1.05 µs [-8.90, +10.40] |
| INSERT, pk named (keys below 2^40) | -0.80 µs [-5.10, +1.95] | +5.60 µs [-9.30, +10.00] | +1.10 µs [-11.70, +10.10] |
| SELECT by pk | -1.00 µs [-2.85, +1.20] | -0.20 µs [-0.80, +0.10] | +0.00 µs [-1.05, +0.65] |
| UPDATE by pk | -3.10 µs [-6.55, +3.00] | +3.85 µs [-8.00, +7.40] | -2.25 µs [-4.95, +7.25] |
| UPDATE of one hot row | +0.15 µs [-7.05, +0.65] | -3.05 µs [-4.15, +4.30] | +0.60 µs [-3.75, +7.35] |
| DELETE by pk | +1.90 µs [-1.40, +8.15] | +1.75 µs [-5.10, +5.50] | -4.10 µs [-6.30, +9.85] |
| COUNT/MIN/MAX over 100 ids (walks the relation) | +0.60 µs [+0.15, +1.40] | +4.70 µs [+4.30, +5.25] | **+42.50 µs [+37.15, +51.00]** |
| SHOW META (control) | -0.30 µs [-0.80, +1.65] | -0.80 µs [-1.15, -0.20] | +0.10 µs [-0.30, +0.50] |

every write shape's interval across zero within a floor of 20-47 µs. **Strict hides anything BK could cost on a write** - the
commit wait is 95 % of the statement - and the strict cell shows only that nothing durability-shaped hides behind the relaxed
flatness. (Its walk row, +42.50 µs at 10,000 rows, is the B file's placement at 08:21-09:26, §3, not a durability fact: the walk
is a read.)

## 6. Against the engine's own last closes

The comparator is this engine's own previous close of the same shape (`CLAUDE.md`, Version Management; PostgreSQL is not
measured here). **The BJ close's B' (`e458e272`, `v2.7.0-757-ge458e272`) is the same binary as this A** (hash above; six commits
apart, no `src/ include/ sim/` change between them), so the A column reads the day-to-day drift of one binary on this host and the
B column reads BK on top of it. Median of the per-run p50 for each:

| cell | shape | BJ close B' (757), median of runs | this A (763), same binary | this B (767) |
|---|---|---|---|---|
| cores = 1, 1,000 rows | UPDATE by pk | 51.75 µs | 52.55 µs (+0.80) | 52.25 µs (+0.50) |
| cores = 1, 1,000 rows | INSERT, pk named | 49.40 µs | 49.20 µs (-0.20) | 49.10 µs (-0.30) |
| cores = 1, 1,000 rows | SELECT by pk | 66.95 µs | 67.60 µs (+0.65) | 67.20 µs (+0.25) |
| cores = 1, 1,000 rows | DELETE by pk | 48.80 µs | 49.10 µs (+0.30) | 49.15 µs (+0.35) |
| cores = 1, 1,000 rows | SHOW META (control) | 50.10 µs | 50.10 µs (+0.00) | 50.30 µs (+0.20) |
| cores = 1, 10,000 rows | UPDATE by pk | 52.55 µs | 52.50 µs (-0.05) | 52.55 µs (+0.00) |
| cores = 1, 10,000 rows | INSERT, pk named | 49.60 µs | 49.00 µs (-0.60) | 49.10 µs (-0.50) |
| cores = 1, 10,000 rows | SELECT by pk | 64.80 µs | 64.90 µs (+0.10) | 64.60 µs (-0.20) |
| cores = 1, 10,000 rows | DELETE by pk | 49.30 µs | 49.20 µs (-0.10) | 49.10 µs (-0.20) |
| cores = 1, 10,000 rows | SHOW META (control) | 50.25 µs | 50.30 µs (+0.05) | 50.30 µs (+0.05) |
| cores = 2, 1,000 rows | UPDATE by pk | 53.60 µs | 54.20 µs (+0.60) | 53.60 µs (+0.00) |
| cores = 2, 1,000 rows | INSERT, pk named | 50.05 µs | 49.85 µs (-0.20) | 49.85 µs (-0.20) |
| cores = 2, 1,000 rows | SELECT by pk | 68.05 µs | 69.05 µs (+1.00) | 68.60 µs (+0.55) |
| cores = 2, 1,000 rows | DELETE by pk | 49.30 µs | 49.60 µs (+0.30) | 49.50 µs (+0.20) |
| cores = 2, 1,000 rows | SHOW META (control) | 50.50 µs | 50.70 µs (+0.20) | 50.90 µs (+0.40) |

The point shapes' A against BJ's B' differ by -0.6 to +1.0 µs (UPDATE and SELECT read 0.3-1.0 µs dearer today at 1,000 rows,
INSERT and DELETE within 0.3): that is what a day on this host does to one binary. B reads -0.5 to +0.55 µs from BJ's B'. The BH
close (`4015736d`, `v2.7.0-712-g4015736d`) is 51 commits and 69 engine files before A (55 before B):

| cell | shape | BH close B' (712), median of runs | this A (763) | this B (767) |
|---|---|---|---|---|
| cores = 1, 200 rows | INSERT, pk omitted | 49.35 µs | 49.20 µs (-0.15) | 49.20 µs (-0.15) |
| cores = 1, 200 rows | INSERT again | 49.30 µs | 49.20 µs (-0.10) | 49.20 µs (-0.10) |
| cores = 1, 200 rows | SELECT by pk | 65.10 µs | 65.45 µs (+0.35) | 64.90 µs (-0.20) |
| cores = 1, 200 rows | UPDATE by pk | 51.25 µs | 52.30 µs (+1.05) | 51.90 µs (+0.65) |
| cores = 1, 200 rows | DELETE by pk | 49.25 µs | 49.10 µs (-0.15) | 49.10 µs (-0.15) |
| cores = 1, 200 rows | COUNT/MIN/MAX over 100 ids | 86.55 µs | 86.50 µs (-0.05) | 86.90 µs (+0.35) |
| cores = 1, 200 rows | SHOW META (control) | 45.40 µs | 50.40 µs (+5.00) | 50.30 µs (+4.90) |
| cores = 1, 1,000 rows | INSERT, pk omitted | 49.30 µs | 49.15 µs (-0.15) | 49.20 µs (-0.10) |
| cores = 1, 1,000 rows | INSERT again | 49.30 µs | 49.20 µs (-0.10) | 49.20 µs (-0.10) |
| cores = 1, 1,000 rows | SELECT by pk | 66.50 µs | 67.60 µs (+1.10) | 67.20 µs (+0.70) |
| cores = 1, 1,000 rows | UPDATE by pk | 51.10 µs | 52.55 µs (+1.45) | 52.25 µs (+1.15) |
| cores = 1, 1,000 rows | DELETE by pk | 49.30 µs | 49.10 µs (-0.20) | 49.15 µs (-0.15) |
| cores = 1, 1,000 rows | COUNT/MIN/MAX over 100 ids | 145.65 µs | 145.55 µs (-0.10) | 147.15 µs (+1.50) |
| cores = 1, 1,000 rows | SHOW META (control) | 45.30 µs | 50.10 µs (+4.80) | 50.30 µs (+5.00) |
| cores = 1, 10,000 rows | INSERT, pk omitted | 49.25 µs | 49.10 µs (-0.15) | 49.10 µs (-0.15) |
| cores = 1, 10,000 rows | INSERT again | 49.20 µs | 49.10 µs (-0.10) | 49.10 µs (-0.10) |
| cores = 1, 10,000 rows | SELECT by pk | 64.20 µs | 64.90 µs (+0.70) | 64.60 µs (+0.40) |
| cores = 1, 10,000 rows | UPDATE by pk | 51.55 µs | 52.50 µs (+0.95) | 52.55 µs (+1.00) |
| cores = 1, 10,000 rows | DELETE by pk | 49.30 µs | 49.20 µs (-0.10) | 49.10 µs (-0.20) |
| cores = 1, 10,000 rows | COUNT/MIN/MAX over 100 ids | 802.75 µs | 805.20 µs (+2.45) | 819.30 µs (+16.55) |
| cores = 1, 10,000 rows | SHOW META (control) | 45.40 µs | 50.30 µs (+4.90) | 50.30 µs (+4.90) |
| cores = 2, 200 rows | INSERT, pk omitted | 49.10 µs | 49.60 µs (+0.50) | 49.40 µs (+0.30) |
| cores = 2, 200 rows | INSERT again | 49.15 µs | 49.50 µs (+0.35) | 49.30 µs (+0.15) |
| cores = 2, 200 rows | SELECT by pk | 66.05 µs | 66.80 µs (+0.75) | 66.20 µs (+0.15) |
| cores = 2, 200 rows | UPDATE by pk | 52.10 µs | 53.85 µs (+1.75) | 53.60 µs (+1.50) |
| cores = 2, 200 rows | DELETE by pk | 49.20 µs | 49.55 µs (+0.35) | 49.50 µs (+0.30) |
| cores = 2, 200 rows | COUNT/MIN/MAX over 100 ids | 87.20 µs | 88.00 µs (+0.80) | 88.10 µs (+0.90) |
| cores = 2, 200 rows | SHOW META (control) | 44.45 µs | 50.55 µs (+6.10) | 50.60 µs (+6.15) |
| cores = 2, 1,000 rows | INSERT, pk omitted | 49.00 µs | 49.60 µs (+0.60) | 49.50 µs (+0.50) |
| cores = 2, 1,000 rows | INSERT again | 49.10 µs | 49.50 µs (+0.40) | 49.60 µs (+0.50) |
| cores = 2, 1,000 rows | SELECT by pk | 67.70 µs | 69.05 µs (+1.35) | 68.60 µs (+0.90) |
| cores = 2, 1,000 rows | UPDATE by pk | 52.00 µs | 54.20 µs (+2.20) | 53.60 µs (+1.60) |
| cores = 2, 1,000 rows | DELETE by pk | 49.15 µs | 49.60 µs (+0.45) | 49.50 µs (+0.35) |
| cores = 2, 1,000 rows | COUNT/MIN/MAX over 100 ids | 147.50 µs | 148.05 µs (+0.55) | 149.45 µs (+1.95) |
| cores = 2, 1,000 rows | SHOW META (control) | 45.30 µs | 50.70 µs (+5.40) | 50.90 µs (+5.60) |
| cores = 2, 10,000 rows | INSERT, pk omitted | 49.05 µs | 49.30 µs (+0.25) | 49.15 µs (+0.10) |
| cores = 2, 10,000 rows | INSERT again | 49.10 µs | 49.40 µs (+0.30) | 49.20 µs (+0.10) |
| cores = 2, 10,000 rows | SELECT by pk | 65.35 µs | 65.85 µs (+0.50) | 65.25 µs (-0.10) |
| cores = 2, 10,000 rows | UPDATE by pk | 52.55 µs | 54.25 µs (+1.70) | 53.80 µs (+1.25) |
| cores = 2, 10,000 rows | DELETE by pk | 49.20 µs | 49.45 µs (+0.25) | 49.40 µs (+0.20) |
| cores = 2, 10,000 rows | COUNT/MIN/MAX over 100 ids | 816.45 µs | 813.55 µs (-2.90) | 830.55 µs (+14.10) |
| cores = 2, 10,000 rows | SHOW META (control) | 45.45 µs | 50.45 µs (+5.00) | 50.75 µs (+5.30) |

Three things in it are not BK's, because A already has them: `SHOW META` is **+4.8 to +6.1 µs** in A (45.4 → 50.3 µs: its reply
or its path grew between 712 and BJ's close, which measured 50.2 µs), UPDATE by pk is +0.95 to +2.20 µs in A (BJ's literal path,
which its own close priced at +0.25 to +0.70 µs, and the day's drift), and **`range100` in A equals the BH close's to within
±3 µs at every size and core count** (cores = 1: 86.5 / 145.6 / 805.2 µs against 86.6 / 145.7 / 802.8 µs) - so what moved the
walk is BK: B is +0.35, +1.50 and +16.55 µs above BH's B' at `cores = 1` and +0.90, +1.95 and +14.10 µs at `cores = 2`, the
sizes of the early-day passes in §3.

## 7. What the run teaches about the engine

- **The width of the Keystone id is a constant, and the engine behaves as if it were one.** An INSERT, an UPDATE, a DELETE and a
  point read cost the same at 48 bits as at 40, with the key in the lower or the upper eight bits, at every size and in both core
  counts; the order's survey (§1.7) finds no page arithmetic that reads the id's width, and the run finds no cost where it would
  show. The sentence BK-S3 kept, restated as overturned, in `docs/rules/keystoneid-invariant.md` - that widening "forfeits the fixed
  64-bit super-column word (40+8+16) that the tuple header, meta-pool handle, and page arithmetic are built on" - was a claim about
  what the word's layout costs; on the write and point-read paths this run measures **it costs nothing**.
- **A scan resolves where the binary's pages sit before it resolves a change of 1-2 %.** The walk is the one shape that touches
  every row, and the same B bytes in two files differ on it by 2.1 % while two files of A differ by 0.0 %. BK-S2's cut removed a
  call from the per-row decode and the walk did not get faster; whatever moves it by 1.2 ns a row is not a call count, and
  at 10,000 rows it is smaller than the file-to-file spread of B itself. **`bench/README.md` rule 3 ("measure a copy of the
  binary") is not enough for a scan shape: a second copy of the same bytes belongs beside every pair as the null, made at the
  same time as the others, because a binary's place in the page cache is a variable of the measurement.**
- **Strict hides everything BK could cost on a write**: the commit wait is 95 % of an INSERT's p50 at every size, so a
  per-statement cost of 0.1 µs is invisible in a cell that spends 950 µs waiting for the device; whatever BK costs is a relaxed-mode
  and read-path fact.
- **The `cores = 2` cell is not what the drivers' "server on CPUs 2,3" says it is**: with `reactor_cpus` unset the engine pins
  core k to CPU k, so the second reactor sits on CPU 1, outside `taskset`'s mask. Earlier closes' pinned-CPU statements for
  `cores = 2` (BH, BJ) describe the mask, not the placement.

## 8. What could not be resolved

- **The walk's mechanism** (§3): +1.2 to +1.8 % between fresh placements, present in every early-day and fresh pair, up to +5.3 %
  in B's file at its worst placement; B executes one call fewer per row, so the cause is not the call. Neither a stage build nor
  `perf` was run.
- **The sign of B's `cores = 2` UPDATE lead** (§1: -0.65 to -0.85 µs, 1.2-1.6 %): resolved against the `cores = 2` null pair in three
  cells, the size of the 0.8 µs by which two files of B differ on SELECT at `cores = 1`; no `cores = 2` copy control or fresh pair was
  run.
- **The slots-exchanged pair at 200 rows and at `cores = 2`**, and the fresh pair at `cores = 2` and at 200 rows: not run.
- **Means.** INSERT and the other relaxed writes carry a 0.2-0.45 s carve stall in one arm per run (BA's record), so means and
  the driver's `ops / elapsed` throughput (kept in the appendix tables) move by tens of µs between arms with no p50 change; the
  matrices use p50.
- **The other sessions' load.** Every number here was measured beside builds and test suites that the discard rules and the
  quiet gate keep off the pinned CPUs and the runs' p50 controls, not off the L3 and the memory bus, and not off the page cache
  that places the binary.
- **Cross-day comparison** (§6) reads one binary at two times; its drift is the floor of that comparison.

## Baseline for the next run

The next measurement of these shapes reads against **`v2.7.0-767-gefa6adba` (this file)**: at `cores = 1`, 1,000 rows, B p50 (and
server CPU per statement): INSERT pk omitted 49.2 µs (28.5), INSERT pk named 49.1 µs (28.7; with keys `2^40 + i`: 49.3 µs), SELECT
by pk 67.2 µs (35.4), UPDATE by pk 52.2 µs (30.6), hot-row UPDATE 51.8 µs (30.4), DELETE by pk 49.2 µs (28.1), `COUNT/MIN/MAX`
over 100 ids 147.1 µs (114.4), `SHOW META` 50.3 µs (32.9); at 10,000 rows the walk reads 798 µs in A and 813 µs in B between
fresh copies, and 817-821 µs in B's first file. **A walk measurement of the next commit needs fresh copies of both sides and a copy
of one of them as the null.**

## Appendix A. Every cell, relaxed, `cores = 1` (A and B pooled over the 32 kept runs per size)

### cores = 1, 200 rows

32 runs kept of 32 (runs 0-31); loadavg(1 min) before each run 0.82-2.01, after 0.89-2.09; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-3.2% (median 2.2%); CPUs 6,7 (another session's benchmark) busy over 10 % in 8 of the kept runs (range 1%-61%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,503 us, B 7,966 us, max A 341,400 us, B 320,648 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 96,000 | 39.9 µs | 48.3 µs | 49.2 µs | 50.5 µs | 64.0 µs | 79.6 µs | 51.9 µs (sd 3.5) | 19,271 /s | 28.50 µs |
| INSERT, pk omitted | B | 96,000 | 40.4 µs | 48.2 µs | 49.2 µs | 50.3 µs | 63.0 µs | 77.6 µs | 54.1 µs (sd 17.4) | 19,372 /s | 28.27 µs |
| INSERT again (replicate) | A | 96,000 | 40.6 µs | 48.2 µs | 49.1 µs | 50.3 µs | 63.7 µs | 78.7 µs | 52.1 µs (sd 2.0) | 19,156 /s | 28.50 µs |
| INSERT again (replicate) | B | 96,000 | 37.5 µs | 48.2 µs | 49.1 µs | 50.2 µs | 63.6 µs | 78.9 µs | 52.3 µs (sd 2.4) | 19,074 /s | 28.31 µs |
| INSERT, pk named (keys below 2^40) | A | 96,000 | 40.3 µs | 48.5 µs | 49.2 µs | 50.7 µs | 64.7 µs | 79.4 µs | 51.9 µs (sd 0.7) | 18,900 /s | 28.71 µs |
| INSERT, pk named (keys below 2^40) | B | 96,000 | 39.6 µs | 48.3 µs | 49.1 µs | 50.5 µs | 64.1 µs | 77.9 µs | 51.8 µs (sd 0.9) | 19,027 /s | 28.47 µs |
| SELECT by pk | A | 96,000 | 57.6 µs | 64.6 µs | 65.5 µs | 69.7 µs | 82.6 µs | 99.0 µs | 68.8 µs (sd 1.0) | 14,442 /s | 33.85 µs |
| SELECT by pk | B | 96,000 | 58.2 µs | 64.1 µs | 65.2 µs | 69.2 µs | 81.8 µs | 97.3 µs | 68.4 µs (sd 0.9) | 14,534 /s | 33.29 µs |
| UPDATE by pk | A | 96,000 | 41.6 µs | 51.6 µs | 52.3 µs | 54.1 µs | 67.1 µs | 80.9 µs | 55.1 µs (sd 0.7) | 17,938 /s | 30.85 µs |
| UPDATE by pk | B | 96,000 | 40.4 µs | 51.3 µs | 52.0 µs | 53.8 µs | 66.7 µs | 79.8 µs | 54.8 µs (sd 1.1) | 18,122 /s | 30.42 µs |
| UPDATE of one hot row | A | 96,000 | 42.7 µs | 51.4 µs | 52.1 µs | 54.2 µs | 67.0 µs | 80.6 µs | 54.4 µs (sd 0.8) | 18,275 /s | 30.74 µs |
| UPDATE of one hot row | B | 96,000 | 39.7 µs | 51.0 µs | 51.7 µs | 53.6 µs | 66.5 µs | 79.6 µs | 54.8 µs (sd 1.5) | 18,204 /s | 30.21 µs |
| DELETE by pk | A | 96,000 | 40.4 µs | 48.3 µs | 49.1 µs | 50.3 µs | 62.6 µs | 73.3 µs | 51.1 µs (sd 0.8) | 19,487 /s | 28.04 µs |
| DELETE by pk | B | 96,000 | 38.9 µs | 48.2 µs | 49.1 µs | 50.2 µs | 62.4 µs | 72.4 µs | 50.9 µs (sd 0.6) | 19,505 /s | 27.99 µs |
| COUNT/MIN/MAX over 100 ids | A | 96,000 | 78.9 µs | 85.5 µs | 86.5 µs | 89.7 µs | 104.1 µs | 119.5 µs | 89.8 µs (sd 1.0) | 11,038 /s | 53.76 µs |
| COUNT/MIN/MAX over 100 ids | B | 96,000 | 80.7 µs | 85.9 µs | 87.0 µs | 90.0 µs | 103.9 µs | 117.5 µs | 90.0 µs (sd 0.9) | 11,017 /s | 54.03 µs |
| SHOW META (control) | A | 96,000 | 42.4 µs | 49.7 µs | 50.4 µs | 52.8 µs | 62.3 µs | 70.1 µs | 52.5 µs (sd 1.5) | 19,067 /s | 33.12 µs |
| SHOW META (control) | B | 96,000 | 42.3 µs | 49.7 µs | 50.4 µs | 52.8 µs | 62.0 µs | 69.7 µs | 52.2 µs (sd 0.5) | 19,079 /s | 33.06 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 96,000 | 40.1 µs | 48.3 µs | 49.1 µs | 50.4 µs | 63.6 µs | 74.9 µs | 51.7 µs (sd 1.3) | 19,102 /s | 28.36 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 96,000 | 40.5 µs | 48.5 µs | 49.2 µs | 50.5 µs | 63.8 µs | 74.3 µs | 51.8 µs (sd 0.8) | 19,016 /s | 28.47 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,325 → 20,325 /s (+0.0 % [-0.2, +0.3]) | +0.00 µs [-0.15, +0.10] | +2.17 µs [-2.12, +9.01] | -0.23 µs [-0.30, -0.12] | +0.5 % [+0.2, +0.9] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.2 µs | 20,325 → 20,325 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +0.12 µs [-0.70, +1.08] | -0.08 µs [-0.20, -0.02] | +0.3 % [-1.0, +1.5] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.0 µs | 20,325 → 20,408 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.18 µs [-0.55, +0.22] | -0.21 µs [-0.36, -0.09] | +0.7 % [-0.7, +1.5] | 0.89 µs | no change resolved |
| SELECT by pk | 65.5 µs | 64.9 µs | 15,279 → 15,408 /s (+0.7 % [+0.0, +0.9]) | -0.45 µs [-0.60, +0.00] | -0.43 µs [-0.72, -0.16] | -0.46 µs [-0.71, -0.11] | +0.3 % [+0.1, +1.3] | 0.89 µs | no change resolved |
| UPDATE by pk | 52.3 µs | 51.9 µs | 19,120 → 19,268 /s (+0.8 % [+0.6, +1.3]) | -0.40 µs [-0.70, -0.30] | -0.34 µs [-0.70, +0.06] | -0.38 µs [-0.66, -0.24] | +0.8 % [+0.3, +1.4] | 0.89 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 52.1 µs | 51.7 µs | 19,194 → 19,361 /s (+0.6 % [+0.4, +1.4]) | -0.30 µs [-0.70, -0.20] | +0.41 µs [-0.07, +0.92] | -0.41 µs [-0.60, -0.20] | -0.6 % [-1.6, +0.4] | 0.89 µs | CI excludes 0, inside the floor: not resolved |
| DELETE by pk | 49.1 µs | 49.1 µs | 20,367 → 20,367 /s (+0.2 % [-0.2, +0.4]) | -0.10 µs [-0.20, +0.05] | -0.17 µs [-0.45, +0.10] | +0.01 µs [-0.15, +0.11] | +0.2 % [-0.4, +0.7] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 86.5 µs | 86.9 µs | 11,561 → 11,507 /s (-0.7 % [-1.0, -0.3]) | +0.65 µs [+0.30, +0.90] | +0.12 µs [-0.32, +0.53] | +0.35 µs [-0.01, +0.86] | -0.1 % [-0.9, +0.2] | 0.89 µs | CI excludes 0, inside the floor: not resolved |
| SHOW META (control) | 50.4 µs | 50.3 µs | 19,841 → 19,881 /s (-0.1 % [-0.2, +0.1]) | +0.05 µs [-0.05, +0.10] | -0.27 µs [-0.89, +0.13] | -0.06 µs [-0.32, +0.05] | -0.2 % [-0.5, +0.2] | 0.89 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.10], Δ mean +0.09 µs [-0.57, +0.68], Δ server CPU per statement +0.12 µs [+0.00, +0.17]; lo p50 49.1 µs, hi p50 49.2 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.00 µs [-0.05, +0.00].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.29 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.22 µs, sd 0.35 µs; control SHOW META Δ p50 +0.05 µs [-0.05, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.32 µs, UPDATE 0.49 µs, SELECT 0.55 µs, ping 0.40 µs.

### cores = 1, 1,000 rows

32 runs kept of 32 (runs 0-31); loadavg(1 min) before each run 0.74-2.65, after 0.84-2.76; busy of the other CPUs (6, 7 excluded) in kept runs 1.5%-4.2% (median 2.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 5 of the kept runs (range 1%-60%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,813 us, B 7,548 us, max A 242,612 us, B 140,789 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 96,000 | 36.5 µs | 48.2 µs | 49.1 µs | 50.5 µs | 64.2 µs | 82.0 µs | 60.9 µs (sd 29.4) | 19,249 /s | 28.51 µs |
| INSERT, pk omitted | B | 96,000 | 38.9 µs | 48.2 µs | 49.2 µs | 50.4 µs | 63.8 µs | 79.1 µs | 57.6 µs (sd 25.2) | 19,344 /s | 28.48 µs |
| INSERT again (replicate) | A | 96,000 | 40.8 µs | 48.2 µs | 49.1 µs | 50.5 µs | 64.1 µs | 80.0 µs | 54.8 µs (sd 12.8) | 19,105 /s | 28.55 µs |
| INSERT again (replicate) | B | 96,000 | 39.7 µs | 48.2 µs | 49.2 µs | 50.4 µs | 63.6 µs | 78.8 µs | 52.3 µs (sd 2.3) | 19,089 /s | 28.46 µs |
| INSERT, pk named (keys below 2^40) | A | 96,000 | 38.3 µs | 48.4 µs | 49.1 µs | 50.7 µs | 64.7 µs | 80.6 µs | 52.3 µs (sd 0.8) | 18,872 /s | 28.81 µs |
| INSERT, pk named (keys below 2^40) | B | 96,000 | 37.9 µs | 48.3 µs | 49.2 µs | 50.6 µs | 64.4 µs | 79.4 µs | 52.4 µs (sd 1.7) | 18,820 /s | 28.66 µs |
| SELECT by pk | A | 96,000 | 58.6 µs | 65.0 µs | 67.7 µs | 73.4 µs | 87.3 µs | 102.9 µs | 70.9 µs (sd 0.7) | 13,983 /s | 35.74 µs |
| SELECT by pk | B | 96,000 | 58.0 µs | 64.6 µs | 67.2 µs | 72.8 µs | 86.2 µs | 100.8 µs | 70.2 µs (sd 0.6) | 14,119 /s | 35.37 µs |
| UPDATE by pk | A | 96,000 | 43.2 µs | 51.7 µs | 52.5 µs | 54.7 µs | 67.5 µs | 81.3 µs | 55.4 µs (sd 0.9) | 17,937 /s | 31.00 µs |
| UPDATE by pk | B | 96,000 | 40.3 µs | 51.5 µs | 52.3 µs | 54.3 µs | 67.1 µs | 80.7 µs | 55.4 µs (sd 1.4) | 17,934 /s | 30.62 µs |
| UPDATE of one hot row | A | 96,000 | 40.8 µs | 51.4 µs | 52.2 µs | 54.3 µs | 67.4 µs | 83.6 µs | 54.7 µs (sd 2.2) | 18,303 /s | 30.74 µs |
| UPDATE of one hot row | B | 96,000 | 39.2 µs | 51.2 µs | 51.9 µs | 54.0 µs | 67.6 µs | 84.0 µs | 55.4 µs (sd 3.0) | 18,162 /s | 30.44 µs |
| DELETE by pk | A | 96,000 | 40.1 µs | 48.3 µs | 49.1 µs | 50.3 µs | 62.8 µs | 73.8 µs | 51.1 µs (sd 0.8) | 19,491 /s | 28.11 µs |
| DELETE by pk | B | 96,000 | 39.5 µs | 48.3 µs | 49.2 µs | 50.3 µs | 62.7 µs | 73.3 µs | 51.2 µs (sd 1.0) | 19,561 /s | 28.05 µs |
| COUNT/MIN/MAX over 100 ids | A | 96,000 | 135.6 µs | 142.7 µs | 145.9 µs | 153.5 µs | 169.8 µs | 206.0 µs | 152.1 µs (sd 6.9) | 6,633 /s | 112.61 µs |
| COUNT/MIN/MAX over 100 ids | B | 96,000 | 136.5 µs | 144.3 µs | 147.5 µs | 155.6 µs | 173.6 µs | 202.2 µs | 153.1 µs (sd 6.3) | 6,570 /s | 114.36 µs |
| SHOW META (control) | A | 96,000 | 43.1 µs | 49.5 µs | 50.2 µs | 52.7 µs | 62.1 µs | 70.5 µs | 52.1 µs (sd 0.5) | 19,097 /s | 32.86 µs |
| SHOW META (control) | B | 96,000 | 43.0 µs | 49.6 µs | 50.3 µs | 53.0 µs | 62.1 µs | 70.2 µs | 52.6 µs (sd 2.2) | 19,013 /s | 32.91 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 96,000 | 40.3 µs | 48.3 µs | 49.1 µs | 50.4 µs | 63.2 µs | 74.2 µs | 51.6 µs (sd 1.4) | 19,212 /s | 28.42 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 96,000 | 39.6 µs | 48.5 µs | 49.2 µs | 50.6 µs | 63.7 µs | 74.8 µs | 52.0 µs (sd 1.1) | 18,981 /s | 28.54 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,346 → 20,325 /s (+0.0 % [-0.2, +0.1]) | +0.00 µs [-0.05, +0.10] | -3.38 µs [-16.19, +9.44] | -0.10 µs [-0.15, +0.03] | +0.5 % [+0.0, +0.6] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.2 µs | 20,325 → 20,325 /s (+0.0 % [-0.2, +0.0]) | +0.00 µs [+0.00, +0.10] | -2.50 µs [-7.15, +0.46] | -0.08 µs [-0.20, +0.15] | +0.1 % [-1.3, +1.5] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.0 % [-0.1, +0.1]) | +0.00 µs [-0.05, +0.05] | +0.18 µs [-0.43, +0.85] | -0.06 µs [-0.22, +0.05] | +0.8 % [-1.2, +1.8] | 0.67 µs | no change resolved |
| SELECT by pk | 67.6 µs | 67.2 µs | 14,793 → 14,881 /s (+0.7 % [+0.3, +1.2]) | -0.50 µs [-0.75, -0.20] | -0.66 µs [-0.93, -0.40] | -0.49 µs [-0.68, -0.36] | +0.8 % [+0.6, +1.3] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 52.5 µs | 52.2 µs | 19,029 → 19,139 /s (+0.8 % [+0.0, +1.0]) | -0.40 µs [-0.50, +0.00] | +0.00 µs [-0.39, +0.40] | -0.30 µs [-0.51, -0.13] | -0.0 % [-0.9, +0.7] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 51.8 µs | 19,212 → 19,286 /s (+0.5 % [+0.0, +0.8]) | -0.25 µs [-0.40, +0.00] | +0.65 µs [+0.14, +1.24] | -0.26 µs [-0.49, +0.18] | -0.9 % [-1.6, +0.3] | 0.67 µs | no change resolved |
| DELETE by pk | 49.1 µs | 49.2 µs | 20,367 → 20,346 /s (+0.0 % [-0.2, +0.1]) | +0.00 µs [-0.05, +0.10] | +0.06 µs [-0.26, +0.42] | +0.05 µs [-0.23, +0.20] | +0.4 % [-0.7, +0.9] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 145.6 µs | 147.1 µs | 6,870 → 6,796 /s (-1.1 % [-1.3, -1.0]) | +1.65 µs [+1.35, +1.90] | +0.96 µs [-1.45, +3.32] | +1.52 µs [+1.23, +1.90] | -1.0 % [-1.5, -0.6] | 0.75 µs | **B slower** (+1.1 % at p50) |
| SHOW META (control) | 50.1 µs | 50.3 µs | 19,960 → 19,881 /s (-0.2 % [-0.4, +0.0]) | +0.10 µs [+0.00, +0.20] | +0.53 µs [+0.01, +1.38] | +0.13 µs [-0.05, +0.33] | -0.3 % [-0.8, +0.2] | 0.67 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.10], Δ mean +0.44 µs [-0.27, +1.02], Δ server CPU per statement +0.09 µs [+0.03, +0.15]; lo p50 49.2 µs, hi p50 49.3 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.10 µs [+0.00, +0.20].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.22 µs, sd 0.25 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.20 µs; control SHOW META Δ p50 +0.10 µs [+0.00, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.21 µs, UPDATE 0.66 µs, SELECT 0.75 µs, ping 0.33 µs.

### cores = 1, 10,000 rows

32 runs kept of 32 (runs 0-31); loadavg(1 min) before each run 0.96-1.72, after 1.14-1.91; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-4.1% (median 2.0%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-7%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,102 us, B 9,600 us, max A 48,940 us, B 22,580 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 96,000 | 38.8 µs | 48.2 µs | 49.1 µs | 50.4 µs | 62.7 µs | 77.6 µs | 89.7 µs (sd 53.8) | 19,061 /s | 28.26 µs |
| INSERT, pk omitted | B | 96,000 | 34.9 µs | 48.2 µs | 49.1 µs | 50.2 µs | 62.2 µs | 75.2 µs | 91.3 µs (sd 55.3) | 19,144 /s | 28.66 µs |
| INSERT again (replicate) | A | 96,000 | 41.3 µs | 48.2 µs | 49.1 µs | 50.3 µs | 64.0 µs | 80.1 µs | 63.4 µs (sd 33.2) | 19,266 /s | 28.36 µs |
| INSERT again (replicate) | B | 96,000 | 40.6 µs | 48.2 µs | 49.1 µs | 50.3 µs | 63.5 µs | 79.1 µs | 79.3 µs (sd 47.6) | 19,169 /s | 28.46 µs |
| INSERT, pk named (keys below 2^40) | A | 96,000 | 40.9 µs | 48.3 µs | 49.1 µs | 50.6 µs | 64.5 µs | 80.9 µs | 52.1 µs (sd 1.6) | 18,990 /s | 28.60 µs |
| INSERT, pk named (keys below 2^40) | B | 96,000 | 39.9 µs | 48.3 µs | 49.1 µs | 50.7 µs | 64.9 µs | 81.1 µs | 52.5 µs (sd 1.5) | 18,902 /s | 28.73 µs |
| SELECT by pk | A | 96,000 | 58.6 µs | 64.1 µs | 65.0 µs | 67.2 µs | 80.1 µs | 93.0 µs | 67.5 µs (sd 0.8) | 14,708 /s | 32.39 µs |
| SELECT by pk | B | 96,000 | 57.7 µs | 63.8 µs | 64.6 µs | 66.6 µs | 79.1 µs | 90.0 µs | 66.9 µs (sd 0.9) | 14,835 /s | 31.97 µs |
| UPDATE by pk | A | 96,000 | 43.6 µs | 51.8 µs | 52.7 µs | 54.6 µs | 67.4 µs | 82.6 µs | 55.9 µs (sd 1.4) | 17,601 /s | 31.30 µs |
| UPDATE by pk | B | 96,000 | 40.1 µs | 51.7 µs | 52.6 µs | 54.6 µs | 67.5 µs | 81.2 µs | 55.6 µs (sd 1.0) | 17,849 /s | 31.12 µs |
| UPDATE of one hot row | A | 96,000 | 39.6 µs | 51.3 µs | 52.0 µs | 53.8 µs | 66.8 µs | 80.9 µs | 54.3 µs (sd 0.8) | 18,310 /s | 30.70 µs |
| UPDATE of one hot row | B | 96,000 | 39.9 µs | 51.2 µs | 51.9 µs | 53.7 µs | 66.6 µs | 81.1 µs | 54.9 µs (sd 1.2) | 18,213 /s | 30.58 µs |
| DELETE by pk | A | 96,000 | 39.5 µs | 48.3 µs | 49.2 µs | 50.3 µs | 62.6 µs | 73.1 µs | 51.4 µs (sd 0.9) | 19,420 /s | 28.05 µs |
| DELETE by pk | B | 96,000 | 40.0 µs | 48.3 µs | 49.1 µs | 50.3 µs | 62.7 µs | 73.1 µs | 52.0 µs (sd 4.0) | 19,430 /s | 28.07 µs |
| COUNT/MIN/MAX over 100 ids | A | 96,000 | 725.8 µs | 781.4 µs | 806.5 µs | 830.0 µs | 867.3 µs | 1,115.1 µs | 825.2 µs (sd 13.3) | 1,215 /s | 765.53 µs |
| COUNT/MIN/MAX over 100 ids | B | 96,000 | 735.9 µs | 797.6 µs | 822.5 µs | 847.7 µs | 905.2 µs | 1,090.2 µs | 842.7 µs (sd 20.6) | 1,190 /s | 780.38 µs |
| SHOW META (control) | A | 96,000 | 42.1 µs | 49.6 µs | 50.3 µs | 52.9 µs | 62.6 µs | 71.9 µs | 52.3 µs (sd 0.6) | 19,073 /s | 33.00 µs |
| SHOW META (control) | B | 96,000 | 43.8 µs | 49.6 µs | 50.3 µs | 52.9 µs | 62.0 µs | 70.2 µs | 52.1 µs (sd 0.4) | 19,116 /s | 32.97 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 96,000 | 39.5 µs | 48.3 µs | 49.1 µs | 50.5 µs | 63.7 µs | 75.7 µs | 52.0 µs (sd 1.3) | 19,021 /s | 28.50 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 96,000 | 39.5 µs | 48.5 µs | 49.2 µs | 50.6 µs | 64.0 µs | 76.1 µs | 52.1 µs (sd 1.5) | 19,035 /s | 28.61 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.1 µs | 49.1 µs | 20,367 → 20,367 /s (+0.1 % [+0.0, +0.2]) | -0.05 µs [-0.10, +0.00] | +1.55 µs [-28.74, +32.97] | +0.36 µs [-0.26, +0.75] | +1.1 % [-22.7, +3.4] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.1 µs | 49.1 µs | 20,367 → 20,367 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +15.91 µs [-5.10, +37.39] | +0.06 µs [-0.15, +0.18] | -0.2 % [-23.4, +0.3] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.0 µs | 49.1 µs | 20,408 → 20,367 /s (+0.0 % [-0.2, +0.0]) | +0.00 µs [+0.00, +0.10] | +0.39 µs [-0.10, +0.82] | +0.04 µs [-0.04, +0.16] | -1.3 % [-2.0, -0.0] | 0.60 µs | no change resolved |
| SELECT by pk | 64.9 µs | 64.6 µs | 15,408 → 15,480 /s (+0.4 % [+0.0, +1.1]) | -0.25 µs [-0.70, +0.00] | -0.52 µs [-0.76, -0.26] | -0.39 µs [-0.73, -0.01] | +0.8 % [+0.3, +1.3] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.5 µs | 52.5 µs | 19,048 → 19,029 /s (+0.3 % [-0.5, +0.7]) | -0.15 µs [-0.40, +0.30] | -0.37 µs [-0.91, +0.20] | +0.04 µs [-0.40, +0.32] | +0.5 % [-0.9, +2.3] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 51.9 µs | 51.9 µs | 19,268 → 19,268 /s (+0.2 % [-0.4, +0.8]) | -0.10 µs [-0.40, +0.20] | +0.58 µs [+0.18, +1.04] | -0.17 µs [-0.34, +0.25] | -0.9 % [-1.4, +0.2] | 0.60 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.15, +0.10] | +0.66 µs [-0.27, +2.21] | +0.13 µs [-0.04, +0.26] | -0.1 % [-0.7, +0.3] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 805.2 µs | 819.3 µs | 1,242 → 1,221 /s (-1.8 % [-2.0, -1.4]) | +14.35 µs [+11.60, +16.50] | +17.49 µs [+10.27, +25.53] | +14.80 µs [+11.02, +17.39] | -1.6 % [-2.5, -1.2] | 5.65 µs | **B slower** (+1.8 % at p50) |
| SHOW META (control) | 50.3 µs | 50.3 µs | 19,881 → 19,881 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | -0.20 µs [-0.43, +0.02] | -0.05 µs [-0.23, +0.09] | +0.2 % [-0.3, +0.9] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.10, +0.15], Δ mean +0.09 µs [-0.47, +0.68], Δ server CPU per statement +0.12 µs [+0.04, +0.17]; lo p50 49.1 µs, hi p50 49.2 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.20 µs [+0.00, +0.30].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.36 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.30 µs; control SHOW META Δ p50 +0.00 µs [-0.10, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.25 µs, UPDATE 0.65 µs, SELECT 0.60 µs, ping 0.35 µs.

## Appendix B. Every cell, relaxed, `cores = 2` (16 kept runs per size)

### cores = 2, 200 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 1.02-2.38, after 1.02-2.43; busy of the other CPUs (6, 7 excluded) in kept runs 1.7%-3.6% (median 2.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 5 of the kept runs (range 2%-56%); gate wait median 3 s; ping p50 reference 50.6 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 6,227 us, B 8,140 us, max A 14,715 us, B 15,278 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 43.4 µs | 48.9 µs | 49.6 µs | 51.5 µs | 67.4 µs | 77.9 µs | 52.3 µs (sd 0.7) | 18,938 /s | 29.59 µs |
| INSERT, pk omitted | B | 48,000 | 42.8 µs | 48.7 µs | 49.4 µs | 51.3 µs | 66.8 µs | 75.9 µs | 51.9 µs (sd 0.4) | 18,987 /s | 29.46 µs |
| INSERT again (replicate) | A | 48,000 | 43.3 µs | 48.8 µs | 49.5 µs | 51.4 µs | 67.1 µs | 78.0 µs | 53.0 µs (sd 0.8) | 18,621 /s | 29.63 µs |
| INSERT again (replicate) | B | 48,000 | 42.8 µs | 48.7 µs | 49.4 µs | 51.2 µs | 66.7 µs | 77.1 µs | 52.8 µs (sd 0.8) | 18,667 /s | 29.42 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 44.1 µs | 49.2 µs | 49.9 µs | 52.6 µs | 67.5 µs | 78.2 µs | 53.5 µs (sd 1.2) | 18,504 /s | 29.77 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 44.0 µs | 49.0 µs | 49.8 µs | 52.2 µs | 67.1 µs | 76.6 µs | 53.0 µs (sd 0.8) | 18,531 /s | 29.74 µs |
| SELECT by pk | A | 48,000 | 60.8 µs | 65.9 µs | 66.9 µs | 71.9 µs | 86.4 µs | 98.4 µs | 70.2 µs (sd 0.5) | 14,114 /s | 35.65 µs |
| SELECT by pk | B | 48,000 | 57.2 µs | 65.3 µs | 66.2 µs | 71.5 µs | 85.8 µs | 97.2 µs | 69.8 µs (sd 0.7) | 14,214 /s | 35.08 µs |
| UPDATE by pk | A | 48,000 | 44.7 µs | 53.0 µs | 53.9 µs | 56.1 µs | 71.1 µs | 80.7 µs | 56.8 µs (sd 0.9) | 17,503 /s | 32.56 µs |
| UPDATE by pk | B | 48,000 | 46.1 µs | 52.7 µs | 53.6 µs | 55.8 µs | 70.9 µs | 80.0 µs | 58.6 µs (sd 7.4) | 17,620 /s | 32.34 µs |
| UPDATE of one hot row | A | 48,000 | 46.8 µs | 52.6 µs | 53.6 µs | 56.0 µs | 70.5 µs | 80.5 µs | 56.1 µs (sd 0.5) | 17,705 /s | 32.40 µs |
| UPDATE of one hot row | B | 48,000 | 46.2 µs | 52.3 µs | 53.2 µs | 55.5 µs | 70.9 µs | 80.8 µs | 60.3 µs (sd 17.4) | 17,764 /s | 32.26 µs |
| DELETE by pk | A | 48,000 | 41.2 µs | 48.8 µs | 49.6 µs | 51.5 µs | 66.0 µs | 74.9 µs | 53.5 µs (sd 2.4) | 18,729 /s | 29.19 µs |
| DELETE by pk | B | 48,000 | 42.3 µs | 48.8 µs | 49.5 µs | 51.3 µs | 65.4 µs | 73.3 µs | 52.3 µs (sd 1.1) | 19,148 /s | 29.13 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 83.0 µs | 86.7 µs | 87.9 µs | 92.7 µs | 109.6 µs | 121.2 µs | 92.3 µs (sd 2.7) | 10,797 /s | 55.91 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 84.2 µs | 87.2 µs | 88.3 µs | 92.4 µs | 109.4 µs | 120.6 µs | 92.1 µs (sd 1.2) | 10,804 /s | 55.98 µs |
| SHOW META (control) | A | 48,000 | 41.6 µs | 49.9 µs | 50.6 µs | 53.2 µs | 64.7 µs | 72.6 µs | 52.7 µs (sd 0.5) | 18,889 /s | 33.58 µs |
| SHOW META (control) | B | 48,000 | 44.0 µs | 49.9 µs | 50.6 µs | 53.4 µs | 64.8 µs | 72.6 µs | 52.8 µs (sd 0.5) | 18,835 /s | 33.49 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 42.9 µs | 48.9 µs | 49.6 µs | 51.7 µs | 66.2 µs | 73.7 µs | 52.7 µs (sd 0.8) | 18,790 /s | 29.56 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 48,000 | 40.8 µs | 49.1 µs | 49.8 µs | 52.0 µs | 66.5 µs | 74.5 µs | 52.5 µs (sd 0.8) | 18,788 /s | 29.80 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.6 µs | 49.4 µs | 20,161 → 20,243 /s (+0.3 % [-0.1, +0.8]) | -0.15 µs [-0.40, +0.05] | -0.33 µs [-0.75, +0.00] | -0.23 µs [-0.41, +0.01] | +0.4 % [-0.3, +1.2] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.5 µs | 49.3 µs | 20,202 → 20,284 /s (+0.1 % [-0.2, +0.6]) | -0.05 µs [-0.30, +0.10] | -0.22 µs [-0.79, +0.27] | -0.27 µs [-0.45, -0.05] | +0.5 % [-0.9, +1.9] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.9 µs | 49.8 µs | 20,040 → 20,080 /s (+0.2 % [+0.0, +0.7]) | -0.10 µs [-0.30, +0.00] | -0.44 µs [-1.13, +0.23] | -0.10 µs [-0.48, +0.04] | +0.8 % [-0.8, +2.1] | 0.60 µs | no change resolved |
| SELECT by pk | 66.8 µs | 66.2 µs | 14,970 → 15,106 /s (+0.9 % [+0.6, +1.4]) | -0.60 µs [-0.90, -0.40] | -0.42 µs [-0.76, -0.03] | -0.65 µs [-0.82, -0.31] | +0.9 % [+0.5, +1.2] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 53.8 µs | 53.6 µs | 18,570 → 18,657 /s (+0.6 % [-0.6, +1.5]) | -0.30 µs [-0.85, +0.30] | +1.84 µs [-0.53, +6.07] | -0.24 µs [-0.83, -0.04] | +0.7 % [-2.0, +1.7] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 53.5 µs | 53.1 µs | 18,692 → 18,832 /s (+0.6 % [-0.5, +2.1]) | -0.30 µs [-1.10, +0.25] | +4.19 µs [-0.65, +13.29] | -0.32 µs [-0.93, +0.20] | +0.7 % [-0.7, +1.5] | 0.60 µs | no change resolved |
| DELETE by pk | 49.5 µs | 49.5 µs | 20,182 → 20,202 /s (-0.1 % [-0.4, +0.8]) | +0.05 µs [-0.40, +0.20] | -1.23 µs [-2.68, +0.01] | -0.06 µs [-0.50, +0.26] | +1.6 % [-0.7, +3.5] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 88.0 µs | 88.1 µs | 11,364 → 11,351 /s (-0.3 % [-0.6, +0.0]) | +0.30 µs [+0.00, +0.50] | -0.21 µs [-1.37, +0.87] | +0.16 µs [-0.09, +0.31] | -0.0 % [-0.5, +0.7] | 0.60 µs | no change resolved |
| SHOW META (control) | 50.5 µs | 50.6 µs | 19,782 → 19,763 /s (-0.2 % [-0.6, +0.0]) | +0.10 µs [+0.00, +0.30] | +0.11 µs [-0.20, +0.42] | -0.04 µs [-0.26, +0.09] | -0.2 % [-0.6, +0.5] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.25 µs [+0.10, +0.35], Δ mean -0.14 µs [-0.73, +0.46], Δ server CPU per statement +0.16 µs [+0.13, +0.23]; lo p50 49.5 µs, hi p50 49.8 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.00 µs [-0.40, +0.20].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.20 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.18 µs; control SHOW META Δ p50 +0.10 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 0.30 µs, UPDATE 0.93 µs, SELECT 0.45 µs, ping 0.26 µs.

### cores = 2, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.95-2.21, after 1.04-2.41; busy of the other CPUs (6, 7 excluded) in kept runs 2.1%-5.6% (median 2.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 1 of the kept runs (range 1%-56%); gate wait median 3 s; ping p50 reference 50.8 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,495 us, B 6,738 us, max A 13,383 us, B 11,749 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 42.4 µs | 48.9 µs | 49.7 µs | 52.9 µs | 69.6 µs | 82.1 µs | 53.0 µs (sd 1.7) | 18,749 /s | 29.92 µs |
| INSERT, pk omitted | B | 48,000 | 42.7 µs | 48.8 µs | 49.6 µs | 51.5 µs | 67.6 µs | 79.1 µs | 52.7 µs (sd 1.4) | 18,861 /s | 29.68 µs |
| INSERT again (replicate) | A | 48,000 | 40.7 µs | 48.8 µs | 49.6 µs | 52.0 µs | 67.8 µs | 80.0 µs | 53.4 µs (sd 1.3) | 18,545 /s | 29.75 µs |
| INSERT again (replicate) | B | 48,000 | 42.4 µs | 48.8 µs | 49.6 µs | 51.7 µs | 67.2 µs | 79.2 µs | 53.3 µs (sd 0.8) | 18,499 /s | 29.80 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 40.7 µs | 49.2 µs | 49.9 µs | 52.1 µs | 67.5 µs | 77.4 µs | 53.5 µs (sd 0.9) | 18,416 /s | 29.84 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 43.2 µs | 49.2 µs | 49.9 µs | 52.1 µs | 67.3 µs | 76.5 µs | 53.7 µs (sd 1.0) | 18,340 /s | 29.91 µs |
| SELECT by pk | A | 48,000 | 60.7 µs | 66.3 µs | 69.2 µs | 75.0 µs | 90.3 µs | 103.6 µs | 72.5 µs (sd 0.6) | 13,679 /s | 37.57 µs |
| SELECT by pk | B | 48,000 | 59.8 µs | 65.8 µs | 68.7 µs | 74.5 µs | 89.6 µs | 102.8 µs | 71.9 µs (sd 0.7) | 13,804 /s | 37.14 µs |
| UPDATE by pk | A | 48,000 | 47.4 µs | 53.3 µs | 54.2 µs | 56.4 µs | 71.8 µs | 81.8 µs | 62.4 µs (sd 18.5) | 17,029 /s | 32.90 µs |
| UPDATE by pk | B | 48,000 | 44.8 µs | 52.7 µs | 53.7 µs | 55.8 µs | 71.0 µs | 80.5 µs | 57.6 µs (sd 1.3) | 17,191 /s | 32.28 µs |
| UPDATE of one hot row | A | 48,000 | 47.4 µs | 52.9 µs | 53.8 µs | 56.0 µs | 71.0 µs | 80.4 µs | 56.3 µs (sd 0.9) | 17,619 /s | 32.58 µs |
| UPDATE of one hot row | B | 48,000 | 42.9 µs | 52.3 µs | 53.2 µs | 55.2 µs | 70.0 µs | 79.5 µs | 62.1 µs (sd 22.0) | 17,548 /s | 32.00 µs |
| DELETE by pk | A | 48,000 | 43.1 µs | 49.0 µs | 49.7 µs | 51.6 µs | 66.3 µs | 74.9 µs | 52.7 µs (sd 1.2) | 19,000 /s | 29.17 µs |
| DELETE by pk | B | 48,000 | 41.6 µs | 48.9 µs | 49.6 µs | 51.4 µs | 65.2 µs | 73.9 µs | 51.9 µs (sd 0.7) | 19,213 /s | 28.91 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 137.2 µs | 144.6 µs | 148.1 µs | 157.8 µs | 175.1 µs | 201.7 µs | 153.0 µs (sd 1.7) | 6,526 /s | 116.55 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 138.9 µs | 146.2 µs | 149.7 µs | 158.8 µs | 177.1 µs | 201.3 µs | 154.6 µs (sd 1.4) | 6,440 /s | 118.34 µs |
| SHOW META (control) | A | 48,000 | 41.8 µs | 50.0 µs | 50.7 µs | 53.5 µs | 65.2 µs | 73.4 µs | 53.0 µs (sd 0.6) | 18,849 /s | 33.50 µs |
| SHOW META (control) | B | 48,000 | 44.4 µs | 50.2 µs | 51.0 µs | 53.8 µs | 65.3 µs | 74.3 µs | 53.3 µs (sd 0.6) | 18,694 /s | 34.05 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 43.5 µs | 49.0 µs | 49.8 µs | 52.1 µs | 67.1 µs | 76.0 µs | 52.7 µs (sd 0.9) | 18,676 /s | 29.68 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 48,000 | 41.3 µs | 49.1 µs | 49.9 µs | 52.1 µs | 66.9 µs | 75.4 µs | 52.9 µs (sd 0.7) | 18,625 /s | 29.79 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.6 µs | 49.5 µs | 20,161 → 20,202 /s (+0.2 % [-0.2, +0.8]) | -0.10 µs [-0.40, +0.10] | -0.37 µs [-1.33, +0.64] | -0.35 µs [-0.91, +0.05] | +0.7 % [-0.6, +1.4] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.5 µs | 49.6 µs | 20,202 → 20,161 /s (+0.0 % [-0.6, +0.4]) | +0.00 µs [-0.20, +0.30] | -0.11 µs [-1.01, +0.77] | -0.37 µs [-0.56, +0.12] | -0.8 % [-1.9, +3.3] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.8 µs | 49.8 µs | 20,060 → 20,060 /s (+0.0 % [-0.6, +0.6]) | +0.00 µs [-0.30, +0.30] | +0.24 µs [-0.30, +0.79] | +0.14 µs [-0.22, +0.28] | -0.3 % [-2.2, +0.8] | 0.89 µs | no change resolved |
| SELECT by pk | 69.1 µs | 68.6 µs | 14,482 → 14,577 /s (+1.1 % [-0.1, +1.5]) | -0.75 µs [-1.00, +0.10] | -0.59 µs [-0.91, -0.23] | -0.82 µs [-1.02, -0.20] | +1.1 % [+0.3, +1.3] | 0.89 µs | no change resolved |
| UPDATE by pk | 54.2 µs | 53.6 µs | 18,450 → 18,657 /s (+1.1 % [+0.2, +2.6]) | -0.60 µs [-1.40, +0.05] | -4.81 µs [-14.68, +0.52] | -0.48 µs [-1.21, +0.01] | +0.1 % [-1.3, +2.2] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 53.8 µs | 53.1 µs | 18,570 → 18,832 /s (+1.2 % [+0.0, +2.6]) | -0.65 µs [-1.40, +0.00] | +5.80 µs [-0.16, +17.04] | -0.59 µs [-1.08, +0.34] | -0.4 % [-2.6, +0.9] | 0.89 µs | no change resolved |
| DELETE by pk | 49.6 µs | 49.5 µs | 20,161 → 20,202 /s (+0.2 % [-0.3, +0.6]) | -0.10 µs [-0.30, +0.15] | -0.81 µs [-1.39, -0.26] | -0.33 µs [-0.73, +0.22] | +0.7 % [+0.2, +1.9] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 148.1 µs | 149.4 µs | 6,754 → 6,691 /s (-0.8 % [-1.2, -0.5]) | +1.20 µs [+0.70, +1.80] | +1.59 µs [+0.55, +2.59] | +1.56 µs [+0.44, +2.32] | -0.9 % [-1.6, -0.4] | 0.89 µs | **B slower** (+0.8 % at p50) |
| SHOW META (control) | 50.7 µs | 50.9 µs | 19,724 → 19,646 /s (-0.4 % [-0.8, +0.0]) | +0.20 µs [+0.00, +0.40] | +0.34 µs [+0.02, +0.69] | +0.36 µs [+0.20, +0.50] | -0.5 % [-1.1, +0.3] | 0.89 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.05, +0.20], Δ mean +0.27 µs [-0.31, +0.86], Δ server CPU per statement +0.08 µs [-0.06, +0.20]; lo p50 49.7 µs, hi p50 49.7 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -0.10 µs [-0.30, +0.20].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.54 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.26 µs; control SHOW META Δ p50 +0.20 µs [+0.00, +0.40]; run-to-run sd of the per-run Δ p50, INSERT 0.57 µs, UPDATE 0.93 µs, SELECT 1.00 µs, ping 0.30 µs.

### cores = 2, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 1.11-3.03, after 1.09-3.02; busy of the other CPUs (6, 7 excluded) in kept runs 1.7%-3.8% (median 3.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 13 of the kept runs (range 2%-59%); gate wait median 3 s; ping p50 reference 50.5 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 6,048 us, B 6,364 us, max A 20,546 us, B 21,428 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 39.4 µs | 48.6 µs | 49.3 µs | 51.4 µs | 67.5 µs | 82.1 µs | 96.2 µs (sd 58.5) | 16,119 /s | 29.64 µs |
| INSERT, pk omitted | B | 48,000 | 39.8 µs | 48.4 µs | 49.2 µs | 51.0 µs | 66.6 µs | 79.6 µs | 89.0 µs (sd 51.4) | 15,550 /s | 29.68 µs |
| INSERT again (replicate) | A | 48,000 | 38.3 µs | 48.7 µs | 49.4 µs | 51.8 µs | 67.9 µs | 81.3 µs | 67.4 µs (sd 41.4) | 18,777 /s | 29.96 µs |
| INSERT again (replicate) | B | 48,000 | 38.9 µs | 48.5 µs | 49.2 µs | 51.4 µs | 67.8 µs | 82.4 µs | 76.8 µs (sd 53.9) | 19,035 /s | 29.57 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 42.5 µs | 49.0 µs | 49.9 µs | 52.8 µs | 68.6 µs | 80.4 µs | 54.0 µs (sd 2.6) | 18,435 /s | 30.32 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 41.7 µs | 48.8 µs | 49.6 µs | 51.9 µs | 67.3 µs | 77.7 µs | 53.1 µs (sd 1.0) | 18,640 /s | 29.68 µs |
| SELECT by pk | A | 48,000 | 61.4 µs | 65.1 µs | 66.0 µs | 69.9 µs | 84.8 µs | 97.1 µs | 69.4 µs (sd 1.0) | 14,346 /s | 34.01 µs |
| SELECT by pk | B | 48,000 | 56.3 µs | 64.5 µs | 65.4 µs | 68.0 µs | 83.3 µs | 93.5 µs | 68.3 µs (sd 1.0) | 14,522 /s | 33.13 µs |
| UPDATE by pk | A | 48,000 | 46.4 µs | 53.5 µs | 54.4 µs | 57.4 µs | 72.7 µs | 85.8 µs | 58.1 µs (sd 1.2) | 17,075 /s | 33.42 µs |
| UPDATE by pk | B | 48,000 | 47.0 µs | 53.0 µs | 53.9 µs | 56.3 µs | 71.3 µs | 81.3 µs | 57.5 µs (sd 1.7) | 17,335 /s | 32.61 µs |
| UPDATE of one hot row | A | 48,000 | 47.5 µs | 52.8 µs | 53.7 µs | 56.3 µs | 70.8 µs | 80.7 µs | 56.3 µs (sd 0.8) | 17,662 /s | 32.62 µs |
| UPDATE of one hot row | B | 48,000 | 45.7 µs | 52.0 µs | 52.9 µs | 55.8 µs | 71.3 µs | 82.8 µs | 56.7 µs (sd 1.9) | 17,588 /s | 32.43 µs |
| DELETE by pk | A | 48,000 | 42.8 µs | 48.8 µs | 49.5 µs | 51.7 µs | 66.9 µs | 76.3 µs | 52.8 µs (sd 0.8) | 18,796 /s | 29.41 µs |
| DELETE by pk | B | 48,000 | 43.2 µs | 48.7 µs | 49.4 µs | 51.1 µs | 65.0 µs | 73.5 µs | 52.3 µs (sd 0.5) | 18,909 /s | 28.94 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 733.1 µs | 790.9 µs | 814.6 µs | 838.6 µs | 873.7 µs | 1,018.5 µs | 832.1 µs (sd 14.4) | 1,210 /s | 780.00 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 743.4 µs | 809.2 µs | 833.9 µs | 857.8 µs | 895.9 µs | 1,075.8 µs | 851.4 µs (sd 13.7) | 1,182 /s | 797.28 µs |
| SHOW META (control) | A | 48,000 | 43.9 µs | 49.7 µs | 50.4 µs | 53.1 µs | 64.5 µs | 73.2 µs | 52.7 µs (sd 0.9) | 18,925 /s | 33.70 µs |
| SHOW META (control) | B | 48,000 | 45.0 µs | 50.0 µs | 50.7 µs | 53.7 µs | 65.7 µs | 74.6 µs | 53.2 µs (sd 1.2) | 18,783 /s | 33.88 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 43.0 µs | 48.7 µs | 49.5 µs | 52.1 µs | 68.4 µs | 79.2 µs | 53.0 µs (sd 0.9) | 18,662 /s | 30.09 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 48,000 | 43.6 µs | 48.8 µs | 49.7 µs | 52.1 µs | 67.3 µs | 77.2 µs | 52.7 µs (sd 1.1) | 18,766 /s | 29.72 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.3 µs | 49.2 µs | 20,284 → 20,346 /s (+0.2 % [-0.4, +0.5]) | -0.10 µs [-0.20, +0.20] | -7.25 µs [-49.11, +33.88] | -0.31 µs [-0.57, +0.31] | -0.1 % [-32.1, +77.1] | 1.11 µs | no change resolved |
| INSERT again (replicate) | 49.4 µs | 49.2 µs | 20,243 → 20,325 /s (+0.3 % [-0.2, +0.6]) | -0.15 µs [-0.30, +0.10] | +9.47 µs [-25.21, +45.96] | -0.25 µs [-0.61, +0.08] | +0.8 % [+0.2, +3.3] | 1.11 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.9 µs | 49.7 µs | 20,040 → 20,141 /s (+0.8 % [+0.4, +1.0]) | -0.40 µs [-0.50, -0.20] | -0.92 µs [-2.43, +0.19] | -0.39 µs [-0.91, -0.17] | +1.0 % [-0.3, +2.3] | 1.11 µs | CI excludes 0, inside the floor: not resolved |
| SELECT by pk | 65.8 µs | 65.2 µs | 15,186 → 15,326 /s (+0.9 % [+0.3, +1.2]) | -0.60 µs [-0.80, -0.20] | -1.11 µs [-1.64, -0.59] | -0.95 µs [-1.29, -0.49] | +1.3 % [+0.6, +2.9] | 1.11 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 54.2 µs | 53.8 µs | 18,433 → 18,587 /s (+0.7 % [+0.4, +1.5]) | -0.40 µs [-0.80, -0.20] | -0.58 µs [-1.16, +0.04] | -0.69 µs [-1.02, -0.50] | +1.3 % [-0.7, +2.8] | 1.11 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 53.5 µs | 52.8 µs | 18,692 → 18,939 /s (+1.1 % [+0.7, +1.5]) | -0.60 µs [-0.80, -0.35] | +0.41 µs [-0.37, +1.41] | -0.26 µs [-0.65, +0.01] | -0.4 % [-1.7, +1.0] | 1.11 µs | CI excludes 0, inside the floor: not resolved |
| DELETE by pk | 49.5 µs | 49.4 µs | 20,222 → 20,243 /s (+0.2 % [-0.2, +0.6]) | -0.10 µs [-0.30, +0.10] | -0.43 µs [-0.82, -0.09] | -0.31 µs [-0.67, -0.03] | +0.4 % [+0.0, +1.4] | 1.11 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 813.5 µs | 830.5 µs | 1,229 → 1,204 /s (-2.0 % [-3.1, -1.6]) | +16.95 µs [+13.90, +25.80] | +19.26 µs [+13.29, +24.66] | +17.28 µs [+14.69, +25.16] | -2.6 % [-3.1, -1.7] | 2.55 µs | **B slower** (+2.1 % at p50) |
| SHOW META (control) | 50.5 µs | 50.8 µs | 19,822 → 19,704 /s (-0.9 % [-1.2, -0.2]) | +0.45 µs [+0.10, +0.60] | +0.49 µs [-0.10, +1.21] | +0.17 µs [-0.06, +0.51] | -0.6 % [-1.6, -0.2] | 1.11 µs | CI excludes 0, inside the floor: not resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.10, +0.15], Δ mean -0.26 µs [-0.99, +0.44], Δ server CPU per statement -0.10 µs [-0.39, +0.04]; lo p50 49.5 µs, hi p50 49.6 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -0.30 µs [-0.40, -0.10].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.22 µs, sd 0.22 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.10 µs, robust sd 0.37 µs, sd 0.35 µs; control SHOW META Δ p50 +0.45 µs [+0.10, +0.60]; run-to-run sd of the per-run Δ p50, INSERT 0.38 µs, UPDATE 0.52 µs, SELECT 0.61 µs, ping 0.36 µs.

## Appendix C. Strict durability, `cores = 1`

### strict, cores = 1, 200 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.26-8.83, after 1.47-7.70; busy of the other CPUs (6, 7 excluded) in kept runs 2.0%-5.2% (median 2.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 2 of the kept runs (range 2%-18%); gate wait median 3 s; ping p50 reference 51.5 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 11,621 us, B 7,582 us, max A 56,559 us, B 11,825 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 874.5 µs | 969.1 µs | 1,002.3 µs | 1,055.7 µs | 1,748.0 µs | 5,070.4 µs | 1169.1 µs (sd 44.2) | 863 /s | 77.48 µs |
| INSERT, pk omitted | B | 3,600 | 868.5 µs | 968.5 µs | 1,003.4 µs | 1,055.1 µs | 2,050.0 µs | 5,052.9 µs | 1185.3 µs (sd 60.4) | 847 /s | 78.79 µs |
| INSERT again (replicate) | A | 3,600 | 861.2 µs | 972.0 µs | 1,009.6 µs | 1,065.1 µs | 2,062.9 µs | 4,871.5 µs | 1190.1 µs (sd 36.0) | 835 /s | 78.49 µs |
| INSERT again (replicate) | B | 3,600 | 857.7 µs | 973.6 µs | 1,012.3 µs | 1,075.0 µs | 2,269.3 µs | 6,862.4 µs | 1745.2 µs (sd 834.5) | 799 /s | 79.55 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 878.8 µs | 971.7 µs | 1,008.5 µs | 1,062.1 µs | 1,875.8 µs | 5,041.7 µs | 1441.6 µs (sd 478.5) | 837 /s | 79.42 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 869.7 µs | 970.9 µs | 1,006.1 µs | 1,056.4 µs | 1,751.0 µs | 4,535.5 µs | 1408.2 µs (sd 469.8) | 874 /s | 79.59 µs |
| SELECT by pk | A | 3,600 | 55.7 µs | 65.2 µs | 69.7 µs | 74.2 µs | 91.1 µs | 128.0 µs | 72.7 µs (sd 0.5) | 13,656 /s | 36.69 µs |
| SELECT by pk | B | 3,600 | 58.9 µs | 65.0 µs | 68.4 µs | 73.8 µs | 89.8 µs | 115.4 µs | 71.9 µs (sd 1.0) | 13,831 /s | 35.85 µs |
| UPDATE by pk | A | 3,600 | 879.7 µs | 980.1 µs | 1,015.4 µs | 1,070.5 µs | 2,013.8 µs | 5,585.3 µs | 1676.0 µs (sd 797.1) | 818 /s | 84.07 µs |
| UPDATE by pk | B | 3,600 | 882.5 µs | 978.3 µs | 1,013.5 µs | 1,068.3 µs | 1,967.8 µs | 5,353.9 µs | 1284.0 µs (sd 212.8) | 825 /s | 84.53 µs |
| UPDATE of one hot row | A | 3,600 | 870.0 µs | 972.9 µs | 1,009.4 µs | 1,062.7 µs | 1,819.9 µs | 6,105.8 µs | 1457.6 µs (sd 612.4) | 813 /s | 83.45 µs |
| UPDATE of one hot row | B | 3,600 | 881.3 µs | 973.2 µs | 1,008.2 µs | 1,055.9 µs | 1,757.0 µs | 4,611.7 µs | 1399.5 µs (sd 631.8) | 872 /s | 82.91 µs |
| DELETE by pk | A | 3,600 | 859.4 µs | 966.2 µs | 1,000.6 µs | 1,050.8 µs | 1,903.0 µs | 4,998.5 µs | 1256.7 µs (sd 202.5) | 839 /s | 78.57 µs |
| DELETE by pk | B | 3,600 | 867.3 µs | 968.3 µs | 1,002.3 µs | 1,055.0 µs | 2,106.6 µs | 5,707.0 µs | 1302.4 µs (sd 263.8) | 839 /s | 79.40 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 82.9 µs | 85.5 µs | 87.0 µs | 92.8 µs | 111.4 µs | 182.0 µs | 96.7 µs (sd 14.6) | 10,922 /s | 54.47 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 83.4 µs | 86.5 µs | 87.7 µs | 93.2 µs | 110.4 µs | 136.1 µs | 92.4 µs (sd 1.1) | 10,737 /s | 55.42 µs |
| SHOW META (control) | A | 3,600 | 44.0 µs | 49.6 µs | 51.4 µs | 53.5 µs | 64.1 µs | 73.1 µs | 53.1 µs (sd 0.3) | 18,745 /s | 32.50 µs |
| SHOW META (control) | B | 3,600 | 45.1 µs | 49.7 µs | 51.6 µs | 53.4 µs | 64.1 µs | 73.8 µs | 53.2 µs (sd 0.6) | 18,732 /s | 32.45 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 867.9 µs | 967.5 µs | 1,003.3 µs | 1,057.2 µs | 1,994.4 µs | 4,888.8 µs | 1181.7 µs (sd 53.4) | 856 /s | 78.94 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 881.4 µs | 967.2 µs | 1,001.5 µs | 1,054.8 µs | 1,790.0 µs | 4,589.7 µs | 1186.8 µs (sd 82.9) | 867 /s | 79.29 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1004.2 µs | 1001.9 µs | 996 → 998 /s (+0.1 % [-0.9, +0.7]) | -0.55 µs [-6.75, +8.50] | +16.12 µs [-50.17, +77.80] | +1.38 µs [-0.38, +1.93] | -1.5 % [-8.6, +6.9] | 47.15 µs | no change resolved |
| INSERT again (replicate) | 1015.9 µs | 1016.0 µs | 984 → 984 /s (-0.0 % [-1.4, +0.3]) | +0.10 µs [-3.15, +14.45] | +555.12 µs [+14.17, +1105.65] | +1.38 µs [-0.37, +1.96] | -4.4 % [-57.3, +0.5] | 47.15 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1006.5 µs | 1004.7 µs | 994 → 995 /s (+0.1 % [-0.2, +0.5]) | -0.80 µs [-5.10, +1.95] | -33.45 µs [-335.38, +271.45] | +0.18 µs [-1.44, +2.86] | +0.1 % [-13.3, +25.8] | 47.15 µs | no change resolved |
| SELECT by pk | 69.3 µs | 68.3 µs | 14,430 → 14,641 /s (+1.5 % [-1.7, +4.2]) | -1.00 µs [-2.85, +1.20] | -0.77 µs [-1.47, +0.17] | -0.62 µs [-1.17, +0.48] | +2.0 % [-0.7, +2.1] | 47.15 µs | no change resolved |
| UPDATE by pk | 1011.3 µs | 1010.8 µs | 989 → 989 /s (+0.3 % [-0.3, +0.6]) | -3.10 µs [-6.55, +3.00] | -392.02 µs [-980.55, +38.27] | +1.02 µs [-1.01, +1.28] | +2.5 % [-6.1, +92.3] | 47.15 µs | no change resolved |
| UPDATE of one hot row | 1005.3 µs | 1005.6 µs | 995 → 994 /s (-0.0 % [-0.1, +0.7]) | +0.15 µs [-7.05, +0.65] | -58.10 µs [-803.53, +691.10] | -0.34 µs [-0.71, +2.31] | +7.3 % [-28.1, +72.5] | 47.15 µs | no change resolved |
| DELETE by pk | 994.1 µs | 1000.9 µs | 1,006 → 999 /s (-0.2 % [-0.8, +0.1]) | +1.90 µs [-1.40, +8.15] | +45.62 µs [-221.70, +328.80] | +1.77 µs [-0.58, +2.92] | -3.1 % [-20.2, +21.4] | 47.15 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 87.0 µs | 87.8 µs | 11,501 → 11,396 /s (-0.7 % [-1.6, -0.2]) | +0.60 µs [+0.15, +1.40] | -4.27 µs [-15.70, +1.75] | +1.00 µs [-0.63, +1.42] | -1.0 % [-2.2, +16.9] | 47.15 µs | CI excludes 0, inside the floor: not resolved |
| SHOW META (control) | 51.5 µs | 51.4 µs | 19,417 → 19,455 /s (+0.6 % [-3.2, +1.6]) | -0.30 µs [-0.80, +1.65] | +0.15 µs [-0.27, +0.65] | +0.05 µs [-0.29, +0.49] | -0.1 % [-1.5, +0.8] | 47.15 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 -2.60 µs [-10.65, +4.80], Δ mean +5.15 µs [-70.05, +81.92], Δ server CPU per statement +0.38 µs [-0.51, +1.73]; lo p50 999.5 µs, hi p50 1001.3 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -12.65 µs [-23.05, +11.45].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +6.65 µs, robust sd 15.72 µs, sd 14.84 µs; replicate arm B (INSERT again − INSERT p50 per run) median +5.75 µs, robust sd 12.53 µs, sd 16.10 µs; control SHOW META Δ p50 -0.30 µs [-0.80, +1.65]; run-to-run sd of the per-run Δ p50, INSERT 7.85 µs, UPDATE 5.30 µs, SELECT 2.10 µs, ping 1.19 µs.

### strict, cores = 1, 1,000 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.95-5.99, after 1.82-5.23; busy of the other CPUs (6, 7 excluded) in kept runs 1.5%-2.6% (median 2.0%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 51.8 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,714 us, B 8,136 us, max A 15,374 us, B 9,166 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 884.0 µs | 972.8 µs | 1,007.1 µs | 1,058.1 µs | 1,735.6 µs | 4,877.9 µs | 1166.0 µs (sd 60.4) | 860 /s | 77.97 µs |
| INSERT, pk omitted | B | 3,600 | 886.6 µs | 976.0 µs | 1,012.3 µs | 1,063.5 µs | 1,980.9 µs | 5,001.4 µs | 1181.0 µs (sd 19.4) | 846 /s | 77.33 µs |
| INSERT again (replicate) | A | 3,600 | 836.4 µs | 979.8 µs | 1,015.6 µs | 1,068.9 µs | 1,794.0 µs | 4,784.1 µs | 1169.5 µs (sd 21.8) | 859 /s | 78.18 µs |
| INSERT again (replicate) | B | 3,600 | 884.0 µs | 978.5 µs | 1,014.4 µs | 1,072.5 µs | 1,924.3 µs | 5,008.4 µs | 1191.7 µs (sd 58.5) | 841 /s | 77.44 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 882.1 µs | 974.3 µs | 1,008.7 µs | 1,058.0 µs | 1,437.4 µs | 4,296.9 µs | 1134.0 µs (sd 28.6) | 885 /s | 78.72 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 879.6 µs | 976.0 µs | 1,010.5 µs | 1,064.0 µs | 1,683.1 µs | 5,357.5 µs | 1199.2 µs (sd 52.7) | 844 /s | 78.02 µs |
| SELECT by pk | A | 3,600 | 59.8 µs | 64.7 µs | 65.7 µs | 68.2 µs | 81.1 µs | 101.1 µs | 68.6 µs (sd 0.8) | 14,459 /s | 32.59 µs |
| SELECT by pk | B | 3,600 | 56.6 µs | 64.3 µs | 65.4 µs | 67.6 µs | 80.3 µs | 101.0 µs | 67.9 µs (sd 0.8) | 14,575 /s | 32.12 µs |
| UPDATE by pk | A | 3,600 | 879.6 µs | 981.0 µs | 1,016.2 µs | 1,063.3 µs | 1,429.7 µs | 4,322.1 µs | 1137.4 µs (sd 23.7) | 880 /s | 83.47 µs |
| UPDATE by pk | B | 3,600 | 878.9 µs | 981.6 µs | 1,018.8 µs | 1,070.9 µs | 1,872.5 µs | 6,530.9 µs | 1211.6 µs (sd 72.3) | 831 /s | 83.55 µs |
| UPDATE of one hot row | A | 3,600 | 870.1 µs | 976.2 µs | 1,013.7 µs | 1,067.5 µs | 1,890.8 µs | 4,639.0 µs | 1177.9 µs (sd 45.4) | 852 /s | 83.28 µs |
| UPDATE of one hot row | B | 3,600 | 875.9 µs | 975.5 µs | 1,012.5 µs | 1,068.2 µs | 1,888.2 µs | 4,964.5 µs | 1173.7 µs (sd 51.4) | 845 /s | 83.28 µs |
| DELETE by pk | A | 3,600 | 873.5 µs | 966.7 µs | 1,002.8 µs | 1,049.7 µs | 1,716.7 µs | 5,046.5 µs | 1180.6 µs (sd 64.8) | 862 /s | 78.95 µs |
| DELETE by pk | B | 3,600 | 858.5 µs | 967.9 µs | 1,002.2 µs | 1,047.5 µs | 1,471.6 µs | 4,218.5 µs | 1127.3 µs (sd 60.3) | 907 /s | 77.19 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 137.3 µs | 143.6 µs | 147.3 µs | 156.8 µs | 172.8 µs | 196.5 µs | 151.7 µs (sd 1.7) | 6,560 /s | 114.82 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 140.5 µs | 148.0 µs | 152.1 µs | 162.0 µs | 179.6 µs | 205.0 µs | 157.0 µs (sd 2.0) | 6,336 /s | 119.28 µs |
| SHOW META (control) | A | 3,600 | 44.5 µs | 50.0 µs | 52.1 µs | 54.2 µs | 65.9 µs | 74.8 µs | 53.6 µs (sd 0.6) | 18,554 /s | 32.50 µs |
| SHOW META (control) | B | 3,600 | 45.8 µs | 50.1 µs | 51.5 µs | 54.0 µs | 65.5 µs | 74.3 µs | 53.3 µs (sd 0.6) | 18,658 /s | 32.84 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 866.4 µs | 973.9 µs | 1,007.6 µs | 1,056.7 µs | 1,539.6 µs | 4,492.0 µs | 1147.1 µs (sd 25.7) | 869 /s | 77.98 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 883.6 µs | 974.3 µs | 1,008.8 µs | 1,057.2 µs | 1,647.6 µs | 4,854.6 µs | 1175.0 µs (sd 52.5) | 856 /s | 78.84 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1007.4 µs | 1009.4 µs | 993 → 991 /s (-0.7 % [-1.5, +0.5]) | +6.80 µs [-4.55, +15.15] | +14.92 µs [-28.32, +56.77] | -0.59 µs [-1.25, -0.17] | -1.0 % [-6.9, +4.1] | 20.24 µs | no change resolved |
| INSERT again (replicate) | 1015.0 µs | 1014.9 µs | 985 → 985 /s (-0.1 % [-0.3, +0.6]) | +0.70 µs [-6.45, +3.35] | +22.15 µs [-28.30, +71.25] | -0.73 µs [-1.25, +0.73] | -1.4 % [-7.5, +4.1] | 20.24 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1011.2 µs | 1009.9 µs | 989 → 990 /s (-0.6 % [-1.0, +0.9]) | +5.60 µs [-9.30, +10.00] | +65.17 µs [+23.13, +116.45] | -0.57 µs [-1.51, +0.21] | -4.7 % [-10.3, -0.8] | 20.24 µs | no change resolved |
| SELECT by pk | 65.8 µs | 65.5 µs | 15,186 → 15,279 /s (+0.3 % [-0.2, +1.2]) | -0.20 µs [-0.80, +0.10] | -0.60 µs [-1.20, -0.02] | -0.42 µs [-0.74, +0.22] | +0.7 % [-0.2, +2.2] | 20.24 µs | no change resolved |
| UPDATE by pk | 1013.2 µs | 1017.1 µs | 987 → 983 /s (-0.4 % [-0.7, +0.8]) | +3.85 µs [-8.00, +7.40] | +74.23 µs [+26.10, +132.93] | -0.21 µs [-1.66, +1.27] | -3.3 % [-12.6, -1.7] | 20.24 µs | no change resolved |
| UPDATE of one hot row | 1011.5 µs | 1010.0 µs | 989 → 990 /s (+0.3 % [-0.4, +0.4]) | -3.05 µs [-4.15, +4.30] | -4.13 µs [-17.53, +9.63] | -0.98 µs [-2.30, +1.34] | +0.6 % [-1.5, +2.0] | 20.24 µs | no change resolved |
| DELETE by pk | 995.5 µs | 1000.5 µs | 1,005 → 1,000 /s (-0.2 % [-0.5, +0.5]) | +1.75 µs [-5.10, +5.50] | -53.30 µs [-107.47, -4.47] | -1.06 µs [-2.40, +0.53] | +3.8 % [-0.8, +11.6] | 20.24 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 146.9 µs | 151.6 µs | 6,805 → 6,596 /s (-3.1 % [-3.4, -2.8]) | +4.70 µs [+4.30, +5.25] | +5.33 µs [+4.80, +6.00] | +4.69 µs [+3.92, +5.55] | -3.3 % [-3.8, -2.9] | 20.24 µs | CI excludes 0, inside the floor: not resolved |
| SHOW META (control) | 52.0 µs | 51.6 µs | 19,249 → 19,380 /s (+1.6 % [+0.4, +2.2]) | -0.80 µs [-1.15, -0.20] | -0.27 µs [-0.47, -0.03] | +0.78 µs [-0.07, +1.28] | +0.7 % [-0.1, +1.1] | 20.24 µs | CI excludes 0, inside the floor: not resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +1.45 µs [-4.35, +6.80], Δ mean +27.87 µs [-8.07, +66.33], Δ server CPU per statement +1.17 µs [+0.09, +1.78]; lo p50 1008.5 µs, hi p50 1010.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +1.20 µs [-11.00, +9.40].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +9.60 µs, robust sd 3.41 µs, sd 6.74 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.95 µs, robust sd 6.75 µs, sd 14.75 µs; control SHOW META Δ p50 -0.80 µs [-1.15, -0.20]; run-to-run sd of the per-run Δ p50, INSERT 9.62 µs, UPDATE 7.75 µs, SELECT 0.42 µs, ping 0.44 µs.

### strict, cores = 1, 10,000 rows

6 runs kept of 6 (runs 0-5); loadavg(1 min) before each run 1.01-6.88, after 1.01-5.64; busy of the other CPUs (6, 7 excluded) in kept runs 1.8%-8.7% (median 2.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 1 of the kept runs (range 2%-35%); gate wait median 3 s; ping p50 reference 50.9 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,056 us, B 8,396 us, max A 13,486 us, B 16,893 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 3,600 | 877.7 µs | 973.4 µs | 1,007.1 µs | 1,058.7 µs | 1,555.1 µs | 4,413.2 µs | 1150.0 µs (sd 18.8) | 866 /s | 79.01 µs |
| INSERT, pk omitted | B | 3,600 | 872.4 µs | 973.3 µs | 1,009.2 µs | 1,061.4 µs | 2,091.4 µs | 5,032.2 µs | 1189.0 µs (sd 43.8) | 849 /s | 77.54 µs |
| INSERT again (replicate) | A | 3,600 | 839.0 µs | 969.8 µs | 1,004.3 µs | 1,059.2 µs | 2,000.9 µs | 4,875.2 µs | 1283.6 µs (sd 208.5) | 822 /s | 80.58 µs |
| INSERT again (replicate) | B | 3,600 | 857.3 µs | 969.1 µs | 1,003.7 µs | 1,055.0 µs | 1,669.0 µs | 4,690.5 µs | 1173.0 µs (sd 45.8) | 859 /s | 79.37 µs |
| INSERT, pk named (keys below 2^40) | A | 3,600 | 881.8 µs | 971.1 µs | 1,006.5 µs | 1,057.8 µs | 1,856.8 µs | 4,583.4 µs | 1240.2 µs (sd 171.3) | 850 /s | 78.62 µs |
| INSERT, pk named (keys below 2^40) | B | 3,600 | 865.1 µs | 972.0 µs | 1,006.6 µs | 1,062.7 µs | 2,063.5 µs | 4,559.4 µs | 1209.9 µs (sd 79.0) | 852 /s | 79.43 µs |
| SELECT by pk | A | 3,600 | 61.6 µs | 64.9 µs | 66.1 µs | 71.7 µs | 81.7 µs | 102.9 µs | 69.4 µs (sd 2.2) | 14,384 /s | 33.28 µs |
| SELECT by pk | B | 3,600 | 57.9 µs | 64.8 µs | 66.1 µs | 71.7 µs | 83.7 µs | 103.9 µs | 69.5 µs (sd 2.5) | 14,408 /s | 33.24 µs |
| UPDATE by pk | A | 3,600 | 888.1 µs | 975.9 µs | 1,009.8 µs | 1,058.9 µs | 1,799.7 µs | 5,011.9 µs | 1173.9 µs (sd 37.0) | 856 /s | 84.18 µs |
| UPDATE by pk | B | 3,600 | 881.7 µs | 975.3 µs | 1,009.1 µs | 1,058.0 µs | 1,657.7 µs | 4,686.8 µs | 1158.3 µs (sd 37.9) | 851 /s | 82.48 µs |
| UPDATE of one hot row | A | 3,600 | 875.5 µs | 970.9 µs | 1,004.5 µs | 1,052.2 µs | 1,670.4 µs | 4,690.0 µs | 1157.0 µs (sd 33.6) | 870 /s | 83.46 µs |
| UPDATE of one hot row | B | 3,600 | 879.3 µs | 970.2 µs | 1,007.2 µs | 1,065.8 µs | 2,129.3 µs | 5,599.9 µs | 1275.2 µs (sd 202.1) | 827 /s | 82.09 µs |
| DELETE by pk | A | 3,600 | 879.7 µs | 970.0 µs | 1,003.4 µs | 1,053.1 µs | 1,579.2 µs | 4,370.6 µs | 1137.0 µs (sd 44.9) | 875 /s | 78.01 µs |
| DELETE by pk | B | 3,600 | 867.5 µs | 970.0 µs | 1,003.4 µs | 1,053.0 µs | 1,683.8 µs | 4,662.3 µs | 1223.3 µs (sd 187.4) | 884 /s | 76.30 µs |
| COUNT/MIN/MAX over 100 ids | A | 3,600 | 736.5 µs | 784.8 µs | 809.4 µs | 834.7 µs | 874.2 µs | 1,050.2 µs | 823.3 µs (sd 14.7) | 1,214 /s | 766.61 µs |
| COUNT/MIN/MAX over 100 ids | B | 3,600 | 774.3 µs | 829.7 µs | 853.3 µs | 877.1 µs | 923.9 µs | 1,143.8 µs | 864.0 µs (sd 11.7) | 1,155 /s | 814.89 µs |
| SHOW META (control) | A | 3,600 | 45.0 µs | 49.7 µs | 51.0 µs | 53.7 µs | 64.0 µs | 73.5 µs | 55.6 µs (sd 6.8) | 18,777 /s | 32.90 µs |
| SHOW META (control) | B | 3,600 | 45.6 µs | 49.8 µs | 51.0 µs | 53.5 µs | 63.3 µs | 72.4 µs | 52.8 µs (sd 0.6) | 18,931 /s | 32.75 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 3,600 | 873.0 µs | 971.7 µs | 1,005.8 µs | 1,055.1 µs | 1,810.4 µs | 4,913.3 µs | 1175.9 µs (sd 20.5) | 854 /s | 76.68 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 3,600 | 863.4 µs | 969.9 µs | 1,005.1 µs | 1,054.3 µs | 1,631.3 µs | 5,237.9 µs | 1248.2 µs (sd 233.2) | 880 /s | 77.03 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 1007.6 µs | 1012.0 µs | 992 → 988 /s (-0.2 % [-0.6, +0.5]) | +2.15 µs [-4.70, +6.35] | +39.00 µs [+10.48, +67.52] | -1.08 µs [-1.81, -0.10] | -3.1 % [-6.7, +0.2] | 35.58 µs | no change resolved |
| INSERT again (replicate) | 1009.7 µs | 1003.1 µs | 990 → 997 /s (+0.1 % [-1.0, +0.9]) | -1.05 µs [-8.90, +10.40] | -110.57 µs [-289.40, +8.47] | -0.44 µs [-3.79, +1.44] | +4.5 % [-2.7, +27.0] | 35.58 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 1004.1 µs | 1006.3 µs | 996 → 994 /s (-0.1 % [-1.0, +1.2]) | +1.10 µs [-11.70, +10.10] | -30.25 µs [-205.32, +108.88] | +1.45 µs [-2.08, +4.16] | +0.6 % [-12.7, +21.9] | 35.58 µs | no change resolved |
| SELECT by pk | 65.9 µs | 65.9 µs | 15,175 → 15,175 /s (-0.0 % [-1.0, +1.6]) | +0.00 µs [-1.05, +0.65] | +0.08 µs [-1.13, +1.32] | -0.21 µs [-1.34, +0.98] | +0.1 % [-2.6, +2.2] | 35.58 µs | no change resolved |
| UPDATE by pk | 1008.2 µs | 1009.2 µs | 992 → 991 /s (+0.2 % [-0.7, +0.5]) | -2.25 µs [-4.95, +7.25] | -15.62 µs [-49.53, +23.97] | -1.38 µs [-1.83, -1.08] | +2.9 % [-3.7, +5.1] | 35.58 µs | no change resolved |
| UPDATE of one hot row | 1008.2 µs | 1012.4 µs | 992 → 988 /s (-0.1 % [-0.7, +0.4]) | +0.60 µs [-3.75, +7.35] | +118.20 µs [+9.73, +295.33] | -1.25 µs [-2.40, -0.09] | -4.5 % [-19.5, +1.2] | 35.58 µs | no change resolved |
| DELETE by pk | 999.3 µs | 1000.1 µs | 1,001 → 1,000 /s (+0.4 % [-1.0, +0.6]) | -4.10 µs [-6.30, +9.85] | +86.37 µs [-30.80, +251.87] | -1.71 µs [-2.11, -0.80] | -0.2 % [-20.5, +4.2] | 35.58 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 807.4 µs | 854.2 µs | 1,239 → 1,171 /s (-5.0 % [-6.0, -4.4]) | +42.50 µs [+37.15, +51.00] | +40.77 µs [+28.38, +54.28] | +44.96 µs [+36.25, +56.01] | -4.9 % [-6.7, -2.5] | 35.58 µs | **B slower** (+5.3 % at p50) |
| SHOW META (control) | 50.8 µs | 50.9 µs | 19,666 → 19,646 /s (-0.2 % [-1.0, +0.6]) | +0.10 µs [-0.30, +0.50] | -2.85 µs [-8.53, +0.10] | -0.09 µs [-0.70, +0.27] | +0.0 % [-0.2, +16.4] | 35.58 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 -0.80 µs [-5.10, +2.65], Δ mean +72.25 µs [-57.85, +256.92], Δ server CPU per statement +0.40 µs [-0.38, +1.21]; lo p50 1011.5 µs, hi p50 1007.9 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 -4.75 µs [-11.05, +9.45].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -3.25 µs, robust sd 6.45 µs, sd 7.35 µs; replicate arm B (INSERT again − INSERT p50 per run) median -6.40 µs, robust sd 11.86 µs, sd 10.44 µs; control SHOW META Δ p50 +0.10 µs [-0.30, +0.50]; run-to-run sd of the per-run Δ p50, INSERT 5.43 µs, UPDATE 5.77 µs, SELECT 0.95 µs, ping 0.41 µs.

## Appendix D. The flipped pair (slot A holds B's binary), `cores = 1`; the tables below are already read as B − A

### flipped pair, cores = 1, 1,000 rows

13 runs kept of 13 (runs 0-14); loadavg(1 min) before each run 0.73-2.79, after 0.76-3.05; busy of the other CPUs (6, 7 excluded) in kept runs 1.6%-4.9% (median 2.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,913 us, B 7,898 us, max A 458,462 us, B 301,699 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (+0.2 % [-1.0, +0.6]) | -0.10 µs [-0.30, +0.50] | +5.95 µs [-0.45, +17.40] | +0.12 µs [-0.19, +0.56] | -0.3 % [-2.4, +1.0] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.3 µs | 48.9 µs | 20,284 → 20,450 /s (+0.2 % [-0.2, +0.6]) | -0.10 µs [-0.30, +0.10] | -0.71 µs [-1.93, +0.38] | +0.14 µs [-0.13, +0.43] | +0.6 % [-1.0, +3.1] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.2 % [-0.4, +0.6]) | -0.10 µs [-0.30, +0.20] | +0.28 µs [-0.72, +1.48] | +0.19 µs [-0.12, +0.60] | +0.4 % [-1.7, +1.2] | 0.89 µs | no change resolved |
| SELECT by pk | 73.3 µs | 72.3 µs | 13,643 → 13,831 /s (+0.5 % [+0.0, +1.7]) | -0.40 µs [-1.30, +0.00] | +2.12 µs [-0.22, +5.86] | +0.07 µs [-0.26, +0.45] | -0.0 % [-1.9, +0.9] | 0.89 µs | no change resolved |
| UPDATE by pk | 53.3 µs | 52.5 µs | 18,762 → 19,048 /s (+0.2 % [-1.0, +0.7]) | -0.10 µs [-0.50, +0.50] | -8.42 µs [-46.12, +21.36] | -0.31 µs [-0.49, +0.12] | +1.5 % [-2.3, +4.9] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 51.9 µs | 52.4 µs | 19,268 → 19,084 /s (+0.0 % [-1.0, +0.8]) | +0.00 µs [-0.40, +0.50] | -7.22 µs [-19.32, -0.53] | -0.06 µs [-0.38, +0.32] | +1.6 % [+0.6, +4.2] | 0.89 µs | no change resolved |
| DELETE by pk | 49.1 µs | 49.0 µs | 20,367 → 20,408 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -4.45 µs [-11.67, +0.72] | -0.10 µs [-0.41, +0.10] | -0.4 % [-1.6, +1.1] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 147.0 µs | 150.9 µs | 6,803 → 6,627 /s (-2.6 % [-3.2, -1.9]) | +4.00 µs [+2.90, +5.00] | -38.37 µs [-108.24, +10.63] | +4.05 µs [+3.19, +5.56] | -2.7 % [-3.6, -2.4] | 0.89 µs | **B slower** (+2.7 % at p50) |
| SHOW META (control) | 50.3 µs | 50.4 µs | 19,881 → 19,841 /s (-0.4 % [-0.6, +0.0]) | +0.20 µs [+0.00, +0.30] | +1.92 µs [-0.81, +6.02] | +0.36 µs [+0.06, +0.50] | -0.5 % [-0.9, +0.9] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 4.18 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 2.89 µs; control SHOW META Δ p50 +0.20 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 3.23 µs, UPDATE 1.61 µs, SELECT 7.02 µs, ping 0.37 µs.

### flipped pair, cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.71-1.22, after 0.86-1.39; busy of the other CPUs (6, 7 excluded) in kept runs 1.6%-4.0% (median 2.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.1 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,494 us, B 8,520 us, max A 14,614 us, B 12,180 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,513 → 20,471 /s (+0.0 % [-0.4, +0.4]) | +0.00 µs [-0.20, +0.20] | +15.96 µs [-16.07, +46.89] | -0.28 µs [-0.74, +0.22] | -1.2 % [-56.7, +1.6] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.7 µs | 20,513 → 20,534 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.20, +0.10] | -20.14 µs [-48.51, +11.43] | -0.10 µs [-0.33, +0.07] | +41.2 % [-0.5, +114.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (-0.2 % [-0.3, +0.2]) | +0.10 µs [-0.10, +0.15] | +0.26 µs [-0.32, +0.83] | +0.13 µs [-0.10, +0.25] | -1.1 % [-2.2, +1.5] | 0.67 µs | no change resolved |
| SELECT by pk | 64.8 µs | 64.8 µs | 15,420 → 15,432 /s (+0.2 % [-0.2, +0.6]) | -0.10 µs [-0.40, +0.15] | +0.01 µs [-0.36, +0.40] | -0.05 µs [-0.33, +0.10] | +0.1 % [-0.4, +0.6] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.0 µs | 52.8 µs | 18,868 → 18,939 /s (+0.0 % [-0.4, +0.9]) | +0.00 µs [-0.45, +0.35] | +0.04 µs [-0.56, +0.65] | +0.02 µs [-0.44, +0.28] | +0.6 % [-1.1, +1.5] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.2 µs | 52.0 µs | 19,175 → 19,249 /s (+0.1 % [-0.6, +1.2]) | -0.05 µs [-0.60, +0.30] | -0.29 µs [-1.51, +1.27] | -0.10 µs [-0.46, +0.22] | +1.2 % [-0.0, +2.5] | 0.67 µs | no change resolved |
| DELETE by pk | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +0.38 µs [-0.16, +0.96] | +0.21 µs [-0.14, +0.36] | -0.4 % [-1.8, +0.3] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 804.5 µs | 839.3 µs | 1,243 → 1,191 /s (-4.4 % [-4.9, -3.5]) | +36.90 µs [+29.30, +41.10] | +41.74 µs [+33.38, +49.96] | +37.69 µs [+30.71, +45.25] | -4.9 % [-6.2, -3.1] | 0.67 µs | **B slower** (+4.6 % at p50) |
| SHOW META (control) | 50.1 µs | 50.1 µs | 19,960 → 19,960 /s (+0.2 % [-0.1, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.11 µs [-0.27, +0.08] | -0.16 µs [-0.24, +0.09] | +0.3 % [-0.1, +0.6] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.22 µs, sd 0.22 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.28 µs, UPDATE 0.65 µs, SELECT 0.46 µs, ping 0.23 µs.

## Appendix D2. Fresh copies: A (copy 3) in slot A against B (copy 2) in slot B, `cores = 1`, made and first run at about 09:40 UTC

### fresh copies (A copy 3 against B copy 2), cores = 1, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.55-0.86, after 0.65-0.95; busy of the other CPUs (6, 7 excluded) in kept runs 1.0%-2.0% (median 1.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-2%); gate wait median 3 s; ping p50 reference 50.1 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 10,882 us, B 10,016 us, max A 23,686 us, B 25,461 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 36.0 µs | 48.1 µs | 48.9 µs | 50.2 µs | 63.2 µs | 78.1 µs | 50.9 µs (sd 0.6) | 19,443 /s | 28.29 µs |
| INSERT, pk omitted | B | 48,000 | 40.9 µs | 48.1 µs | 48.9 µs | 50.1 µs | 62.7 µs | 76.4 µs | 50.7 µs (sd 0.5) | 19,489 /s | 28.31 µs |
| INSERT again (replicate) | A | 48,000 | 41.1 µs | 48.0 µs | 48.8 µs | 50.0 µs | 62.6 µs | 77.8 µs | 51.4 µs (sd 1.4) | 19,422 /s | 28.17 µs |
| INSERT again (replicate) | B | 48,000 | 40.7 µs | 48.1 µs | 48.8 µs | 50.0 µs | 62.4 µs | 75.9 µs | 51.6 µs (sd 1.5) | 19,095 /s | 28.26 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 39.7 µs | 48.2 µs | 48.9 µs | 50.2 µs | 63.3 µs | 77.9 µs | 51.7 µs (sd 0.8) | 19,031 /s | 28.47 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 40.8 µs | 48.2 µs | 49.0 µs | 50.2 µs | 63.2 µs | 75.8 µs | 51.5 µs (sd 0.8) | 19,086 /s | 28.54 µs |
| SELECT by pk | A | 48,000 | 59.6 µs | 64.8 µs | 67.2 µs | 73.0 µs | 85.8 µs | 101.1 µs | 70.4 µs (sd 0.6) | 14,079 /s | 35.53 µs |
| SELECT by pk | B | 48,000 | 56.7 µs | 64.3 µs | 66.9 µs | 72.5 µs | 85.2 µs | 100.0 µs | 70.2 µs (sd 0.9) | 14,147 /s | 35.16 µs |
| UPDATE by pk | A | 48,000 | 42.9 µs | 51.5 µs | 52.2 µs | 54.1 µs | 66.6 µs | 81.1 µs | 55.2 µs (sd 1.2) | 17,978 /s | 30.71 µs |
| UPDATE by pk | B | 48,000 | 42.8 µs | 51.2 µs | 52.1 µs | 54.0 µs | 66.5 µs | 80.0 µs | 55.5 µs (sd 1.9) | 18,030 /s | 30.45 µs |
| UPDATE of one hot row | A | 48,000 | 40.1 µs | 51.2 µs | 51.8 µs | 53.7 µs | 66.5 µs | 80.2 µs | 54.1 µs (sd 0.6) | 18,378 /s | 30.50 µs |
| UPDATE of one hot row | B | 48,000 | 43.6 µs | 51.0 µs | 51.8 µs | 53.6 µs | 66.3 µs | 78.9 µs | 54.9 µs (sd 1.3) | 18,046 /s | 30.38 µs |
| DELETE by pk | A | 48,000 | 40.7 µs | 48.1 µs | 48.9 µs | 49.9 µs | 61.0 µs | 71.0 µs | 51.1 µs (sd 1.4) | 19,697 /s | 27.71 µs |
| DELETE by pk | B | 48,000 | 39.9 µs | 48.2 µs | 49.0 µs | 50.0 µs | 61.5 µs | 71.4 µs | 50.8 µs (sd 0.8) | 19,637 /s | 27.88 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 135.4 µs | 142.3 µs | 145.3 µs | 152.5 µs | 168.0 µs | 195.1 µs | 149.1 µs (sd 1.6) | 6,687 /s | 112.21 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 136.8 µs | 144.2 µs | 147.3 µs | 153.6 µs | 167.7 µs | 188.4 µs | 150.6 µs (sd 1.0) | 6,622 /s | 113.99 µs |
| SHOW META (control) | A | 48,000 | 40.9 µs | 49.5 µs | 50.2 µs | 52.6 µs | 61.5 µs | 68.7 µs | 51.9 µs (sd 0.4) | 19,161 /s | 32.80 µs |
| SHOW META (control) | B | 48,000 | 44.1 µs | 49.4 µs | 50.1 µs | 52.5 µs | 61.1 µs | 68.4 µs | 51.8 µs (sd 0.6) | 19,251 /s | 32.67 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 40.6 µs | 48.3 µs | 49.0 µs | 50.4 µs | 63.4 µs | 74.3 µs | 51.2 µs (sd 0.7) | 19,193 /s | 28.44 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 48,000 | 40.2 µs | 48.4 µs | 49.1 µs | 50.5 µs | 63.6 µs | 73.3 µs | 52.0 µs (sd 0.9) | 18,925 /s | 28.49 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.9 µs | 49.0 µs | 20,450 → 20,429 /s (+0.1 % [+0.0, +0.3]) | -0.05 µs [-0.15, +0.00] | -0.18 µs [-0.39, -0.01] | -0.02 µs [-0.25, +0.11] | +0.3 % [-0.2, +0.6] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,471 /s (-0.2 % [-0.2, +0.2]) | +0.10 µs [-0.10, +0.10] | +0.24 µs [-0.43, +0.89] | +0.08 µs [-0.27, +0.20] | -0.3 % [-2.3, +1.3] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.8 µs | 48.8 µs | 20,471 → 20,492 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.10] | -0.23 µs [-0.67, +0.26] | +0.01 µs [-0.23, +0.25] | +1.2 % [-0.7, +1.6] | 0.60 µs | no change resolved |
| SELECT by pk | 67.0 µs | 66.8 µs | 14,925 → 14,959 /s (+0.6 % [-0.1, +1.3]) | -0.40 µs [-0.85, +0.10] | -0.18 µs [-0.61, +0.26] | -0.53 µs [-0.96, +0.23] | +0.2 % [-0.8, +1.5] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.2 µs | 51.8 µs | 19,175 → 19,305 /s (+0.8 % [+0.2, +0.9]) | -0.40 µs [-0.45, -0.10] | +0.27 µs [-0.62, +1.23] | -0.24 µs [-0.41, +0.17] | +0.9 % [-2.4, +2.5] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 51.8 µs | 51.5 µs | 19,305 → 19,417 /s (+0.5 % [+0.0, +1.0]) | -0.25 µs [-0.50, +0.00] | +0.82 µs [+0.21, +1.48] | -0.09 µs [-0.39, +0.07] | -1.2 % [-3.1, +0.5] | 0.60 µs | no change resolved |
| DELETE by pk | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.6, +0.4]) | +0.00 µs [-0.20, +0.30] | -0.27 µs [-0.95, +0.22] | +0.16 µs [-0.14, +0.44] | +0.2 % [-0.5, +0.7] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 145.2 µs | 147.1 µs | 6,887 → 6,798 /s (-1.2 % [-1.5, -1.0]) | +1.70 µs [+1.50, +2.20] | +1.54 µs [+0.83, +2.23] | +1.65 µs [+1.42, +2.18] | -1.0 % [-1.6, -0.6] | 0.60 µs | **B slower** (+1.2 % at p50) |
| SHOW META (control) | 50.1 µs | 50.0 µs | 19,960 → 19,980 /s (+0.2 % [+0.0, +0.5]) | -0.10 µs [-0.20, +0.00] | -0.10 µs [-0.28, +0.09] | -0.15 µs [-0.33, +0.00] | +0.4 % [-0.2, +0.7] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.15], Δ mean +0.74 µs [+0.20, +1.24], Δ server CPU per statement +0.05 µs [+0.01, +0.17]; lo p50 48.9 µs, hi p50 49.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.25 µs [+0.00, +0.35].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.15 µs, sd 0.17 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.13 µs, UPDATE 0.65 µs, SELECT 0.67 µs, ping 0.28 µs.

### fresh copies (A copy 3 against B copy 2), cores = 1, 10,000 rows

15 runs kept of 15 (runs 1-15); loadavg(1 min) before each run 0.64-0.95, after 0.76-1.08; busy of the other CPUs (6, 7 excluded) in kept runs 1.0%-2.0% (median 1.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.0 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,439 us, B 9,906 us, max A 23,006 us, B 21,203 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 45,000 | 39.7 µs | 47.9 µs | 48.7 µs | 50.1 µs | 62.1 µs | 76.6 µs | 105.2 µs (sd 60.7) | 9,797 /s | 28.44 µs |
| INSERT, pk omitted | B | 45,000 | 38.4 µs | 48.0 µs | 48.7 µs | 50.0 µs | 61.9 µs | 74.9 µs | 97.7 µs (sd 57.6) | 18,718 /s | 28.68 µs |
| INSERT again (replicate) | A | 45,000 | 40.3 µs | 47.9 µs | 48.7 µs | 50.2 µs | 63.7 µs | 79.1 µs | 58.9 µs (sd 20.9) | 19,218 /s | 28.44 µs |
| INSERT again (replicate) | B | 45,000 | 41.1 µs | 48.0 µs | 48.7 µs | 50.2 µs | 63.5 µs | 79.4 µs | 73.8 µs (sd 30.1) | 18,538 /s | 28.69 µs |
| INSERT, pk named (keys below 2^40) | A | 45,000 | 40.7 µs | 48.3 µs | 49.0 µs | 50.4 µs | 63.7 µs | 78.6 µs | 51.8 µs (sd 0.7) | 19,043 /s | 28.56 µs |
| INSERT, pk named (keys below 2^40) | B | 45,000 | 40.6 µs | 48.3 µs | 48.9 µs | 50.4 µs | 63.8 µs | 78.4 µs | 51.7 µs (sd 0.8) | 19,120 /s | 28.70 µs |
| SELECT by pk | A | 45,000 | 58.3 µs | 63.8 µs | 64.7 µs | 66.6 µs | 78.5 µs | 89.9 µs | 67.2 µs (sd 1.1) | 14,811 /s | 31.97 µs |
| SELECT by pk | B | 45,000 | 58.5 µs | 63.7 µs | 64.5 µs | 66.2 µs | 77.9 µs | 87.0 µs | 66.4 µs (sd 0.7) | 14,914 /s | 31.81 µs |
| UPDATE by pk | A | 45,000 | 43.7 µs | 51.9 µs | 52.9 µs | 54.7 µs | 67.2 µs | 81.1 µs | 55.6 µs (sd 0.9) | 17,836 /s | 31.14 µs |
| UPDATE by pk | B | 45,000 | 43.2 µs | 51.9 µs | 52.8 µs | 54.6 µs | 67.0 µs | 79.5 µs | 55.4 µs (sd 1.0) | 17,773 /s | 31.26 µs |
| UPDATE of one hot row | A | 45,000 | 43.0 µs | 51.3 µs | 52.2 µs | 54.5 µs | 66.9 µs | 79.7 µs | 54.4 µs (sd 0.9) | 18,291 /s | 30.61 µs |
| UPDATE of one hot row | B | 45,000 | 40.4 µs | 51.2 µs | 52.1 µs | 54.2 µs | 66.6 µs | 78.2 µs | 55.0 µs (sd 1.1) | 18,102 /s | 30.65 µs |
| DELETE by pk | A | 45,000 | 39.9 µs | 48.1 µs | 48.9 µs | 50.1 µs | 61.9 µs | 72.5 µs | 51.2 µs (sd 0.9) | 19,394 /s | 27.91 µs |
| DELETE by pk | B | 45,000 | 40.6 µs | 48.2 µs | 48.9 µs | 50.1 µs | 62.4 µs | 72.2 µs | 50.8 µs (sd 1.0) | 19,569 /s | 28.02 µs |
| COUNT/MIN/MAX over 100 ids | A | 45,000 | 722.0 µs | 776.0 µs | 799.7 µs | 823.7 µs | 853.7 µs | 991.3 µs | 814.9 µs (sd 12.1) | 1,227 /s | 758.22 µs |
| COUNT/MIN/MAX over 100 ids | B | 45,000 | 733.8 µs | 790.2 µs | 813.2 µs | 837.5 µs | 874.6 µs | 1,059.8 µs | 832.1 µs (sd 11.8) | 1,197 /s | 773.68 µs |
| SHOW META (control) | A | 45,000 | 41.8 µs | 49.4 µs | 50.1 µs | 52.5 µs | 61.3 µs | 68.7 µs | 51.7 µs (sd 0.5) | 19,216 /s | 32.90 µs |
| SHOW META (control) | B | 45,000 | 42.5 µs | 49.3 µs | 50.0 µs | 52.6 µs | 61.2 µs | 69.1 µs | 51.8 µs (sd 0.4) | 19,213 /s | 32.87 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 45,000 | 39.2 µs | 48.3 µs | 49.0 µs | 50.3 µs | 62.9 µs | 73.2 µs | 51.4 µs (sd 0.7) | 19,183 /s | 28.44 µs |
| INSERT, pk named, keys 2^40 + i (named-pair arm) | B | 45,000 | 39.6 µs | 48.5 µs | 49.1 µs | 50.5 µs | 63.2 µs | 74.1 µs | 52.0 µs (sd 1.2) | 18,866 /s | 28.63 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,492 → 20,492 /s (+0.0 % [-0.2, +0.6]) | +0.00 µs [-0.30, +0.20] | -7.47 µs [-62.95, +47.14] | +0.64 µs [-0.50, +1.12] | +86.4 % [-60.5, +128.0] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,492 /s (+0.0 % [-0.4, +0.2]) | +0.00 µs [-0.10, +0.20] | +14.89 µs [-5.27, +34.74] | +0.07 µs [-0.07, +0.28] | -3.5 % [-52.6, -0.2] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.4, +0.4]) | +0.00 µs [-0.20, +0.20] | -0.11 µs [-0.59, +0.41] | +0.06 µs [-0.18, +0.40] | +0.7 % [-1.5, +1.7] | 0.89 µs | no change resolved |
| SELECT by pk | 64.5 µs | 64.5 µs | 15,504 → 15,504 /s (+0.3 % [-0.3, +0.9]) | -0.20 µs [-0.60, +0.20] | -0.75 µs [-1.22, -0.34] | -0.21 µs [-0.59, +0.01] | +0.9 % [+0.2, +2.0] | 0.89 µs | no change resolved |
| UPDATE by pk | 52.6 µs | 52.8 µs | 19,011 → 18,939 /s (+0.4 % [-0.6, +1.1]) | -0.20 µs [-0.60, +0.30] | -0.18 µs [-0.81, +0.46] | +0.17 µs [-0.28, +0.30] | +0.1 % [-0.8, +2.0] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 51.9 µs | 19,231 → 19,268 /s (+0.6 % [-0.2, +1.3]) | -0.30 µs [-0.70, +0.10] | +0.59 µs [+0.03, +1.26] | -0.24 µs [-0.39, +0.30] | -0.5 % [-2.0, -0.1] | 0.89 µs | no change resolved |
| DELETE by pk | 48.8 µs | 48.9 µs | 20,492 → 20,450 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | -0.32 µs [-0.84, +0.30] | +0.11 µs [-0.22, +0.33] | +0.7 % [-0.0, +1.2] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 798.2 µs | 813.0 µs | 1,253 → 1,230 /s (-1.5 % [-2.0, -1.3]) | +12.10 µs [+10.60, +16.00] | +17.23 µs [+11.29, +23.28] | +15.85 µs [+12.43, +19.50] | -1.8 % [-3.6, -0.9] | 0.89 µs | **B slower** (+1.5 % at p50) |
| SHOW META (control) | 50.1 µs | 50.0 µs | 19,960 → 20,000 /s (+0.2 % [-0.4, +0.6]) | -0.10 µs [-0.20, +0.20] | +0.06 µs [-0.20, +0.36] | +0.05 µs [-0.28, +0.17] | +0.1 % [-0.5, +0.7] | 0.89 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.20 µs [+0.10, +0.20], Δ mean +0.59 µs [-0.20, +1.37], Δ server CPU per statement +0.12 µs [+0.05, +0.26]; lo p50 49.0 µs, hi p50 49.2 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.20 µs [+0.00, +0.50].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.25 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.25 µs; control SHOW META Δ p50 -0.10 µs [-0.20, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.38 µs, UPDATE 0.66 µs, SELECT 0.49 µs, ping 0.35 µs.

## Appendix D3. Copy control: the B binary's bytes in both slots (`kds_server-B`, the file run all day, and `kds_server-B2`, a copy made at 09:32 UTC), `cores = 1`

### copy control, B bytes in both slots: cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.78-8.54, after 0.92-7.53; busy of the other CPUs (6, 7 excluded) in kept runs 1.1%-2.4% (median 1.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-4%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,292 us, B 8,348 us, max A 21,039 us, B 14,413 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 38.6 µs | 48.0 µs | 48.9 µs | 50.2 µs | 61.5 µs | 75.8 µs | 88.1 µs (sd 49.2) | 17,177 /s | 28.27 µs |
| INSERT, pk omitted | B | 48,000 | 38.1 µs | 48.0 µs | 48.9 µs | 50.0 µs | 61.1 µs | 73.4 µs | 87.3 µs (sd 40.9) | 13,956 /s | 28.56 µs |
| INSERT again (replicate) | A | 48,000 | 37.8 µs | 48.0 µs | 48.8 µs | 50.2 µs | 62.4 µs | 78.1 µs | 67.5 µs (sd 34.0) | 19,262 /s | 28.30 µs |
| INSERT again (replicate) | B | 48,000 | 40.3 µs | 47.8 µs | 48.7 µs | 49.9 µs | 61.9 µs | 77.9 µs | 86.6 µs (sd 46.2) | 19,430 /s | 28.27 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 37.8 µs | 48.3 µs | 49.0 µs | 50.5 µs | 63.1 µs | 78.1 µs | 51.9 µs (sd 1.0) | 19,042 /s | 28.54 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 38.9 µs | 48.1 µs | 48.7 µs | 50.0 µs | 62.4 µs | 76.4 µs | 51.5 µs (sd 1.6) | 19,345 /s | 28.25 µs |
| SELECT by pk | A | 48,000 | 59.8 µs | 64.0 µs | 64.7 µs | 66.5 µs | 78.6 µs | 89.4 µs | 67.2 µs (sd 1.4) | 14,840 /s | 32.18 µs |
| SELECT by pk | B | 48,000 | 57.4 µs | 63.1 µs | 63.9 µs | 65.7 µs | 77.3 µs | 87.2 µs | 66.3 µs (sd 1.7) | 15,014 /s | 31.31 µs |
| UPDATE by pk | A | 48,000 | 43.2 µs | 52.1 µs | 53.0 µs | 54.8 µs | 67.6 µs | 81.7 µs | 55.9 µs (sd 1.0) | 17,617 /s | 31.38 µs |
| UPDATE by pk | B | 48,000 | 43.5 µs | 51.7 µs | 52.6 µs | 54.5 µs | 67.3 µs | 80.6 µs | 55.5 µs (sd 1.2) | 17,900 /s | 31.16 µs |
| UPDATE of one hot row | A | 48,000 | 41.7 µs | 51.4 µs | 52.2 µs | 54.3 µs | 67.1 µs | 80.4 µs | 54.5 µs (sd 1.0) | 18,277 /s | 30.89 µs |
| UPDATE of one hot row | B | 48,000 | 40.4 µs | 51.1 µs | 51.8 µs | 53.8 µs | 66.8 µs | 80.3 µs | 54.8 µs (sd 0.9) | 18,226 /s | 30.50 µs |
| DELETE by pk | A | 48,000 | 40.4 µs | 48.2 µs | 48.9 µs | 50.2 µs | 62.6 µs | 72.8 µs | 51.6 µs (sd 1.2) | 19,237 /s | 28.01 µs |
| DELETE by pk | B | 48,000 | 40.2 µs | 48.1 µs | 48.9 µs | 50.1 µs | 62.1 µs | 72.5 µs | 50.8 µs (sd 0.9) | 19,637 /s | 28.04 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 760.0 µs | 816.3 µs | 836.2 µs | 857.0 µs | 889.6 µs | 1,018.2 µs | 853.6 µs (sd 13.9) | 1,167 /s | 793.88 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 735.4 µs | 794.9 µs | 819.0 µs | 842.6 µs | 879.4 µs | 1,004.0 µs | 833.3 µs (sd 12.5) | 1,200 /s | 778.54 µs |
| SHOW META (control) | A | 48,000 | 43.9 µs | 49.7 µs | 50.4 µs | 53.1 µs | 62.1 µs | 70.5 µs | 52.3 µs (sd 0.6) | 19,043 /s | 32.92 µs |
| SHOW META (control) | B | 48,000 | 43.3 µs | 49.4 µs | 50.1 µs | 52.8 µs | 61.6 µs | 69.7 µs | 52.0 µs (sd 0.6) | 19,181 /s | 32.79 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 41.0 µs | 48.1 µs | 48.8 µs | 50.1 µs | 62.7 µs | 74.0 µs | 51.6 µs (sd 0.9) | 19,004 /s | 28.44 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.9 µs | 48.8 µs | 20,450 → 20,471 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.15] | -0.81 µs [-37.09, +33.54] | +0.23 µs [-0.30, +1.14] | +1.6 % [-55.8, +67.1] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.7 µs | 48.8 µs | 20,555 → 20,513 /s (-0.1 % [-0.3, +0.2]) | +0.05 µs [-0.10, +0.15] | +19.16 µs [-14.33, +50.95] | -0.07 µs [-0.15, +0.18] | -0.9 % [-55.9, +11.9] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.9 µs | 48.8 µs | 20,450 → 20,492 /s (+0.2 % [+0.0, +0.5]) | -0.10 µs [-0.20, +0.00] | -0.41 µs [-1.33, +0.64] | -0.28 µs [-0.52, -0.05] | +1.6 % [-0.9, +2.6] | 0.67 µs | no change resolved |
| SELECT by pk | 64.7 µs | 63.9 µs | 15,456 → 15,649 /s (+1.3 % [+0.9, +1.9]) | -0.80 µs [-1.20, -0.60] | -0.87 µs [-1.22, -0.51] | -0.90 µs [-1.20, -0.42] | +1.2 % [+0.8, +2.0] | 0.67 µs | **B faster** (-1.2 % at p50) |
| UPDATE by pk | 52.9 µs | 52.6 µs | 18,904 → 18,993 /s (+0.9 % [+0.2, +1.5]) | -0.45 µs [-0.80, -0.30] | -0.37 µs [-1.01, +0.26] | -0.31 µs [-0.60, -0.12] | +0.6 % [-0.9, +2.7] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 52.0 µs | 51.8 µs | 19,212 → 19,324 /s (+0.8 % [+0.3, +1.3]) | -0.40 µs [-0.70, -0.20] | +0.30 µs [-0.16, +0.72] | -0.35 µs [-0.66, +0.11] | -0.9 % [-1.4, +0.4] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| DELETE by pk | 48.9 µs | 49.0 µs | 20,450 → 20,408 /s (+0.0 % [-0.6, +0.2]) | +0.00 µs [-0.10, +0.30] | -0.82 µs [-1.47, -0.16] | -0.07 µs [-0.26, +0.13] | +1.1 % [+0.5, +2.9] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 834.8 µs | 818.0 µs | 1,198 → 1,222 /s (+2.1 % [+2.0, +2.4]) | -17.20 µs [-19.50, -16.40] | -20.30 µs [-26.01, -14.71] | -18.18 µs [-21.13, -16.91] | +2.3 % [+1.8, +3.2] | 0.67 µs | **B faster** (-2.1 % at p50) |
| SHOW META (control) | 50.3 µs | 50.1 µs | 19,861 → 19,960 /s (+0.6 % [+0.0, +1.0]) | -0.30 µs [-0.50, +0.00] | -0.31 µs [-0.63, -0.01] | -0.27 µs [-0.48, -0.04] | +0.5 % [+0.0, +1.1] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.22 µs, sd 0.25 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.21 µs; control SHOW META Δ p50 -0.30 µs [-0.50, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.74 µs, UPDATE 0.70 µs, SELECT 0.50 µs, ping 0.39 µs.

## Appendix D4. Copy control: the A binary's bytes in both slots (`kds_server-A` and `kds_server-A3`, a copy made at 09:32 UTC), `cores = 1`

### copy control, A bytes in both slots: cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.70-1.09, after 0.77-1.34; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-1.9% (median 1.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-2%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,714 us, B 9,444 us, max A 18,168 us, B 19,516 us

| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 48,000 | 39.3 µs | 48.0 µs | 48.8 µs | 50.1 µs | 61.9 µs | 78.0 µs | 82.3 µs (sd 40.2) | 18,768 /s | 28.26 µs |
| INSERT, pk omitted | B | 48,000 | 36.3 µs | 48.1 µs | 48.9 µs | 50.2 µs | 61.6 µs | 73.7 µs | 100.4 µs (sd 48.5) | 9,441 /s | 28.66 µs |
| INSERT again (replicate) | A | 48,000 | 40.3 µs | 48.1 µs | 48.7 µs | 50.1 µs | 62.8 µs | 77.9 µs | 54.9 µs (sd 15.8) | 19,415 /s | 28.21 µs |
| INSERT again (replicate) | B | 48,000 | 40.8 µs | 48.0 µs | 48.8 µs | 50.1 µs | 62.8 µs | 77.5 µs | 89.7 µs (sd 32.5) | 9,839 /s | 28.53 µs |
| INSERT, pk named (keys below 2^40) | A | 48,000 | 40.7 µs | 48.3 µs | 49.0 µs | 50.8 µs | 63.8 µs | 78.6 µs | 52.0 µs (sd 0.7) | 18,964 /s | 28.79 µs |
| INSERT, pk named (keys below 2^40) | B | 48,000 | 39.5 µs | 48.2 µs | 48.9 µs | 50.5 µs | 63.4 µs | 77.4 µs | 52.1 µs (sd 1.9) | 19,031 /s | 28.59 µs |
| SELECT by pk | A | 48,000 | 56.6 µs | 64.3 µs | 65.2 µs | 67.2 µs | 79.0 µs | 88.7 µs | 67.4 µs (sd 0.7) | 14,716 /s | 32.46 µs |
| SELECT by pk | B | 48,000 | 57.7 µs | 64.2 µs | 64.9 µs | 67.0 µs | 78.7 µs | 88.8 µs | 67.2 µs (sd 0.6) | 14,800 /s | 32.15 µs |
| UPDATE by pk | A | 48,000 | 43.7 µs | 52.3 µs | 53.3 µs | 55.5 µs | 68.0 µs | 81.3 µs | 56.1 µs (sd 0.9) | 17,694 /s | 31.58 µs |
| UPDATE by pk | B | 48,000 | 42.5 µs | 52.2 µs | 53.1 µs | 55.1 µs | 67.7 µs | 80.4 µs | 55.8 µs (sd 0.9) | 17,789 /s | 31.36 µs |
| UPDATE of one hot row | A | 48,000 | 39.3 µs | 51.5 µs | 52.3 µs | 54.2 µs | 67.0 µs | 79.1 µs | 54.5 µs (sd 0.5) | 18,244 /s | 30.90 µs |
| UPDATE of one hot row | B | 48,000 | 43.6 µs | 51.5 µs | 52.4 µs | 54.4 µs | 67.0 µs | 79.4 µs | 55.1 µs (sd 0.9) | 18,004 /s | 30.98 µs |
| DELETE by pk | A | 48,000 | 40.1 µs | 48.3 µs | 48.9 µs | 50.2 µs | 62.7 µs | 72.6 µs | 51.2 µs (sd 0.8) | 19,444 /s | 28.15 µs |
| DELETE by pk | B | 48,000 | 40.4 µs | 48.2 µs | 48.9 µs | 50.1 µs | 62.2 µs | 72.8 µs | 51.3 µs (sd 1.3) | 19,532 /s | 28.00 µs |
| COUNT/MIN/MAX over 100 ids | A | 48,000 | 722.8 µs | 775.5 µs | 799.2 µs | 823.4 µs | 853.9 µs | 1,037.0 µs | 818.1 µs (sd 10.3) | 1,216 /s | 760.83 µs |
| COUNT/MIN/MAX over 100 ids | B | 48,000 | 723.0 µs | 779.4 µs | 805.4 µs | 831.3 µs | 893.1 µs | 997.9 µs | 825.4 µs (sd 23.7) | 1,219 /s | 759.83 µs |
| SHOW META (control) | A | 48,000 | 42.6 µs | 49.5 µs | 50.1 µs | 52.7 µs | 62.1 µs | 70.3 µs | 52.0 µs (sd 0.4) | 19,120 /s | 32.89 µs |
| SHOW META (control) | B | 48,000 | 43.1 µs | 49.5 µs | 50.2 µs | 52.9 µs | 62.1 µs | 69.4 µs | 52.0 µs (sd 0.3) | 19,125 /s | 32.86 µs |
| INSERT, pk named, keys below 2^40 (named-pair arm) | B | 48,000 | 40.8 µs | 48.3 µs | 49.0 µs | 50.5 µs | 63.5 µs | 75.8 µs | 51.6 µs (sd 0.8) | 19,102 /s | 28.49 µs |

| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 48.8 µs | 48.8 µs | 20,492 → 20,471 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | +18.08 µs [-19.10, +55.80] | +0.70 µs [-0.52, +0.92] | -32.2 % [-55.0, +101.4] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 48.8 µs | 48.8 µs | 20,492 → 20,513 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.20] | +34.78 µs [+14.52, +53.06] | +0.07 µs [-0.06, +0.46] | -49.6 % [-55.9, -0.3] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.0 µs | 48.8 µs | 20,408 → 20,492 /s (+0.1 % [-0.1, +0.6]) | -0.05 µs [-0.30, +0.05] | +0.06 µs [-0.74, +1.24] | -0.10 µs [-0.40, +0.06] | +0.8 % [-0.3, +2.0] | 0.67 µs | no change resolved |
| SELECT by pk | 65.0 µs | 64.9 µs | 15,396 → 15,408 /s (+0.3 % [-0.2, +0.8]) | -0.20 µs [-0.50, +0.10] | -0.14 µs [-0.49, +0.21] | -0.12 µs [-0.68, +0.07] | -0.1 % [-0.3, +1.2] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.1 µs | 53.0 µs | 18,832 → 18,850 /s (+0.0 % [-0.6, +0.6]) | +0.00 µs [-0.30, +0.30] | -0.29 µs [-0.79, +0.22] | -0.09 µs [-0.27, +0.07] | -0.1 % [-0.4, +2.1] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 52.4 µs | 19,231 → 19,084 /s (-0.3 % [-0.9, +0.4]) | +0.15 µs [-0.20, +0.45] | +0.56 µs [+0.22, +0.86] | +0.07 µs [-0.02, +0.21] | -0.9 % [-1.9, -0.5] | 0.67 µs | no change resolved |
| DELETE by pk | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (-0.1 % [-0.2, +0.4]) | +0.05 µs [-0.20, +0.10] | +0.09 µs [-0.56, +0.81] | -0.06 µs [-0.37, +0.08] | +0.1 % [-0.7, +1.5] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 797.8 µs | 800.9 µs | 1,253 → 1,249 /s (+0.0 % [-1.0, +0.3]) | -0.05 µs [-2.70, +8.20] | +7.24 µs [-3.33, +20.31] | -1.05 µs [-4.90, +6.80] | -0.1 % [-2.0, +0.8] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.1 µs | 50.2 µs | 19,960 → 19,920 /s (-0.4 % [-0.4, +0.2]) | +0.20 µs [-0.10, +0.20] | -0.00 µs [-0.15, +0.14] | -0.04 µs [-0.14, +0.11] | +0.0 % [-0.5, +0.5] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.16 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.22 µs, sd 0.21 µs; control SHOW META Δ p50 +0.20 µs [-0.10, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.24 µs, UPDATE 0.80 µs, SELECT 0.46 µs, ping 0.20 µs.

## Appendix E. The null pair (the same A binary in both slots), `cores = 1`

### null pair, cores = 1, 200 rows

15 runs kept of 15 (runs 0-15); loadavg(1 min) before each run 1.28-8.73, after 1.34-8.19; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-8.6% (median 2.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-9%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,618 us, B 8,507 us, max A 14,215 us, B 44,146 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.4 µs | 49.4 µs | 20,243 → 20,243 /s (+0.2 % [+0.0, +0.6]) | -0.10 µs [-0.30, -0.10] | -0.33 µs [-0.76, +0.03] | -0.28 µs [-0.31, -0.08] | +0.5 % [+0.0, +1.3] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| INSERT again (replicate) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.4 % [+0.0, +0.6]) | -0.20 µs [-0.30, +0.00] | +1.12 µs [-0.45, +2.81] | -0.08 µs [-0.34, +0.06] | -1.7 % [-4.2, +2.6] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.4 µs | 49.3 µs | 20,243 → 20,284 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.47 µs [-1.24, +0.34] | -0.18 µs [-0.34, +0.23] | +0.6 % [-1.2, +2.7] | 0.60 µs | no change resolved |
| SELECT by pk | 65.3 µs | 65.6 µs | 15,314 → 15,244 /s (+0.0 % [-0.3, +0.9]) | +0.00 µs [-0.60, +0.20] | -0.04 µs [-0.62, +0.69] | +0.12 µs [-0.59, +0.27] | +0.5 % [-0.7, +1.0] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.6 µs | 52.5 µs | 19,011 → 19,048 /s (+0.4 % [-0.2, +1.0]) | -0.20 µs [-0.50, +0.10] | -0.05 µs [-0.51, +0.50] | -0.20 µs [-0.52, +0.04] | +0.8 % [-0.9, +1.4] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 52.2 µs | 52.3 µs | 19,157 → 19,120 /s (+0.6 % [+0.0, +0.8]) | -0.30 µs [-0.40, +0.10] | +3.00 µs [+0.06, +8.54] | -0.15 µs [-0.31, +0.06] | -0.9 % [-1.4, +0.2] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.2 µs | 20,284 → 20,325 /s (+0.4 % [-0.2, +0.6]) | -0.20 µs [-0.30, +0.10] | -0.03 µs [-0.55, +0.51] | -0.17 µs [-0.35, -0.09] | +0.1 % [-0.4, +1.4] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 86.5 µs | 86.2 µs | 11,561 → 11,601 /s (+0.2 % [-0.2, +0.5]) | -0.20 µs [-0.40, +0.20] | +0.31 µs [-0.66, +1.43] | -0.16 µs [-0.39, +0.03] | +0.0 % [-0.3, +0.8] | 0.60 µs | no change resolved |
| SHOW META (control) | 50.2 µs | 50.4 µs | 19,920 → 19,841 /s (-0.4 % [-0.6, +0.0]) | +0.20 µs [+0.00, +0.30] | +0.09 µs [-0.20, +0.39] | +0.05 µs [-0.23, +0.28] | +0.0 % [-0.7, +0.5] | 0.60 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.95 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.32 µs; control SHOW META Δ p50 +0.20 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 0.74 µs, UPDATE 0.45 µs, SELECT 0.84 µs, ping 0.42 µs.

### null pair, cores = 1, 1,000 rows

15 runs kept of 15 (runs 0-15); loadavg(1 min) before each run 1.46-3.98, after 1.67-3.82; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-4.0% (median 2.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 6 of the kept runs (range 2%-63%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 6,477 us, B 10,139 us, max A 343,345 us, B 32,404 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.4 µs | 49.2 µs | 20,243 → 20,325 /s (+0.0 % [-0.4, +0.6]) | +0.00 µs [-0.20, +0.20] | +0.91 µs [-18.42, +20.23] | -0.05 µs [-0.48, +0.07] | +0.6 % [-0.2, +1.7] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.1 µs | 49.0 µs | 20,367 → 20,408 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +12.11 µs [-2.57, +32.15] | +0.02 µs [-0.48, +0.29] | -0.0 % [-3.9, +3.7] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.4 % [-0.2, +0.6]) | -0.20 µs [-0.30, +0.10] | -0.31 µs [-0.87, +0.23] | -0.11 µs [-0.36, +0.11] | +1.1 % [-1.9, +2.0] | 0.89 µs | no change resolved |
| SELECT by pk | 68.1 µs | 67.6 µs | 14,684 → 14,793 /s (+0.7 % [-0.1, +1.2]) | -0.50 µs [-0.90, -0.30] | -0.67 µs [-1.01, -0.36] | -0.39 µs [-0.61, -0.07] | +0.8 % [+0.3, +1.5] | 0.89 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 52.9 µs | 52.3 µs | 18,904 → 19,120 /s (+1.0 % [+0.0, +1.9]) | -0.50 µs [-1.00, +0.10] | -0.17 µs [-0.92, +0.51] | -0.38 µs [-0.65, +0.08] | +0.1 % [-0.9, +1.7] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.4 µs | 51.8 µs | 19,084 → 19,305 /s (+0.8 % [+0.0, +1.7]) | -0.40 µs [-0.90, +0.00] | -1.29 µs [-4.33, +0.53] | -0.48 µs [-0.64, -0.04] | +0.5 % [-0.8, +1.4] | 0.89 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.1 µs | 20,325 → 20,367 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | +0.09 µs [-0.48, +0.90] | -0.06 µs [-0.40, +0.02] | +0.5 % [-0.3, +1.0] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 146.1 µs | 145.9 µs | 6,845 → 6,854 /s (+0.0 % [-0.6, +0.5]) | +0.00 µs [-0.70, +0.80] | -0.75 µs [-1.72, +0.11] | -0.94 µs [-1.23, +0.43] | +0.5 % [-0.4, +0.9] | 0.89 µs | no change resolved |
| SHOW META (control) | 50.4 µs | 50.2 µs | 19,841 → 19,920 /s (+0.0 % [-0.4, +0.6]) | +0.00 µs [-0.30, +0.20] | +1.01 µs [-4.03, +5.77] | -0.30 µs [-0.51, +0.61] | +0.4 % [+0.0, +0.9] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.30 µs, robust sd 0.30 µs, sd 0.41 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.54 µs; control SHOW META Δ p50 +0.00 µs [-0.30, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.25 µs, UPDATE 0.69 µs, SELECT 0.59 µs, ping 0.32 µs.

### null pair, cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 1.79-5.18, after 1.80-4.69; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-3.3% (median 2.1%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 50.4 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,596 us, B 9,280 us, max A 21,750 us, B 21,293 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.5 µs | 49.7 µs | 20,182 → 20,141 /s (+0.2 % [-0.4, +0.3]) | -0.10 µs [-0.15, +0.20] | +22.21 µs [-4.61, +48.73] | +0.70 µs [+0.43, +1.11] | -16.6 % [-55.7, +20.8] | 0.89 µs | no change resolved |
| INSERT again (replicate) | 49.6 µs | 49.5 µs | 20,161 → 20,182 /s (+0.0 % [-0.2, +0.1]) | +0.00 µs [+0.00, +0.10] | +4.03 µs [-28.71, +38.43] | -0.02 µs [-0.20, +0.37] | +1.4 % [-54.7, +99.7] | 0.89 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.5 µs | 49.5 µs | 20,202 → 20,202 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.20, +0.15] | +0.30 µs [-0.28, +0.87] | +0.01 µs [-0.21, +0.13] | -0.5 % [-2.1, +0.4] | 0.89 µs | no change resolved |
| SELECT by pk | 65.0 µs | 64.9 µs | 15,373 → 15,408 /s (-0.1 % [-0.6, +0.5]) | +0.05 µs [-0.30, +0.40] | -0.12 µs [-0.63, +0.39] | -0.08 µs [-0.26, +0.07] | -0.0 % [-0.7, +1.0] | 0.89 µs | no change resolved |
| UPDATE by pk | 52.7 µs | 52.8 µs | 18,975 → 18,939 /s (-0.4 % [-1.0, +0.6]) | +0.20 µs [-0.20, +0.50] | +0.41 µs [-0.29, +1.08] | +0.26 µs [-0.15, +0.52] | -1.0 % [-2.2, +0.8] | 0.89 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 52.0 µs | 19,231 → 19,212 /s (-0.3 % [-1.0, +0.4]) | +0.15 µs [-0.20, +0.50] | +1.29 µs [+0.71, +2.00] | +0.21 µs [-0.63, +0.51] | -1.8 % [-3.4, -0.7] | 0.89 µs | no change resolved |
| DELETE by pk | 49.5 µs | 49.5 µs | 20,222 → 20,202 /s (+0.0 % [-0.3, +0.2]) | +0.00 µs [-0.10, +0.20] | -0.33 µs [-1.24, +0.53] | +0.13 µs [-0.11, +0.25] | +0.6 % [-1.4, +2.4] | 0.89 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 799.7 µs | 804.0 µs | 1,251 → 1,244 /s (-0.7 % [-1.3, -0.4]) | +5.65 µs [+3.10, +9.10] | +14.19 µs [+5.62, +23.49] | +6.56 µs [+4.26, +8.71] | -1.1 % [-2.4, -0.9] | 0.89 µs | **B slower** (+0.7 % at p50) |
| SHOW META (control) | 50.3 µs | 50.5 µs | 19,881 → 19,802 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | -0.13 µs [-0.51, +0.16] | +0.01 µs [-0.15, +0.08] | -0.0 % [-0.4, +0.3] | 0.89 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.15 µs, sd 0.24 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.30 µs, sd 0.23 µs; control SHOW META Δ p50 +0.10 µs [-0.10, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.21 µs, UPDATE 0.74 µs, SELECT 0.71 µs, ping 0.25 µs.

## Appendix F. The null pair, `cores = 2`

### null pair, cores = 2, 200 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.46-1.05, after 0.57-1.13; busy of the other CPUs (6, 7 excluded) in kept runs 1.9%-4.1% (median 3.0%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-3%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,380 us, B 7,800 us, max A 24,341 us, B 11,962 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,346 → 20,346 /s (+0.1 % [-0.2, +0.4]) | -0.05 µs [-0.20, +0.10] | +0.01 µs [-0.29, +0.32] | -0.09 µs [-0.40, +0.23] | +0.0 % [-1.0, +1.0] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.3 µs | 49.1 µs | 20,284 → 20,367 /s (+0.2 % [-0.4, +0.5]) | -0.10 µs [-0.20, +0.20] | -0.79 µs [-1.62, -0.07] | -0.03 µs [-0.36, +0.13] | +0.6 % [-0.3, +2.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.7 µs | 49.7 µs | 20,121 → 20,141 /s (+0.2 % [-0.2, +0.9]) | -0.10 µs [-0.40, +0.15] | +0.29 µs [-0.83, +1.39] | -0.13 µs [-0.32, +0.16] | -0.7 % [-2.0, +1.1] | 0.67 µs | no change resolved |
| SELECT by pk | 66.3 µs | 66.2 µs | 15,083 → 15,094 /s (+0.2 % [-0.7, +0.6]) | -0.10 µs [-0.40, +0.50] | -0.64 µs [-1.51, +0.04] | -0.04 µs [-0.64, +0.29] | +0.6 % [-0.8, +1.4] | 0.67 µs | no change resolved |
| UPDATE by pk | 53.8 µs | 53.7 µs | 18,605 → 18,622 /s (-0.2 % [-0.6, +0.6]) | +0.10 µs [-0.30, +0.30] | +0.63 µs [-0.31, +1.44] | +0.05 µs [-0.25, +0.24] | -1.9 % [-3.2, -0.6] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 53.5 µs | 53.5 µs | 18,709 → 18,674 /s (+0.0 % [-0.6, +0.3]) | +0.00 µs [-0.15, +0.35] | +0.93 µs [+0.36, +1.45] | +0.02 µs [-0.18, +0.26] | -1.9 % [-2.7, -1.2] | 0.67 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.5 µs | 20,346 → 20,182 /s (-0.4 % [-0.7, +0.0]) | +0.20 µs [+0.00, +0.35] | -1.38 µs [-2.61, -0.38] | +0.19 µs [-0.15, +0.45] | +1.8 % [-0.3, +3.8] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 87.4 µs | 87.5 µs | 11,442 → 11,429 /s (+0.1 % [-0.8, +0.6]) | -0.05 µs [-0.50, +0.70] | +0.34 µs [-0.53, +1.24] | +0.19 µs [-0.52, +1.13] | +0.1 % [-1.0, +1.1] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.3 µs | 50.2 µs | 19,881 → 19,940 /s (+0.4 % [+0.0, +0.8]) | -0.20 µs [-0.40, +0.00] | -0.53 µs [-0.95, -0.18] | -0.19 µs [-0.53, +0.13] | +0.5 % [+0.1, +1.3] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.22 µs, sd 0.16 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.20 µs [-0.40, +0.00]; run-to-run sd of the per-run Δ p50, INSERT 0.48 µs, UPDATE 0.67 µs, SELECT 0.68 µs, ping 0.37 µs.

### null pair, cores = 2, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.36-1.14, after 0.33-1.21; busy of the other CPUs (6, 7 excluded) in kept runs 2.2%-4.3% (median 3.3%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,840 us, B 7,610 us, max A 18,680 us, B 15,695 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.2 µs | 20,346 → 20,346 /s (+0.0 % [-0.2, +0.3]) | +0.00 µs [-0.10, +0.10] | +0.01 µs [-0.27, +0.27] | -0.00 µs [-0.18, +0.19] | -0.2 % [-0.6, +0.4] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.2 µs | 20,346 → 20,325 /s (-0.2 % [-0.4, +0.4]) | +0.10 µs [-0.20, +0.20] | -0.20 µs [-0.75, +0.39] | +0.08 µs [-0.21, +0.23] | +0.7 % [-1.0, +2.4] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.7 µs | 49.7 µs | 20,141 → 20,141 /s (+0.1 % [-0.8, +0.4]) | -0.05 µs [-0.25, +0.40] | -0.09 µs [-0.96, +0.86] | -0.05 µs [-0.30, +0.15] | +1.0 % [-1.8, +2.0] | 0.60 µs | no change resolved |
| SELECT by pk | 68.5 µs | 68.2 µs | 14,599 → 14,674 /s (+0.1 % [-0.3, +1.0]) | -0.10 µs [-0.70, +0.20] | -0.61 µs [-1.11, -0.09] | -0.06 µs [-0.70, +0.28] | +0.8 % [-0.2, +1.7] | 0.60 µs | no change resolved |
| UPDATE by pk | 53.8 µs | 53.9 µs | 18,587 → 18,553 /s (+0.1 % [-0.6, +0.4]) | -0.05 µs [-0.20, +0.30] | -0.08 µs [-0.63, +0.49] | -0.07 µs [-0.35, +0.16] | +0.5 % [-1.0, +0.9] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 53.3 µs | 53.5 µs | 18,762 → 18,692 /s (+0.0 % [-0.7, +0.3]) | +0.00 µs [-0.10, +0.30] | +0.95 µs [+0.31, +1.54] | +0.17 µs [-0.09, +0.48] | -1.7 % [-3.2, +0.0] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.40] | -0.77 µs [-1.48, -0.11] | +0.09 µs [-0.20, +0.48] | +0.5 % [-0.1, +3.4] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 147.7 µs | 147.4 µs | 6,770 → 6,784 /s (-0.0 % [-0.3, +0.5]) | +0.05 µs [-0.80, +0.50] | +0.16 µs [-0.89, +1.21] | +0.15 µs [-0.46, +0.92] | -0.0 % [-0.7, +0.7] | 0.60 µs | no change resolved |
| SHOW META (control) | 50.3 µs | 50.2 µs | 19,861 → 19,920 /s (+0.6 % [-0.2, +1.0]) | -0.30 µs [-0.50, +0.10] | -0.28 µs [-1.36, +0.77] | -0.17 µs [-0.40, +0.18] | +0.4 % [-0.6, +1.3] | 0.60 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; control SHOW META Δ p50 -0.30 µs [-0.50, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.30 µs, UPDATE 0.50 µs, SELECT 0.56 µs, ping 0.48 µs.

### null pair, cores = 2, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.48-1.07, after 0.56-1.20; busy of the other CPUs (6, 7 excluded) in kept runs 2.2%-4.5% (median 2.6%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-4%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,585 us, B 7,882 us, max A 15,018 us, B 24,706 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.0 µs | 49.3 µs | 20,408 → 20,284 /s (+0.0 % [-1.4, +0.4]) | +0.00 µs [-0.20, +0.70] | +19.60 µs [-0.31, +39.57] | +0.72 µs [+0.14, +1.36] | -29.7 % [-41.8, +4.0] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.1 µs | 49.1 µs | 20,367 → 20,367 /s (-0.2 % [-0.6, +0.0]) | +0.10 µs [+0.00, +0.30] | +1.74 µs [-1.34, +6.48] | +0.01 µs [-0.33, +0.26] | +0.2 % [-0.8, +1.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.8 µs | 50.0 µs | 20,060 → 19,980 /s (-0.2 % [-0.8, +0.7]) | +0.10 µs [-0.20, +0.40] | -0.40 µs [-1.88, +0.56] | -0.12 µs [-0.25, +0.22] | -0.3 % [-1.3, +0.7] | 0.67 µs | no change resolved |
| SELECT by pk | 65.8 µs | 65.7 µs | 15,198 → 15,232 /s (+0.1 % [-0.3, +0.5]) | -0.05 µs [-0.30, +0.20] | -0.29 µs [-0.78, +0.22] | -0.40 µs [-0.56, +0.07] | +0.3 % [-0.1, +1.1] | 0.67 µs | no change resolved |
| UPDATE by pk | 54.0 µs | 54.2 µs | 18,501 → 18,450 /s (-0.6 % [-0.9, +0.0]) | +0.30 µs [+0.00, +0.50] | +0.18 µs [-0.47, +0.94] | +0.15 µs [-0.12, +0.20] | -0.1 % [-1.1, +1.3] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 53.3 µs | 53.5 µs | 18,762 → 18,674 /s (-0.5 % [-1.3, +0.5]) | +0.25 µs [-0.25, +0.70] | +1.58 µs [+0.56, +3.01] | +0.30 µs [+0.05, +0.61] | -2.0 % [-2.9, -0.9] | 0.67 µs | no change resolved |
| DELETE by pk | 49.2 µs | 49.5 µs | 20,325 → 20,202 /s (-0.5 % [-1.6, -0.1]) | +0.25 µs [+0.05, +0.80] | +0.15 µs [-0.23, +0.53] | +0.27 µs [+0.00, +0.51] | -0.3 % [-1.3, +0.7] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| COUNT/MIN/MAX over 100 ids | 815.5 µs | 813.8 µs | 1,226 → 1,229 /s (+0.2 % [-0.2, +0.5]) | -1.55 µs [-3.70, +1.40] | -0.12 µs [-6.21, +6.17] | -0.81 µs [-3.95, +1.72] | +0.1 % [-0.5, +1.1] | 0.67 µs | no change resolved |
| SHOW META (control) | 50.2 µs | 50.4 µs | 19,920 → 19,841 /s (-0.2 % [-0.5, +0.0]) | +0.10 µs [+0.00, +0.20] | +1.05 µs [+0.09, +2.26] | +0.12 µs [-0.28, +0.37] | -0.5 % [-1.3, +0.2] | 0.67 µs | no change resolved |

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.15 µs, robust sd 0.22 µs, sd 0.22 µs; replicate arm B (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.22 µs, sd 0.40 µs; control SHOW META Δ p50 +0.10 µs [+0.00, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.62 µs, UPDATE 0.45 µs, SELECT 0.53 µs, ping 0.37 µs.

## Appendix G. `cores = 1`, pass 1 alone (runs 0-15)

### pass 1, cores = 1, 200 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.82-1.27, after 0.89-1.27; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-2.4% (median 1.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.5 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,782 us, B 7,966 us, max A 16,928 us, B 16,322 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.3 µs | 49.2 µs | 20,284 → 20,305 /s (+0.0 % [-0.4, +0.2]) | +0.00 µs [-0.10, +0.20] | -0.04 µs [-0.31, +0.28] | -0.24 µs [-0.31, +0.01] | +0.5 % [-0.7, +0.7] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.3 µs | 20,305 → 20,284 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.20] | +0.23 µs [-0.44, +0.90] | -0.06 µs [-0.22, +0.05] | -0.5 % [-2.6, +1.5] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.2 µs | 20,325 → 20,325 /s (+0.0 % [+0.0, +0.2]) | +0.00 µs [-0.10, +0.00] | +0.12 µs [-0.52, +0.81] | -0.16 µs [-0.29, +0.03] | -0.1 % [-1.5, +2.0] | 0.60 µs | no change resolved |
| SELECT by pk | 65.5 µs | 65.1 µs | 15,267 → 15,361 /s (+0.2 % [-0.2, +1.4]) | -0.10 µs [-0.90, +0.10] | -0.36 µs [-0.82, +0.09] | -0.11 µs [-0.48, -0.03] | +0.1 % [-0.1, +2.1] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.3 µs | 52.1 µs | 19,120 → 19,194 /s (+0.6 % [+0.0, +0.8]) | -0.30 µs [-0.40, +0.00] | -0.22 µs [-0.75, +0.39] | -0.36 µs [-0.62, -0.07] | +0.5 % [-0.7, +1.3] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 52.1 µs | 51.9 µs | 19,194 → 19,268 /s (+0.4 % [+0.0, +0.6]) | -0.20 µs [-0.35, +0.00] | +0.62 µs [+0.14, +1.08] | -0.28 µs [-0.45, +0.14] | -1.4 % [-2.5, -0.1] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.2 µs | 20,284 → 20,305 /s (+0.3 % [-0.6, +0.6]) | -0.15 µs [-0.30, +0.30] | +0.03 µs [-0.27, +0.36] | +0.05 µs [-0.16, +0.11] | +0.1 % [-0.8, +0.8] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 86.5 µs | 87.1 µs | 11,554 → 11,481 /s (-0.7 % [-1.2, -0.2]) | +0.65 µs [+0.20, +1.10] | +0.43 µs [-0.16, +0.94] | +0.46 µs [+0.02, +1.19] | -0.4 % [-1.6, +0.2] | 0.60 µs | **B slower** (+0.8 % at p50) |
| SHOW META (control) | 50.5 µs | 50.4 µs | 19,782 → 19,841 /s (+0.0 % [-0.2, +1.0]) | +0.00 µs [-0.50, +0.10] | -0.20 µs [-0.54, +0.10] | -0.14 µs [-0.62, +0.02] | +0.1 % [-0.2, +0.6] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.10], Δ mean +0.28 µs [-0.56, +1.06], Δ server CPU per statement +0.14 µs [+0.03, +0.20]; lo p50 49.2 µs, hi p50 49.2 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.00 µs [-0.10, +0.10].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.20 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.07 µs, sd 0.16 µs; control SHOW META Δ p50 +0.00 µs [-0.50, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.23 µs, UPDATE 0.35 µs, SELECT 0.59 µs, ping 0.43 µs.

### pass 1, cores = 1, 1,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.92-1.22, after 1.01-1.28; busy of the other CPUs (6, 7 excluded) in kept runs 1.5%-3.3% (median 1.9%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,018 us, B 7,843 us, max A 15,380 us, B 21,476 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.2 µs | 49.3 µs | 20,325 → 20,263 /s (-0.2 % [-0.4, +0.0]) | +0.10 µs [+0.00, +0.20] | -0.06 µs [-0.35, +0.26] | +0.01 µs [-0.34, +0.16] | +0.2 % [-0.4, +0.5] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 49.3 µs | 49.4 µs | 20,284 → 20,243 /s (-0.1 % [-0.4, +0.2]) | +0.05 µs [-0.10, +0.20] | +0.33 µs [-0.40, +1.12] | -0.11 µs [-0.32, +0.20] | -0.3 % [-2.4, +1.6] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (+0.0 % [-0.2, +0.2]) | +0.00 µs [-0.10, +0.10] | +0.23 µs [-0.78, +1.44] | -0.25 µs [-0.36, +0.10] | +1.5 % [-3.1, +2.7] | 0.60 µs | no change resolved |
| SELECT by pk | 67.5 µs | 67.1 µs | 14,826 → 14,903 /s (+0.8 % [+0.1, +1.1]) | -0.55 µs [-0.75, -0.10] | -0.56 µs [-0.90, -0.20] | -0.49 µs [-0.99, -0.26] | +0.6 % [+0.4, +1.6] | 0.60 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 52.6 µs | 52.3 µs | 19,011 → 19,102 /s (+0.1 % [-0.4, +1.1]) | -0.05 µs [-0.55, +0.20] | +0.38 µs [-0.08, +0.79] | -0.16 µs [-0.62, +0.21] | -1.0 % [-1.8, +0.3] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 52.0 µs | 19,212 → 19,231 /s (+0.2 % [-0.6, +0.8]) | -0.10 µs [-0.40, +0.30] | +1.08 µs [+0.32, +2.01] | -0.03 µs [-0.54, +0.36] | -1.2 % [-2.9, -0.6] | 0.60 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.4 µs | 20,263 → 20,243 /s (+0.0 % [-0.4, +0.2]) | +0.00 µs [-0.10, +0.20] | +0.04 µs [-0.43, +0.57] | +0.05 µs [-0.25, +0.29] | +0.4 % [-0.5, +0.9] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 145.4 µs | 147.1 µs | 6,875 → 6,798 /s (-1.2 % [-1.4, -0.9]) | +1.75 µs [+1.30, +2.20] | +1.81 µs [+0.91, +2.68] | +1.73 µs [+1.16, +2.32] | -1.4 % [-1.7, -0.8] | 0.75 µs | **B slower** (+1.2 % at p50) |
| SHOW META (control) | 50.3 µs | 50.4 µs | 19,881 → 19,841 /s (-0.1 % [-0.5, +0.1]) | +0.05 µs [+0.00, +0.20] | +0.12 µs [-0.14, +0.40] | -0.01 µs [-0.20, +0.31] | -0.1 % [-1.0, +0.7] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.10], Δ mean +0.50 µs [-0.12, +1.09], Δ server CPU per statement +0.12 µs [+0.02, +0.19]; lo p50 49.3 µs, hi p50 49.5 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.20 µs [+0.00, +0.20].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.15 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.14 µs; control SHOW META Δ p50 +0.05 µs [+0.00, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.21 µs, UPDATE 0.75 µs, SELECT 0.67 µs, ping 0.33 µs.

### pass 1, cores = 1, 10,000 rows

16 runs kept of 16 (runs 0-15); loadavg(1 min) before each run 0.96-1.43, after 1.14-1.56; busy of the other CPUs (6, 7 excluded) in kept runs 1.4%-2.1% (median 1.7%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 1%-3%); gate wait median 3 s; ping p50 reference 50.3 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 8,843 us, B 10,744 us, max A 48,940 us, B 21,161 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.3 µs | 49.3 µs | 20,284 → 20,284 /s (+0.1 % [-0.1, +0.3]) | -0.05 µs [-0.15, +0.05] | +10.94 µs [-38.63, +61.44] | +0.68 µs [+0.07, +1.12] | -8.9 % [-70.5, +151.6] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.2 µs | 49.2 µs | 20,325 → 20,346 /s (+0.2 % [-0.2, +0.2]) | -0.10 µs [-0.10, +0.10] | +1.76 µs [-12.54, +15.31] | +0.02 µs [-0.19, +0.16] | +0.2 % [-0.3, +1.6] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.2 µs | 49.2 µs | 20,325 → 20,325 /s (+0.0 % [-0.2, +0.1]) | +0.00 µs [-0.05, +0.10] | +0.26 µs [-0.52, +0.93] | +0.02 µs [-0.06, +0.26] | -1.5 % [-2.6, -0.1] | 0.67 µs | no change resolved |
| SELECT by pk | 64.9 µs | 64.6 µs | 15,408 → 15,480 /s (+0.7 % [+0.0, +1.2]) | -0.45 µs [-0.80, +0.00] | -0.54 µs [-0.89, -0.15] | -0.21 µs [-0.73, +0.08] | +0.8 % [+0.2, +1.6] | 0.67 µs | no change resolved |
| UPDATE by pk | 52.6 µs | 52.6 µs | 18,993 → 18,993 /s (+0.1 % [-1.1, +0.8]) | -0.05 µs [-0.45, +0.60] | -0.56 µs [-1.29, +0.11] | +0.00 µs [-0.58, +0.38] | +0.8 % [-0.9, +2.7] | 0.67 µs | no change resolved |
| UPDATE of one hot row | 52.0 µs | 51.9 µs | 19,231 → 19,268 /s (+0.2 % [-1.0, +1.0]) | -0.10 µs [-0.50, +0.50] | +0.54 µs [-0.05, +1.28] | -0.01 µs [-0.41, +0.49] | -0.9 % [-1.4, +0.8] | 0.67 µs | no change resolved |
| DELETE by pk | 49.3 µs | 49.2 µs | 20,284 → 20,325 /s (+0.2 % [+0.0, +0.4]) | -0.10 µs [-0.20, +0.00] | -0.05 µs [-0.50, +0.36] | +0.13 µs [-0.25, +0.27] | -0.1 % [-0.9, +1.1] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 804.2 µs | 817.9 µs | 1,243 → 1,223 /s (-1.7 % [-2.1, -0.8]) | +14.05 µs [+7.30, +15.80] | +16.84 µs [+7.95, +25.76] | +14.27 µs [+7.45, +17.42] | -1.5 % [-3.0, -1.0] | 5.65 µs | **B slower** (+1.7 % at p50) |
| SHOW META (control) | 50.3 µs | 50.3 µs | 19,861 → 19,881 /s (+0.2 % [-0.2, +0.8]) | -0.10 µs [-0.40, +0.10] | -0.27 µs [-0.52, -0.01] | -0.06 µs [-0.34, +0.28] | +0.3 % [-0.2, +1.4] | 0.67 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.20], Δ mean +0.42 µs [-0.40, +1.32], Δ server CPU per statement +0.09 µs [+0.02, +0.14]; lo p50 49.2 µs, hi p50 49.3 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.15 µs [-0.05, +0.30].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.10 µs, robust sd 0.22 µs, sd 0.18 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.14 µs; control SHOW META Δ p50 -0.10 µs [-0.40, +0.10]; run-to-run sd of the per-run Δ p50, INSERT 0.23 µs, UPDATE 0.72 µs, SELECT 0.68 µs, ping 0.36 µs.

## Appendix H. `cores = 1`, pass 2 alone (runs 16-31)

### pass 2, cores = 1, 200 rows

16 runs kept of 16 (runs 16-31); loadavg(1 min) before each run 1.19-2.01, after 1.34-2.09; busy of the other CPUs (6, 7 excluded) in kept runs 1.9%-3.2% (median 2.4%); CPUs 6,7 (another session's benchmark) busy over 10 % in 8 of the kept runs (range 1%-61%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 5,712 us, B 8,116 us, max A 341,400 us, B 320,648 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.1 µs | 49.0 µs | 20,367 → 20,387 /s (+0.3 % [-0.2, +0.4]) | -0.15 µs [-0.25, +0.10] | +4.38 µs [-4.12, +18.01] | -0.19 µs [-0.37, -0.11] | +0.8 % [+0.2, +1.6] | 1.33 µs | no change resolved |
| INSERT again (replicate) | 49.0 µs | 49.0 µs | 20,429 → 20,408 /s (+0.2 % [+0.0, +0.6]) | -0.10 µs [-0.30, +0.05] | +0.01 µs [-1.41, +1.71] | -0.09 µs [-0.27, -0.02] | +1.0 % [-0.6, +3.0] | 1.33 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 49.1 µs | 49.0 µs | 20,367 → 20,429 /s (+0.4 % [+0.2, +0.7]) | -0.20 µs [-0.35, -0.10] | -0.48 µs [-0.88, -0.12] | -0.26 µs [-0.46, -0.12] | +0.9 % [-0.0, +1.7] | 1.33 µs | CI excludes 0, inside the floor: not resolved |
| SELECT by pk | 65.2 µs | 64.9 µs | 15,326 → 15,408 /s (+0.9 % [+0.2, +1.1]) | -0.60 µs [-0.70, -0.10] | -0.51 µs [-0.83, -0.18] | -0.63 µs [-0.82, -0.36] | +0.8 % [+0.1, +1.5] | 1.33 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE by pk | 52.3 µs | 51.7 µs | 19,120 → 19,342 /s (+1.3 % [+0.6, +1.7]) | -0.70 µs [-0.85, -0.25] | -0.47 µs [-0.94, +0.04] | -0.51 µs [-0.84, -0.22] | +1.1 % [+0.4, +1.9] | 1.33 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 52.0 µs | 51.5 µs | 19,212 → 19,417 /s (+1.4 % [+0.6, +1.8]) | -0.70 µs [-0.90, -0.30] | +0.21 µs [-0.59, +1.14] | -0.66 µs [-0.92, -0.30] | +0.1 % [-1.4, +1.1] | 1.33 µs | CI excludes 0, inside the floor: not resolved |
| DELETE by pk | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (+0.2 % [-0.2, +0.4]) | -0.10 µs [-0.20, +0.10] | -0.37 µs [-0.83, +0.04] | -0.02 µs [-0.23, +0.21] | +0.2 % [-0.5, +0.9] | 1.33 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 86.2 µs | 86.7 µs | 11,601 → 11,541 /s (-0.7 % [-1.0, +0.0]) | +0.65 µs [+0.00, +0.90] | -0.19 µs [-0.81, +0.42] | +0.14 µs [-0.29, +0.86] | -0.0 % [-0.9, +1.3] | 1.33 µs | no change resolved |
| SHOW META (control) | 50.1 µs | 50.3 µs | 19,960 → 19,881 /s (-0.2 % [-0.6, +0.0]) | +0.10 µs [+0.00, +0.30] | -0.34 µs [-1.48, +0.32] | -0.03 µs [-0.54, +0.18] | -0.4 % [-0.6, +0.0] | 1.33 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.20], Δ mean -0.11 µs [-1.11, +0.77], Δ server CPU per statement +0.08 µs [-0.13, +0.19]; lo p50 49.0 µs, hi p50 49.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.00 µs [-0.30, +0.10].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.05 µs, robust sd 0.44 µs, sd 0.36 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.44 µs, sd 0.47 µs; control SHOW META Δ p50 +0.10 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 0.38 µs, UPDATE 0.56 µs, SELECT 0.51 µs, ping 0.33 µs.

### pass 2, cores = 1, 1,000 rows

16 runs kept of 16 (runs 16-31); loadavg(1 min) before each run 0.74-2.65, after 0.84-2.76; busy of the other CPUs (6, 7 excluded) in kept runs 1.7%-4.2% (median 2.5%); CPUs 6,7 (another session's benchmark) busy over 10 % in 5 of the kept runs (range 2%-60%); gate wait median 3 s; ping p50 reference 50.1 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 7,650 us, B 6,144 us, max A 242,612 us, B 140,789 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.0 µs | 48.9 µs | 20,429 → 20,450 /s (+0.1 % [-0.2, +0.4]) | -0.05 µs [-0.20, +0.10] | -6.69 µs [-31.94, +19.35] | -0.12 µs [-0.21, -0.03] | +0.6 % [-0.2, +6.6] | 0.67 µs | no change resolved |
| INSERT again (replicate) | 49.0 µs | 49.0 µs | 20,408 → 20,408 /s (+0.0 % [-0.4, +0.2]) | +0.00 µs [-0.10, +0.20] | -5.33 µs [-14.49, +0.40] | -0.08 µs [-0.13, +0.14] | +0.5 % [-0.8, +1.8] | 0.67 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.9 µs | 48.9 µs | 20,450 → 20,450 /s (+0.0 % [-0.2, +0.4]) | +0.00 µs [-0.20, +0.10] | +0.14 µs [-0.47, +0.82] | +0.02 µs [-0.12, +0.08] | +0.1 % [-1.2, +1.5] | 0.67 µs | no change resolved |
| SELECT by pk | 67.9 µs | 67.3 µs | 14,728 → 14,859 /s (+0.6 % [-0.0, +1.5]) | -0.40 µs [-1.00, +0.00] | -0.76 µs [-1.16, -0.35] | -0.47 µs [-0.77, -0.26] | +0.9 % [+0.7, +1.6] | 0.67 µs | no change resolved |
| UPDATE by pk | 52.5 µs | 52.0 µs | 19,066 → 19,249 /s (+1.0 % [+0.2, +1.4]) | -0.50 µs [-0.75, -0.10] | -0.37 µs [-0.93, +0.24] | -0.39 µs [-0.57, -0.13] | +0.7 % [-0.1, +1.8] | 0.67 µs | CI excludes 0, inside the floor: not resolved |
| UPDATE of one hot row | 52.0 µs | 51.6 µs | 19,212 → 19,380 /s (+0.8 % [+0.0, +1.6]) | -0.40 µs [-0.80, +0.00] | +0.22 µs [-0.36, +0.88] | -0.36 µs [-0.76, +0.38] | -0.0 % [-1.3, +1.3] | 0.67 µs | no change resolved |
| DELETE by pk | 49.0 µs | 48.8 µs | 20,429 → 20,471 /s (-0.1 % [-0.3, +0.2]) | +0.05 µs [-0.10, +0.15] | +0.09 µs [-0.38, +0.60] | +0.05 µs [-0.33, +0.17] | +0.1 % [-0.9, +1.1] | 0.67 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 145.7 µs | 147.2 µs | 6,863 → 6,793 /s (-1.1 % [-1.3, -0.7]) | +1.55 µs [+1.10, +1.90] | +0.12 µs [-4.81, +4.86] | +1.27 µs [+0.76, +1.94] | -0.8 % [-2.0, +0.4] | 0.75 µs | **B slower** (+1.1 % at p50) |
| SHOW META (control) | 50.1 µs | 50.2 µs | 19,960 → 19,900 /s (-0.3 % [-0.6, +0.0]) | +0.15 µs [+0.00, +0.30] | +0.94 µs [-0.01, +2.58] | +0.23 µs [+0.03, +0.59] | -0.4 % [-1.0, +0.0] | 0.67 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.00, +0.20], Δ mean +0.38 µs [-0.92, +1.36], Δ server CPU per statement +0.07 µs [-0.01, +0.14]; lo p50 48.8 µs, hi p50 48.8 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.10 µs [-0.20, +0.20].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median +0.10 µs, robust sd 0.15 µs, sd 0.32 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.05 µs, robust sd 0.22 µs, sd 0.25 µs; control SHOW META Δ p50 +0.15 µs [+0.00, +0.30]; run-to-run sd of the per-run Δ p50, INSERT 0.20 µs, UPDATE 0.55 µs, SELECT 0.85 µs, ping 0.33 µs.

### pass 2, cores = 1, 10,000 rows

16 runs kept of 16 (runs 16-31); loadavg(1 min) before each run 1.02-1.72, after 1.24-1.91; busy of the other CPUs (6, 7 excluded) in kept runs 1.8%-4.1% (median 2.4%); CPUs 6,7 (another session's benchmark) busy over 10 % in 0 of the kept runs (range 2%-7%); gate wait median 3 s; ping p50 reference 50.2 us; errors in kept runs: 0

`recovery_checkpoint_us` at mount (thermometer), median A 9,102 us, B 9,380 us, max A 23,742 us, B 22,580 us


| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.0 µs | 48.9 µs | 20,408 → 20,450 /s (+0.1 % [-0.2, +0.3]) | -0.05 µs [-0.15, +0.10] | -7.84 µs [-46.23, +29.69] | -0.18 µs [-0.48, +0.45] | +2.1 % [-10.5, +83.5] | 0.60 µs | no change resolved |
| INSERT again (replicate) | 48.9 µs | 49.0 µs | 20,450 → 20,408 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | +30.07 µs [-9.17, +68.96] | +0.09 µs [-0.18, +0.38] | -23.4 % [-48.1, +0.4] | 0.60 µs | no change resolved |
| INSERT, pk named (keys below 2^40) | 48.8 µs | 48.9 µs | 20,471 → 20,450 /s (-0.2 % [-0.4, +0.1]) | +0.10 µs [-0.05, +0.20] | +0.52 µs [-0.02, +1.07] | +0.04 µs [-0.09, +0.50] | -1.1 % [-2.4, +0.5] | 0.60 µs | no change resolved |
| SELECT by pk | 65.0 µs | 64.7 µs | 15,396 → 15,468 /s (+0.2 % [-0.2, +1.1]) | -0.10 µs [-0.75, +0.10] | -0.50 µs [-0.84, -0.17] | -0.59 µs [-0.83, +0.11] | +0.7 % [+0.1, +1.4] | 0.60 µs | no change resolved |
| UPDATE by pk | 52.5 µs | 52.5 µs | 19,048 → 19,066 /s (+0.4 % [-0.4, +1.0]) | -0.20 µs [-0.50, +0.20] | -0.18 µs [-0.97, +0.64] | +0.12 µs [-0.42, +0.45] | +0.2 % [-1.7, +2.6] | 0.60 µs | no change resolved |
| UPDATE of one hot row | 51.8 µs | 51.7 µs | 19,305 → 19,361 /s (+0.2 % [-0.1, +0.9]) | -0.10 µs [-0.40, +0.05] | +0.62 µs [+0.12, +1.10] | -0.20 µs [-0.50, +0.28] | -0.9 % [-1.8, -0.1] | 0.60 µs | no change resolved |
| DELETE by pk | 48.9 µs | 49.0 µs | 20,450 → 20,408 /s (-0.1 % [-0.4, +0.4]) | +0.05 µs [-0.20, +0.20] | +1.38 µs [-0.41, +4.36] | +0.13 µs [-0.12, +0.34] | -0.2 % [-1.3, +0.3] | 0.60 µs | no change resolved |
| COUNT/MIN/MAX over 100 ids | 806.1 µs | 820.4 µs | 1,241 → 1,219 /s (-1.8 % [-3.0, -1.5]) | +14.75 µs [+11.70, +24.90] | +18.14 µs [+6.88, +31.40] | +15.92 µs [+10.52, +23.43] | -1.7 % [-2.8, -1.2] | 5.65 µs | **B slower** (+1.8 % at p50) |
| SHOW META (control) | 50.0 µs | 50.2 µs | 19,980 → 19,920 /s (-0.2 % [-0.4, +0.2]) | +0.10 µs [-0.10, +0.20] | -0.14 µs [-0.49, +0.19] | -0.04 µs [-0.50, +0.21] | +0.2 % [-0.6, +1.0] | 0.60 µs | no change resolved |

Named-pair arm, B only, same blocks (hi - lo): Δ p50 +0.10 µs [+0.10, +0.20], Δ mean -0.24 µs [-0.86, +0.62], Δ server CPU per statement +0.16 µs [+0.06, +0.27]; lo p50 48.8 µs, hi p50 49.0 µs.
B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 +0.20 µs [-0.10, +0.30].

Noise floor from inside the runs: replicate arm A (INSERT again − INSERT p50 per run) median -0.15 µs, robust sd 0.15 µs, sd 0.49 µs; replicate arm B (INSERT again − INSERT p50 per run) median +0.00 µs, robust sd 0.15 µs, sd 0.40 µs; control SHOW META Δ p50 +0.10 µs [-0.10, +0.20]; run-to-run sd of the per-run Δ p50, INSERT 0.28 µs, UPDATE 0.58 µs, SELECT 0.51 µs, ping 0.33 µs.

## Appendix: discarded runs and the rules

**Rules, declared before the first run unless marked** (`archive/.../drivers/analyze_bk.py`, docstring). A finished run is
discarded if, in its own record: **(1)** a real `cc1plus`, `cmake`, `make`, `ninja` or `ld` process is in its before or after list
(*amended*: a foreign `ctest` or `kds_tests` process no longer discards on its own - the first is a sleeping parent, the second one test - and the CPU rules see their load); **(2)** the CPUs outside the server's, the driver's and 6,7 averaged more than 10 % busy
(*amended during the run*: it first counted CPUs 6,7, and at `cores = 2` counted CPU 1, which the engine pins its second
reactor to); **(2b)** a hyperthread sibling of a pinned CPU (3 and 5 at `cores = 1`; 0 and 5 at `cores = 2`, amended with
rule 2) was more than 15 % busy; **(3)** any error reply, a row count other than the expected one, or a refusal probe other
than the expected one; **(4)** the `SHOW META` control's p50 in either arm exceeds 1.04 x the cell's median (*amended*: it first
read 1.10 x, and no kept run of `cores = 1` or `cores = 2` moves between the two values). Discarded runs are moved aside with their reasons, not deleted, and the same run index is run again, up to six
times; re-running a discarded index keeps the A/B start order, which alternates by index parity. The discards below were
applied by rules that read only the host and the control, never a statement's latency. A dry run of the final rules over every kept run
of every cell discards none of them (the final 1.04 rule moved two runs, one of the `cores = 1` null pair and one of the fresh pair, which were not
re-run: those cells hold 15 runs where 16 were asked). Runs discarded under an earlier form of a rule were re-run at the same index and the re-run is the one kept; the earlier
record stays under `discarded/` with its reasons.

**`overhead-c1`**: 96 runs kept, 0 discarded.

**`overhead-c2`**: 48 runs kept, 33 discarded.
- 18 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 13 x `other-CPU busy #% > # %`
- 7 x `host_after`
- 5 x `host_before`
- 5 x `sibling CPU # busy #% > # %`
- 2 x `A ping p# # > # x #`
- 2 x `B ping p# # > # x #`

**`null-c1`**: 46 runs kept, 24 discarded.
- 25 x `sibling CPU # busy #% > # %`
- 19 x `host_after`
- 17 x `host_before`
- 17 x `other-CPU busy #% > # %`
- 15 x `A ping p# # > # x #`
- 12 x `B ping p# # > # x #`
- 1 x `other-CPU busy (CPUs #,# excluded) #% > # %`

**`null-c2`**: 48 runs kept, 2 discarded.
- 2 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 1 x `sibling CPU # busy #% > # %`

**`strict-c1`**: 18 runs kept, 18 discarded.
- 20 x `sibling CPU # busy #% > # %`
- 16 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 3 x `host_after`
- 3 x `host_before`
- 1 x `B ping p# # > # x #`

**`flip-c1`**: 29 runs kept, 5 discarded.
- 4 x `A ping p# # > # x #`
- 3 x `B ping p# # > # x #`
- 2 x `sibling CPU # busy #% > # %`
- 1 x `host_after`
- 1 x `other-CPU busy (CPUs #,# excluded) #% > # %`

**`nullB-c1`**: 16 runs kept, 0 discarded.

**`nullA-c1`**: 16 runs kept, 0 discarded.

**`fresh-c1`**: 31 runs kept, 1 discarded.
- 1 x `A ping p# # > # x #`
- 1 x `B ping p# # > # x #`
