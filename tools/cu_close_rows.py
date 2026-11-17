#!/usr/bin/env python3
"""CU close, question 1's supplement: the per-row tests CU-R5 names.

`cu_close_overhead.py`'s shapes carry no boolean conjunct and record no
trail, so of the tests CU-S1 restated under CU-R5 they reach one, the test
per run of a lookup or probe step. This driver runs the statements that reach
the others, on the same three arms (A, B, A2 = a copy of A), `cores = 1`,
relaxed, one connection each, one relation `t1000`
(`id int64, v int64, w int64, u int64`, row i = (i, i, i, i), an index on
`w`), blocks rotated in arm order:

  scan-in2      SELECT * FROM t1000 WHERE u IN (k, k2)    a list on a column
                nothing serves: a walk on every arm, the boolean conjunct
                asked of each of the 1,000 rows
  scan-or       SELECT * FROM t1000 WHERE u = k OR w = k2  an OR over two
                columns, which is no list: the same walk
  update-in2    UPDATE t1000 SET v = v + 1 WHERE u IN (k, k2)   the write
                path's boolean conjunct, a walked write on every arm
  pk-trail      SELECT * FROM t1000 WHERE id = k, after `WAYSTONE ON` for
                the shape: each of the 1,000 keys is asked three times a
                run, so a trail is recorded once and replayed twice

Usage: cu_close_rows.py --arm A=bin --arm B=bin --arm A2=bin --workdir D
       --port P --out OUT.json --progress FILE [--runs 10] [--blocks 6]
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp, bn_census as bn  # noqa: E402,F401

N = 1000
OPS = {"scan-in2": 1200, "scan-or": 1200, "pk-trail": 3000, "update-in2": 1200}


def unit(shape, q, conn):
    k = 1 + (q * 7919) % N          # 7919 is prime to 1,000: every key in turn
    k2 = 1 + (k + 498) % N          # never k
    if shape == "scan-in2":
        return len(conn.execute(f"SELECT * FROM t{N} WHERE u IN ({k}, {k2})")[1]) == 2
    if shape == "scan-or":
        return len(conn.execute(f"SELECT * FROM t{N} WHERE u = {k} OR w = {k2}")[1]) == 2
    if shape == "pk-trail":
        return len(conn.execute(f"SELECT * FROM t{N} WHERE id = {k}")[1]) == 1
    if shape == "update-in2":
        return conn.execute(f"UPDATE t{N} SET v = v + 1 WHERE u IN ({k}, {k2})")[2] == "UPDATED 2"
    raise SystemExit(f"unknown shape {shape}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = [tuple(s.split("=", 1)) for s in a.arm]
    results, notes = [], []
    for run in range(a.runs):
        srvs = [bn.Server(a, lab, b, a.port + k, f"s{run}") for k, (lab, b) in enumerate(arms)]
        for s in srvs:
            bn.load(s.conn, f"t{N}", N, cols="id int64, v int64, w int64, u int64",
                    row=lambda i: f"({i}, {i}, {i}, {i})")
            bn.must(s.conn, f"CREATE INDEX t{N}_w ON t{N} (w)", "CREATE")
            tag = s.conn.execute(f"WAYSTONE ON SELECT * FROM t{N} WHERE id = 1")[2]
            if run == 0:
                notes.append({"arm": s.label, "waystone_on": tag})
        time.sleep(1.5)
        for shape, cnt in OPS.items():
            accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srvs]
            per = cnt // a.blocks
            for blk in range(a.blocks):
                qs = range(blk * per, (blk + 1) * per)
                for j in [(k + run + blk) % len(srvs) for k in range(len(srvs))]:
                    s, acc = srvs[j], accs[j]
                    s0, w0 = bn.sched_ns(s.proc.pid), time.perf_counter()
                    for q in qs:
                        t0 = time.perf_counter()
                        ok = unit(shape, q, s.conn)
                        acc["lat"].append(time.perf_counter() - t0)
                        acc["bad"] += 0 if ok else 1
                    acc["wall"] += time.perf_counter() - w0
                    acc["server_ns"] += bn.sched_ns(s.proc.pid) - s0
                open(a.progress, "w").write(str(time.time()))
            for j, s in enumerate(srvs):
                r = bn.summarize(shape, "literal", s.label, accs[j])
                r["p75_us"] = bn.pct(accs[j]["lat"], 75) * 1e6
                r["max_us"] = max(accs[j]["lat"]) * 1e6
                r.update(run=run, rows=N, load=open("/proc/loadavg").read().split()[0],
                         host=bn.host_state())
                results.append(r)
            if run == 0:
                for s in srvs:  # what each arm ran, once
                    sql = {"scan-in2": f"SELECT * FROM t{N} WHERE u IN (7, 505)",
                           "scan-or": f"SELECT * FROM t{N} WHERE u = 7 OR w = 505",
                           "pk-trail": f"SELECT * FROM t{N} WHERE id = 7",
                           "update-in2": None}[shape]
                    if sql:
                        rows = s.conn.execute("ANALYZE " + sql)[1]
                        notes.append({"arm": s.label, "shape": shape, "analyze": [
                            " | ".join(v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray))
                                       else str(v) for v in row)[:240] for row in rows]})
            json.dump({"cells": results, "notes": notes}, open(a.out, "w"))
        for s in srvs:
            s.stop()
        print(f"run {run+1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}", flush=True)
    open(a.progress, "w").write(str(time.time()))
    print("ALL DONE", flush=True)


if __name__ == "__main__":
    main()
