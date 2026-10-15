#!/usr/bin/env python3
"""range100 A / A' / B + a same-session PostgreSQL twin, one size per invocation.

    r100_ab.py --arm A=BIN --arm Ap=BIN --arm B=BIN --pg-port 15434 --rows N
               --workdir DIR --ops 400 --blocks 20 --run R --out OUT.json

Relation `be_r (id int64, v int64, w int64) BTREE`, rows (i, i, i), i = 1..N,
loaded with named ascending keys (BK's be_r layout). Fresh server and data file
per arm per invocation (cores = 1, relaxed, buffer_pool_frames = 65536), all
KDS servers pinned to --pin-server, the PG backend to --pin-backend, this
driver to --pin-driver. Each cell is cut into --blocks blocks; every block runs
the same statements on every arm (KDS A, A', B and PG), the order of the arms
rotating per block and run. Per statement: wall time; per arm per block: the
server's on-CPU time (sum of /proc/<pid>/task/*/schedstat).
"""
import argparse, hashlib, json, os, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "..", "tools"))
import bn_census as bn  # noqa: E402
from pg_wire import PgConnection  # noqa: E402
from bb_overhead_benchmark import host_state  # noqa: E402

SHAPES = {
    "range100": "SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id >= {lo} AND id < {hi1}",
    "win100-rows": "SELECT * FROM be_r WHERE id >= {lo} AND id < {hi1}",
    "between100": "SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id BETWEEN {lo} AND {hi}",
}
EXPECT_ROWS = {"range100": 1, "win100-rows": 100, "between100": 1}


def sql_for(shape, lo):
    return SHAPES[shape].format(lo=lo, hi=lo + 99, hi1=lo + 100)


def lo_of(q, n):
    return 1 + (q * 7919) % max(1, n - 100)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--pg-port", type=int, required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--ops", type=int, default=400)
    ap.add_argument("--blocks", type=int, default=20)
    ap.add_argument("--run", type=int, default=0)
    ap.add_argument("--base-port", type=int, default=16100)
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--pin-backend", default="2")
    ap.add_argument("--progress", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    a.cores, a.durability = 1, "relaxed"
    os.sched_setaffinity(0, {int(a.pin_driver)})
    n = a.rows
    prog = lambda s: open(a.progress, "w").write(f"{time.time()} n={n} run={a.run} {s}\n")

    srvs = []
    for i, spec in enumerate(a.arm):
        label, binary = spec.split("=", 1)
        srvs.append(bn.Server(a, label, binary, a.base_port + i, f"n{n}r{a.run}"))
    pg = PgConnection(port=a.pg_port, database="bench", application_name="r100_ab.py")
    pgpid = pg.backend_pid
    subprocess.run(["taskset", "-p", "-c", a.pin_backend, str(pgpid)], check=True, capture_output=True)

    prog("load")
    for s in srvs:
        bn.load(s.conn, "be_r", n)
    pg.send_command("DROP TABLE IF EXISTS be_r")
    pg.send_command("CREATE TABLE be_r (id bigint PRIMARY KEY, v bigint, w bigint)")
    pg.send_command(f"INSERT INTO be_r SELECT i, i, i FROM generate_series(1, {n}) i")
    pg.send_command("VACUUM (ANALYZE) be_r")

    out = {"rows": n, "run": a.run, "args": vars(a), "plans": {}, "verify": {}, "cells": [],
           "host_before": host_state(), "pids": {s.label: s.proc.pid for s in srvs}, "pg_pid": pgpid}
    # plans (first run only is enough, but cheap) ----------------------------
    for shape in SHAPES:
        q = sql_for(shape, 5)
        for s in srvs:
            _, rows, tag, _ = s.conn.execute("ANALYZE " + q)
            out["plans"][f"{shape}:{s.label}"] = [r[0].decode() if r and r[0] else "" for r in rows] if rows else tag
        r, e = pg.fetch("EXPLAIN " + q)
        out["plans"][f"{shape}:PG"] = [x[0].decode() for x in r] if not e else "ERR " + e

    # verify: identical rows across KDS arms on edge + spread lows ------------
    lows = sorted({1, n - 99, 2, n // 2} | {lo_of(q, n) for q in range(1, 21)})
    bad = 0
    for shape in SHAPES:
        for lo in lows:
            q = sql_for(shape, lo)
            res = [s.conn.execute(q)[1] for s in srvs]
            if any(r != res[0] for r in res) or len(res[0]) != EXPECT_ROWS[shape]:
                bad += 1
            tag, prow, _, err = pg.query(q)
            if err or prow != EXPECT_ROWS[shape]:
                bad += 1
    out["verify"] = {"statements": len(lows) * len(SHAPES), "mismatches": bad}
    print("verify", out["verify"], flush=True)

    labels = [s.label for s in srvs] + ["PG"]
    per = a.ops // a.blocks
    for shape in SHAPES:
        for q in range(1, 4):
            for s in srvs:
                s.conn.execute(sql_for(shape, lo_of(q, n)))
            pg.query(sql_for(shape, lo_of(q, n)))
        acc = {l: {"lat": [], "cpu_ns": 0, "bad": 0, "block_us": []} for l in labels}
        c0 = bn.cpu_times()
        host0 = host_state()
        qn = 0
        for b in range(a.blocks):
            qs = list(range(qn + 1, qn + per + 1))
            qn += per
            prog(f"{shape} block {b}/{a.blocks}")
            order = labels[(b + a.run) % len(labels):] + labels[:(b + a.run) % len(labels)]
            for l in order:
                A = acc[l]
                if l == "PG":
                    pid, run = pgpid, None
                else:
                    srv = next(s for s in srvs if s.label == l)
                    pid = srv.proc.pid
                s0 = bn.sched_ns(pid)
                for qq in qs:
                    text = sql_for(shape, lo_of(qq, n))
                    t0 = time.perf_counter()
                    if l == "PG":
                        tag, rows, _, err = pg.query(text)
                        ok = err is None and rows == EXPECT_ROWS[shape]
                    else:
                        _, rws, tag, _ = srv.conn.execute(text)
                        ok = len(rws) == EXPECT_ROWS[shape]
                    A["lat"].append(time.perf_counter() - t0)
                    A["bad"] += 0 if ok else 1
                d = bn.sched_ns(pid) - s0
                A["cpu_ns"] += d
                A["block_us"].append(round(d / per / 1e3, 2))
        busy = bn.cpu_busy_pct(c0, bn.cpu_times())
        cell = {"shape": shape, "rows": n, "host_before": host0, "host_after": host_state(),
                "cpu_busy_pct": busy, "arms": {}}
        for l in labels:
            A = acc[l]
            lat = A["lat"]
            cell["arms"][l] = {"ops": len(lat), "bad": A["bad"],
                               "p0_us": min(lat) * 1e6, "p25_us": bn.pct(lat, 25) * 1e6,
                               "p50_us": bn.pct(lat, 50) * 1e6, "p95_us": bn.pct(lat, 95) * 1e6,
                               "p99_us": bn.pct(lat, 99) * 1e6,
                               "server_cpu_us": A["cpu_ns"] / len(lat) / 1e3,
                               "block_server_us": A["block_us"]}
        out["cells"].append(cell)
        print(shape, n, {l: (round(v["p50_us"], 1), round(v["server_cpu_us"], 1)) for l, v in cell["arms"].items()}, flush=True)
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)
    out["host_after"] = host_state()
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    pg.send_command("DROP TABLE be_r")
    pg.close()
    for s in srvs:
        s.stop()
    prog("done")


if __name__ == "__main__":
    main()
