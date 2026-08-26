# KDS Client Manual

How to talk to `kds_server` from client code: the wire protocol, reply
shapes, error handling and retry rules, and the bundled client tools.
Verified against `docs/spec/client-manual.md`, `include/kds/server/tcp_server.hpp`,
`src/server/command_dispatcher.cpp` and `tools/ckdbs_cli.py` as of
2026-08-10. For what you can *say*, see `manual/sql/sql.md`; for running
the server, `manual/server/server.md`.

> The binary wire protocol **KWP/1** (`docs/spec/protocol.md`) will eventually
> replace this newline text protocol. Only its frame codec exists in code,
> so everything below is still exactly how the server behaves.

---

## 1. Connecting

Plain TCP, **loopback only**, port `15432` by default. No TLS, no
authentication, no framing beyond newlines — a development/inspection
surface, not a production API.

```sh
printf 'PING\n' | nc 127.0.0.1 15432        # → PONG
python3 tools/ckdbs_cli.py                  # interactive REPL
```

Multiple clients may connect at once; they are served **concurrently but
cooperatively on one thread** — no client blocks another, and no two
requests are ever handled in parallel (`tcp_server.hpp`). A note in older
docs that only one connection is served at a time is stale.

## 2. Wire protocol

- One command per line in, terminated `\n` (a trailing `\r` is tolerated,
  so CRLF clients work).
- **Exactly one reply line back per command**, `\n`-terminated, never
  containing a raw newline byte.
- ASCII/UTF-8; keywords case-insensitive.
- Strictly serial per connection: send a line, read one reply, then send
  the next. No pipelining contract beyond that, no request ids, no
  out-of-order replies.
- Errors are ordinary reply lines prefixed `ERR ` — there is no separate
  error channel, and a malformed line never closes the connection.

