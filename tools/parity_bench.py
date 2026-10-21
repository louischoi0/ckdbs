#!/usr/bin/env python3
"""KDS against PostgreSQL on equal clients: the read shapes scenario 1 left
behind PostgreSQL, beside the ones it led, over one data set loaded into both.

Why this exists. The scenario drivers time `ckdbs_cli.send_command`, which
decodes and renders every returned field to text, while their PostgreSQL
twins time `pg_wire` paths that decode less or nothing; on this host the
difference is about 25 µs per statement and 3.8 µs per returned 9-column
row of client CPU, charged to KDS. Here both sides go through a `query()`
that counts rows and walks field lengths and builds no per-field object
(`kwp.Connection.query`, `pg_wire.PgConnection.query`), and each cell also
reads the server's own CPU, which no client choice touches:

  * KDS: the sum of every server thread's `schedstat` on-CPU time (reactor,
    WAL writer, data-sync), so a cost a background thread pays is counted -
    and so is the idle tick, whose rate is measured once per run and
    reported (`idle_cpu_us_per_s`);
  * PostgreSQL: the session backend's `schedstat` only, with
    `max_parallel_workers_per_gather = 0` so no worker's CPU goes uncounted
    (and KDS runs at `cores = 1`).

The two engines run at their defaults otherwise. **KDS records no Waystone
trail by default**: since CF-S1b nothing records or replays until a shape is
opted in with `WAYSTONE ON`, and this driver opts none in, so this is a
scan-against-scan comparison; a not-opted SELECT pays one memory load.

Shapes (`--shapes` picks a subset), all over `pb_bars` (8 symbols x 7,560
sessions = 60,480 rows by default), `pb_symbols` and `pb_exchanges`; no
secondary index on either engine, so a non-key filter has no index to use:

  pk-lookup       tuple key equality, 1 row
  bar-range       tuple key BETWEEN, 201 rows
  symbol-history  symbol_id equality, 7,560 rows
  day-slice       session_no equality, 8 rows
  point-join      tuple key anchored 3-relation join, 1 row
  agg-global      COUNT/MIN/MAX/SUM over the whole relation, 1 row
  agg-by-session  GROUP BY session_no, 7,560 rows

Cells interleave: each rep runs every shape on both engines, the order of
the two engines alternating per rep (KDS first on odd reps). Each cell is
preceded by an untimed warm-up of a tenth of its ops, drawn from a separate
generator so the timed statements are not its repeats. Both engines must
return the same rows per statement in every rep, or the run stops.

Usage:
    parity_bench.py --kds-port 15791 --kds-pid 1234 --pg-port 15434 \\
        --pg-db bench --reps 5 --json out.json --status status.txt
"""

import argparse
import json
import os
import random
import statistics
import sys
import time

import kwp
import pg_wire
from bb_overhead_benchmark import host_state
from bn_census import sched_ns
from bn_pg_scan import cpu_ns as backend_ns


def bar_range(r, a):
    k = r.randint(1, a.bars - 200)
    return f"SELECT * FROM pb_bars WHERE id BETWEEN {k} AND {k + 200}"


SHAPES = {
    # name: (ops per cell, statement for a random key)
    "pk-lookup": (2000, lambda r, a: f"SELECT * FROM pb_bars WHERE id = {r.randint(1, a.bars)}"),
    "bar-range": (300, bar_range),
    "symbol-history": (10, lambda r, a: f"SELECT * FROM pb_bars WHERE symbol_id = {r.randint(1, a.symbols)}"),
    "day-slice": (200, lambda r, a: f"SELECT * FROM pb_bars WHERE session_no = {r.randint(0, a.sessions - 1)}"),
    "point-join": (2000, lambda r, a: (
        "SELECT b.id, b.close, s.ticker, e.code FROM pb_bars AS b "
        "JOIN pb_symbols AS s ON b.symbol_id = s.id "
        "JOIN pb_exchanges AS e ON s.exchange_id = e.id "
        f"WHERE b.id = {r.randint(1, a.bars)}")),
    "agg-global": (20, lambda r, a: "SELECT COUNT(*), MIN(close), MAX(close), SUM(volume) FROM pb_bars"),
    "agg-by-session": (10, lambda r, a: "SELECT session_no, COUNT(*), SUM(volume) FROM pb_bars GROUP BY session_no"),
}

