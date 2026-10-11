#!/usr/bin/env python3
"""Scratch A/B driver for the assertion group-key coercion (KeyFor) change.

Shaped like tools/assertion_overhead_benchmark.py (whose helpers it imports):
serial, one session per server, two servers side by side, arms alternating
which server goes first per block. Adds UPDATE and DELETE arms (the paths that
key an old row and a new row) and a `--gtype date|int` switch for the group
column. Not a tools/ driver; it lives with the run's archive.

Relations, BTREE, `(id int64, g <gtype>)`:
  ev_a   CREATE ASSERTION cap ON ev_a GROUP BY (g) CHECK COUNT(*) <= 2000000000
  ev_n   no assertion (control)
Each preloaded with --preload rows, id i in group i (date: 2000-01-01 + i days).
"""
import argparse
import datetime
import json
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-assertion-literal-group-key/tools")
from bench_common import Phase, nearest_rank  # noqa: E402
from ckdbs_cli import DEFAULT_HOST  # noqa: E402
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa: E402

ASSERTED, PLAIN = "ev_a", "ev_n"
BASE = datetime.date(2000, 1, 1)
NEWBASE = datetime.date(2100, 1, 1)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, required=True)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--gtype", choices=["date", "int"], default="date")
    p.add_argument("--preload", type=int, default=1000)
    p.add_argument("--ops", type=int, default=8000)
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--json", default=None)
    args = p.parse_args()

    date = args.gtype == "date"
    coltype = "date" if date else "int64"

    def gval(i, new=False):
        if date:
            return "'%s'" % ((NEWBASE if new else BASE) + datetime.timedelta(days=i))
        return str(50_000_000 + i if new else i)

    servers = [Server(args.label, args.host, args.port),
               Server(args.ab_label, args.host, args.ab_port)]
    before = host_state()
    setup_info = {}
    preload = args.preload
    for s in servers:
        c = s.conn
        for t in (ASSERTED, PLAIN):
            c.send_command(f"DROP TABLE {t}")
        for t in (ASSERTED, PLAIN):
            must(c, f"CREATE TABLE {t} (id int64, g {coltype}) BTREE", "CREATED")
            for i in range(1, preload + 1):
                must(c, f"INSERT INTO {t} VALUES ({i}, {gval(i)})", "INSERTED")
        t0 = time.perf_counter()
        r = c.send_command(
            f"CREATE ASSERTION cap ON {ASSERTED} GROUP BY (g) CHECK COUNT(*) <= 2000000000")
        if r.startswith("ERR"):
            raise SystemExit(f"setup: CREATE ASSERTION -> {r!r}")
        setup_info[s.label] = {"create_assertion_ms": (time.perf_counter() - t0) * 1e3,
                               "show_assertions": c.send_command("SHOW ASSERTIONS")}

    per_block = max(1, args.ops // args.blocks)
    ctr = {"id": 10_000_000, "g": 0}

    def nid():
        ctr["id"] += 1
        return ctr["id"]

    def ngrp():
        ctr["g"] += 1
        return ctr["g"]

    # ids inserted by the existing-group arms, per table, deleted later.
    # Ids are deterministic per arm index so both servers see the same ids.
    def ins(table, existing, base_id):
        def fn(conn, k, ph, aux):
            i = base_id + k
            if existing:
                g = gval(1 + (k % preload))
            else:
                g = gval(k + 1, new=True)
            timed(conn, f"INSERT INTO {table} VALUES ({i}, {g})", ph)
        return fn

    def upd(table):
        def fn(conn, k, ph, aux):
            row = 1 + (k % preload)
            g = gval(1 + ((k * 7 + 3) % preload))
            timed(conn, f"UPDATE {table} SET g = {g} WHERE id = {row}", ph)
        return fn

    def dele(table, base_id):
        def fn(conn, k, ph, aux):
            timed(conn, f"DELETE FROM {table} WHERE id = {base_id + k}", ph)
        return fn

    def select(conn, k, ph, aux):
        timed(conn, f"SELECT * FROM {PLAIN} WHERE id = {1 + (k % preload)}", ph)

    def ping(conn, k, ph, aux):
        timed(conn, "SHOW META", ph)

    ID_EX, ID_NEW, ID_AGAIN = 10_000_000, 20_000_000, 30_000_000
    arm_list = [
        ("insert-assert-existing", ins(ASSERTED, True, ID_EX)),
        ("insert-assert-newgroup", ins(ASSERTED, False, ID_NEW)),
        ("insert-none-existing", ins(PLAIN, True, ID_EX)),
        ("insert-none-newgroup", ins(PLAIN, False, ID_NEW)),
        ("insert-none-again", ins(PLAIN, True, ID_AGAIN)),
        ("update-assert", upd(ASSERTED)),
        ("update-none", upd(PLAIN)),
        ("update-none-again", upd(PLAIN)),
        ("delete-assert", dele(ASSERTED, ID_EX)),
        ("delete-none", dele(PLAIN, ID_EX)),
        ("select", select),
        ("ping", ping),
    ]
    results = {s.label: {} for s in servers}
    engine = {s.label: {} for s in servers}
    blocks = {s.label: {n: [] for n, _ in arm_list} for s in servers}
    for name, fn in arm_list:
        for s in servers:
            results[s.label][name] = Phase(name)
            engine[s.label][name] = {"polls": 0.0, "us": 0.0}
        for block in range(args.blocks):
            order = servers if block % 2 == 0 else list(reversed(servers))
            for s in order:
                ph = results[s.label][name]
                n0 = len(ph.latencies)
                m0 = meta(s.conn)
                t0 = time.perf_counter()
                for k in range(per_block):
                    fn(s.conn, block * per_block + k, ph, None)
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                d_us = m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += d_us
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({
                    "block": block, "p50_us": lat[len(lat) // 2] * 1e6,
                    "mean_us": sum(lat) / len(lat) * 1e6,
                    "engine_us_per_op": d_us / len(lat)})
    after = host_state()

    def summ(ph):
        s = ph.summary()
        s["p75_us"] = round(ph.percentile(75) * 1e6, 1)
        return s

    payload = {"driver": "assertion_keyfor_ab.py", "args": vars(args),
               "setup": setup_info, "host_before": before, "host_after": after,
               "arms": {l: {n: summ(p) for n, p in r.items()} for l, r in results.items()},
               "engine": engine, "blocks": blocks, "ops_per_arm": per_block * args.blocks}
    for label, arms in payload["arms"].items():
        print(f"== {label} gtype={args.gtype} preload={preload} "
              f"create_assertion_ms={setup_info[label]['create_assertion_ms']:.1f} "
              f"enforcing={'enforcing=1' in setup_info[label]['show_assertions']}")
        for name, s in arms.items():
            print(f"  {name:<24} p50={s['p50_us']:>8} p99={s['p99_us']:>8} qps={s['qps']:>8.0f} err={s['errors']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)


if __name__ == "__main__":
    sys.exit(main())
