#!/usr/bin/env python3
"""Prices the T1 multi-row INSERT (docs/spec/bulkinsert.md, BLK08).

The question this driver answers, in one sentence: how much of the
~21 us/row statement cost that `bench/results-scenario1-vs-pg.md` measured
on single-row INSERTs does a multi-row VALUES statement actually remove?

The matrix is rows-per-statement (1 / 10 / 100 / 1000 by default), at a
fixed total row count so every configuration does equal work and the
throughput numbers compare. Every batch size writes into a fresh relation
of scenario1's `write_probe` shape - `(id int64, a int64, b int64,
c int64, d int64) HEAP`, four int64 values supplied per row - so the
batch-1 phase is directly comparable against the old baseline's insert
sweep.

Four phase families:

    ping           500 x PING: the client+socket round-trip floor.
    bulk-<B>       rows/B statements of B rows each, autocommitted -
                   one durability point per statement.
    txn-1000       the OLD way of batching, re-measured on this commit:
                   single-row INSERTs inside BEGIN/COMMIT transactions of
                   1000. This is the configuration the 21 us/row baseline
                   came from; the gap between it and bulk-1000 is what T1
                   removed.  (--txn-control to enable)
    parse-<B>      a B-row INSERT into a table that does not exist: the
                   server lexes and parses the whole statement, then fails
                   at catalog resolution before touching the write
                   pipeline. Round trip + parse, no rows - the probe that
                   splits parse cost from pipeline cost. Its replies are
                   ERR by design; the phase reports them as expected_err
                   rather than errors.  (--parse-probe to enable)

Durability is a server config key, not a client property, so one run of
this driver measures ONE durability class - start the server with the
class under test (`durability = relaxed|group|strict` in --config) and
tell the driver which it was via --durability so the JSON is labeled.

Verification: after every bulk phase the driver checks
`SELECT COUNT(*)` == the rows it sent, checks every reply's `rows=` field,
and checks id continuity (first_id/last_id arithmetic; contiguity is not
promised by the engine, but on a single connection a gap means lost or
double-counted work). A failure aborts the run - a throughput number over
a workload that lost writes measures nothing.

Usage:
    ./build-release/kds_server ~/bench-bulk/relaxed.db --port 15599 \
        --config relaxed.conf   # durability = relaxed
    python3 tools/bulk_insert_benchmark.py --port 15599 \
        --durability relaxed --rows 100000 --txn-control --parse-probe \
        --json ~/bench-bulk/relaxed.json
"""

import argparse
import re
import sys
import time

from bench_common import Phase, report, run_phase, write_json
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection

# --trace: keep each bulk phase's (arrival, latency) series and write it
# into the JSON, so a wide distribution can be told apart: a trend line is
# per-relation-size growth, an oscillation is something periodic.
TRACE = False

# scenario1's write_probe shape: the Keystone pk plus four int64 body
# columns, heap-clustered, so batch-1 numbers line up against
# bench/results-scenario1-vs-pg.md's insert sweep.
COLUMNS = "(id int64, a int64, b int64, c int64, d int64) HEAP"

BULK_REPLY = re.compile(
    r"^INSERTED oid=\d+ rows=(\d+) first_id=(\d+) last_id=(\d+)$")
SINGLE_REPLY = re.compile(r"^INSERTED oid=\d+ id=(\d+) page=\d+ slot=\d+$")


def row_values(i):
    """One row's value list, matching scenario1's write sweep exactly."""
    return f"({i}, {i * 2}, {i * 3}, {i * 5})"


def bulk_statement(table, start, count):
    rows = ", ".join(row_values(i) for i in range(start, start + count))
    return f"INSERT INTO {table} VALUES {rows}"


def count_rows(client, table):
    reply = client(f"SELECT COUNT(*) FROM {table}")
    m = re.search(r"\\n(\d+)", reply)
    if not m:
        sys.exit(f"FATAL: unparseable COUNT reply for {table}: {reply!r}")
    return int(m.group(1))


def run_bulk_phase(client, table, batch, total_rows):
    """total_rows rows in ceil(total/batch) statements of `batch` rows."""
    statements = []
    sent = 0
    while sent < total_rows:
        n = min(batch, total_rows - sent)
        statements.append((bulk_statement(table, sent, n), n))
        sent += n

    phase = Phase(f"bulk-{batch}", f"{batch} rows/statement, "
                                   f"{len(statements)} statements, "
                                   f"{total_rows} rows")
    if TRACE:
        phase.trace = []
    ids_seen = 0
    first_seen = None
    last_seen = None
    started = time.perf_counter()
    for stmt, n in statements:
        t0 = time.perf_counter()
        reply = client(stmt)
        phase.record(time.perf_counter() - t0, reply)
        # Verification outside the per-statement clock, inside the phase.
        if n > 1:
            m = BULK_REPLY.match(reply)
            if not m or int(m.group(1)) != n:
                sys.exit(f"FATAL: bad bulk reply for {n} rows: {reply!r}")
            ids_seen += int(m.group(3)) - int(m.group(2)) + 1
            if first_seen is None:
                first_seen = int(m.group(2))
            last_seen = int(m.group(3))
        else:
            m = SINGLE_REPLY.match(reply)
            if not m:
                sys.exit(f"FATAL: bad single-row reply: {reply!r}")
            ids_seen += 1
            if first_seen is None:
                first_seen = int(m.group(1))
            last_seen = int(m.group(1))
    phase.elapsed = time.perf_counter() - started

    # The id span check: on one connection nothing else allocates, so the
    # span [first, last] must hold exactly the rows this phase inserted.
    # (Contiguity is not an engine promise; a hole here means lost work.)
    if total_rows == 0:
        return phase
    if last_seen - first_seen + 1 != total_rows or ids_seen != total_rows:
        sys.exit(f"FATAL: id span [{first_seen},{last_seen}] does not match "
                 f"{total_rows} rows ({ids_seen} counted from replies)")
    return phase


