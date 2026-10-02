#!/usr/bin/env python3
"""AP-S5: what milestone AP costs the statements that never use it, A/B across
two engines.

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8 (the milestone-close A/B).
Shaped like `assertion_overhead_benchmark.py` and `fk_overhead_benchmark.py`
(whose helpers it imports): serial, one session per server, `--ab-port`
naming the other engine, arms alternating which server goes first per block,
`SHOW META` read around every block for the engine's own time per statement
(`sched_foreground_polled_us / sched_foreground_polls`). Without `--ab-port`
it runs on one server (the `fn` cell is B-only: A has no `DATE()`).

Every relation is BTREE. Every arm sends a *different literal* per statement,
so what is priced is the parse + fingerprint + dispatch + execute path, with
Waystone recording and replay as the server's default config has them.

Cell `point` (`--rows N`: 200, 1000, 10000), table `ap_t`
(id, v, a1..a9: eleven int64 columns, `v = 7 * id`, unique and unindexed):

  point-star        SELECT * FROM ap_t WHERE id = <k>          (C1)
  point-star-again  the noise floor: the arm above, repeated
  point-wide        SELECT id, v, a1..a8 (ten named columns) WHERE id = <k>  (C2)
  update-pk         UPDATE ap_t SET a1 = <n> WHERE id = <k>    (C4)
  update-pk-again   the noise floor: the arm above, repeated
  update-walk       UPDATE ap_t SET a2 = <n> WHERE v = <7k>    (C4: walks N rows,
                    one match; `--walk-ops` statements per arm)
  ping              control: SHOW META

Cell `scan` (`--rows N`: 1000, 10000, 60000), table `ap_s` (id, v, w; v = 7 id):

  scan-reject        SELECT id FROM ap_s WHERE v = <7k>        (C3: one match in N)
  scan-reject-again  the noise floor
  ping               control: SHOW META

Cell `fn` (`--rows N`, B-only information, not a delta), table `ap_f`
(id, ts timestamp, v), the rows spread over 100 UTC days:

  fn-date          SELECT id FROM ap_f WHERE DATE(ts) = '<day>'
  between          SELECT id FROM ap_f WHERE ts BETWEEN '<day> 00:00:00'
                   AND '<day> 23:59:59.999999'  (the same rows, checked once)
  fn-date-again    the noise floor
  ping             control: SHOW META

  `--index` adds a secondary index on ts first: BETWEEN can range over it, a
  function conjunct never becomes an index range.

Usage:
    tools/ap_overhead_benchmark.py --cell point --rows 1000 --port 15600 --label B \\
        --ab-port 15601 --ab-label A --ops 6000 --blocks 12 --json out.json
    tools/ap_overhead_benchmark.py --cell fn --rows 10000 --port 15600 --label B \\
        --ops 600 --blocks 12 --json out.json

Both servers must already be running on their own fresh data files and their
own chosen ports (`bench/README.md` rule 5).
"""

import argparse
import datetime
import json
import sys
import time

from bench_common import Phase
from ckdbs_cli import DEFAULT_HOST
from fk_overhead_benchmark import Server, host_state, meta, must, timed

BATCH = 500
DAY0 = datetime.datetime(2026, 1, 1)
DAYS = 100
WIDE = "id, v, a1, a2, a3, a4, a5, a6, a7, a8"


def load(conn, table, rows, row_fn):
    for lo in range(1, rows + 1, BATCH):
        hi = min(rows, lo + BATCH - 1)
        vals = ",".join(row_fn(i) for i in range(lo, hi + 1))
        must(conn, f"INSERT INTO {table} VALUES {vals}", "INSERTED")


def setup_point(conn, rows):
    conn.send_command("DROP TABLE ap_t")
    cols = ", ".join(f"a{j} int64" for j in range(1, 10))
    must(conn, f"CREATE TABLE ap_t (id int64, v int64, {cols}) BTREE", "CREATED")
    load(conn, "ap_t", rows,
         lambda i: f"({i}, {7 * i}, " + ", ".join(str(i + j) for j in range(1, 10)) + ")")


def setup_scan(conn, rows):
    conn.send_command("DROP TABLE ap_s")
    must(conn, "CREATE TABLE ap_s (id int64, v int64, w int64) BTREE", "CREATED")
    load(conn, "ap_s", rows, lambda i: f"({i}, {7 * i}, {i % 13})")


