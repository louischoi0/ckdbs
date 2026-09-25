# KDS Client Manual

> **The wire is KWP/1.** `kds_server`'s default port speaks the binary
> framed protocol `docs/spec/protocol.md` specifies: a handshake,
> PARSE / BIND / EXECUTE over server-side statement and portal handles,
> typed row batches, transaction and durability frames, structured
> errors. The newline text protocol is an **off-by-default loopback debug
> surface** — Appendix A, `debug_text_port`.
>
> The statement surface is the same on either port: §3's command
> reference is SQL, and SQL does not depend on how the bytes are framed.

How to talk to the `kds_server` process from a client: the wire protocol,
the full command reference, and how to use the bundled CLI tool. This
documents the *client-facing* surface only - for server internals see
`CLAUDE.md` / `docs/spec/overview.md`.

---

## 1. Connecting

`kds_server` listens on a TCP socket, loopback only, port `15432` by
default (see `kDefaultPort` in `src/server/main.cpp`), and that port
speaks **KWP/1** (§2). Authentication is off by default; when it is on,
the exchange runs inside the handshake and no statement is admitted
before it succeeds.

The newline text protocol is **not** on that port. It is reachable only
where a deployment opens `debug_text_port`, which defaults to 0 — no
socket at all — and Appendix A documents it. One consequence worth
stating before it is discovered: **`STOP` is a newline command**, so an
instance with no debug port open cannot be stopped by connecting and
typing `STOP`. Sending `STOP` as an ordinary *statement* over KWP works
and is admin-classed; `STOP` is not a capability-gated protocol frame
(`docs/spec/protocol.md` §10).

With `tls = on` (off by default; `docs/spec/protocol.md` §1) the same port
speaks **direct TLS 1.3**: every connection must open with a handshake,
and a plaintext client is refused at its first byte. To talk to a
TLS-enabled server interactively:

```sh
openssl s_client -connect 127.0.0.1:15432 -CAfile server.crt -quiet
```

With `auth = scram` (off by default) every connection must complete a
**SCRAM-SHA-256** exchange (RFC 5802/7677) before its first statement —
anything else is refused and the connection closed. Over KWP/1 the
exchange runs in `C_AUTH` / `S_AUTH` frames and the server withholds
`S_HELLO` until it succeeds, so an unauthenticated connection never
learns a session id or a cancel key; the message bodies are **the same
strings** the newline exchange sends, one per frame instead of one per
line. On the debug port, one SCRAM message per line:

```
C: AUTH SCRAM-SHA-256 <client-first-message>
S: AUTH+ <server-first-message>
C: AUTH <client-final-message>
S: AUTH+ <server-final-message>        ← connection is now open
```

The password never crosses the wire (proofs do), and the server's final
message proves *it* holds the verifier — check it. Provision users with
`kds_server --add-user <name> [--role readonly|readwrite|admin]
--users-file <path>` (prompts for the password; the server does not
start; the role defaults to `readonly`).

Every authenticated user holds one of three **roles**, checked per
statement (`docs/spec/protocol.md` §10):

| Role | May run |
|---|---|
| `readonly` | `SELECT`, `WITH`, `ANALYZE`, `SHOW *`, `DESCRIBE`, `PING`, `BEGIN`/`COMMIT`/`ROLLBACK`, `SET ISOLATION` |
| `readwrite` | everything above, plus `INSERT`, `UPDATE`, `DELETE` |
| `admin` | everything, including `CREATE`/`DROP`/`ALTER`, `STOP`, `SYNC`, `SET CABIN_OPTIMIZER` |

