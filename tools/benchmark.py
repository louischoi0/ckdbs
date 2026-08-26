#!/usr/bin/env python3
"""Measures end-to-end query throughput of a running ckdbs server.

What this measures: the whole request path a client actually pays for -
socket write, newline framing, parse, catalog binding, heap access, reply
formatting - against a table with **5 columns** (the mandatory Keystone pk
plus four body columns). It is the client-visible counterpart to
bench/bench_main.cpp, which measures WAL/page internals in-process with
no server, no parser and no socket. The two are not comparable and are not
meant to be.

tools/pg_benchmark.py runs these same four phases against PostgreSQL over
its own wire protocol, sharing the timing/reporting harness in
bench_common.py, so `--json` output from the two is diffable phase by
phase. Read that tool's docstring before quoting a comparison: index
parity and per-statement WAL fsync are the two places the engines are not
doing the same work.

Phases, each timed separately and reported as queries/sec plus a latency
distribution:

    insert        INSERT INTO <t> VALUES (...)         write path
    point-select  SELECT * FROM <t> WHERE id = <n>     read path, 1 row out
    full-scan     SELECT * FROM <t>                    read path, all rows out
    update        UPDATE <t> SET c_int = <n> WHERE id = <n>   write path
    join-probe    <child> JOIN <t> ON child.parent_id = t.id
    join-scan     <child> JOIN <t> ON child.match_val = t.c_small
    join-semi     <child> WHERE EXISTS (SELECT ... WHERE t.id = child.parent_id)

The three join phases exist to price **one decision**: the step compiler
marks a join step `Probe` when its equality binds the relation's primary
key and `Scan` otherwise (docs/spec/parser-v2.md section 1). Same statement
shape, same driving relation, same number of output rows - and the second
one walks the whole inner relation per outer row. The gap between
join-probe and join-scan is what that access kind is worth, and it is the
same line Waystone draws between what a trail may replace and what it may
only prefetch for, so it is worth knowing in numbers rather than in
principle.

    join-scan is **quadratic**: --join-rows x --rows tuple decodes per
    query. It has its own smaller op count (--join-scan-ops) for that
    reason, and the report prints the implied comparison count so a
    surprising number can be checked against the work it implies.

`join-probe` is only a descent when the inner relation is a B+ tree. On a
heap relation there is no pk index, so the probe step falls through to the
same chain walk a Scan does (step_vm.cpp's RunPointStep) - which means
under the default --clustered heap, join-probe and join-scan measure
nearly the same thing. Run with --clustered btree to see them separate.

Two properties of today's engine shape the numbers, and quoting them
without both is misleading:

1. **Index behaviour depends on --clustered.** A heap relation has no
   index at all: a `WHERE id = <n>` SELECT or UPDATE is a full scan of
   the relation's whole page chain (docs/spec/client-manual.md section 3), so
   both phases' QPS fall roughly as 1/--rows and neither number means
   anything without the row count beside it. The way out is
   `--clustered btree`: the relation *is* a B+ tree on the pk, so a point
   lookup is an O(depth) descent and is authoritative - a miss is an
   answer rather than a fallback. That goes flat in row count on the read
   phases and changes what the insert phase does (a descent and the
   occasional split). The full-scan phase is unaffected either way: it
   reads every page by definition, and the tree's leaves are chained
   exactly as the heap's pages are.
2. **The server serves one connection at a time** (see the concurrency
   note in include/kds/server/tcp_server.hpp), so this is deliberately a
   single-connection, one-request-at-a-time driver. Adding client threads
   today would measure the accept() queue, not the engine. Revisit when
   the thread-per-core scheduler lands.

Timing is per-request wall clock around a send+recv pair (the wire
protocol is strictly one line in, one line out), so every reported
latency includes Python's own socket overhead. That overhead is the floor
of what this tool can resolve; treat sub-10us differences as noise.

Usage:
    # start a server first, ideally a Release build on a scratch data file:
    #   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    #   cmake --build build-release -j && ./build-release/kds_server /tmp/bench.db
    python3 tools/benchmark.py
    python3 tools/benchmark.py --rows 5000 --read-ops 2000 --update-ops 2000
    python3 tools/benchmark.py --host 127.0.0.1 --port 15432 --json out.json

    # the two access paths, same rows, diffable JSON:
    python3 tools/benchmark.py --rows 20000 --json scan.json
    python3 tools/benchmark.py --rows 20000 --clustered btree --json bt.json

Each run creates its own table (`bench_<pid>_<epoch>` by default) so a
persistent data file can be benchmarked repeatedly without earlier runs'
rows inflating the scan cost. --table pins the name instead.
"""

