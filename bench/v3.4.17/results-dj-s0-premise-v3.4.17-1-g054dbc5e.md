# DJ-S0 - the premise: what a connection costs, what a connect-and-close loop costs the session beside it, where `Begin` refuses, and what a capped portal holds

Measured 2026-11-20 at `v3.4.17-1-g054dbc5e` (`054dbc5e`, worktree
`dj-connections-sessions-cursors`, before any DJ code), for
`instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` §5
(DJ-S0). No bar is applied: the stage prices the parts, and the order's
items are restated against the prices in its §7.

## Setup

- Binary: `build-release/kds_server` built at `054dbc5e` in the worktree,
  copied to the run directory and hashed: sha256 `3df8801d61b6cbd8...` (in
  full in the archive's JSON files). One binary; nothing is compared with
  another build.
- Data file on `/dev/nvme0n1p1`, ext4 (`/home/louis/dj-work/s0`); never
  tmpfs. Ports 15671 and 15681, chosen.
- Host: 8 CPUs (Xeon Platinum 8581C), Linux 6.12. The server's reactor
  pinned to CPU 4 (`reactor_cpus`; CPUs 4 and 5 at `cores = 2`), its other
  threads to CPUs 4-7, the driver to CPU 0 and the connect loop's process
  to CPU 1. `durability = group`, `buffer_pool_frames = 65536`,
  `auth = off`.
- Relation `dj_t (id int64, a int64, b int64, c varchar(32)) BTREE`,
  100,000 rows, the whole of it resident. The standing statement is
  `SELECT * FROM dj_t WHERE id = k`, k uniform over the rows, one
  connection.
- Driver: `tools/dj_s0_premise.py`, its `--help` the cells' definition.
- **The host was not quiet, and what that did.** Another session built and
  ran its suite through the stage. The first run (`s0.json`) had 7 to 8
  compilers beside most of its timed arms and **its `beside` and `churn`
  cells are discarded** - kept in the archive, not reported. The driver was
  then given a gate per arm - an arm starts on a host with no compiler,
  linker or suite running and is run again if one appeared while it ran -
  and the timed cells were measured again (`s0b.json`, `s0c.json`): every
  arm reported below ended on a quiet host, 5 of 38 on a second attempt.
  The gate held the two gated runs 2,652 s and 336 s; the measurement
  itself is about 4 minutes of arms. Load average at the arms' ends 1.7 to 8.7 (the tail of
  the other session's work, not a compiler). The `memory` and `begin` cells
  are counts and resident sizes, taken from the first run.
- Cut to fit the stage's 20 minutes: 3 repeats of 3 s an arm; one relation
  size; `cores = 1` for everything but one `begin` arm; no arm under SCRAM
  (the exchange's PBKDF2 is the client's, `scram.hpp`).

## 1. Memory a session holds

`VmRSS` of the server, `cores = 1`, sessions opened by one client and held.

| state | 1,000 sessions | 10,000 sessions |
|---|---|---|
| handshake done, nothing run | 2.46 KiB a session | 3.00 KiB a session |
| after one statement each | 4.71 KiB a session | 4.23 KiB a session |
| server CPU over 30 s with every session idle | 0.32 s | 0.30 s |

- The server's resident size went from 30.1 MiB with no session to 59.4 MiB
  with 10,000 idle and 71.3 MiB once each had run a statement; closing them
  all returned none of it to the host (71.3 MiB still), so the figure is
  what the allocator took, not what a session frees.
- The first statement adds 1.2 to 2.2 KiB: the session's wait statistics
  (`sched::MakeSessionWaitStats`), made at its first statement.
- **Under TLS a session is twenty times dearer in memory: 59.4 KiB** each
  (1,000 idle sessions, `tls = on`: 33.1 MiB to 91.2 MiB).
- The two sweeps that walk every connection (idle portals, the lifetime
  ceiling; a quarter of 60 s each) cost nothing readable: the idle server
  used the same CPU with 1,000 sessions as with 10,000.

## 2. The standing point read beside idle sessions

Mean of 3 repeats; p0 the minimum. `idle 0 again` repeats `idle 0` after the
sessions are closed and is the floor under a delta.

| idle sessions | p0 | p25 | p50 | p75 | p95 | p99 | statements/s | server CPU / stmt |
|---|---|---|---|---|---|---|---|---|
| 0 | 60.5 µs | 70.2 µs | 71.1 µs | 72.4 µs | 77.8 µs | 90.5 µs | 13,725 /s | 30.4 µs |
| 1,000 | 66.0 µs | 70.6 µs | 71.5 µs | 72.8 µs | 78.3 µs | 91.4 µs | 13,697 /s | 30.4 µs |
| 10,000 | 63.6 µs | 70.5 µs | 71.4 µs | 72.7 µs | 77.9 µs | 89.0 µs | 13,725 /s | 30.4 µs |
| 0 again | 63.1 µs | 69.9 µs | 70.8 µs | 72.1 µs | 77.2 µs | 86.6 µs | 13,838 /s | 30.2 µs |

**Ten thousand idle sessions cost the working one nothing this cell can
read**: every percentile and the CPU a statement sit inside the spread of
the two `idle 0` arms (p50 70.7 to 71.7 µs, p99 84.9 to 96.9 µs, CPU 30.0
to 30.8 µs across the repeats).

## 3. A connect, one statement and close loop beside the standing session

One client process loops connect, one point read, close; the standing
session runs beside it (C), alone before (S) and alone after (S2). Mean of
3 repeats. The loop's rate is that one Python process's - a floor on what
the server admits, not its ceiling.

| port | arm | p0 | p25 | p50 | p75 | p95 | p99 | statements/s | server CPU / stmt |
|---|---|---|---|---|---|---|---|---|---|
| plain | S | 64.9 µs | 70.6 µs | 71.5 µs | 73.0 µs | 78.4 µs | 88.4 µs | 13,703 /s | 30.6 µs |
| plain | C | 62.6 µs | 89.5 µs | 93.9 µs | 98.4 µs | 125.4 µs | 146.5 µs | 10,430 /s | 78.2 µs |
| plain | S2 | 65.6 µs | 70.6 µs | 71.6 µs | 72.8 µs | 78.1 µs | 88.0 µs | 13,709 /s | 30.6 µs |
| TLS | S | 111.4 µs | 125.8 µs | 127.5 µs | 133.6 µs | 152.2 µs | 162.2 µs | 7,535 /s | 51.6 µs |
| TLS | C | 107.1 µs | 133.0 µs | 158.8 µs | 193.8 µs | 572.3 µs | 614.9 µs | 4,807 /s | 142.9 µs |
| TLS | S2 | 109.9 µs | 126.4 µs | 129.3 µs | 138.6 µs | 159.6 µs | 180.4 µs | 7,390 /s | 52.2 µs |

"Server CPU / stmt" in a C arm is the whole server's CPU over the standing
session's statements, the loop's share included.

| port | loop, beside the standing session | loop alone | server CPU a connection (loop alone) | the loop's own p50 / p95 / p99 (alone) |
|---|---|---|---|---|
| plain | 6,458 to 6,672 connections/s | 7,755 to 7,805 connections/s | 67.1 to 68.3 µs | 123.6 µs / 150.1 µs / 193.3 µs |
| TLS | 544 to 548 connections/s | 522 to 545 connections/s | 733.9 to 761.4 µs | 1,834.4 µs / 2,214.6 µs / 2,680.0 µs |

- **A plain connection costs its core about 68 µs** - connect, handshake,
  one point read, close - of which the point read is about 30 µs. About
  6,500 of them a second beside it move the standing session's p50 from
  71.5 µs to 93.9 µs (+22.4 µs) and its p99 from 88.4 µs to 146.5 µs.
- **A TLS connection costs its core about 750 µs, eleven times a plain
  one**, on the reactor (`tcp_server.cpp:549`): the handshake's private-key
  operation runs where the socket is read. About 545 of them a second -
  a twelfth of the plain rate - move the standing session's p50 from
  127.5 µs to 158.8 µs (+31.3 µs), its p95 from 152.2 µs to 572.3 µs and
  its p99 from 162.2 µs to 614.9 µs: a statement that arrives behind a
  handshake waits one out.

## 4. Where `Begin` refuses

| arm | sessions | refused | what was refused |
|---|---|---|---|
| each sends `BEGIN` and holds it, `cores = 1` | 1,500 | 476 | every `BEGIN` past the core's 1,024 |
| every session loops `BEGIN`, one `INSERT`, `COMMIT` for 6 s, `cores = 1` | 1,500 | 1,105 of 41,900 `BEGIN`s (2.64 %) | the same |
| each sends `BEGIN` and holds it, `cores = 2` | 1,900 of 2,048 slots | 0 | - |

- The refusal is `RESOURCE_EXHAUSTED`, no detail, **`retryable = 0`**:
  "core 0 already has 1024 transactions in flight, the most one core
  publishes (kInFlightSlotsPerCore)".
- While 1,024 transactions were held on the core, one more session's
  `SELECT` and `BEGIN READ ONLY` were answered, and **its autocommit
  `INSERT` was refused** with the same message: a writer that opened no
  transaction is refused for the transactions others hold.
- In the loop every session ends its transaction within a round trip, so
  a slot frees in well under the lock family's 1 s fault net; the 1,105
  refusals are sessions that asked while 1,024 others were between their
  `BEGIN` and their `COMMIT`.
- At `cores = 2` the kernel's spread of 1,900 sessions left neither core
  past 1,024. The skew that fills one core beside a core with room was not
  produced and is not priced.

## 5. What a capped portal holds

`C_EXECUTE {max_rows = 100}` of `SELECT * FROM dj_t` (100,000 rows), the
portal left suspended, against the same statement with `LIMIT 100`.

| statement | time to the first batch p0 | p25 | p50 | p75 | p95 | p99 | server CPU / stmt |
|---|---|---|---|---|---|---|---|
| `SELECT * FROM dj_t`, `max_rows = 100` | 19,339.0 µs | 19,650.1 µs | 19,836.5 µs | 20,045.6 µs | 21,218.2 µs | 27,604.6 µs | 20,135.1 µs |
| `SELECT * FROM dj_t LIMIT 100`, uncapped | 72.8 µs | 91.5 µs | 92.3 µs | 93.7 µs | 98.0 µs | 103.9 µs | 79.8 µs |

- **A client that asks for a hundred rows of a 100,000-row result waits for
  all of it: 19.8 ms against 92.3 µs, 215 times**, and the server spends
  20.1 ms of CPU where 80 µs would do. `max_rows` bounds delivery, not
  execution (`protocol.md` §7).
- **Each suspended portal holds 4.4 MiB** (20 portals: 35.3 MiB to
  123.6 MiB resident) - the whole encoded result - until it is closed or
  the 60 s portal timeout releases it. A session may hold 64.

## What the stage says about the order's parts

- **P1, P2** (the operator's): an idle plain session is 3 to 5 KiB and
  costs a working session nothing readable at 10,000, so the two settings
  defend against counts well past these and against TLS sessions' 59 KiB;
  nothing here gives either a default other than off (DJ-Q1, DJ-Q4).
- **P4**: the refusal is real past 1,024 open transactions a core, it
  reaches autocommit writers, and it is not retryable; under transactions
  that end within a round trip a wait of well under a second would have
  admitted every one of the 2.64 %.
- **P5b**: a capped portal's cost is the whole result, in time and in
  memory, 64 times a session.
- **DJ-Q12**: the TLS handshake is priced - about 750 µs of the accepting
  core, and a p99 near four times the quiet one for the session beside a
  loop of them. Moving it off the reactor is not in DJ.

## Archive

`bench/v3.4.17/archive/dj-s0-premise-v3.4.17-1-g054dbc5e/`: `s0.json` (the
first run: `memory` and `begin` reported, `beside` and `churn` discarded
for the host's noise), `s0b.json` (the gated `beside` and `churn`),
`s0c.json` (the gated `portal`), and each run's driver log and status.
