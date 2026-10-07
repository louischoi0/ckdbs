#!/usr/bin/env python3
"""BB-S5, cell C5: `ORDER BY <pk>` over a relation that was kUnordered at A.

`instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` BB-R8 / BB-R10.
A and B run side by side (fresh server and data file each, per run), the
statements interleaved by block like `bb_overhead_benchmark.py`. The relation
is `bb_u (id int64, v int64) BTREE` holding `--rows` rows loaded in 500-row
batches of named keys: **A loads them in descending order** (the relation
turns kUnordered there and its walk sorts each page), **B loads the same keys
ascending** (a key below the tail is refused at B). The first run's first
descending batch is also sent to B and its reply recorded in the JSON.

Arms:
  limit        SELECT * FROM bb_u ORDER BY id LIMIT 100
  limit-again  the noise floor
  full         SELECT * FROM bb_u ORDER BY id
  ping         control: SHOW META

`--limit-ops` / `--full-ops` statements per arm per server. The first reply of
each arm on each server is compared (row sets and order) and the result
recorded.
"""

import argparse
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase  # noqa: E402
from bb_overhead_benchmark import Srv, host_state, meta_all, must, wait_for_load  # noqa: E402

BATCH = 500


def load(conn, rows, descending):
    starts = list(range(1, rows + 1, BATCH))
    if descending:
        starts.reverse()
    for lo in starts:
        hi = min(rows, lo + BATCH - 1)
        ids = range(hi, lo - 1, -1) if descending else range(lo, hi + 1)
        must(conn, "INSERT INTO bb_u VALUES " + ",".join(f"({i}, {i * 3})" for i in ids), "INSERTED")


def run_one(args, rows, run):
    tag = f"c5-n{rows}-r{run}"
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    wd = os.path.join(args.workdir, tag)
    mk = {"B": (args.bin_b, args.port_b), "A": (args.bin_a, args.port_a)}
    labels = ["B", "A"] if run % 2 == 0 else ["A", "B"]
    servers = []
    try:
        for l in labels:
            servers.append(Srv(l, mk[l][0], wd, mk[l][1], args.pin_server, tag))
        servers.sort(key=lambda s: s.label)
        probe = None
        for s in servers:
            must(s.conn, "CREATE TABLE bb_u (id int64, v int64) BTREE", "CREATED")
            if s.label == "B" and run == 0:
                # the descending batch B is asked to take; recorded, not loaded
                s.conn.send_command("CREATE TABLE bb_probe (id int64, v int64) BTREE")
                s.conn.send_command("INSERT INTO bb_probe VALUES (500, 1)")
                probe = s.conn.send_command("INSERT INTO bb_probe VALUES (499, 1)")
            load(s.conn, rows, descending=(s.label == "A"))
        stm = {"limit": "SELECT * FROM bb_u ORDER BY id LIMIT 100",
               "limit-again": "SELECT * FROM bb_u ORDER BY id LIMIT 100",
               "full": "SELECT * FROM bb_u ORDER BY id", "ping": "SHOW META"}
        firsts = {}
        for s in servers:
            firsts[s.label] = {k: s.conn.send_command(q) for k, q in stm.items() if k in ("limit", "full")}
        same = {k: firsts["A"][k] == firsts["B"][k] for k in ("limit", "full")}
        sizes = {l: {k: len(v) for k, v in d.items()} for l, d in firsts.items()}
        res = {s.label: {} for s in servers}
        eng = {s.label: {} for s in servers}
        for name, ops in (("limit", args.limit_ops), ("limit-again", args.limit_ops),
                          ("full", args.full_ops), ("ping", args.limit_ops)):
            per_block = max(1, ops // args.blocks)
            for s in servers:
                res[s.label][name] = Phase(name)
                eng[s.label][name] = {"polls": 0.0, "us": 0.0}
            for block in range(args.blocks):
                order = servers if block % 2 == 0 else list(reversed(servers))
                for s in order:
                    ph = res[s.label][name]
                    m0 = meta_all(s.conn)
                    t0 = time.perf_counter()
                    for _ in range(per_block):
                        t1 = time.perf_counter()
                        r = s.conn.send_command(stm[name])
                        ph.record(time.perf_counter() - t1, r)
                    ph.elapsed += time.perf_counter() - t0
                    m1 = meta_all(s.conn)
                    eng[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                    eng[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
        after = host_state()
        payload = {"rows": rows, "run": run, "wait_s": round(wait, 1), "host_before": before,
                   "host_after": after, "probe_B_descending_reply": probe, "same_reply_AB": same,
                   "reply_bytes": sizes,
                   "mount": {s.label: s.mount.get("recovery_checkpoint_us") for s in servers},
                   "arms": {l: {n: p.summary() for n, p in r.items()} for l, r in res.items()},
                   "engine": eng}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(payload, f, indent=1)
        a, b = payload["arms"]["A"], payload["arms"]["B"]
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} same={same} "
              f"limit p50 A={a['limit']['p50_us']} B={b['limit']['p50_us']} "
              f"full p50 A={a['full']['p50_us']} B={b['full']['p50_us']} "
              f"errs={sum(p['errors'] for l in payload['arms'].values() for p in l.values())} probe={probe}",
              flush=True)
    finally:
        for s in servers:
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rows", default="10000")
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--limit-ops", type=int, default=1200)
    p.add_argument("--full-ops", type=int, default=120)
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--port-a", type=int, required=True)
    p.add_argument("--port-b", type=int, required=True)
    p.add_argument("--pin-server", type=int, default=None)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-load", type=float, default=1.3)
    p.add_argument("--max-wait", type=float, default=120)
    args = p.parse_args()
    for rows in [int(x) for x in args.rows.split(",")]:
        for run in range(args.runs):
            run_one(args, rows, run)


if __name__ == "__main__":
    sys.exit(main())