# table: [(column, KDS type, PostgreSQL type)]; the first column is the tuple key.
SCHEMA = {
    "pb_exchanges": [("id", "int64", "bigint"), ("code", "varchar", "text"), ("country", "varchar", "text")],
    "pb_symbols": [("id", "int64", "bigint"), ("ticker", "varchar", "text"), ("exchange_id", "int64", "bigint")],
    "pb_bars": [("id", "int64", "bigint"), ("symbol_id", "int64", "bigint"), ("session_no", "int32", "integer"),
                ("open", "int64", "bigint"), ("high", "int64", "bigint"), ("low", "int64", "bigint"),
                ("close", "int64", "bigint"), ("volume", "int64", "bigint")],
}
TABLES = ("pb_exchanges", "pb_symbols", "pb_bars")
LOAD_BATCH = 500


class Kds:
    name = "kds"

    def __init__(self, port, pid):
        self.conn = kwp.Connection("127.0.0.1", port, timeout=120.0, client_name="parity_bench")
        self.pid = pid

    def run(self, sql):
        return self.conn.query(sql)

    def cpu_ns(self):
        return sched_ns(self.pid)

    def create(self, table):
        return f"CREATE TABLE {table} ({', '.join(f'{c} {k}' for c, k, _ in SCHEMA[table])}) BTREE"


class Pg:
    name = "pg"

    def __init__(self, port, database):
        self.conn = pg_wire.PgConnection(port=port, database=database, timeout=120.0,
                                         application_name="parity_bench")
        must(self, "SET max_parallel_workers_per_gather = 0")

    def run(self, sql):
        return self.conn.query(sql)

    def cpu_ns(self):
        return backend_ns(self.conn.backend_pid)

    def create(self, table):
        return f"CREATE TABLE {table} ({', '.join(f'{c} {p}' for c, _, p in SCHEMA[table])}, PRIMARY KEY (id))"


def must(engine, sql):
    tag, rows, _bytes, error = engine.run(sql)
    if error is not None:
        sys.exit(f"{engine.name}: {sql[:120]} -> {error}")
    return tag, rows


def load(engine, args):
    """The same rows into both engines, from one seed."""
    for table in reversed(TABLES):
        engine.run(f"DROP TABLE {table}")  # absent on a fresh run; the error is the expected answer
    for table in TABLES:
        must(engine, engine.create(table))
    rng = random.Random(args.seed)
    rows = {
        "pb_exchanges": [f"({i}, 'X{i}', 'C{i}')" for i in range(1, 3)],
        "pb_symbols": [f"({i}, 'SYM{i}', {1 + i % 2})" for i in range(1, args.symbols + 1)],
        "pb_bars": [],
    }
    for symbol in range(1, args.symbols + 1):
        close = 10_000
        for session in range(args.sessions):
            close = max(100, close + rng.randint(-200, 200))
            low, high = close - rng.randint(0, 100), close + rng.randint(0, 100)
            rows["pb_bars"].append(
                f"({len(rows['pb_bars']) + 1}, {symbol}, {session}, {close + rng.randint(-50, 50)}, "
                f"{high}, {low}, {close}, {rng.randint(1_000, 50_000_000)})")
    for table in TABLES:
        columns = ", ".join(c for c, _, _ in SCHEMA[table])
        for at in range(0, len(rows[table]), LOAD_BATCH):
            must(engine, f"INSERT INTO {table} ({columns}) VALUES " + ", ".join(rows[table][at:at + LOAD_BATCH]))
    if engine.name == "pg":
        for table in TABLES:
            must(engine, f"VACUUM (FREEZE, ANALYZE) {table}")
    _tag, n = must(engine, "SELECT * FROM pb_bars")
    if n != args.bars:
        sys.exit(f"{engine.name}: loaded {n} bars, expected {args.bars}")


