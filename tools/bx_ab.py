#!/usr/bin/env python3
"""BX close A/B driver (scratch; imports tools/bn_census.py unmodified).
Arms LABEL=BINARY[,unique]. Per run: fresh server per arm, all pinned to one
CPU, one busy at a time, blocks rotate order. Shapes on indexed btree tables:
insert (index on w), update-idx (SET v, index on v), update-w (SET w, index on v)."""
import argparse, json, os, sys, time, types
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp, bn_census as bn
from bb_overhead_benchmark import host_state

i64 = kwp.Connection.int64
ROWS = 1000
SHAPES = [
    ("insert", "INSERT INTO i VALUES (?, ?, ?)", lambda q: [i64(10_000_000 + q)] * 3, "INSERT", "point"),
    ("update-idx", "UPDATE t SET v = ? WHERE id = ?", lambda q: [i64(1_000_000 + q), i64(1 + (q * 7919) % ROWS)], "UPDATED 1", "point"),
    ("update-w", "UPDATE t SET w = ? WHERE id = ?", lambda q: [i64(q), i64(1 + (q * 7919) % ROWS)], "UPDATED 1", "point"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31960)
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    global ROWS
    ROWS = a.rows
    a.pin_server, a.pin_driver, a.cores, a.durability = "2", "4", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = []
    for s in a.arm:
        lab, rest = s.split("=", 1)
        b, _, flag = rest.partition(",")
        arms.append((lab, b, flag == "unique"))
    results = []
    for run in range(a.runs):
        srv = []
        for k, (lab, b, uniq) in enumerate(arms):
            s = bn.Server(a, lab, b, a.port + k, f"r{run}")
            c = s.conn
            bn.load(c, "t", a.rows)
            bn.must(c, "CREATE TABLE i (id int64, v int64, w int64) BTREE", "CREATE")
            u = "UNIQUE " if uniq else ""
            bn.must(c, f"CREATE {u}INDEX ix_i_w ON i(w)", "CREATE")
            bn.must(c, f"CREATE {u}INDEX ix_t_v ON t(v)", "CREATE")
            srv.append(s)
        time.sleep(1.5)
        for shape in SHAPES:
            for path in ("literal", "prepared"):
                host = host_state()
                accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srv]
                n = a.ops
                per = n // a.blocks
                for blk in range(a.blocks):
                    qs = range(blk * per, (blk + 1) * per)
                    # statement numbers must be unique across paths: offset literal/prepared
                    off = 0 if path == "literal" else 500_000
                    qs = [q + off for q in qs]
                    order = [(j + run + blk) % len(srv) for j in range(len(srv))]
                    for j in order:
                        bn.run_block(srv[j], shape, path, qs, accs[j])
                for j, s in enumerate(srv):
                    r = bn.summarize(shape[0], path, s.label, accs[j])
                    r["run"] = run
                    r["load"] = host["loadavg"]
                    r["competing"] = len(host["competing"])
                    results.append(r)
        for s in srv:
            s.stop()
        with open(a.progress, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S', time.gmtime())} run {run + 1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}\n")
        json.dump(results, open(a.out, "w"))
    json.dump(results, open(a.out, "w"))
    with open(a.progress, "a") as f:
        f.write("DONE\n")


main()
