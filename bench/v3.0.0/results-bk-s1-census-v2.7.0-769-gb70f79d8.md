# BK-S1's census: where a statement's server time goes, at the commit BK opens from

**Thesis.** At `v2.7.0-769-gb70f79d8` (`b70f79d8`, BK's opening commit, engine code identical to `ac04b84e`),
**the largest single part of every point statement's server CPU is the socket send, and half of it is a second
`send()` the server need not make**: a pipelined PARSE/BIND/EXECUTE gets `S_PARSE_OK`/`S_BIND_OK` flushed the moment
the read submits the statement, then the rest at completion - two writes and two client wakeups per statement. The
sends are 38-46 % of a point statement's server CPU (`socket send` below), split evenly between the two flushes in
every point shape sampled. Second: **an UPDATE of a row with a spilled `varchar` re-appends the value it does not
assign**, and its append walks the relation's var-heap chain from the root, so the cost grows with every such UPDATE
- 37-54 % of the statement's server CPU here after 30,000 updates, and 38 var-heap pages leaked per 1,000 updates.
Parse is 4-9 %, compile 1-6 %, Waystone recording 9 % of a point `SELECT`, access statistics under 1 %. A scan row's
cost is the walk itself - visibility, decode, residual - at 92 %.

- **A** = `b70f79d8`, `v2.7.0-769-gb70f79d8`. Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target
  `kds_server`; the profiled copy is the same commit built with `-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer`. Hashes
  in `archive/bk-s1-census-v2.7.0-769-gb70f79d8/binaries.txt`; every server ran from a copy under
  `/home/cdkbs/bench-runs/bk/bin/`.
- Drivers: `tools/bk_census.py` (this commit's), `tools/kds_sampler.c` (an `LD_PRELOAD` SIGPROF sampler: `perf` is
  refused on this host, `perf_event_paranoid = 4`), `tools/kds_sampler_report.py`, `tools/bk_parts.json` (the part
  regular expressions), `tools/bk_census_table.py`.
- Raw output: `archive/bk-s1-census-v2.7.0-769-gb70f79d8/` - the A/B census JSON (`ab-wip.json`, whose B side is a
  work-in-progress binary of BK-S4 and BK-S5 and is **not** quoted here), the profile JSON, the parts table and each
  profiled arm's top-25 report.

| Item | Value |
|---|---|
| Executed | 2026-10-11, census 07:58-08:07 UTC, profile 08:09-08:15 UTC |
| Worktree | `worktree-bk-read-update-paths`; A built from `b70f79d8` before any `src/` change of BK |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure; server pinned to CPU 2, driver to CPU 4; data and WAL under `/home/cdkbs/bench-runs/bk/` on `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`; fresh server and data file per run; ports 31931/31932 (census), 31931 (profile) |
| Host load | **Busy.** Loadavg (1 min) 13-20 throughout; 7-18 competing `cc1plus`/`cmake --build`/`ctest` processes from other sessions in every run's before and after list (per run and, from this commit's driver on, per cell, in the JSON). Wall-clock latencies from this run are **not** quoted as engine numbers: a point statement read 150-180 µs at p50 against ~50 µs on a quiet host (BJ's close). What is quoted is **on-CPU time** - the server's from every thread's `schedstat`, per statement - which a busy host inflates less, and sampled shares of it |
| Suite | Not part of a measurement; BK-S1 changes no engine code |

## Method

Per run, `tools/bk_census.py` loads `t`, `r` (1,000 rows each), `s` (10,000), `sp` and `sa` (1,000 rows, a 300-byte
`varchar` cell each, spilled), then runs each shape over both driver paths - `literal` (an unnamed `C_PARSE` of the
statement's text per execution, what every driver in `tools/` does) and `prepared` (one named `C_PARSE`, then
BIND/EXECUTE per execution, `tools/kwp.py`'s new `execute_prepared`) - 3,000 point statements, 60 every-row UPDATEs
and 192 scans per arm, five runs. The server's on-CPU time is the sum of `/proc/<pid>/task/*/schedstat`'s first field
across the arm, per statement; it includes the kernel time of the server's syscalls (a loopback `send()` delivers
into the client's socket and wakes it inside the sender's syscall).

The profile pass ran A's frame-pointer build under the sampler at 1 kHz of process CPU time, 30,000 statements per
point arm (60 every-row, 600 scans), the sampling armed and dumped per arm by signal. Each sample is assigned to the
first part in `tools/bk_parts.json` any frame of its chain matches; the shares are of samples, and the µs beside each
are that share of the arm's measured server CPU.

## 1. Server CPU per statement (A, median of five runs)

| shape | path | server CPU µs | driver CPU µs |
|---|---|---|---|
| `noop` (a `C_PING` frame: the request path, no dispatcher) | literal | 24.9 | 30.9 |
| `SHOW META` | literal / prepared | 59.6 / 62.0 | 56.0 / 55.5 |
| `SELECT * FROM t WHERE id = ?` | literal / prepared | 70.2 / 75.1 | 79.9 / 78.4 |
| `UPDATE t SET v = ? WHERE id = ?` | literal / prepared | 66.3 / 61.1 | 66.9 / 62.2 |
| `UPDATE t SET v = v + 1 WHERE id = ?` | literal / prepared | 72.7 / 70.8 | 64.4 / 58.3 |
| `UPDATE sp SET v = ? WHERE id = ?` (spilled cell not assigned) | literal / prepared | 81.7 / 82.2 | 66.6 / 62.7 |
| `UPDATE sa SET txt = ? WHERE id = ?` (spilled cell assigned) | literal / prepared | 89.6 / 85.3 | 70.4 / 67.0 |
| `INSERT INTO i VALUES (?, ?, ?)` | literal / prepared | 62.9 / 62.0 | 65.0 / 61.7 |
| `UPDATE r SET v = ? WHERE id >= 1` (1,000 rows) | literal / prepared | 1,906 / 1,793 | 144 / 141 |
| `SELECT * FROM s WHERE w = ?` (10,000 rows, unindexed) | literal / prepared | 1,181 / 1,180 | 133 / 124 |

A thousand updates of an unspilled row allocated 0 pages in every run; a thousand of a spilled row, assigning
another column, **38 pages in every run** - one 300-byte copy per update, about 26 to a page.

## 2. The parts (A's profile, share of server CPU and µs per statement)

| shape (literal path) | send | read + epoll wait | KWP frames | parse | compile | Waystone + access stats | var-heap append | undo + WAL | txn + locks | descent + pages | decode/encode | scaffolding |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `noop` | 61 % (11.5) | 34 % (6.5) | 2 % | - | - | - | - | - | - | - | - | - |
| `SHOW META` | 43 % (25.9) | 15 % (9.0) | 6 % (3.6) | - | - | - | - | - | - | - | - | 30 % (17.9, its text) |
| point `SELECT` | 39 % (25.9) | 15 % (9.7) | 7 % (4.4) | 5 % (3.7) | 4 % (2.9) | 9 % (5.7) | - | 2 % | 1 % | 1 % | 1 % | 12 % (8.2) |
| point `UPDATE`, literal | 39 % (24.1) | 15 % (9.4) | 7 % (4.2) | 4 % (2.6) | 1 % (0.8) | - | - | 6 % (3.9) | 5 % (3.1) | 4 % (2.5) | 1 % | 11 % (6.7) |
| point `UPDATE`, `v + 1` | 38 % (24.6) | 14 % (8.9) | 6 % (4.2) | 9 % (5.9) | 3 % (2.0) | - | - | 6 % (3.7) | 5 % (3.0) | 4 % (2.9) | 1 % | 9 % (6.0) |
| spilled row, cell not assigned | 25 % (19.3) | 8 % (6.1) | 3 % | 3 % | 1 % | - | **37 % (28.3)** | 4 % | 2 % | 4 % | 3 % | 6 % |
| spilled row, cell not assigned (prepared, run second) | 18 % | 7 % | 2 % | 2 % | 1 % | - | **54 % (47.1)** | 3 % | 2 % | 3 % | 2 % | 4 % |
| spilled cell assigned | 27 % | 9 % | 7 % | 4 % | 1 % | - | **32 % (18.6)** | 4 % | 2 % | 4 % | 3 % | 5 % |
| spilled cell assigned (prepared, run second) | 16 % | 5 % | 2 % | 3 % | 1 % | - | **60 % (64.1)** | 2 % | 1 % | 3 % | 2 % | 3 % |
| `INSERT` | 46 % | 15 % | 7 % | 5 % | - | - | - | 5 % | 4 % | 4 % | 1 % | 9 % |
| every-row `UPDATE` (per statement of 1,000 rows) | - | - | - | - | - | - | 1 % | 41 % (670) | 2 % | 55 % (899) | - | - |
| scan (per statement of 10,000 rows) | 3 % | 1 % | - | - | - | - | - | - | 1 % | 92 % (811, the walk) | - | 2 % |

(`-` is under 0.5 %. Every arm's full row, both paths, is `archive/.../partsA.md`; the top-25 functions of each are
`profA-reports.txt`. The every-row and scan arms carry 92-99 samples and 450-526, so their split is coarse.)

**Which flush the sends are.** In every point arm the `__libc_send` samples split evenly between
`TcpServer::OnClientReadable` (the early flush of `S_PARSE_OK`/`S_BIND_OK`) and `TcpServer::OnStatementComplete`
(the reply): 225/299, 247/243, 212/211 and 235/208 samples in the `SHOW META`, point `SELECT` and point `UPDATE` arms
(the first, smaller profile of the same binary, `prof1`, same host and hour). The prepared path flushes `S_BIND_OK`
early the same way.

**Why the spilled arms grow.** `varheap::ChainAppend` walks the relation's chain from its root to the tail on every
append, and every UPDATE of a spilled row appended a copy whatever it assigned, so each arm lengthened the chain the
next one walked: the prepared arm, run after the literal one, spends 47-64 µs a statement where the literal spent
19-28.

## 3. What BK-Q1's bar says of each proposal

The bar (adopted): a part is material when it is at least 5 % of its statement's or row's server time in a clean
cell and the predicted win exceeds the run-to-run spread. On this host the server's CPU per point statement moved
6-22 µs between runs of one binary (`ab-wip.json`, A side), so a win is judged by an interleaved pair, whose
per-run difference moves far less (the close's file states it per cell).

| proposal | the part it cuts | share | verdict |
|---|---|---|---|
| **BK-R7** (new): a statement's reply leaves in one write | the early flush, half of `send` | 19-23 % of every point statement (≈ 12 µs) | **Material, ranked first** - BK-Q5 gives BK-S4 to it |
| BK-R3: an unassigned spill is carried | the re-append, and the walk it lengthens | 37-54 % of a spilled row's UPDATE, growing; 38 pages per 1,000 updates | **Material** (BK-S5 was due on BK-Q4 alone) |
| **BK-R8** (new): an append starts at the chain's tail | the walk from the root | 32-60 % of an UPDATE assigning a spilled value, growing with the chain | **Material** |
| BK-R2: decode and encode by column | decode/encode on a point UPDATE; every-row UPDATE's per-row work | 1-3 % of a point UPDATE; the every-row arm's split is too coarse here - re-profiled after BK-S5 | Point: not material. Every-row: decided on the re-profile |
| BK-R1: compile once, bind many | parse + compile (+ the PARSE frame's fingerprint) | 5-12 % of a point statement | Material by the bar on the prepared path; its form is decided after BK-S4 and BK-S5 move the denominators |
| BK-R4: the read path's largest part | for a point `SELECT`, the double send (BK-R7); for a scan row, the walk | as above | BK-S4 = BK-R7. The scan row's walk is one undivided 92 % at this resolution |
| BK-R5: byte-range undo and WAL records | the size of undo and WAL records | the every-row arm's 41 % is per-record work, not bytes: a 3-column row's image is 44 bytes | Decided on the every-row re-profile |
| BK-R6: access statistics per core | `Catalog::RecordAccess` | **0.7 %** of a point `SELECT` | **Not material - discarded** |

Waystone's recording and replay attempt (`RecordExecution`, `TryReplay`) are 7-9 % of a point `SELECT` by pk, a
part no BK proposal names; it is reported, not cut (`known-gaps.md`).

## 4. What this census did not do

- **No wall-clock number from this host is an engine number.** The close re-measures A against BK's last commit,
  interleaved, and states the host it got.
- **The trace spans `trace.hpp` names were not built**: the sampler splits the same parts without code under
  `src/` and without the per-row clocks §1.4 of the order rules out (BK-Q11).
- **The schema version word's rate under load was not counted**: it prices the invalidation of a cached plan, and
  whether BK-R1 caches one is what §3 leaves open.
- The request path's remaining floor - `noop`'s 25 µs of server CPU, 61 % in its one `send()` and 34 % in the read
  and the wake - is the kernel's loopback work and the reactor's wake, which no BK proposal reaches (BK-Q8).
