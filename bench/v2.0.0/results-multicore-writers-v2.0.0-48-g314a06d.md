# The per-core writer at equal parallelism — PW6 on a two-CPU host

**The number PW6's row asks for — a speedup from two writer cores — could not
be measured on this host, and this file says so first.** The server refuses
`cores = 3` on a machine that reports two CPUs, and at `cores = 2` rotation
skips the system core, so every rotated relation is core 1's. What *can* run
is the comparison the driver prints a warning about: the peer write path
(core 1, sessions the kernel accepted on core 1's own listener) against core
0's, at the same number of writers. That is a cost, not a scaling number, and
here it is: **for two writers the peer path costs nothing this harness can
resolve** — 0.977× the single-core configuration against a control that
cannot differ and measured 0.982×, with a ±5% floor between them — and every
per-statement median lands within 2% of core 0's. **For four writers the peer
configuration loses 20%, and none of it is in the write path.** Two of the
four relations wait 0.5–1.75 s for their *first* INSERT — the servers' own
logs, read after the run (§6a), name the wait: the **row-id lease refill**
for the third and fourth relations, a ring round trip that took one to two
seconds, with the trx-id lease spent beside it — and
the 64-page extent lease is spent mid-run and refused with a message that
promises a retry but carries no `retryable=` bit, so the driver does not
retry it and **1, 13 and 51 INSERTs per run were lost**. The stalls
desynchronise the four client threads, which exposes a third thing that is
not the peer's at all: **a point-SELECT on a core with a committing session
waits out that session's fsync** — 973 µs at p50 on core 0 and 962 µs on
core 1, against 37 and 35 µs alone — because the WAL drain and its
`fdatasync` run inline on the reactor thread.

The workload is `tools/multicore_benchmark.py`'s: N relations × M rows, one
connection per relation, INSERT / point-SELECT by pk / UPDATE by pk / DELETE
the odd half / one scan, each configuration a fresh server on a fresh data
file. The matrix, the PostgreSQL twin, the probes and the report are
`bench/run_pw6.py`. How to run both: `bench/docs/README.md`.

## 1. The run

| | |
|---|---|
| executed | **2026-08-25 00:44:17 → 01:04:42 UTC**: the interleaved matrix (A, B, C × 3) 00:44:17–00:52:02, the probes to 00:53:41, the PostgreSQL twin 00:54:34–00:57:19, the row-set sweep 00:59:17–01:04:42 |
| branch / worktree | `worktree-pw6-rotate-benchmark`, in the worktree `pw6-rotate-benchmark` |
| version / commit | **`v2.0.0-48-g314a06d`** — `314a06d`, "feat(tools): PW6 client half - the per-core writer shape, and SHOW META's core=", one commit over `origin/main` `a5fc289` |
| tree | clean at `314a06d` when the binary was copied; during the run the working tree carried a concurrent review's uncommitted edits to `tools/multicore_benchmark.py` (docstring text and a `kill()` fallback after a failed `terminate()` — nothing on the timing path; the driver measured is that working-tree version), this file, `bench/run_pw6.py` and the `bench/docs/README.md` entries. **Nothing under `src/` or `include/` differed from `314a06d`** |
| **binary measured** | a **copy**, `sha256 8312e8a8a64c56cdeaa230cf83b3b532e3edc435886bd4f864dd5481e663cd1e`, of `build-release/kds_server` (4,985,032 bytes, linked **2026-08-25 00:30:07 UTC**), taken before the first cell and never rewritten; every server started from it. The binary predates the commit by 3 m 45 s. `314a06d`'s only engine change is six lines in `src/server/command_dispatcher.cpp` — `SHOW META`'s `core=` field — and the copy carries it (the driver found every core-1 session by it), so **the copy is the engine at `314a06d`**. No source file is newer than the binary |
| build | `CMAKE_BUILD_TYPE=Release`, `KDS_WITH_TLS=ON` (`build-release/CMakeCache.txt`) |
| test suite | **not executed in this session** — no engine code was changed here; the binary was built by the main session. Stated, not implied green |
| device | `/dev/root`, **ext4**, 247 GB with 186 GB free (`df -T $HOME`). Not tmpfs; every data file, WAL segment and log under `$HOME/mcbench-pw6/run/<cell>/` |
| host | 2 vCPUs (`nproc` = 2), AMD EPYC 9V74, Linux 6.17.0-1022-azure, Ubuntu 24.04 |
| server configuration | per configuration: `cores = 1` or `2`, `placement = creating` or `rotate`, `peer_listeners = off` or `on`, `log_level = warn`; **everything else default** — `durability = group`, `wal_drain_interval_us = 1000`, `checkpoint_interval_ms = 5000`, `buffer_pool_frames = 0`, `inline_cell_width = 64`, `tls = off`, `auth = off` |
| relations | `CREATE TABLE benchN (id int64, owner varchar, balance int64) BTREE`, `ASSIGNED` keys — the shape the peer's PW1c-5 gate admits since PW2-4 lifted the btree arm (`src/server/command_dispatcher.cpp`, `CheckWriteAffinity`) |
| client | the Python driver, one thread and one newline-protocol connection per relation, statements retried while the reply is retryable (the wire's `retryable=1`, or the row-id lease's "retry after the refill grant lands"), the whole wait recorded as the statement's latency |
| work | **2,000 rows per relation** in the matrix: 2,000 INSERTs, 2,000 point-SELECTs, 2,000 UPDATEs, 1,000 DELETEs, one scan per relation. Equal work, not equal time. §9 sweeps 200 and 10,000 |
| ports | matrix 15470–15487, probes 15520–15524, sweep 15540–15549, PostgreSQL 15433. Two ports per invocation, never reused; `pgrep -x kds_server` empty before and after every configuration |
| machine quiet | every configuration gated on no `cc1plus`/`cc1`/`ld`/`as`/`kds_tests`/`cmake`/`ninja`/`make` and a 1-minute load under 0.50; the load each configuration started at is recorded (0.09–0.49) and the gate waited 22 times, every one on the load alone — it never found a build or test process. A code review ran concurrently in this worktree; the gate cannot see a short gtest that starts *inside* a cell, and no cell's end load (0.14–0.95, the cell's own work) or per-run spread (§3) suggests one did |
| PostgreSQL | **16.14**, the rootless extraction under `$HOME/pg16`, cluster `$HOME/pg-bench` at defaults, `synchronous_commit = on`, one backend per relation — §8 |

