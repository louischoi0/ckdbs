#!/usr/bin/env python3
"""PostgreSQL baseline for tools/benchmark.py - the same script, other engine.

Runs the identical four phases against the identical 5-column row shape, over
the same single connection, with the same per-request timing, so the two JSON
outputs are diffable phase by phase:

    insert        INSERT INTO <t> (4 body cols) VALUES (...)
    point-select  SELECT * FROM <t> WHERE id = <n>
    full-scan     SELECT * FROM <t>
    update        UPDATE <t> SET c_int = <n> WHERE id = <n>

Three things make this a fair baseline rather than a rigged one, and a number
quoted without them is misleading:

1. **Index parity is explicit, not assumed.** Which variant is comparable
   depends on the ckdbs run it is being diffed against: a `--clustered btree`
   relation has an O(depth) pk lookup on SELECT and UPDATE, so **`pk` is the
   apples-to-apples variant** for that run, and `noidx` is the one for a
   heap-clustered run, which has no index at all.
   `--index both` (default) runs every phase twice so either diff is available.
   Phase names carry the variant: `point-select[noidx]` vs `point-select[pk]`.
2. **The id column is server-generated on both sides.** ckdbs invariant 10
   forbids a caller-supplied pk, so the table uses
   `id bigint GENERATED ALWAYS AS IDENTITY` and INSERT names only the four
   body columns. An identity column is a sequence, not an index, so the
   `noidx` variant really has zero indexes (`SHOW INDEXES` in the run header
   proves it).
3. **Durability is not silently different**, and the mapping is per statement
   as of 2026-07-30. ckdbs INSERT *is* WAL-logged now, so
   `synchronous_commit = on` (PostgreSQL's default and this tool's) is the
   right analogue of `durability = strict|group`, and `off` of
   `durability = relaxed`. ckdbs UPDATE is still **entirely unlogged**, so
   only `--synchronous-commit off` is comparable for that phase - quoting a
   ckdbs update against PostgreSQL's default compares a non-durable write to
   a durable one. Run both and say which you mean.

Timing is a wall clock around one send + one receive through tools/pg_wire.py
(no libpq, no psycopg - see that module's docstring), so latencies include
Python's socket cost on both sides, and sub-10us differences are noise.

Usage:
    ./tools/pg_setup.sh init                     # scratch cluster on :15433
    python3 tools/pg_benchmark.py --port 15433 --database bench
    python3 tools/pg_benchmark.py --index noidx --rows 5000 --json pg.json
    python3 tools/pg_benchmark.py --synchronous-commit off --sync

    # side by side with ckdbs (same phases, same row shape):
    python3 tools/benchmark.py    --json /tmp/ckdbs.json
    python3 tools/pg_benchmark.py --json /tmp/pg.json

Each run creates its own tables (`pgbench_<epoch>_<rand>_<variant>`) so a
persistent cluster can be benchmarked repeatedly without earlier runs' rows
inflating the scan cost. `--keep` leaves them behind for inspection.
"""

import argparse
import random
import re
import sys
import time

from bench_common import report, run_phase, write_json
from pg_wire import DEFAULT_HOST, PgConnection, PgError

# Four body columns after the generated id = 5 columns total, matching
# benchmark.py's COLUMNS. Types are the closest PostgreSQL equivalents of
# ckdbs's int64 / int32 / bool / varchar, so the row's byte shape is
# comparable and the varchar path is exercised on both sides.
BODY_COLUMNS = "c_int bigint, c_small integer, c_flag boolean, c_text varchar(64)"
BODY_NAMES = "c_int, c_small, c_flag, c_text"

# benchmark.py writes the varchar once at insert time at a constant length,
# because ckdbs UPDATE cannot grow a row in place. Same length here so the
# rows are the same size.
TEXT_LEN = 16

