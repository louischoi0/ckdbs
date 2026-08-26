# The per-core writer at equal parallelism — the PW6 matrix at `v2.0.0-67-g952bbb9`

**Four writers on one peer core run at 1.030× the single-core
configuration (1.007 / 1.026 / 1.057 over three interleaved runs) with every
relation at its expected row count, and the peer's lease refills wait
milliseconds: the row-id refill's longest wait is 3.0–3.3 ms, the extent
refill's 4.9–5.8 ms, the trx-id refill's 7.4–26.5 ms, and not one of the
three was ever refused mid-run** — every refusal in every peer's log is the
row-id lease's first-INSERT refusal that `docs/inflight/in-progress/workplan-peer-writer.md` §7a
makes the contract. Those are the numbers `docs/inflight/known-gaps.md` and
`docs/inflight/in-progress/workplan-peer-writer.md` PW7 record as owed from a `bench/v2.0.0/`
file, and this file is it, measured on the tree at `v2.0.0-67-g952bbb9`.

The rest of the matrix says what it said before the scheduler's floors,
now on its own evidence at this commit: **two writers on the peer core cost
nothing this harness resolves** — 0.990× the single-core configuration
(0.962–1.021) against a control of identical engines that measured 0.944×
(0.866–0.983, §3 accounts for the 0.866) — with every write median within
2% of core 0's and the pk lookup 25 µs against 26. And **a point-SELECT on
a core with a committing session still waits out that session's
`fdatasync`**: 1,088 µs at p50 on core 0 and 1,083 µs on core 1, against
37 and 35 µs alone (§7), because the WAL drain runs on the reactor thread
at this commit as `src/server/core_runtime.cpp:731` and
`src/server/expeditor.cpp:1657` install it. At four sessions that exposure
is the read p99 on both cores (1,122–1,170 µs) and the one row of §8 where
PostgreSQL leads by more than 3×.

What this host cannot measure is unchanged (§2): the server refuses
`cores = 3` on two CPUs, so PW6's speedup cell is still unrun, and every
peer cell here is the peer write path against core 0's at equal parallelism.

The workload is `tools/multicore_benchmark.py`'s: N relations × M rows, one
connection per relation, INSERT / point-SELECT by pk / UPDATE by pk / DELETE
the odd half / one scan, each configuration a fresh server on a fresh data
file, and the driver's `COUNT(*)` verify per relation at the end. The
matrix, the PostgreSQL twin, the probes and the report are
`bench/run_pw6.py`. How to run both: `bench/docs/README.md`. The raw
driver output of this run — every `result.json`, `probes.json`, the
servers' configs, logs and stderr, the driver logs — is archived beside
this file under `bench/v2.0.0/archive/multicore-writers-v2.0.0-67-g952bbb9/`
(the `result.json` files gzipped; no data file or WAL segment).

## 1. The run

