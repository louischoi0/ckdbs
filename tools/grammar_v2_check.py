#!/usr/bin/env python3
"""End-to-end check of the v2 SQL grammar (docs/spec/parser-v2.md, tasks V04-V07)
against a running ckdbs server.

Unlike tools/demo_queries.py, this script is NOT a demo and "no errors" is
NOT the pass condition. The language runs as of V18 - joins, projection and
every subquery form - but a good deal of the grammar exists only to be
declined truthfully, so for many statements here the correct answer from
the server is a refusal, and what matters is *which* one:

    Unsupported   a well-formed statement the engine declines, with the
                  byte position of the thing it declined. The client's fix
                  is to write a different statement.
    InvalidArgument   malformed input. The client's fix is to find a typo.
    accepted      the statement ran.

So every case below declares the outcome it expects, and the script fails
when the server disagrees. A statement that starts returning *rows* where
a refusal was expected is the failure this exists to catch: for a join or
a subquery that would mean answering a multi-relation question with one
relation's rows, which looks like it worked.

    ONE LIMITATION, worth knowing before reading the expectations.
    The newline text protocol replies "ERR <message>" and carries no
    status code (src/server/command_dispatcher.cpp), so this script cannot
    tell Unsupported from InvalidArgument over the wire - it matches on
    message text instead. KWP/1 has an ErrorCategory field for exactly
    this (include/kds/wire/kwp.hpp, docs/spec/protocol.md section 11) but
    nothing speaks KWP/1 yet. The unit tests assert the real codes;
    tests/parser_*_test.cpp is where that is pinned.

Usage:
    python3 tools/grammar_v2_check.py
    python3 tools/grammar_v2_check.py --host 127.0.0.1 --port 15432
    python3 tools/grammar_v2_check.py --task V07     only that task's cases
    python3 tools/grammar_v2_check.py --verbose      print every reply

Requires a server already running (./build/kds_server); this script only
sends commands. It creates its own tables under a `gv2_` prefix so it does
not collide with whatever else is in the database, and it is re-runnable:
tables surviving from a previous run are reused and their rows are not
re-inserted.

Exit status is 0 when every case matched its expectation, 1 otherwise.
"""

import argparse
import sys

from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply


# ---- Expectations ---------------------------------------------------------

class Expect:
    """What a case expects back.

    `needles` are substrings that must all appear; `absent` are substrings
    that must not. Matching on text rather than on a status code is forced
    by the wire format - see the module docstring.

    The `absent` half is not symmetry for its own sake. The failures worth
    catching in an execution layer are wrong *answers*, not errors: a
    projection that emits a column nobody asked for, or a join that
    returns a row that should not have paired. Neither shows up as a
    missing substring.
    """

    def __init__(self, accepted, *needles, absent=(), why=""):
        self.accepted = accepted
        self.needles = needles
        self.absent = absent
        self.why = why

    def check(self, reply):
        """Returns None when the reply matched, else a failure reason."""
        is_err = reply.startswith("ERR")
        if self.accepted and is_err:
            return f"expected the statement to run, got a refusal: {reply}"
        if not self.accepted and not is_err:
            return (f"expected a refusal, but the server ANSWERED: {reply!r} - "
                    "a wrong answer that looks like a right one")
        for needle in self.needles:
            if needle not in reply:
                return f"reply is missing {needle!r}: {reply}"
        for needle in self.absent:
            if needle in reply:
                return f"reply contains {needle!r}, which must not be there: {reply}"
        return None


def runs(*needles, absent=(), why=""):
    return Expect(True, *needles, absent=absent, why=why)


def refused(*needles, why=""):
    return Expect(False, *needles, why=why)


# ---- Cases ----------------------------------------------------------------
#
# (task, label, sql, expectation). Grouped by the workplan task that made
# each form behave the way it does, so a failure names the task to look at.