VARIANTS = {
    "noidx": "no index at all: WHERE id = <n> is a seqscan, matching ckdbs's "
             "indexless read path",
    "pk": "PRIMARY KEY on id: btree index scan, what PostgreSQL does by default",
}


def create_table(conn, table, variant):
    """Creates one variant's table. `noidx` gets an identity column and no
    index; `pk` adds the primary key and nothing else."""
    identity = "id bigint GENERATED ALWAYS AS IDENTITY"
    if variant == "pk":
        identity += " PRIMARY KEY"
    reply = conn.send_command(f"CREATE TABLE {table} ({identity}, {BODY_COLUMNS})")
    if reply.startswith("ERR"):
        raise RuntimeError(f"CREATE TABLE failed: {reply}")


def index_count(conn, table):
    value = conn.scalar(f"SELECT count(*) FROM pg_index i JOIN pg_class c "
                        f"ON c.oid = i.indrelid WHERE c.relname = '{table}'")
    return int(value or b"0")


def insert_commands(table, rows, rng):
    for _ in range(rows):
        text = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=TEXT_LEN))
        # Same value ranges and same RNG call order as benchmark.py, so a
        # shared --seed produces the same data on both engines. PostgreSQL has
        # real boolean literals, but 0/1 casts fine and keeps the two
        # statements textually identical in length.
        yield (f"INSERT INTO {table} ({BODY_NAMES}) VALUES "
               f"({rng.randint(0, 1_000_000)}, {rng.randint(0, 30000)}, "
               f"{rng.randint(0, 1)}::boolean, '{text}')")


def count_rows(conn, table):
    return int(conn.scalar(f"SELECT count(*) FROM {table}") or b"0")


def run_variant(conn, table, variant, args, rng):
    """Runs the four phases against one table variant. Returns (phases, rows)."""
    create_table(conn, table, variant)
    execute = conn.send_command
    tag = f"[{variant}]"
    phases = []

    phases.append(run_phase(execute, f"insert{tag}",
                            insert_commands(table, args.rows, rng),
                            detail="4 body columns per row, one varchar of "
                                   f"{TEXT_LEN} chars"))

    resident = count_rows(conn, table)
    if args.analyze:
        # Only meaningful for `pk`: without it the planner may still pick a
        # seqscan on a freshly loaded table and the index would look useless.
        conn.send_command(f"ANALYZE {table}")
    hi = max(1, resident)
    phases.append(run_phase(execute, f"point-select{tag}",
                            (f"SELECT * FROM {table} WHERE id = {rng.randint(1, hi)}"
                             for _ in range(args.read_ops)),
                            detail=f"WHERE id = <random 1..{hi}>, {VARIANTS[variant]}"))

    if args.scan_ops:
        scan = run_phase(execute, f"full-scan{tag}",
                         (f"SELECT * FROM {table}" for _ in range(args.scan_ops)),
                         detail=f"{resident} rows per reply")
        if scan.elapsed > 0:
            scan.detail += f", {resident * scan.ops / scan.elapsed:,.0f} rows/s"
        phases.append(scan)

    phases.append(run_phase(execute, f"update{tag}",
                            (f"UPDATE {table} SET c_int = {rng.randint(0, 1_000_000)} "
                             f"WHERE id = {rng.randint(1, hi)}"
                             for _ in range(args.update_ops)),
                            detail="fixed-width column only; PostgreSQL still writes a "
                                   "new row version (MVCC), unlike ckdbs's in-place "
                                   "overwrite"))

    if args.sync:
        # The counterpart of ckdbs's SYNC (flush the page store): force a
        # checkpoint so the write phases' dirty buffers hit disk here too.
        phases.append(run_phase(execute, f"checkpoint{tag}", ["CHECKPOINT"],
                                detail="server-wide checkpoint; needs a superuser role"))
    return phases, resident


