#!/usr/bin/env python3
"""AZ-S7: what assertion admission costs a one-row INSERT, A/B across two engines.

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8 (the milestone-close A/B).
`assertion_benchmark.py` prices an assertion against a twin on one engine; this
driver prices one *engine* against another, shaped like
`fk_overhead_benchmark.py` (whose helpers it imports): serial, one session per
server, `--ab-port` naming the other engine, arms alternating which server goes
first per block, `SHOW META` read around every block for the engine's own time
per statement (`sched_foreground_polled_us / sched_foreground_polls`).

Relations (all BTREE, `(id int64, g int64, v int64)`), each preloaded with
`--preload` rows in `--preload` distinct groups before the assertion is
declared:

  as_a   CREATE ASSERTION ... GROUP BY (g) CHECK COUNT(*) <= 2000000000
  as_n   no assertion (the control)

Arms:

  insert-assert-existing  autocommit INSERT into as_a, g drawn from the groups
                          that already exist (admission of a key that finds its
                          group)
  insert-assert-newgroup  autocommit INSERT into as_a, every row a g never seen
                          (admission of a key that opens a group)
  insert-none-existing    control: the same into as_n
  insert-none-newgroup    control: the same into as_n
  insert-none-again       the noise floor: insert-none-existing repeated
  select                  control: pk point read of as_n
  ping                    control: SHOW META

`--preload` is the row-set axis: 200, 1000, 10000 rows (and groups) present
when the measured inserts start. The CREATE ASSERTION over the preloaded
relation is timed once per server and reported as `create_assertion_ms`.

Usage:
    tools/assertion_overhead_benchmark.py --port 15600 --label B \\
        --ab-port 15601 --ab-label A --preload 1000 --ops 8000 --blocks 16 \\
        --json out.json

Both servers must already be running on their own fresh data files and their
own chosen ports (`bench/README.md` rule 5).
"""

import argparse
import json
import sys
import time

from bench_common import Phase
from ckdbs_cli import DEFAULT_HOST
from fk_overhead_benchmark import Server, host_state, meta, must, timed

ASSERTED, PLAIN = "as_a", "as_n"


def setup(conn, preload):
    for t in (ASSERTED, PLAIN):
        conn.send_command(f"DROP TABLE {t}")
    for t in (ASSERTED, PLAIN):
        must(conn, f"CREATE TABLE {t} (id int64, g int64, v int64) BTREE", "CREATED")
        for i in range(1, preload + 1):
            must(conn, f"INSERT INTO {t} VALUES ({i}, {i}, 0)", "INSERTED")
    t0 = time.perf_counter()
    r = conn.send_command(
        f"CREATE ASSERTION cap ON {ASSERTED} GROUP BY (g) CHECK COUNT(*) <= 2000000000")
    if r.startswith("ERR"):
        raise SystemExit(f"setup: CREATE ASSERTION -> {r!r}")
    ms = (time.perf_counter() - t0) * 1e3
    enforcing = conn.send_command("SHOW ASSERTIONS")
    return ms, enforcing


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, default=None)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--preload", type=int, default=1000)
    p.add_argument("--ops", type=int, default=8000, help="statements per arm per server")
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--json", default=None)
    args = p.parse_args()

    servers = [Server(args.label, args.host, args.port)]
    if args.ab_port is not None:
        servers.append(Server(args.ab_label, args.host, args.ab_port))
    before = host_state()
    setup_info = {}
    for s in servers:
        ms, show = setup(s.conn, args.preload)
        setup_info[s.label] = {"create_assertion_ms": ms, "show_assertions": show}
    per_block = max(1, args.ops // args.blocks)
    preload = args.preload
    ctr = {"id": 10_000_000, "g": 50_000_000}

    def nid():
        ctr["id"] += 1
        return ctr["id"]

    def ngrp():
        ctr["g"] += 1
        return ctr["g"]

    def ins(table, existing):
        def fn(conn, k, ph, aux):
            g = 1 + (k % preload) if existing else ngrp()
            timed(conn, f"INSERT INTO {table} VALUES ({nid()}, {g}, 0)", ph)
        return fn

    def select(conn, k, ph, aux):
        timed(conn, f"SELECT * FROM {PLAIN} WHERE id = {1 + (k % preload)}", ph)

    def ping(conn, k, ph, aux):
        timed(conn, "SHOW META", ph)

    arm_list = [
        ("insert-assert-existing", ins(ASSERTED, True)),
        ("insert-assert-newgroup", ins(ASSERTED, False)),
        ("insert-none-existing", ins(PLAIN, True)),
        ("insert-none-newgroup", ins(PLAIN, False)),
        ("insert-none-again", ins(PLAIN, True)),
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
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({
                    "block": block, "p50_us": lat[len(lat) // 2] * 1e6,
                    "mean_us": sum(lat) / len(lat) * 1e6,
                    "engine_us_per_op": (m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]) / len(lat)})
    after = host_state()
    payload = {"driver": "assertion_overhead_benchmark.py", "args": vars(args),
               "setup": setup_info, "host_before": before, "host_after": after,
               "arms": {l: {n: p.summary() for n, p in r.items()} for l, r in results.items()},
               "engine": engine, "blocks": blocks, "ops_per_arm": per_block * args.blocks}
    print(f"host before: {before['loadavg']}  competing: {before['competing'] or 'none'}")
    print(f"host after : {after['loadavg']}  competing: {after['competing'] or 'none'}")
    for label, arms in payload["arms"].items():
        print(f"== {label}  create_assertion_ms={setup_info[label]['create_assertion_ms']:.1f}")
        for name, s in arms.items():
            print(f"  {name:<26} p50={s['p50_us']:>8} p99={s['p99_us']:>8} qps={s['qps']:>8.0f} "
                  f"err={s['errors']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"raw: {args.json}")


if __name__ == "__main__":
    sys.exit(main())