Three cells, interleaved A, B, C, A, B, C, A, B, C, each invocation running
its own `cores = 1` baseline first:

| cell | `--placement` | `--peer-listeners` | `--tables` | what it compares |
|---|---|---|---|---|
| **A** (control) | `creating` | off | 2 | both configurations serve every statement on core 0; parity expected, and the spread between them is the noise floor |
| **B** (the PW6 shape) | `rotate` | on | 2 | two relations on core 1, each written from a session core 1 accepted, against the same two relations on core 0 |
| **C** | `rotate` | on | 4 | the same with four writers on core 1 |

## 2. What this host could not measure

The server refuses the only configuration in which two writer cores exist:

```
startup failed: cores 3 exceeds the 2 this machine reports; reactors are pinned
one per core and never block, so overcommitting them serializes whole workloads
behind each other
```

At `cores = 2` the rotation policy skips the system core, so every rotated
relation carries `owner_core=1` — the driver prints it per relation, and B
and C confirm it. Two relations on one peer core is parallelism 1 on the
peer side against parallelism 1 on core 0, which is why this file's headline
is a cost. **A host with three or more CPUs runs the missing cell as
`--cores 3 --tables 2 --placement rotate --peer-listeners`**, one relation on
core 1 and one on core 2, each with its own WAL stream and its own
`fdatasync`, against the same two on core 0. §10 says what that cell would
have to show for PW6's row to be closed and what §6's stalls would do to it.

## 3. The noise floor is ±5% on throughput and ±2% on a write median

The A cell's two configurations differ only in `cores` and both serve on
core 0, so their spread is the floor for every other comparison. Three
interleaved runs:

| run | cores = 1, stmt/s | cores = 2, stmt/s | ratio |
|---|---:|---:|---:|
| A-r1 | 1,398 | 1,313 | 0.939 |
| A-r2 | 1,385 | 1,386 | 1.001 |
| A-r3 | 1,329 | 1,337 | 1.006 |
| **mean** | **1,371** | **1,345** | **0.982** |

A ratio between two identical engines ranged 0.939–1.006 in three runs, and
the aggregate itself 1,313–1,398 (±3% around its mean; B's `cores = 2` side
spread 1,263–1,407, ±5%). The floor is the fsync: every write statement in
this workload is one commit, and the device's fdatasync drifts run to run
(§7, and `bench/results-scenario2-freight.md` §2 found the same ±8% on the
same device). **A throughput delta under ~5% or a write-median delta under
~2% is not a finding in this file.** The one control that moved outside it
is C's, and §6 accounts for all of it.

## 4. The matrix — throughput

Rates are statements per second. The aggregate is the run's statement count
over its wall clock (mean of three runs); the per-phase rate is **derived** —
the phase's statements over the slowest connection's busy time in that
phase, since the driver reports per-statement latencies and not per-phase
elapsed. PostgreSQL rows are the twin's, §8.

| cell | engine / configuration | writers | aggregate stmt/s | INSERT/s | point-SELECT/s | UPDATE/s | DELETE/s | multi ÷ single (per run) |
|---|---|---:|---:|---:|---:|---:|---:|---|
| A | KDS `cores = 1`, core 0 | 2 | 1,371 | 994 | 58,813 | 985 | 976 | — |
| A | KDS `cores = 2`, `creating`, core 0 | 2 | 1,345 | 983 | 60,316 | 951 | 973 | **0.982** (0.939 / 1.001 / 1.006) |
| B | KDS `cores = 1`, core 0 | 2 | 1,366 | 979 | 52,604 | 987 | 984 | — |
| B | **KDS `cores = 2`, `rotate` + listeners, core 1** | 2 | 1,333 | 988 | 52,311 | 918 | 997 | **0.977** (0.924 / 0.941 / 1.066) |
| C | KDS `cores = 1`, core 0 | 4 | 2,643 | 1,922 | 34,531 | 1,937 | 1,918 | — |
| C | **KDS `cores = 2`, `rotate` + listeners, core 1** | 4 | 2,118 | 1,440 | 4,959 | 1,902 | 1,906 | **0.802** (0.835 / 0.779 / 0.791) |
| — | PostgreSQL 16.14, defaults | 2 | 1,275 | 917 | 30,218 | 929 | 921 | — |
| — | PostgreSQL 16.14, defaults | 4 | 2,435 | 1,774 | 32,532 | 1,772 | 1,791 | — |

