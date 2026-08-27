# KDS Client Manual

> **Heads up:** a new binary wire protocol, **KWP/1**, is the eventual
> replacement for everything below — see
> `docs/spec/protocol.md` (spec) and `docs/inflight/in-progress/protocol-wp.md` (task breakdown).
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
`CLAUDE.md` / `docs/spec/overview.md`.

---

## 1. Connecting

`kds_server` listens on a TCP socket, loopback only, port `15432` by
default (see `kDefaultPort` in `src/server/main.cpp`). There is no
authentication, and no wire framing beyond newlines - this is an internal
development/inspection protocol, not a client-facing production API.

With `tls = on` (off by default; `docs/spec/protocol.md` §1) the same port
speaks **direct TLS 1.3**: every connection must open with a handshake,
and a plaintext client is refused at its first byte. To talk to a
TLS-enabled server interactively:

```sh
openssl s_client -connect 127.0.0.1:15432 -CAfile server.crt -quiet
```

With `auth = scram` (off by default) every connection must complete a
**SCRAM-SHA-256** exchange (RFC 5802/7677) before its first statement —
any other line, `STOP` included, is refused and the connection closed.
One SCRAM message per line:

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
statement (`docs/spec/protocol.md` §14):

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
| `wal_dir` | — | `<data_file>.wal` | Per-core WAL segment directory. |
| `isolation` | — | `read committed` | The level a connection starts at, and so the level an autocommit statement runs at (`docs/spec/txn.md` §1). `read committed` takes a read view per **statement**; `repeatable read` takes one per **transaction**. READ COMMITTED is the default for a reason specific to this engine rather than convention: under first-updater-wins with no waiting, holding one view for a whole transaction turns more concurrent writes into retryable aborts. `serializable` is refused with its reason. This is the server rung of a three-level chain — a connection overrides it with `SET ISOLATION LEVEL`, one transaction with `BEGIN ISOLATION LEVEL`. Names are case-insensitive and accept `-`/`_`. |
| `checkpoint_interval_ms` | — | `5000` | How often dirty pages are flushed (`docs/spec/wal.md` §11). `0` disables the cadence, leaving `SYNC`/shutdown as the only durability points. |
| `durability` | — | `group` | Durability class for every **logged** statement — `INSERT`, `UPDATE` and `DELETE` (`docs/spec/wal.md` §1). It is applied at `COMMIT`, so inside an explicit transaction one wait covers every statement in it rather than one per statement. `strict`/`d1` fsyncs before replying; `group`/`d2` is the same durability point with the fsync amortized over concurrent committers, which costs the same as `strict` while the server serves one connection at a time; `relaxed`/`d3` replies immediately and syncs on the drain below. Names are case-insensitive. |
| `wal_drain_interval_us` | — | `1000` | How often the WAL drain runs. Bounds a `relaxed` commit's loss window; a tick with nothing staged does no I/O. `0` disables it. |
| `inline_cell_width` | — | `64` | How many bytes every variable-width value (`varchar`) occupies inside a tuple (`docs/spec/heap-and-tuple.md` §3.3). A longer value still stores fine — it spills to the var-heap and the cell holds a pointer — so this is a **performance** knob, not a limit: raising it keeps more values in the tuple at the cost of padding every short one. Read **once**, at the bootstrap of a new database, and pinned into the superblock; every later mount validates the running value against the pinned one and refuses to start on a disagreement, naming both. Changing it for existing data is a rebuild, which is `Unsupported` — there is no migration. Legal range 16..4096. The default is `[PROPOSED]`, to be settled against measured string-length distributions. |
| `cores` | — | `1` | How many reactor cores this instance runs (`docs/inflight/in-progress/workplan-crosscore.md` M6). Read **once**, at the bootstrap of a new database, and pinned into the superblock; every later mount validates the running value against the pinned one and refuses to start on a disagreement, naming both — WAL streams are per core, so the count decides how many streams the database has, and mounting under a different one would leave streams with nothing to replay them. A value above 1 spawns that many pinned reactor threads, each with its own WAL stream (workplan P0-P2). **Core 0 still serves every statement**, because a peer cannot read the catalog until the per-core catalog cache exists (workplan P6) - so the other cores come up alive and idle, and raising this today buys parallel WAL streams and nothing else. Bounded above by 64 (the superblock's WAL anchor slots, indexed by `core_id`) and by the machine's reported core count — pinned reactors never block, so overcommitting them serializes whole workloads behind each other. |
| `indexes` | — | `on` | Whether a secondary index may be **read** (`docs/spec/index.md` §12.3). Off makes a statement on an indexed column take the walk it would have taken had the index not existed. It does **not** change the compiled plan — `ANALYZE` still reports `IndexProbe`, and the switch steers the branch inside that step — so replies are byte-identical either way and the difference is work, not planning. **There is deliberately no key for index maintenance**: an index that stops being maintained is *wrong* rather than slow, and a config key that can produce a wrong answer is not a config key. Turning the write cost off is `DROP INDEX`. |
| `physical_optimizer` | — | `shadow` | `off` or `shadow` (`docs/spec/physical-optimizer.md` R3). Shadow costs nothing at rest — the planner is pull-only, computed when `SHOW RELAYOUT` asks — which is why on-by-default is safe where a background optimizer would not be; `off` makes `SHOW RELAYOUT` answer a one-line disabled notice. **`on` is refused at startup naming §6's three gates** — compact blocked on the reader horizon, cluster on ordered-between pruning, defrag on cross-relation page reuse — so a config written for the future fails loudly today instead of silently under-delivering. |
| `cabin_optimizer` | — | `off` | Part II of `docs/spec/physical-optimizer.md`: the background controller over Observational Cabins. **Off by default, experimental** — the opposite default from `physical_optimizer`, because a controller that acts is not a report. `SET CABIN_OPTIMIZER ON\|OFF` flips it at runtime (non-destructive both ways, PO8), and `SHOW META` reports it. The consumer is PHY04's cadence task, which reads it at every batch boundary — before a tick's snapshot, between actions, and between a build's pages — so an `off` lands mid-build and the build discards cleanly. `SHOW CABIN_OPTIMIZER` is the view. |
| `cabin_optimizer_*` | — | see sample | The controller's tuning (spec §II.6, every number `[PROPOSED]`): `_page_budget` (per-core pages for optimizer-managed Cabins, > 0), the five `_theta_*_pct` thresholds as **percent integers** (300 = ratio 3.0; validation enforces the hysteresis gap `theta_drop < 100 < theta_create`, whatever the numbers), `_confirm_snapshots` (1..1000) and `_snapshot_interval_ms` (0 = no cadence). Parsed and validated at boot; consumed by PHY04's task when it exists. |
| `decay_half_life` | — | `600` | How long an untouched lazy-decay score takes to lose half its weight, in seconds (`docs/spec/physical-optimizer.md` R1). The one time-weighting the engine has; decay is computed lazily at read and touch, so the key prices nothing per second — it only decides how fast "hot" goes cold. `0` is refused: instant decay is "no score", which is not a configuration this engine offers. Consumed by the physical-optimizer planner (workplan PX05); until that lands the key parses, validates, and steers nothing. The default is the spec's `[PROPOSED]` 600, to be settled against a measured workload. |
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
separate, not-yet-built surface proposed in `docs/inflight/in-progress/observability.md`. The
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
- **`ERR TXN_CONFLICT retryable=1 ...` is the one error worth special-casing
  in a client.** It means another transaction wrote a row this one wanted,
  and the whole transaction must be rolled back and retried — there is no
  lock to wait on and no partial recovery. The `retryable=1` token is a
  compatibility surface rather than prose: retry loops are expected to read
  it, and it will not change spelling. On a multi-core instance the same
  token answers a statement that reached a peer core whose id or page lease
  is spent while its refill is in flight; retrying the statement is the
  right response there too — unless core 0 has refused the refill (a
  dropped relation, an exhausted id space), which answers the next
  statement without the bit. Every other `ERR` is not retryable.
  After a conflict the connection is in a failed transaction and answers
  only `ROLLBACK`/`ABORT`/`SYNC`/`STOP`/`PING` until it is rolled back.
- **`ERR UNKNOWN_OUTCOME retryable=0 ...` means the statement may have run,
  and a client must not retry it** (2026-08-26, statement shipping). On a
  multi-core instance a statement whose relation another core owns is
  carried to that core, executed there and answered back. If the answer
  does not return within ten seconds, the connection is told the outcome is
  unknown — because the owner may already have committed it, and this
  engine issues primary keys, so a retry would insert a *second* row rather
  than replay an idempotent one. The correct response is to **read the data
  back**, never to resend. The token is deliberately one no retry loop
  follows; it is not a `TXN_CONFLICT` and never carries `retryable=1`.
- **A connection that drops mid-statement may find the statement applied.**
  That is the documented contract, not an edge case: the server does not
  cancel a running statement when its client disappears — there is no
  cancellation in this engine — so a shipped statement already on its way
  to its owner is executed there whatever happens to the connection. The
  same holds for the ten-second answer above: the client is told nothing is
  known, and the row may well be committed. A client that needs certainty
  after a disconnect or an `UNKNOWN_OUTCOME` reads the data back.
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
| `SHOW META` | none | `version=<n> create_time=<n> last_mount_time=<n> wal_anchor_count=<n> cabin_optimizer=<on\|off> core=<n>` plus the recovery block below | Dumps the superblock. Times are Unix seconds; `wal_anchor_count` is how many per-core WAL anchors the database carries (`docs/spec/wal.md` §14-3). `core` is the core serving **this session** (2026-08-25, `docs/inflight/in-progress/workplan-peer-writer.md` PW6): under `peer_listeners = on` the kernel picks the accepting core and a client cannot choose it, so this is how a client learns which relations it can write; constant for the session's life. `0` on every session when peer listeners are off. **On a peer** the line also carries its lease refills' cost (2026-08-25, the lease-refill trace): for each of `extent`, `trxid`, `rowid` — `<kind>_refill_requests=<n> <kind>_refill_grants=<n> <kind>_refill_wait_max_us=<n> <kind>_refill_submit_lag_max_us=<n> <kind>_refill_grant_lag_max_us=<n> <kind>_refill_resume_lag_max_us=<n> <kind>_refill_submit_lag_max_iters=<n> <kind>_refill_grant_lag_max_iters=<n> <kind>_refill_resume_lag_max_iters=<n>`: requests sent and grants received, and the longest wait from the request's submit to its completion, split into submit→sent (the reactor queueing the request task), sent→grant received (the ring and core 0), and grant received→the parked coroutine resuming (this core's reactor). Absent on core 0, which leases from nobody. |
| `SHOW TABLES` | none | space-separated table names | Includes system catalog tables (`tables`, `objects`, `columns`, ...) alongside any user tables. |
| — (no command) | `SIGTERM` / `SIGINT` | — | **A process-manager stop is a graceful stop.** `systemctl stop`, a container stop and Ctrl-C are blocked and delivered to the reactor as a descriptor (`include/kds/server/stop_signal.hpp`), so each takes the same path `STOP` does: the listener detaches, the final sync runs, and a checkpoint publishes the anchor so the next mount replays only what followed it. `SIGKILL` is unblockable and remains an immediate kill — that is the crash path, and recovery covers it. |
| `SHOW META`'s recovery block | none | `recovery_records=<n> recovery_committed=<n> recovery_rolled_back=<n> recovery_compensations=<n> recovery_redo_applied=<n> recovery_pages_healed=<n> recovery_torn_tail=<0\|1>`, then `recovery_analysis_us=<n> recovery_redo_us=<n> recovery_high_water_us=<n> recovery_undo_us=<n> recovery_checkpoint_us=<n>`, then `recovery_relations_checked=<n> recovery_relations_missing_pages=<n> catalog_recovered=0 recovery_assertions_enforcing=<n> recovery_assertions_unrecovered=<n> recovery_assertions_foreign=<n>` | What the **last mount's recovery** did (`docs/spec/wal.md` §13, `docs/workplan-wal-recovery.md` RC09). Appended to `SHOW META` by a server; **absent entirely** in an embedded caller that mounts nothing, because a dispatcher with no report has no answer and printing zeroes would be one. **On a peer** (2026-08-25, `docs/inflight/in-progress/workplan-peer-writer.md` PW3b) the block describes *that core's* mount — its own stream, scanned from the anchor core 0 holds for it — so after a clean stop every core's `recovery_records` is small, not only core 0's. `recovery_records` is what the scan read — bounded by the previous mount's completion checkpoint (RC08), so a large log does not mean a large number. `recovery_torn_tail=1` is the **expected** shape of a crash, not a fault. The five `_us` fields are omitted when the mount supplied no clock, for the same reason: a duration of zero and an unmeasured duration are the same bytes. `recovery_relations_missing_pages` counts user relations the catalog still describes whose descriptor or var-heap root page the crash took — the detectable half of the unlogged-DDL gap. **`catalog_recovered=0` is a constant and says so on purpose**: DDL is unlogged (RV3), so a crash can still lose a `CREATE TABLE`, and the converse of the counter above — rows whose relation the catalog lost — is **not detectable at all**, because no page names its relation. Read it as "recovery restored every acknowledged commit to a relation that survived", never as "nothing was lost". `recovery_assertions_enforcing` is how many surviving assertions the mount could resume enforcing (RC07); `recovery_assertions_unrecovered` is the honest remainder — a declaration whose directory could not be rebuilt is **left out of the registry**, so `SHOW ASSERTIONS` reports `enforcing=0` for it rather than a constraint that would admit every write, **and on a relation this core owns its writes are refused too** rather than admitted unchecked (2026-08-26, PW1c-6c). `recovery_assertions_foreign` is neither: an assertion is enforced by the core that owns its relation, because that core's writes are what maintain its Bound Cabin, so on every other core it is counted here and skipped — a correctly-partitioned instance reports a non-zero `_foreign` on each core and is not half-broken. The same change gives `SHOW ASSERTIONS` an `enforced_by_core=<n>` field on a row whose relation another core owns: `enforcing=` there is *this* core's answer, and the named core is the one that can answer for the instance. |
| `SHOW META`'s cross-core write counters | none | `cross_core_write_refusals=<n> cross_core_write_refusal_keys=<n>`, and when a key exists `cross_core_write_refusal_detail=<home>><target>:<oid>=<n>[,...][,+<n>more]` | The writes this core refused because the relation belongs to another core, or because the transaction was already bound to one (`docs/spec/crosscore.md` §6, CC3). **Since 2026-08-26 this counts the residue rather than the demand**: an autocommit single-relation statement is now *shipped* to the owner instead of refused, so what remains here is what shipping does not convert - a statement inside a transaction, and one spanning two owners. The converted population is `shipped_statements` in the row below. **Printed unconditionally, zero included** - unlike the blocks above it, a zero is an answer (*this workload asked for no cross-core write*). The counter is **core-local**, so a whole-instance total is one reading per core, and the unit is a **statement**, not a row: a refused multi-row `INSERT` counts once. `_keys` is how many distinct (home core, target core, relation) triples appear; `_detail` lists them in key order (stable run to run), capped at 16 with `,+<n>more` when it truncated - a silent cut would read as "these were all of them". **What it cannot see**, because the key needs a resolved relation: DDL on a peer (refused by verb before any relation is parsed) and anything refused before resolution at all. The two owner-core refusals, `RelationWriteRightsPending` and `IndexBuildPending`, are excluded **by decision** - the write is this core's own, waiting on a grant or a build window, not a cross-core write. |
| `SHOW META`'s statement-shipping counters (2026-08-26) | none | `shipped_statements=<n> shipped_replies=<n> shipped_refusals=<n> shipped_wait_us_max=<n> shipped_waiting=<n> shipped_late_executed=<n> shipped_identity_mismatches=<n>` on a core that can ship, and `shipped_executed=<n> shipped_running=<n> shipped_deduped=<n> shipped_unanswerable=<n> shipped_early_evictions=<n>` on a core that can be shipped to | **Absent, not zeroed, where shipping is not armed** - a single-core instance prints none of them, which is the honest reading of "there is nothing to ship". The first group is this core as an *arrival core*: statements it sent to owners, answers that came back, how many of those were refusals, and the longest a statement stayed parked waiting. The second is this core as an *owner*: statements it ran for other cores, how many are running now, duplicates it answered from its record rather than running again, and duplicates whose outcome it could no longer state. `shipped_statements - shipped_replies` is "still parked, or lost"; `shipped_identity_mismatches` and `shipped_early_evictions` should both be **0** and are printed so that can be checked rather than assumed. Core-local, like every counter here: one reading per core. |
| `SHOW META`'s group-accounting block | none | `sched_wall_us=<n> sched_iterations=<n> sched_idle_blocks=<n> sched_wake_race_skips=<n> sched_parked_idle_blocks=<n> sched_idle_block_us=<n> sched_wakes_sent=<n> sched_wakes_received=<n> sched_spurious_wakes=<n>`, then for each of `foreground`, `maintenance`, `system`: `sched_<group>_polled_us=<n> sched_<group>_polls=<n> sched_<group>_consumed_us=<n>` | What this core's reactor spent, and on whom (`docs/spec/sched.md` §4). `sched_wall_us` is wall time since the reactor's **first iteration** (0 before it runs), so `sched_wall_us - sum(sched_*_polled_us)` is the reactor time charged to **no** scheduling group: the WAL drain's `fdatasync`, the idle block in `PollReady`, timer callbacks, the io drain. **Two counters per group and the difference matters**: `_polled_us`/`_polls` are cumulative and never decay, while `_consumed_us` is the share law's own input and is *halved periodically* so history does not dominate the pick - a scheduling weight, never a total. A spin reads as `_polls` climbing while `_polled_us` does not. **Absent entirely** off a reactor (an embedded caller, a socket-free test), the rule the recovery block follows: a dispatcher with no reactor has no answer, and zeroes would be one. The same block carries the idle policy's three counters (`docs/spec/sched.md` §7): `sched_idle_blocks` is how often this reactor slept with its wake flag raised, `sched_wake_race_skips` how often the pre-block re-check found a message a sender had decided not to wake for - the race the flag exists for, so a run holding it at 0 has not exercised it - and `sched_parked_idle_blocks` the blocks taken while tasks were still queued, every one of which was a spin before a parked coroutine stopped counting as runnable. **Reading the first two against `sched_iterations` is how a wake path is checked from outside a process.** **Four more make the wall clock add up** (2026-08-27): `sched_idle_block_us` is the time this reactor spent inside a `PollReady` it was allowed to block in - its sleep - so `sched_wall_us - sum(sched_*_polled_us) - sched_idle_block_us` is the time charged to nobody that was **not** sleep, which is the reading the accounting gap actually needs; measured on a shipped run, an arrival core is 79.5% sleep and 10.3% unaccounted work where the pair used to read as one 90% lump, and an idle peer is 99.7% sleep and 0.2% work. `sched_wakes_sent` is the whole **instance's** wake count and therefore repeats identically on every core - it equals the sum of the cores' `sched_wakes_received`, which is the consistency check - and `sched_spurious_wakes` counts wakes this reactor read whose iteration then drained no message, which the send/sleep race makes ordinary rather than wrong. |
| `SHOW PAGE <page_id> [VALUES]` | page id (`uint32_t`), optional `VALUES` keyword | heap page header + slot directory dump | Development/inspection only, not part of any transactional read path. Still one wire line - see below for the escaping convention that makes it render as multiple lines. `VALUES` additionally hex-encodes each live slot's tuple payload (dead slots never show a value). `ERR ...` if the id is missing, non-numeric, unknown to the store, or the trailing option isn't `VALUES`. |
| `DESCRIBE <name>` (or `DESC`) | table name | summary line `oid=<n> root_page_id=<n> clustered_type=<HEAP\|BTREE> next_id=<n> owner_core=<n> columns=<n> ids_issued=<n> ids_remaining=<n> budget_used=<f>%`, then one `\n`-escaped section per column: `pos=<n> name=<s> type=<s> len=<n> notnull=<yes\|no> pk=<yes\|no> autoincrement=<yes\|no> cabin=<yes\|auto\|no>` | Replaces the former `FIND TABLE`, which reported the same summary and no schema. Column 0 is always the Keystone primary key. A relation with no registered columns (the bootstrap catalog tables) reports `columns=0` rather than erroring. The three budget fields are `docs/rules/keystoneid-invariant.md` K4's lifetime id budget: **`ids_issued` counts ids spent, not rows living** — an id burned by a failed insert is spent — and `budget_warning=yes` / `budget_exhausted=yes` are appended only when they apply, so their absence is the normal case. `SHOW BUDGET` is the same numbers for every relation at once. `cabin=` is the column's declared **cabin policy** (`docs/spec/cabin.md` §8.1), omitted for the pk since the clustered tree is its Cabin: `yes` was declared `CABIN`, `no` was declared `NO CABIN`, and `auto` covers both `CABIN AUTO` and saying nothing. It reports what the schema *permits* — whether a Cabin actually exists is `SHOW CABINS`. `owner_core=` is the core that owns the relation (`docs/inflight/in-progress/workplan-crosscore.md` M1), assigned at `CREATE TABLE` and never rebalanced; it is `0` on every relation of a single-core instance, which today is every instance. A non-pk column that declares a foreign key also carries `references=<parent>`, the relation its value is a row id of (`docs/spec/foreign-keys.md`); the column is absent for a column that references nothing. `ERR ...` if the name is unknown. |
| `SHOW ACCESS` | none | `access_shapes=<n>`, then one `\n`-escaped section per `sys.access_stats` row: `kind=<Lookup\|Probe\|Range\|CabinProbe\|IndexProbe\|IndexRange\|FilterScan\|Scan> rel=<s> columns=[<s>[,...]] uses=<n> last_seen=<n>` | What the workload actually ran, per access *shape* (`docs/spec/heap-and-tuple.md` §7). A shape is `(kind, relation, columns)` and is **keyed by columns, never by values**: `WHERE flag = 1` and `WHERE flag = 2` are one shape with two uses, which is what bounds the relation by the schema rather than by the data. The unbounded axis — *which arguments repeat* — is Waystone's, and `SHOW PATTERNS` is where it shows. Every kind is counted through one call with no per-kind branch, so the numbers are comparable across kinds; a `Lookup` and a `FilterScan` are counted the same way. Names are resolved at render time from the stored oids and bitmap: a relation that has gone or become unreadable prints `rel=oid=<n>`, and an unresolvable column prints its position, because the statistic outlives the relation — nothing removes these rows. A column past position 63 sets no bit and merges with any access differing only in it: coarser, not wrong. Switched by `access_statistics` (default `on`); with it off, no new rows are written and any already recorded are still listed. Inspection only — and since 2026-08-09 the physical optimizer's shadow planner consumes it (`SHOW RELAYOUT` below); no mover exists, so nothing acts on it yet. |
| `SHOW RELAYOUT [<table>]` | optional relation name | `relayout_relations=<n>`, then per relation: `rel=<s> clustered=<heap\|btree> shapes=<n> walk_weight_q8=<n>`, one `shape kind=<s> columns_mask=0x<hex> uses=<n> weight_q8=<n>` line per recorded shape, a `survey pages=<n> live=<n> delete_marked=<n> tuples_per_page=<n>` line (named form, heap relation only), and one `plan=<compact\|cluster\|defrag> blocked_on=<gate> surveyed=<0\|1> predicted_pages_saved=<n> predicted_benefit=<n> measured_pages_saved=<n>` line per candidate — or `plans=none reason=<s>` for a btree or catalog relation. With `physical_optimizer = off`: the one line `RELAYOUT off (physical_optimizer=off)`. | The physical optimizer's shadow report (`docs/spec/physical-optimizer.md` §5, v1 shadow-only). **Every plan is blocked and says by what** — `reader-horizon`, `ordered-between` or `page-reuse` (§6's gates) — so the report is what turns "should a gate be opened" into a number, never an action. The bare form reads statistics and catalog only and **cannot walk a relation** (the planner's all-relations entry takes no page store at all); the named form walks that one relation read-only, priced against `max_rows_touched` — a spent budget refuses the survey rather than serving a half-count. The survey is a census, not a read: `delete_marked` counts marks without MVCC, an upper bound on what a compact could drop, which is gate 1's whole point. Weights are R1's lazy-decay score in 1/256ths (`_q8`), decayed from each shape's `last_seen` under `decay_half_life`. `measured_pages_saved` is carried unpopulated so the promotion comparison needs no format change when a mover exists. Read-only; changes no query result. |
| `SHOW CABIN_OPTIMIZER` | none | `cabin_optimizer=<on|off> managed=<n> pages_committed=<n> page_budget=<n> ticks=<n> creates=<n> extends=<n> heals=<n> drops=<n> deferred=<n> failures=<n>`, then one `\n`-escaped section per managed candidate: `rel=<s> column=<s> state=<CANDIDATE|BUILDING|ACTIVE|DECAYING> cabin_id=<n> pages=<n> streak=<n> benefit_q16=<n> cost_q16=<n> [hint_fail_pct=<n> coverage_miss_pct=<n>] last_action=<s> reason=<s> epoch=<n>` (or `last_action=none`). With no controller constructed: the one line `CABIN_OPTIMIZER absent (cabins = off)`. | The cabin optimizer's view (`docs/spec/physical-optimizer.md` PO9, workplan PHY06). Renders, never computes: the managed table and last-Decide B/C scores come from the controller, the counters from the executor — **applied** actions, not decided ones, so a decided CREATE beside `creates=0` reads as "the effectful half deferred or refused", which is the diagnostic — and the quality percentages from the S3 collector through a version-silent read, so looking cannot perturb the decision log's snapshot digests. `benefit_q16`/`cost_q16` are the controller's 16.16 fixed point, raw. Read-only; changes no query result. |
| `SHOW BUDGET` | none | `relations=<n> warning=<n> exhausted=<n> capacity=<n> warn_at=<f>%`, then one `\n`-escaped section per relation: `rel=<s> issued=<n> remaining=<n> used=<f>% warn=<yes\|no> exhausted=<yes\|no>` | Keystone id consumption per relation (`docs/rules/keystoneid-invariant.md` K4/K-M4). Every relation is listed, **the catalog's own included**: `patterns` and `pattern_defs` genuinely issue ids, and hiding them would hide the only consumption an operator does not control. The counts on the summary line are there so crossing the threshold is visible without reading every row. `capacity` is 2^40−1 — one short of 2^40 because id 0 is reserved as "unset" — and is stated once rather than per row, since it is the same constant everywhere. `warn_at` is `[PROPOSED: 90%]` and still proposed. A relation with a `sys.objects` row but no `sys.tables` row is reported in place with `error=<msg>` rather than failing the listing. Inspection only. |
| `CREATE TABLE <name>` | table name | `ERR ...` | The bare, pre-parser form asks for a zero-column table. Every relation's first column is its mandatory Keystone primary key (`docs/spec/heap-and-tuple.md` §4), so a zero-column relation cannot exist and this now always errors. Use the column-list form below. |
| `CREATE TABLE <name> (<col> <type> [, ...]) [HEAP \| BTREE]` | column list, optional storage clause | `CREATED oid=<n>` or `EXISTS oid=<n>` | The real SQL-grammar form, parsed via `src/parser`. Same idempotency as the bare form. Each `<type>` is resolved case-insensitively against `sys.types` (`Catalog::ResolveTypeByName()`); see `src/exec/row_codec.hpp` for the supported set: `int8`/`int16`/`int32`/`int64`/`uint64`/`bool`/`char`/`varchar`, plus **`date`**, **`timestamp`** and **`decimal(p, s)`** (`docs/spec/types.md`). A `date` is stored as days since 1970-01-01 and a `timestamp` as microseconds since it, **always UTC** — there is no session time zone and no conversion, so what an instant means in a local calendar is the client's to decide. A `decimal` is an exact scaled integer with `1 ≤ p ≤ 38` and `0 ≤ s ≤ p`; **both arguments are mandatory** — a bare `decimal` is refused, because a default scale is a silent decision about what a stored value means. The declared precision selects the storage width: `p ≤ 18` is an 8-byte value, `p ≥ 19` a 16-byte one (`decimal128`, also declarable by that name — `DESCRIBE` shows which one a column got), and the two are **different types**: columns on opposite sides of the 18-digit split cannot be compared to each other, exactly as different scales cannot. Values of all three are written as **quoted strings**: `'2026-08-07'`, `'2026-08-07 09:15:00.250'`, `'12.34'` — and a decimal may also be written **bare**, `12.34`, which is exactly the quoted string of its spelling (same value, same pattern, same errors; `docs/spec/types.md` TY3 phase 2). Digits are required on both sides of the point: `12.` and `.5` are syntax errors. A literal with more fractional digits than the column's scale is an error rather than a rounded value. **`float` is refused** and stays refused: IEEE comparison and aggregation semantics conflict with this engine's exactness discipline, and `decimal(p, s)` is what money should use. **There is no `VARCHAR(n)` syntax and none is planned**: the width is one instance-wide `inline_cell_width`, not a per-column declaration, which is what removes `ALTER ... WIDEN` from the surface entirely (`docs/rules/rule-fixed-length-tuple.md` V1/V5). A `varchar` value has no declared limit — up to `inline_cell_width − 3` bytes it lives in the tuple, and beyond that it spills to the var-heap, invisibly. The one hard ceiling is **8144 bytes**, one var-heap page; longer is `ERR ... Unsupported`, because values spanning pages are not implemented. `BTREE` stores the relation as a clustered B+ tree on the Keystone pk instead of a heap chain, which makes `WHERE id = <n>` a descent rather than a scan. Disambiguated from the bare form purely by whether `(` follows the name. Each column may also carry a **cabin policy** (`docs/spec/cabin.md` §8.1), which decides who may give that column a Cabin: `<col> <type> CABIN` creates one immediately and observes its values on first selection; `<col> <type> NO CABIN` forbids one by any route, so `CREATE CABIN` on it is refused; `<col> <type> CABIN AUTO` says the engine may create one when its own signals justify it — **specified but not built**, so it behaves today exactly as saying nothing does. A policy on the primary-key column is an `ERR`, not a no-op: the clustered tree is its Cabin, so there is nothing for the clause to mean. A column may also carry `REFERENCES <parent>` (`docs/spec/foreign-keys.md`), declaring that its value is a row id of `<parent>`. Both suffixes on one column are written in that order - `<col> <type> REFERENCES <parent> CABIN` - because two optional clauses accepted in either order is a grammar with no statable shape. **`REFERENCES <parent>(<col>)` is refused with a position**: the parent side is always that relation's Keystone primary key, so naming a column either repeats the only possible answer or asks for one the engine cannot reference. Four more refusals, each leaving nothing created: an unknown parent; a column whose type cannot hold a 40-bit id; the primary-key column itself, which is the row's identity and not a field of it; and a **heap parent**, which has no primary-key index, so every check of the constraint would scan it - declare the parent `BTREE`. `REFERENCES` is unreserved, so a column may still be named `references`. |
| `INSERT INTO <name> VALUES (<val> [, ...])` | positional values, one per column in `pos` order **after the primary key** | `INSERTED oid=<table_oid> id=<n> page=<n> slot=<n>` | No explicit column list in this grammar (ast.hpp). **Do not supply the primary key**: column 0 is the Keystone id, issued by the engine from the relation's `next_id` sequence and reported back as `id=`. Supplying a full-width value list is an `ERR` naming the pk column. Ids are unique and ascending; they are not gapless, since a failed insert after a successful allocation burns one. `ERR ...` also on a type/width mismatch or a NULL into a column not declared `NULL` (columns are NOT NULL by default — null.md D1). A full page is no longer an error: the relation is a **chain of heap pages** linked through `next_page_id`, and a full tail page grows the chain by one page rather than refusing the row (`page=` in the reply says where it landed, which is no longer implied by the table). Space freed by deleted rows on earlier pages is not reused - the chain only grows at the tail until page compaction exists. **This is the one logged statement**: it appends `TXN_BEGIN`/`HEAP_INSERT`/`TXN_COMMIT` (plus a `FULL_PAGE_IMAGE` and `PAGE_INIT` when the chain grows) and does not reply until they are durable to the configured `durability` class. `ERR ...` if the log cannot be written - in which case the row *is* in the page and will be lost on a crash. Inside an explicit transaction the `TXN_BEGIN`/`TXN_COMMIT` pair belongs to the transaction rather than to the statement, and the row is invisible to everyone else until `COMMIT`. |
| `SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]` | table name, optional AND-only WHERE | header line + one row per match | On a `BTREE`-clustered table, a WHERE that is exactly one equality against the pk column descends the clustered index - O(depth) page fetches, flat in row count, and authoritative, so a pk that does not exist costs a descent rather than a scan. Anything else - a heap-clustered table, an extra `AND`, a non-pk column, a range - is a full scan of the table's whole page chain, in chain order - which is primary-key order page by page, so rows come back roughly pk-sorted without anything sorting them. Still a scan: no index, and no `min_key` pruning of pages the `WHERE` cannot match. See below for the `\n`-escaping convention this reuses from `SHOW PAGE`. An explicit select list is available (`SELECT a.x, b.y`); `*` is refused once more than one relation is in scope, because which columns it means would depend on a join order you were promised is yours. |
| `SHOW PATTERNS` | none | `patterns=<n>`, then one `\n`-escaped section per `sys.patterns` row | Each section carries `pattern_id=0x<hex> oid=<n> origin=<user\|auto> pinned=<yes\|no> class=<n> uses=<n> last_seen=<n> waystone=<root=<n>,depth=<n>\|none>`, plus `name=<s> params=<n>` for patterns that were **declared** (an auto-registered pattern has no name to print and stays bare hex). Rows left behind by an older fingerprint revision are listed too, marked `stale=v<n>` — they are the dead weight a version bump leaves for retention, and seeing them is the point. Inspection only. |
| `SELECT <item> [, ...] FROM <name> ... [GROUP BY <col> [, ...]]` | select items, optional `GROUP BY` after the `WHERE` | header line + one row per group | An item is a column or one of `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `MIN(col)`, `MAX(col)`, `AVG(col)`, each taking an optional `DISTINCT` (`MIN`/`MAX` accept it and ignore it; `AVG(DISTINCT)` averages the distinct values). **`AVG` takes a `decimal(p, s)` column only** and answers at that column's declared scale, rounding **half to even** on the exact integer arithmetic - `avg(amt)` over a `decimal(12, 2)` is money to the cent, ties to the even cent. An integer column is refused: it declared no scale, so any answer would either invent digits or silently drop the remainder - declare a `decimal`, or select `SUM` and `COUNT` and divide at the precision you mean. NULLs are skipped: `COUNT(*)` counts rows, everything else folds the non-NULL values and answers NULL for a group that had none. **With no `GROUP BY` you get exactly one row even over an empty relation** - `COUNT` 0, the rest NULL - and **with one you get none**; both are the standard's answers. Groups come back in **first-seen order**, the order the rows founded them, never sorted. A bare column beside an aggregate must appear in `GROUP BY`: there is no "any row" mode, because an answer that depends on scan order is one this engine refuses to give. `SUM` needs a signed integer column and is exact - a sum crossing `INT64_MAX` **fails the statement** rather than wrapping - so `SUM` over a `uint64` column is refused, while `MIN`/`MAX` over one are exact. `HAVING`, `ORDER BY` over an aggregated statement, aggregates inside a subquery, and aggregation over a `sys.` view are each refused with the byte the offending word starts at. Two server caps, `aggregate_max_groups` and `aggregate_max_distinct`, **fail the statement** naming the key rather than returning a truncated answer. |
| `CREATE PATTERN <name> ($p <type> [, ...]) [WITH (<k> = <v> [, ...])] OF <select>` | parameter list with **mandatory** types, optional options, a SELECT body | `CREATED PATTERN name=<s> pattern_id=0x<hex> dir_depth=<n> params=<n>`, or `ADOPTED PATTERN ...` | Declares a query pattern up front instead of waiting for the engine to learn it from traffic (`docs/spec/create-pattern-user-defined-patterns-v1.md`). Parameters are `$`-sigiled in **both** the declaration and the body — a bare `a` is always a column, a `$a` is always a parameter, which is what stops a parameter colliding with an `AS` alias. The sigil is a parse error anywhere outside a declaration body. `WITH` must come **before** `OF`; the body runs to end of statement. Options: `pinned = on\|off` (default `on` — exempts the pattern's waystones from retention) and `expected_instances = <n>` (pre-sizes the waystone directory; growth is a cache flush, so pre-sizing is a mitigation, not a tuning knob). The declaration is validated hard at CREATE and the reply may carry `\n`-escaped `WARN ...` sections — an implicit conversion on every execution, or a body with no lookup/probe step whose trail could never replay. `ADOPTED` means an auto-registered row for the same shape already existed and was upgraded in place, **keeping the trails it had already recorded**. The `pattern_id` in the reply is the same number `ANALYZE <statement>` prints, which is how you confirm that live traffic actually matches what you declared. `ERR check <n>: ...` names the check that refused it. |
| `CREATE CABIN ON <table>(<column>)` | table and one non-pk column | `CREATED CABIN on=<t>.<c> cabin_id=<n> column=<n> observed=0` | Declares a **Cabin** on the column (`docs/spec/cabin.md`): a store that is *authoritative for the values queries have actually observed*. It changes no answer and accelerates nothing on its own — `observed=0` is printed to say so. Values enter it as a side effect of scans that were going to happen anyway: a declared Cabin observes a value on its **first** selection (an engine-created one waits for the second, the same n=1/n=2 split `CREATE PATTERN` uses, and for the same reason). After that, `WHERE <c> = <v>` reads the matching rows instead of the relation, and an observed value with no matching rows answers zero rows **without opening the relation** — which nothing advisory can do. The cost is one in-memory probe per write to the relation, per Cabin. Refused for the primary-key column (the clustered tree is its Cabin), for a column already carrying one, and for one declared `NO CABIN`. One column only in v1; a comma says so with a position. The reply may carry `\n`-escaped `WARN ...` sections — a column nothing has ever filtered on, or one that already has an index. |
| `DROP CABIN ON <table>(<column>)` | table and column | `DROPPED CABIN on=<t>.<c> cabin_id=<n>` | Removes the Cabin and everything it had observed. Always safe: queries for those values return to the ordinary scan path, a performance loss and never a correctness one — which is exactly what makes an *authoritative* structure evictable. |
| `SHOW CABINS` | none | `cabins=<n>`, then one `\n`-escaped section per `sys.cabins` row: `cabin_id=<n> rel=<s> column=<s> origin=<user\|auto> status=<active\|building\|demoted> observed=<n> entries=<n> hits=<n> misses=<n>` | Joins two sources deliberately. The catalog half — relation, column, origin, status — is DDL and survives a restart. The runtime half — `observed` (values with an entry set), `entries`, and the probe counts — lives in memory and does **not**: by design, a crash declares every Cabin unobserved and traffic rebuilds it. That is what makes `observed=0 hits=0` on an old Cabin readable as "declared and never probed by equality", and `observed>0 hits=0` as "the values being probed are not the ones being observed". With `cabins = off` the runtime fields print `-`, not `0` — nothing is being recorded, which is not the same as nothing having happened. |
| `CREATE INDEX <name> ON <table>(<col>[, ...]) [COVERING (<col>[, ...])]` | index name, table, key columns, optional covered columns | `CREATED INDEX name=<s> on=<t> index_oid=<n> root_page=<n> key_width=<n> entry_width=<n> entries=0` | Declares a **secondary index** (`docs/spec/index.md`). Unlike a Cabin it is authoritative for *every* key value, not only observed ones — so `WHERE <c> = <v>` descends it, and a value with no rows answers zero rows without opening the relation. Key columns are matched **left to right**: an index on `(a, b)` serves `a`, and `(a, b)`, and a range on the column after the matched prefix; it cannot be entered by `b` alone. `COVERING` columns are carried on each entry and used to **filter before the base row is fetched** — they do *not* enable an index-only scan, because visibility still requires the tuple, so a covering clause buys the base descents it avoids and nothing else (`ANALYZE`'s `index_filtered` is that number). `entries=0` is printed for a fresh index on an empty relation; on a populated one the index is **built over every existing version**, including those an older snapshot still reads through, so it is complete or absent and never partial. Refused for: a **heap** relation (an entry resolves through the primary key, which a heap has no index for — declare the table `BTREE`), the primary-key column itself, a column the relation does not have, a repeated column, more than 4 key or 8 covered columns, a name already in use, and `UNIQUE`, which v1 does not enforce. The reply may carry a `\n`-escaped `WARN ...` section when the column already has a Cabin, which the index supersedes. |
| `DROP INDEX <name>` | index name | `DROPPED INDEX name=<s> index_oid=<n>` | Removes the index. Statements on that column return to the walk — a performance loss and never a correctness one. It is also the only way to remove the **write** cost: `indexes = off` stops an index being read and never stops it being maintained, because an index that stops being maintained is wrong rather than slow. The index's pages are **not** freed; nothing frees a page in this engine yet. An index's name is unique instance-wide, so no relation is named here. |
| `SHOW INDEXES` | none | `indexes=<n>`, then one `\n`-escaped section per `sys.indexes` row: `index_oid=<n> name=<s> rel=<s> keys=(<s>[,...]) [covering=(<s>[,...])] root_page=<n> key_width=<n> entry_width=<n> height=<n> entries=<n>` | Every declared index. `keys=` is printed in **declared order**, not sorted: that order *is* what a probe must match a prefix of. `height` and `entries` are walked from the tree rather than stored, because the catalog can say an index exists and never what is in it — a tree that cannot be walked prints `-` for both rather than `0`, since unreadable is not empty. `entries` counts index entries and **not live rows**: maintenance is append-only, so an UPDATE that moves a key adds an entry and leaves the old one, and a DELETE removes nothing. A count well above the row count is that working as specified, not a leak — nothing reclaims a superseded entry until purge exists. |
| `SHOW FKEYS` | none | `fkeys=<n>`, then one `\n`-escaped section per `sys.fkeys` row: `fk_id=<n> child=<s> column=<s> parent=<s> action=RESTRICT nullable=<yes\|no>` | Every declared foreign key (`docs/spec/foreign-keys.md`). **No parent column is printed because there is not one**: a foreign key references the parent relation's Keystone primary key and never another column, which is what makes `ON UPDATE CASCADE` unnecessary rather than deferred - the referenced key is immutable. `action=RESTRICT` is printed unconditionally: v1 has one action, and `CASCADE`/`SET NULL` are deferred behind a budget-interaction design, so a stored action field would have exactly one legal value. Foreign keys are **enforced**: an INSERT or an UPDATE naming a parent row that does not exist answers `ERR FK_VIOLATION retryable=0`, and a DELETE of a row a child still references answers the same. When the row a check depends on is being written by another live transaction the answer is `ERR TXN_CONFLICT retryable=1` instead - the check refuses immediately rather than waiting, and the client retries. |
| `DROP PATTERN <name>` | pattern name | `DROPPED PATTERN name=<s> pattern_id=0x<hex>` | Removes the **declaration**, not the shape: if the engine later re-learns it from traffic it reappears as a nameless auto pattern. The waystones under it are left for retention to reclaim, which is safe because a waystone is advisory (invariant 8). Case-insensitive, like every other identifier. `ERR ...` if no pattern carries the name. There is no `DROP TABLE`. |
| `UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]` | SET list, optional WHERE | `UPDATED <n>` | In-place (HOT-style) overwrite of each matching row. **An UPDATE can never move a row**: the new payload is the same size as the old one, because a row's size is a schema constant and not a function of its values (invariant 13), so a row keeps its `(page_id, slot)` for life. This used to be able to fail with `ERR ...` when a `varchar` grew past its slot's reservation; it cannot now. The pk is not updatable — it is the tuple's identity, not a field of it. **Logged and transactional as of `docs/spec/txn.md`**: an `UNDO_WRITE` carrying the before-image precedes a `HEAP_OVERWRITE`, and the row is stamped with the writing transaction. It was previously unlogged entirely. Fails with `ERR TXN_CONFLICT retryable=1 ...` if another transaction wrote a matching row first (see below). |
| `DELETE FROM <name> [WHERE <cond> [AND <cond>]*]` | table name, optional AND-only WHERE | `DELETED <n>` | A **delete-mark**, never a physical removal: the slot keeps its bytes and gains a deleted flag, and the deleting transaction's id goes in the tuple's writer field. That pair is the whole of DELETE in the no-`xmax` model, and it is why a reader whose snapshot predates the delete **still sees the row** — it steps back over the delete-mark's undo record and finds the payload unchanged. Physical reclamation is a purge pass that does not exist, so the space is not reused. Takes the same `WHERE` as `UPDATE`, through the same compiler, and the same pk fast path. Conflicts exactly as `UPDATE` does. |
| `BEGIN [TRANSACTION] [ISOLATION LEVEL <level>]` (or `START ...`) | optional level: `READ COMMITTED` (default) or `REPEATABLE READ` | `BEGIN trx_id=<n> isolation=<s>` | Opens an explicit transaction on **this connection only** — two clients on one server never see each other's. Without one, every statement is its own transaction and commits on its own (autocommit). The level given here overrides the session's for this transaction only; `SERIALIZABLE` is refused with the reason, not as an unknown word. A second `BEGIN` is an `ERR`: there are no savepoints, so it would have no meaning that is not a guess about which transaction a later `COMMIT` ends. |
| `COMMIT` | none | `COMMIT trx_id=<n>` | Makes the transaction's writes visible to everyone and waits for the configured `durability` class before replying. `ERR no transaction is open` in autocommit. |
| `ROLLBACK` (or `ABORT`) | none | `ROLLBACK trx_id=<n>` | Undoes every write of the transaction, in reverse: an insert's slot is retired, an update's bytes are put back, a delete's mark is cleared. This is also the only way out of a failed transaction. Undo pages are not freed. |
| `SET ISOLATION LEVEL <level>` | `READ COMMITTED` or `REPEATABLE READ` | `SET isolation=<s>` | Applies to the **next** transaction on this connection, never the open one — changing what a running transaction's read view means halfway through would make its earlier statements unexplainable. `ERR` inside a transaction. |
| `SET CABIN_OPTIMIZER [=] <v>` | `ON` or `OFF` | `OK cabin_optimizer=<on\|off>` | PO8's runtime kill switch for the cabin optimizer (`docs/spec/physical-optimizer.md` Part II, workplan PHY05). Non-destructive in both directions: `OFF` halts new decisions and in-flight builds and touches no existing Cabin. Server-wide, not per connection — the controller is per core, not per session. `SHOW META` reports the current value. The consumer is PHY04's cadence task, which reads it at every batch boundary; `SHOW CABIN_OPTIMIZER` shows what the controller is managing and what the executor has applied. |

Anything else, or a blank line, gets `ERR unknown command` / `ERR empty
command` / `ERR unknown <SHOW|CREATE> target` as appropriate - the
connection stays open and usable after an error. `DROP` is a recognized
verb with exactly one target, so `DROP TABLE t` answers `ERR only DROP
PATTERN is supported` rather than `ERR unknown command`: there is no
`DROP TABLE`, and saying which is more use than a generic refusal.

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
type registry, and `src/exec/row_codec.cpp` as the executor. What it still
does not do: **no `float`** — the one type refused for
its semantics rather than for want of work. Three limits that used to be on
this list are gone: `decimal` values store, compare and aggregate exactly
(`docs/spec/types.md`, alongside `date` and `timestamp`), heaps are
page chains rather than single pages, and NULLs store into columns
declared `NULL` (`docs/spec/null.md` — columns are NOT NULL by default).
See the table above and `command_dispatcher.hpp`'s doc comment for exact
behavior. `CREATE TABLE <name>` with no parens is the older bare-name form
and now always errors: every relation's first column is its mandatory
Keystone primary key (`docs/spec/heap-and-tuple.md` §4), so a zero-column table
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

`tools/scenario0_stockmarket.py` measures the same server under a **business
scenario** instead of one statement kind at a time, and reports the number
an OLTP system is operated on: **committed transactions per second**. It
builds five relations - `users`, `accounts` (many per user), `assets`,
`trades` (append-only history), `user_periodic_profit` - and the measured
transaction is one executed trade, exactly four statements:

```
INSERT INTO trades ...    the buy leg  (side=0, buyer's account)
INSERT INTO trades ...    the sell leg (side=1, seller's account)
UPDATE accounts SET ...   buyer:  balance down, asset_qty up
UPDATE accounts SET ...   seller: balance up,   asset_qty down
```

Concurrently, a **separate process** plays the periodic reporting job:
every wake-up it checks whether a simulated reporting period has passed
and, if so, reads each sampled user's accounts with `SELECT * FROM
accounts WHERE user_id = <n>` - a non-pk equality, so a `FilterScan` - and
appends one `user_periodic_profit` row per user. That is the contention
the scenario exists to create, and `--no-profit` prices what it costs.

```sh
python3 tools/scenario0_stockmarket.py --port 15599              # 10K users, 10K assets, 180 days
python3 tools/scenario0_stockmarket.py --seconds 120 --traders 4 --json out.json
python3 tools/scenario0_stockmarket.py --no-profit               # traders alone, for the delta
```

`--days` (default 180) is a **business** span compressed into the
`--seconds` the run actually takes: a trade's `trade_day` and the reporting
job's period boundaries both derive from wall-clock progress. Nothing
sleeps to make that true.

Three properties of today's engine shape everything it prints, and are
stated in its own footer rather than left for the reader to remember:

- **The four statements are not atomic** *as the tool runs them*. The
  engine has transactions now (`BEGIN`/`COMMIT`/`ROLLBACK`), but the tool
  does not wrap the four in one — so a failure between them still leaves a
  trade recorded against balances that never moved, and those are counted
  and reported as `torn`. That number is now a property of the tool's
  choices rather than of the engine, and wrapping the four in a transaction
  would drive it to zero.
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
user columns** (`docs/rules/keystoneid-k0-findings.md`), and these five relations
spend 27 per run. Nothing reclaims them — there is no `DROP TABLE` — so the
count is columns ever created, not columns live, and a long-lived scratch
file will eventually refuse a `CREATE TABLE`. The tool says so by name
rather than passing on the storage error underneath. Until 2026-08-06 the
ceiling was ~68 for the whole instance, and a data file survived exactly
two runs of this scenario.

**Put the data file on a real disk.** The tool's footer says so and it is
not a formality: the same 4-trader configuration measures **1,731 TPS on
tmpfs and 167 TPS on an xfs root volume** — 10× — because `INSERT` is the
one logged statement and `fsync` on tmpfs is free. A tmpfs number here is
not a fast result, it is a different measurement.

Measured numbers and what they mean are in
`bench/results-business-stress.md`, with the raw JSON under
`bench/results/`.

Exit code is 0 regardless of whether the server replied `OK`/`PONG` or
`ERR ...` - the CLI does not interpret the reply, it only fails (exit 1)
if it cannot connect at all.

**Interactive REPL** - omit the command:

```sh
python3 tools/ckdbs_cli.py
ckdbs> PING
PONG
ckdbs> DESCRIBE accounts
oid=4000 root_page_id=128 clustered_type=HEAP next_id=1 owner_core=0 columns=2
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