**The `\n` escape convention.** Multi-section replies (SELECT rows,
`SHOW ...` listings, `DESCRIBE` columns) keep the one-line contract by
joining sections with the **literal two characters `\` `n`**, not a real
newline byte. A client that renders for humans unescapes them
(`ckdbs_cli.py`'s `format_reply()` does); a client that does not will see
the literal `\n` inline, which is still parseable. Binary-capable payloads
(`SHOW PAGE ... VALUES`) are hex-encoded for the same reason — a tuple can
contain a raw newline byte.

## 3. Reply shapes

The full per-command table is `docs/spec/client-manual.md` §3; the shapes a
client parses:

| Statement | Success reply |
|---|---|
| `PING` | `PONG` |
| `INSERT` (one row) | `INSERTED oid=<n> id=<n> page=<n> slot=<n>` — the engine-assigned primary key comes back as `id=` |
| `INSERT` (multi-row `VALUES (...), (...)`) | `INSERTED oid=<n> rows=<n> first_id=<n> last_id=<n>` — the id range, with no contiguity promise; over `max_insert_rows` (1024) refuses whole |
| `DROP TABLE` | `DROPPED TABLE <name> oid=<n>` — catalog-scoped; RESTRICTs on assertions and referencing FKs |
| `ALTER TABLE ... RENAME ...` | `RENAMED TABLE <t> TO <new>` / `RENAMED COLUMN <t>.<old> TO <new>` — catalog-only |
| `UPDATE` / `DELETE` | `UPDATED <n>` / `DELETED <n>` (rows affected) |
| `SELECT` | header line, then one `\n`-escaped comma row per match (per group when aggregated; groups in first-seen order) |
| `CREATE TABLE` (SQL form) | `CREATED oid=<n> ...` |
| `CREATE INDEX` / `CABIN` / `PATTERN` / `ASSERTION` | `CREATED <KIND> name=... ...`, possibly with `\n`-escaped `WARN ...` sections |
| `BEGIN` / `COMMIT` / `ROLLBACK` | `BEGIN trx_id=<n> isolation=<s>` / `COMMIT trx_id=<n>` / `ROLLBACK trx_id=<n>` |
| `SET ISOLATION LEVEL` | `SET isolation=<s>` |
| `SHOW <X>` | a count line (`tables=<n>`, `indexes=<n>`, ...) then one `\n`-escaped section per row |
| `DESCRIBE <t>` | summary line (`oid= root_page_id= clustered_type= next_id= ... budget_used=`), then one section per column |
| `SYNC` | `OK synced` |
| `STOP` | `OK bye`, then the server closes the socket |

A `WARN` section on a successful CREATE is advice, not failure — e.g. a
pattern body with no replayable step, or an index superseding a Cabin.

## 4. Errors and retries

Error grammar (`server::ErrorReply` — a compatibility surface; the token
spellings will not change):

```
ERR <TOKEN> retryable=<0|1> <message>     for the coded errors
ERR <message>                             for everything else
```

**`ERR TXN_CONFLICT retryable=1 ...` is the one error worth
special-casing.** It means another transaction wrote a row this one
wanted — first-updater-wins, no lock to wait on, no partial recovery. The
correct response, always: `ROLLBACK`, then retry the whole transaction.
After a conflict inside an explicit transaction the session is **failed**
and answers only `ROLLBACK`/`ABORT`/`SYNC`/`STOP`/`PING` until rolled
back. The same token (retryable) is used for writes refused by cross-core
affinity and for constraint checks that met an in-flight writer.

Not retryable — retrying will fail identically:

- `ERR FK_VIOLATION retryable=0 ...` — missing parent, or a live child on
  parent DELETE.
- `ERR ASSERTION_VIOLATION retryable=0 ...` — the write would exceed a
  declared ceiling; the message names the group and the *enforced* bound.
- Every plain `ERR <message>`: parse refusals (with byte positions),
  unknown names, `CardinalityViolation` (scalar subquery returned >1 row),
  `ResourceExhausted` (the `max_rows_touched` budget), and the rest of
  `manual/sql/sql.md` §8.

A client retry loop should key on the literal token `retryable=1` and
nothing else.

## 5. Sessions and transactions, client view

- **Autocommit by default**: each statement is its own transaction and its
  reply means the configured durability class was honored.
- Explicit transactions are per connection (`BEGIN` ... `COMMIT`); two
  clients never share one. No nesting, no savepoints.
- A failed statement inside an explicit transaction poisons it (see §4); a
  failed autocommit statement unwinds fully by itself.
- Closing the connection with a transaction open **rolls it back** — do
  not use disconnect as commit.
- Isolation: `READ COMMITTED` default, `REPEATABLE READ` available per
  session (`SET ISOLATION LEVEL`, takes effect next transaction) or per
  transaction (`BEGIN ISOLATION LEVEL ...`).
- A `recv()` of zero bytes means the server closed the socket (after
  `STOP`, or a crash) — reconnect, do not spin.

## 6. Data rules a client must know

- **The primary key is the engine's.** Column 0 is the Keystone id;
  `INSERT` supplies columns 1..n-1 only and the assigned key returns as
  `id=<n>`. Supplying it is a dedicated error. `UPDATE` cannot change it.
- **No NULL storage yet**: `NULL` parses as a literal, but rows with NULLs
  are not storable today.
- **Ordering and pagination is
  `[ORDER BY <col> [ASC|DESC] [, ...]] [LIMIT <n>] [OFFSET <m>]`**
  (V09 2026-08-10; general ordering 2026-08-11), each clause optional, in
  that order, on non-aggregated top-level SELECTs. `ORDER BY` takes any
  columns, up to eight keys, each with its own direction; ties keep the
  order the engine would have emitted anyway, so a paged client sees a
  stable order across requests. Two costs worth designing around:
  ordering by the primary key ascending is free, every other order is a
  real sort; and a *sorted* `LIMIT` bounds what you receive but not what
  the engine reads. No cursors: a reply still streams whole, so prefer
  keyset form (`WHERE id > <last seen> LIMIT n`) over a growing `OFFSET`.
  Groups come back in first-seen order; ordering them is still refused.
- **Decimals render at declared scale, always** — a client that parses
  `avg(amt)` at scale 2 can rely on scale 2 forever. `DATE` renders
  `YYYY-MM-DD`, `TIMESTAMP` as UTC.
- Statements are one line: a multi-line SQL string must be flattened
  before sending (the bundled tools do this).

## 7. Bundled clients

**`tools/ckdbs_cli.py`** — the reference client and REPL:

```sh
ckdbs_cli.py                                  # REPL
ckdbs_cli.py "SELECT * FROM accounts WHERE id = 1"   # one-shot
ckdbs_cli.py -f schema.sql -f load.sql > out.txt     # scripts; ';'-separated
cat report.sql | ckdbs_cli.py -f -            # from stdin
ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES
```

It ships lines verbatim and unescapes `\n` sections for display; SELECT
replies render as a table (pandas when installed). A script file with no
`;` is read one-statement-per-line.

**`tools/run_sql.py`** — script runner with inline expectations, the
ad-hoc test harness:

```
-- expect: <text>     next statement's reply must contain <text>
-- reject: <text>     ... must NOT contain <text>
-- error: <text>      ... must be an ERR containing <text>
-- rows: <n>          ... must hold exactly <n> data rows
```

## 8. Writing your own client

Minimum viable client in any language: open a TCP socket to
`127.0.0.1:15432`; per command, write `<line>\n`, then read and buffer
until a `\n` arrives — everything before it is the whole reply.
`ckdbs_cli.py`'s `ServerConnection` class is a ~15-line reference
implementation. A robust client additionally handles: zero-byte reads
(server gone), the `retryable=1` retry loop of §4, unescaping `\n`
sections, and flattening multi-line statements.
