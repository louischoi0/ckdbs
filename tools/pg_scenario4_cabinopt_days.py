#!/usr/bin/env python3
"""PostgreSQL twin of scenario4_cabinopt_days.py: one business day, unpaced.

Same five relations, same rows (imported from the ckdbs driver, same seed),
same day-1 statement shapes - the open insert burst, the skewed hot-symbol
probes on the board and the three tapes, the pk-lookup control, the close
aggregates - against the scratch cluster `tools/pg_setup.sh` runs on port
15433 at PostgreSQL defaults.

What the twin deliberately does not have is the scenario's subject: at
defaults nothing in PostgreSQL observes the hot predicate, builds a serving
structure for it, or retires one when the hot set rotates - the probe
seq-scans on day 1 exactly as it would on day 300. One day is therefore
enough: day 2 would measure the same plan against the same pages, so the
rotation axis has nothing to act on. `EXPLAIN (ANALYZE, BUFFERS)` for the
board and tape probes is captured as evidence of that plan. The comparison
this feeds is per-statement-shape latency, not TPS: the ckdbs sessions are
wall-paced and this twin is not.

    ./tools/pg_setup.sh init          # once
    python3 tools/pg_scenario4_cabinopt_days.py --json pg.json
"""

import argparse
import datetime
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase, report
from scenario4_cabinopt_days import (BOARD_ROWS, MATCHES, TAPE_SIZES,
                                     TAPE_TAGS, ZERO_ROW_SYMBOL,
                                     build_day_plans, make_board_rows,
                                     make_tape_rows, pk_sql, probe_sql)
from pg_wire import DEFAULT_HOST, PgConnection

DEFAULT_PORT = 15433


def abort(message, detail=None):
    print(f"pg_scenario4_cabinopt_days aborted: {message}", file=sys.stderr)
    if detail:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


class Client:
    def __init__(self, conn):
        self._conn = conn
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = self._conn.send_command(command)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def timed(self, command, phase):
        t0 = time.perf_counter()
        reply = self(command)
        dt = time.perf_counter() - t0
        phase.record(dt, reply)
        phase.elapsed += dt
        return reply

    def fetch(self, sql):
        rows, error = self._conn.fetch(sql)
        if error:
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{sql}  ->  {error}"
        return rows


def pg_insert_sql(table, row):
    """The ckdbs driver's INSERT omits the system-generated pk (invariant
    11); bigserial needs the column list spelled so VALUES do not shift
    into `id`."""
    symbol, qty, price = row
    return (f"INSERT INTO {table} (symbol, qty, price) "
            f"VALUES ('{symbol}', {qty}, {price})")


