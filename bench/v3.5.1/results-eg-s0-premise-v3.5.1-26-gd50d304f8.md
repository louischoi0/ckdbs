# EG-S0: one client's autocommit insert beside the log's live run

**One line.** On `main`'s engine an autocommit insert is at PostgreSQL 18.7's cost while the log has one live segment (145.8 µs at `strict` and 146.8 µs at `group` against 143.8 µs) and **65 µs to 70 µs dearer while it has two**: 210.6 µs and 217.1 µs at once after a bulk load that rolled the log, back to 158.6 µs and 156.1 µs once a checkpoint has removed the old segment (so 52.0 µs and 61.0 µs at one relation size), with the server's CPU (13 µs to 16 µs an insert), one sync an insert and 809 B to 865 B logged an insert across the three states. PostgreSQL's does not move after the same load (145.1 µs). **That is scenario 1's `result-insert`**: its 60,480-bar cell loads 120,960 rows first, its log rolled once in the run, and its insert reads 215.2 µs with a p50 of 202.4 µs, the two-segment figure. The cause is one the tree already names - every sync covers every live segment (`docs/inflight/known-gaps.md`, "WAL") - and it is DY's to narrow, not EG's (EG-R2). The run also shows the order's second matter: **at `strict` on one core no commit is booked under `commit.wal_flush`**, 0 waits over 500 commits where `group` books 500.

Measured by CLA in the session that opened EG (no `ck-tester` agent was spawned). Driver: `tools/eg_s0_premise.py`, which this stage adds; the timed inserts go through clients that count on both sides.

## Stamp

| Field | Value |
|---|---|
| Version, commit | worktree `eg-one-client-durable-write` at `d50d304f8` (`v3.5.1-26-gd50d304f8`). **The measured binary is EF's**, built Release from `git archive 72580aa04`: `git diff 72580aa04 d50d304f8 -- src include tests CMakeLists.txt scripts` is empty, so both commits build one engine; the copy's sha256 is in the archive |
| Date and time | 2026-11-28 02:52:49 to 02:55:33 UTC, **under 3 minutes**, inside the 20-minute stage budget |
| KDS | `cores = 1`, `buffer_pool_frames = 65536`, `checkpoint_interval_ms` at its default of 5,000, a fresh server and data file a cell, `taskset -c 0-7`; `strict` and `group` |
| PostgreSQL | 18.7, one fresh `docker.io/library/postgres:18` container (run by `podman`) at its defaults, host network, on CPUs 0 to 7, `synchronous_commit = on` |
| Procedure | three rounds, the three engines' cells in a rotating order; driver `taskset -c 8-23`. A cell: 500 autocommit one-row inserts into a five-column relation (`idle`); a bulk load in 500-row transactions until the log has rolled - 90,000 rows on KDS in every run, and 90,000 on PostgreSQL to match; 500 inserts at once (`after-load`); then, on KDS, a wait until `sys.meta` shows one live segment again (9.7 s in every run) and on PostgreSQL 12 s; 500 inserts (`after the settle`) |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs (`df -T` at the run's start) |
| Quiet gate | before every cell, no build or test process and at most 8 % of all CPUs busy over 3 s; every cell passed at once; 1-minute load 0.19 to 0.77 |
| Rule | spread = (max - min) / median over the three rounds' means; a difference smaller than the larger spread is inside the spread |
| Comparator | none of this engine's own: the shape is new. Scenario 1's `result-insert` in EF-S2 (`results-ef-s2-matched-v3.5.1-6-g72580aa04.md`) is the figure it explains |
| Suite | not executed: no build input changed |

## The insert in three states of one server

Live segments are `1 + wal_rolls - wal_segments_removed` from `sys.meta`, read before and after each cell; server CPU is every server thread's `schedstat`; "booked" is the instance row of `sys.wait_stats`.

