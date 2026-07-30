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

Four phases, each timed separately and reported as queries/sec plus a
latency distribution:

    insert        INSERT INTO <t> VALUES (...)         write path
    point-select  SELECT * FROM <t> WHERE id = <n>     read path, 1 row out
    full-scan     SELECT * FROM <t>                    read path, all rows out
    update        UPDATE <t> SET c_int = <n> WHERE id = <n>   write path

Two properties of today's engine shape the numbers, and quoting them
without both is misleading:

1. **There is no index.** A `WHERE id = <n>` SELECT is a full scan of the
   relation's whole page chain (docs/client-manual.md section 3), so
   point-select QPS falls roughly as 1/--rows. A point-select number is
   only meaningful next to the row count it was measured at.
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

# UPDATE is an in-place (HOT-style) overwrite with no relocation fallback:
# a new value that outgrows the row's original slot reservation is an ERR
# (docs/client-manual.md section 3). So the update phase only ever rewrites
# the fixed-width c_int column, and the varchar is written once at insert
# time at a constant length.
TEXT_LEN = 16


def create_table(conn, table):
    reply = format_reply(conn.send_command(f"CREATE TABLE {table} ({COLUMNS})"))
    if reply.startswith("ERR"):
        raise RuntimeError(f"CREATE TABLE failed: {reply}")
    return reply


def insert_commands(table, rows, rng):
    for _ in range(rows):
        text = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=TEXT_LEN))
        # The bool column takes 0/1: this grammar has no boolean literal
        # (row_codec.cpp's "expects 0 or 1" check).
        yield (f"INSERT INTO {table} VALUES "
               f"({rng.randint(0, 1_000_000)}, {rng.randint(0, 30000)}, "
               f"{rng.randint(0, 1)}, '{text}')")


def count_rows(conn, table):
    """Rows currently visible to a full scan - the reply is a header line
    plus one line per row, joined with the literal `\\n` escape."""
    text = format_reply(conn.send_command(f"SELECT * FROM {table}"))
    if text.startswith("ERR"):
        raise RuntimeError(f"scan failed: {text}")
    return max(0, len([line for line in text.split("\n") if line != ""]) - 1)


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
    parser.add_argument("--warmup", type=int, default=50,
                        help="untimed PINGs before the first phase (default: 50)")
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed for values and probed ids (default: 1)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="socket timeout in seconds (default: 60); a full scan of a "
                             "large table is a slow single reply")
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
        create_table(conn, table)

        phases.append(run_phase(execute, "insert",
                                insert_commands(table, args.rows, rng),
                                detail="4 body columns per row, one varchar of "
                                       f"{TEXT_LEN} chars"))

        resident = count_rows(conn, table)
        # Probe ids from the range this run just issued. Ids are unique and
        # ascending but not gapless (a failed insert burns one), so a miss is
        # possible; a miss still costs a full scan, which is what is timed.
        hi = max(1, resident)
        phases.append(run_phase(execute, "point-select",
                                (f"SELECT * FROM {table} WHERE id = {rng.randint(1, hi)}"
                                 for _ in range(args.read_ops)),
                                detail=f"WHERE id = <random 1..{hi}>, full chain scan"))

        if args.scan_ops:
            scan = run_phase(execute, "full-scan",
                             (f"SELECT * FROM {table}" for _ in range(args.scan_ops)),
                             detail=f"{resident} rows per reply")
            if scan.elapsed > 0:
                scan.detail += f", {resident * scan.ops / scan.elapsed:,.0f} rows/s"
            phases.append(scan)

        phases.append(run_phase(execute, "update",
                                (f"UPDATE {table} SET c_int = {rng.randint(0, 1_000_000)} "
                                 f"WHERE id = {rng.randint(1, hi)}"
                                 for _ in range(args.update_ops)),
                                detail="fixed-width column only (in-place overwrite)"))

        if args.sync:
            phases.append(run_phase(execute, "sync", ["SYNC"],
                                    detail="page store flushed to the data file"))
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
    }
    report(phases, meta, footer=[
        "point-select is a full page-chain scan (no index): its qps is a "
        f"function of the {resident} resident rows.",
        f"INSERT is WAL-logged{durability_note}: under strict/group the reply "
        "waits on an fsync, which on a real disk dominates everything the "
        "engine does (~1 ms vs ~12 us of engine time) - so an insert number "
        "means nothing without the durability class beside it. UPDATE and "
        "CREATE TABLE are still unlogged. Do not measure this on tmpfs, where "
        "fsync is free and all three classes look identical.",
        "latencies include the Python client's own socket cost.",
    ])

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
