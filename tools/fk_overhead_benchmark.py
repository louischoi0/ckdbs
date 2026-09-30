#!/usr/bin/env python3
"""AY-S11: what D9(a)'s parent fence costs a child write, and how the borrow
ledger's linear `Holds` scan grows with a transaction's borrows.

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8 (the milestone-close A/B).
`lock_contention_benchmark.py` cannot express these cells - it has no foreign
key and no multi-row transaction - so this is a second driver, shaped like it:
serial and interleaved, one session per server, with `--ab-port` naming the
other engine.

Cell 2 (`--cell price`), per-statement, one session per server:

  child-insert-fk      autocommit INSERT into a child whose fk references an
                       existing parent row: one row, one transaction. The
                       write that D9(a) makes take IS on the parent relation
                       and S on the parent row before its descent, held to
                       the decide.
  child-insert-fk-txn  the same in BEGIN / INSERT / COMMIT; the whole unit,
                       and the INSERT alone as `child-insert-fk-txn:stmt`.
  child-insert-plain   control: an identically shaped child with no fk
                       declared - what the write costs with no check at all.
  parent-update        control: UPDATE of a parent row's non-key column, no
                       child writer open.
  child-update         control: UPDATE of a child row's non-fk column.
  child-update-again   the noise floor: the arm above, repeated.
  select               control: pk point read of a child row.
  ping                 control: SHOW META.

Cell 3 (`--cell ledger`): one explicit transaction inserting K child rows,
each referencing a distinct parent (and the same K rows into the fk-less
child as control), K from `--ks`. Reports per-row cost against K, and for the
largest K the per-statement mean by position in the transaction (a quadratic
term shows as a slope inside one transaction).

Arms alternate which server goes first per block/rep, so a drifting host does
not favour one side. Each server's `SHOW META` is read before and after every
arm: the engine's own time per statement is the delta of
`sched_foreground_polled_us` over `sched_foreground_polls`.

Usage:
    tools/fk_overhead_benchmark.py --cell price --port 15600 --label B \\
        --ab-port 15601 --ab-label A --ops 2000 --blocks 4 --json out.json
    tools/fk_overhead_benchmark.py --cell ledger --port 15600 --label B \\
        --ab-port 15601 --ab-label A --ks 1,64,1024,4096 --json out.json

Both servers must already be running on their own fresh data files and their
own chosen ports (`bench/README.md` rule 5).
"""

import argparse
import json
import subprocess
import sys
import time

from bench_common import Phase
from ckdbs_cli import DEFAULT_HOST, ServerConnection

PARENT, CHILD, PLAIN = "fk_p", "fk_c", "fk_n"
KEYS = ("sched_foreground_polls", "sched_foreground_polled_us")


