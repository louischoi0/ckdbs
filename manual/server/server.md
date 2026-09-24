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
./build/kds_server                     # defaults: kds.db, port 15432, ./kdb.log
./build/kds_server --config kds.conf   # from a settings file
scripts/run.sh                         # build + run with defaults
scripts/stop.sh                        # graceful stop (sends STOP via the CLI)
```

On startup the server prints the data file, page count, superblock version,
log destination and the port, then blocks in `Serve()` for the life of the
process. The data file is created if absent; the WAL segments live in
`<data_file>.wal/` unless `wal_dir` says otherwise. Every volume this build
mounts has **one WAL stream for the instance**, whatever `cores` says, and
`SHOW META`'s `wal_topology` field says so. A volume written before that
change has one stream per core and **no longer mounts**: there is no
migration, and the refusal names it. Recreate the database.

The listener is **loopback only** (`127.0.0.1`), plain TCP, no TLS, no
authentication — a development/inspection surface, not a production API.
The binary wire protocol (KWP/1, `docs/spec/protocol.md`) is specified with a
handshake and auth stages, but only its frame codec exists in code.

**Every core accepts on the port.** With `cores > 1` each core binds `port`
with `SO_REUSEPORT` and the kernel decides which core accepts a connection;
the session then runs to completion on that core, under the same `tls`,
`auth` and statement limits as on any other. A client cannot choose its
core. `SO_REUSEPORT` has one cost worth knowing: another process running as
the same user can bind the same port and silently take a share of the
connections, so do not start two instances on one port. With `cores = 1`
the port is bound exclusively. The `peer_listeners` key that used to switch
this is retired and refused at startup.

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
| `port` | `15432` | TCP port, loopback only. |
| `wal_dir` | `<data_file>.wal` | WAL segment directory. |
| `cores` | `1` | Reactor cores, pinned one per CPU. **Not pinned since v3.0.0's M3**: a mount under a different count records the new one in the superblock and logs the change. Above 1 the cores share one WAL stream and one log: core 0 owns it, peers append through it, and every `fdatasync` is issued once (`docs/spec/wal.md` §3). |
| `inline_cell_width` | `64` | Bytes every `varchar` occupies inside a tuple. **Pinned at bootstrap**, mount-checked; changing it for existing data is a rebuild, no migration. Range 16..4096. |
| `isolation` | `read committed` | The level a connection starts at; overridable per session (`SET ISOLATION LEVEL`) and per transaction (`BEGIN ISOLATION LEVEL`). |
| `durability` | `group` | `strict`/`d1`, `group`/`d2`, `relaxed`/`d3` — applied at COMMIT for every logged statement (INSERT/UPDATE/DELETE). Instance-wide; the per-transaction class is a KWP/1 field, not wired. |
| `wal_drain_interval_us` | `1000` | WAL drain cadence; bounds a `relaxed` commit's loss window. `0` disables. |
| `relaxed_flush_interval_us` | `10000` | How long a `relaxed` commit may sit unsynced. |
| `checkpoint_interval_ms` | `5000` | Dirty-page flush cadence. `0` disables — durability then rests on `SYNC` and clean shutdown alone. |
| `max_rows_touched` | `100000000` | Per-statement tuple ceiling; exceeding it is `ResourceExhausted`. An availability knob, not a performance one. `0` = unlimited. |
| `waystone_recording` / `waystone_replay` | `on` / `on` | The Waystone switches. Turning either off must never change a reply (invariant 8). |
| `access_statistics` | `on` | Per-shape access recording for `SHOW ACCESS`. +1-2% on a point lookup. |
| `physical_optimizer` | `shadow` | `off` or `shadow`. `on` is **refused at startup** naming the three gates that block every plan (`docs/spec/physical-optimizer.md` §6). |
| `decay_half_life` | `600` | Seconds for an untouched decay score to halve. `0` refused. |
| `cabin_optimizer` | `off` | The Cabin controller (PHY04): `on` lets it CREATE/EXTEND/HEAL/DROP Observational Cabins for `CABIN AUTO` columns. Tuning keys (`cabin_optimizer_page_budget`, `_theta_*_pct`, `_confirm_snapshots`, `_amort_windows` — 64, the build-cost amortization window ratified from `bench/results-cabin-optimizer-days.md` — `_cooldown_half_lives` — 128, the DECAYING dwell, its own parameter since 2026-08-10 and the one that provides overnight survival — and `_snapshot_interval_ms`) documented in `kds.conf.sample`. |
| `max_insert_rows` | `1024` | Cap on rows in one multi-row `INSERT ... VALUES (...), (...)`. Over-cap refuses the whole statement, inserting nothing. |
| `cabins` | `on` | Whether Cabins may be built and served. Does nothing until a `CREATE CABIN`. |
| `indexes` | `on` | Whether a secondary index may be **read**. Off takes the walk instead; replies are byte-identical. There is deliberately no maintenance switch — that is `DROP INDEX`. |
| `cabin_max_values` / `cabin_max_entries_per_value` | `4096` / `4096` | Cabin caps. A cap refuses to observe, never truncates. |
| `aggregate_max_groups` / `aggregate_max_distinct` | `65536` / `1048576` | Aggregation caps. A cap fails the statement, never truncates. |
| `sort_max_rows` | `1048576` | How many rows one `ORDER BY` may hold. Fails the statement naming the key; never truncates, never spills. A `LIMIT` caps what is held at `offset + limit`, so this binds only an unlimited sort — and `ORDER BY <pk>` ascending is elided rather than sorted, so it is never bound at all. |
| `lock_wait_fault_net_ms` | `1000` | **How long a statement may wait before the engine calls the wait a fault.** Not the ordinary end of a wait: a statement waiting for a row, a range or a relation is woken by the holder's decide whenever that comes, and this bound is for the case where no decide comes — a detector that missed a cycle, or a holder that is stuck. It aborts the **waiter**, never the holder, and logs the fault as one. `0` refuses at once instead of waiting, which is the other policy rather than an off-switch. **This key was `in_doubt_ceiling_ms`, and a file that still sets that name is refused at startup naming this one** (AO-R8's plan, taken at M3's AT-S6): the old key bounded a wait on a row held by a transaction this core had prepared for a cross-owner commit, and there is no cross-owner transaction. The default was 11 s while two-phase commit was in the tree, because an honest holder could sit inside a coordinator's 10 s phase deadline; nothing waits on another core now. |
| `log_dir` / `log_file` / `log_level` | — / `kdb.log` / `info` | Log destination and level (`trace`..`off`). Empty `log_file` disables file logging. |

`inline_cell_width` is the one superblock-pinned key and the one that can
refuse a mount: it is read once when a *new* database is bootstrapped, and
every later start validates the running value against it. Four keys are
retired and refused by name - `peer_listeners`, `in_doubt_ceiling_ms`,
`placement` and `range_size_ids` - each refusal saying what replaced it.

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
class) — but **recovery is not implemented**: nothing reads the log back,
so a crash is protected only by what the last checkpoint/SYNC persisted.
WAL-before-data is store-enforced regardless (a frame is not flushed until
its log is durable).

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
`SHOW RELAYOUT` (physical-optimizer shadow report), `SHOW PAGE <id>
[VALUES]` (page-level debugging). Full list: `manual/sql/sql.md` §6.

**Multi-core.** `cores > 1` spawns one pinned reactor thread per core.
No relation is owned by a core, and every statement runs on the core its
session is on; a namespace groups relations and decides nothing about cores
(`docs/spec/namespace.md`).
The cores share **one WAL stream** — core 0 opens the log and every peer
appends through it — so a peer's `SHOW META` WAL block reads zero syncs by
construction and the instance's durability cost is read on core 0
(`docs/spec/wal.md` §3). `waystone_recording` and `access_statistics` mean
the same thing on every core: a peer writes `sys.patterns` and
`sys.access_stats` itself, and both relations are the instance's one.
Recovery runs once, on core 0, before any peer exists.

## 6. What a restart loses — known gaps

The engine-wide list lives in **`docs/inflight/known-gaps.md`** — durability and
recovery gaps, what a restart loses, the no-purge rule, multicore limits
and protocol gaps, each entry naming its owning doc. The three an operator
must know before trusting this server with data: **WAL recovery is not
implemented** (a crash loses everything since the last
checkpoint/`SYNC`/clean shutdown), **an uncommitted row surviving a crash
reads as committed on the next boot**, and **assertions report
`enforcing=0` after a restart** until recovery exists.
