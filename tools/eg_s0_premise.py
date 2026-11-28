#!/usr/bin/env python3
"""EG-S0: what one client's autocommit insert costs beside the log's live run.

The premise (`instructions/v3.2.0/workorder-eg-one-client-durable-write.md`):
scenario 1's `result-insert` is behind PostgreSQL at 60,480 bars and not at
the three smaller sizes, and its statement follows a bulk load. The engine
syncs every live WAL segment at each durable point (`known-gaps.md`, "Every
sync covers every live segment"), and a segment stays live until a checkpoint
moves the redo start past it. So the same insert is timed here in three
states of one server:

  idle        a fresh instance, one live segment
  after-load  at once after a bulk load that rolled the log (two or more live)
  recycled    after the checkpoints have removed the segments the load left

and, for the same rows and statements, on PostgreSQL: idle, at once after the
load, and `--settle` seconds later. Every timed insert goes through a client
that counts (`kwp.Connection.query`, `pg_wire`'s `query`).

Beside each KDS cell: the server's CPU (every thread's `schedstat`), and from
`sys.meta` the syncs, the bytes appended, the page images and the live
segments before and after; from `sys.wait_stats`, what `commit.wal_flush`
booked.

Usage:
    eg_s0_premise.py --engine kds --port 15791 --pid 1234 --json out.json
    eg_s0_premise.py --engine pg --port 15434 --database bench --json out.json
"""

import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ckdbs_cli  # noqa: E402
import kwp  # noqa: E402
import pg_wire  # noqa: E402
from bn_census import sched_ns  # noqa: E402

TABLE = "eg_s0_t"
WAL_FIELDS = ("wal_syncs", "wal_append_lsn", "wal_rolls", "wal_segments_removed", "wal_group_commits",
              "wal_group_batches", "contention_syncs_inline", "contention_syncs_writer", "pool_fetch_images",
              "pool_fetch_image_bytes")


def row(i):
    return f"({i}, {i * 7}, {i % 97}, {i * 13}, 'r{i:012d}')"


class Kds:
    def __init__(self, args):
        self.conn = kwp.Connection(args.host, args.port, timeout=300.0, client_name="eg_s0")
        self.cli = ckdbs_cli.ServerConnection(args.host, args.port, timeout=300.0)
        self.pid = args.pid
        self.create = f"CREATE TABLE {TABLE} (id int64, a int64, b int64, c int64, d varchar) BTREE"

    def run(self, sql):
        return self.conn.query(sql)

    def meta(self):
        rows, _ = self.cli.rows("SELECT * FROM sys.meta")
        fields = {r[1]: r[2] for r in rows or []}
        out = {k: int(fields[k]) for k in WAL_FIELDS if k in fields and fields[k].lstrip("-").isdigit()}
        out["live_segments"] = 1 + out.get("wal_rolls", 0) - out.get("wal_segments_removed", 0)
        waits, _ = self.cli.rows("SELECT * FROM sys.wait_stats")
        for r in waits or []:
            if r[0] == "NULL" and r[1] == "commit.wal_flush":
                out["commit_wal_flush_count"], out["commit_wal_flush_us"] = int(r[3]), int(r[4])
        out["server_cpu_ns"] = sched_ns(self.pid)
        return out


class Pg:
    def __init__(self, args):
        self.conn = pg_wire.PgConnection(host=args.host, port=args.port, database=args.database,
                                         timeout=300.0, application_name="eg_s0")
        self.create = f"CREATE TABLE {TABLE} (id bigint PRIMARY KEY, a bigint, b bigint, c bigint, d text)"

    def run(self, sql):
        return self.conn.query(sql)

    def meta(self):
        lsn = self.conn.scalar("SELECT pg_current_wal_lsn() - '0/0'::pg_lsn")
        return {"wal_append_lsn": int(lsn.decode().split(".")[0])}