import argparse
import random
import re
import sys
import time

from bench_common import report, run_phase, write_json
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# Four body columns after the Keystone pk = 5 columns total. Mixed widths
# on purpose: an int-only row would not exercise row_codec's varchar path,
# which is the one with a variable payload length.
COLUMNS = "id int64, c_int int64, c_small int32, c_flag bool, c_text varchar"

# The join phases' driving relation. Deliberately NOT the same shape as
# COLUMNS: this table is scanned end to end by every join query, so it is
# kept narrow, and it is always heap-clustered because nothing ever probes
# into it - it is always step 0.
#
#   parent_id   references a real id of the benchmark table, so the pk
#               equi-join finds exactly one row per child row.
#   match_val   drawn from the same range as the parent's c_small, so the
#               NON-pk join has real matches rather than measuring the
#               cost of finding nothing.
CHILD_COLUMNS = "id int64, parent_id int64, match_val int64, c_int int64"

# The upper bound both the parent's c_small and the child's match_val are
# drawn from. Shared so the non-pk join's selectivity is a property of
# this constant rather than an accident of two separate literals.
SMALL_RANGE = 30000

# UPDATE is an in-place (HOT-style) overwrite with no relocation fallback:
# a new value that outgrows the row's original slot reservation is an ERR
# (docs/spec/client-manual.md section 3). So the update phase only ever rewrites
# the fixed-width c_int column, and the varchar is written once at insert
# time at a constant length.
TEXT_LEN = 16


# Server errors that mean "this data file cannot be used", mapped to the
# thing to actually do about them. Without this a stale data file shows up
# as a wall of failed inserts and then a traceback three phases later,
# which says nothing about the cause.
FATAL_HINTS = {
    "catalog row size mismatch":
        "the data file was written before the sys.tables row format changed\n"
        "(Waystone fields removed, 2026-07-31) and cannot be read by this build.\n"
        "Start the server on a fresh data file - there is no migration.",
    "unsupported": "the data file was written by a different build of the engine.",
}


def fail(message, reply=None):
    """Stops the run with a diagnosis instead of a traceback. A benchmark
    that cannot run is an operator problem, not a bug to debug from a
    stack trace."""
    print(f"benchmark aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
        for needle, hint in FATAL_HINTS.items():
            if needle in reply.lower():
                for line in hint.splitlines():
                    print(f"  {line}", file=sys.stderr)
                break
    sys.exit(1)


def check_phase(phase):
    """A phase where nothing succeeded measured nothing, so the run stops
    there rather than reporting a qps for a column of errors and failing
    later somewhere unrelated."""
    if phase.errors == 0:
        return phase
    if phase.errors == phase.ops:
        fail(f"every {phase.name} failed - there is nothing to measure",
             phase.first_error)
    # Partial failure still reports, but must not slip past unread: the
    # err column is easy to miss next to a plausible-looking qps.
    print(f"warning: {phase.errors} of {phase.ops} {phase.name} "
          f"operations failed; first: {phase.first_error}", file=sys.stderr)
    return phase


def create_table(conn, table, clustered):
    """Creates the run's table in one storage organization or the other.

    The trailing HEAP/BTREE word is the CREATE TABLE storage clause
    (docs/spec/client-manual.md section 3): HEAP is a chain of heap pages, BTREE
    is a clustered B+ tree over the same leaf format. It is a create-time
    property with no ALTER, so the two are only ever compared across runs.
    """
    clause = "" if clustered == "heap" else f" {clustered.upper()}"
    reply = format_reply(conn.send_command(f"CREATE TABLE {table} ({COLUMNS}){clause}"))
    if reply.startswith("ERR"):
        fail(f"could not create the benchmark table as {clustered}", reply)
    return reply


def insert_commands(table, rows, rng):
    for _ in range(rows):
        text = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=TEXT_LEN))
        # The bool column takes 0/1: this grammar has no boolean literal
        # (row_codec.cpp's "expects 0 or 1" check).
        yield (f"INSERT INTO {table} VALUES "
               f"({rng.randint(0, 1_000_000)}, {rng.randint(0, SMALL_RANGE)}, "
               f"{rng.randint(0, 1)}, '{text}')")


