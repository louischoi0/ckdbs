# Work order EN - three engines on one client: KDS, PostgreSQL and Oracle through one Rust harness, every scenario's relations their own kind, and a commercial OLTP scenario

Status: **CLOSED 2026-11-28 at S4** (`bench/v3.5.1/results-en-close-v3.5.1-63-g9543cf720.md`; the three-engine file is kept uncommitted under `bench/private/`); not landed: the work branch waits for the operator's word. **OPENED 2026-11-28** on worktree `three-engine-equal-client-bench`
(branch `worktree-three-engine-equal-client-bench`) from `e571ae574`
(`v3.5.1-57-ge571ae574`), on the operator's words of 2026-11-28
(`raft-marks-2026-11-28.md` §17). Written and opened in one session. A
benchmark order: no engine code is changed by it. **EN-Q3..Q8 adopted as
CLA proposed, on the operator's standing go-ahead of 2026-11-28
(go-ahead-achieving-milestone; `raft-marks-2026-11-28.md` §18)**.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[assumed]`, as in DT's order.

Related: `bench/README.md` (the rules this order wrote, "What a relation is
stored as", "Three engines on one client", "The Oracle control");
`bench/v3.5.1/results-ef-s2-matched-v3.5.1-6-g72580aa04.md` (EF's matched
run, the last KDS-PostgreSQL comparison on the Python clients);
`tools/kwp.py` (the reference KWP client the Rust one follows);
`tools/scenario0_stockmarket.py` .. `tools/scenario4_cabinopt_days.py` and
their `tools/pg_*` twins; `docs/spec/protocol.md` §2-§9.

## 0. What EN is

`[operator]` Seven words (§17): Oracle Database Free as a control beside
PostgreSQL, its comparison for internal analysis only and never marketing;
every scenario's relations given the kind that suits them, with no cell
comparing the kinds; missing cells measured; the scenario time budget
lifted for the session; the three engines measured through clients as equal
as possible, Rust considered; every result before 3.4.9 deleted, 3.4.9
included; a new scenario for commercial OLTP; and every case's delay
broken down by phase, with its lock events (EN-Q8).

`[measured]` The Python drivers' clients differ per engine: EF-S0 put
16.6 µs a statement of client work on the KDS side against 0.8 µs
(`bench/v3.5.1/results-ef-s0-premise-v3.5.1-6-g72580aa04.md`), and EF moved
the drivers to the cheapest mode each side has. They remain two different
Python clients, and an Oracle twin would be a third.

## 1. Stages

| Stage | What | Gate |
|---|---|---|
| **S0** | The records: §17's marks, `bench/README.md`'s three rules, `ck-tester`'s rule 4, `bench/private/` ignored, `bench/v3.0.0/`-`v3.4.9/` deleted | review |
| **S1** | `tools/eqbench/`: one Rust crate, an `Engine` trait and three backends - KWP/1 written for it after `tools/kwp.py`, PostgreSQL by the `postgres` crate, Oracle by the `oracle` crate over ODPI-C and the Oracle client libraries; the timer, the percentile table (p0, p25, p50, p95, p99, max), the client's thread CPU and the server's CPU a phase; JSON out; the setup of each server on a chosen port | review; a smoke run on each engine |
| **S2** | Scenarios 0 to 4 in the harness, each relation of the kind §2 assigns, DDL per dialect, the same statements and parameter sequence on all three | review; each scenario's checks (§3) pass on all three |
| **S3** | Scenario 5, commercial OLTP (§4) | review; its consistency checks pass on all three |
| **S4** | The measurement: every scenario on all three engines, rounds interleaved; a committed results file (KDS beside PostgreSQL) and a private one (all three, `bench/private/`) | review of the results files |

The suite: `tools/eqbench/` is no CMake target and no `ctest` cell, so
no stage changes a build input (`CLAUDE.md`, Working Rules); each stage
says "suite not executed: no build input changed".

## 2. Questions, with CLA's proposals

- **EN-Q1, where Oracle's figures live.** (a) committed with a disclaimer;
  (b) **the drivers committed, every Oracle figure under `bench/private/`,
  never committed**; (c) wait for the licence text. `[operator]` **(b)**.
  Basis: the repository is public (`gh repo view`, 2026-11-28), so a
  committed figure is published, and the Free licence's text could not be
  read (HTTP 403).
- **EN-Q2, the harness.** (a) **one Rust binary**; (b) the Python drivers
  in their cheapest modes, `python-oracledb` for Oracle; (c) both.
  `[operator]` **(a)**.
- **EN-Q3, the kind rule** (`[design]`, adopted on §17's "the type that
  suits it"): `BTREE` where the statements reach rows by the tuple key, `HEAP`
  where they reach rows chiefly through a secondary index, or only append
  and walk (`bench/README.md`, "What a relation is stored as"). Each
  scenario's table is in `tools/eqbench/README.md`.
- **EN-Q4, what "equal client" means** (`[design]`): the same scenario
  code and parameter sequence; a statement prepared once per connection
  and executed with bound values on every engine; every value decoded to a
  Rust value; one round trip a statement where the protocol allows it
  (Oracle's fetch array and prefetch set to the largest result a scenario
  reads, so a result is one round trip as KWP's batch and PostgreSQL's
  stream are). What stays unequal is each protocol's own: a KWP or
  PostgreSQL `BEGIN` is a round trip and Oracle's transaction opens on
  its first statement, and the harness's README names it.
- **EN-Q5, the durability compared** (`[design]`): every engine durable at
  commit - KDS `group` and `strict` (both acknowledged only once the commit
  record is durable, `wal.md` §8 rule 2, `protocol.md` §9), PostgreSQL
  `synchronous_commit = on` (its default), Oracle's default
  `COMMIT WRITE IMMEDIATE WAIT`. No engine is measured at a rung that loses
  an acknowledged commit.
- **EN-Q6, KDS's configuration** (`[design]`): `cores = 1` as EF, a fresh
  server and data file a cell, the server pinned to CPUs 0 to 7 and the
  harness to 8 to 23, as EF ran (the host is P/E hybrid, P-cores 0 to 7;
  `bench/v3.5.1/results-ef-s1-drivers-v3.5.1-6-g72580aa04.md`),
  PostgreSQL's and Oracle's server processes pinned the same way.
- **EN-Q7, Oracle's configuration** (`[design]`): the `gvenzl/oracle-free`
  image already on the host, host networking so no port forwarder sits in
  its path as none sits in the others', a password generated per run and
  never committed, defaults otherwise. **1521 is the port chosen** (`bench/README.md` rule 5):
  the image's listener does not move under host networking, so each cell
  checks it free (`ss -ltn`) before the container starts and the results
  file records the check.
- **EN-Q8, where a case's time goes** (`[operator]`, §17 Word 7: *"collect
  statistics for every case: the share of the delay between parse,
  optimise, execution, disk read and formatting, according to each
  database's architecture, and what lock events occurred"*; the sources
  `[design]`).
  Every phase of every scenario carries a breakdown read from the engine's
  own accounting, a delta between the phase's head and its end, read on a
  second connection outside the timed loop:
  - **KDS**: `sys.statement_stats` per shape - `compile_us` (parse,
    name resolution and plan together: the engine counts them as one, and
    a named statement's kept parse skips the parse, EA), `cpu_us`,
    `io_us` and `page_reads` (disk read), `commit_us`, `app_us`, `net_us`,
    `sched_us`, `other_us`; `sys.wait_stats` for each wait event's count
    and time, and `sys.lock_waits` for the lock waits by unit and mode.
  - **PostgreSQL**: `pg_stat_statements` per statement - `total_plan_time`
    (optimise), `total_exec_time`, `shared_blks_read` and
    `shared_blk_read_time` (disk read) - and a sampler of `pg_stat_activity`'s
    `wait_event_type`/`wait_event` for the harness's backend every 1 ms
    (the lock events, `Lock`/`LWLock`). These need
    `shared_preload_libraries = pg_stat_statements`,
    `pg_stat_statements.track_planning`, `track_io_timing`: instruments, not
    tuning, but not the defaults, so **the breakdown is a second pass** and
    the timed cells run at the defaults.
  - **Oracle**: `V$SESS_TIME_MODEL` for the harness's session - `parse time
    elapsed`, `hard parse elapsed time` (the optimiser runs in a hard
    parse), `sql execute elapsed time`, `DB CPU` - `V$SESSION_EVENT` for each
    wait event (`db file sequential read` and `db file scattered read` the
    disk reads, `log file sync` the commit, `enq: TX - row lock contention`
    and every `enq:` the locks), read in the same pass as the timed cells:
    Oracle's timed statistics are on by default.
  - **Formatting**, on every engine: the harness's own time turning a
    result's bytes into Rust values, timed apart from the round trip; the
    server's encoding of a row is inside its execution and no engine
    reports it apart.
  - A share no engine can give is written "not reported by the engine",
    never 0: KDS's parse apart from its plan, PostgreSQL's parse of a
    prepared statement (once, at the prepare, outside the loop).

## 3. What each scenario checks

The harness carries each Python driver's checks that read the engine's
state, not its plan: scenario 0's balance conservation, scenario 2's
booking verification, scenario 1's model results read back, scenario 3's
loan counts, scenario 4's answers equal across arms. A scenario whose
checks fail on an engine reports no number for it.

## 4. Scenario 5, commercial OLTP (`[design]`)

An order-entry workload after the shape of TPC-C's five transactions -
**not TPC-C**: not audited, not the specification's full rules, and its
rate is no tpmC. Nine relations (warehouses, districts, customers, items,
stock, orders, order lines, new orders, history), every key a flat
`int64`, money in minor units. The mix: new order 45 %, payment 43 %,
order status 4 %, delivery 4 %, stock level 4 %, each a transaction of
its statements; one terminal and several. Its checks: each district's
next order id is one above its highest order, every warehouse's year to
date is the sum of its districts', and every order's line count matches
its lines.

## 5. Constraints

- `Constraint`: the Oracle licence text was not read. `Effect`: EN-Q1 (b).
- `Constraint`: Oracle Database Free caps itself at 2 CPU threads and 2 GB
  of memory. `Effect`: a cell above two concurrent sessions measures
  Free's cap as much as Oracle; the results file says so where it
  applies.
- `Constraint`: Oracle Free listens on 1521 under host networking.
  `Effect`: the port is reachable from the network while a run is up; the
  password is generated per run and the container removed at the run's
  end.

## 6. EN-Q9, scenario 4's kinds (`[design]`, adopted on the operator's standing go-ahead of 2026-11-28, go-ahead-achieving-milestone)

On `off`, the arm every engine runs, scenario 4's tapes and its second
board are only walked, which EN-Q3 makes HEAP. (a) **Keep every relation
BTREE on both arms**: the scenario's subject is the declared Cabin, whose
authority is the tuple key and which resolves through the clustered tree,
and the arms keep one kind so the arm comparison isolates the Cabin; (b)
HEAP on both arms. **Adopted: (a)**, recorded in the scenario's header.

## 7. Stages as landed

| Stage | Commit | Review |
|---|---|---|
| S0 | `237896433`, `f7f2ccf4b` | 8 errors fixed by the reviewer (a quotation count, EN-Q5's and EN-Q6's citations, the heap point-read wording, a contradiction with rule 5); CLA applied its 7 reported items but the cosmetic blank line, which was fixed too |
| S1 | `6cebe2bf0`, applied in `640a27f92` | 13 correctness items. Applied: Oracle string binds as `VARCHAR2` (the crate's `NVARCHAR2` bind made a `VARCHAR2` index unusable, measured), decode from the borrowed row; PostgreSQL's typed bind wrapper; KDS's split from the measured sessions' `sys.session_waits`, lock waits per core; PostgreSQL's sampler time per sample, lock out of execution, `WALWrite` into commit; a falling server-CPU sum gives no figure; the session key retried; KWP reads to `S_READY` on every path. **Rejected**: one canonical decimal and date decode (no scenario declares a DECIMAL, DATE or TIMESTAMP column, `engine.rs`' `Val` says so); dropping `--kds-durability` (`matrix.py` passes the server's rung to both, so one run never commits at two); moving KDS into a cgroup (the per-thread `schedstat` sum with a checked difference serves); `#![allow(dead_code)]` kept for the framework's helpers that only some scenarios call |
| S2 | `640a27f92`, `9543cf720` | Ported by five passes, one a scenario. A defect found: `docs/inflight/bugs/undo-chain-cyclic-on-a-concurrently-updated-row.md` (reproduced by CLA, 3 runs of 6; 10 refusals in the close measurement, in scenario 0 at its defaults and in scenario 5). Review: 8 fixed by the reviewer (write warm-ups, scenario 5's alone phases doing unequal work, its warm-up below PostgreSQL's generic plan, figures from failed rounds, scenario 4's cell names, a torn count, a missing row passed, the matrix's rotation, kill, attempts and watchdog pid); of 9 reported, CLA applied the warm-up helper, EN-Q9, a fresh PostgreSQL a cell, scenario 5's order and in-phase clock, the watchdog's cap and the README's CPU source. **Rejected**: an `ErrClass` name helper (within a scenario every engine records the same spelling, so no comparison is affected); `Option` client CPU for scenario 0's derived phases (their `0.0` is labelled derived in `meta.derived_phases`); scenario 2's deadline (its phase ends at the booking target, the deadline a cap the defaults never reach); Oracle fresh a cell (its start takes minutes; it is fresh a round and the results say so) |
| S3 | `21a6f99ba` | reviewed with S2 |
| S4 | this close's commit | 188 runs (57 cells x 3 rounds, 17 PostgreSQL profile cells), every check passing, 08:54 to 10:11 UTC; the milestone A/B not applicable (no engine build input changed, `e571ae574` to `9543cf720`); the suite not executed (no build input changed) |