def run_txn_control(client, table, txn_rows, total_rows):
    """Single-row INSERTs batched by transaction: the pre-T1 batching."""
    phase = Phase("txn-1000", f"single-row INSERTs, {txn_rows}/txn, "
                              f"{total_rows} rows")
    started = time.perf_counter()
    in_txn = 0
    for i in range(total_rows):
        if in_txn == 0:
            reply = client("BEGIN")
            if not reply.startswith("BEGIN"):
                sys.exit(f"FATAL: BEGIN failed: {reply!r}")
        t0 = time.perf_counter()
        reply = client(f"INSERT INTO {table} VALUES {row_values(i)}")
        phase.record(time.perf_counter() - t0, reply)
        in_txn += 1
        if in_txn == txn_rows:
            reply = client("COMMIT")
            if not reply.startswith("COMMIT"):
                sys.exit(f"FATAL: COMMIT failed: {reply!r}")
            in_txn = 0
    if in_txn:
        client("COMMIT")
    phase.elapsed = time.perf_counter() - started
    return phase


def run_parse_probe(client, batch, ops):
    """A B-row INSERT into a nonexistent relation: round trip + full parse,
    refused at catalog resolution before the write pipeline. ERR expected."""
    stmt = bulk_statement("no_such_relation_xx", 0, batch)
    phase = Phase(f"parse-{batch}", f"{batch}-row parse+dispatch probe, "
                                    f"{len(stmt)} bytes, ERR expected")
    started = time.perf_counter()
    for _ in range(ops):
        t0 = time.perf_counter()
        reply = client(stmt)
        phase.record(time.perf_counter() - t0, reply)
        if "no table" not in reply and "unknown" not in reply.lower():
            sys.exit(f"FATAL: parse probe got an unexpected reply: {reply!r}")
    phase.elapsed = time.perf_counter() - started
    # The ERRs are the phase working as designed, not failures.
    phase.detail += f" ({phase.errors} ERR replies, all expected)"
    phase.errors = 0
    phase.first_error = None
    return phase


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--rows", type=int, default=100000,
                    help="total rows per batch-size configuration")
    ap.add_argument("--batches", default="1,10,100,1000",
                    help="comma-separated rows-per-statement list")
    ap.add_argument("--durability", default="unknown",
                    help="the server's durability class, for the JSON label")
    ap.add_argument("--txn-control", action="store_true",
                    help="also run the pre-T1 transaction-batching control")
    ap.add_argument("--parse-probe", action="store_true",
                    help="also run the parse+dispatch probes")
    ap.add_argument("--parse-ops", type=int, default=200)
    ap.add_argument("--ping-ops", type=int, default=500)
    ap.add_argument("--cabin", action="store_true",
                    help="CREATE CABIN ON <table>(a) after each CREATE TABLE, "
                         "which closes the T3 sorted-fill gate (cabin_mask) "
                         "and forces the per-row loop - the gate-closed twin")
    ap.add_argument("--suffix", default=str(int(time.time())) )
    ap.add_argument("--trace", action="store_true",
                    help="record per-statement (arrival, latency) series "
                         "for the bulk phases into the JSON")
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    global TRACE
    TRACE = args.trace

    batches = [int(b) for b in args.batches.split(",") if b]
    conn = ServerConnection(args.host, args.port, timeout=120.0)
    client = conn.send_command

    phases = []

    phases.append(run_phase(client, "ping",
                            ("PING" for _ in range(args.ping_ops)),
                            f"{args.ping_ops} ops, client+socket floor"))

    for batch in batches:
        table = f"bulk_{args.suffix}_{batch}"
        reply = client(f"CREATE TABLE {table} {COLUMNS}")
        if reply.startswith("ERR"):
            sys.exit(f"FATAL: CREATE TABLE {table}: {reply!r}")
        if args.cabin:
            reply = client(f"CREATE CABIN ON {table}(a)")
            if not reply.startswith("CREATED CABIN"):
                sys.exit(f"FATAL: CREATE CABIN on {table}: {reply!r}")
        phase = run_bulk_phase(client, table, batch, args.rows)
        counted = count_rows(client, table)
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
        counted = count_rows(client, table)
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
        "engine": "ckdbs",
        "driver": "bulk_insert_benchmark.py",
        "durability": args.durability,
        "columns": 5,
        "rows": args.rows,
        "batches": batches,
        "clustered": "heap",
        "host": args.host,
        "port": args.port,
        "table": f"bulk_{args.suffix}_<batch>",
    }
    report(phases, meta, footer=(
        "bulk-<B> latencies are per STATEMENT (B rows each); divide by B "
        "for per-row cost.",
        "txn-1000 latencies are per single-row INSERT; its elapsed/qps "
        "include the BEGIN/COMMIT round trips.",
        "parse-<B> statements parse fully and fail at catalog resolution: "
        "round trip + parse, no write pipeline.",
    ))
    if args.json:
        write_json(args.json, meta, phases)
        if TRACE:
            import json as _json
            traces = {p.name: p.trace for p in phases if p.trace}
            with open(args.json + ".trace.json", "w") as f:
                _json.dump(traces, f)
            print(f"  wrote {args.json}.trace.json")
    conn.close()


if __name__ == "__main__":
    main()
