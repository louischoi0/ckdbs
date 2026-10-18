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

  * KDS: the sum of `/proc/<pid>/task/*/schedstat`'s on-CPU time, every
    thread of the server (reactor, WAL writer, data-sync), so a cost a
    background thread pays for the statement is counted;
  * PostgreSQL: `/proc/<backend pid>/schedstat`, the session's backend only
    (its WAL writer and checkpointer are not counted; a read writes no WAL).

Shapes (`--shapes` picks a subset), all over `pb_bars` (8 symbols x 7,560
sessions = 60,480 rows by default), `pb_symbols` and `pb_exchanges`; no
secondary index on either engine, so a non-key filter is a scan on both:

  pk-lookup       tuple key equality, 1 row
  bar-range       tuple key BETWEEN, 201 rows
  symbol-history  symbol_id equality, a scan returning 7,560 rows
  day-slice       session_no equality, a scan returning 8 rows
  point-join      tuple key anchored 3-relation join, 1 row
  agg-global      COUNT/MIN/MAX/SUM over the whole relation, 1 row
  agg-by-session  GROUP BY session_no, 7,560 rows

Cells interleave: each rep runs every shape on both engines, the order of
the two engines alternating per rep (KDS first on odd reps). The first
`--warm` statements of every cell are run untimed.

Usage:
    parity_bench.py --kds-port 15791 --kds-pid 1234 --pg-port 15434 \\
        --pg-db bench --reps 5 --json out.json --status status.txt