| engine | state | live segments | mean, median of 3 (runs; spread) | p0 | p25 | p50 | p95 | p99 | server CPU an insert | log bytes an insert | syncs an insert | `commit.wal_flush` booked |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| KDS `strict` | idle | 1 to 1 segments | 145.8 µs (145.8, 154.4, 143.5; 7.5 %) | 130.2 µs | 132.6 µs | 135.1 µs | 166.6 µs | 216.4 µs | 13.1 µs | 815.5 B | 1.00 syncs | 0 waits, 0.0 µs an insert |
| KDS `strict` | at once after the load | 2 to 2 segments | 210.6 µs (210.6, 201.3, 222.3; 10.0 %) | 180.6 µs | 196.2 µs | 200.3 µs | 236.5 µs | 406.5 µs | 13.7 µs | 864.5 B | 1.00 syncs | 0 waits, 0.0 µs an insert |
| KDS `strict` | after the settle | 1 to 1 segments | 158.6 µs (159.0, 158.6, 152.0; 4.4 %) | 134.0 µs | 137.6 µs | 144.0 µs | 183.0 µs | 381.6 µs | 14.5 µs | 808.7 B | 1.00 syncs | 0 waits, 0.0 µs an insert |
| KDS `group` | idle | 1 to 1 segments | 146.8 µs (146.8, 143.3, 151.2; 5.3 %) | 129.9 µs | 133.8 µs | 136.3 µs | 185.4 µs | 276.9 µs | 12.8 µs | 815.5 B | 1.00 syncs | 500 waits, 120.8 µs an insert |
| KDS `group` | at once after the load | 2 to 2 segments | 217.1 µs (217.1, 217.9, 213.5; 2.0 %) | 184.0 µs | 193.0 µs | 204.0 µs | 304.9 µs | 624.3 µs | 15.6 µs | 864.5 B | 1.00 syncs | 500 waits, 179.5 µs an insert |
| KDS `group` | after the settle | 1 to 1 segments | 156.1 µs (156.1, 151.0, 158.1; 4.5 %) | 130.8 µs | 136.0 µs | 143.4 µs | 183.2 µs | 357.7 µs | 13.9 µs | 808.7 B | 1.00 syncs | 500 waits, 123.6 µs an insert |
| PostgreSQL | idle | - | 143.8 µs (146.2, 142.2, 143.8; 2.7 %) | 132.3 µs | 135.3 µs | 138.0 µs | 169.1 µs | 267.8 µs | - | 227.0 B | - | - |
| PostgreSQL | at once after the load | - | 145.1 µs (144.2, 145.1, 148.2; 2.8 %) | 132.5 µs | 138.5 µs | 140.4 µs | 165.2 µs | 227.1 µs | - | 210.1 B | - | - |
| PostgreSQL | after the settle | - | 180.2 µs (180.2, 166.9, 185.0; 10.1 %) | 138.3 µs | 141.8 µs | 147.1 µs | 353.4 µs | 652.9 µs | - | 210.1 B | - | - |

