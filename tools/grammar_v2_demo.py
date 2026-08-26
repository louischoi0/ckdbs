#!/usr/bin/env python3
"""Walks the v2 SQL grammar statement by statement, printing what the
server says to each - the readable companion to tools/grammar_v2_check.py.

Where grammar_v2_check.py asserts and exits non-zero, this one explains.
It sends the same kinds of statement in the order the grammar grew
(V04 lexer -> V05 joins -> V06 projection -> V07 subqueries) and prints
each reply with a one-line note on what it demonstrates, so the shape of
the language is visible without reading docs/spec/parser-v2.md first.

The language **runs** as of V18: joins pair rows through a pk probe, select
lists emit exactly the columns named, and every subquery form - EXISTS,
NOT EXISTS, IN, NOT IN, scalar, correlated or not - filters for real. What
still comes back "ERR" is the forms the language declines outright.

Those refusals are content, not failure. Each names what it declined and
where, rather than falling through to a scan that would answer a
two-relation question with one relation's rows.

Usage:
    python3 tools/grammar_v2_demo.py
    python3 tools/grammar_v2_demo.py --host 127.0.0.1 --port 15432

Requires a server already running (./build/kds_server), or use
tools/run_grammar_v2.sh which starts a throwaway one for you.

Always exits 0: an "ERR" here is content, not failure. Use
grammar_v2_check.py when you want a pass/fail answer.
"""

import argparse
import sys

from ckdbs_cli import (DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply,
                       render_select_reply)

# (note, sql). The note says what the statement is for; it prints above
# the reply so the output reads top to bottom without a legend.
SCRIPT = [
    ("--- setup " + "-" * 62, None),
    ("two relations to join, and three rows",
     "CREATE TABLE demo_acct (id int64, name varchar, tier varchar)"),
    (None, "CREATE TABLE demo_trade (id int64, acct_id int64, sym varchar)"),
    (None, "INSERT INTO demo_acct VALUES ('alice', 'gold')"),
    (None, "INSERT INTO demo_acct VALUES ('bob', 'silver')"),
    (None, "INSERT INTO demo_trade VALUES (1, 'AAPL')"),

    ("--- V04  the lexer: keywords, the dot token, byte offsets " + "-" * 14, None),
    ("the baseline: one relation, no qualifier",
     "SELECT * FROM demo_acct"),
    ("`a.x` now tokenizes at all - before V04 the '.' was a lexing error",
     "SELECT * FROM demo_acct WHERE demo_acct.tier = 'gold'"),
    ("a qualifier that names no relation is caught rather than ignored",
     "SELECT * FROM demo_acct WHERE nosuch.tier = 'gold'"),
    ("only 11 words are reserved, so `tier`, `note`, `values` are still column names",
     "SELECT * FROM demo_acct WHERE tier = 'silver'"),

    ("--- V05  joins and aliases " + "-" * 45, None),
    ("an alias renames the relation for predicates, not for the catalog",
     "SELECT * FROM demo_acct AS a WHERE a.id = 1"),
    ("a join executes: rows are paired through a pk probe on the second relation",
     "SELECT demo_acct.name, demo_trade.sym FROM demo_acct "
     "JOIN demo_trade ON demo_acct.id = demo_trade.acct_id"),
    ("outer joins are reserved before they are implementable, so the error is truthful",
     "SELECT demo_acct.id FROM demo_acct LEFT JOIN demo_trade ON demo_acct.id = demo_trade.acct_id"),
    ("one relation twice with no way to tell the occurrences apart",
     "SELECT demo_acct.id FROM demo_acct JOIN demo_acct ON demo_acct.id = demo_acct.id"),
    ("...and the same statement is fine once each occurrence has its own alias",
     "SELECT a.id, b.id FROM demo_acct AS a JOIN demo_acct AS b ON a.id = b.id"),

    ("--- V06  projection and qualified names " + "-" * 32, None),
    ("an explicit select list emits exactly those columns, resolved to indices at compile",
     "SELECT name FROM demo_acct"),
    ("`SELECT *` has no answer across two relations - which columns, in what order?",
     "SELECT * FROM demo_acct JOIN demo_trade ON demo_acct.id = demo_trade.acct_id"),
    ("one relation is never ambiguous, so `SELECT *` still works, alias and all",
     "SELECT * FROM demo_acct AS a WHERE a.tier = 'gold'"),

    ("--- V07  predicate-position subqueries " + "-" * 33, None),
    ("EXISTS: uncorrelated, so evaluated once before the outer relation is opened",
     "SELECT * FROM demo_acct WHERE EXISTS (SELECT * FROM demo_trade)"),
    ("NOT IN is its own predicate, not !IN - a NULL in the result makes it UNKNOWN",
     "SELECT * FROM demo_acct WHERE id NOT IN (SELECT acct_id FROM demo_trade)"),
    ("a scalar subquery: more than one row is a runtime error, not a parse error",
     "SELECT * FROM demo_acct WHERE id = (SELECT acct_id FROM demo_trade)"),
    ("an UPDATE takes the same predicate through the same evaluator",
     "UPDATE demo_acct SET tier = 'x' WHERE id IN (SELECT acct_id FROM demo_trade)"),
    ("a derived table: the relation-reference production must not reach the statement one",
     "SELECT * FROM (SELECT * FROM demo_trade)"),
    ("a CTE is declined by name, so a client learns WITH is refused and not unknown",
     "WITH x AS (SELECT * FROM demo_trade) SELECT * FROM x"),
    ("the depth cap is 4; the parser recurses per level, so an uncapped nest is a crash",
     "SELECT * FROM demo_acct WHERE EXISTS (SELECT * FROM demo_acct WHERE EXISTS "
     "(SELECT * FROM demo_acct WHERE EXISTS (SELECT * FROM demo_acct WHERE EXISTS "
     "(SELECT * FROM demo_acct WHERE EXISTS (SELECT * FROM demo_acct)))))"),

    ("--- still open " + "-" * 57, None),
    ("I14 undecided: an aggregate gives a bare syntax error, not a positioned refusal",
     "SELECT * FROM demo_acct WHERE id = (SELECT COUNT(*) FROM demo_trade)"),
]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    args = parser.parse_args()

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        print("start one with: ./build/kds_server <data-file> --port <port>", file=sys.stderr)
        print("or run: tools/run_grammar_v2.sh", file=sys.stderr)
        sys.exit(1)

    ran = refused = 0
    try:
        for note, sql in SCRIPT:
            if sql is None:
                print(f"\n{note}")
                continue
            if note:
                print(f"\n# {note}")

            raw = conn.send_command(sql)
            print(f"  {sql}")

            # render_select_reply takes the *raw* reply and passes an
            # "ERR ..." through unchanged, so one call covers both.
            is_select = sql.strip().upper().startswith("SELECT")
            text = render_select_reply(raw) if is_select else format_reply(raw)

            if format_reply(raw).startswith("ERR"):
                refused += 1
            else:
                ran += 1
            for line in text.splitlines():
                print(f"  -> {line}")
    finally:
        conn.close()

    print("\n" + "=" * 72)
    print(f"{ran} statements ran, {refused} were refused.")
    print("The refusals that remain are forms the language declines outright.")
    print("Each names what it declined and where, instead of quietly answering a")
    print("question the engine cannot answer.")


if __name__ == "__main__":
    main()
