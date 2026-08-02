# KDS Client Manual

> **Heads up:** a new binary wire protocol, **KWP/1**, is the eventual
> replacement for everything below — see
> `docs/protocol.md` (spec) and `docs/protocol-wp.md` (task breakdown).
> Once KWP/1's handshake and session layer actually exist in code, this
> newline text protocol becomes an off-by-default loopback debug surface
> and this manual gets rewritten around KWP/1. As of this note, only the
> KWP frame format itself has landed in code (`include/kds/wire/kwp.hpp`,
> `src/wire/frame_codec.cpp`) — no handshake, sessions, or client-visible
> behavior change yet, so everything documented below is still accurate
> and still how `kds_server` actually behaves today.

How to talk to the `kds_server` process from a client: the wire protocol,
the full command reference, and how to use the bundled CLI tool. This
documents the *client-facing* surface only - for server internals see
`CLAUDE.md` / `docs/overview.md`.

---

## 1. Connecting

`kds_server` listens on a plain TCP socket, loopback only, port `15432` by
default (see `kDefaultPort` in `src/server/main.cpp`). There is no TLS, no
authentication, and no wire framing beyond newlines - this is an internal
development/inspection protocol, not a client-facing production API.

Start the server:

```sh
./build.sh
./build/kds_server                        # defaults: kds.db, port 15432, ./kdb.log
./build/kds_server --config kds.conf      # or from a settings file
```

It prints the data file, the log destination, and the port it bound.

### Configuration

Settings come from three places, later winning over earlier: **built-in
defaults → config file (`--config <path>`) → command-line flags.** See
`kds.conf.sample` for a commented template.

| Key | Flag | Default | Meaning |
|---|---|---|---|
| `data_file` | positional arg | `kds.db` | Data file path; created if absent. |
| `port` | `--port` | `15432` | TCP port, loopback only. |
| `wal_dir` | — | `<data_file>.wal` | Per-core WAL segment directory. |
| `checkpoint_interval_ms` | — | `5000` | How often dirty pages are flushed (`docs/wal.md` §11). `0` disables the cadence, leaving `SYNC`/shutdown as the only durability points. |
| `durability` | — | `group` | Durability class for every **logged** statement — today `INSERT` only (`docs/wal.md` §1). `strict`/`d1` fsyncs before replying; `group`/`d2` is the same durability point with the fsync amortized over concurrent committers, which costs the same as `strict` while the server serves one connection at a time; `relaxed`/`d3` replies immediately and syncs on the drain below. Names are case-insensitive. |
| `wal_drain_interval_us` | — | `1000` | How often the WAL drain runs. Bounds a `relaxed` commit's loss window; a tick with nothing staged does no I/O. `0` disables it. |
| `inline_cell_width` | — | `64` | How many bytes every variable-width value (`varchar`) occupies inside a tuple (`docs/heap-and-tuple.md` §3.3). A longer value still stores fine — it spills to the var-heap and the cell holds a pointer — so this is a **performance** knob, not a limit: raising it keeps more values in the tuple at the cost of padding every short one. Read **once**, at the bootstrap of a new database, and pinned into the superblock; every later mount validates the running value against the pinned one and refuses to start on a disagreement, naming both. Changing it for existing data is a rebuild, which is `Unsupported` — there is no migration. Legal range 16..4096. The default is `[PROPOSED]`, to be settled against measured string-length distributions. |
| `log_dir` | `--log-dir` | *(empty)* | Prepended to `log_file` unless that is absolute. Created if missing. |
| `log_file` | `--log-file` | `kdb.log` | Log file name. Empty disables file logging. |
| `log_level` | `--log-level` | `info` | `trace`/`debug`/`info`/`warn`/`error`/`off`. |

An **unknown key is a startup error**, not a warning — a typo such as
`chekpoint_interval_ms` would otherwise look like it applied. Duplicate
keys, malformed lines, and out-of-range values are refused the same way,
each naming the file and line.

### The log

One line per event: `<unix_seconds> <LEVEL> [component] message`.

```
1785309288 INFO [expeditor] opening database 'lg.db', wal dir 'lg.db.wal'
1785309288 INFO [expeditor] checkpoint cadence 2000ms
1785309288 INFO [expeditor] listening on 127.0.0.1:15499
1785309290 DEBUG [checkpoint] checkpoint complete: redo_start=4096 pages_flushed=5
1785309295 INFO [expeditor] stopped cleanly; 8 pages persisted
```

