#!/usr/bin/env python3
"""A/B driver for the wrapped-integer-literal refusal (CoerceLiteralToColumn / PkBound).

Scratch driver for one measurement stage; tools/ is untouched. One process per
(rows, run): two servers side by side (A = base, B = change), identical data, every
arm run in blocks that alternate which server goes first, an equal count of
statements per server.

Relations (all BTREE), N = --rows:
  t   (id int64, qty int64, v int64)  N rows, id 1..N, qty = 3*id (unique)
  d   (id int64, v int64)             --blocks*--point-ops rows (the DELETE arm's target)
  ins (id int64, v int64)             the INSERT control's target
"""
import argparse
import json
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-int-literal-wrap-in-where/tools")
from bench_common import Phase  # noqa: E402
from ckdbs_cli import DEFAULT_HOST  # noqa: E402
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa: E402

BATCH = 500
MAXI = "9223372036854775807"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, required=True)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--point-ops", type=int, default=400, help="statements per block, point arms")
    p.add_argument("--scan-evals", type=int, default=40000,
                   help="row visits per block of a scan arm (statements per block = max(2, this // rows))")
    p.add_argument("--json", default=None)
    p.add_argument("--only", default=None, help="comma list of arm-name prefixes (smoke runs)")
    args = p.parse_args()
    n = args.rows
    servers = [Server(args.label, args.host, args.port),
               Server(args.ab_label, args.host, args.ab_port)]
    before = host_state()
    ndel = args.blocks * args.point_ops
    for s in servers:
        c = s.conn
        for t in ("ins", "d", "t"):
            c.send_command(f"DROP TABLE {t}")
        must(c, "CREATE TABLE t (id int64, qty int64, v int64) BTREE", "CREATED")
        must(c, "CREATE TABLE d (id int64, v int64) BTREE", "CREATED")
        must(c, "CREATE TABLE ins (id int64, v int64) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            rows = ",".join(f"({i},{3 * i},0)" for i in range(lo, min(n, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO t VALUES {rows}", "INSERTED")
        for lo in range(1, ndel + 1, BATCH):
            rows = ",".join(f"({i},0)" for i in range(lo, min(ndel, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO d VALUES {rows}", "INSERTED")

    scan_per_block = max(2, args.scan_evals // n)
    point_per_block = args.point_ops
    arms = [
        ("sel-pk", "point", lambda k: f"SELECT * FROM t WHERE id = {1 + (k % n)}"),
        ("sel-pk-again", "point", lambda k: f"SELECT * FROM t WHERE id = {1 + ((k * 7) % n)}"),
        ("sel-pk-long", "point", lambda k: f"SELECT * FROM t WHERE id = {MAXI}"),
        ("sel-nonpk", "scan", lambda k: f"SELECT * FROM t WHERE qty = {3 * (1 + (k % n))}"),
        ("sel-nonpk-again", "scan", lambda k: f"SELECT * FROM t WHERE qty = {3 * (1 + ((k * 7) % n))}"),
        ("upd-pk", "point", lambda k: f"UPDATE t SET v = {k} WHERE id = {1 + (k % n)}"),
        ("del-pk", "point", lambda k: f"DELETE FROM d WHERE id = {1 + k}"),
        ("ctl-insert", "point", lambda k: f"INSERT INTO ins VALUES ({1000000 + k}, {k})"),
        ("ctl-ping", "point", lambda k: "SHOW META"),
    ]
    if args.only:
        pre = tuple(args.only.split(","))
        arms = [a for a in arms if a[0].startswith(pre)]
    labels = [s.label for s in servers]
    results = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    blocks = {l: {} for l in labels}
    rowcount = {l: {} for l in labels}
    for name, kind, fn in arms:
        per_block = point_per_block if kind == "point" else scan_per_block
        for s in servers:
            results[s.label][name] = Phase(name)
            engine[s.label][name] = {"polls": 0.0, "us": 0.0}
            blocks[s.label][name] = []
        for block in range(args.blocks):
            order = servers if block % 2 == 0 else list(reversed(servers))
            for s in order:
                ph = results[s.label][name]
                n0 = len(ph.latencies)
                m0 = meta(s.conn)
                t0 = time.perf_counter()
                for k in range(per_block):
                    kk = block * per_block + k
                    reply = timed(s.conn, fn(kk), ph)
                    tot = rowcount[s.label].setdefault(
                        name, {"first_rows": reply.count("\\n"), "first_head": reply[:60], "total_rows": 0})
                    tot["total_rows"] += reply.count("\\n")
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                d_us = m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += d_us
                lat = ph.latencies[n0:]
                blocks[s.label][name].append({
                    "block": block, "n": len(lat),
                    "lat_us": [round(x * 1e6, 1) for x in lat],
                    "engine_us": d_us})
    after = host_state()
    payload = {"driver": "lw_ab.py", "args": vars(args), "host_before": before, "host_after": after,
               "per_block": {"point": point_per_block, "scan": scan_per_block},
               "rowcount": rowcount, "engine": engine,
               "errors": {l: {nm: ph.errors for nm, ph in r.items()} for l, r in results.items()},
               "first_error": {l: {nm: ph.first_error for nm, ph in r.items() if ph.errors} for l, r in results.items()},
               "blocks": blocks}
    for l in labels:
        print(f"== {l} rows={n}")
        for name, _, _ in arms:
            s = results[l][name].summary()
            print(f"  {name:<18} ops={s['ops']:>5} p50={s['p50_us']:>9} p99={s['p99_us']:>9} err={s['errors']} "
                  f"rows0={rowcount[l][name]['first_rows']} total={rowcount[l][name]['total_rows']} "
                  f"head={rowcount[l][name]['first_head']!r} fe={results[l][name].first_error!r}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f)


if __name__ == "__main__":
    sys.exit(main())
