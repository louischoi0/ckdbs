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
process. The data file is created if absent; the per-core WAL segments live
in `<data_file>.wal/` unless `wal_dir` says otherwise.

The listener is **loopback only** (`127.0.0.1`), plain TCP, no TLS, no
authentication — a development/inspection surface, not a production API.
The binary wire protocol (KWP/1, `docs/spec/protocol.md`) is specified with a
handshake and auth stages, but only its frame codec exists in code.

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
| `wal_dir` | `<data_file>.wal` | Per-core WAL segment directory. |
| `cores` | `1` | Reactor cores. **Pinned into the superblock at bootstrap**; a later mount under a different count refuses to start naming both numbers. Above 1: parallel WAL streams only — core 0 still serves every statement (`docs/spec/crosscore.md`). |
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
| `log_dir` / `log_file` / `log_level` | — / `kdb.log` / `info` | Log destination and level (`trace`..`off`). Empty `log_file` disables file logging. |

The two superblock-pinned keys (`cores`, `inline_cell_width`) are the ones
that can refuse a mount: they are read once when a *new* database is
bootstrapped, and every later start validates the running value against the
pinned one.

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

**Multi-core.** `cores > 1` spawns pinned reactor threads with their own
WAL streams, but core 0 serves every statement today; peers come up alive
and idle, with `waystone_recording` and `access_statistics` off on peers by
design. Leave it at 1 until the cross-core pipeline lands
(`docs/inflight/in-progress/workplan-crosscore.md` P4).

## 6. What a restart loses — known gaps

The engine-wide list lives in **`docs/inflight/known-gaps.md`** — durability and
recovery gaps, what a restart loses, the no-purge rule, multicore limits
and protocol gaps, each entry naming its owning doc. The three an operator
must know before trusting this server with data: **WAL recovery is not
implemented** (a crash loses everything since the last
checkpoint/`SYNC`/clean shutdown), **an uncommitted row surviving a crash
reads as committed on the next boot**, and **assertions report
`enforcing=0` after a restart** until recovery exists.