A refused statement answers `ERR permission: <cmd> needs <role>; this
connection is <role>`. Commands the server does not recognize also
require `admin` — refused by default, never admitted by omission. A role
change is re-provisioning (delete the line, `--add-user` again); with
`auth = off` every connection is `admin`.

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
| `tls` | — | `off` | Direct TLS 1.3 on the port above (`docs/spec/protocol.md` §1): every connection opens with a handshake, no plaintext fallback, no STARTTLS-style upgrade. Requires both file keys below; a server built with `-DKDS_WITH_TLS=OFF` refuses `on` naming the flag. |
| `tls_cert_file` | — | — | PEM certificate presented to every client, leaf first with any chain appended. Read once at startup. |
| `tls_key_file` | — | — | PEM private key for the certificate, unencrypted. Read once at startup. |
| `auth` | — | `off` | `off` or `scram`. `scram` gates every connection behind a SCRAM-SHA-256 exchange (see "Connecting"); named values only, so a future method is a new word rather than a reinterpreted boolean. |
| `users_file` | `--users-file` | — | The user store: one `<username> <role> <verifier>` per line, `#` comments. Required (and non-empty) when `auth = scram`; a malformed line, unknown role, or missing role column refuses startup naming the line. Written by `--add-user`, never by the running server. |
| `wal_dir` | — | `<data_file>.wal` | WAL segment directory. |
| `isolation` | — | `read committed` | The level a connection starts at, and so the level an autocommit statement runs at (`docs/spec/txn.md` §1). `read committed` takes a read view per **statement**; `repeatable read` takes one per **transaction**. READ COMMITTED is the default for a reason specific to this engine: holding one view for a whole transaction turns more concurrent writes into retryable aborts, and it also **narrows what a write can wait for** — a repeatable-read statement waits on a holder of a coarser unit or on an `INSERT`'s refusal, never on the writer of the row it wants, because that commit is invisible to a view minted at `BEGIN` (`docs/spec/txn.md` §5). The clause "with little waiting" stood here until AO-S8 and is no longer true of the engine: a writer meeting an undecided holder waits for its decide. `serializable` is refused with its reason. This is the server rung of a three-level chain — a connection overrides it with `SET ISOLATION LEVEL`, one transaction with `BEGIN ISOLATION LEVEL`. Names are case-insensitive and accept `-`/`_`. |
| `checkpoint_interval_ms` | — | `5000` | How often dirty pages are flushed (`docs/spec/wal.md` §11). `0` disables the cadence, leaving `SYNC`/shutdown as the only durability points. |
| `durability` | — | `group` | Durability class for every **logged** statement — `INSERT`, `UPDATE` and `DELETE` (`docs/spec/wal.md` §1). It is applied at `COMMIT`, so inside an explicit transaction one wait covers every statement in it rather than one per statement. `strict`/`d1` fsyncs before replying; `group`/`d2` is the same durability point with the fsync amortized over concurrent committers; `relaxed`/`d3` replies immediately and syncs on the drain below. Names are case-insensitive. |
| `wal_drain_interval_us` | — | `1000` | How often the WAL drain runs. Bounds a `relaxed` commit's loss window; a tick with nothing staged does no I/O. `0` disables it. |
| `inline_cell_width` | — | `64` | How many bytes a variable-width value (`varchar`) occupies inside a tuple (`docs/spec/heap-and-tuple.md` §3.3), unless the column declared its own width as `varchar(N)` (`docs/spec/types.md` §2b). A longer value still stores fine — it spills to the var-heap and the cell holds a pointer — so this is a **performance** knob, not a limit: raising it keeps more values in the tuple at the cost of padding every short one. Read **once**, at the bootstrap of a new database, and pinned into the superblock; every later mount validates the running value against the pinned one and refuses to start on a disagreement, naming both. Changing it for existing data is a rebuild, which is `Unsupported` — there is no migration. Legal range 16..4096. |
| `cores` | — | `1` | How many reactor cores this instance runs. **Not pinned since AT-S9**: the superblock records the count, and a mount under a different one records the new count and logs the change rather than refusing - nothing on disk names a core any more, and one stream publishes the anchor's slot 0 alone. A value above 1 spawns that many pinned reactor threads, which **share one WAL stream**: core 0 owns the log and every peer appends through it. (A volume written with one stream per core, before AR0 M0, no longer mounts at all — there is no migration.) Bounded above by 64 (the superblock's WAL anchor slots, indexed by `core_id`) and by the machine's reported core count — pinned reactors never block, so overcommitting them serializes whole workloads behind each other. |
| `indexes` | — | `on` | Whether a secondary index may be **read** (`docs/spec/index.md` §12.3). Off makes a statement on an indexed column take the walk it would have taken had the index not existed. It does **not** change the compiled plan — `ANALYZE` still reports `IndexProbe`, and the switch steers the branch inside that step — so replies are byte-identical either way and the difference is work, not planning. **There is deliberately no key for index maintenance**: an index that stops being maintained is *wrong* rather than slow, and a config key that can produce a wrong answer is not a config key. Turning the write cost off is `DROP INDEX`. |
| `physical_optimizer` | — | `shadow` | `off` or `shadow` (`docs/spec/physical-optimizer.md` R3). Shadow costs nothing at rest — the planner is pull-only, computed when `SHOW RELAYOUT` asks — which is why on-by-default is safe where a background optimizer would not be; `off` makes `SHOW RELAYOUT` answer a one-line disabled notice. **`on` is refused at startup naming §6's three gates** — compact blocked on the reader horizon, cluster on ordered-between pruning, defrag on cross-relation page reuse. |
| `cabin_optimizer` | — | `off` | Part II of `docs/spec/physical-optimizer.md`: the background controller over Observational Cabins. **Off by default, experimental** — the opposite default from `physical_optimizer`, because a controller that acts is not a report. `SET CABIN_OPTIMIZER ON\|OFF` flips it at runtime (non-destructive both ways), and `SHOW META` reports it. The consumer is the controller's cadence task, which reads it at every batch boundary — before a tick's snapshot, between actions, and between a build's pages — so an `off` lands mid-build and the build discards cleanly. `SHOW CABIN_OPTIMIZER` is the view. |
| `cabin_optimizer_*` | — | see sample | The controller's tuning (spec §II.6): `_page_budget` (per-core pages for optimizer-managed Cabins, > 0), the five `_theta_*_pct` thresholds as **percent integers** (300 = ratio 3.0; validation enforces the hysteresis gap `theta_drop < 100 < theta_create`, whatever the numbers), `_confirm_snapshots` (1..1000) and `_snapshot_interval_ms` (0 = no cadence). Parsed and validated at boot. |
| `decay_half_life` | — | `600` | How long an untouched lazy-decay score takes to lose half its weight, in seconds (`docs/spec/physical-optimizer.md` R1). The one time-weighting the engine has; decay is computed lazily at read and touch, so the key prices nothing per second — it only decides how fast "hot" goes cold. `0` is refused: instant decay is "no score", which is not a configuration this engine offers. Consumed by the physical-optimizer planner (`SHOW RELAYOUT`). |
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

