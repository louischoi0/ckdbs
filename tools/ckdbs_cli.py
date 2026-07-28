#!/usr/bin/env python3
"""Simple CLI client for the ckdbs server's plain-text TCP protocol.

The server (src/server, ./build/kds_server) speaks one command per line,
newline-terminated, and always replies with exactly one line back. This
is a thin client for that protocol - it does no parsing/validation of its
own, it just ships whatever you type and prints back whatever the server
says.

Usage:
    ckdbs_cli.py                        interactive REPL
    ckdbs_cli.py PING                    one-shot: send "PING", print reply, exit
    ckdbs_cli.py SHOW META
    ckdbs_cli.py SHOW TABLES
    ckdbs_cli.py SHOW PAGE 128
    ckdbs_cli.py SHOW PAGE 128 VALUES
    ckdbs_cli.py FIND TABLE accounts
    ckdbs_cli.py CREATE TABLE accounts
    ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES

REPL-only local commands (never sent to the server):
    help / ?     list known server commands
    exit / quit  close the connection and exit (does NOT stop the server -
                 use the server's own STOP command for that)
"""

import argparse
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 15432

# Kept here only to print a hint in `help` - the server is the source of
# truth for what it actually accepts; this list can drift if commands are
# added there without updating this comment.
KNOWN_COMMANDS = """\
  PING                    -> PONG
  SHOW META               -> superblock stats
  SHOW TABLES             -> space-separated table names
  SHOW PAGE <page_id> [VALUES]
                          -> heap page header + slot directory, pretty-printed;
                             VALUES also hex-encodes each live tuple's payload
  FIND TABLE <name>       -> oid=<n> or ERR ...
  CREATE TABLE <name>     -> "CREATED oid=<n>" or "EXISTS oid=<n>" (idempotent)
  STOP                    -> shuts the whole server down (not just this client)
"""


class ServerConnection:
    """One TCP connection to the ckdbs server, one line in / one line out."""

    def __init__(self, host, port, timeout=5.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._buf = b""

    def send_command(self, line):
        self._sock.sendall(line.encode("utf-8") + b"\n")
        return self._read_line()

    def _read_line(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", errors="replace")

    def close(self):
        self._sock.close()


def format_reply(reply):
    """Renders a reply for display.

    The wire protocol allows exactly one line back per command (see
    docs/client-manual.md section 2) - a raw newline byte in a reply would
    desync this client's "read up to the next \\n" framing. Commands that
    want a readable multi-line dump (e.g. SHOW PAGE) instead join sections
    with the literal two-character escape "\\n", which is unescaped here
    into a real newline purely for display; nothing is sent back over the
    wire in this form.
    """
    return reply.replace("\\n", "\n")


def run_one_shot(conn, command):
    print(format_reply(conn.send_command(command)))


def run_repl(conn):
    print("ckdbs interactive CLI. Type 'help' for known commands, 'exit' to quit.")
    while True:
        try:
            line = input("ckdbs> ")
        except EOFError:
            print()
            break

        stripped = line.strip()
        if not stripped:
            continue
        if stripped.lower() in ("exit", "quit"):
            break
        if stripped.lower() in ("help", "?"):
            print(KNOWN_COMMANDS, end="")
            continue

        try:
            print(format_reply(conn.send_command(stripped)))
        except ConnectionError as e:
            print(f"connection lost: {e}")
            break

        if stripped.strip().upper() == "STOP":
            # The server just shut itself down - nothing more to send.
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    parser.add_argument("command", nargs="*",
                         help="command to send (e.g. PING, FIND TABLE accounts); "
                              "omit for an interactive REPL")
    args = parser.parse_args()

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.command:
            run_one_shot(conn, " ".join(args.command))
        else:
            run_repl(conn)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