def child_insert_commands(child, rows, parent_rows, parent_smalls, rng):
    """Rows for the join phases' driving relation.

    Two columns, two jobs:

    `parent_id` names a real parent row, so the pk equi-join pairs
    one-to-one: `join-probe` returns exactly `rows` rows, which is what
    makes it comparable with `join-scan` at all - and what lets the
    pre-flight check notice a join that has quietly stopped matching.

    `match_val` is drawn from the parent's **actual** c_small values for
    half the rows, and randomly for the other half. Drawing it from the
    same *range* is not enough: c_small is uniform over 30,000 values, so
    at a few hundred rows the two sets almost never intersect and the
    non-pk join ends up timing the cost of finding nothing. The walk is
    the same either way - every outer row still reads the whole inner
    relation - but a phase that reports "0 rows per reply" reads like a
    broken join rather than a deliberate one.
    """
    for i in range(rows):
        if parent_smalls and i % 2 == 0:
            match = rng.choice(parent_smalls)
        else:
            match = rng.randint(0, SMALL_RANGE)
        yield (f"INSERT INTO {child} VALUES "
               f"({rng.randint(1, max(1, parent_rows))}, "
               f"{match}, {rng.randint(0, 1_000_000)})")


CLUSTERED_LINE = re.compile(r"clustered_type=(\w+)")


def verify_clustered(conn, table, clustered):
    """Reads the storage organization back off the server rather than
    trusting the CREATE. --table can point at a table an earlier run
    created with the other clause, and an unnoticed mismatch turns a
    heap-vs-btree comparison into two runs of the same thing."""
    reply = format_reply(conn.send_command(f"DESCRIBE {table}"))
    m = CLUSTERED_LINE.search(reply)
    if m is None:
        fail("could not read the table's clustered_type back", reply)
    actual = m.group(1).lower()
    if actual != clustered:
        fail(f"table {table} is {actual}-clustered, but --clustered {clustered} was asked for "
             f"(a pre-existing --table keeps the organization it was created with)")
    return actual


def access_path(clustered):
    """How the server will find a row by pk in this run, as
    CommandDispatcher::LocateByPk() decides it: a clustered B+ tree
    relation descends and the descent is authoritative; a heap relation
    has no index and scans the chain."""
    return "btree descent" if clustered == "btree" else "full chain scan"


def reply_rows(text):
    """Data rows in a SELECT reply: a header line plus one line per row,
    joined with the literal `\\n` escape."""
    return max(0, len([line for line in text.split("\n") if line != ""]) - 1)


def count_rows(conn, table):
    """Rows currently visible to a full scan."""
    text = format_reply(conn.send_command(f"SELECT * FROM {table}"))
    if text.startswith("ERR"):
        fail("could not scan the benchmark table", text)
    return reply_rows(text)


# Field index of c_small in COLUMNS: id, c_int, c_small, c_flag, c_text.
C_SMALL_FIELD = 2