This is a *diagnostic* log, not per-request tracing — no per-request
tracing surface exists. The `in <n>us` figure on a query line is the
closest thing available, and it is one number for the whole request, not
a per-layer breakdown.

## 2. Wire protocol — KWP/1

The whole normative description is `docs/spec/protocol.md`; this is what a
client author has to know to write one.

- **Transport:** one TCP connection per session. TLS 1.3 when the port is
  configured for it, from the first byte — there is no upgrade handshake.
- **Framing:** every message in both directions is
  `{length u32 LE, type u8, flags u8, reserved u16, payload}`, where
  `length` counts everything after itself. Binary, little-endian end to
  end. Two string shapes inside a payload: `Str` is `{u16 len, bytes}` for
  names, `Text` is `{u32 len, bytes}` for content, both UTF-8 and neither
  NUL-terminated.
- **Handshake, before anything else:** `C_HELLO` carries the magic
  `'KWP1'`, a `[min, max]` version range, a capability bitset and an
  `auth_method`. The server answers `S_HELLO` with the chosen version, the
  **intersection** of the capabilities, a `session_id` and a `cancel_key`,
  then `S_READY`. A capability bit this server does not know is dropped by
  the intersection rather than refused, which is how a newer client
  connects to an older server.
- **Statements:** `C_PARSE {name, sql}` → `S_PARSE_OK {pattern_id}`;
  `C_BIND {portal, statement, params}` → `S_BIND_OK`;
  `C_EXECUTE {portal, max_rows}` → a result. A named statement lives until
  `C_CLOSE` or disconnect; the unnamed one (`""`) is replaced by the next
  `C_PARSE`. **`pattern_id` is informational** — log it, never interpret
  it; it names a row in this instance's statistics and is not a handle.
