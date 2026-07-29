#!/usr/bin/env python3
"""Runs a fixed sequence of CREATE TABLE / INSERT / SELECT / UPDATE queries
against a running ckdbs server, printing each query and its reply.

Demonstrates the SQL-grammar CREATE TABLE (with a real column list),
INSERT, WHERE-filtered SELECT, and UPDATE support added to
command_dispatcher.cpp (src/exec/row_codec.cpp does the encoding). Reuses
ckdbs_cli.py's ServerConnection instead of re-implementing the wire
protocol - see that file for the one-line-per-command/response contract.

Usage:
    python3 tools/demo_queries.py
    python3 tools/demo_queries.py --host 127.0.0.1 --port 15432

Requires a ckdbs server already running (./build/kds_server) - this
script only sends commands, it does not start one. Exits non-zero if any
query gets an "ERR ..." reply.

Every reply prints as-is (ckdbs_cli.py's format_reply()), except SELECT,
which renders dataframe-style via ckdbs_cli.py's render_select_reply()
(a real pandas DataFrame when installed, a text table otherwise). The
wire protocol itself is unchanged either way.
"""

import argparse
import sys

from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply, render_select_reply

# (label, command) pairs, run in order against a fresh table. Numbered
# comments below match the ~10-query create/insert/select/update spread
# this script exists to demonstrate.
QUERIES = [
    # 1. DDL: real column list, resolved through Catalog::ResolveTypeByName().
    ("CREATE TABLE", "CREATE TABLE accounts (id int64, name varchar, balance int64)"),
    # 2-4. Populate three rows.
    ("INSERT alice", "INSERT INTO accounts VALUES ('alice', 100)"),
    ("INSERT bob", "INSERT INTO accounts VALUES ('bob', 50)"),
    ("INSERT carol", "INSERT INTO accounts VALUES ('carol', 75)"),
    # 5. Full scan.
    ("SELECT all", "SELECT * FROM accounts"),
    # 6. WHERE-filtered scan.
    ("SELECT by id", "SELECT * FROM accounts WHERE id = 2"),
    # 7. Update one row's balance.
    ("UPDATE balance", "UPDATE accounts SET balance = 150 WHERE id = 1"),
    # 8. Confirm the update landed.
    ("SELECT after balance update", "SELECT * FROM accounts WHERE id = 1"),
    # 9. Update a different row's varchar column. Same length as 'bob':
    # OverwriteTuple() is a HOT-style in-place update with no fallback if
    # a new payload outgrows the tuple's original slot reservation (see
    # command_dispatcher.hpp's UPDATE doc comment) - growing this value
    # would fail with an ERR here, which is correct current behavior but
    # not what this demo is showing.
    ("UPDATE name", "UPDATE accounts SET name = 'rob' WHERE id = 2"),
    # 10. Final full scan shows both updates, alice/carol untouched.
    ("SELECT final", "SELECT * FROM accounts"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    args = parser.parse_args()

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    had_error = False
    try:
        for label, command in QUERIES:
            raw_reply = conn.send_command(command)
            text = render_select_reply(raw_reply) if command.strip().upper().startswith("SELECT") \
                else format_reply(raw_reply)
            print(f"[{label}] {command}\n{text}\n")
            if format_reply(raw_reply).startswith("ERR"):
                had_error = True
    finally:
        conn.close()

    sys.exit(1 if had_error else 0)


if __name__ == "__main__":
    main()
