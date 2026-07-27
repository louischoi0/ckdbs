# KDS Client Manual

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
| `LIST TABLES` | none | space-separated table names | Includes system catalog tables (`tables`, `objects`, `columns`, ...) alongside any user tables. |
| `FIND TABLE <name>` | table name | `oid=<n>` | `ERR ...` if the name is unknown. |

Anything else, or a blank line, gets `ERR unknown command` / `ERR empty
command` / `ERR unknown <SHOW|LIST|FIND> target` as appropriate - the
connection stays open and usable after an error.

There is no SQL yet (`src/parser` exists but nothing wires it into
`CommandDispatcher` yet) and no INSERT/SELECT data-manipulation commands -
this command set only covers what the catalog/superblock can already
report. Expect this table to grow as those subsystems land.

## 4. Using `tools/ckdbs_cli.py`

A zero-dependency Python 3 client (stdlib only: `socket`, `argparse`).

**One-shot mode** - send one command, print the reply, exit:

```sh
python3 tools/ckdbs_cli.py PING
python3 tools/ckdbs_cli.py SHOW META
python3 tools/ckdbs_cli.py FIND TABLE accounts
python3 tools/ckdbs_cli.py --host 127.0.0.1 --port 15432 LIST TABLES
```

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
  LIST TABLES             -> space-separated table names
  FIND TABLE <name>       -> oid=<n> or ERR ...
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
