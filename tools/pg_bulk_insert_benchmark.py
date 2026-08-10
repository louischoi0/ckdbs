#!/usr/bin/env python3
"""PostgreSQL twin of tools/bulk_insert_benchmark.py (BLK08).

Same matrix - rows-per-statement 1/10/100/1000 at a fixed total row
count - against the tools/pg_setup.sh scratch cluster on port 15433, whose
tuning stays at PostgreSQL defaults. The relation is the pg twin of
scenario1's write_probe: `(id bigint GENERATED ALWAYS AS IDENTITY,
a bigint, b bigint, c bigint, d bigint)`, values supplied through an
explicit column list because the pk is engine-issued on both sides.

Durability twin: ckdbs's `durability` is a per-transaction property, and
PostgreSQL's counterpart is `synchronous_commit`, a per-session/-txn GUC
clients set in the ordinary course of business. `--synchronous-commit off`
issues `SET synchronous_commit = off` on the session as the twin of
ckdbs `relaxed`; the default (`on`) is the twin of `group`/`strict` (one
connection, so group's amortization has nobody to share with). Cluster
configuration is not touched.

The parse probe (INSERT into a table that does not exist) is refused by
PostgreSQL at analysis after a full parse, same as ckdbs refuses it at
catalog resolution - round trip + parse, no write pipeline, ERR expected.

Usage:
    ./tools/pg_setup.sh start
    python3 tools/pg_bulk_insert_benchmark.py --port 15433 --database bench \
        --rows 100000 --synchronous-commit off --txn-control --parse-probe \
        --json ~/bench-bulk/pg-relaxed.json
"""

import argparse
import sys
import time

from bench_common import Phase, report, run_phase, write_json
from pg_wire import PgConnection

COLUMNS = ("(id bigint GENERATED ALWAYS AS IDENTITY, "
           "a bigint, b bigint, c bigint, d bigint)")


def row_values(i):
    return f"({i}, {i * 2}, {i * 3}, {i * 5})"


def bulk_statement(table, start, count):
    rows = ", ".join(row_values(i) for i in range(start, start + count))
    return f"INSERT INTO {table} (a, b, c, d) VALUES {rows}"


def run_bulk_phase(client, table, batch, total_rows):
    statements = []
    sent = 0
    while sent < total_rows:
        n = min(batch, total_rows - sent)
        statements.append((bulk_statement(table, sent, n), n))
        sent += n

    phase = Phase(f"bulk-{batch}", f"{batch} rows/statement, "
                                   f"{len(statements)} statements, "
                                   f"{total_rows} rows")
    started = time.perf_counter()
    for stmt, n in statements:
        t0 = time.perf_counter()
        reply = client(stmt)
        phase.record(time.perf_counter() - t0, reply)
        if not reply.startswith(f"OK INSERT 0 {n}"):
            sys.exit(f"FATAL: bad INSERT reply for {n} rows: {reply!r}")
    phase.elapsed = time.perf_counter() - started
    return phase


def run_txn_control(client, table, txn_rows, total_rows):
    phase = Phase("txn-1000", f"single-row INSERTs, {txn_rows}/txn, "
                              f"{total_rows} rows")
    started = time.perf_counter()
    in_txn = 0
    for i in range(total_rows):
        if in_txn == 0:
            client("BEGIN")
        t0 = time.perf_counter()
        reply = client(f"INSERT INTO {table} (a, b, c, d) "
                       f"VALUES {row_values(i)}")
        phase.record(time.perf_counter() - t0, reply)
        in_txn += 1
        if in_txn == txn_rows:
            reply = client("COMMIT")
            if not reply.startswith("OK COMMIT"):
                sys.exit(f"FATAL: COMMIT failed: {reply!r}")
            in_txn = 0
    if in_txn:
        client("COMMIT")
    phase.elapsed = time.perf_counter() - started
    return phase


