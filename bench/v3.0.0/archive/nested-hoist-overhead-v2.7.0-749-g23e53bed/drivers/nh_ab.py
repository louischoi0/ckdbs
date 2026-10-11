#!/usr/bin/env python3
"""A/B driver for the nested-hoist change (SubChain::hoisted, HoistedAllTrue).

Scratch driver for one measurement stage; tools/ is untouched. Shaped like the
assertion KeyFor driver: one process per (rows, run), two servers side by side
(A = base, B = change), each with the same data, every arm run in blocks that
alternate which server goes first, an equal count of statements per server.

Relations (all BTREE), N = --rows:
  a (id int64, v int64)             N rows, id 1..N, v = id % 7
  b (id int64, a_id int64, v int64) N/2 rows, a_id = 2*id (every even a.id has one);
                                    CREATE INDEX ix_b_aid ON b(a_id)
  c (id int64, tag int64)           50 rows, tag = id (tag 999999 never exists)
  ins (id int64, v int64)           the INSERT control's target

Arms: see ARMS below.
"""
import argparse
import json
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery/tools")
from bench_common import Phase  # noqa: E402
from ckdbs_cli import DEFAULT_HOST  # noqa: E402
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa: E402

BATCH = 500
CORR = "EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id)"
NCORR = "NOT EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id)"
UNC = "EXISTS (SELECT c.id FROM c WHERE c.tag = 5)"
INSUB = "a.id IN (SELECT b.a_id FROM b WHERE b.id BETWEEN 1 AND 20)"
SCAL = "a.v = (SELECT c.tag FROM c WHERE c.id = 3)"
NONE = "EXISTS (SELECT c.id FROM c WHERE c.tag = 999999)"
SOME = "EXISTS (SELECT c.id FROM c WHERE c.tag = 5)"
NEST_NONE = f"EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id AND {NONE})"
NEST_SOME = f"EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id AND {SOME})"


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
                   help="outer-row visits per block of a scan arm (statements per block = max(2, this // rows))")
    p.add_argument("--json", default=None)
    p.add_argument("--only", default=None, help="comma list of arm-name prefixes (smoke runs)")
    args = p.parse_args()
    n = args.rows
    servers = [Server(args.label, args.host, args.port),
               Server(args.ab_label, args.host, args.ab_port)]
    before = host_state()
    for s in servers:
        c = s.conn
        for t in ("ins", "c", "b", "a"):
            c.send_command(f"DROP TABLE {t}")
        must(c, "CREATE TABLE a (id int64, v int64) BTREE", "CREATED")
        must(c, "CREATE TABLE b (id int64, a_id int64, v int64) BTREE", "CREATED")
        must(c, "CREATE TABLE c (id int64, tag int64) BTREE", "CREATED")
        must(c, "CREATE TABLE ins (id int64, v int64) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            rows = ",".join(f"({i},{i % 7})" for i in range(lo, min(n, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO a VALUES {rows}", "INSERTED")
        half = n // 2
        for lo in range(1, half + 1, BATCH):
            rows = ",".join(f"({i},{2 * i},0)" for i in range(lo, min(half, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO b VALUES {rows}", "INSERTED")
        rows = ",".join(f"({i},{i})" for i in range(1, 51))
        must(c, f"INSERT INTO c VALUES {rows}", "INSERTED")
        must(c, "CREATE INDEX ix_b_aid ON b(a_id)", "CREATED")

    ctr = {"ins": 0}

    def sel(where):
        return lambda k: f"SELECT a.id FROM a WHERE {where}"

    def pt(where):
        return lambda k: f"SELECT a.id FROM a WHERE a.id = {1 + (k % n)} AND {where}"

    scan_per_block = max(2, args.scan_evals // n)
    point_per_block = args.point_ops
    # (name, kind, sql(k)) ; kind decides statements per block
    arms = [
        ("corr-exists-point", "point", pt(CORR)),
        ("corr-notexists-point", "point", pt(NCORR)),
        ("corr-exists-scan", "scan", sel(CORR)),
        ("unc-exists-point", "point", pt(UNC)),
        ("unc-exists-scan", "scan", sel(f"a.v = 3 AND {UNC}")),
        ("unc-in-point", "point", pt(INSUB)),
        ("unc-in-scan", "scan", sel(INSUB)),
        ("unc-scalar-point", "point", pt(SCAL)),
        ("nested-none-point", "point", pt(NEST_NONE)),
        ("nested-none-scan", "scan", sel(NEST_NONE)),
        ("nested-some-point", "point", pt(NEST_SOME)),
        ("nested-some-scan", "scan", sel(NEST_SOME)),
        ("ctl-select-pk", "point", lambda k: f"SELECT * FROM a WHERE id = {1 + (k % n)}"),
        ("ctl-select-pk-again", "point", lambda k: f"SELECT * FROM a WHERE id = {1 + ((k * 7) % n)}"),
        ("ctl-scan-filter", "scan", sel("a.v = 3")),
        ("ctl-scan-filter-again", "scan", sel("a.v = 3")),
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
    payload = {"driver": "nh_ab.py", "args": vars(args), "host_before": before, "host_after": after,
               "per_block": {"point": point_per_block, "scan": scan_per_block},
               "rowcount": rowcount, "engine": engine,
               "errors": {l: {nm: ph.errors for nm, ph in r.items()} for l, r in results.items()},
               "first_error": {l: {nm: ph.first_error for nm, ph in r.items() if ph.errors} for l, r in results.items()},
               "blocks": blocks}
    for l in labels:
        print(f"== {l} rows={n}")
        for name, _, _ in arms:
            s = results[l][name].summary()
            print(f"  {name:<22} ops={s['ops']:>5} p50={s['p50_us']:>9} p99={s['p99_us']:>9} err={s['errors']} rows0={rowcount[l][name]['first_rows']} total={rowcount[l][name]['total_rows']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f)


if __name__ == "__main__":
    sys.exit(main())