def cell(engine, shape, args, rep):
    ops, make = SHAPES[shape]
    ops = max(1, int(ops * args.scale))
    rng = random.Random(f"{args.seed}-{shape}-{rep}")
    warm = random.Random(f"{args.seed}-{shape}-{rep}-warm")
    for _ in range(max(1, ops // 10)):
        sql = make(warm, args)
        if engine.run(sql)[3] is not None:
            sys.exit(f"{engine.name} {shape} warm-up: {sql[:120]}")
    statements = [make(rng, args) for _ in range(ops)]
    latencies, counts = [], []
    cpu0, client0, wall0 = engine.cpu_ns(), time.process_time(), time.perf_counter()
    for sql in statements:
        t0 = time.perf_counter()
        _tag, n, _bytes, error = engine.run(sql)
        latencies.append(time.perf_counter() - t0)
        if error is not None:
            sys.exit(f"{engine.name} {shape}: {error}")
        counts.append(n)
    wall = time.perf_counter() - wall0
    client_us = (time.process_time() - client0) * 1e6 / ops
    server_us = (engine.cpu_ns() - cpu0) / 1e3 / ops
    latencies.sort()

    def at(q):
        return latencies[min(len(latencies) - 1, int(len(latencies) * q))] * 1e6

    return {
        "engine": engine.name, "shape": shape, "rep": rep, "ops": ops, "counts": counts,
        "rows_per_op": sum(counts) / ops, "qps": ops / wall, "wall_s": wall,
        "mean_us": statistics.fmean(latencies) * 1e6,
        "p0_us": at(0), "p25_us": at(0.25), "p50_us": at(0.5), "p95_us": at(0.95), "p99_us": at(0.99),
        "max_us": latencies[-1] * 1e6,
        "server_cpu_us": server_us, "client_cpu_us": client_us, "host": host_state(),
    }


def status(args, line):
    if args.status:
        tmp = args.status + ".tmp"
        with open(tmp, "w") as f:
            f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {line}\n")
        os.replace(tmp, args.status)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--kds-port", type=int, required=True)
    p.add_argument("--kds-pid", type=int, required=True)
    p.add_argument("--pg-port", type=int, required=True)
    p.add_argument("--pg-db", default="bench")
    p.add_argument("--symbols", type=int, default=8)
    p.add_argument("--sessions", type=int, default=7560)
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--scale", type=float, default=1.0, help="multiplies every shape's ops")
    p.add_argument("--shapes", default=",".join(SHAPES))
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--json", required=True)
    p.add_argument("--status")
    args = p.parse_args()
    args.bars = args.symbols * args.sessions
    shapes = args.shapes.split(",")

    kds, pg = Kds(args.kds_port, args.kds_pid), Pg(args.pg_port, args.pg_db)
    for engine in (kds, pg):
        status(args, f"load {engine.name}")
        load(engine, args)
    status(args, "idle burn")
    idle0 = kds.cpu_ns()
    time.sleep(1.0)
    idle_us_per_s = (kds.cpu_ns() - idle0) / 1e3
    cells = []
    for rep in range(1, args.reps + 1):
        for shape in shapes:
            pair = []
            for engine in ((kds, pg) if rep % 2 else (pg, kds)):
                status(args, f"rep {rep} {shape} {engine.name}")
                pair.append(cell(engine, shape, args, rep))
            if pair[0]["counts"] != pair[1]["counts"]:
                sys.exit(f"rep {rep} {shape}: the engines returned different row counts")
            for c in pair:
                # KDS's figure less its idle tick over the cell's wall time.
                idle = idle_us_per_s * c["wall_s"] / c["ops"] if c["engine"] == "kds" else 0.0
                c["server_cpu_net_us"] = c["server_cpu_us"] - idle
            cells.extend(pair)
        with open(args.json, "w") as f:
            json.dump({"args": vars(args), "idle_cpu_us_per_s": idle_us_per_s, "cells": cells}, f, indent=1)
    for engine in (kds, pg):
        for table in reversed(TABLES):
            engine.run(f"DROP TABLE {table}")
    status(args, "DONE")

    print(f"KDS idle server CPU: {idle_us_per_s:,.0f} µs per second\n")
    print("| shape | rows/op | KDS p50 | PG p50 | KDS/PG qps | KDS server CPU (net of idle) "
          "| PG backend CPU | KDS/PG server CPU | KDS client CPU | PG client CPU |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for shape in shapes:
        med = {}
        for name in ("kds", "pg"):
            mine = [c for c in cells if c["shape"] == shape and c["engine"] == name]
            med[name] = {k: statistics.median(c[k] for c in mine)
                         for k in ("p50_us", "qps", "server_cpu_us", "server_cpu_net_us",
                                   "client_cpu_us", "rows_per_op")}
        k, g = med["kds"], med["pg"]
        print(f"| {shape} | {k['rows_per_op']:,.0f} rows | {k['p50_us']:,.1f} µs | {g['p50_us']:,.1f} µs | "
              f"{k['qps'] / g['qps']:.2f}x | {k['server_cpu_us']:,.1f} µs ({k['server_cpu_net_us']:,.1f} µs) | "
              f"{g['server_cpu_us']:,.1f} µs | {k['server_cpu_net_us'] / g['server_cpu_us']:.2f}x | "
              f"{k['client_cpu_us']:,.1f} µs | {g['client_cpu_us']:,.1f} µs |")


if __name__ == "__main__":
    main()