- **The two-segment state costs 64.8 µs an insert at `strict` and 70.3 µs at `group`** over the idle state, against spreads of 10.0 % and 5.3 % at most. **Against the settled state, at the same relation size, it costs 52.0 µs and 61.0 µs** (outside spreads of 10.0 % and 4.5 %): about one more `fdatasync`, and near the 50 µs a segment `known-gaps.md` gives from `4cf30270`. *(Review of EG-S0: the settled state is 12.8 µs and 9.3 µs above idle, against larger spreads of 7.5 % and 5.3 %, just outside them; that residual is present with one live segment and is not the second segment's. It is not attributed: the relation's 91,000 rows, the 5 page images and a checkpoint's writeback just before the cell are candidates, none read.)*
- **Little else moves with it**: one sync an insert in every cell; server CPU 13.1 µs, 13.7 µs and 14.5 µs across the three states at `strict` and 12.8 µs, 15.6 µs and 13.9 µs at `group`; 815.5, 864.5 and 808.7 bytes logged an insert (the after-load cells 49 B more, not examined); no page image in the after-load cells and 5 in each settled one.
- **At idle the engines are level** (0.99x and 0.98x of PostgreSQL's rate, inside the spreads); at once after the load KDS is 0.69x and 0.67x.
- **The two rungs are one path for one client**: 145.8 µs against 146.8 µs idle. They differ in what is booked: `group` parks and books 120.8 µs to 179.5 µs an insert under `commit.wal_flush`; `strict` syncs on the statement's stack and books nothing.
- **KDS logs 3.6 times PostgreSQL's bytes for the same insert** (815.5 B against 227.0 B). Where the sync is one segment's that does not show in the latency; it is not examined here.
- **PostgreSQL's settled cell is dearer in its tail, not its body**: mean 180.2 µs on a p50 of 147.1 µs and a p95 of 353.4 µs, 12 s after the load. Not attributed: a timed checkpoint is unlikely under the default 5-minute `checkpoint_timeout` in a run of under 3 minutes, and 15 MB of load is far below the default `max_wal_size`; autovacuum on the 90,000 new rows is a candidate. Neither was read.

## Scenario 1, tied to it

One run of scenario 1 at 60,480 bars, `strict`, the EF drivers, with `sys.meta` read before the server stopped: `wal_rolls` 1 and `wal_segments_removed` 1 - **the log rolled once during the run** - and `result-insert` mean 215.2 µs, p0 183.0 µs, p50 202.4 µs, p99 416.8 µs. The after-load cell above is 210.6 µs, p0 180.6 µs, p50 200.3 µs. The counters are the run's end: that the roll came before `result-insert` and the removal after it is inferred from these figures and the phase order (a 5.1 s load, then the backtest), not read at the phase. The smaller sizes of EF-S2 (252 to 10,080 bars, 138.6 µs to 142.6 µs) load too little to roll a 64 MiB segment.

## Eight bookers on one core (EG-Q3's record)

| cell | bookings a second | `commit` mean | `commit` p50 | `commit` p99 |
|---|---|---|---|---|
| 8 bookers, 1 core, `strict` | 3,325.2 tps | 280.7 µs | 182.0 µs | 2,114.5 µs |
| 8 bookers, 1 core, `group` | 4,191.9 tps | 236.6 µs | 202.5 µs | 413.1 µs |

One run each, so no spread and no claim beyond the record: at `strict` every commit syncs on the reactor with the other seven sessions behind it, and `group` covers the commits one reactor iteration staged with one sync. No PostgreSQL twin drives more than one booker. The driver's `--verify 100` pass reports 34 and 37 failures of 400 (`I1 ... booked_cbm=..., SUM(freights.cbm)=...`, `logs/b8-s2-c1-{s,g}.stdout`): the driver's client-side lost update under the default isolation with more than one booker, as `results-scenario2-freight-v3.4.17-159-g9f8f8b354.md` records, not engine corruption.

## The sync, traced

One more run of the `strict` cell after the stage's review, on the corrected driver, with `strace -f -T -e trace=fdatasync` on the server (2026-11-28, about 03:07 UTC; the trace and its cells are in the archive's `strace/`). The tracer slows every call, so its latencies are not the table's; its counts are the point.

| state | `fdatasync` calls on the reactor thread | a call's duration under the tracer, median (mean) |
|---|---|---|
| one live segment, the first | 676 calls, one a commit (the idle cell's 500 and the load's) | 140.0 µs (167.5 µs) |
| two live segments | **506 pairs, two calls a commit**: the old segment, then the tail | old segment 60.0 µs (61.8 µs); tail 120.0 µs (123.1 µs) |
| one live segment, the second | 500 calls, one a commit | 115.0 µs (119.9 µs) |

**So the second live segment costs one more `fdatasync` a commit, and that call takes about 60 µs on a file nothing has written to since the roll.** It is the measured form of the 52.0 µs to 61.0 µs the table reads between the after-load and the settled cells. Nine other calls came from the WAL writer thread. Under the tracer the three cells read 183.6 µs, 268.4 µs and 182.1 µs.

## What the run teaches

1. **`result-insert`'s 0.81x is the breadth of a sync, for as long as a bulk load's segment stays live** - 9.7 s here, about two of the default 5 s checkpoint intervals; which checkpoint removed it was not read. An instance that rolls its log continuously (about 80,000 such inserts a segment) pays it on every commit between a roll and the checkpoint that follows.
2. **This is a second price for the narrowed sync.** BQ priced it against a segment roll's hold and found it unable to meet that bar (`known-gaps.md`, "WAL"); DY-S0 revised CLA's proposal on DY-Q2 to (b), "left", and DY-Q2 is unmarked. Neither had a one-client commit at 0.67x to 0.69x of PostgreSQL's rate in front of it.
3. **The wait statistics cannot see a one-core `strict` commit**: 500 commits of 146 µs to 211 µs each, 0 booked.

## What is cut, absent or unverified

- **The 10 µs or so between the idle and the settled state** (12.8 µs at `strict`, 9.3 µs at `group`) is not attributed; the settled cells follow a checkpoint and carry 5 page images each.
- **The run's bulk load** sent each batch's `BEGIN` and `COMMIT` on the driver's second session, not on the one running the `INSERT` (`tools/eg_s0_premise.py` at `b3f491527`, `Kds.batch`): each batch was one autocommit transaction, as intended, beside an empty transaction on the other session. The load books 360 syncs over its 180 batches; whether the second 180 are those empty commits was not read. The driver now sends the batch alone (review of EG-S0).
- **One relation shape, one load size, three rounds.** A third live segment was not produced.
- **Why KDS logs 815 B for the insert** is not examined.
- **PostgreSQL's settled tail** is not attributed.
- **The eight-booker cells are one run each.**

## Archive

`bench/v3.5.1/archive/eg-s0-v3.5.1-26-gd50d304f8/`: `json/` (the nine insert cells with their `sys.meta` readings before and after, scenario 1's and scenario 2's runs), `logs/` (stdout, return codes, prechecks, server configurations, scenario 1's `sys.meta` at its end), `run.sh`, the watchdog and its log, the timeline, the device, the binary's hash and the hashes of the tool copies that ran.
