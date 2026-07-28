# KDS Client Manual

> **Heads up:** a new binary wire protocol, **KWP/1**, is confirmed
> (2026-07-28) as the eventual replacement for everything below — see
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
./build/kds_server
```

It prints the port it bound and a one-line hint on startup.

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
| `STOP` | none | `OK bye` | Shuts the **entire server** down, not just this client's connection. Any other clients connected at the time lose their session. |
| `SHOW META` | none | `version=<n> max_page_id=<n> create_time=<n> last_mount_time=<n> last_fsync_time=<n> total_pages=<n> free_pages=<n>` | Dumps the superblock. Times are Unix seconds. |
| `SHOW TABLES` | none | space-separated table names | Includes system catalog tables (`tables`, `objects`, `columns`, ...) alongside any user tables. |
| `SHOW PAGE <page_id> [VALUES]` | page id (`uint32_t`), optional `VALUES` keyword | heap page header + slot directory dump | Development/inspection only, not part of any transactional read path. Still one wire line - see below for the escaping convention that makes it render as multiple lines. `VALUES` additionally hex-encodes each live slot's tuple payload (dead slots never show a value). `ERR ...` if the id is missing, non-numeric, unknown to the store, or the trailing option isn't `VALUES`. |
| `FIND TABLE <name>` | table name | `oid=<n>` | `ERR ...` if the name is unknown. |
| `CREATE TABLE <name>` | table name | `CREATED oid=<n>` or `EXISTS oid=<n>` | Idempotent: if a table with this name already exists, its oid is returned and nothing is created - never errors or creates a duplicate. Always a zero-column `ClusteredType::kHeap` table under the public namespace. Legacy bare form - no column list. |
| `CREATE TABLE <name> (<col> <type> [, ...]) [HEAP \| BTREE]` | column list, optional storage clause | `CREATED oid=<n>` or `EXISTS oid=<n>` | The real SQL-grammar form, parsed via `src/parser`. Same idempotency as the bare form. Each `<type>` is resolved case-insensitively against `sys.types` (`Catalog::ResolveTypeByName()`); see `src/exec/row_codec.hpp` for the supported set (`int8`/`int16`/`int32`/`int64`/`uint64`/`bool`/`char`/`varchar` are insertable today - `float`/`decimal` can be declared but not populated, no on-disk encoding decided yet). `BTREE` parses but is rejected (`ERR ...`) - not implemented. Disambiguated from the bare form purely by whether `(` follows the name. |
| `INSERT INTO <name> VALUES (<val> [, ...])` | positional values, one per column in `pos` order | `INSERTED oid=<table_oid> slot=<n>` | No explicit column list in this grammar (ast.hpp) - value count must exactly match the schema. `ERR ...` on a type/width mismatch, a NULL value (not supported yet), or if the table's single heap page is full (no multi-page overflow yet). |
| `SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]` | table name, optional AND-only WHERE | header line + one row per match | Full scan of the table's root heap page only (no multi-page chains, no index use). See below for the `\n`-escaping convention this reuses from `SHOW PAGE`. No projection - column list is always `*`. |
| `UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]` | SET list, optional WHERE | `UPDATED <n>` | In-place (HOT-style) overwrite of each matching row. `ERR ...` if a new value no longer fits the row's original slot reservation (e.g. growing a `varchar`) - no fallback to relocate the row yet. |

Anything else, or a blank line, gets `ERR unknown command` / `ERR empty
command` / `ERR unknown <SHOW|FIND|CREATE> target` as appropriate - the
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
`Catalog::ResolveTypeByName()`) as a stand-in for the not-yet-ported real
type registry, and `src/exec/row_codec.cpp` as the (still narrow-scope)
executor: no NULLs, no `float`/`decimal` values, single-page heaps only.
See the table above and `command_dispatcher.hpp`'s doc comment for exact
behavior. `CREATE TABLE <name>` with no parens is a separate, older thing
that still exists alongside it: a bare-name wire command with no column
list.

## 4. Using `tools/ckdbs_cli.py`

A zero-dependency Python 3 client (stdlib only: `socket`, `argparse`).

**One-shot mode** - send one command, print the reply, exit:

```sh
python3 tools/ckdbs_cli.py PING
python3 tools/ckdbs_cli.py SHOW META
python3 tools/ckdbs_cli.py SHOW TABLES
python3 tools/ckdbs_cli.py SHOW PAGE 500 VALUES
python3 tools/ckdbs_cli.py FIND TABLE accounts
python3 tools/ckdbs_cli.py CREATE TABLE accounts
python3 tools/ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES

# Full SQL grammar - quote the statement as one shell argument so '(', ','
# and quoted string literals reach the CLI intact:
python3 tools/ckdbs_cli.py "CREATE TABLE accounts (id int64, name varchar, balance int64)"
python3 tools/ckdbs_cli.py "INSERT INTO accounts VALUES (1, 'alice', 100)"
python3 tools/ckdbs_cli.py "SELECT * FROM accounts WHERE id = 1"
python3 tools/ckdbs_cli.py "UPDATE accounts SET balance = 150 WHERE id = 1"
```

`tools/demo_queries.py` runs a fixed ~10-query CREATE TABLE/INSERT/SELECT/
UPDATE sequence end-to-end against a running server and prints each
query's reply - a quick way to see the whole path work without typing it
out by hand.

Exit code is 0 regardless of whether the server replied `OK`/`PONG` or
`ERR ...` - the CLI does not interpret the reply, it only fails (exit 1)
if it cannot connect at all.

**Interactive REPL** - omit the command:

```sh
python3 tools/ckdbs_cli.py
ckdbs> PING
PONG
ckdbs> FIND TABLE accounts
ERR table not found: accounts
ckdbs> help        # local-only, not sent to the server
  PING                    -> PONG
  SHOW META               -> superblock stats
  SHOW TABLES             -> space-separated table names
  SHOW PAGE <page_id> [VALUES]
                          -> heap page header + slot directory, pretty-printed
  FIND TABLE <name>       -> oid=<n> or ERR ...
  CREATE TABLE <name>     -> "CREATED oid=<n>" or "EXISTS oid=<n>" (idempotent)
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