def host_state():
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(
            ["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
            capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "unavailable"
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


def meta(conn):
    text = conn.send_command("SHOW META").replace("\\n", " ")
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            if k in KEYS:
                out[k] = float(v)
    return out


def must(conn, sql, prefix):
    reply = conn.send_command(sql)
    if not reply.startswith(prefix):
        raise SystemExit(f"setup: {sql!r} -> {reply!r}")
    return reply


def setup(conn, parents, children):
    for t in (CHILD, PLAIN, PARENT):
        conn.send_command(f"DROP TABLE {t}")
    must(conn, f"CREATE TABLE {PARENT} (id int64, v int64) BTREE", "CREATED")
    must(conn, f"CREATE TABLE {CHILD} (id int64, pid int64 REFERENCES {PARENT}, v int64) BTREE",
         "CREATED")
    must(conn, f"CREATE TABLE {PLAIN} (id int64, pid int64, v int64) BTREE", "CREATED")
    for i in range(1, parents + 1):
        must(conn, f"INSERT INTO {PARENT} VALUES ({i}, 0)", "INSERTED")
    # Rows the update / select controls address. pk named so the ids are known.
    for i in range(1, children + 1):
        must(conn, f"INSERT INTO {CHILD} VALUES ({i}, {1 + (i % parents)}, 0)", "INSERTED")


class Server:
    def __init__(self, label, host, port):
        self.label = label
        self.conn = ServerConnection(host, port)


def timed(conn, sql, phase):
    t0 = time.perf_counter()
    reply = conn.send_command(sql)
    phase.record(time.perf_counter() - t0, reply)
    return reply


def cell_price(args, servers):
    parents, children = args.parents, args.children
    for s in servers:
        setup(s.conn, parents, children)
    per_block = max(1, args.ops // args.blocks)
    # Named pks for inserts start above the setup rows so they cannot collide
    # and are identical across servers.
    counter = {"n": 10_000_000}

    def next_id():
        counter["n"] += 1
        return counter["n"]

    def child_insert_fk(conn, k, ph, aux):
        pid = 1 + (k % parents)
        timed(conn, f"INSERT INTO {CHILD} VALUES ({next_id()}, {pid}, 0)", ph)

    def child_insert_fk_txn(conn, k, ph, aux):
        pid = 1 + (k % parents)
        t0 = time.perf_counter()
        r1 = conn.send_command("BEGIN")
        timed(conn, f"INSERT INTO {CHILD} VALUES ({next_id()}, {pid}, 0)", aux)
        r3 = conn.send_command("COMMIT")
        ph.record(time.perf_counter() - t0, r1 if r1.startswith("ERR") else r3)

    def child_insert_plain(conn, k, ph, aux):
        pid = 1 + (k % parents)
        timed(conn, f"INSERT INTO {PLAIN} VALUES ({next_id()}, {pid}, 0)", ph)

    def parent_update(conn, k, ph, aux):
        timed(conn, f"UPDATE {PARENT} SET v = {k} WHERE id = {1 + (k % parents)}", ph)

    def child_update(conn, k, ph, aux):
        timed(conn, f"UPDATE {CHILD} SET v = {k} WHERE id = {1 + (k % children)}", ph)

    def select(conn, k, ph, aux):
        timed(conn, f"SELECT * FROM {CHILD} WHERE id = {1 + (k % children)}", ph)

    def ping(conn, k, ph, aux):
        timed(conn, "SHOW META", ph)

    arm_list = [
        ("child-insert-fk", child_insert_fk),
        ("child-insert-fk-txn", child_insert_fk_txn),
        ("child-insert-plain", child_insert_plain),
        ("parent-update", parent_update),
        ("child-update", child_update),
        ("child-update-again", child_update),
        ("select", select),
        ("ping", ping),
    ]
    results = {s.label: {} for s in servers}
    engine = {s.label: {} for s in servers}
    blocks = {s.label: {n: [] for n, _ in arm_list} for s in servers}
    for name, fn in arm_list:
        for s in servers:
            results[s.label][name] = Phase(name)
            if name == "child-insert-fk-txn":
                results[s.label][name + ":stmt"] = Phase(name + ":stmt")
            engine[s.label][name] = {"polls": 0.0, "us": 0.0}
        for block in range(args.blocks):
            order = servers if block % 2 == 0 else list(reversed(servers))
            for s in order:
                ph = results[s.label][name]
                aux = results[s.label].get(name + ":stmt")
                n0 = len(ph.latencies)
                m0 = meta(s.conn)
                t0 = time.perf_counter()
                for k in range(per_block):
                    fn(s.conn, block * per_block + k, ph, aux)
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({
                    "block": block, "p50_us": lat[len(lat) // 2] * 1e6,
                    "mean_us": sum(lat) / len(lat) * 1e6,
                    "engine_us_per_op": (m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]) / len(lat)})
    return {"arms": {l: {n: p.summary() for n, p in r.items()} for l, r in results.items()},
            "engine": engine, "blocks": blocks, "ops_per_arm": per_block * args.blocks}


def cell_ledger(args, servers):
    ks = [int(x) for x in args.ks.split(",")]
    parents = max(ks)
    for s in servers:
        setup(s.conn, parents, 1)
    counter = {"n": 20_000_000}
    out = {s.label: {} for s in servers}
    for k in ks:
        reps = max(5, args.rows_per_k // k)
        for kind, table in (("fk", CHILD), ("plain", PLAIN)):
            for s in servers:
                out[s.label][f"{kind}-K{k}"] = {"reps": reps, "txn_us": [], "stmt_us_by_pos": None,
                                               "engine_us": 0.0, "engine_polls": 0.0, "errors": 0}
            for rep in range(reps):
                order = servers if rep % 2 == 0 else list(reversed(servers))
                for s in order:
                    rec = out[s.label][f"{kind}-K{k}"]
                    m0 = meta(s.conn)
                    lat = []
                    t0 = time.perf_counter()
                    r = s.conn.send_command("BEGIN")
                    for j in range(k):
                        counter["n"] += 1
                        t1 = time.perf_counter()
                        r = s.conn.send_command(
                            f"INSERT INTO {table} VALUES ({counter['n']}, {1 + j}, 0)")
                        lat.append(time.perf_counter() - t1)
                        if r.startswith("ERR"):
                            rec["errors"] += 1
                            rec.setdefault("first_error", r)
                            break
                    r = s.conn.send_command("COMMIT")
                    rec["txn_us"].append((time.perf_counter() - t0) * 1e6)
                    m1 = meta(s.conn)
                    rec["engine_us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                    rec["engine_polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                    if k >= 64:
                        acc = rec["stmt_us_by_pos"] or [[0.0, 0] for _ in range(8)]
                        n = len(lat)
                        for i, v in enumerate(lat):
                            b = min(7, i * 8 // max(1, n))
                            acc[b][0] += v * 1e6
                            acc[b][1] += 1
                        rec["stmt_us_by_pos"] = acc
    return {"ks": ks, "cells": out}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--cell", choices=("price", "ledger"), required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, default=None)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--ops", type=int, default=2000, help="price: statements per arm per server")
    p.add_argument("--blocks", type=int, default=4)
    p.add_argument("--parents", type=int, default=256)
    p.add_argument("--children", type=int, default=256)
    p.add_argument("--ks", default="1,64,1024,4096")
    p.add_argument("--rows-per-k", type=int, default=4096,
                   help="ledger: rows inserted per K per kind per server (reps = rows / K, min 5)")
    p.add_argument("--json", default=None)
    args = p.parse_args()

    servers = [Server(args.label, args.host, args.port)]
    if args.ab_port is not None:
        servers.append(Server(args.ab_label, args.host, args.ab_port))
    before = host_state()
    body = cell_price(args, servers) if args.cell == "price" else cell_ledger(args, servers)
    after = host_state()
    payload = {"driver": "fk_overhead_benchmark.py", "cell": args.cell, "args": vars(args),
               "host_before": before, "host_after": after, **body}
    print(f"host before: {before['loadavg']}  competing: {before['competing'] or 'none'}")
    print(f"host after : {after['loadavg']}  competing: {after['competing'] or 'none'}")
    if args.cell == "price":
        for label, arms in payload["arms"].items():
            print(f"== {label}")
            for name, s in arms.items():
                print(f"  {name:<26} p50={s['p50_us']:>8} p99={s['p99_us']:>8} qps={s['qps']:>8.0f} "
                      f"err={s['errors']}")
    else:
        for label, cells in payload["cells"].items():
            print(f"== {label}")
            for name, rec in cells.items():
                us = sorted(rec["txn_us"])
                k = int(name.split("K")[1])
                print(f"  {name:<12} reps={rec['reps']} txn median={us[len(us)//2]:.0f}us "
                      f"per-row={us[len(us)//2]/k:.1f}us errors={rec['errors']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"raw: {args.json}")


if __name__ == "__main__":
    sys.exit(main())