- **Parameters carry their own type**: `{type_oid u32, type_mod u32,
  i32 len | -1 = NULL, bytes}`, and they replace the `?` placeholders of
  the parsed statement in order. `type_oid` is the engine's own column
  type number, which is also what `S_ROW_DESC` reports — one numbering,
  two directions.
- **Results:** `S_ROW_DESC` describes the shape once — per field a name, a
  `type_oid`, a wire width (`-1` for variable), flags and a `type_mod`
  (the packed `(precision, scale)` of a `DECIMAL`, zero otherwise). Then
  `S_ROW_BATCH` frames, each `{row_count u16, rows…}`, each row a
  `{i32 len | -1 = NULL, bytes}` per field. Then `S_COMPLETE {tag,
  rows_affected}`.
- **A statement with no result set** answers `S_COMPLETE` alone, its `tag`
  being the sentence the engine rendered. Where that sentence carries
  *lines* — `SHOW`, `DESCRIBE`, `ANALYZE`, `TRACE` — it arrives as a
  one-column `varchar` result set, one row per line, flagged
  `kFieldFlagDiagnosticLine` in the description.
- **No text result mode exists.** A `DATE` is an epoch day, a `TIMESTAMP`
  is microseconds, a `DECIMAL` is its unscaled integer with the scale in
  the description. Rendering them for a human is the client's job, and
  `tools/kwp.py`'s `render_value` is the reference for how.
- **Pipelining and errors:** a client may send `C_PARSE`/`C_BIND`/
  `C_EXECUTE` without waiting. After any `S_ERROR` the server **discards
  every frame until the next `C_SYNC`**, then answers `S_READY` with the
  transaction state. That skip-to-sync rule is the whole pipelining error
  contract; a client that does not send `C_SYNC` after an error will find
  its next statement swallowed.
- **Errors** are `S_ERROR {code, retryable, severity, message, detail?,
  position?}`, `code` being `category << 16 | detail`. `severity` says
  whether the connection survives: an ordinary refusal leaves it standing,
  and only framing corruption and a handshake failure close it.
- **Transactions:** `C_TXN_BEGIN {durability}` / `C_TXN_COMMIT` /
  `C_TXN_ABORT` → `S_TXN_OK`, whose `flags` bit 0 says the commit was
  acked under D3's relaxed semantics. `durability` is 0 for the session
  default, 1/2/3 for D1/D2/D3.

### What a client must special-case

These are the engine's own semantics, unchanged by the framing, and they
hold on the debug port too (Appendix A gives the text spellings).

- **`TXN_CONFLICT` with `retryable = 1` is the one error worth a retry
  loop.** Another transaction wrote a row this one wanted; there is no lock
  to wait on and no partial recovery, so the whole transaction is rolled
  back and retried. Until AT-S10b the same code also answered a statement
  on a peer core whose id lease was spent while its refill was in flight;
  every core issues its own ids since, so a peer's first write runs.
  **Every other code is not retryable**, and
  the bit is the compatibility surface: read the bit, not the message.
  After a conflict the session is in a failed transaction and answers only
  `C_TXN_ABORT` and `C_SYNC` until it is rolled back.
- **`UNKNOWN_OUTCOME` means the statement may have run, and must not be
  retried.** The code stays in the pinned registry, and **nothing produces
  it since AT-S6**: it answered a statement carried to the core that owned
  its relation when no answer came back within ten seconds, and every
  statement runs on its session's core now. A client should still handle
  it as the contract says - this engine issues primary keys, so a retry
  would insert a *second* row rather than replay an idempotent one; **read
  the data back**. It is not a `TXN_CONFLICT` and never carries
  `retryable = 1`.
- **A connection that drops mid-statement may find the statement applied.**
  That is the contract, not an edge case: there is no cancellation in this
  engine, so a statement already running runs to completion whatever
  happens to the connection.