def parent_small_values(conn, table, limit=512):
    """A sample of the benchmark table's real `c_small` values.

    Read back rather than reproduced from the RNG: the generator has moved
    on by the time the child table is built, and re-deriving the same
    sequence would couple the two in a way that breaks silently the first
    time a phase draws one extra number.
    """
    text = format_reply(conn.send_command(f"SELECT * FROM {table}"))
    if text.startswith("ERR"):
        return []
    values = []
    for line in text.split("\n")[1:]:
        fields = line.split(",")
        if len(fields) <= C_SMALL_FIELD:
            continue
        try:
            values.append(int(fields[C_SMALL_FIELD]))
        except ValueError:
            continue
        if len(values) >= limit:
            break
    return values


# ---- the join phases -----------------------------------------------------
#
# Three statements over the same two relations. What differs is only which
# column the equality binds, which is exactly what the step compiler reads
# to choose an access kind - so the statements are written out here side by
# side rather than built from a template, because the difference between
# them is the whole measurement.

def join_probe_sql(child, parent):
    """Equality on the inner relation's **primary key** -> a `Probe` step.

    On a btree parent this is an O(depth) descent per outer row. On a heap
    parent there is no pk index and it degrades to the same walk join_scan
    does, which is why --clustered changes what this phase means."""
    return (f"SELECT c.c_int, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.parent_id = p.id")


def join_scan_sql(child, parent):
    """Equality on a **non-pk** column -> a `Scan` step: the whole inner
    relation is walked once per outer row. Quadratic, and the point of
    comparison."""
    return (f"SELECT c.c_int, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.match_val = p.c_small")


def join_semi_sql(child, parent):
    """A correlated `EXISTS` - a semi-join. Same pk probe as join_probe,
    but it stops at the first qualifying row instead of pairing, so it
    prices the short-circuit rather than the pairing."""
    return (f"SELECT c.c_int FROM {child} AS c "
            f"WHERE EXISTS (SELECT p.id FROM {parent} AS p WHERE p.id = c.parent_id)")


def preflight_joins(conn, child, parent, child_rows):
    """Runs each join once, untimed, and checks it returned what it should.

    Worth the round trips: a join that has quietly stopped matching returns
    zero rows *very* fast, and a benchmark reports that as a spectacular
    qps. Every child row references a real parent, so the pk join and the
    semi-join must both return exactly `child_rows`.
    """
    for name, sql, expected in (
            ("join-probe", join_probe_sql(child, parent), child_rows),
            ("join-semi", join_semi_sql(child, parent), child_rows)):
        text = format_reply(conn.send_command(sql))
        if text.startswith("ERR"):
            fail(f"the {name} statement does not run", text)
        got = reply_rows(text)
        if got != expected:
            fail(f"{name} returned {got} rows, expected {expected} - every child row "
                 f"references a real parent, so a different count means the join is "
                 f"not pairing correctly and its timing would be meaningless")

    text = format_reply(conn.send_command(join_scan_sql(child, parent)))
    if text.startswith("ERR"):
        fail("the join-scan statement does not run", text)
    # The non-pk join's row count is a property of the data, not a
    # requirement - but zero would mean it is timing the cost of finding
    # nothing, which is a different measurement and should be visible.
    return reply_rows(text)


# ---- server-side timings -------------------------------------------------
#
# The server logs one line per statement at debug level:
#
#   <unix> DEBUG [query] "INSERT INTO t VALUES (1)" -> 29B reply in 81us
#
# That `in <n>us` is the server's own measurement of the whole request,
# excluding everything this client pays for. Since each run uses a uniquely
# named table, filtering log lines on that name isolates this run's
# statements from any other traffic in the same log.

QUERY_LINE = re.compile(r'\[query\] "(\w+)[^"]*".* in (\d+)us')
DURABILITY_LINE = re.compile(r'INSERT durability (\w+)')


