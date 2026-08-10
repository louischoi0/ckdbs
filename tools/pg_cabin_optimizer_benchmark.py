#!/usr/bin/env python3
"""PostgreSQL twin of cabin_optimizer_benchmark.py's improve case.

Same relations, same rows, same hot non-pk equality, driven over the v3 wire
protocol against the scratch cluster `tools/pg_setup.sh` runs on port 15433
at PostgreSQL defaults. The schema, row generator and probe are imported
from the ckdbs driver so the two cannot drift into measuring different
questions.

What this twin deliberately does NOT have is the interesting part: at
defaults PostgreSQL has no counterpart to the cabin optimizer - nothing
observes the hot predicate and declares a serving structure for it, so the
probe seq-scans on the first execution and on the five-thousandth. That flat
line is the baseline the self-created Cabin's before/after is read against.
The twin therefore runs one phase per size (`probe-<tag>[seqscan]`, the same
fixed op count as each ckdbs phase) plus the pk-lookup control, and captures
`EXPLAIN (ANALYZE, BUFFERS)` once per size as evidence of the plan.
"""

import argparse
import datetime
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase, report
from cabin_optimizer_benchmark import (COLUMNS, HOT_VAL, MATCHES, SIZES,
                                       SIZE_TAGS, make_rows, probe_sql,
                                       pk_sql)
from pg_wire import DEFAULT_HOST, PgConnection

DEFAULT_PORT = 15433


def abort(message, detail=None):
    print(f"pg_cabin_optimizer_benchmark aborted: {message}", file=sys.stderr)
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
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def fetch(self, sql):
        rows, error = self._conn.fetch(sql)
        if error:
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{sql}  ->  {error}"
        return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--database", default="bench")
    ap.add_argument("--suffix", default="a")
    ap.add_argument("--seed", type=int, default=20260810)
    ap.add_argument("--probe-ops", type=int, default=1200)
    ap.add_argument("--pk-ops", type=int, default=300)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    import random
    rng = random.Random(args.seed)

    try:
        conn = PgConnection(host=args.host, port=args.port,
                            database=args.database,
                            application_name="pg_cabin_optimizer_benchmark.py")
    except OSError as e:
        abort(f"could not connect to {args.host}:{args.port}/{args.database}",
              f"{e}\n  start it with: ./tools/pg_setup.sh init")
    client = Client(conn)
    version = client.fetch("SELECT version()")

    t0 = time.time()
    names = {SIZE_TAGS[s]: f"coh_{SIZE_TAGS[s]}_{args.suffix}" for s in SIZES}
    load = Phase("load", "setup, txn batches")
    for size in SIZES:
        name = names[SIZE_TAGS[size]]
        client(f"DROP TABLE IF EXISTS {name}")
        reply = client(f"CREATE TABLE {name} "
                       f"(id bigserial PRIMARY KEY, val bigint, pad text)")
        if reply.startswith("ERR"):
            abort(f"CREATE TABLE {name} failed", reply)
        client("BEGIN")
        pending = 0
        for val, pad in make_rows(size):
            client.timed(
                f"INSERT INTO {name} (val, pad) VALUES ({val}, '{pad}')", load)
            pending += 1
            if pending >= 500:
                client("COMMIT")
                client("BEGIN")
                pending = 0
        client("COMMIT")

    phases = [load]
    plans = {}
    for size in SIZES:
        tag = SIZE_TAGS[size]
        name = names[tag]
        pk = Phase(f"pk-{tag}[control]", f"{size} rows")
        for _ in range(args.pk_ops):
            client.timed(pk_sql(name, rng.randrange(1, size + 1)), pk)
        probe = Phase(f"probe-{tag}[seqscan]", f"{size} rows, no index ever")
        sql = probe_sql(name)
        for _ in range(args.probe_ops):
            client.timed(sql, probe)
        plans[tag] = [r[0].decode() if isinstance(r[0], bytes) else r[0]
                      for r in
                      client.fetch(f"EXPLAIN (ANALYZE, BUFFERS) {sql}")]
        phases += [pk, probe]

    meta = {
        "engine": "postgresql",
        "driver": "pg_cabin_optimizer_benchmark.py",
        "host": args.host, "port": args.port,
        "columns": COLUMNS,
        "rows": "/".join(str(s) for s in SIZES),
        "table": f"coh*_{args.suffix}",
        "seed": args.seed,
        "version": (version[0][0].decode()
                    if version and isinstance(version[0][0], bytes)
                    else (version[0][0] if version else None)),
        "started_utc": datetime.datetime.utcfromtimestamp(t0)
        .strftime("%Y-%m-%d %H:%M:%S"),
        "elapsed_s": round(time.time() - t0, 1),
        "client_errors": client.errors,
        "first_error": client.first_error,
    }
    report(phases, meta,
           footer=("PostgreSQL defaults on purpose - a hand-tuned baseline "
                   "is not a baseline (bench/docs/README.md).",))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "plans": plans,
                       "phases": [p.summary() for p in phases]}, f, indent=2)
        print(f"json -> {args.json}")
    sys.exit(1 if client.errors else 0)


if __name__ == "__main__":
    main()