- **`UNSUPPORTED` and `NOT_IMPLEMENTED` are two different answers to "will
  this ever work?"** Both mean the statement was understood and declined,
  both carry the byte position of what was declined, and neither is
  retryable. They differ in one thing, and it is the thing a client library
  wants: `UNSUPPORTED` is a form this engine's architecture cannot admit —
  updating a primary key, comparing two decimals of different width, a
  subquery nested deeper than the engine's fixed limit — so **no later server answers
  it** and the statement must be rewritten. `NOT_IMPLEMENTED` is a form the
  design admits and this release has not built — outer joins, CTEs,
  `UNIQUE`, `ALTER TABLE ADD COLUMN` — so a client may feature-detect and
  try again against a newer server.
- **`REPEATABLE READ` is one snapshot, and there is one core to read it
  on.** A transaction pins its view at `BEGIN` and every statement of it -
  read and write - runs where the session is, so the promise needs no
  protocol to hold across cores. It had one until v3.0.0's M3: a
  transaction could reach another core, and the snapshot its `BEGIN`
  pinned was carried there and adopted for that transaction's life. What a
  client sees is unchanged; what it can no longer see is a statement of
  its transaction running anywhere else.
- **The four statements are not atomic** *as the tool runs them*. The
  engine has transactions (`BEGIN`/`COMMIT`/`ROLLBACK`), but the tool does
  not wrap the four in one — so a failure between them leaves a trade
  recorded against balances that never moved, and those are counted and
  reported as `torn`. That number is a property of the tool's choices
  rather than of the engine.
- **Balances are computed client-side**, because `UPDATE ... SET col =
  <val>` takes a literal and not an expression. Each trader process owns a
  *disjoint* partition of accounts, which is what makes that safe without
  locks; `--verify` reads a sample back and compares stored against
  expected.
- **Money is `int64` minor units, or `decimal(p, s)`.** `float` columns are refused
  at `CREATE TABLE`.

Storage is chosen per relation and the choice is part of the measurement:
`accounts`/`users`/`assets` are `BTREE` because every access is `WHERE id =
<n>`, and on a heap relation that would be a full chain scan - the update
number would then be a function of `--users` rather than of the engine.
`trades` and `user_periodic_profit` are `HEAP`: insert-only, never probed
by pk, so a tail append is exactly right.

One operational limit worth knowing: catalog relations chain into a
reserved range of low page ids, so the whole instance holds roughly **7,800
user columns** (`docs/rules/keystoneid-k0-findings.md`), and these five
relations spend 27 per run. `DROP TABLE` retires a relation's column rows
(`docs/spec/drop-table.md`); a long-lived scratch file that never drops
will eventually refuse a `CREATE TABLE`, and the tool says so by name
rather than passing on the storage error underneath.

**Put the data file on a real disk.** The tool's footer says so and it is
not a formality: `INSERT` is logged and `fsync` on tmpfs is free, so a
tmpfs number here is not a fast result, it is a different measurement.

Exit code is 0 regardless of whether the server replied `OK`/`PONG` or
`ERR ...` - the CLI does not interpret the reply, it only fails (exit 1)
if it cannot connect at all.

**Interactive REPL** - omit the command:

```sh
python3 tools/ckdbs_cli.py
ckdbs> PING
PONG
ckdbs> DESCRIBE accounts
oid=4000 root_page_id=128 clustered_type=HEAP key_order=ascending next_id=1 columns=2
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
  BEGIN [ISOLATION LEVEL READ COMMITTED|REPEATABLE READ]
  COMMIT | ROLLBACK   -> ends the transaction; ROLLBACK undoes every write
  SET ISOLATION LEVEL <level>   -> applies to the next transaction
  DELETE FROM <name> [WHERE ...]  -> delete-mark, not removal
  STOP                    -> shuts the whole server down (not just this client)
ckdbs> exit         # local-only: closes this connection, does NOT stop the server
```

`exit` / `quit` (and Ctrl-D) close just the CLI's own connection. Sending
the server's own `STOP` command instead shuts the whole server down, and
the REPL detects that and exits automatically since there is nothing left
to talk to.

