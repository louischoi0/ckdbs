#!/usr/bin/env python3
"""Amplification arms for the row-encode overhead A/B: the same wide table as ew_ab.py, but one statement
encodes many rows, so the per-column cost of EncodeOneValue is multiplied by 9 * ROWS per statement.

  ins-bulk   INSERT INTO w VALUES <250 wide rows>      250 rows x 9 columns encoded per statement
  upd-range  UPDATE w SET c3 = k WHERE id >= a AND id <= a+199   200 rows re-encoded (9 columns each)
  sel-range  SELECT * FROM w WHERE id >= a AND id <= a+199       200 rows read, no encode (control)

Two servers side by side, blocks alternate which goes first, equal statement counts, fresh servers per run.
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
RANGE = 200


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
    p.add_argument("--blocks", type=int, default=20)
    p.add_argument("--ops", type=int, default=10, help="statements per block per arm")
    p.add_argument("--json", default=None)
    args = p.parse_args()
    n = args.rows
    servers = [Server(args.label, args.host, args.port), Server(args.ab_label, args.host, args.ab_port)]
    before = host_state()
    for s in servers:
        c = s.conn
        c.send_command("DROP TABLE w")
        must(c, "CREATE TABLE w (id int64, c1 int64, c2 int64, c3 int64, c4 int64, c5 int64, c6 int64, "
                "s varchar, d date) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            rows = ",".join(wide_row(i, 3 * i) for i in range(lo, min(n, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO w VALUES {rows}", "INSERTED")

    base = 10_000_000
    span = max(1, n - RANGE + 1)

    def ins_bulk(k):
        return "INSERT INTO w VALUES " + ",".join(wide_row(base + k * BATCH + j, j) for j in range(BATCH))

    def rng(k):
        a = 1 + ((k * 37) % span)
        return a, a + RANGE - 1
    arms = [
        ("sel-range", lambda k: "SELECT * FROM w WHERE id >= %d AND id <= %d" % rng(k)),
        ("upd-range", lambda k: f"UPDATE w SET c3 = {k} WHERE id >= %d AND id <= %d" % rng(k)),
        ("ins-bulk", ins_bulk),
    ]
    labels = [s.label for s in servers]
    results = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    blocks = {l: {} for l in labels}
    rowcount = {l: {} for l in labels}
    for name, fn in arms:
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
                for k in range(args.ops):
                    kk = block * args.ops + k
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
    n_ins = args.blocks * args.ops * BATCH
    verify = {}
    for s in servers:
        c = s.conn
        verify[s.label] = {
            "count_w": c.send_command("SELECT COUNT(*) FROM w"),
            "bulk_last": c.send_command(f"SELECT * FROM w WHERE id = {base + n_ins - 1}"),
            "upd_sum": c.send_command("SELECT SUM(c3) FROM w WHERE id >= 1 AND id <= %d" % n)}
    ok = (verify[labels[0]] == verify[labels[1]] and str(n + n_ins) in verify[labels[0]]["count_w"])
    after = host_state()
    payload = {"driver": "ew_bulk.py", "args": vars(args), "host_before": before, "host_after": after,
               "verify": verify, "verify_ok": ok, "rowcount": rowcount, "engine": engine,
               "errors": {l: {nm: ph.errors for nm, ph in r.items()} for l, r in results.items()},
               "first_error": {l: {nm: ph.first_error for nm, ph in r.items() if ph.errors}
                               for l, r in results.items()},
               "blocks": blocks}
    for l in labels:
        print(f"== {l} rows={n}")
        for name, _ in arms:
            s = results[l][name].summary()
            print(f"  {name:<10} ops={s['ops']:>4} p50={s['p50_us']:>9} p99={s['p99_us']:>9} err={s['errors']} "
                  f"head={rowcount[l][name]['first_head']!r} total={rowcount[l][name]['total_rows']} fe={results[l][name].first_error!r}")
        print("  verify", verify[l])
    print("verify_ok", ok)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