The file is opened append-only, so a restart continues it rather than
erasing why the last run died.

**What each level costs.** The level a component reports at is a deliberate
contract, not a preference: `info` has to stay quiet under load, or a busy
server pays a `write()` syscall per tuple to say nothing.

| Level | Components | Volume |
|---|---|---|
| `error` | failed checkpoint, failed WAL sync, failed client `SYNC`, page corruption detected on read, a refused WAL-gate flush, a failed page write or barrier, a failed anchor publish, io-backend failure | rare; something is wrong |
| `warn` | failed query (with its full reason), cadence disabled, failed heap insert, buffer-pool frame exhaustion, io backend recovering | per failure |
| `info` | startup/shutdown, fresh-vs-existing bootstrap, catalog bootstrap, DDL (`CREATE TABLE`), published checkpoint anchor, client `SYNC`, `STOP` received | per lifecycle event — the default |
| `debug` | every query with its duration, checkpoint start/completion, connection accept/close, WAL sync, page-flush batches and their WAL waits, free-map and device syncs, superblock anchor writes, the catalog side of a DDL (root page, desc-page relink), catalog cache invalidation | **per request** |
| `trace` | every client request line, every heap insert/overwrite, every WAL record appended, **every page dirtied**, every page allocated/read/written, every row id issued | **per tuple** — a development tool, not an operating mode |

**Component tags.** `expeditor` (lifecycle), `client`/`query` (connections and
statements), `ddl`/`catalog` (schema and catalog writes), `heap` (tuple
writes), `page` (a page being dirtied — the page-modification journal),
`buffer` (frame table, flush batches, the WAL gate), `pagestore` (device
reads/writes, free map, corruption), `wal` (record append and sync),
`checkpoint` (checkpoint progress), `superblock` (anchor writes),
`bootstrap` (fresh vs existing), `sched` (reactor and io backend).

Two of these are worth knowing before turning `trace` on: `page` emits one
line per page mutation, and `buffer`/`pagestore` emit one per page touched.
On a write-heavy workload that is several lines per tuple.

Successful replies are summarized (`-> 29B reply`), never echoed: a log that
reproduces result sets is a log that cannot be kept. A *failed* reply is
logged in full, because its whole content is the reason.

Sample at `trace`:

```
1785309852 DEBUG [client] accepted fd=8 open_connections=1
1785309852 TRACE [client] fd=8 request "INSERT INTO acct VALUES ('alice')"
1785309852 TRACE [heap] insert page=128 slot=0 id=1 bytes=15
1785309852 DEBUG [query] "INSERT INTO acct VALUES ('alice')" -> 29B reply in 81us
1785309852 WARN [query] "SELECT * FROM nosuchtable" -> ERR no table with this name in 32us
1785309849 TRACE [wal] append CHECKPOINT_BEGIN lsn=4096 txn=0 page=4294967295 bytes=40
1785309849 DEBUG [wal] sync durable_lsn=4176 appended_lsn=4176 pending_group_commits=0
1785309852 TRACE [catalog] issued row id 1 for table oid 1000
1785309852 TRACE [page] dirty page=128 lsn=4176 rec_lsn=4176
1785309853 DEBUG [checkpoint] started: begin_lsn=4256 dirty_pages=3 active_txns=0 redo_start=4176
1785309853 DEBUG [buffer] wal wait: page 128 needs lsn 4176 durable
1785309853 TRACE [pagestore] wrote page=128 (checkpoint)
1785309853 DEBUG [superblock] wal anchor written for core 0: redo_start=4176 durable_lsn=4340
1785309853 INFO [checkpoint] anchor published: core=0 checkpoint_lsn=4256 redo_start=4176 durable_lsn=4340 segment=0
```

This is a *diagnostic* log, deliberately not per-request tracing — that is a
separate, not-yet-built surface proposed in `docs/observability.md`. The
`in <n>us` figure on a query line is the closest thing available today, and
it is one number for the whole request, not a per-layer breakdown.

Any tool that can open a TCP socket and speak newline-terminated text can
be a client. For a quick manual check:

```sh
printf 'PING\n' | nc 127.0.0.1 15432
```

## 2. Wire protocol