# Tables first, then the rows that go in them. The rows are inserted ONLY
# when the table was created by this run: re-running against a database
# that already has them would double every row, and several expectations
# below count rows. (An earlier version of this script inserted
# unconditionally and claimed to be re-runnable. It was not.)
SETUP_TABLES = [
    ("gv2_t", "CREATE TABLE gv2_t (id int64, name varchar, note varchar)"),
    ("gv2_u", "CREATE TABLE gv2_u (id int64, t_id int64, status varchar)"),
]

SETUP_ROWS = {
    "gv2_t": ["INSERT INTO gv2_t VALUES ('alice', 'first')",
              "INSERT INTO gv2_t VALUES ('bob', 'second')"],
    "gv2_u": ["INSERT INTO gv2_u VALUES (1, 'open')"],
}

CASES = [
    # ---- V04: the lexer. Reserving keywords and adding the dot token
    # changed no accepted statement, which is the point.
    ("V04", "a bare statement still runs",
     "SELECT * FROM gv2_t",
     runs("alice", why="the baseline every other case is measured against")),
    ("V04", "a qualified column in WHERE",
     "SELECT * FROM gv2_t WHERE gv2_t.id = 1",
     runs("alice", why="`a.x` could not even be tokenized before V04")),
    ("V04", "a qualifier naming no relation is caught",
     "SELECT * FROM gv2_t WHERE nosuch.id = 1",
     refused("names no relation",
             why="the dispatcher has one binding in hand and checks it; without "
                 "this the predicate would silently filter on gv2_t's id")),
    ("V04", "an unreserved word is still a usable column name",
     "SELECT * FROM gv2_t WHERE note = 'first'",
     runs("alice", why="only 11 words are reserved; `note`, `values`, `set` are not")),

    # ---- V05: joins and aliases.
    ("V05", "an alias resolves to its table",
     "SELECT * FROM gv2_t AS a",
     runs("alice", why="an alias renames the relation for predicates, not for the catalog")),
    ("V05", "a join executes and pairs rows through its probe",
     "SELECT gv2_t.id, gv2_u.status FROM gv2_t JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     runs("1,open",
          why="refused from V05 until V17, and the reason for the refusal was that "
              "scanning the first relation and ignoring the rest would have answered "
              "with gv2_t's rows and looked correct. Now it pairs them properly")),
    ("V05", "a join drops the rows that do not pair",
     "SELECT gv2_t.name FROM gv2_t JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     runs("alice",
          why="gv2_u has one row, pointing at gv2_t id 1 - so bob must not appear. "
              "The half of a join that is easy to get wrong is the exclusion")),
    ("V05", "an outer join is declined, with its position",
     "SELECT gv2_t.id FROM gv2_t LEFT JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     refused("outer joins are not supported", "byte 27",
             why="reserved before implementable so the error is truthful, not a syntax error")),
    ("V05", "one relation named twice without aliases",
     "SELECT gv2_t.id FROM gv2_t JOIN gv2_t ON gv2_t.id = gv2_t.id",
     refused("named twice", "alias",
             why="declined rather than guessed at; the message has to say how to fix it")),
    ("V05", "a self-join binds two rows of one relation",
     "SELECT a.id, b.id FROM gv2_t AS a JOIN gv2_t AS b ON a.id = b.id",
     runs("1,1",
          why="one table, two bindings - which is only expressible because the "
              "duplicate-binding rule refuses the version without aliases")),
    ("V05", "INNER is not a silent synonym",
     "SELECT gv2_t.id FROM gv2_t INNER JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     refused(why="spec I9 reserves only the outer keywords, so INNER is trailing garbage")),

    # ---- V06: projection and qualified names.
    ("V06", "an explicit select list emits only the named columns",
     "SELECT name FROM gv2_t",
     runs("alice",
          why="emitting every column when the client named one would be a wrong answer, "
              "which is why this was refused between V06 and V17")),
    ("V06", "a column the client did not name does not appear",
     "SELECT name FROM gv2_t",
     runs("alice", absent=("first",),
          why="'first' is gv2_t.note's value. The projection is resolved to column "
              "indices at compile, so an unnamed column has no way to reach the reply")),
    ("V06", "SELECT * is declined across two relations",
     "SELECT * FROM gv2_t JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     refused("ambiguous across 2 relations", "byte 7",
             why="which columns `*` means would depend on a join order the client chose")),
    ("V06", "SELECT * survives for one relation, alias and all",
     "SELECT * FROM gv2_t AS a WHERE a.id = 1",
     runs("alice", why="one relation is never ambiguous; refusing would break every existing "
                       "statement for no gain")),
    ("V06", "a more specific refusal wins over the star rule",
     "SELECT * FROM gv2_t LEFT JOIN gv2_u ON gv2_t.id = gv2_u.t_id",
     refused("outer joins are not supported",
             why="both rules are true here; the one that tells the client what to do runs first")),

    # ---- V07: predicate-position subqueries.
    ("V07", "EXISTS admits every row when the subquery has one",
     "SELECT * FROM gv2_t WHERE EXISTS (SELECT gv2_u.id FROM gv2_u)",
     runs("alice", "bob",
          why="uncorrelated, so its answer is the same for every outer row - and it is "
              "evaluated once, before the outer relation is opened")),
    ("V07", "NOT EXISTS is that walk negated",
     "SELECT * FROM gv2_t WHERE NOT EXISTS (SELECT gv2_u.id FROM gv2_u)",
     runs(absent=("alice", "bob"),
          why="gv2_u has a row, so NOT EXISTS is false for every outer row. The header "
              "line comes back with no rows under it")),
    ("V07", "IN tests the outer column against the inner values",
     "SELECT * FROM gv2_t WHERE id IN (SELECT t_id FROM gv2_u)",
     runs("alice", absent=("bob",),
          why="gv2_u holds t_id = 1, which is alice")),
    ("V07", "NOT IN excludes exactly what IN admits",
     "SELECT * FROM gv2_t WHERE id NOT IN (SELECT t_id FROM gv2_u)",
     runs("bob", absent=("alice",),
          why="and it is not `!IN`: a NULL in the subquery result would make this "
              "UNKNOWN rather than true, which is why the evaluator is tri-state")),
    ("V07", "a scalar comparison subquery",
     "SELECT * FROM gv2_t WHERE id = (SELECT t_id FROM gv2_u)",
     runs("alice", absent=("bob",),
          why="one row, so it compares. Two would be a CardinalityViolation, never a "
              "first-row pick")),
    ("V07", "a correlated subquery filters per outer row",
     "SELECT * FROM gv2_t WHERE EXISTS (SELECT gv2_u.id FROM gv2_u WHERE gv2_u.t_id = gv2_t.id)",
     runs("alice", absent=("bob",),
          why="the correlation value is read through the frame stack, never written back "
              "into the AST - which is shared across the whole execution")),
    ("V07", "an UPDATE takes the same predicate, meaning the same thing",
     "UPDATE gv2_t SET note = 'x' WHERE id IN (SELECT t_id FROM gv2_u)",
     runs("UPDATED 1",
          why="UPDATE walks the relation itself, so it evaluates conjuncts through the "
              "same shared evaluator - a second one would drift on the first NULL")),
    ("V07", "a derived table is declined, with its position",
     "SELECT * FROM (SELECT * FROM gv2_u)",
     refused("cannot appear in FROM", "byte 14",
             why="the relation-reference production must never reach the statement production")),
    ("V07", "a subquery in join position, same rule",
     "SELECT gv2_t.id FROM gv2_t JOIN (SELECT * FROM gv2_u) ON gv2_t.id = gv2_u.t_id",
     refused("cannot appear in FROM",
             why="one rule on the relation reference covers FROM and join position both")),
    ("V07", "a CTE is declined rather than unrecognized",
     "WITH x AS (SELECT * FROM gv2_u) SELECT * FROM x",
     refused("common table expressions", "byte 0",
             why="routed to the parser so the client learns WITH is declined, not unknown")),
    ("V07", "nesting past the depth cap",
     "SELECT * FROM gv2_t WHERE EXISTS (SELECT * FROM gv2_t WHERE EXISTS "
     "(SELECT * FROM gv2_t WHERE EXISTS (SELECT * FROM gv2_t WHERE EXISTS "
     "(SELECT * FROM gv2_t WHERE EXISTS (SELECT * FROM gv2_t)))))",
     refused("nesting deeper than 4",
             why="the parser recurses per level, so an uncapped nest is a stack overflow "
                 "reachable from one client string")),
    ("V07", "an IN value list names what it wanted",
     "SELECT * FROM gv2_t WHERE id IN (1, 2)",
     refused("expected a subquery",
             why="V08's form; the message has to name it, since this is the spelling a "
                 "client reaches for first")),
    ("V07", "bare NOT is declined with an explanation",
     "SELECT * FROM gv2_t WHERE NOT id = 1",
     refused("NOT is supported only",
             why="no expression tree for a bare NOT to negate (spec I10)")),

    # ---- I14, still OPEN. Recorded so the day it is decided, this line
    # moves with it rather than being discovered by surprise.
    ("I14", "an aggregate in a subquery is a bare syntax error [OPEN]",
     "SELECT * FROM gv2_t WHERE id = (SELECT COUNT(*) FROM gv2_u)",
     refused(why="V07's done-condition wants Unsupported with a position here. Answering "
                 "that would pick one of I14's two open options, so it stays a syntax "
                 "error until I14 is decided. See CLAUDE.md Open Decisions.")),
]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--task", action="append", metavar="V0n",
                        help="only run cases for this task (repeatable)")
    parser.add_argument("--verbose", action="store_true", help="print every reply, not just failures")
    args = parser.parse_args()

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        print("start one with: ./build/kds_server <data-file> --port <port>", file=sys.stderr)
        sys.exit(1)

    failures = []
    try:
        # Setup is the one place an ERR is unconditionally fatal.
        for table, create in SETUP_TABLES:
            reply = format_reply(conn.send_command(create))
            if reply.startswith("EXISTS"):
                continue  # left by an earlier run; its rows are there too
            if reply.startswith("ERR"):
                print(f"setup failed: {create}\n  -> {reply}", file=sys.stderr)
                sys.exit(1)
            for insert in SETUP_ROWS[table]:
                row_reply = format_reply(conn.send_command(insert))
                if row_reply.startswith("ERR"):
                    print(f"setup failed: {insert}\n  -> {row_reply}", file=sys.stderr)
                    sys.exit(1)

        cases = [c for c in CASES if not args.task or c[0] in args.task]
        if not cases:
            print(f"no cases match --task {args.task}", file=sys.stderr)
            sys.exit(1)

        current_task = None
        for task, label, sql, expect in cases:
            if task != current_task:
                print(f"\n=== {task} " + "=" * (68 - len(task)))
                current_task = task

            reply = format_reply(conn.send_command(sql))
            problem = expect.check(reply)

            mark = "FAIL" if problem else ("ok  " if expect.accepted else "ok- ")
            print(f"  [{mark}] {label}")
            if problem or args.verbose:
                print(f"         {sql}")
                print(f"      -> {reply}")
            if problem:
                print(f"      !! {problem}")
                if expect.why:
                    print(f"      .. why this matters: {expect.why}")
                failures.append((task, label, problem))
    finally:
        conn.close()

    total = len([c for c in CASES if not args.task or c[0] in args.task])
    print()
    print("=" * 72)
    if failures:
        print(f"{len(failures)} of {total} cases did not match:")
        for task, label, problem in failures:
            print(f"  {task} {label}: {problem}")
        print("\n'ok-' means the statement was correctly *refused*.")
        sys.exit(1)

    accepted = len([c for c in cases if c[3].accepted])
    print(f"all {total} cases matched: {accepted} ran, {total - accepted} were refused as expected.")
    print("The refusals are the forms the language declines outright - outer joins,")
    print("derived tables, CTEs, over-depth nesting - each with the byte position of")
    print("what it declined. Joins, projection and every subquery form execute.")
    sys.exit(0)


if __name__ == "__main__":
    main()
