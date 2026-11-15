#!/usr/bin/env python3
"""CS close, question 1 (CS-R5): the overhead off the read-only path.

Three arms on three servers (labels given by --arm LABEL=BINARY; the run uses
A, B and A2 = a byte-identical copy of A), cores = 1, relaxed, one connection.
Shapes at 200 / 1000 / 10000 rows: autocommit tuple-key SELECT, autocommit
point UPDATE, and `BEGIN; 4 tuple-key SELECTs; COMMIT` (plain BEGIN, READ
WRITE). Per op: client latency; per block: the server's on-CPU ns
(/proc/<pid>/task/*/schedstat, as bn_census does). Blocks rotate arm order.

Usage: cs_close_overhead.py --arm A=bin --arm B=bin --arm A2=bin --workdir D
       --out OUT.json --progress FILE [--runs 5] [--blocks 6] [--scale 1.0]
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp, bn_census as bn  # noqa: E402,F401

SIZES = [200, 1000, 10000]


def unit(shape, n, q, conn):
    key = lambda j: 1 + ((q * 4 + j) * 7919) % n
    if shape == "select-pk":
        _f, rows, _t, _c = conn.execute(f"SELECT * FROM t{n} WHERE id = {key(0)}")
        return len(rows) == 1
    if shape == "update-pk":
        _f, _r, got, _c = conn.execute(f"UPDATE t{n} SET v = v + 1 WHERE id = {key(0)}")
        return got.startswith("UPDATED 1")
    ok = True
    conn.execute("BEGIN")
    for j in range(4):
        _f, rows, _t, _c = conn.execute(f"SELECT * FROM t{n} WHERE id = {key(j)}")
        ok = ok and len(rows) == 1
    conn.execute("COMMIT")
    return ok


def block(srv, shape, n, qs, acc):
    s0, w0 = bn.sched_ns(srv.proc.pid), time.perf_counter()
    for q in qs:
        t0 = time.perf_counter()
        ok = unit(shape, n, q, srv.conn)
        acc["lat"].append(time.perf_counter() - t0)
        acc["bad"] += 0 if ok else 1
    acc["wall"] += time.perf_counter() - w0
    acc["server_ns"] += bn.sched_ns(srv.proc.pid) - s0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31970)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = [tuple(s.split("=", 1)) for s in a.arm]
    ops = {"select-pk": 3000, "update-pk": 3000, "txn4-rw": 1000}
    results = []
    for run in range(a.runs):
        srvs = [bn.Server(a, lab, b, a.port + k, f"r{run}") for k, (lab, b) in enumerate(arms)]
        for s in srvs:
            for n in SIZES:
                bn.load(s.conn, f"t{n}", n)
        time.sleep(1.5)
        for shape in ops:
            for n in SIZES:
                cnt = int(ops[shape] * a.scale)
                accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srvs]
                per = cnt // a.blocks
                for blk in range(a.blocks):
                    qs = list(range(blk * per, (blk + 1) * per))
                    for j in [(k + run + blk) % len(srvs) for k in range(len(srvs))]:
                        block(srvs[j], shape, n, qs, accs[j])
                for j, s in enumerate(srvs):
                    r = bn.summarize(shape, "literal", s.label, accs[j])
                    r["p75_us"] = bn.pct(accs[j]["lat"], 75) * 1e6
                    r.update(run=run, rows=n, load=open("/proc/loadavg").read().split()[0])
                    results.append(r)
                json.dump(results, open(a.out, "w"))
                open(a.progress, "w").write(str(time.time()))
        for s in srvs:
            s.stop()
        print(f"run {run+1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}", flush=True)
    open(a.progress, "w").write(str(time.time()))
    print("ALL DONE", flush=True)


main()