def server_side_us(log_path, table):
    """Maps statement keyword -> sorted list of server-side microseconds for
    the statements of this run, read back from the server's debug log."""
    by_kind = {}
    with open(log_path, errors="replace") as f:
        for line in f:
            if table not in line:
                continue
            m = QUERY_LINE.search(line)
            if m is None:
                continue
            by_kind.setdefault(m.group(1).upper(), []).append(int(m.group(2)))
    return {kind: sorted(values) for kind, values in by_kind.items()}


def read_durability(log_path):
    """The durability class the server started under, from the line the
    expeditor logs at Info. None if the log cannot be read or predates the
    line - the report then just omits it rather than guessing a default
    that may not be what this server is running."""
    try:
        with open(log_path, errors="replace") as f:
            for line in f:
                m = DURABILITY_LINE.search(line)
                if m is not None:
                    return m.group(1)
    except OSError:
        return None
    return None


def report_server_side(by_kind):
    if not by_kind:
        print("  --server-log: no timed query lines for this run's table "
              "(is the server at --log-level debug?)")
        return
    print("  server-side, as measured by the server itself (excludes this "
          "client's round trip):")
    for kind in sorted(by_kind):
        v = by_kind[kind]
        print(f"    {kind:<8}{len(v):>7} stmts  p50 {v[len(v) // 2]:>7} us"
              f"  p95 {v[int(len(v) * 0.95)]:>7} us")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--table", default=None,
                        help="table name; default bench_<pid>_<epoch>, i.e. fresh per run")
    parser.add_argument("--rows", type=int, default=2000,
                        help="rows inserted in the write phase (default: 2000)")
    parser.add_argument("--read-ops", type=int, default=1000,
                        help="point SELECTs in the read phase (default: 1000)")
    parser.add_argument("--scan-ops", type=int, default=20,
                        help="full-table SELECTs (default: 20); each returns every row")
    parser.add_argument("--update-ops", type=int, default=1000,
                        help="UPDATEs in the write phase (default: 1000)")
    parser.add_argument("--join-rows", type=int, default=200,
                        help="rows in the join phases' driving relation (default: 200); "
                             "0 skips the join phases entirely. Each join query reads all "
                             "of them, so this is the outer loop's trip count")
    parser.add_argument("--join-ops", type=int, default=20,
                        help="join-probe and join-semi queries (default: 20)")
    parser.add_argument("--join-scan-ops", type=int, default=5,
                        help="join-scan queries (default: 5). Lower than --join-ops "
                             "because this phase is quadratic: --join-rows x --rows tuple "
                             "decodes per query, which at the defaults is 400,000")
    parser.add_argument("--warmup", type=int, default=50,
                        help="untimed PINGs before the first phase (default: 50)")
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed for values and probed ids (default: 1)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="socket timeout in seconds (default: 60); a full scan of a "
                             "large table is a slow single reply")
    parser.add_argument("--clustered", choices=("heap", "btree"), default="heap",
                        help="storage organization of the benchmark table (default: heap). "
                             "btree creates it with the CREATE TABLE ... BTREE clause, so a "
                             "point SELECT/UPDATE descends the clustered index instead of "
                             "scanning the page chain - flat in row count, and "
                             "authoritative, so a miss costs a descent rather than a "
                             "fallback scan")
    parser.add_argument("--sync", action="store_true",
                        help="send SYNC after the write phases and time it")
    parser.add_argument("--json", metavar="PATH",
                        help="also write the results as JSON to PATH")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the server's log file, written at --log-level debug: adds the "
                             "server-side per-statement microseconds the server itself "
                             "measured, which is the only way to see engine cost separately "
                             "from this client's round-trip floor")
    args = parser.parse_args()

    table = args.table or f"bench_{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}"
    rng = random.Random(args.seed)

    try:
        conn = ServerConnection(args.host, args.port, timeout=args.timeout)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}\n"
              f"start one with: ./build-release/kds_server /tmp/bench.db", file=sys.stderr)
        sys.exit(1)

    # The one-command-one-reply callable bench_common.run_phase scores; the
    # ckdbs reply is one line of text, so format_reply() is the whole
    # client-side decode.
    def execute(command):
        return format_reply(conn.send_command(command))

    phases = []
    try:
        for _ in range(args.warmup):
            conn.send_command("PING")
        create_table(conn, table, args.clustered)
        verify_clustered(conn, table, args.clustered)

        phases.append(check_phase(run_phase(
            execute, "insert", insert_commands(table, args.rows, rng),
            detail=f"4 body columns per row, one varchar of {TEXT_LEN} chars, " +
                   ("tail append into the page chain" if args.clustered == "heap"
                    else "descent to the rightmost leaf, split on full"))))

        resident = count_rows(conn, table)
        # Probe ids from the range this run just issued. Ids are unique and
        # ascending but not gapless (a failed insert burns one), so a miss is
        # possible; a miss still costs a full scan, which is what is timed.
        hi = max(1, resident)
        phases.append(check_phase(run_phase(
            execute, "point-select",
            (f"SELECT * FROM {table} WHERE id = {rng.randint(1, hi)}"
             for _ in range(args.read_ops)),
            detail=f"WHERE id = <random 1..{hi}>, " +
                   access_path(args.clustered))))

        if args.scan_ops:
            scan = run_phase(execute, "full-scan",
                             (f"SELECT * FROM {table}" for _ in range(args.scan_ops)),
                             detail=f"{resident} rows per reply")
            if scan.elapsed > 0:
                scan.detail += f", {resident * scan.ops / scan.elapsed:,.0f} rows/s"
            phases.append(check_phase(scan))

        phases.append(check_phase(run_phase(
            execute, "update",
            (f"UPDATE {table} SET c_int = {rng.randint(0, 1_000_000)} "
             f"WHERE id = {rng.randint(1, hi)}"
             for _ in range(args.update_ops)),
            detail="fixed-width column only (in-place overwrite), " +
                   access_path(args.clustered))))

        # ---- joins -------------------------------------------------------
        #
        # The driving relation is created and filled *after* the phases
        # above, so it cannot change what they measured: `resident` is
        # already taken, and the child table is a separate relation the
        # scan phase never saw.
        if args.join_rows > 0:
            child = f"{table}_child"
            child_reply = format_reply(
                conn.send_command(f"CREATE TABLE {child} ({CHILD_COLUMNS})"))
            if child_reply.startswith("ERR"):
                fail("could not create the join phases' driving relation", child_reply)

            # Untimed: this is fixture, not measurement.
            smalls = parent_small_values(conn, table)
            for command in child_insert_commands(child, args.join_rows, resident, smalls, rng):
                reply = execute(command)
                if reply.startswith("ERR"):
                    fail("could not populate the join phases' driving relation", reply)

            scan_matches = preflight_joins(conn, child, table, args.join_rows)

            probe = run_phase(execute, "join-probe",
                              (join_probe_sql(child, table) for _ in range(args.join_ops)),
                              detail=f"{args.join_rows} outer rows, pk equality -> Probe "
                                     f"({access_path(args.clustered)} per outer row), "
                                     f"{args.join_rows} rows per reply")
            if probe.elapsed > 0:
                probe.detail += f", {args.join_rows * probe.ops / probe.elapsed:,.0f} rows/s"
            phases.append(check_phase(probe))

            phases.append(check_phase(run_phase(
                execute, "join-semi",
                (join_semi_sql(child, table) for _ in range(args.join_ops)),
                detail=f"correlated EXISTS over the same pk equality; stops at the first "
                       f"qualifying row instead of pairing, {args.join_rows} rows per reply")))

            if args.join_scan_ops:
                comparisons = args.join_rows * max(1, resident)
                scan_join = run_phase(
                    execute, "join-scan",
                    (join_scan_sql(child, table) for _ in range(args.join_scan_ops)),
                    detail=f"non-pk equality -> Scan: the whole inner relation walked once "
                           f"per outer row, {comparisons:,} tuple decodes per query, "
                           f"{scan_matches} rows per reply")
                if scan_join.elapsed > 0:
                    scan_join.detail += (
                        f", {comparisons * scan_join.ops / scan_join.elapsed:,.0f} "
                        "comparisons/s")
                if scan_matches == 0:
                    # The walk is identical whether or not anything
                    # matches, so the timing still means what it says -
                    # but the reader should not have to wonder.
                    scan_join.detail += (
                        " (nothing matched: the inner relation is still walked in full "
                        "per outer row, so this times the same work, minus the reply "
                        "formatting)")
                phases.append(check_phase(scan_join))

        if args.sync:
            phases.append(check_phase(run_phase(
                execute, "sync", ["SYNC"],
                detail="page store flushed to the data file")))

    finally:
        conn.close()

    # The durability class governs the insert number more than anything the
    # engine does, so name it in the report when the server's log is at
    # hand rather than leaving the reader to guess (expeditor.cpp logs it
    # at Info on startup).
    durability = read_durability(args.server_log) if args.server_log else None
    durability_note = f" at durability={durability}" if durability else ""

    meta = {
        "engine": "ckdbs",
        "host": args.host,
        "port": args.port,
        "table": table,
        "columns": len(COLUMNS.split(",")),
        "rows": resident,
        "seed": args.seed,
        "durability": durability,
        "clustered": args.clustered,
        "join_rows": args.join_rows,
    }

    if args.clustered == "btree":
        access_note = ("point-select and update descend the clustered B+ tree: O(depth) "
                       "page fetches, so their qps is flat in the relation's size, and "
                       "the answer is authoritative - a pk that does not exist costs a "
                       "descent, not a fallback scan. The insert phase pays for that: a "
                       "descent per row, plus a leaf split and a full-page image in the "
                       "WAL once per page of relation.")
    else:
        access_note = ("point-select and update are both full page-chain scans (no "
                       f"index): their qps is a function of the {resident} resident rows.")

    if args.join_rows <= 0:
        join_note = None
    elif args.clustered == "btree":
        join_note = ("join-probe vs join-scan is the price of one compile-time decision: "
                     "the step compiler marks a join step Probe when its equality binds "
                     "the inner relation's pk and Scan otherwise. Here the Probe descends "
                     "the tree per outer row while the Scan walks the whole relation, so "
                     "the ratio between them is roughly the relation's size. That same "
                     "line is Waystone's: a Probe may be served from a recorded trail, a "
                     "Scan may only be prefetched for.")
    else:
        join_note = ("join-probe and join-scan will look alike on this run, and that is "
                     "the finding rather than a flaw: a heap relation has no pk index, so "
                     "the Probe step falls through to the same chain walk the Scan does "
                     "(step_vm.cpp's RunPointStep). Re-run with --clustered btree to "
                     "price the access kind - that is where the two separate.")

    report(phases, meta, footer=[
        access_note,
        f"INSERT is WAL-logged{durability_note}: under strict/group the reply "
        "waits on an fsync, which on a real disk dominates everything the "
        "engine does (~1 ms vs ~12 us of engine time) - so an insert number "
        "means nothing without the durability class beside it. UPDATE and "
        "CREATE TABLE are still unlogged. Do not measure this on tmpfs, where "
        "fsync is free and all three classes look identical.",
        "latencies include the Python client's own socket cost.",
    ] + ([join_note] if join_note else []) + ([] if args.clustered == "btree" else
         ["no index on this table: a point-select is a full chain scan. Re-run with "
          "--clustered btree (the relation stored as a clustered index) to compare."]))

    if args.server_log:
        try:
            report_server_side(server_side_us(args.server_log, table))
        except OSError as e:
            print(f"  --server-log: could not read {args.server_log}: {e}")

    if args.json:
        write_json(args.json, meta, phases)

    sys.exit(1 if any(p.errors for p in phases) else 0)


if __name__ == "__main__":
    main()