| | |
|---|---|
| executed | **2026-08-25 12:22:05 → 12:44:59 UTC**: the interleaved matrix (A, B, C × 3) 12:22:05–12:30:59, the probes 12:31:05–12:31:46, the row-set sweep 12:32:29–12:41:34, the PostgreSQL twin 12:41:36–12:44:59 |
| branch / worktree | `worktree-agent-a88b32b3e80c45166`, in the worktree `agent-a88b32b3e80c45166` (a measurement worktree; no engine code was changed in it) |
| version / commit | **`v2.0.0-67-g952bbb9`** — `952bbb9`, "docs(stride-forest): the stride-forest workplan, with its review re-checked against 9b498d0", committed 2026-08-25 12:13:22 UTC. The last engine commits under it: `e13ad71` (the key mode deleted — an INSERT names the pk or omits it per row, and the peer's shape gate is per row), `250cd3b` (PW3b — the peer's shutdown checkpoint, and the extent grant persisted on core 0 before it leaves), `78f45a6`–`a17f981` (PW1c-6b), `454d492` (the PW7 review applied) |
| tree | **clean** at `952bbb9` when the binary was built and copied, and for the whole run (`git status` empty; `build-release/` is ignored) |
| **binary measured** | a **copy**, `sha256 9528bfdad4b355c7d05d610bda5df8575e64ffad9b67365ea9cd0794c9478d92`, of this worktree's `build-release/kds_server` (5,124,032 bytes, linked **2026-08-25 12:20:33 UTC**, 7 minutes *after* HEAD's commit — so the binary is the engine at `952bbb9`, with no source file newer than it), taken at 12:21:04 before the first cell and never rewritten; every server started from `$HOME/mcbench-pw7/bin/kds_server` |
| build | a fresh `build-release/` configured in this worktree: `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), `KDS_WITH_TLS=ON` |
| test suite | **not executed in this session** — no engine code was changed here. Stated, not implied green |
| device | `/dev/root`, **ext4**, 247 GB with 180 GB free (`df -T $HOME`). Not tmpfs; every data file, WAL segment and log under `$HOME/mcbench-pw7/run/<cell>/` |
| host | 2 vCPUs (`nproc` = 2), AMD EPYC 9V74, Linux 6.17.0-1022-azure, Ubuntu 24.04.4 |
| server configuration | per configuration: `cores = 1` or `2`, `placement = creating` or `rotate`, `peer_listeners = off` or `on`, `log_level = warn`; **everything else default** — `durability = group`, `wal_drain_interval_us = 1000`, `checkpoint_interval_ms = 5000`, `buffer_pool_frames = 0`, `inline_cell_width = 64`, `tls = off`, `auth = off` |
| relations | `CREATE TABLE benchN (id int64, owner varchar, balance int64) BTREE`; every INSERT omits the pk, so the engine issues it (`e13ad71`: `autoincrement=if-omitted`, the shape a peer admits on that arity) |
| client | the Python driver, one thread and one newline-protocol connection per relation, statements retried while the reply is retryable (the wire's `retryable=1`, or the three lease messages that spell "retry" without it), the whole wait recorded as the statement's latency; `COUNT(*)` per relation after the DELETE phase |
| work | **2,000 rows per relation** in the matrix: 2,000 INSERTs, 2,000 point-SELECTs, 2,000 UPDATEs, 1,000 DELETEs, one scan per relation. Equal work, not equal time. §9 sweeps 200 and 10,000 for every cell |
| ports | matrix 15470–15487, probes 15520–15524, sweep 15540–15551, PostgreSQL 15433. Two ports per invocation, never reused; `pgrep -x kds_server` empty before and after every configuration |
| machine quiet | every configuration gated on no `cc1plus`/`cc1`/`ld`/`as`/`kds_tests`/`cmake`/`ninja`/`make` and a 1-minute load under 0.50; the load each of the 36 configurations started at is recorded (0.30–0.50), and the gate waited 30 times, every one on the load alone — the previous configuration's own tail; it never found a build or test process. This worktree's release build finished at 12:20:33, before the first cell; nothing else ran on the host |
| PostgreSQL | **16.14** (Ubuntu 16.14-0ubuntu0.24.04.1), the rootless extraction under `$HOME/pg16`, cluster `$HOME/pg-bench` at defaults, `synchronous_commit = on`, one backend per relation, started for its own cells only — §8 |
| overhead | **overhead not measured (the v2 amendment)** — the interleaved A/B per-statement overhead measurement is suspended by the operator's 2026-08-24 amendment; this file carries the cells and nothing beyond them |

Three cells, interleaved A, B, C, A, B, C, A, B, C, each invocation running
its own `cores = 1` baseline first:

| cell | `--placement` | `--peer-listeners` | `--tables` | what it compares |
|---|---|---|---|---|
| **A** (control) | `creating` | off | 2 | both configurations serve every statement on core 0; parity expected, and the spread between them is the noise floor |
| **B** (the PW6 shape) | `rotate` | on | 2 | two relations on core 1, each written from a session core 1 accepted, against the same two relations on core 0 |
| **C** | `rotate` | on | 4 | the same with four writers on core 1 — the cell PW7 was traced on |

## 2. What this host could not measure

The server refuses the only configuration in which two writer cores exist:

```
startup failed: cores 3 exceeds the 2 this machine reports; reactors are pinned
one per core and never block, so overcommitting them serializes whole workloads
behind each other
```

At `cores = 2` the rotation policy skips the system core, so every rotated
relation carries `owner_core=1` — the driver prints it per relation, and
every B and C configuration confirms it. Two or four relations on one peer
core is parallelism 1 on the peer side against parallelism 1 on core 0,
which is why every peer number in this file is a cost, not a scaling
number. **A host with three or more CPUs runs the missing cell as
`--cores 3 --tables 2 --placement rotate --peer-listeners`**, and §11 says
what to probe first.

## 3. The noise floor: ±5% on throughput when nothing stalls, and one run in which something did

The A cell's two configurations differ only in `cores` and both serve on
core 0, so their spread is the floor for every other comparison. Three
interleaved runs:

| run | cores = 1, stmt/s | cores = 2, stmt/s | ratio |
|---|---:|---:|---:|
| A-r1 | 1,228 | 1,064 | **0.866** |
| A-r2 | 1,202 | 1,182 | 0.983 |
| A-r3 | 1,243 | 1,222 | 0.983 |
| **mean** | **1,225** | **1,156** | **0.944** |

A-r1's `cores = 2` configuration contains one event the other 35
configurations do not: **both sessions' 95th INSERT waited 485.7 and
485.8 ms at the same moment** — core 0's reactor, or the device under it,
was unavailable for half a second 0.2 s into the run — and the same
configuration's UPDATE and DELETE phases carry ten and eight statements of
15–46 ms where the `cores = 1` run beside it carries one and three. The
server's warn-level log is empty; nothing in the driver's output attributes
it. Half a second is 4% of a 12 s run on its own, and the ratio without
that run is 0.983. Two consequences for reading this file: **a per-run
throughput ratio within ±5% is not a finding**, and **a single ratio as
low as 0.87 can be one stall of this kind** rather than the engine — which
is why every cell here is three runs, and why §9's one-run cells are read
against the medians and not the aggregate alone.

On the write median the floor is tighter: A's INSERT p50 is 2,100 against
2,128 µs (+1.3%), UPDATE 2,097 against 2,132, DELETE 2,065 against 2,124 —
**±2% on a write median at 2,000 rows**. §9's 10,000-row control moved
2,094 → 1,838 µs (−12%) between two identical engines a minute apart, so at
that size the device's drift is the floor and the ratio (1.048 there) says
nothing.

The absolute level of every write in this file is the device's: §7's
`fdatasync` probe is 980 µs at p50 on this run, and every write median is
two of them plus ~170 µs. It is not a number to compare across files.

## 4. The matrix — throughput

Rates are statements per second. The aggregate is the run's statement count
over its wall clock (mean of three runs); the per-phase rate is **derived**
— the phase's statements over the slowest connection's busy time in that
phase, since the driver reports per-statement latencies and not per-phase
elapsed. PostgreSQL rows are the twin's, §8.

| cell | engine / configuration | writers | aggregate stmt/s | INSERT/s | point-SELECT/s | UPDATE/s | DELETE/s | multi ÷ single (per run) |
|---|---|---:|---:|---:|---:|---:|---:|---|
| A | KDS `cores = 1`, core 0 | 2 | 1,225 | 880 | 62,665 | 879 | 880 | — |
| A | KDS `cores = 2`, `creating`, core 0 | 2 | 1,156 | 815 | 48,038 | 842 | 840 | **0.944** (0.866 / 0.983 / 0.983) |
| B | KDS `cores = 1`, core 0 | 2 | 1,189 | 848 | 46,101 | 861 | 856 | — |
| B | **KDS `cores = 2`, `rotate` + listeners, core 1** | 2 | 1,177 | 852 | 55,462 | 831 | 860 | **0.990** (1.021 / 0.988 / 0.962) |
| C | KDS `cores = 1`, core 0 | 4 | 2,269 | 1,646 | 39,853 | 1,630 | 1,697 | — |
| C | **KDS `cores = 2`, `rotate` + listeners, core 1** | 4 | 2,337 | 1,697 | 41,562 | 1,685 | 1,700 | **1.030** (1.026 / 1.007 / 1.057) |
| — | PostgreSQL 16.14, defaults | 2 | 1,230 | 888 | 26,943 | 891 | 897 | — |
| — | PostgreSQL 16.14, defaults | 4 | 2,382 | 1,748 | 30,990 | 1,731 | 1,742 | — |

Three things the matrix says on its own. **B is A**: 0.990 against a
control of 0.944 (0.983 without §3's stalled run), the INSERT and DELETE
rates a hair higher on core 1, the UPDATE rate 3% lower — every one inside
the floor. **C is parity too, and its four columns say so individually**:
INSERT 1,697/s against 1,646, point-SELECT 41,562 against 39,853, UPDATE
1,685 against 1,630, DELETE 1,700 against 1,697 — the peer's four writers
run every phase at core 0's rate, and the throughput ratio of 1.030 is the
device's drift between two configurations a minute apart, not a peer
advantage (the control shows +4.8% at 10,000 rows on the same device, §9).
**Four writers commit 1.9–2.0× what two do on either engine**: 880 → 1,646
INSERTs/s on KDS, 888 → 1,748 on PostgreSQL. Each commit is one
`fdatasync` on this volume; two sessions in lockstep alternate their syncs
rather than share one, and it takes a third and fourth session for a sync
to carry more than one commit. That is what `group` durability buys here,
and it is the device's shape, shown by both engines.

## 5. Latency distributions

Microseconds, pooled over the three matrix runs of each cell, nearest-rank
percentiles; `n` is the statement count (the first relation's INSERT phase
carries one probe row, hence 12,003 and 24,003). Per-run p50/p99 are in
the archive's `matrix-tables.md`; the largest per-run INSERT p50 spread
inside any row below is 2,175–2,321 µs (C `cores = 1`, the device's drift
between runs), and the peer rows' is 2,090–2,155 (B core 1) — the floor
of §3.

**INSERT** (autocommit, one `fdatasync` each)

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,003 | 2,273 | 968 | 2,029 | 2,100 | 2,208 | 2,514 | 3,098 | 5,357 | 59,035 |
| A `cores = 2` | 12,003 | 2,461 | 992 | 2,053 | 2,128 | 2,248 | 2,664 | 3,490 | 6,666 | 485,842 |
| B `cores = 1` | 12,003 | 2,359 | 1,057 | 2,079 | 2,153 | 2,266 | 2,648 | 3,533 | 6,260 | 38,483 |
| **B core 1** | 12,003 | 2,347 | 981 | 2,044 | 2,121 | 2,247 | 2,671 | 3,458 | 6,125 | 117,204 |
| C `cores = 1` | 24,003 | 2,433 | 1,090 | 2,134 | 2,220 | 2,388 | 2,858 | 3,563 | 6,025 | 31,345 |
| **C core 1** | 24,003 | 2,356 | 1,005 | 2,100 | 2,177 | 2,307 | 2,698 | 3,378 | 5,168 | 25,758 |
| PostgreSQL, 2 backends | 12,000 | 2,253 | 1,013 | 2,036 | 2,105 | 2,218 | 2,464 | 3,232 | 5,051 | 23,134 |
| PostgreSQL, 4 backends | 24,000 | 2,286 | 1,045 | 2,059 | 2,136 | 2,262 | 2,534 | 3,123 | 4,986 | 20,055 |

The peer's INSERT body is core 0's: B's p25/p50/p75 are 2,044 / 2,121 /
2,247 against 2,079 / 2,153 / 2,266, C's 2,100 / 2,177 / 2,307 against
2,134 / 2,220 / 2,388 — the peer side lower by 1–2% in both, the sign the
control does not share (A's `cores = 2` side is the higher one), so below
the floor and not a finding. **C core 1's p100 is 25.8 ms and its p0 is a
real INSERT** (1,005 µs): no refused statement and no second-long first
INSERT is in this distribution — §6 has the first INSERTs at 2.8–11 ms.
A `cores = 2`'s p100 is §3's stall.

**Point-SELECT by pk**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 30 | 23 | 26 | 26 | 27 | 30 | 38 | 59 | 3,168 |
| A `cores = 2` | 12,000 | 48 | 19 | 26 | 26 | 27 | 35 | 44 | 1,040 | 77,459 |
| B `cores = 1` | 12,000 | 39 | 22 | 26 | 26 | 27 | 43 | 56 | 89 | 3,151 |
| **B core 1** | 12,000 | 33 | 21 | 24 | 25 | 26 | 35 | 41 | 64 | 3,347 |
| C `cores = 1` | 24,000 | 85 | 23 | 47 | 51 | 59 | 100 | 120 | 1,170 | 6,885 |
| **C core 1** | 24,000 | 76 | 18 | 47 | 48 | 51 | 70 | 91 | 1,122 | 10,263 |
| PostgreSQL, 2 backends | 12,000 | 74 | 44 | 50 | 55 | 83 | 121 | 146 | 180 | 1,907 |
| PostgreSQL, 4 backends | 24,000 | 126 | 40 | 86 | 116 | 144 | 186 | 221 | 337 | 2,080 |

A pk lookup on core 1 is a pk lookup on core 0 — 25 against 26 µs at p50,
21 against 22 at p0. Four sessions on one core double the median on either
core (47–51 µs: four clients pipelining through one reactor), and **the
four-session p99 of 1,122–1,170 µs on both cores is §7's mode** — a read
that landed while another session's commit held the reactor in
`fdatasync`. It is 2.0% of C core 1's reads and 2.1% of core 0's (reads
≥ 900 µs), against 0.2–1.1% at two sessions and 0.19% for PostgreSQL's four
backends; the share is the same on the peer as on core 0, which is what
makes it a per-core property and not a peer one.

**UPDATE by pk**

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 12,000 | 2,275 | 996 | 2,032 | 2,097 | 2,192 | 2,449 | 3,176 | 5,928 | 39,808 |
| A `cores = 2` | 12,000 | 2,367 | 996 | 2,060 | 2,132 | 2,236 | 2,519 | 3,473 | 7,226 | 48,156 |
| B `cores = 1` | 12,000 | 2,319 | 987 | 2,068 | 2,146 | 2,258 | 2,597 | 3,312 | 5,836 | 24,928 |
| **B core 1** | 12,000 | 2,402 | 983 | 2,060 | 2,139 | 2,279 | 2,802 | 3,673 | 6,604 | 99,969 |
| C `cores = 1` | 24,000 | 2,452 | 1,064 | 2,138 | 2,228 | 2,377 | 2,917 | 3,763 | 6,584 | 16,173 |
| **C core 1** | 24,000 | 2,367 | 1,038 | 2,087 | 2,160 | 2,268 | 2,711 | 3,562 | 6,298 | 24,171 |
| PostgreSQL, 2 backends | 12,000 | 2,243 | 1,041 | 2,026 | 2,094 | 2,203 | 2,456 | 3,131 | 5,173 | 31,103 |
| PostgreSQL, 4 backends | 24,000 | 2,308 | 1,053 | 2,081 | 2,167 | 2,297 | 2,548 | 3,170 | 5,028 | 26,663 |

**DELETE by pk** (the odd half; delete-marks, nothing reclaimed)

| cell / configuration | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A `cores = 1` | 6,000 | 2,269 | 989 | 2,000 | 2,065 | 2,162 | 2,466 | 3,297 | 5,883 | 37,841 |
| A `cores = 2` | 6,000 | 2,364 | 991 | 2,045 | 2,124 | 2,236 | 2,599 | 3,753 | 7,283 | 29,379 |
| B `cores = 1` | 6,000 | 2,326 | 995 | 2,041 | 2,115 | 2,241 | 2,666 | 3,416 | 6,044 | 33,898 |
| **B core 1** | 6,000 | 2,321 | 1,008 | 2,029 | 2,117 | 2,268 | 2,770 | 3,513 | 6,057 | 24,651 |
| C `cores = 1` | 12,000 | 2,354 | 1,020 | 2,065 | 2,138 | 2,248 | 2,640 | 3,510 | 6,606 | 21,363 |
| **C core 1** | 12,000 | 2,345 | 1,006 | 2,071 | 2,140 | 2,238 | 2,684 | 3,592 | 6,080 | 17,997 |
| PostgreSQL, 2 backends | 6,000 | 2,228 | 1,036 | 2,016 | 2,085 | 2,188 | 2,408 | 2,984 | 5,217 | 16,243 |
| PostgreSQL, 4 backends | 12,000 | 2,295 | 1,035 | 2,060 | 2,134 | 2,242 | 2,507 | 3,125 | 5,264 | 20,118 |

Every UPDATE and DELETE median on the peer is within 1.5% of core 0's, in
both cells, with p0 within 30 µs — the write path after the row-id and
trx-id leases are held is the same path.

**Scan** (`WHERE balance > 0` over the surviving 1,000 rows, one per
relation per run — too few for a distribution, so the three values that
exist)

| cell / configuration | n | p0 | p50 | p100 |
|---|---:|---:|---:|---:|
| A `cores = 1` / `cores = 2` | 6 / 6 | 252 / 267 | 280 / 502 | 1,355 / 1,460 |
| B `cores = 1` / **core 1** | 6 / 6 | 265 / 262 | 288 / 277 | 1,395 / 3,099 |
| C `cores = 1` / **core 1** | 12 / 12 | 261 / 259 | 1,246 / 1,251 | 1,422 / 1,379 |
| PostgreSQL, 2 / 4 backends | 6 / 12 | 1,123 / 1,104 | 1,138 / 1,142 | 1,381 / 3,027 |

The KDS scan is bimodal in every cell, ~260 µs or ~1,250 µs, and which
relation gets which is the barrier's accident: the scan is each thread's
last statement, and the thread that reaches it while its neighbour is still
in the DELETE phase pays that neighbour's fsync (§7). On the peer as on
core 0.

## 6. The four-writer cell: every relation starts within 11 ms, and nothing is lost

A relation on a peer core is written through three leases the core does not
hold at `CREATE TABLE` — row-id (`kRowIdLeasePerGrant = 4096`), trx-id
(`kTrxIdBlockSize = 4096`, per core), extent (`kDefaultExtentPages = 64`,
per core) — the first a refused statement until its refill lands, the other
two refilled pre-emptively. The B and C cells' first INSERT per relation,
with the driver's retry counts, the session hunts and the verify:

| run | writers on core 1 | connections opened for them | first INSERT per relation, µs | retries (INSERT) | failed INSERTs | verify |
|---|---:|---:|---|---:|---:|---|
| B-r1 | 2 | 3 | 3,667 · 4,093 | 7 | 0 | every relation at its expected count |
| B-r2 | 2 | 3 | 3,680 · 7,575 | 7 | 0 | every relation at its expected count |
| B-r3 | 2 | 4 | 3,791 · 5,409 | 7 | 0 | every relation at its expected count |
| C-r1 | 4 | 7 | 2,804 · 4,486 · 7,243 · 10,528 | 18 | 0 | every relation at its expected count |
| C-r2 | 4 | 4 | 3,040 · 5,643 · 7,726 · 11,071 | 18 | 0 | every relation at its expected count |
| C-r3 | 4 | 4 | 2,792 · 4,318 · 7,650 · 10,663 | 20 | 0 | every relation at its expected count |

On core 0 the first INSERT of a relation is 1.1–2.3 ms in every run, with
no retries — one fsync, like every other. The session hunt is cheap:
`SO_REUSEPORT` handed the driver a core-1 session on roughly every second
connection (3–4 opens for two sessions, 4–7 for four), and the DDL session
landed on core 0 at the first connection in all six runs.

**The verify is the finding this cell exists for.** Each relation's
`COUNT(*)` after the DELETE phase is exactly `rows / 2` (plus the probe
row in the first) in all six peer configurations, and in §9's four more,
including four writers at 10,000 rows through ten extent refills. The
peer's warn-level logs, read after the run, hold **only row-id refusals** —
7 per B run and 18/18/20 per C run, every one an `INSERT` of a relation's
first row refused with `row-id lease for relation oid N is spent; retry
after the refill grant lands` — and not one `transaction-id lease` or
`extent lease` refusal in any of the ten peer logs. `docs/inflight/in-progress/workplan-peer-writer.md`
PW7's row claims this cell at 0.99–1.03× with no lost rows; at
`v2.0.0-67-g952bbb9` it measures 1.030× (1.007 / 1.026 / 1.057) with every
relation at its expected count.

**6a. The first INSERTs are the serial row-id refill, at 2.5–3 ms per
relation.** The row-id refill runs one-in-flight per core
(`row_id_refill_in_flight_`, `include/kds/server/core_runtime.hpp:386`), so
relation k's first INSERT queues behind k−1 refills: 2.8 · 4.5 · 7.5 ·
10.7 ms in C, 3.7 · 4.1–7.6 in B, each step one ring round trip plus the
peer's own reactor iteration — which, with four sessions committing, is a
`fdatasync` long. The retry counts are the same arithmetic: 2 + 5 = 7 in B,
2 + 3 + 5 + 8 = 18 in C-r1 (the log names each relation's), at the driver's
0.5 ms backoff. The wait is per relation and per mount, and §9 shows it at
the same 2.4–14 ms at 200 rows and at 10,000.

**6b. The refills, per leg, from the peer's own `SHOW META`.** Every refill
carries `LeaseRefillStats` (`include/kds/server/lease_refill_stats.hpp`):
requests, grants, and the longest wait split into submit→sent (this
reactor queueing the request task), sent→grant (the ring and core 0), and
grant→resumed (this reactor reaching the parked coroutine), in milliseconds
and in reactor iterations. The driver reads them from a fresh core-1
session after each peer configuration:

| run | lease | requests / grants | longest wait, ms | submit→sent, ms / iterations | sent→grant, ms / iterations | grant→resumed, ms / iterations |
|---|---|---:|---:|---|---|---|
| B-r1 | row-id | 2 / 2 | 1.0 | 0.0 / 0 | 1.0 / 2,177 | 0.0 / 1 |
| B-r1 | trx-id | 3 / 3 | 30.3 | 0.0 / 0 | 30.3 / 83,064 | 2.0 / 1 |
| B-r1 | extent | 1 / 1 | 5.6 | 0.0 / 0 | 4.6 / 3 | 1.0 / 1 |
| B-r2 | row-id | 2 / 2 | 1.2 | 0.0 / 0 | 1.2 / 2,107 | 0.0 / 1 |
| B-r2 | trx-id | 3 / 3 | 33.0 | 0.0 / 0 | 33.0 / 91,547 | 3.3 / 1 |
| B-r2 | extent | 1 / 1 | 4.4 | 0.0 / 0 | 3.4 / 2 | 1.1 / 1 |
| B-r3 | row-id | 2 / 2 | 1.1 | 0.0 / 0 | 1.1 / 2,047 | 0.0 / 1 |
| B-r3 | trx-id | 3 / 3 | 22.9 | 0.0 / 0 | 22.8 / 61,850 | 2.0 / 1 |
| B-r3 | extent | 1 / 1 | 4.4 | 0.0 / 0 | 3.3 / 2 | 1.1 / 1 |
| **C-r1** | **row-id** | 4 / 4 | **3.0** | 0.0 / 0 | 1.2 / 802 | 1.8 / 1 |
| C-r1 | trx-id | 6 / 6 | 8.4 | 0.0 / 0 | 8.2 / 20,499 | 1.9 / 1 |
| C-r1 | extent | 2 / 2 | 5.8 | 0.0 / 0 | 4.0 / 2 | 2.6 / 1 |
| **C-r2** | **row-id** | 4 / 4 | **3.1** | 0.0 / 0 | 2.1 / 889 | 1.1 / 1 |
| C-r2 | trx-id | 6 / 6 | 7.4 | 0.0 / 0 | 5.8 / 13,531 | 2.6 / 1 |
| C-r2 | extent | 2 / 2 | 4.9 | 0.0 / 0 | 3.7 / 2 | 1.7 / 1 |
| **C-r3** | **row-id** | 4 / 4 | **3.3** | 0.0 / 0 | 2.3 / 792 | 1.0 / 1 |
| C-r3 | trx-id | 6 / 6 | 26.5 | 0.0 / 0 | 26.5 / 72,660 | 1.4 / 1 |
| C-r3 | extent | 2 / 2 | 5.6 | 0.0 / 0 | 4.1 / 3 | 2.0 / 1 |

Every request was granted, and **the submit→sent leg is zero in every
row** — the request task is polled in the iteration it is submitted, which
is the floor `docs/spec/sched.md` §4 states. The three legs that remain each
say something about the engine at this commit:

- **The row-id refill's longest wait is 3.0–3.3 ms under four writers**
  (1.0–1.2 ms under two), of which the ring-and-core-0 leg is 1.2–2.3 ms
  and the resume leg 1.0–1.8 ms over **exactly one iteration**: the grant
  arrived while the peer's reactor was inside an iteration that lasted a
  millisecond — a statement's `fdatasync` (§7) — and the coroutine resumed
  at the next. A long-in-time, one-iteration resume leg is the drain on the
  reactor, measured from the inside.
- **The extent refill's ring leg is 3.3–4.6 ms over 2–3 iterations.** At
  this commit core 0 persists the free map before the grant leaves
  (`ExtentAllocator::Persist` → `DevicePageStore::PersistMaps`, called at
  `src/server/extent_lease_service.cpp:26`, PW3b's finding at `250cd3b`),
  so the round trip is bounded below by a sync on core 0 plus the peer's own
  iteration — two device syncs on this volume are ~2 ms, and the rest is the
  ring's idle-poll cadence on a core 0 that serves nothing else in these
  cells. Two refills at 2,000 rows × 4 relations, ten at 10,000 (§9), none
  late enough to refuse an allocation.
- **The trx-id refill's ring leg is 5.8–33 ms over 13,531–91,547
  iterations** — 0.35–0.45 µs per iteration — which is the peer's reactor
  spinning while it waits: `docs/inflight/known-gaps.md` records that a reactor with
  a parked coroutine drops its idle block to zero, and these counts are
  what that looks like. The wait itself is core 0's side of the grant (the
  ceiling raise is persisted before the reply, `src/server/expeditor.cpp:1434`)
  plus its idle-poll cadence; the refill is requested with a quarter of
  the window still held (`TrxIdSequence::low_water`,
  `include/kds/txn/trx_id.hpp:145`: held ≤ window / 4, ~1,000 ids of
  `kTrxIdBlockSize = 4096`), which at this cell's ~1,700 commits a second
  is 0.6 s of headroom against a 33 ms wait, and no statement in any cell
  saw it — the logs hold no trx-id refusal. It is the longest wait in the table and the one the workload does
  not feel; it stays recorded as the spin it is.

**6c. Three refusals still promise a retry without the wire's bit.**
`include/kds/base/status.hpp:96` at this commit reads `IsRetryable(code) ==
kTxnConflict` and nothing else, so the row-id, trx-id and extent leases'
`ResourceExhausted` replies still print as bare `ERR <message>`; the driver
matches their text (`RETRY_TEXTS`, `tools/multicore_benchmark.py`). Only
the first was ever emitted in this run. A client retrying on the bit alone
would have lost the first INSERT of every peer relation. No `TXN_CONFLICT`
retry occurred in any phase of any run.

## 7. The wait breakdown: a write is one fsync, and a read on the same core waits for it

Probes on the same copied binary and device, after the matrix
(`bench/run_pw6.py --probes`; µs, n = 2,000 unless stated):

| probe | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `pwrite` 4 KiB + `fdatasync`, overwrite in place | 856 | 949 | **980** | 1,021 | 1,105 | 1,278 | 2,653 | 5,789 |
| the same, appending (the file grows) | 1,268 | 1,388 | 1,431 | 1,493 | 1,704 | 2,284 | 4,433 | 18,653 |
| `SHOW META` round trip, core 0 session, `cores = 1` | 22 | 30 | 31 | 32 | 32 | 33 | 41 | 658 |
| `SHOW META` round trip, core 0 session, `cores = 2` rotate + listeners | 22 | 29 | 30 | 31 | 32 | 34 | 41 | 1,175 |
| `SHOW META` round trip, **core 1 session**, `cores = 2` rotate + listeners (the reply carries the refill block) | 22 | 32 | 33 | 33 | 35 | 38 | 43 | 1,076 |
| INSERT, one session alone, core 0 (n = 500) | 947 | 1,043 | **1,082** | 1,142 | 1,315 | 1,551 | 2,906 | 6,120 |
| INSERT, one session alone, **core 1** (n = 500; the first took 2 retries) | 942 | 1,062 | **1,106** | 1,178 | 1,662 | 2,492 | 4,132 | 9,876 |
| point-SELECT, alone, core 0 | 25 | 36 | 37 | 41 | 44 | 46 | 58 | 436 |
| point-SELECT, alone, **core 1** | 30 | 34 | 35 | 36 | 37 | 39 | 47 | 1,355 |
| **point-SELECT while a second session on core 0 commits INSERTs back to back** | 34 | 1,045 | **1,088** | 1,153 | 1,392 | 1,760 | 3,623 | 12,362 |
| **the same on core 1** | 24 | 1,043 | **1,083** | 1,139 | 1,396 | 2,014 | 4,123 | 11,889 |
| the committing session's INSERTs meanwhile, core 0 (n = 2,125, 0 errors) | 933 | 1,048 | 1,090 | 1,155 | 1,374 | 1,699 | 3,529 | 12,353 |
| the same on core 1 (n = 2,106, 0 errors) | 942 | 1,049 | 1,088 | 1,144 | 1,404 | 2,019 | 4,061 | 11,885 |

**A write statement, one session** — 1,082 µs at p50 on core 0, 1,106 on
core 1:

| wait | µs | share | how it was measured |
|---|---:|---:|---|
| durability — the WAL segment's `fdatasync` (`src/wal/file_log_device.cpp:390`; segments are prewritten, so it is the overwrite class, not the append class) | 980 | 89–91% | the fdatasync probe on the same device |
| client and socket round trip | ~26 | 2% | the point-SELECT p50 of §5 bounds it from above; `SHOW META`'s 30–33 µs includes building a long reply |
| the statement itself — parse, the btree insert, the WAL append, the reply | ~76–100 | 7–9% | the residual |
| lock or conflict wait | 0 | — | none by construction — each connection owns its relation — and no `TXN_CONFLICT` reply in any run |
| cross-core round trip inside a statement | 0 | — | every statement is served on the core that owns the relation, which is the shape PW6 defines; the leases' waits (§6b) are per relation and per window, not per statement |

Server-side CPU per statement is not measurable by this harness, which has
no server-side timer; the point-SELECT p0 of 18–23 µs bounds the whole
statement including the socket.

**A write statement, two sessions on one core** — 2,121–2,153 µs at p50
(§5, B and A alike): the same three plus **~1,000 µs waiting for the other
session's sync**. The WAL drain runs on the reactor thread as the
scheduler's post-task hook and on the drain timer
(`src/server/core_runtime.cpp:731` for a peer, `src/server/expeditor.cpp:1657`
for core 0, both `SetPostTaskHook(drain)`), and the `fdatasync` inside it
blocks that thread; a statement that arrives while it is in the kernel is
neither parsed nor executed until it returns. Two writers in lockstep
therefore each wait out one full sync of the other's and then their own —
980 + 980 + ~170 ≈ 2,130 — which is what the tables show, on core 1
exactly as on core 0. At four writers the queue behind a sync holds three
statements and their commits share the next one, so the per-statement
median stays near 2.2 ms while the rate doubles (§4).

**A read on a core with a committing session** — 1,083–1,088 µs at p50,
against 35–37 alone: one full fsync plus the lookup. Not a residual (p25 is
already 1,043) because the reader and the writer are in lockstep: the
reader's next SELECT lands ~30 µs after its reply, by which time the
writer's next INSERT has arrived, executed and entered its sync. In the
matrix this is the p99 of every four-session read (§5: 1,122 µs on core 1,
1,170 on core 0, 2% of reads on either), and it is not the median there
because the four threads stay in phase — their SELECT phases overlap
their neighbours' SELECT phases, not their INSERT phases — so a read meets
a commit only at the phase boundaries. Which is the point §6 makes from
the other side: with every relation's first INSERT inside 11 ms, nothing
pushes a peer's threads out of phase, and the peer's read median is core
0's.

## 8. Versus PostgreSQL 16.14

There is no `tools/pg_multicore_benchmark.py`; the twin is
`bench/run_pw6.py --pg` — the identical statement sequence per relation
(the INSERT spelled `INSERT INTO t (owner, balance) VALUES (...)` for the
`bigserial` pk), one backend per relation, timed by the driver's own
`timed()` through `tools/pg_wire.py`, against the port-15433 cluster at
defaults with `synchronous_commit = on`, three runs of each shape
interleaved with each other and gated on the same quiet-box rule; the
cluster was started for these cells and stopped after them. Building that
twin as a `tools/pg_*.py` file is the open task. The table is §4's and
§5's rows side by side; PostgreSQL has no peer/core axis, so its column
stands against both KDS configurations.

Throughput (statements per second; the per-phase rates derived as in §4):

| shape | KDS core 0 (`cores = 1`) | KDS core 1 (`rotate` + listeners) | PostgreSQL | KDS core 0 ÷ PG | KDS core 1 ÷ PG |
|---|---:|---:|---:|---:|---:|
| 2 relations, aggregate stmt/s | 1,189 | 1,177 | 1,230 | 0.97× | 0.96× |
| 2 relations, INSERT/s | 848 | 852 | 888 | 0.96× | 0.96× |
| 2 relations, point-SELECT/s | 46,101 | 55,462 | 26,943 | 1.71× | 2.06× |
| 4 relations, aggregate stmt/s | 2,269 | 2,337 | 2,382 | 0.95× | 0.98× |
| 4 relations, INSERT/s | 1,646 | 1,697 | 1,748 | 0.94× | 0.97× |
| 4 relations, point-SELECT/s | 39,853 | 41,562 | 30,990 | 1.29× | 1.34× |

The positions in §5's distributions the throughput turns on (µs; the full
percentile rows are §5's):

| shape | KDS core 0 (`cores = 1`) | KDS core 1 (`rotate` + listeners) | PostgreSQL | KDS core 0 ÷ PG | KDS core 1 ÷ PG |
|---|---:|---:|---:|---:|---:|
| 2 relations, INSERT p50 | 2,153 | 2,121 | 2,105 | 1.02× | 1.01× |
| 2 relations, point-SELECT p50 | 26 | 25 | 55 | 0.48× | 0.45× |
| 2 relations, point-SELECT p99 | 89 | 64 | 180 | 0.49× | 0.35× |
| 4 relations, INSERT p50 | 2,220 | 2,177 | 2,136 | 1.04× | 1.02× |
| 4 relations, point-SELECT p50 | 51 | 48 | 116 | 0.44× | 0.41× |
| 4 relations, point-SELECT p99 | 1,170 | 1,122 | 337 | **3.48×** | **3.33×** |

On statements a second the two engines are **inside the floor of each
other at both parallelisms** — 0.95–0.98× on the aggregate, 0.94–0.97× on
INSERTs, with write medians 1–4% apart — because both are paying the same
device for the same `fdatasync` per commit, and §4's two-to-four-writer
doubling is identical on both. **The pk lookup is where KDS leads**: half
PostgreSQL's median at two relations (25–26 µs against 55) and at four
(48–51 against 116), on the peer exactly as on core 0. **The read tail is
where PostgreSQL leads, by 3.3–3.5× at four relations**: its p99 of 337 µs
is four backends contending for two CPUs, while KDS's 1,122–1,170 µs is
§7's fsync on the reactor — a backend blocked in its own fsync blocks no
other backend's read, and a reactor blocked in one blocks every session on
its core. PostgreSQL's first INSERT per relation is 2.4–5.3 ms (a fresh
backend's first statement), the same order as the peer's row-id refill
(§6a), and its four-relation cell lost nothing either.

## 9. The row-set sweep: 200 / 2,000 / 10,000 rows

One pass of every cell at 200 and at 10,000 rows per relation (`--rows`),
the 2,000-row column being the matrix's three-run mean. **C at 10,000 runs
in this file** — four writers through 125 pages per relation, ten extent
refills — where the peer's 64-page lease is exercised hardest.

| cell | rows | single stmt/s | multi stmt/s | multi ÷ single | INSERT p50 µs, single → multi | point-SELECT p50 µs | first INSERT per relation on core 1, µs | retries / failed | rows lost |
|---|---:|---:|---:|---:|---|---|---|---|---|
| A | 200 | 1,224 | 1,222 | 0.998 | 2,086 → 2,109 | 26 → 26 | — | 0 / 0 | none |
| A | 2,000 | 1,225 | 1,156 | 0.944 | 2,100 → 2,128 | 26 → 26 | — | 0 / 0 | none |
| A | 10,000 | 1,308 | 1,372 | 1.048 | 2,094 → 1,838 | 26 → 26 | — | 0 / 0 | none |
| B | 200 | 1,254 | 1,218 | 0.971 | 2,085 → 2,074 | 27 → 25 | 2,420 · 4,870 | 5 / 0 | none |
| B | 2,000 | 1,189 | 1,177 | 0.990 | 2,153 → 2,121 | 26 → 25 | 3,667–3,791 · 4,093–7,575 | 7 / 0 | none |
| B | 10,000 | 1,358 | 1,339 | 0.986 | 1,824 → 1,871 | 26 → 25 | 2,217 · 3,819 | 5 / 0 | none |
| C | 200 | 2,281 | 2,219 | 0.973 | 2,216 → 2,229 | 51 → 50 | 2,449 · 7,555 · 10,690 · 14,303 | 18 / 0 | none |
| C | 2,000 | 2,269 | 2,337 | 1.030 | 2,220 → 2,177 | 51 → 48 | 2,792–3,040 · 4,318–5,643 · 7,243–7,726 · 10,528–11,071 | 18–20 / 0 | none |
| C | 10,000 | 2,508 | 2,317 | 0.924 | 1,999 → 2,139 | 51 → 49 | 3,147 · 2,664 · 7,612 · 11,563 | 12 / 0 | none |

The peer's refills over the sweep, from the same `SHOW META` read
(requests / grants, longest wait; the legs in the archive):

| cell | rows | row-id | trx-id | extent |
|---|---:|---|---|---|
| B | 200 | 2 / 2, 1.1 ms | 1 / 1, 50.2 ms (119,885 iterations) | 0 / 0 |
| B | 10,000 | 6 / 6, 2.9 ms | 13 / 13, 32.1 ms | 5 / 5, 8.5 ms (ring leg 6.7 ms / 5 iterations) |
| C | 200 | 4 / 4, 4.4 ms | 1 / 1, 26.5 ms | 0 / 0 |
| C | 10,000 | 12 / 12, 5.0 ms | 25 / 25, 28.3 ms | 10 / 10, 9.5 ms (ring leg 7.7 ms / 2 iterations) |

The sweep separates the fixed costs from the per-row ones. **Every
per-statement number is flat across a 50× range of rows**: the peer's
INSERT median is core 0's at 200, 2,000 and 10,000 within the device's
drift (the control's own 2,094 → 1,838 at 10,000 is the largest move in
the table, between two identical engines), and the pk lookup is 25–26 µs
at every size on either core. **The relation start-up waits are fixed per
relation**: 2.2–3.8 ms for the first relation and 1.7–5 ms more per
relation queued behind it, at every size — which is why C's ratio at 200
rows (0.973, a 1.2 s run with 14 ms of start-up on its last relation) sits
where its ratio at 10,000 does. **Everything the leases do scales with the
rows, and none of it reaches a statement**: 3 row-id grants per relation
at 10,000 rows (4,096 ids each), 13–25 trx-id grants per core, 5–10 extent
grants — every one requested and granted, the longest extent wait 9.5 ms,
and every relation at its expected count. The extent lease's refill runs
ahead of four concurrent allocators through 640 pages of growth; the only
row a peer ever refuses in this file is a relation's first.

C at 10,000's 0.924 is the one ratio in the sweep outside ±5%, and it is
read against §3: its INSERT / UPDATE / DELETE medians moved +7% / +11% /
+13% between two configurations a minute apart, the control's moved −12%
over the same size and interval, and its point-SELECT median and refill
waits are the 2,000-row cell's. One run at that size does not separate the
peer from the device, and this file does not.

## 10. What the engine teaches

**Under the share law's two floors, a peer's lease refills are a
per-relation start-up cost of a few milliseconds and nothing else.** On
`agent-a88b32b3e80c45166` at `952bbb9` every refill request in ten peer
configurations was granted, the submit leg is zero in every one — the
request task is polled in the iteration it is submitted, which is exactly
what `docs/spec/sched.md` §4's first floor promises — and the longest row-id
wait under four writers is 3.3 ms. The two legs that carry time are both
the device's: the resume leg is one reactor iteration long because that
iteration holds a `fdatasync`, and the extent leg holds core 0's map sync
before the grant leaves. The four-writer cell's throughput, its four
per-phase rates, its write medians and its read median are core 0's, and
its verify is clean at 2,000 and at 10,000 rows. `docs/inflight/in-progress/workplan-peer-writer.md`
PW7's claim for this cell — 0.99–1.03×, no lost rows — holds at this
commit, measured on its own evidence.

**The peer write path itself is free at this parallelism, at every
size.** INSERT, UPDATE and DELETE medians on core 1 are within 2% of core
0's at 200, 2,000 and 10,000 rows, the pk lookup is 25 µs against 26, the
round trip 33 µs against 31 (the peer's reply carries the refill block),
and the throughput ratio (0.990 at two writers, 1.030 at four) is inside
what the control does to itself. PW1–PW5's write path — the row-id lease,
the trx-id lease, the PL-B handoff's write grant and the peer's own WAL
stream — has no per-statement tax this harness can resolve. Still
unmeasured, and the binding constraint `docs/inflight/in-progress/workplan-peer-writer.md` §1
names: whether a second writer *core* adds capacity, which needs a third
CPU (§2, §11).

**Every session on a core pays every other session's fsync, reads
included, and at four sessions that is the read p99.** The probe puts a
read beside a committing session at 1,088 µs on core 0 and 1,083 on core
1 against 35–37 alone; the matrix puts the four-session read p99 at
1,122–1,170 µs on both cores, 2% of reads, against PostgreSQL's 337. The
drain's `fdatasync` runs on the reactor thread at this commit and blocks
it ~1 ms per commit, so a saturating writer takes ~90% of its core's wall
clock out of service for everyone else on it. `docs/spec/wal.md` §6 has the
drain in the `system` group and the commit "suspended on a flush future",
and `kds.conf.sample`'s `cores` key has reactors that "are pinned and never
block"; the measurement says otherwise for reads on a committing core. The
sizing consequence for range-granular ownership is direct: **on a write
workload a core's capacity is ~1,000 commits a second at this device's
fdatasync, and a read colocated with those commits inherits their latency
at the tail** — in this file only at the tail, because nothing
desynchronises the sessions any more; a workload whose readers and writers
are not in phase would see it at the median, as the probe does. An
asynchronous sync — the drain handing the `fdatasync` to an I/O thread or
`io_uring` and the reactor continuing — is `docs/spec/heap-and-tuple.md` §8's
open I/O-backend decision, and this is its second data point at the same
size as the first.

**A peer waiting on a grant spins.** The trx-id refill's 13,531–119,885
iterations over 6–50 ms are a reactor polling at 0.4 µs per iteration with
nothing to run but the ring check — `docs/inflight/known-gaps.md`'s "a reactor with
any parked coroutine spins", and `docs/spec/sched.md` §4's accounting gap beside
it (reactor time outside polls is charged to no group). It costs this
workload nothing measurable, because the peer's other sessions are in the
kernel for most of every millisecond anyway; on a host where the peer
shares its CPU with anything else it is a CPU burned for the length of
every refill.

**PostgreSQL is the calibration, not the competitor, in this file.** It
shows the same two-writer fsync serialisation (888/s at two backends,
1,748 at four) and the same ~2.1 ms write median, so the two engines'
statement rates sit inside each other's floor; KDS's 2× on the pk lookup
is the engine's own; and PostgreSQL's process-per-backend model is exactly
what keeps *its* read p99 at 180–337 µs while a neighbour commits, which
is the comparison that makes the reactor's fsync an architectural cost
rather than a device fact.

## 11. What this run leaves open, and for whom

- **The cell PW6's row asks for is unrun.** `--cores 3 --tables 2 --placement
  rotate --peer-listeners` on a host with ≥ 3 CPUs, and its answer is not
  guaranteed to be a speedup: two writer cores would each `fdatasync` their
  own segment on the same volume, and whether two concurrent fdatasyncs on
  one ext4 device overlap is the fact that decides whether the aggregate
  INSERT rate is 2 × ~1,000/s or ~1,000/s shared. §7's fdatasync probe on
  a second file in parallel is the pre-measurement to take before spending
  the host.
- **The drain's `fdatasync` on the reactor is the read tail at four
  sessions and the read median for any out-of-phase reader** (§7, §10).
  `docs/spec/wal.md` §6 and `kds.conf.sample`'s `cores` comment describe a
  reactor that does not block; the spec should carry the measured cost
  until the I/O-backend decision retires it.
- **Three refusals promise a retry without the wire's bit** (§6c) —
  unchanged at this commit; only the row-id one was reached in this run,
  and it is reached by every peer relation's first INSERT. Either the bit
  goes on those statuses or the messages stop promising what the bit
  denies.
- **A peer waiting on a grant spins** (§6b, §10) — recorded, unfixed,
  free here, not free on a shared CPU.
- **One control run stalled for half a second with nothing to attribute it
  to** (§3): both core-0 sessions' INSERT held for 485 ms at the same
  instant, in a `cores = 2` / `creating` configuration where core 1 serves
  nothing. It did not recur in 35 other configurations. A `log_level`
  above `warn` on the next run of this matrix, or a per-iteration reactor
  stall counter, is what would name it.
- **The PostgreSQL twin lives in `bench/run_pw6.py`** and belongs in
  `tools/pg_multicore_benchmark.py` beside the other twins.
- **C at 10,000 rows is one run** (§9), and its 0.924 is inside what the
  device did to the control at that size; three interleaved runs at 10,000
  would settle whether the peer's write medians carry a per-row cost the
  2,000-row matrix cannot see.