def explain(client, sql):
    return [r[0].decode() if isinstance(r[0], bytes) else r[0]
            for r in client.fetch(f"EXPLAIN (ANALYZE, BUFFERS) {sql}")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--database", default="bench")
    ap.add_argument("--suffix", default="a")
    ap.add_argument("--seed", type=int, default=20260810)
    ap.add_argument("--blocks", type=int, default=12)
    ap.add_argument("--open-inserts", type=int, default=240)
    ap.add_argument("--board-probes", type=int, default=2400)
    ap.add_argument("--tape-probes", type=int, default=396)
    ap.add_argument("--pk-ops", type=int, default=240)
    ap.add_argument("--close-rounds", type=int, default=3)
    ap.add_argument("--days", type=int, default=3,
                    help="plan days generated (only day 1 is run); kept a "
                    "flag so the plan RNG stream matches the ckdbs driver's")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    try:
        conn = PgConnection(host=args.host, port=args.port,
                            database=args.database,
                            application_name="pg_scenario4_cabinopt_days.py")
    except OSError as e:
        abort(f"could not connect to {args.host}:{args.port}/{args.database}",
              f"{e}\n  start it with: ./tools/pg_setup.sh init")
    client = Client(conn)
    version = client.fetch("SELECT version()")

    t0 = time.time()
    plan = build_day_plans(args, random.Random(args.seed + 1))[0]  # day 1
    board_tbl = f"board_{plan['board']}_{args.suffix}"
    names = {"board_a": f"board_a_{args.suffix}",
             "board_b": f"board_b_{args.suffix}"}
    for size in TAPE_SIZES:
        names[f"tape_{TAPE_TAGS[size]}"] = f"tape_{TAPE_TAGS[size]}_{args.suffix}"

    # Same content as every ckdbs arm: one generator, same order. bigserial
    # stands in for the Keystone pk; no secondary index ever, matching a
    # ckdbs relation with no Cabin and no index.
    rng = random.Random(args.seed)
    load = Phase("load[pg]", "setup, txn batches of 500")

    def batch_insert(table, rows):
        client("BEGIN")
        pending = 0
        for row in rows:
            client.timed(pg_insert_sql(table, row), load)
            pending += 1
            if pending >= 500:
                client("COMMIT")
                client("BEGIN")
                pending = 0
        client("COMMIT")

    for key in ("board_a", "board_b"):
        client(f"DROP TABLE IF EXISTS {names[key]}")
        client(f"CREATE TABLE {names[key]} (id bigserial PRIMARY KEY, "
               f"symbol text, qty bigint, price bigint)")
    for size in TAPE_SIZES:
        key = f"tape_{TAPE_TAGS[size]}"
        client(f"DROP TABLE IF EXISTS {names[key]}")
        client(f"CREATE TABLE {names[key]} (id bigserial PRIMARY KEY, "
               f"symbol text, qty bigint, price bigint)")
    if client.errors:
        abort("setup failed", client.first_error)
    batch_insert(names["board_a"], make_board_rows("a", BOARD_ROWS, rng))
    batch_insert(names["board_b"], make_board_rows("b", BOARD_ROWS, rng))
    for size in TAPE_SIZES:
        batch_insert(names[f"tape_{TAPE_TAGS[size]}"],
                     make_tape_rows(size, rng))

    # ---- day 1, unpaced: the same statements the ckdbs arms ran ----------
    open_phase = Phase("d1-open[pg]", f"{args.open_inserts} inserts")
    for row in plan["open_rows"]:
        client.timed(pg_insert_sql(board_tbl, row), open_phase)

    board = Phase("d1-board[pg]", f"hot equality on {board_tbl}")
    tapes = {size: Phase(f"d1-tape{TAPE_TAGS[size]}[pg]", f"{size} rows")
             for size in TAPE_SIZES}
    pk = Phase("d1-pk[pg]", "control")
    for block in plan["blocks"]:
        for sym in block["board"]:
            client.timed(probe_sql(board_tbl, sym), board)
        for size in TAPE_SIZES:
            tbl = names[f"tape_{TAPE_TAGS[size]}"]
            for sym in block["tape"][size]:
                client.timed(probe_sql(tbl, sym), tapes[size])
        for key in block["pk"]:
            client.timed(pk_sql(board_tbl, key), pk)

    close = Phase("d1-close[pg]", "COUNT/SUM/GROUP BY full scans")
    close_set = [f"SELECT COUNT(*) FROM {board_tbl}",
                 f"SELECT SUM(qty) FROM {board_tbl}",
                 f"SELECT symbol, COUNT(*) FROM {board_tbl} GROUP BY symbol",
                 f"SELECT COUNT(*) FROM {names['tape_10k']}"]
    for _ in range(args.close_rounds):
        for sql in close_set:
            client.timed(sql, close)

    plans_evidence = {
        "board": explain(client, probe_sql(board_tbl, plan["hot_syms"][0])),
        "tape_10k": explain(client, probe_sql(names["tape_10k"],
                                              plan["hot_tape"][0])),
        "zero_row": explain(client, probe_sql(board_tbl, ZERO_ROW_SYMBOL)),
    }

    phases = [load, open_phase, board] + [tapes[s] for s in TAPE_SIZES] \
        + [pk, close]
    meta = {
        "engine": "postgresql",
        "driver": "pg_scenario4_cabinopt_days.py",
        "host": args.host, "port": args.port, "database": args.database,
        "columns": "id bigserial pk, symbol text, qty bigint, price bigint",
        "rows": f"boards 2x{BOARD_ROWS}, tapes {'/'.join(map(str, TAPE_SIZES))}",
        "table": f"board_*/tape_*_{args.suffix}",
        "seed": args.seed, "suffix": args.suffix,
        "day_run": 1, "hot_board": board_tbl,
        "version": (version[0][0].decode() if version and
                    isinstance(version[0][0], bytes) else str(version)),
        "started_utc": datetime.datetime.utcfromtimestamp(t0)
        .strftime("%Y-%m-%d %H:%M:%S"),
        "elapsed_s": round(time.time() - t0, 1),
        "client_errors": client.errors,
        "first_error": client.first_error,
    }
    report(phases, meta,
           footer=("PostgreSQL at defaults: no structure is ever created "
                   "for the hot predicate, so one day stands for every day.",))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "explain": plans_evidence,
                       "phases": [p.summary() for p in phases]}, f, indent=2)
        print(f"json -> {args.json}")
    conn.close()
    sys.exit(1 if client.errors else 0)


if __name__ == "__main__":
    main()