def day_of(i, rows):
    return DAY0 + datetime.timedelta(days=(i - 1) * DAYS // rows)


def setup_fn(conn, rows):
    conn.send_command("DROP TABLE ap_f")
    must(conn, "CREATE TABLE ap_f (id int64, ts timestamp, v int64) BTREE", "CREATED")

    def row(i):
        d = day_of(i, rows)
        sec = (i * 37) % 86400  # spread inside the day, deterministic
        t = d + datetime.timedelta(seconds=sec)
        return f"({i}, '{t:%Y-%m-%d %H:%M:%S}', {i % 11})"
    load(conn, "ap_f", rows, row)


def run_arms(args, servers, arm_list, walk_arms=()):
    results = {s.label: {} for s in servers}
    engine = {s.label: {} for s in servers}
    blocks = {s.label: {n: [] for n, _ in arm_list} for s in servers}
    for name, fn in arm_list:
        ops = args.walk_ops if name in walk_arms else args.ops
        per_block = max(1, ops // args.blocks)
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
                    fn(s.conn, block * per_block + k, ph)
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({
                    "block": block, "p50_us": lat[len(lat) // 2] * 1e6,
                    "mean_us": sum(lat) / len(lat) * 1e6,
                    "engine_us_per_op": (m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]) / len(lat)})
    return results, engine, blocks


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--cell", choices=("point", "scan", "fn"), required=True)
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, default=None)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--ops", type=int, default=6000, help="statements per arm per server")
    p.add_argument("--walk-ops", type=int, default=600,
                   help="statements per arm per server for the arms that walk the relation")
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--index", action="store_true",
                   help="fn: CREATE INDEX on ap_f(ts) before the arms (BETWEEN can use it, DATE() cannot)")
    p.add_argument("--json", default=None)
    args = p.parse_args()

    servers = [Server(args.label, args.host, args.port)]
    if args.ab_port is not None:
        servers.append(Server(args.ab_label, args.host, args.ab_port))
    if args.cell == "fn" and len(servers) > 1:
        raise SystemExit("fn is B-only: A has no DATE()")
    before = host_state()
    rows = args.rows
    ctr = {"n": 10_000_000, "bad": 0}
    setup_info = {}

    def nn():
        ctr["n"] += 1
        return ctr["n"]

    def ping(conn, k, ph):
        timed(conn, "SHOW META", ph)

    walk_arms = ()
    if args.cell == "point":
        for s in servers:
            setup_point(s.conn, rows)

        def star(conn, k, ph):
            timed(conn, f"SELECT * FROM ap_t WHERE id = {1 + (k * 7919) % rows}", ph)

        def wide(conn, k, ph):
            timed(conn, f"SELECT {WIDE} FROM ap_t WHERE id = {1 + (k * 7919) % rows}", ph)

        def upk(conn, k, ph):
            r = timed(conn, f"UPDATE ap_t SET a1 = {nn()} WHERE id = {1 + (k * 7919) % rows}", ph)
            ctr["bad"] += r != "UPDATED 1"

        def upw(conn, k, ph):
            r = timed(conn, f"UPDATE ap_t SET a2 = {nn()} WHERE v = {7 * (1 + (k * 7919) % rows)}", ph)
            ctr["bad"] += r != "UPDATED 1"

        arm_list = [("point-star", star), ("point-star-again", star), ("point-wide", wide),
                    ("update-pk", upk), ("update-pk-again", upk), ("update-walk", upw),
                    ("ping", ping)]
        walk_arms = ("update-walk",)
    elif args.cell == "scan":
        for s in servers:
            setup_scan(s.conn, rows)

        def scan(conn, k, ph):
            timed(conn, f"SELECT id FROM ap_s WHERE v = {7 * (1 + (k * 7919) % rows)}", ph)

        arm_list = [("scan-reject", scan), ("scan-reject-again", scan), ("ping", ping)]
        walk_arms = ("scan-reject", "scan-reject-again")
        args.walk_ops = args.ops
    else:
        for s in servers:
            setup_fn(s.conn, rows)
        if args.index:
            for s in servers:
                must(s.conn, "CREATE INDEX ap_f_ts ON ap_f (ts)", "CREATED")
        s0 = servers[0]

        def day(k):
            return DAY0 + datetime.timedelta(days=(k * 31) % DAYS)

        def q_fn(d):
            return f"SELECT id FROM ap_f WHERE DATE(ts) = '{d:%Y-%m-%d}'"

        def q_bt(d):
            return (f"SELECT id FROM ap_f WHERE ts BETWEEN '{d:%Y-%m-%d} 00:00:00' "
                    f"AND '{d:%Y-%m-%d} 23:59:59.999999'")
        # Same rows, checked on every distinct day before anything is timed.
        mism = 0
        counts = {}
        for dd in range(DAYS):
            d = DAY0 + datetime.timedelta(days=dd)
            a, b = s0.conn.send_command(q_fn(d)), s0.conn.send_command(q_bt(d))
            if a.startswith("ERR") or b.startswith("ERR"):
                raise SystemExit(f"fn setup: {a[:120]!r} / {b[:120]!r}")
            if sorted(a.split("\\n")[1:]) != sorted(b.split("\\n")[1:]):
                mism += 1
            counts[dd] = len(a.split("\\n")) - 1
        setup_info = {"days_with_different_rows": mism,
                      "rows_per_day_min": min(counts.values()), "rows_per_day_max": max(counts.values())}

        def fnq(conn, k, ph):
            timed(conn, q_fn(day(k)), ph)

        def btq(conn, k, ph):
            timed(conn, q_bt(day(k)), ph)

        arm_list = [("fn-date", fnq), ("between", btq), ("fn-date-again", fnq), ("ping", ping)]
        walk_arms = ("fn-date", "between", "fn-date-again")
        args.walk_ops = args.ops

    results, engine, blocks = run_arms(args, servers, arm_list, walk_arms)
    after = host_state()
    payload = {"driver": "ap_overhead_benchmark.py", "args": vars(args), "setup": setup_info,
               "update_replies_not_updated_1": ctr["bad"], "host_before": before, "host_after": after,
               "arms": {l: {n: dict(p.summary(), p90_us=round(p.percentile(90) * 1e6, 1))
                            for n, p in r.items()} for l, r in results.items()},
               "engine": engine, "blocks": blocks}
    print(f"host before: {before['loadavg']}  competing: {before['competing'] or 'none'}")
    print(f"host after : {after['loadavg']}  competing: {after['competing'] or 'none'}")
    print(f"update replies other than 'UPDATED 1': {ctr['bad']}")
    if setup_info:
        print(f"setup: {setup_info}")
    for label, arms in payload["arms"].items():
        print(f"== {label}  cell={args.cell} rows={rows}")
        for name, s in arms.items():
            print(f"  {name:<20} ops={s['ops']:>5} p50={s['p50_us']:>9} p90={s['p90_us']:>9} "
                  f"p99={s['p99_us']:>9} qps={s['qps']:>8.0f} err={s['errors']}")
            if s["errors"]:
                print(f"    first error: {s['first_error']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"raw: {args.json}")


if __name__ == "__main__":
    sys.exit(main())