**`SHOW META` lost two fields at AT-S10d**: `sched_wake_race_skips` and
`sched_spurious_wakes`, which measured the cross-core ring transport the
engine no longer has (`docs/spec/sched.md` §7). A tool that parses them
finds them absent, not zero; the rest of the wake block is unchanged.

`--host` / `--port` override the loopback default if the server is bound
elsewhere.

## 5. Writing your own client

**Read `docs/spec/protocol.md` first** — it is the normative description
and this is the orientation.

The shortest correct client is a loop over four things: frame, handshake,
statement, sync.

1. **Frame.** Write `{length u32 LE, type u8, flags u8, reserved u16,
   payload}` and read the same. TCP has no message boundaries, so a read
   may split a frame anywhere including mid-header — accumulate until
   `4 + length` bytes are buffered, then take exactly that many. Refuse a
   `length` outside `[4, 16 MiB]` rather than allocating against it.
2. **Handshake.** Send `C_HELLO`, expect `S_HELLO` then `S_READY`. Keep
   the negotiated capability set: it is the intersection, so a bit you
   asked for and did not get is a feature this server does not have.
3. **Statement.** `C_PARSE`, `C_BIND`, `C_EXECUTE`; then read frames until
   `S_COMPLETE` or `S_ERROR`, collecting `S_ROW_DESC` once and
   `S_ROW_BATCH` repeatedly, and answering `S_PORTAL_SUSPENDED` with
   `C_CONTINUE` if you set a `max_rows`.
4. **Sync.** Send `C_SYNC` and wait for `S_READY` — **always, not only
   after an error**. After an `S_ERROR` the server discards frames until
   it sees one, so a client that skips it loses its next statement to the
   discard; sending it unconditionally is one code path instead of two.

`tools/kwp.py` is the reference implementation of exactly that, in about
four hundred lines with the reasoning in comments, and `tools/ckdbs_cli.py`
drives it.

Things a robust client should handle that the CLI does not bother with,
since it is a manual-use tool:

- A `read()`/`recv()` returning zero bytes means the server closed the
  connection (after `C_TERMINATE`, a fatal error, or a crash) — do not
  spin retrying.
- **Render values yourself.** There is no text result mode: a `DATE`
  arrives as an epoch day, a `TIMESTAMP` as microseconds, a `DECIMAL` as
  its unscaled integer with the scale in `S_ROW_DESC.type_mod`. A client
  that prints the integer has printed the wrong thing.
- **Switch on the error `code`, not on the message.** The category is the
  compatibility surface and the message is prose. In particular, retry on
  `retryable = 1` and nothing else, and never retry `UNKNOWN_OUTCOME`.
- Many clients are served concurrently, cooperatively, on one thread per
  core; no client blocks another, and no two statements of one connection
  run at once. Do not send a second statement before the first has
  answered — the session is stateful and the protocol has no request ids,
  which is why `C_SYNC` is the barrier.

---

# Appendix A — the newline debug surface

**Off by default.** It exists on `debug_text_port` and nowhere else
(`docs/spec/protocol.md` §12): one command per line in, one line out, no
framing beyond the newline. It is an internal development and inspection
protocol, not a client-facing API.

`tools/ckdbs_cli.py --text` speaks it, and so does any tool that can open
a TCP socket and write newline-terminated text:

```sh
printf 'PING\n' | nc 127.0.0.1 <debug_text_port>
```

Everything §3's command reference says about statements applies unchanged
— the difference is only how the bytes are framed and how the answer is
rendered.

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
- **The special cases of §2 carry their category as a token** a retry
  loop reads rather than prose: `ERR TXN_CONFLICT retryable=1 ...`,
  `ERR UNKNOWN_OUTCOME retryable=0 ...`, `ERR UNSUPPORTED retryable=0 ...`,
  `ERR NOT_IMPLEMENTED retryable=0 ...`, `ERR FK_VIOLATION retryable=0 ...`.
  The `retryable=` token will not change spelling. The semantics are §2's;
  every other `ERR` is not retryable. After a conflict the connection is
  in a failed transaction and answers only `ROLLBACK`/`ABORT`/`SYNC`/
  `STOP`/`PING` until it is rolled back.