def cell(engine, name, first_id, ops):
    """`ops` autocommit single-row inserts, each its own durable commit."""
    before = engine.meta()
    lat = []
    for i in range(first_id, first_id + ops):
        t0 = time.perf_counter()
        _tag, _rows, _bytes, error = engine.run(f"INSERT INTO {TABLE} VALUES {row(i)}")
        lat.append(time.perf_counter() - t0)
        if error is not None:
            sys.exit(f"{name}: insert {i}: {error}")
    after = engine.meta()
    lat.sort()

    def at(q):
        return lat[min(len(lat) - 1, int(len(lat) * q))] * 1e6

    out = {"cell": name, "ops": ops, "mean_us": statistics.fmean(lat) * 1e6, "p0_us": at(0), "p25_us": at(0.25),
           "p50_us": at(0.5), "p95_us": at(0.95), "p99_us": at(0.99), "max_us": lat[-1] * 1e6,
           "before": before, "after": after, "loadavg": open("/proc/loadavg").read().split()[0]}
    for k in after:
        if k in before and k != "live_segments":
            out["d_" + k] = after[k] - before[k]
    if "server_cpu_ns" in after:
        out["server_cpu_us_per_op"] = out["d_server_cpu_ns"] / 1e3 / ops
    out["wal_bytes_per_op"] = out.get("d_wal_append_lsn", 0) / ops
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--engine", choices=("kds", "pg"), required=True)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--pid", type=int, default=0, help="the KDS server's pid, for its CPU")
    p.add_argument("--database", default="bench")
    p.add_argument("--ops", type=int, default=500, help="timed autocommit inserts a cell")
    p.add_argument("--load-rows", type=int, default=0,
                   help="PostgreSQL: rows the bulk load writes; KDS loads until the log has rolled")
    p.add_argument("--load-batch", type=int, default=500)
    p.add_argument("--max-load-rows", type=int, default=4_000_000)
    p.add_argument("--settle", type=float, default=45.0,
                   help="KDS: the longest wait for the live run to return to one segment; PostgreSQL: the wait itself")
    p.add_argument("--json", required=True)
    args = p.parse_args()

    engine = Kds(args) if args.engine == "kds" else Pg(args)
    engine.run(f"DROP TABLE {TABLE}")           # absent on a fresh server; the error is the expected answer
    if engine.run(engine.create)[3] is not None:
        sys.exit("create failed")
    cells, next_id = [], 1

    cells.append(cell(engine, "idle", next_id, args.ops))
    next_id += args.ops

    # The bulk load: one autocommit multi-row INSERT a batch, so one transaction and one commit each, on both
    # engines alike, until the log has rolled (KDS) or the row count is met.
    load_start, loaded = time.perf_counter(), 0
    while True:
        if args.engine == "kds":
            if engine.meta()["live_segments"] >= 2 or loaded >= args.max_load_rows:
                break
        elif loaded >= args.load_rows:
            break
        for _ in range(20):                     # 20 batches between two looks at the log
            values = ", ".join(row(i) for i in range(next_id, next_id + args.load_batch))
            error = engine.run(f"INSERT INTO {TABLE} VALUES {values}")[3]
            if error is not None:
                sys.exit(f"load: {error}")
            next_id += args.load_batch
            loaded += args.load_batch
    load_s = time.perf_counter() - load_start

    cells.append(cell(engine, "after-load", next_id, args.ops))
    next_id += args.ops

    settle_start = time.perf_counter()
    if args.engine == "kds":
        while engine.meta()["live_segments"] > 1 and time.perf_counter() - settle_start < args.settle:
            time.sleep(0.25)
    else:
        time.sleep(args.settle)
    settled_s = time.perf_counter() - settle_start

    cells.append(cell(engine, "recycled", next_id, args.ops))
    with open(args.json, "w") as f:
        json.dump({"args": vars(args), "loaded_rows": loaded, "load_s": load_s, "settled_s": settled_s,
                   "cells": cells}, f, indent=1)
    for c in cells:
        live = f" live {c['before'].get('live_segments', '-')}->{c['after'].get('live_segments', '-')}"
        print(f"{c['cell']:11} mean {c['mean_us']:7.1f} us  p50 {c['p50_us']:7.1f} us  "
              f"wal {c['wal_bytes_per_op']:7.1f} B/op{live}  syncs {c.get('d_wal_syncs', '-')}")
    print(f"loaded {loaded:,} rows in {load_s:.1f} s; settled in {settled_s:.1f} s")


if __name__ == "__main__":
    main()