# ---- server-side timings -------------------------------------------------
#
# PostgreSQL's counterpart of the ckdbs debug log line. With
# log_min_duration_statement = 0 (./tools/pg_setup.sh timing on) the server
# writes one line per statement:
#
#   2026-07-30 05:40:01.123 UTC [1234] LOG:  duration: 0.081 ms  statement: INSERT ...
#
# That duration is the server's own measurement, excluding everything this
# client pays for - the same role the `in <n>us` field plays in benchmark.py.
# Filtering on the run's table name isolates this run's statements.

DURATION_LINE = re.compile(r"duration: ([\d.]+) ms\s+statement: (\w+)")


def server_side_us(log_path, table_prefix):
    """Maps statement keyword -> sorted server-side microseconds for this run."""
    by_kind = {}
    with open(log_path, errors="replace") as f:
        for line in f:
            if table_prefix not in line:
                continue
            m = DURATION_LINE.search(line)
            if m is None:
                continue
            by_kind.setdefault(m.group(2).upper(), []).append(float(m.group(1)) * 1000.0)
    return {kind: sorted(values) for kind, values in by_kind.items()}


def report_server_side(by_kind, variant=None):
    if not by_kind:
        print("  --server-log: no timed statements for this run's tables "
              "(run ./tools/pg_setup.sh timing on)")
        return
    scope = f" [{variant}]" if variant else ""
    print(f"  server-side{scope}, as measured by the server itself (excludes this "
          "client's round trip):")
    for kind in sorted(by_kind):
        v = by_kind[kind]
        print(f"    {kind:<8}{len(v):>7} stmts  p50 {v[len(v) // 2]:>7.0f} us"
              f"  p95 {v[int(len(v) * 0.95)]:>7.0f} us")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=15433,
                        help="default: 15433, the scratch cluster tools/pg_setup.sh builds")
    parser.add_argument("--user", default=None, help="default: $PGUSER, else $USER")
    parser.add_argument("--database", default="bench", help="default: bench")
    parser.add_argument("--password", default=None, help="default: $PGPASSWORD")
    parser.add_argument("--table", default=None,
                        help="table name prefix; default pgbench_<epoch>_<rand>, "
                             "i.e. fresh per run. One table per --index variant.")
    parser.add_argument("--index", choices=["both", "noidx", "pk"], default="both",
                        help="which table variants to run (default: both) - see the "
                             "index-parity note in this tool's description")
    parser.add_argument("--rows", type=int, default=2000,
                        help="rows inserted in the write phase (default: 2000)")
    parser.add_argument("--read-ops", type=int, default=1000,
                        help="point SELECTs in the read phase (default: 1000)")
    parser.add_argument("--scan-ops", type=int, default=20,
                        help="full-table SELECTs (default: 20); each returns every row")
    parser.add_argument("--update-ops", type=int, default=1000,
                        help="UPDATEs in the write phase (default: 1000)")
    parser.add_argument("--warmup", type=int, default=50,
                        help="untimed round trips before the first phase (default: 50)")
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed; the same seed as benchmark.py generates the "
                             "same rows (default: 1)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="socket timeout in seconds (default: 60)")
    parser.add_argument("--synchronous-commit", choices=["on", "off", "local", "remote_write"],
                        default=None,
                        help="SET synchronous_commit for this session. Unset leaves the "
                             "server default (on): every write waits for a WAL fsync. "
                             "off is the closer analogue of today's ckdbs write path.")
    parser.add_argument("--analyze", action="store_true",
                        help="ANALYZE after the insert phase (planner stats); without it a "
                             "freshly loaded pk table may still be seqscanned")
    parser.add_argument("--sync", action="store_true",
                        help="issue CHECKPOINT after the write phases and time it, the "
                             "counterpart of ckdbs's SYNC")
    parser.add_argument("--keep", action="store_true",
                        help="do not DROP the run's tables on exit")
    parser.add_argument("--json", metavar="PATH", help="also write the results as JSON to PATH")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the cluster's log file (default $HOME/pg-bench/pg.log): adds "
                             "the server-side per-statement microseconds PostgreSQL itself "
                             "measured, with log_min_duration_statement = 0")
    args = parser.parse_args()

    prefix = args.table or f"pgbench_{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}"
    variants = ["noidx", "pk"] if args.index == "both" else [args.index]

    try:
        conn = PgConnection(args.host, args.port, args.user, args.database,
                            args.password, timeout=args.timeout)
    except (OSError, PgError) as e:
        print(f"could not connect to {args.host}:{args.port}/{args.database}: {e}\n"
              f"start one with: ./tools/pg_setup.sh init", file=sys.stderr)
        sys.exit(1)

    server_version = (conn.scalar("SHOW server_version") or b"?").decode()
    phases, resident, tables = [], 0, []
    try:
        if args.synchronous_commit:
            reply = conn.send_command(f"SET synchronous_commit = {args.synchronous_commit}")
            if reply.startswith("ERR"):
                raise RuntimeError(f"SET synchronous_commit failed: {reply}")
        sync_commit = (conn.scalar("SHOW synchronous_commit") or b"?").decode()

        for _ in range(args.warmup):
            conn.send_command("SELECT 1")

        for variant in variants:
            # A fresh RNG per variant, so both variants see identical data and
            # probe identical ids - the only difference between them is the
            # index.
            rng = random.Random(args.seed)
            table = f"{prefix}_{variant}"
            tables.append(table)
            variant_phases, resident = run_variant(conn, table, variant, args, rng)
            indexes = index_count(conn, table)
            if variant == "noidx" and indexes != 0:
                raise RuntimeError(f"{table} unexpectedly has {indexes} index(es)")
            phases.extend(variant_phases)

        meta = {
            "engine": f"PostgreSQL {server_version}",
            "host": args.host,
            "port": args.port,
            "table": prefix + "_{" + ",".join(variants) + "}",
            "columns": 5,
            "rows": resident,
            "seed": args.seed,
            "index_variants": variants,
            "synchronous_commit": sync_commit,
            "analyze": args.analyze,
        }
    finally:
        if not args.keep:
            for table in tables:
                conn.send_command(f"DROP TABLE IF EXISTS {table}")

    footer = [
        f"synchronous_commit = {sync_commit}: "
        + ("every INSERT/UPDATE above waited for a WAL fsync; today's ckdbs server "
           "does not append to its WAL at all, so its write numbers are not paying "
           "this. Compare with --synchronous-commit off too."
           if sync_commit in ("on", "remote_apply") else
           "writes did NOT wait for a WAL fsync - closer to today's ckdbs write path, "
           "but not a durable configuration."),
        ("point-select[noidx] is the comparable number for ckdbs (both seqscan); "
         "point-select[pk] shows what an index buys."
         if variants == ["noidx", "pk"] else
         f"only the {variants[0]} variant ran: "
         + ("comparable to ckdbs, which also has no index."
            if variants[0] == "noidx" else
            "a btree index scan, which ckdbs has no counterpart for yet - not "
            "comparable to its point-select.")),
        "PostgreSQL UPDATE writes a new row version and leaves dead tuples behind; "
        "no VACUUM ran between phases.",
        "latencies include the Python client's own socket cost, as in benchmark.py.",
    ]
    report(phases, meta, footer)

    log_path = args.server_log
    if log_path:
        # Per variant, not per run: a seqscan and an index scan of the same
        # statement text would otherwise be averaged into one meaningless
        # SELECT row.
        try:
            for table in tables:
                report_server_side(server_side_us(log_path, table),
                                   table.rsplit("_", 1)[-1] if len(tables) > 1 else None)
        except OSError as e:
            print(f"  --server-log: could not read {log_path}: {e}")

    if args.json:
        write_json(args.json, meta, phases)

    conn.close()
    sys.exit(1 if any(p.errors for p in phases) else 0)


if __name__ == "__main__":
    main()
