# KDS Server Manual

Building, running, configuring and operating `kds_server`, verified against
`src/server/main.cpp`, `scripts/*.sh`, `CMakeLists.txt`, `kds.conf.sample`
and `docs/spec/client-manual.md` as of 2026-08-10. For the SQL surface see
`manual/sql/sql.md`; for the wire-level command reference see
`docs/spec/client-manual.md`.

---

## 1. Building

Requirements: CMake ≥ 3.20, a C++20 compiler. The build has no external
dependencies apart from GoogleTest for the test targets
(`KDS_BUILD_TESTS`, default `ON`).

```sh
scripts/build.sh          # configure + build into ./build (Debug)
scripts/test.sh           # build, then ctest --output-on-failure
```

`build.sh` resolves paths from its own location, so it works from anywhere.

**Debug is the default build type** (`CMakeLists.txt` sets it when unset).
Two trees exist by convention:

- `./build` — Debug. Development and tests.
- `./build-release` — Release. **Every measurement must come from here** —
  Debug has reported the wrong sign of a change twice
  (`docs/inflight/in-progress/workplan-aggregate-perf.md`).

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"
```

## 2. Running

```
usage: kds_server [<data_file>] [--config <path>] [--port <n>]
                  [--log-file <name>] [--log-dir <dir>] [--log-level <level>]
```

```sh
./build/kds_server --config kds.conf.sample   # kds.db, port 15432, ./kdb.log
./build/kds_server --config kds.conf          # from your own settings file
scripts/run.sh                                # build + run with kds.conf.sample
scripts/stop.sh                               # graceful stop (sends STOP via the CLI)
```

**A config file is required to start**: `buffer_pool_frames` has no
default, so a bare `./build/kds_server` is refused at startup.
`kds.conf.sample` sets it to 131072 frames (1 GiB of page memory).

**In Docker**, `compose.yaml` at the repository root builds a Release image
and runs it with `docker/kds.conf` (`listen_address = 0.0.0.0`, 256 MiB of
page memory, data under the `kds-data` volume):

```sh
docker compose run --rm cli    # build, start the server, open a SQL prompt
docker compose up -d kds       # the server alone, published on 127.0.0.1:15432
docker compose down            # stop it (SIGTERM: a clean shutdown checkpoint)
```

The port is published on the host's loopback only, because `auth` is off;
set `auth = scram` and `tls = on` in `docker/kds.conf` before publishing it
wider. A container build has no `.git`, so the version a backup manifest
records is `unknown`.

On startup the server prints the data file, page count, superblock version,
log destination and the port, then blocks in `Serve()` for the life of the
process. At `log_level = info` the log also carries, after recovery, one
Waystone line, `INFO [waystone] waystone_recording=<on|off>
waystone_replay=<on|off> waystone_opted=<n>`: the two keys and how many
query shapes are opted in. With `waystone_opted=0` nothing records,
whatever the keys say (`manual/sql/sql.md`, `WAYSTONE ON`). Each start
with `waystone_recording = on` also gives every opted shape a fresh, empty
trail directory (with it off, none is made), and at `log_level = info` logs
`INFO [waystone] <n> opt-ins lapsed at fingerprint version <v>` when a newer
build stopped honouring opt-ins made under an older fingerprint. The data file is created if absent; the WAL segments live in
`<data_file>.wal/` unless `wal_dir` says otherwise. Every volume this build
mounts has **one WAL stream for the instance**, whatever `cores` says, and
`SHOW META`'s `wal_topology` field says so. A volume whose superblock
records more than one stream **does not mount**: there is no migration,
and the refusal names it. Recreate the database. The same holds
for every volume older than superblock version 39 (2026-11-21): 20 keeps a
B+ tree leaf in key order by placement, 21 records a dropped relation's
roots in its tombstone, 22 carries the 48-bit Keystone id, 27 marks a
unique index, 28 orders a secondary index by its whole entry, 29 logs
the removal of a dropped index's anchor slot, 30 stores a heap as a
B+ tree with a hidden tuple key, 31 grows the catalog onto ordinary
pages, which lifts the old limit of about 800 tables, 32 marks a
dropped column whose bytes `PURGE DROPPED COLUMNS` has cleaned, 33
(2026-10-21) makes a `sys.patterns` row a Waystone opt-in rather than a
shape traffic registered, 34 (2026-10-23) marks an index an online
`CREATE INDEX` is still building, and 35 (2026-11-02) lets a checkpoint
list the assertion reservations of an undecided transaction, so a crash
gives them back, 36 (2026-11-18) keeps the join planner's relation
statistics across a clean shutdown in `sys.statistics`, 37 (2026-11-18)
keeps captured join orders in `sys.plans`, 38 (2026-11-20) names a
Waystone opt-in under the relations its example bound, and 39
(2026-11-21) stores a heap as data pages addressed by rowid, found by key
through a Keystone index. **A version-38 volume does not mount under this
build, and a backup taken from one is refused by its restore**: recreate
the database, or keep the build that wrote it.

The listener is **loopback only** (`127.0.0.1`) unless `listen_address`
says otherwise, and by default plain TCP with no authentication: TLS 1.3
and SCRAM are off until `tls = on` and `auth = scram`. The port speaks the
binary wire protocol, KWP/1 (`docs/spec/protocol.md`).

**Every core accepts on the port.** With `cores > 1` each core binds `port`
with `SO_REUSEPORT` and the kernel decides which core accepts a connection,
under the same `tls`, `auth` and statement limits as on any other. A client
cannot choose its core, and **a session does not keep it**: at the start of
each transaction the server may move the session to a core that has been
markedly less busy, and the transaction then runs to its end there. A
session moves at most once per half second, and a session whose own work is
what makes its core busy is not moved. `SO_REUSEPORT` has one cost worth knowing: another process running as
the same user can bind the same port and silently take a share of the
connections, so do not start two instances on one port. With `cores = 1`
the port is bound exclusively. The `peer_listeners` key is refused at
startup.

Where the platform refuses `SO_REUSEPORT`, the server does not fail: it
logs a `WARN` from `expeditor` ("the port cannot be shared ... core 0
accepts and hands connections off"), core 0 binds the port exclusively and
accepts every connection, and each is handed round-robin to a core — core 0
included — from where its session may move between transactions as above. No configuration
selects this; Linux never refuses the option, so a Linux deployment does
not see it.

Shutdown: send `STOP` (what `scripts/stop.sh` does), which flushes and
persists pages before exiting — the clean path. A kill signal is a crash by
definition; see §6 for what that loses.

## 3. Configuration

Precedence, later wins: **built-in defaults → config file (`--config`) →
command-line flags.** Format is `key = value`, `#` comments;
`kds.conf.sample` is the commented template. **An unknown key is a startup
error, not a warning** — so are duplicates, malformed lines and
out-of-range values, each naming the file and line.