def run_parse_probe(client, batch, ops):
    stmt = bulk_statement("no_such_relation_xx", 0, batch)
    phase = Phase(f"parse-{batch}", f"{batch}-row parse+dispatch probe, "
                                    f"{len(stmt)} bytes, ERR expected")
    started = time.perf_counter()
    for _ in range(ops):
        t0 = time.perf_counter()
        reply = client(stmt)
        phase.record(time.perf_counter() - t0, reply)
        if "no_such_relation_xx" not in reply:
            sys.exit(f"FATAL: parse probe got an unexpected reply: {reply!r}")
    phase.elapsed = time.perf_counter() - started
    phase.detail += f" ({phase.errors} ERR replies, all expected)"
    phase.errors = 0
    phase.first_error = None
    return phase


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15433)
    ap.add_argument("--database", default="bench")
    ap.add_argument("--user", default=None)
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--batches", default="1,10,100,1000")
    ap.add_argument("--synchronous-commit", default="on",
                    choices=("on", "off"))
    ap.add_argument("--txn-control", action="store_true")
    ap.add_argument("--parse-probe", action="store_true")
    ap.add_argument("--parse-ops", type=int, default=200)
    ap.add_argument("--ping-ops", type=int, default=500)
    ap.add_argument("--suffix", default=str(int(time.time())))
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    batches = [int(b) for b in args.batches.split(",") if b]
    conn = PgConnection(host=args.host, port=args.port, user=args.user,
                        database=args.database)
    client = conn.send_command

    if args.synchronous_commit == "off":
        reply = client("SET synchronous_commit = off")
        if reply.startswith("ERR"):
            sys.exit(f"FATAL: SET synchronous_commit: {reply!r}")

    phases = []
    phases.append(run_phase(client, "ping",
                            ("SELECT 1" for _ in range(args.ping_ops)),
                            f"{args.ping_ops} ops, SELECT 1 floor"))

    for batch in batches:
        table = f"bulk_{args.suffix}_{batch}"
        reply = client(f"CREATE TABLE {table} {COLUMNS}")
        if reply.startswith("ERR"):
            sys.exit(f"FATAL: CREATE TABLE {table}: {reply!r}")
        phase = run_bulk_phase(client, table, batch, args.rows)
        counted = int(conn.scalar(f"SELECT COUNT(*) FROM {table}"))
        if counted != args.rows:
            sys.exit(f"FATAL: {table} holds {counted} rows, sent {args.rows}")
        phase.detail += ", COUNT verified"
        phases.append(phase)
        print(f"  bulk-{batch:>5}: {phase.qps * batch:>10,.0f} rows/s "
              f"({phase.qps:,.0f} stmts/s), COUNT ok", flush=True)

    if args.txn_control:
        table = f"bulk_{args.suffix}_txn"
        reply = client(f"CREATE TABLE {table} {COLUMNS}")
        if reply.startswith("ERR"):
            sys.exit(f"FATAL: CREATE TABLE {table}: {reply!r}")
        phase = run_txn_control(client, table, 1000, args.rows)
        counted = int(conn.scalar(f"SELECT COUNT(*) FROM {table}"))
        if counted != args.rows:
            sys.exit(f"FATAL: {table} holds {counted} rows, sent {args.rows}")
        phase.detail += ", COUNT verified"
        phases.append(phase)
        print(f"  txn-1000  : {args.rows / phase.elapsed:>10,.0f} rows/s "
              f"(incl. BEGIN/COMMIT)", flush=True)

    if args.parse_probe:
        for batch in batches:
            phases.append(run_parse_probe(client, batch, args.parse_ops))

    meta = {
        "engine": f"postgresql (synchronous_commit={args.synchronous_commit})",
        "driver": "pg_bulk_insert_benchmark.py",
        "durability": f"synchronous_commit={args.synchronous_commit}",
        "columns": 5,
        "rows": args.rows,
        "batches": batches,
        "host": args.host,
        "port": args.port,
        "table": f"bulk_{args.suffix}_<batch>",
    }
    report(phases, meta, footer=(
        "bulk-<B> latencies are per STATEMENT (B rows each); divide by B "
        "for per-row cost.",
        "cluster tuning is PostgreSQL defaults; synchronous_commit is a "
        "session GUC, the twin of ckdbs's per-transaction durability.",
    ))
    if args.json:
        write_json(args.json, meta, phases)
    conn.close()


if __name__ == "__main__":
    main()