Two things the matrix says on its own. **B is A**: 0.977 against 0.982, the
INSERT and DELETE rates a hair *higher* on core 1, the UPDATE rate 7% lower
— and the UPDATE medians (§5) are 1,887 against 1,854 µs, a 1.8% delta with
the same sign in C (1,941 against 1,906) and in A itself (1,871 against
1,858); consistent in sign, below the floor, not a finding. **C's 20% is in
two columns**: the INSERT rate, 1,922 → 1,440, and the point-SELECT rate,
34,531 → 4,959. UPDATE and DELETE are at parity. §6 and §7 take those two
columns apart.

A structural note the matrix carries about the *device*, not the engine: the
INSERT rate at two writers (994/s) is no higher than a single session's
(1,043/s, §7's probe), while four writers reach 1,922/s. PostgreSQL shows the
same shape — 917/s at two backends, 1,774/s at four. Each commit is one
`fdatasync` on this volume, two sessions in lockstep alternate their syncs
rather than share one, and it takes a third and fourth session for a sync to
carry more than one commit. That is what "group" durability buys here, on
both engines.

## 5. Latency distributions

Microseconds, pooled over the three matrix runs of each cell, nearest-rank
percentiles; `n` is the statement count. Per-run p50/p99 are in the report
(`bench/run_pw6.py --report`); the largest per-run p50 spread inside any
row below is 1,833–1,912 µs on a write, the floor of §3.

**INSERT** (autocommit, one `fdatasync` each)

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 2,013 | 907 | 1,789 | 1,848 | 1,940 | 2,228 | 2,782 | 5,157 | 18,840 |
| A `cores = 2` | 12,000 | 2,037 | 880 | 1,807 | 1,867 | 1,965 | 2,270 | 2,946 | 5,044 | 29,714 |
| B `cores = 1` | 12,000 | 2,046 | 920 | 1,799 | 1,862 | 1,966 | 2,307 | 3,028 | 5,250 | 31,336 |
| **B core 1** | 12,000 | 2,023 | 923 | 1,807 | 1,863 | 1,965 | 2,263 | 2,881 | 4,906 | 14,109 |
| C `cores = 1` | 24,000 | 2,080 | 936 | 1,856 | 1,916 | 2,014 | 2,331 | 2,902 | 5,249 | 27,313 |
| **C core 1** | 24,000 | 2,411 | 37 | 1,853 | 1,918 | 2,019 | 2,322 | 2,871 | 4,997 | **1,754,467** |
| PostgreSQL, 2 backends | 12,000 | 2,182 | 940 | 1,972 | 2,058 | 2,164 | 2,355 | 2,795 | 5,071 | 24,449 |
| PostgreSQL, 4 backends | 24,000 | 2,260 | 962 | 2,008 | 2,128 | 2,273 | 2,512 | 3,055 | 5,166 | 23,104 |

The peer's INSERT body is core 0's to the microsecond: B's p25/p50/p75 are
1,807 / 1,863 / 1,965 against 1,799 / 1,862 / 1,966. C core 1's p0 of 37 µs
is not a fast INSERT — it is a refused one (§6b), and its p100 of 1.75 s is
a first INSERT waiting for its row-id lease refill (§6a). Its mean carries both;
its median does not.

**Point-SELECT by pk**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 32.6 | 22 | 26 | 26 | 27 | 39 | 44 | 71 | 2,923 |
| A `cores = 2` | 12,000 | 32.0 | 22 | 26 | 26 | 27 | 42 | 52 | 71 | 1,823 |
| B `cores = 1` | 12,000 | 35.2 | 22 | 26 | 26 | 26 | 30 | 36 | 71 | 8,808 |
| **B core 1** | 12,000 | 36.2 | 20 | 24 | 25 | 26 | 34 | 41 | 83 | 3,905 |
| C `cores = 1` | 24,000 | 93.9 | 23 | 47 | 51 | 59 | 95 | 121 | 1,050 | 32,949 |
| **C core 1** | 24,000 | 739.0 | 23 | 62 | **943** | 992 | 1,063 | 1,199 | 2,299 | 31,394 |
| PostgreSQL, 2 backends | 12,000 | 65.9 | 45 | 48 | 50 | 76 | 116 | 134 | 173 | 1,551 |
| PostgreSQL, 4 backends | 24,000 | 113.4 | 36 | 74 | 113 | 135 | 160 | 180 | 254 | 2,719 |

A pk lookup on core 1 is a pk lookup on core 0 — 25 against 26 µs at p50,
20 against 22 at p0. Four sessions on one core double the median on either
core (47–51 µs: four clients pipelining through one reactor), and C core 1's
943 µs median is not a lookup cost at all; §7 measures it as the fsync of a
session that is still writing, and §6a explains why C's sessions were still
writing when the others read.

**UPDATE by pk**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 2,030 | 880 | 1,800 | 1,858 | 1,941 | 2,172 | 2,876 | 5,796 | 31,296 |
| A `cores = 2` | 12,000 | 2,105 | 902 | 1,814 | 1,871 | 1,961 | 2,231 | 2,898 | 5,906 | 123,866 |
| B `cores = 1` | 12,000 | 2,027 | 892 | 1,796 | 1,854 | 1,940 | 2,176 | 2,876 | 5,878 | 31,057 |
| **B core 1** | 12,000 | 2,196 | 890 | 1,827 | 1,887 | 1,999 | 2,371 | 3,173 | 6,433 | 82,164 |
| C `cores = 1` | 24,000 | 2,055 | 904 | 1,848 | 1,906 | 1,995 | 2,204 | 2,837 | 5,584 | 35,045 |
| **C core 1** | 24,000 | 2,087 | 921 | 1,877 | 1,941 | 2,038 | 2,288 | 2,890 | 5,035 | 32,647 |
| PostgreSQL, 2 backends | 12,000 | 2,154 | 1,007 | 1,968 | 2,062 | 2,159 | 2,355 | 2,748 | 4,272 | 18,227 |
| PostgreSQL, 4 backends | 24,000 | 2,258 | 1,034 | 2,009 | 2,116 | 2,245 | 2,491 | 3,012 | 5,223 | 67,098 |

**DELETE by pk** (the odd half; delete-marks, nothing reclaimed)

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 6,000 | 2,050 | 867 | 1,785 | 1,845 | 1,946 | 2,241 | 2,927 | 5,799 | 30,936 |
| A `cores = 2` | 6,000 | 2,055 | 877 | 1,796 | 1,849 | 1,934 | 2,203 | 3,098 | 6,065 | 27,540 |
| B `cores = 1` | 6,000 | 2,027 | 824 | 1,784 | 1,843 | 1,928 | 2,171 | 2,918 | 6,020 | 27,567 |
| **B core 1** | 6,000 | 1,996 | 869 | 1,807 | 1,864 | 1,950 | 2,151 | 2,548 | 4,361 | 42,775 |
| C `cores = 1` | 12,000 | 2,073 | 857 | 1,860 | 1,922 | 2,014 | 2,229 | 2,705 | 4,673 | 35,226 |
| **C core 1** | 12,000 | 2,020 | 847 | 1,821 | 1,891 | 1,999 | 2,264 | 2,877 | 5,560 | 15,847 |
| PostgreSQL, 2 backends | 6,000 | 2,174 | 945 | 1,936 | 2,064 | 2,161 | 2,368 | 2,954 | 5,951 | 11,051 |
| PostgreSQL, 4 backends | 12,000 | 2,243 | 954 | 1,963 | 2,123 | 2,255 | 2,498 | 3,194 | 5,340 | 18,060 |

**Scan** (`WHERE balance > 0` over the surviving 1,000 rows, one per
relation per run — too few for a distribution, so the three values that
exist)

| cell / configuration | n | p0 | p50 | p100 |
|---|---:|---:|---:|---:|
| A `cores = 1` / `cores = 2` | 6 / 6 | 266 / 269 | 298 / 270 | 1,420 / 1,231 |
| B `cores = 1` / **core 1** | 6 / 6 | 270 / 262 | 284 / 292 | 1,250 / 1,258 |
| C `cores = 1` / **core 1** | 12 / 12 | 276 / 277 | 1,194 / 1,176 | 1,546 / 1,308 |
| PostgreSQL, 2 / 4 backends | 6 / 12 | 1,108 / 1,104 | 1,343 / 1,150 | 1,404 / 1,360 |

The KDS scan is bimodal in every cell, ~270 µs or ~1,200 µs, and which
relation gets which is the barrier's accident: the scan is each thread's
last statement, and the thread that reaches it while its neighbour is still
in the DELETE phase pays that neighbour's fsync (§7). In C, three of four
do. On the peer as on core 0.

## 6. Where C's 20% went: two per-relation start-up waits on a peer

A relation on a peer core is written through three leases the core does not
hold at `CREATE TABLE` — row-id, trx-id, extent — each a refused statement
until its refill lands, and one grant the client never sees refused (the
relation grant landed before the first write in every run, §6a). The B and C
cells' first INSERT per relation, with the driver's retry counts and session
hunts:

| run | writers on core 1 | connections opened for them | first INSERT per relation, µs | retries (INSERT) | failed INSERTs |
|---|---:|---:|---|---:|---:|
| B-r1 | 2 | 3 | 2,334 · 6,475 | 7 | 0 |
| B-r2 | 2 | 3 | 2,344 · 5,206 | 7 | 0 |
| B-r3 | 2 | 7 | 2,322 · 5,283 | 7 | 0 |
| C-r1 | 4 | 6 | 2,706 · 6,800 · **783,149** · **1,667,667** | 2,225 | **51** |
| C-r2 | 4 | 8 | 4,515 · 13,703 · **1,432,082** · **1,754,467** | 3,032 | **1** |
| C-r3 | 4 | 10 | 3,081 · 6,489 · **1,455,012** · **1,455,949** | 2,754 | **13** |

On core 0 the first INSERT of a relation is 1.0–2.1 ms in every run, with no
retries — one fsync, like every other. The session hunt itself is cheap: the
kernel's `SO_REUSEPORT` distribution handed the driver a core-1 session on
roughly every second connection (3–7 opens for two sessions, 6–10 for four),
and the DDL session landed on core 0 at the first connection in all six
runs. That is the whole cost of PW5's "clients cannot choose their core" at
this scale.

**6a. The row-id lease, and what a second-long refill looks like.** The first INSERT into a
relation on a peer is refused until the row-id refill grant lands (PW1b, the
documented and deliberate contract — `docs/inflight/in-progress/workplan-peer-writer.md` §7a) and
the driver retries it at 0.5 ms. Relation 1 clears in 2.3–2.7 ms after two
retries; relation 2 in 5.2–6.8 ms after five, because the refill request
runs one-in-flight per core (`row_id_refill_in_flight_`,
`include/kds/server/core_runtime.hpp`) and relation 2's demand queues behind
relation 1's. Each leg is a drain tick plus the ring's idle-block latency —
a peer that is idle polls its ring only when its reactor wakes, at most
`max_idle_block_ms` or the next timer, which the 1 ms drain timer bounds
(`src/sched/scheduler.cpp`, `IdleTimeoutMs`) — plus core 0's publish, which
is an fsync. Two to three round trips of that shape is the 2–7 ms observed.
That wait is per relation and per mount, and §9 shows it is the same 3 ms
and 7 ms at 200 rows and at 10,000.

Relations 3 and 4 are a different wait. **0.51 s, 0.78 s, 1.43–1.75 s** —
three orders of magnitude above relations 1 and 2, in every C run at both
row-set sizes, and always on the third and fourth relations. **The servers'
logs, read after the run, say what it is** (`$HOME/mcbench-pw6/run/
C-rotate-t4-r*/multi-core.log`; `log_level = warn` logs every refused
statement): every refusal on relations 3 and 4 is `ERR row-id lease for
relation oid 4008|4012 is spent; retry after the refill grant lands` — in
C-r1, 15 + 678 refusals of `bench2` across two wall-clock seconds and
16 + 871 + 609 of `bench3` across three — and **not one is PW1c-7's
`RelationWriteRightsPending`**, so the relation-grant request and its
`kRelationGrantRequestTicks` latch are not on this path (the first draft
of this file attributed the wait to them from a reading of the source; the
logs retract it). The same logs show the **trx-id lease** spent mid-run
(`this core's transaction-id lease is spent`, 7–8 retries on single INSERTs
and UPDATEs of every relation — a pre-emptive refill with a quarter-window
of headroom lagging its consumption) and the **extent lease** spent (§6b).
Core 0 logged no failed grant of any kind, and the peer no failed send. So
the fact is: **under four active sessions on one peer, every lease refill —
row-id, trx-id, extent — completes hundreds of milliseconds to seconds
after a ring round trip that idle takes 2–7 ms**, and the mechanism is
untraced. Each refill is a coroutine parked on `WaitFor` and resumed by the
group scheduler, which picks the least-consumed group first
(`src/sched/scheduler.cpp`, `PickNextGroup`), so starvation of the system
group by the query group is not the obvious reading either. It is the first
thing §11 asks the workplan to record, with the trace that decides it.

What those waits cost: 2,225–3,032 retries per C run at 0.5 ms each, a
client thread stalled for 1.5 s out of a 13 s run, and — the part that
matters more than the throughput — the four threads leaving lockstep, which
§7 turns into a 943 µs read median.

**6b. The extent lease, spent and not retried.** The C runs also lost
INSERTs outright:

```
ERR extent lease: this core's lease of 64 pages is spent; a refill must be granted
before it can allocate again
```

51, 1 and 13 times in the three runs (`src/storage/extent_lease.cpp:59`).
Four relations growing at once on one peer consume its 64-page lease faster
than the refill lands, and the refusal is not one the driver retries: it
matches neither `retryable=1` nor the row-id lease's "retry after the refill
grant lands", so each is recorded as an error and the row is never written.
**C core 1's numbers are therefore over a workload that lost 0.01–0.6% of
its INSERTs**, and its point-SELECT/UPDATE/DELETE phases touched ids that
did not exist. The driver had no verify at this run (`314a06d`; `2eb49a4`
added the per-relation `COUNT(*)` afterwards), so the reply count was the
only witness. At two writers the lease refilled ahead of demand through ~250
pages of growth — B at 10,000 rows finished with zero errors (§9) — so this
is a rate effect of four concurrent allocators against one lease and one
refill round trip, not a per-relation constant.

**6c. Three refusals that promise a retry, and one that carries the bit.**
`ErrorReply` (`src/server/command_dispatcher.cpp`) spells `retryable=1` on
`TxnConflict` and `retryable=0` on the two constraint violations; every other
status is a bare `ERR <message>`. Three messages a peer emits in this
workload tell the client to retry in prose and carry no bit: the row-id
lease's (`include/kds/catalog/row_id_lease.hpp:99`), the trx-id lease's
(`include/kds/txn/trx_id_lease.hpp:65`), and the extent lease's above. The
driver special-cases the first; the third cost this run its rows. A client
library built on `docs/spec/protocol.md` §11's bit — "authoritative client
guidance … part of the compatibility surface" — would have retried none of
them. The UPDATE phase's 1, 28 and 29 retries in the C runs *were* bit-carrying
refusals (`TXN_CONFLICT retryable=1`, the only spelling the driver matches
by the bit); their message text was not kept, and the two candidates on a
peer are CC3's affinity refusal and PW1c-7's rights probe.

## 7. The wait breakdown: a write is one fsync, and a read on the same core waits for it

Probes on the same copied binary and device, after the matrix
(`bench/run_pw6.py --probes`; µs, n = 2,000 unless stated):

| probe | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `pwrite` 4 KiB + `fdatasync`, overwrite in place | 748 | 843 | **874** | 929 | 1,068 | 1,547 | 3,173 | 13,155 |
| the same, appending (the file grows) | 2,587 | 2,866 | 2,960 | 3,110 | 3,901 | 4,743 | 7,272 | 13,096 |
| `SHOW META` round trip, core 0 session, `cores = 1` | 21 | 31 | 31 | 32 | 32 | 33 | 41 | 132 |
| `SHOW META` round trip, core 0 session, `cores = 2` rotate + listeners | 23 | 30 | 31 | 31 | 32 | 34 | 40 | 379 |
| `SHOW META` round trip, **core 1 session**, `cores = 2` rotate + listeners | 19 | 29 | 29 | 30 | 30 | 31 | 38 | 105 |
| INSERT, one session alone, core 0 (n = 500) | 863 | 924 | **959** | 1,006 | 1,127 | 1,350 | 1,890 | 6,817 |
| INSERT, one session alone, **core 1** (n = 500; the first took 2 retries) | 864 | 928 | **958** | 1,003 | 1,125 | 1,389 | 3,003 | 6,259 |
| point-SELECT, alone, core 0 | 25 | 37 | 37 | 41 | 43 | 46 | 54 | 111 |
| point-SELECT, alone, **core 1** | 30 | 34 | 35 | 36 | 36 | 39 | 46 | 901 |
| **point-SELECT while a second session on core 0 commits INSERTs back to back** | 35 | 940 | **973** | 1,014 | 1,110 | 1,362 | 2,635 | 10,415 |
| **the same on core 1** | 32 | 931 | **962** | 1,011 | 1,137 | 1,516 | 3,943 | 14,362 |
| the committing session's INSERTs meanwhile, core 0 (n = 2,174, 0 errors) | 857 | 941 | 972 | 1,016 | 1,114 | 1,356 | 2,592 | 10,387 |
| the same on core 1 (n = 2,168, 0 errors) | 841 | 932 | 964 | 1,013 | 1,144 | 1,504 | 3,948 | 14,348 |

**A write statement, one session** — 959 µs at p50, identical on both cores:

| wait | µs | share | how it was measured |
|---|---:|---:|---|
| durability — the WAL segment's `fdatasync` (`src/wal/file_log_device.cpp:390`; segments are prewritten, so it is the overwrite class, not the append class) | 874 | 91% | the fdatasync probe on the same device |
| client and socket round trip | ~26 | 3% | the point-SELECT p50 bounds it from above; `SHOW META`'s 29–31 µs includes building a long reply |
| the statement itself — parse, the btree insert, the WAL append, the reply | ~59 | 6% | the residual |

**A write statement, two sessions on one core** — 1,863 µs at p50 (§5, B
and A alike): the same three plus **~900 µs waiting for the other session's
sync**. The WAL drain runs on the reactor thread as the scheduler's
post-task hook and on the drain timer (`src/server/expeditor.cpp`,
`SetPostTaskHook(drain)`), and the `fdatasync` inside it blocks that thread;
a statement that arrives while it is in the kernel is neither parsed nor
executed until it returns. Two writers in lockstep therefore each wait out
one full sync of the other's and then their own — 874 + 874 + ~85 ≈ 1,833 —
which is what the tables show, on core 1 exactly as on core 0. At four
writers the queue behind a sync holds three statements and their commits
share the next one, so the per-statement median stays near 1.9 ms while the
rate doubles (§4).

**A read on a core with a committing session** — 962–973 µs at p50, against
35–37 alone: one full fsync plus the lookup. Not a residual (p25 is already
940) because the reader and the writer are in lockstep too: the reader's
next SELECT lands ~30 µs after its reply, by which time the writer's next
INSERT has arrived, executed and entered its sync. **This is the C cell's
943 µs point-SELECT median** (§5): the §6a stalls put relations 3 and 4
1.5 s behind relations 1 and 2, so every thread's SELECT phase ran while
another thread on the same core was in its INSERT or UPDATE phase. Core 0's
C configuration, whose four threads stayed within a few milliseconds of
each other, shows the same mode only at its tail (p95 121, p99 1,050 µs).
It is not a peer property; the probe measures it at the same size on both
cores, and A's `cores = 2` configuration would show it too if its threads
were pushed apart.

**Waits that do not apply here.** Lock and conflict waits: none by
construction — each connection owns its relation — beyond the UPDATE-phase
`TXN_CONFLICT` retries of §6c (1, 28, 29 per C run; at most ~15 ms in total
per run). Cross-core round trips inside a statement: none — every statement
is served on the core that owns the relation, which is the shape PW6
defines. Server-side CPU per statement: not measurable by this harness, which
has no server-side timer; the point-SELECT p0 of 20–22 µs bounds the whole
statement including the socket.

## 8. Versus PostgreSQL 16.14

There is no `tools/pg_multicore_benchmark.py`; the twin is
`bench/run_pw6.py --pg` — the identical statement sequence per relation
(the INSERT spelled `INSERT INTO t (owner, balance) VALUES (...)` for the
`bigserial` pk), one backend per relation, timed by the driver's own
`timed()` through `tools/pg_wire.py`, against the port-15433 cluster at
defaults with `synchronous_commit = on`, three runs of each shape
interleaved with each other and gated on the same quiet-box rule. Building
that twin as a `tools/pg_*.py` file is the open task. The table is §4's and
§5's rows side by side; PostgreSQL has no peer/core axis, so its column
stands against both KDS configurations.

| shape | KDS core 0 (`cores = 1`) | KDS core 1 (`rotate` + listeners) | PostgreSQL | KDS core 0 ÷ PG | KDS core 1 ÷ PG |
|---|---:|---:|---:|---:|---:|
| 2 relations, aggregate stmt/s | 1,366 | 1,333 | 1,275 | 1.07× | 1.05× |
| 2 relations, INSERT/s | 979 | 988 | 917 | 1.07× | 1.08× |
| 2 relations, point-SELECT/s | 52,604 | 52,311 | 30,218 | 1.74× | 1.73× |
| 2 relations, INSERT p50 µs | 1,862 | 1,863 | 2,058 | 0.90× | 0.91× |
| 2 relations, point-SELECT p50 / p99 µs | 26 / 71 | 25 / 83 | 50 / 173 | 0.52× / 0.41× | 0.50× / 0.48× |
| 4 relations, aggregate stmt/s | 2,643 | 2,118 | 2,435 | 1.09× | **0.87×** |
| 4 relations, INSERT/s | 1,922 | 1,440 | 1,774 | 1.08× | **0.81×** |
| 4 relations, point-SELECT p50 µs | 51 | 943 | 113 | 0.45× | **8.3×** |

Two relations and two writers is the clean comparison, and it is the
scenario-2 result again on a different workload: **KDS commits 7% more
statements a second than PostgreSQL and answers a pk lookup in half the
time, on the peer core as on core 0**, while the write medians sit 10% apart
because both engines are paying the same device for the same fdatasync.
PostgreSQL's 1.75-second first INSERT does not exist; its first INSERT per
relation is 2.3–8.3 ms (a fresh backend's first statement), the same order
as KDS's row-id lease wait. At four relations KDS on core 0 keeps its 9%,
and **KDS on core 1 falls 13% behind PostgreSQL** — the §6 stalls and lost
rows — with a pk lookup median eight times PostgreSQL's, which is §7's
fsync exposure and nothing PostgreSQL's process-per-backend model has to
pay: a backend blocked in its own fsync blocks no other backend's read.

## 9. The row-set sweep: 200 / 2,000 / 10,000 rows

One pass of the cells at 200 and at 10,000 rows per relation (`--rows`; C at
10,000 was not run — at 125 pages per relation it would spend the 64-page
lease repeatedly and §6b already prices that), the 2,000-row column being
the matrix's three-run mean.

| cell | rows | single stmt/s | multi stmt/s | multi ÷ single | INSERT p50 µs, single → multi | point-SELECT p50 µs | first INSERT per relation on core 1, µs | retries / failed |
|---|---:|---:|---:|---:|---|---|---|---|
| A | 200 | 1,254 | 1,320 | 1.053 | 2,108 → 2,055 | 25 → 27 | — | 0 / 0 |
| A | 2,000 | 1,371 | 1,345 | 0.982 | 1,848 → 1,867 | 26 → 26 | — | 0 / 0 |
| A | 10,000 | 1,301 | 1,405 | 1.080 | 2,112 → 1,856 | 26 → 26 | — | 0 / 0 |
| B | 200 | 1,142 | 1,128 | 0.987 | 2,152 → 2,120 | 26 → 25 | 3,013 · 6,480 | 7 / 0 |
| B | 2,000 | 1,366 | 1,333 | 0.977 | 1,862 → 1,863 | 26 → 25 | 2,322–2,344 · 5,206–6,475 | 7 / 0 |
| B | 10,000 | 1,370 | 1,382 | 1.009 | 1,870 → 1,854 | 26 → 25 | 2,980 · 7,094 | 38 / 0 |
| C | 200 | 2,172 | 1,377 | **0.634** | 2,281 → 2,215 | 50 → **1,099** | 3,649 · 14,824 · **514,207** · **517,616** | 814 / 0 |
| C | 2,000 | 2,643 | 2,118 | **0.802** | 1,916 → 1,918 | 51 → **943** | 2.7–4.5 ms · 6.5–13.7 ms · **0.78–1.46 s** · **1.46–1.75 s** | 2,225–3,032 / 1–51 |

The sweep separates the fixed costs from the per-row ones. **Every
per-statement number is flat across a 50× range of rows**: the peer's INSERT
median is core 0's at 200, 2,000 and 10,000 (the ±0.25 ms drift between
columns is the device's, §3 — A at 10,000 rows moved 2,112 → 1,856 between
two configurations that are the same engine). **The relation start-up
waits are fixed per relation**: 3 ms and 7 ms for relations 1 and 2 at every
size, and half a second to 1.75 s for relations 3 and 4 — so C's ratio is a
function of run length alone, 0.634 over a 2 s run and 0.802 over 13 s,
and would approach 1.0 for a long-lived relation. Two things do scale. The
row-id lease is spent again mid-run: B at 10,000 rows took 38 INSERT
retries against 7 at 2,000, with no trace in the tail (a refill on a busy
peer lands inside the ~1 ms the statement was going to wait anyway). And
the extent lease is *not* spent at two writers even at 10,000 rows — zero
errors through ~250 pages of refills — which is what makes §6b's failures a
concurrency effect rather than a size one.

## 10. What the engine teaches

**The peer write path itself is free at this parallelism, and this is the
measurement that says so.** `worktree-pw6-rotate-benchmark` at `314a06d`
writes a peer-owned relation through the row-id lease, the trx-id lease, the
PL-B handoff's write grant and its own WAL stream, and none of that appears
in a statement's latency: INSERT, UPDATE and DELETE medians on core 1 are
within 1.8% of core 0's at three row-set sizes, the pk lookup is 25 µs
against 26, the round trip 29 µs against 31, and the throughput ratio
(0.977) is inside the control's (0.982). PW1–PW5 delivered a write path with
no per-statement tax. `docs/inflight/in-progress/workplan-peer-writer.md` §1 calls the missing
number "the binding constraint"; this is the half of it a two-CPU host can
supply.

**What a peer relation costs is paid once per relation, at its first
write, and the third relation on a core pays a thousand times more than
the first.** 2–7 ms for the row-id lease — the contract PW1b chose
(demand-driven, §7a) and priced at "one retry per relation per mount", which
this run confirms at two retries for the first relation and five for the
second. Half a second to 1.75 s for the **same row-id refill** when four
relations start together — the round trip that takes 2–7 ms idle, with no
failed grant or send in any log, and the trx-id and extent refills lagging
beside it (§6a, §6b). No document prices it, no counter names it, and the
source's own reading — a parked coroutine resumed by a scheduler that picks
the least-consumed group first — does not predict it. Tracing that second
is the next job (§11); whatever it is, removing it removes the whole C-cell
loss and, with it, the desynchronisation that produced the 943 µs reads.

**Every session on a core pays every other session's fsync, reads
included.** This is the finding with the widest reach, and it is not a
peer finding — the probe puts it at 973 µs on core 0 and 962 µs on core 1.
`docs/spec/wal.md` §6 has the drain in the `system` group and the commit
"suspended on a flush future", and `kds.conf.sample`'s `cores` key has
reactors that "are pinned and never block"; at `314a06d` the drain's
`fdatasync` runs on the reactor thread and
blocks it for ~0.9 ms per commit, so a saturating writer takes ~90% of its
core's wall clock out of service for everyone else on it. `kds.conf.sample`
states the consequence for `relaxed` ("the sync that enforces it runs on
the reactor thread, so the statement in flight when it fires pays the
device's full latency"); this run states it for `group`, which is the
default, and for reads, which are the workload OLTP wants to keep fast. The
sizing consequence for range-granular ownership is direct: **on a write
workload a core's capacity is ~1,000 commits a second at this device's
fdatasync, and a read colocated with those commits inherits their latency.**
An asynchronous sync — the drain submitting the fdatasync to an I/O thread
or `io_uring` and the reactor continuing — is the change that would make a
core's reads independent of its writes; it is `docs/spec/heap-and-tuple.md` §8's
open "I/O backend" decision, and this is its first data point.

**The wire's `retryable` bit is not the retry contract the engine
actually enforces.** Three refusals on the peer write path tell the client
to retry in their message and give a bit-driven client nothing; one of them
cost this run its rows. Either the bit goes on those statuses or the
messages stop promising what the bit denies.

**PostgreSQL is the calibration, not the competitor, in this file.** It
shows the same two-writer fsync serialisation (917/s at two backends, 1,774
at four) and the same ~2 ms write median, so KDS's 7–9% margin on
throughput and 2× on lookups are the engine's own; and its process-per-
backend model is exactly what keeps *its* reads at 50–113 µs while a
neighbour commits, which is the comparison that makes §7's stall an
architectural cost rather than a device fact.

## 11. What this run leaves open, and for whom

- **The cell PW6's row asks for is unrun.** `--cores 3 --tables 2 --placement
  rotate --peer-listeners` on a host with ≥ 3 CPUs, and its answer is not
  guaranteed to be a speedup: two writer cores would each fdatasync their own
  segment on the same volume, and whether two concurrent fdatasyncs on one
  ext4 device overlap is the fact that decides whether the aggregate INSERT
  rate is 2 × ~1,000/s or ~1,000/s shared. §7's fdatasync probe on a second
  file in parallel is the pre-measurement to take before spending the host.
- **Every lease refill lags by hundreds of milliseconds to seconds under
  four active sessions on one peer** — the row-id refill for relations 3–4
  (§6a's 0.5–1.75 s, per the servers' logs), the trx-id lease spent
  mid-run, the extent lease spent (§6b) — where an idle round trip is
  2–7 ms. Core 0 logged no failed grant; the mechanism is untraced.
  `docs/inflight/in-progress/workplan-peer-writer.md` should carry it as the next item with the
  trace that decides it: `SHOW META` counters per lease kind (requests,
  grants, the longest request-to-grant wait in drain ticks) and a
  debug-level run of cell C at 200 rows.
- **The extent lease's refill loses the race at four concurrent allocators**
  (§6b), and its refusal is not retried by the only client that retries the
  row-id lease's. PW1c-6's grant extension owns the lease; the bit owns the
  refusal (§6c).
- **`docs/spec/wal.md` §6 and `kds.conf.sample`'s `cores` comment describe a
  reactor that does not block, and the drain blocks it** (§7, §10). The spec should carry the measured
  cost — 0.9 ms of every commit spent with the reactor in `fdatasync`, paid
  by every colocated read — until the I/O backend decision retires it.
- **`tools/multicore_benchmark.py` had no verify at this run** (`314a06d`),
  and it needed one: a `SELECT COUNT(*)` per relation after the DELETE phase
  would have named the 51 lost rows without the reply count — `2eb49a4`
  added exactly that, after the measurement. It should also carry the
  extent lease's message in `is_retryable`, or better, the engine should
  carry the bit.
- **The PostgreSQL twin lives in `bench/run_pw6.py`** and belongs in
  `tools/pg_multicore_benchmark.py` beside the other twins.