- **Transport:** one TCP connection per client.
- **Request:** exactly one command per line, terminated by `\n` (a
  trailing `\r` before the `\n` is tolerated, so CRLF clients work too).
- **Response:** exactly one line back per command, `\n`-terminated, never
  containing an embedded newline of its own.
- **Encoding:** ASCII/UTF-8 text; commands are case-insensitive keywords,
  arguments are space-separated.
- **Session model:** a connection can send any number of commands in
  sequence, reusing the same socket. There is no pipelining contract
  beyond "one line in, wait for one line out, then send the next" -
  clients should not assume out-of-order or batched responses.
- **Errors** are just another response line, always prefixed `ERR `. There
  is no separate error channel - a malformed or unrecognized line never
  closes the connection or crashes the server, it just gets an `ERR ...`
  reply (see `CommandDispatcher::Dispatch`, `src/server/command_dispatcher.cpp`).

## 3. Command reference

| Command | Arguments | Success reply | Notes |
|---|---|---|---|
| `PING` | none | `PONG` | Liveness check. |
| `SYNC` | none | `OK synced` | Syncs the WAL, then writes the page store back to the data file. For an `INSERT` this is belt-and-braces (it is already logged and, unless `durability = relaxed`, already durable); for **every other mutation** - `CREATE TABLE`, `UPDATE`, catalog rows - this and `STOP` are still the only things that make it survive the process dying. |
| `STOP` | none | `OK bye` | Shuts the **entire server** down, not just this client's connection. Any other clients connected at the time lose their session. |
| `SHOW META` | none | `version=<n> create_time=<n> last_mount_time=<n> wal_anchor_count=<n>` | Dumps the superblock. Times are Unix seconds; `wal_anchor_count` is how many per-core WAL anchors the database carries (`docs/wal.md` §14-3). |
| `SHOW TABLES` | none | space-separated table names | Includes system catalog tables (`tables`, `objects`, `columns`, ...) alongside any user tables. |
| `SHOW PAGE <page_id> [VALUES]` | page id (`uint32_t`), optional `VALUES` keyword | heap page header + slot directory dump | Development/inspection only, not part of any transactional read path. Still one wire line - see below for the escaping convention that makes it render as multiple lines. `VALUES` additionally hex-encodes each live slot's tuple payload (dead slots never show a value). `ERR ...` if the id is missing, non-numeric, unknown to the store, or the trailing option isn't `VALUES`. |
| `DESCRIBE <name>` (or `DESC`) | table name | summary line `oid=<n> root_page_id=<n> clustered_type=<HEAP\|BTREE> next_id=<n> columns=<n>`, then one `\n`-escaped section per column: `pos=<n> name=<s> type=<s> len=<n> notnull=<yes\|no> pk=<yes\|no> autoincrement=<yes\|no>` | Replaces the former `FIND TABLE`, which reported the same summary and no schema. Column 0 is always the Keystone primary key. A relation with no registered columns (the bootstrap catalog tables) reports `columns=0` rather than erroring. `ERR ...` if the name is unknown. |
| `CREATE TABLE <name>` | table name | `ERR ...` | The bare, pre-parser form asks for a zero-column table. Every relation's first column is its mandatory Keystone primary key (`docs/heap-and-tuple.md` §4), so a zero-column relation cannot exist and this now always errors. Use the column-list form below. |
| `CREATE TABLE <name> (<col> <type> [, ...]) [HEAP \| BTREE]` | column list, optional storage clause | `CREATED oid=<n>` or `EXISTS oid=<n>` | The real SQL-grammar form, parsed via `src/parser`. Same idempotency as the bare form. Each `<type>` is resolved case-insensitively against `sys.types` (`Catalog::ResolveTypeByName()`); see `src/exec/row_codec.hpp` for the supported set: `int8`/`int16`/`int32`/`int64`/`uint64`/`bool`/`char`/`varchar`. **`float`/`decimal` are refused** — under the fixed-length rule a relation's row size is a schema constant, so every column needs a decided on-disk width, and theirs is still an open decision; reserving one would be half of settling it. They used to be declarable-but-not-populatable, which cost nothing while a row's size did not depend on them. **There is no `VARCHAR(n)` syntax and none is planned**: the width is one instance-wide `inline_cell_width`, not a per-column declaration, which is what removes `ALTER ... WIDEN` from the surface entirely (`docs/rule-fixed-length-tuple.md` V1/V5). A `varchar` value has no declared limit — up to `inline_cell_width − 3` bytes it lives in the tuple, and beyond that it spills to the var-heap, invisibly. The one hard ceiling is **8144 bytes**, one var-heap page; longer is `ERR ... Unsupported`, because values spanning pages are not implemented. `BTREE` stores the relation as a clustered B+ tree on the Keystone pk instead of a heap chain, which makes `WHERE id = <n>` a descent rather than a scan. Disambiguated from the bare form purely by whether `(` follows the name. |
| `INSERT INTO <name> VALUES (<val> [, ...])` | positional values, one per column in `pos` order **after the primary key** | `INSERTED oid=<table_oid> id=<n> page=<n> slot=<n>` | No explicit column list in this grammar (ast.hpp). **Do not supply the primary key**: column 0 is the Keystone id, issued by the engine from the relation's `next_id` sequence and reported back as `id=`. Supplying a full-width value list is an `ERR` naming the pk column. Ids are unique and ascending; they are not gapless, since a failed insert after a successful allocation burns one. `ERR ...` also on a type/width mismatch or a NULL value (not supported yet). A full page is no longer an error: the relation is a **chain of heap pages** linked through `next_page_id`, and a full tail page grows the chain by one page rather than refusing the row (`page=` in the reply says where it landed, which is no longer implied by the table). Space freed by deleted rows on earlier pages is not reused - the chain only grows at the tail until page compaction exists. **This is the one logged statement**: it appends `TXN_BEGIN`/`HEAP_INSERT`/`TXN_COMMIT` (plus a `FULL_PAGE_IMAGE` and `PAGE_INIT` when the chain grows) and does not reply until they are durable to the configured `durability` class. `ERR ...` if the log cannot be written - in which case the row *is* in the page and will be lost on a crash, since no transaction manager exists yet to roll it back. |
| `SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]` | table name, optional AND-only WHERE | header line + one row per match | On a `BTREE`-clustered table, a WHERE that is exactly one equality against the pk column descends the clustered index - O(depth) page fetches, flat in row count, and authoritative, so a pk that does not exist costs a descent rather than a scan. Anything else - a heap-clustered table, an extra `AND`, a non-pk column, a range - is a full scan of the table's whole page chain, in chain order - which is primary-key order page by page, so rows come back roughly pk-sorted without anything sorting them. Still a scan: no index, and no `min_key` pruning of pages the `WHERE` cannot match. See below for the `\n`-escaping convention this reuses from `SHOW PAGE`. No projection - column list is always `*`. |
| `UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]` | SET list, optional WHERE | `UPDATED <n>` | In-place (HOT-style) overwrite of each matching row. **An UPDATE can never move a row**: the new payload is the same size as the old one, because a row's size is a schema constant and not a function of its values (invariant 13), so a row keeps its `(page_id, slot)` for life. This used to be able to fail with `ERR ...` when a `varchar` grew past its slot's reservation; it cannot now. The pk is not updatable — it is the tuple's identity, not a field of it. |

