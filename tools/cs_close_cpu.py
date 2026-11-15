#!/usr/bin/env python3
"""CS close, supplement: server CPU per group for arms A (autocommit), W
(plain BEGIN) and R (BEGIN READ ONLY) on one binary, cores = 1, relaxed, one
connection, one 10,000-row relation; k in {1, 16} tuple-key SELECTs per group.
The server's on-CPU ns come from /proc/<pid>/task/*/schedstat (bn_census.sched_ns);
arms run in rotated blocks on one fresh server per run.

Usage: cs_close_cpu.py --bin B --workdir D --out OUT.json [--runs 8] [--blocks 6]
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bn_census as bn  # noqa: E402

N = 10000


def group(conn, arm, k, q):
    if arm == "W":
        conn.execute("BEGIN")
    elif arm == "R":
        conn.execute("BEGIN READ ONLY")
    ok = True
    for j in range(k):
        key = 1 + ((q * 16 + j) * 7919) % N
        _f, rows, _t, _c = conn.execute(f"SELECT * FROM t WHERE id = {key}")
        ok = ok and len(rows) == 1
    if arm != "A":
        conn.execute("COMMIT")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=31980)
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--blocks", type=int, default=6)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = ["A", "W", "R"]
    groups = {1: 3000, 16: 300}
    out = []
    for run in range(a.runs):
        srv = bn.Server(a, "B", a.bin, a.port, f"cpu{run}")
        bn.load(srv.conn, "t", N)
        time.sleep(1.0)
        for k, cnt in groups.items():
            acc = {x: {"lat": [], "cpu": 0, "bad": 0} for x in arms}
            per = cnt // a.blocks
            for blk in range(a.blocks):
                for x in [arms[(i + run + blk) % 3] for i in range(3)]:
                    s0 = bn.sched_ns(srv.proc.pid)
                    for q in range(blk * per, (blk + 1) * per):
                        t0 = time.perf_counter()
                        ok = group(srv.conn, x, k, q)
                        acc[x]["lat"].append(time.perf_counter() - t0)
                        acc[x]["bad"] += 0 if ok else 1
                    acc[x]["cpu"] += bn.sched_ns(srv.proc.pid) - s0
            for x in arms:
                lat = acc[x]["lat"]
                out.append({"run": run, "k": k, "arm": x, "groups": len(lat), "bad": acc[x]["bad"],
                            "server_cpu_us": acc[x]["cpu"] / len(lat) / 1e3,
                            "mean_us": sum(lat) / len(lat) * 1e6,
                            "p0_us": min(lat) * 1e6, "p25_us": bn.pct(lat, 25) * 1e6,
                            "p50_us": bn.pct(lat, 50) * 1e6, "p75_us": bn.pct(lat, 75) * 1e6,
                            "p99_us": bn.pct(lat, 99) * 1e6,
                            "load": open("/proc/loadavg").read().split()[0]})
        srv.stop()
        json.dump(out, open(a.out, "w"))
        print(f"run {run+1}/{a.runs}", flush=True)
    print("ALL DONE", flush=True)


main()