| Key | Default | Meaning |
|---|---|---|
| `data_file` | `kds.db` | Data file path (also the positional argument). |
| `port` | `15432` | TCP port of the default (KWP) listener. |
| `listen_address` | `127.0.0.1` | **The IPv4 address the default port binds**, on every core; `0.0.0.0` is every interface, which is what a container needs to publish the port. A name or anything that is not a dotted IPv4 address is refused at startup. `debug_text_port` and `kwp_port` stay loopback whatever it says. With `auth = off` a non-loopback address serves anyone who can reach it. |
| `tcp_keepalive_s` | `60` | **Seconds of silence before an accepted client socket is probed.** Six probes follow a sixth of this apart, and `TCP_USER_TIMEOUT` is set to the same total, so a client that vanished without closing - power lost, a cable pulled - is reaped after about twice this. Every listener: the default port, `debug_text_port` and `kwp_port`. A live idle client is never ended by keepalive - `idle_session_timeout_s` is what ends one, where set - **but a live client that leaves a reply unread (a zero receive window) for about twice this is**, which is what the user timeout costs. `0` turns keepalive off; above `32767` (Linux's `MAX_TCP_KEEPIDLE`) is refused at startup. |
| `max_connections` | `0` | **How many sessions the default port serves**, on every core together. `0` is no limit. A session is a connection that has finished its handshake - authenticated, where `auth = scram` - until it closes; past the cap a new one is refused with `RESOURCE_EXHAUSTED` and the connection-limit detail, the key named in the message, and closed. **A cancel is always admitted**: a `C_CANCEL` connection is not a session and takes no seat, so a full server still cancels. `debug_text_port` and `kwp_port` are neither counted nor capped. A connection that has not finished its handshake is not counted - so a client without credentials cannot use up the cap - and is ended by `idle_session_timeout_s` where that is set, and by keepalive when its peer is gone: the cap bounds sessions, not sockets. Read the count as `connection_sessions` in `sys.meta`. At most `1000000`. |
| `reserved_admin_connections` | `3` | **Seats past `max_connections` kept for `admin` sessions**, so a full server does not lock out the operator who would raise the cap. In force only with `max_connections` set and `auth = scram`: a session is seated once it has authenticated, a `readonly` or `readwrite` one while fewer than `max_connections` are held and an `admin` one while fewer than `max_connections + reserved_admin_connections` are. With `auth = off` every session is `admin` and nothing is reserved - the limit is `max_connections` itself - and naming this key there, or without `max_connections`, is refused at startup. `0` reserves nothing. |
| `idle_session_timeout_s` | `0` | **Seconds a connection may sit idle before the server closes it.** `0` is no timeout. Idle means no statement running, no transaction open, no portal held and no statement run: an open transaction is `txn_lifetime_ceiling_ms`'s to end and a held portal the portal timeout's (60 s), and only a connection holding neither is closed. A held portal is one bound and not yet executed, or suspended with rows left; one whose result has been delivered whole is not held, closed or not. **A `C_PING` does not count as activity** - a pool that keeps connections open by pinging must set this to `0` or above its own idle period - and neither does a `C_SYNC` or a `C_PARSE`; a statement does. The client is told first - `RESOURCE_EXHAUSTED` with the idle-session-timeout detail and the key named, or an `ERR` line on `debug_text_port` - so a pool can tell this close from a failure; nothing is lost and reconnecting is the remedy. The close comes no sooner than the timeout after the last statement ended and within a quarter more of it. Applies to the default port and `debug_text_port`, `admin` sessions included, and to a connection that never finished its handshake; not to `kwp_port`. A value from `1` to `59` is refused at startup: a session holding a portal is not idle until the 60 s portal timeout releases it. |
| `wal_dir` | `<data_file>.wal` | WAL segment directory. |
| `cores` | `1` | Reactor cores, pinned one per CPU. **The count is not pinned to the volume**: a mount under a different count records the new one in the superblock and logs the change. Above 1 the cores share one WAL stream and one log: core 0 owns it, peers append through it, and every `fdatasync` is issued once (`docs/spec/wal.md` §3). |
| `inline_cell_width` | `64` | Bytes every `varchar` occupies inside a tuple. **Pinned at bootstrap**, mount-checked; changing it for existing data is a rebuild, no migration. Range 16..4096. |
| `isolation` | `read committed` | The level a connection starts at; overridable per session (`SET ISOLATION LEVEL`) and per transaction (`BEGIN ISOLATION LEVEL`). |
| `durability` | `group` | `strict`/`d1`, `group`/`d2`, `relaxed`/`d3` — applied at COMMIT for every logged statement (INSERT/UPDATE/DELETE). Instance-wide; the per-transaction class is a KWP/1 field, not wired. |
| `wal_drain_interval_us` | `1000` | WAL drain cadence; bounds a `relaxed` commit's loss window. `0` disables. |
| `relaxed_flush_interval_us` | `10000` | How long a `relaxed` commit may sit unsynced. |
| `checkpoint_interval_ms` | `5000` | Dirty-page flush cadence **of the instance**: one checkpoint starts each interval whatever `cores` is, the cores taking turns, so a core's own checkpoint comes every `cores` intervals and a restart after a crash replays up to about `cores` intervals of log. `0` disables — durability then rests on `SYNC` and clean shutdown alone. |
| `max_rows_touched` | `100000000` | Per-statement tuple ceiling; exceeding it is `ResourceExhausted`. An availability knob, not a performance one. `0` = unlimited. |
| `waystone_recording` / `waystone_replay` | `on` / `on` | **Ceilings over the shapes an operator opted in** with `WAYSTONE ON` (`manual/sql/sql.md`), boot-only. Since superblock 33 nothing records until a shape is opted in, so `on` here no longer means every repeated `SELECT` records: it means an opted shape may record (and replay). With `waystone_recording = off` an opt-in is accepted, gets no trail directory and records nothing; the next start with it on gives it one. Turning either off must never change a reply (invariant 8). The boot log and `SHOW META`'s `waystone` section print both with the opted count. |
| `access_statistics` | `on` | Per-shape access recording for `SHOW ACCESS`, into memory (emptied by a restart). Off records nothing and allocates nothing. |
| `access_stats_bytes_per_core` | `327680` | The bytes of one core's part of the access statistics: 40-byte slots, half of them admitted as shapes - the default admits 4,096 a core. `[80, 268435456]`; `0` refused. A shape past a part's bound is dropped and counted in `SHOW META`'s `access_stats_dropped`. |
| `decay_half_life` | `600` | Seconds for an untouched decay score to halve. `0` refused. |
| `cabin_optimizer` | `off` | The Cabin controller (PHY04): `on` lets it CREATE/EXTEND/HEAL/DROP Observational Cabins for `CABIN AUTO` columns. Tuning keys (`cabin_optimizer_page_budget`, `_theta_*_pct`, `_confirm_snapshots`, `_amort_windows` — 64, the build-cost amortization window ratified from `bench/results-cabin-optimizer-days.md` — `_cooldown_half_lives` — 128, the DECAYING dwell, its own parameter since 2026-08-10 and the one that provides overnight survival — and `_snapshot_interval_ms`) documented in `kds.conf.sample`. |
| `max_insert_rows` | `1024` | Cap on rows in one multi-row `INSERT ... VALUES (...), (...)`. Over-cap refuses the whole statement, inserting nothing. |
| `cabins` | `on` | Whether Cabins may be built and served. Does nothing until a `CREATE CABIN`. |
| `indexes` | `on` | Whether a secondary index may be **read**. Off takes the walk instead; replies are byte-identical. There is deliberately no maintenance switch — that is `DROP INDEX`. |
| `cabin_max_values` / `cabin_max_entries_per_value` | `4096` / `4096` | Cabin caps. A cap refuses to observe, never truncates. |
| `aggregate_max_groups` / `aggregate_max_distinct` | `65536` / `1048576` | Aggregation caps. A cap fails the statement, never truncates. |
| `sort_max_rows` | `1048576` | How many rows one `ORDER BY` may hold. Fails the statement naming the key; never truncates, never spills. A `LIMIT` caps what is held at `offset + limit`, so this binds only an unlimited sort — and `ORDER BY <tuple key>` ascending is elided rather than sorted, so it is never bound at all. |
| `join_order` | `fixed` | **Who may change the order a `SELECT`'s inner joins run in.** `fixed`: the engine chooses a statement shape's order once - the first time every relation of the join has a published size in the plan statistics - and runs it from then on, so no plan changes on its own; a captured order is written to `sys.plans` within about a second, off the statement, and is in force again after a restart - and after a crash once the log was next synced (the next commit, or the next checkpoint under a read-only load); one a crash loses is captured again; `DROP PLAN <name>` removes one and its shape is captured again. `planned`: the engine chooses at every compile, so a plan follows the statistics. `written`: the order the statement wrote. Any other value refuses the boot. An admin changes it for the run with `SET JOIN_ORDER planned \| fixed \| written` (every session, from its next statement; a restart reads this key again; captured orders are kept across a switch). `SHOW META`'s `join_order` block shows the value in force and `captured_plans`, `captured_served`, `capture_deferred`, `capture_unkeyed`, `capture_over_ceiling`, `capture_declined`, `captured_pending`, `captured_persisted`, `captured_loaded`. |
| `lock_wait_fault_net_ms` | `1000` | **How long a statement may wait before the engine calls the wait a fault.** Not the ordinary end of a wait: a statement waiting for a row, a range or a relation is woken by the holder's decide whenever that comes, and this bound is for the case where no decide comes — a detector that missed a cycle, or a holder that is stuck. It aborts the **waiter**, never the holder, and logs the fault as one. `0` refuses at once instead of waiting, which is the other policy rather than an off-switch. **A file that sets `in_doubt_ceiling_ms` is refused at startup naming this key.** |
| `txn_lifetime_ceiling_ms` | `60000` | **How old an explicit transaction may get before the server rolls it back**, counted from its `BEGIN`: busy or idle, poisoned or not, DDL inside it included. The client's next statement is refused `RESOURCE_EXHAUSTED` with the transaction-lifetime-ceiling detail - `COMMIT` included, because the writes are already undone - until it sends `ROLLBACK`. A running statement is never interrupted; the rollback comes at the first sweep after it ends, and the sweep runs every quarter of the ceiling. Autocommit statements are not bounded by it, and a KWP load session (`kwp_port`) is exempt: a load is one transaction and may run as long as it needs. `0` is refused: the ceiling has no off-switch. |
| `ash_enabled` / `ash_interval_ms` / `ash_capture_arg_hash` / `ash_ring_bytes_per_core` | `on` / `1000` / `off` / `4194304` | **The active-session sampler behind `sys.ash`.** Once per interval each core records one row per task that is not idle (a statement's wait, its run-queue time, a background task) into a fixed in-memory ring that overwrites its oldest; a restart begins empty. The interval is 100-10000 ms and the ring 1-64 MiB per core, a value outside is refused at startup. `SET ASH ENABLED off\|on`, `SET ASH INTERVAL_MS n` and `SET ASH CAPTURE_ARG_HASH on\|off` (admin) change the first three while running; turning it off keeps the ring. Cumulative wait statistics (`sys.wait_stats` and the other `sys.wait_*` views) are always on and have no key. `docs/spec/wait-events.md` says what every event means. |
| `log_dir` / `log_file` / `log_level` | — / `kdb.log` / `info` | Log destination and level (`trace`..`off`). Empty `log_file` disables file logging. |

`inline_cell_width` is the one superblock-pinned key and the one that can
refuse a mount: it is read once when a *new* database is bootstrapped, and
every later start validates the running value against it. Five keys are
refused by name - `peer_listeners`, `in_doubt_ceiling_ms`,
`placement`, `range_size_ids` and `physical_optimizer` - each refusal saying what replaced it.

## 4. Connecting

One command per line in, exactly one reply line back (multi-row replies are
one response with embedded `\n`). Anything that talks line-oriented TCP
works:

```sh
python3 tools/ckdbs_cli.py                          # interactive REPL
python3 tools/ckdbs_cli.py PING                     # one-shot
python3 tools/ckdbs_cli.py "SELECT * FROM accounts WHERE id = 1"
python3 tools/ckdbs_cli.py -f schema.sql -f load.sql > out.txt
python3 tools/ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES
```

The CLI ships lines verbatim and prints replies; SELECT results are
rendered as a table client-side (pandas when available). Script files
split statements on `;` (or one per line when a file has no `;`).

`tools/run_sql.py` runs `.sql` scripts with inline expectations
(`-- expect:` / `-- reject:` / `-- error:` / `-- rows:`) — the ad-hoc test
harness.

One statement is in flight per connection; replies leave in arrival order.
A connection that closes with a transaction open gets it rolled back.

## 5. Operating

**Durability points.** Dirty pages reach the data file on the checkpoint
cadence, on client `SYNC`, and at clean shutdown. The WAL narrows the gap
for logged statements (INSERT/UPDATE/DELETE at the configured durability
class), and every mount runs recovery from the log (below), so a crash loses
only what the log had not yet made durable, which the durability class
bounds. WAL-before-data is store-enforced regardless (a frame is not flushed until
its log is durable).

**A read that misses the buffer pool.** A page that is not in the pool is
read from the data file. For most statements that read is made on the
core's own thread, and every other session on that core waits for the
device while it lasts (`io.page_read` in `sys.wait_stats` is where the time
shows). Two kinds of statement give the core back instead, when the page is
not in the operating system's cache either: a plain `SELECT` by tuple key,
key list or probe, and a `SELECT` that walks a relation. Such a statement
is suspended while a reader thread fetches the page, and the core serves
its other sessions meanwhile; `pool_would_wait` in `SHOW META` counts the
reads that left a core this way and `contention_miss_reruns` the point
reads that were started over for one. Writes, locking reads, a `SELECT`
under `ANALYZE`, a query the engine answers through its general query layer
(for example outer joins and set operations), joins' inner lookups
and the reads under a walked page (an index's row fetch, a spilled value)
still read on the core. The server starts four
reader threads at boot for this, at every `cores` setting; there is no
setting for them. A pool sized to the working set (`buffer_pool_frames`)
remains the remedy: the mechanism bounds what a miss costs the sessions
that did not miss, not what it costs the one that did.

**The log.** One line per event:
`<unix_seconds> <LEVEL> [component] message`, opened append-only so a
restart preserves why the last run died. Levels are a cost contract:
`info` (default) is per lifecycle event; `debug` is per request; `trace` is
per tuple — a development tool, not an operating mode. Successful replies
are summarized, never echoed; failed replies are logged in full. Component
tags and per-level detail: `docs/spec/client-manual.md` §1.

**Health and inspection.** `PING` → `PONG`. `SHOW META` (instance),
`SHOW TABLES` / `DESCRIBE`, `SHOW BUDGET` (Keystone id headroom, with
`warning=`/`exhausted=` counts), `SHOW ACCESS` (access shapes),
`SHOW PAGE <id>
[VALUES]` (page-level debugging). Full list: `manual/sql/sql.md` §6.

**Multi-core.** `cores > 1` spawns one pinned reactor thread per core.
No relation is tied to a core, and every statement runs on the core its
session is on; a namespace groups relations and decides nothing about cores
(`docs/spec/namespace.md`).
The cores share **one WAL stream** — core 0 opens the log and every peer
appends through it — so a peer's `SHOW META` WAL block reads zero syncs by
construction and the instance's durability cost is read on core 0
(`docs/spec/wal.md` §3). `waystone_recording` and `access_statistics` mean
the same thing on every core: a peer records an opted shape's trail and
its own access shapes itself, and `sys.patterns`, `sys.access_stats`
and the trails are the instance's one - the access statistics one store
with a part per core, merged when read. A `WAYSTONE ON` on one core reaches
every other core within one statement.
Recovery runs once, on core 0, before any peer exists.
`SHOW META`'s `contention_*` fields, and since BQ-S1 `wal_append_lsn` and the
segment roll's `wal_rolls`/`wal_roll_*`, are the exceptions to "read on core
0": the first are summed over every core and count where one core waited for
another; the second are the one stream's totals, printed the same on every
core and never summed, and they include rolls on peers, whose inline
syncs `wal_syncs` never counts (`docs/spec/client-manual.md`).

**Which CPU each reactor runs on.** By default a peer reactor `k` is pinned
to CPU `k` and core 0 runs unpinned, which on a host that numbers SMT
siblings adjacently puts reactors 0 and 1 on one physical core.
`reactor_cpus = 0,2` pins reactor `k` to the `k`th CPU listed, core 0
included; it must name exactly `cores` distinct CPUs this machine has, or
the server refuses to start.

## 6. What a restart loses — known gaps

The engine-wide list lives in **`docs/inflight/known-gaps.md`** — durability and
recovery gaps, what a restart loses, multicore limits
and protocol gaps, each entry naming its owning doc. Every mount runs
recovery from the log (§5): a crash loses only what the log had not made
durable, an uncommitted transaction is rolled back at the mount, a torn
page of a logged class is healed from the log, and assertions enforce
immediately after a restart. What an operator must know before trusting
this server with data: **a torn page 0 or free-map page refuses the
mount, and a torn page of an unlogged class is refused `Corruption`**,
each lost until a restore (`docs/spec/wal.md` §10), and **memory-resident
state is gone after a restart** - Cabin entry sets, Waystone trails, the
access and statement statistics, and the planner's statistics after a
crash.