Anything else, or a blank line, gets `ERR unknown command` / `ERR empty
command` / `ERR unknown <SHOW|CREATE> target` as appropriate - the
connection stays open and usable after an error.

`SHOW PAGE`'s reply is a one-off exception worth calling out: the wire
contract above still holds (exactly one line, no raw newline byte), but
its sections are joined with the literal two-character escape `\n`
(backslash followed by `n`, not an actual newline byte) so the reply can
render as a multi-line, human-readable dump on the client side without
breaking that contract. `tools/ckdbs_cli.py` unescapes it back into real
newlines before printing (see `format_reply()`); a client that doesn't
bother will just see the literal `\n` text inline, which is still valid
and parseable. Example (line-wrapped here for readability; the actual
reply is one line with `\n` in place of real breaks):

```
ckdbs> SHOW PAGE 500
page_id=500
min_key=42
nr_slots=2
lower=26
upper=8110
free_space=8084
next_page_id=4294967295
slot[0] offset=8150 length=33 dead=0
slot[1] offset=8110 length=33 dead=1
```

Add `VALUES` to also see each live slot's payload, hex-encoded:

```
ckdbs> SHOW PAGE 500 VALUES
...
slot[0] offset=8150 length=33 dead=0 value=68656c6c6f
slot[1] offset=8110 length=33 dead=1
```

