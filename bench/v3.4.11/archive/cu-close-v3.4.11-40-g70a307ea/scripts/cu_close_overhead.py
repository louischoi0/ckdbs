#!/usr/bin/env python3
"""CU close, question 1 (CU-R5) and the writes: what CU costs a statement with
no value list, and what a write by key costs once it descends.

Three arms on three servers (labels given by --arm LABEL=BINARY; the run uses
A, B and A2 = a byte-identical copy of A), cores = 1, relaxed, one connection
each. Every server loads `t1000` and `t100000`
(`id int64, v int64, w int64, u int64`, row i = (i, i, i, i)) with an index on
`w`. Blocks rotate arm order; per unit: client latency; per block: the
server's on-CPU ns (/proc/<pid>/task/*/schedstat, as bn_census does).

Off the set's shape (no list conjunct in the statement; CU-R5):

  select-pk     SELECT * FROM t WHERE id = k
  select-range  SELECT * FROM t WHERE id >= k AND id < k + 10
  select-idx    SELECT * FROM t WHERE w = k               (index probe)
  select-scan   SELECT * FROM t WHERE u = k               (walk; 1,000 rows only)
  select-join   SELECT a.id, b.v FROM t1000 AS a JOIN t100000 AS b ON b.id = a.w
                WHERE a.id >= k AND a.id < k + 10         (ten inner probes)
  update-pk     UPDATE t SET v = v + 1 WHERE id = k       (CU-S3b reads its key
                off the compiled WHERE)
  update-scan   UPDATE t SET v = v + 1 WHERE u = k        (a walked write, which
                now asks both recognisers; 1,000 rows only)
  ins-del       INSERT one row under a fresh key; DELETE ... WHERE id = it

On the shapes CU moved (A walks every leaf under a write latch, B descends):

  update-conj   UPDATE t SET v = v + 1 WHERE id = k AND u >= 0
  update-in4    UPDATE t SET v = v + 1 WHERE id IN (4 distinct keys)
  update-in64   the same, 64 keys
  ins-del-in4   INSERT four rows under fresh keys; DELETE ... WHERE id IN (them)

Usage: cu_close_overhead.py --arm A=bin --arm B=bin --arm A2=bin --workdir D
       --out OUT.json --progress FILE [--runs 5] [--blocks 6] [--scale 1.0]
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp, bn_census as bn  # noqa: E402,F401

SIZES = [1000, 100000]
COLS = "id int64, v int64, w int64, u int64"
# units per (shape, rows) per run, before --scale. A walked write of 100,000
# rows is milliseconds a unit on arm A, so its count is small.
OPS = {
    "select-pk": {1000: 3000, 100000: 3000},
    "select-range": {1000: 3000, 100000: 3000},
    "select-idx": {1000: 3000, 100000: 3000},
    "select-scan": {1000: 1200},
    "select-join": {1000: 1200},
    "update-pk": {1000: 3000, 100000: 3000},
    "update-scan": {1000: 1200},
    "update-conj": {1000: 1200, 100000: 120},
    "update-in4": {1000: 1200, 100000: 120},
    "update-in64": {1000: 600, 100000: 120},
    # The deletes leave their slots delete-marked, so a relation grows while
    # these two run; they come last, and `ins-del-in4`, whose arm A walks,
    # before `ins-del`, which descends on every arm.
    "ins-del-in4": {1000: 240, 100000: 120},
    "ins-del": {1000: 1500, 100000: 1500},
}


def unit(shape, n, q, conn):
    t = f"t{n}"
    key = lambda j: 1 + ((q * 64 + j) * 7919) % n  # 7919 is prime to both sizes
    lo = 1 + (q * 7919) % (n - 10)
    if shape == "select-pk":
        return len(conn.execute(f"SELECT * FROM {t} WHERE id = {key(0)}")[1]) == 1
    if shape == "select-range":
        return len(conn.execute(f"SELECT * FROM {t} WHERE id >= {lo} AND id < {lo + 10}")[1]) == 10
    if shape == "select-idx":
        return len(conn.execute(f"SELECT * FROM {t} WHERE w = {key(0)}")[1]) == 1
    if shape == "select-scan":
        return len(conn.execute(f"SELECT * FROM {t} WHERE u = {key(0)}")[1]) == 1
    if shape == "select-join":
        return len(conn.execute(
            f"SELECT a.id, b.v FROM t1000 AS a JOIN t100000 AS b ON b.id = a.w "
            f"WHERE a.id >= {lo} AND a.id < {lo + 10}")[1]) == 10
    if shape == "update-pk":
        return conn.execute(f"UPDATE {t} SET v = v + 1 WHERE id = {key(0)}")[2].startswith("UPDATED 1")
    if shape == "update-scan":
        return conn.execute(f"UPDATE {t} SET v = v + 1 WHERE u = {key(0)}")[2].startswith("UPDATED 1")
    if shape == "update-conj":
        return conn.execute(
            f"UPDATE {t} SET v = v + 1 WHERE id = {key(0)} AND u >= 0")[2].startswith("UPDATED 1")
    if shape in ("update-in4", "update-in64"):
        k = 4 if shape == "update-in4" else 64
        keys = ", ".join(str(key(j)) for j in range(k))  # 64 consecutive multiples: distinct
        return conn.execute(
            f"UPDATE {t} SET v = v + 1 WHERE id IN ({keys})")[2].startswith(f"UPDATED {k}")
    # Fresh keys above the loaded range: a deleted key stays bound, so no
    # unit names one twice. `ins-del` takes q-th key of its band, `ins-del-in4`
    # four of another band.
    if shape == "ins-del":
        k = 2 * n + q
        ok = conn.execute(f"INSERT INTO {t} VALUES ({k}, 0, {k}, {k})")[2].startswith("INSERT")
        return conn.execute(f"DELETE FROM {t} WHERE id = {k}")[2].startswith("DELETED 1") and ok
    if shape == "ins-del-in4":
        ks = [4 * n + 4 * q + j for j in range(4)]
        ok = conn.execute(f"INSERT INTO {t} VALUES " +
                          ", ".join(f"({k}, 0, {k}, {k})" for k in ks))[2].startswith("INSERT")
        return conn.execute(f"DELETE FROM {t} WHERE id IN ({', '.join(map(str, ks))})")[2] \
            .startswith("DELETED 4") and ok
    raise SystemExit(f"unknown shape {shape}")


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
    ap.add_argument("--port", type=int, default=31980)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--blocks", type=int, default=6)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = [tuple(s.split("=", 1)) for s in a.arm]
    results = []
    for run in range(a.runs):
        srvs = [bn.Server(a, lab, b, a.port + k, f"r{run}") for k, (lab, b) in enumerate(arms)]
        for s in srvs:
            for n in SIZES:
                bn.load(s.conn, f"t{n}", n, cols=COLS, row=lambda i: f"({i}, {i}, {i}, {i})")
                bn.must(s.conn, f"CREATE INDEX t{n}_w ON t{n} (w)", "CREATE")
            open(a.progress, "w").write(str(time.time()))
        time.sleep(1.5)
        for shape, sizes in OPS.items():
            for n, base in sizes.items():
                cnt = max(a.blocks, int(base * a.scale))
                accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srvs]
                per = cnt // a.blocks
                for blk in range(a.blocks):
                    # A unit index is never reused within a server: a later run
                    # is a fresh server, and a block's range follows the last.
                    qs = list(range(blk * per, (blk + 1) * per))
                    for j in [(k + run + blk) % len(srvs) for k in range(len(srvs))]:
                        block(srvs[j], shape, n, qs, accs[j])
                    open(a.progress, "w").write(str(time.time()))
                for j, s in enumerate(srvs):
                    r = bn.summarize(shape, "literal", s.label, accs[j])
                    r["p75_us"] = bn.pct(accs[j]["lat"], 75) * 1e6
                    r["max_us"] = max(accs[j]["lat"]) * 1e6
                    r.update(run=run, rows=n, load=open("/proc/loadavg").read().split()[0])
                    results.append(r)
                json.dump(results, open(a.out, "w"))
        for s in srvs:
            s.stop()
        print(f"run {run+1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}", flush=True)
    open(a.progress, "w").write(str(time.time()))
    print("ALL DONE", flush=True)


main()
