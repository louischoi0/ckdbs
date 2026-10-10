#!/usr/bin/env python3
"""A/B driver for the row-encode overhead of EncodeOneValue's pre-switch IntLiteralWrapped check.

Scratch driver for one measurement stage; tools/ is untouched. One process per (rows, run): two
servers side by side (slot A, slot B), identical data, every arm run in blocks that alternate which
server goes first, an equal count of statements per server.

Relations (all BTREE), N = --rows:
  w   (id int64, c1..c6 int64, s varchar, d date)   N rows, id 1..N          -- the wide row, 9 columns
  nw  (id int64, v int64)                           empty                     -- the narrow row, 2 columns
Arms (run in this order; ins-* grow their relation by blocks*ins-ops rows):
  sel-pk      SELECT * FROM w WHERE id = ..          control, no encode
  upd-wide    UPDATE w SET c3 = k WHERE id = ..      point row, re-encodes all 9 columns
  ins-narrow  INSERT INTO nw VALUES (id, v)          2 columns encoded
  ins-wide    INSERT INTO w VALUES (9 values)        9 columns encoded
  ctl-ping    SHOW META                              client/socket floor, no storage
"""
import argparse
import json
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-date-wrapped-literal-stored/tools")
from bench_common import Phase  # noqa: E402
from ckdbs_cli import DEFAULT_HOST  # noqa: E402
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa: E402

BATCH = 250


def wide_row(i, c):
    return f"({i},{c},{c + 1},{c + 2},{c + 3},{c + 4},{c + 5},'s{i}','2026-08-09')"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, required=True)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--blocks", type=int, default=10)
    p.add_argument("--sel-ops", type=int, default=400, help="statements per block, sel-pk / ctl-ping")
    p.add_argument("--upd-ops", type=int, default=200, help="statements per block, upd-wide")
    p.add_argument("--ins-ops", type=int, default=100, help="statements per block, ins-*")
    p.add_argument("--json", default=None)
    args = p.parse_args()
    n = args.rows
    servers = [Server(args.label, args.host, args.port), Server(args.ab_label, args.host, args.ab_port)]
    before = host_state()
    for s in servers:
        c = s.conn
        for t in ("w", "nw"):
            c.send_command(f"DROP TABLE {t}")
        must(c, "CREATE TABLE w (id int64, c1 int64, c2 int64, c3 int64, c4 int64, c5 int64, c6 int64, "
                "s varchar, d date) BTREE", "CREATED")
        must(c, "CREATE TABLE nw (id int64, v int64) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            rows = ",".join(wide_row(i, 3 * i) for i in range(lo, min(n, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO w VALUES {rows}", "INSERTED")

    base = 10_000_000
    arms = [
        ("sel-pk", args.sel_ops, lambda k: f"SELECT * FROM w WHERE id = {1 + (k % n)}"),
        ("upd-wide", args.upd_ops, lambda k: f"UPDATE w SET c3 = {k} WHERE id = {1 + (k % n)}"),
        ("ins-narrow", args.ins_ops, lambda k: f"INSERT INTO nw VALUES ({base + k}, {k})"),
        ("ins-wide", args.ins_ops, lambda k: "INSERT INTO w VALUES " + wide_row(base + k, k)),
        ("ctl-ping", args.sel_ops, lambda k: "SHOW META"),
    ]
    labels = [s.label for s in servers]
    results = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    blocks = {l: {} for l in labels}
    rowcount = {l: {} for l in labels}
    for name, per_block, fn in arms:
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
    # verify: the writes landed identically on both servers
    n_ins = args.blocks * args.ins_ops
    last_u = args.blocks * args.upd_ops - 1
    verify = {}
    for s in servers:
        c = s.conn
        v = {"count_w": c.send_command("SELECT COUNT(*) FROM w"),
             "count_nw": c.send_command("SELECT COUNT(*) FROM nw"),
             "wide_last": c.send_command(f"SELECT * FROM w WHERE id = {base + n_ins - 1}"),
             "narrow_last": c.send_command(f"SELECT * FROM nw WHERE id = {base + n_ins - 1}"),
             "upd_last": c.send_command(f"SELECT * FROM w WHERE id = {1 + (last_u % n)}")}
        verify[s.label] = v
    ok = (verify[labels[0]] == verify[labels[1]]
          and str(n + n_ins) in verify[labels[0]]["count_w"]
          and str(n_ins) in verify[labels[0]]["count_nw"])
    after = host_state()
    payload = {"driver": "ew_ab.py", "args": vars(args), "host_before": before, "host_after": after,
               "verify": verify, "verify_ok": ok,
               "rowcount": rowcount, "engine": engine,
               "errors": {l: {nm: ph.errors for nm, ph in r.items()} for l, r in results.items()},
               "first_error": {l: {nm: ph.first_error for nm, ph in r.items() if ph.errors}
                               for l, r in results.items()},
               "blocks": blocks}
    for l in labels:
        print(f"== {l} rows={n}")
        for name, _, _ in arms:
            s = results[l][name].summary()
            print(f"  {name:<12} ops={s['ops']:>5} p50={s['p50_us']:>9} p99={s['p99_us']:>9} err={s['errors']} "
                  f"head={rowcount[l][name]['first_head']!r} fe={results[l][name].first_error!r}")
        print("  verify", verify[l])
    print("verify_ok", ok)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