(Hex, not raw text: a tuple payload can contain any byte value, including
a literal `\n` byte, which would desync the one-line-per-response
contract if spliced in unescaped.)

`src/parser`'s full CREATE TABLE/INSERT/SELECT/UPDATE SQL grammar is now
wired up (`command_dispatcher.cpp`'s `HandleCreateTableSql`/`HandleInsert`/
`HandleSelect`/`HandleUpdate`), using `sys.types` (via
`Catalog::ResolveTypeByName()`) as a stand-in for the not-yet-built real
type registry, and `src/exec/row_codec.cpp` as the (still narrow-scope)
executor: no NULLs, no `float`/`decimal` values, single-page heaps only.
See the table above and `command_dispatcher.hpp`'s doc comment for exact
behavior. `CREATE TABLE <name>` with no parens is the older bare-name form
and now always errors: every relation's first column is its mandatory
Keystone primary key (`docs/heap-and-tuple.md` §4), so a zero-column table
cannot exist.

**Primary keys are the engine's, not the client's.** Column 0 of every
relation is the Keystone id: system-generated, unique and autoincrement
(`CLAUDE.md` invariant 10). `INSERT` therefore supplies values for columns
1..n-1 only, and the assigned key comes back in the reply as `id=<n>`;
`UPDATE` cannot change it. Two rows with the same key are not expressible,
which is the point - a tuple's id is its identity, and the clustered
index addresses tuples by it directly.

## 4. Using `tools/ckdbs_cli.py`

A zero-dependency Python 3 client (stdlib only: `socket`, `argparse`).

**One-shot mode** - send one command, print the reply, exit:

```sh
python3 tools/ckdbs_cli.py PING
python3 tools/ckdbs_cli.py SHOW META
python3 tools/ckdbs_cli.py SHOW TABLES
python3 tools/ckdbs_cli.py SHOW PAGE 500 VALUES
python3 tools/ckdbs_cli.py DESCRIBE accounts
python3 tools/ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES

# Full SQL grammar - quote the statement as one shell argument so '(', ','
# and quoted string literals reach the CLI intact:
python3 tools/ckdbs_cli.py "CREATE TABLE accounts (id int64, name varchar, balance int64)"
python3 tools/ckdbs_cli.py "INSERT INTO accounts VALUES ('alice', 100)"
python3 tools/ckdbs_cli.py "SELECT * FROM accounts WHERE id = 1"
python3 tools/ckdbs_cli.py "UPDATE accounts SET balance = 150 WHERE id = 1"
```

**Script mode** (`-f`/`--file`) — run a local `.sql` file and put the
replies on stdout, so the output redirects, pipes and diffs:

```sh
python3 tools/ckdbs_cli.py -f schema.sql
python3 tools/ckdbs_cli.py -f schema.sql -f load.sql -f report.sql > out.txt
python3 tools/ckdbs_cli.py -f queries.sql --echo    # also print each statement
cat report.sql | python3 tools/ckdbs_cli.py -f -    # script from stdin
```

Statements are separated by `;` and one may span several lines — each is
flattened to a single line before it goes out, because the wire protocol is
one line in / one line out (§2). A file with **no `;` anywhere** is read as
one statement per line instead, which is what `adhoc/*.sql` already uses.
`--` starts a comment to end of line, except inside a quoted string, and
SQL's doubled `''` escape is honoured.

Only replies go to stdout; anything about the run itself — an unreadable
file, a count of failures — goes to stderr, which is what keeps a
redirected stdout clean. A failing statement does not stop the script, but
the exit status is `1` if any statement replied `ERR`.

For a script with inline *expectations* (`-- rows: 3`, `-- expect: ...`),
use `tools/run_sql.py` instead: this runs a script, that one checks one.

`tools/demo_queries.py` runs a fixed ~10-query CREATE TABLE/INSERT/SELECT/
UPDATE sequence end-to-end against a running server and prints each
query's reply - a quick way to see the whole path work without typing it
out by hand.