"""

import argparse
import glob
import json
import random
import statistics
import sys
import time

import kwp
import pg_wire

SHAPES = {
    # name: (ops per cell, statement for a random key)
    "pk-lookup": (2000, lambda r, a: f"SELECT * FROM pb_bars WHERE id = {r.randint(1, a.bars)}"),
    "bar-range": (300, lambda r, a: (lambda k: f"SELECT * FROM pb_bars WHERE id BETWEEN {k} AND {k + 200}")(
        r.randint(1, a.bars - 200))),
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

KDS_SCHEMA = {
    "pb_exchanges": "id int64, code varchar, country varchar",
    "pb_symbols": "id int64, ticker varchar, exchange_id int64",
    "pb_bars": ("id int64, symbol_id int64, session_no int32, open int64, high int64, "
                "low int64, close int64, volume int64"),
}
PG_SCHEMA = {
    "pb_exchanges": "id bigint PRIMARY KEY, code text, country text",
    "pb_symbols": "id bigint PRIMARY KEY, ticker text, exchange_id bigint",
    "pb_bars": ("id bigint PRIMARY KEY, symbol_id bigint, session_no integer, open bigint, "
                "high bigint, low bigint, close bigint, volume bigint"),
}
COLUMNS = {name: ", ".join(c.split()[0] for c in cols.split(", ")) for name, cols in KDS_SCHEMA.items()}
LOAD_BATCH = 500


class Kds:
    name = "kds"

    def __init__(self, port, pid):
        self.conn = kwp.Connection("127.0.0.1", port, timeout=120.0, client_name="parity_bench")
        self.pid = pid

    def run(self, sql):
        return self.conn.query(sql)

    def cpu_ns(self):
        total = 0
        for path in glob.glob(f"/proc/{self.pid}/task/*/schedstat"):
            try:
                with open(path) as f:
                    total += int(f.read().split()[0])
            except OSError:
                pass  # a thread that exited between the glob and the read
        return total

    def create(self, table):
        return f"CREATE TABLE {table} ({KDS_SCHEMA[table]}) BTREE"


class Pg:
    name = "pg"

    def __init__(self, port, database):
        self.conn = pg_wire.PgConnection(port=port, database=database, timeout=120.0,
                                         application_name="parity_bench")

    def run(self, sql):
        return self.conn.query(sql)

    def cpu_ns(self):
        with open(f"/proc/{self.conn.backend_pid}/schedstat") as f:
            return int(f.read().split()[0])

    def create(self, table):
        return f"CREATE TABLE {table} ({PG_SCHEMA[table]})"


def must(engine, sql):
    tag, rows, _bytes, error = engine.run(sql)
    if error is not None:
        sys.exit(f"{engine.name}: {sql[:120]} -> {error}")
    return tag, rows


def load(engine, args):
    """The same rows into both engines, from one seed."""
    for table in ("pb_bars", "pb_symbols", "pb_exchanges"):
        engine.run(f"DROP TABLE {'IF EXISTS ' if engine.name == 'pg' else ''}{table}")
    for table in ("pb_exchanges", "pb_symbols", "pb_bars"):
        must(engine, engine.create(table))
    rng = random.Random(args.seed)
    exchanges = [f"({i}, 'X{i}', 'C{i}')" for i in range(1, 3)]
    symbols = [f"({i}, 'SYM{i}', {1 + i % 2})" for i in range(1, args.symbols + 1)]
    bars = []
    next_id = 1
    for symbol in range(1, args.symbols + 1):
        close = 10_000
        for session in range(args.sessions):
            close = max(100, close + rng.randint(-200, 200))
            low, high = close - rng.randint(0, 100), close + rng.randint(0, 100)
            bars.append(f"({next_id}, {symbol}, {session}, {close + rng.randint(-50, 50)}, "
                        f"{high}, {low}, {close}, {rng.randint(1_000, 50_000_000)})")
            next_id += 1
    for table, rows in (("pb_exchanges", exchanges), ("pb_symbols", symbols), ("pb_bars", bars)):
        for at in range(0, len(rows), LOAD_BATCH):
            must(engine, f"INSERT INTO {table} ({COLUMNS[table]}) VALUES "
                         + ", ".join(rows[at:at + LOAD_BATCH]))
    if engine.name == "pg":
        for table in ("pb_exchanges", "pb_symbols", "pb_bars"):
            must(engine, f"ANALYZE {table}")
    _tag, n = must(engine, "SELECT * FROM pb_bars WHERE id = 1")
    if n != 1:
        sys.exit(f"{engine.name}: load check read {n} rows")


def cell(engine, shape, args, rep):
    ops, make = SHAPES[shape]
    ops = max(1, int(ops * args.scale))
    rng = random.Random(f"{args.seed}-{shape}-{rep}")
    statements = [make(rng, args) for _ in range(ops)]
    for sql in statements[:max(1, ops // 10)]:
        engine.run(sql)
    latencies, rows = [], 0
    cpu0, client0, wall0 = engine.cpu_ns(), time.process_time(), time.perf_counter()
    for sql in statements:
        t0 = time.perf_counter()
        _tag, n, _bytes, error = engine.run(sql)
        latencies.append(time.perf_counter() - t0)
        if error is not None:
            sys.exit(f"{engine.name} {shape}: {error}")
        rows += n
    wall = time.perf_counter() - wall0
    server_us = (engine.cpu_ns() - cpu0) / 1e3 / ops
    client_us = (time.process_time() - client0) * 1e6 / ops
    latencies.sort()
    return {
        "engine": engine.name, "shape": shape, "rep": rep, "ops": ops,
        "rows_per_op": rows / ops, "qps": ops / wall,
        "mean_us": statistics.fmean(latencies) * 1e6,
        "p0_us": latencies[0] * 1e6, "p25_us": latencies[len(latencies) // 4] * 1e6,
        "p50_us": latencies[len(latencies) // 2] * 1e6,
        "p95_us": latencies[min(len(latencies) - 1, int(len(latencies) * 0.95))] * 1e6,
        "max_us": latencies[-1] * 1e6,
        "server_cpu_us": server_us, "client_cpu_us": client_us,
        "loadavg": open("/proc/loadavg").read().split()[0],
    }


def status(args, line):
    if args.status:
        with open(args.status, "w") as f:
            f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {line}\n")


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

    engines = [Kds(args.kds_port, args.kds_pid), Pg(args.pg_port, args.pg_db)]
    for engine in engines:
        status(args, f"load {engine.name}")
        load(engine, args)
    cells = []
    for rep in range(1, args.reps + 1):
        order = engines if rep % 2 else engines[::-1]
        for shape in shapes:
            for engine in order:
                status(args, f"rep {rep} {shape} {engine.name}")
                cells.append(cell(engine, shape, args, rep))
        with open(args.json, "w") as f:
            json.dump({"args": vars(args), "cells": cells}, f, indent=1)
    for engine in engines:
        for table in ("pb_bars", "pb_symbols", "pb_exchanges"):
            engine.run(f"DROP TABLE {table}")
    status(args, "DONE")

    print(f"| shape | rows/op | KDS p50 | PG p50 | KDS/PG qps | KDS server CPU | PG backend CPU | KDS client CPU | PG client CPU |")
    print("|---|---|---|---|---|---|---|---|---|")
    for shape in shapes:
        med = {}
        for name in ("kds", "pg"):
            mine = [c for c in cells if c["shape"] == shape and c["engine"] == name]
            med[name] = {k: statistics.median(c[k] for c in mine)
                         for k in ("p50_us", "qps", "server_cpu_us", "client_cpu_us", "rows_per_op")}
        k, g = med["kds"], med["pg"]
        print(f"| {shape} | {k['rows_per_op']:,.0f} rows | {k['p50_us']:,.1f} µs | {g['p50_us']:,.1f} µs | "
              f"{k['qps'] / g['qps']:.2f}x | {k['server_cpu_us']:,.1f} µs | {g['server_cpu_us']:,.1f} µs | "
              f"{k['client_cpu_us']:,.1f} µs | {g['client_cpu_us']:,.1f} µs |")


if __name__ == "__main__":
    main()