`tools/benchmark.py` measures client-visible throughput against a running
server: it creates its own 5-column table (Keystone pk + four body
columns) and reports queries/sec plus a latency distribution for four
phases - `INSERT`, point `SELECT ... WHERE id = <n>`, full-table `SELECT`,
and `UPDATE`. Two caveats belong with any number it prints: there is no
index yet, so a point SELECT is a **full scan of the page chain** and its
qps falls as roughly 1/rows; and the server serves one connection at a
time (section 5), so the tool is deliberately single-connection - client
threads would measure the `accept()` queue. Run the server from a Release
build on a scratch data file, or the numbers describe a `-O0` build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
./build-release/kds_server /tmp/bench.db --port 15599 --log-dir /tmp --log-file b.log --log-level debug &
python3 tools/benchmark.py --port 15599 --rows 5000 --read-ops 2000 --update-ops 2000 \
    --server-log /tmp/b.log
```

Since INSERT is logged, the insert phase now measures the `durability`
setting as much as the engine, and the two must be quoted together. On an
EBS gp3 root volume (2,000 rows, 5 columns, one connection, Release):

| `durability` | inserts/sec | p50 |
|---|---|---|
| `strict` | 802 | 1.04 ms |
| `group` | 798 | 1.04 ms |
| `relaxed` | 6,332 | 116 µs |

`group` matching `strict` is the expected result, not a bug: a batch needs
concurrent committers and the server takes one connection at a time. And
**do not benchmark on `tmpfs`** — `fsync` there is free, all three classes
come out identical, and the measurement says nothing.

`--server-log` reads the server's own `in <n>us` per-statement figure back
out of its debug log (matched to this run by its unique table name) and
prints a server-side p50/p95 per statement kind next to the client-side
numbers. That is the figure to judge engine changes on: the client-side
round-trip floor on loopback is ~70-90 µs here, so a change worth 5 µs of
engine time is invisible in qps and obvious in the server-side column.

This is the whole-request counterpart to `bench/bench_main.cpp`, which
times WAL/page internals in-process with no server, parser or socket; the
two sets of numbers are not comparable.

Exit code is 0 regardless of whether the server replied `OK`/`PONG` or
`ERR ...` - the CLI does not interpret the reply, it only fails (exit 1)
if it cannot connect at all.

**Interactive REPL** - omit the command:

```sh
python3 tools/ckdbs_cli.py
ckdbs> PING
PONG
ckdbs> DESCRIBE accounts
oid=4000 root_page_id=128 clustered_type=HEAP next_id=1 columns=2
pos=0 name=id type=int64 len=8 notnull=yes pk=yes autoincrement=yes
pos=1 name=name type=varchar len=0 notnull=yes pk=no autoincrement=no
ckdbs> help        # local-only, not sent to the server
  PING                    -> PONG
  SHOW META               -> superblock stats
  SHOW TABLES             -> space-separated table names
  SHOW PAGE <page_id> [VALUES]
                          -> heap page header + slot directory, pretty-printed
  DESCRIBE <name>         -> table header + one section per column
  CREATE TABLE <name> (<col> <type> [, ...]) [HEAP|BTREE]
  STOP                    -> shuts the whole server down (not just this client)
ckdbs> exit         # local-only: closes this connection, does NOT stop the server
```

`exit` / `quit` (and Ctrl-D) close just the CLI's own connection. Sending
the server's own `STOP` command instead shuts the whole server down, and
the REPL detects that and exits automatically since there is nothing left
to talk to.

`--host` / `--port` override the loopback default if the server is bound
elsewhere.

## 5. Writing your own client

Minimum viable client in any language: open a TCP socket to
`127.0.0.1:15432`, then for each command: write `COMMAND args\n`, read
until you see a `\n`, and treat everything before it as the full reply.
`tools/ckdbs_cli.py`'s `ServerConnection` class (`tools/ckdbs_cli.py`) is a
~15-line reference implementation of exactly that loop, buffering partial
reads until a newline shows up.

Things a robust client should handle that the CLI's REPL does not bother
with, since it is a manual-use tool:

- A `read()`/`recv()` returning zero bytes means the server closed the
  connection (e.g. after `STOP`, or a crash) - do not spin retrying.
- There is currently exactly one accepted client connection served at a
  time end-to-end (see the concurrency note in
  `include/kds/server/tcp_server.hpp`); a second client blocks at the TCP
  `accept()` queue until the first disconnects. This is expected to change
  once the thread-per-core scheduler work lands - this manual will be
  updated when concurrent client handling ships.
