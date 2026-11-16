#!/usr/bin/env python3
"""Audit F4 overhead A/B (scratch driver; imports tools/bn_census.py and tools/kwp.py unmodified).

Per run: one fresh server per arm (cores=1, relaxed, CPU 2), same tables in each, one busy at a time,
blocks rotate order. Cell 1 (uncontended, no open transaction): UPDATE ... WHERE w = k (non-key scan,
1 row matches), point UPDATE WHERE id = k, at 200 / 1000 / 10000 rows. Cell 2: a second session holds
an open transaction that updated every row (v = -5); the measured session runs an autocommit
UPDATE/DELETE whose WHERE (v = -1) fails on the page version AND the snapshot version -> 0 rows both."""
import argparse, json, os, struct, sys, time
sys.path.insert(0, os.environ["KDS_TOOLS"])
import kwp, bn_census as bn
from bb_overhead_benchmark import host_state

SIZES = [200, 1000, 10000]


def run_cell(srvs, shape, n, blocks, run):
    accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srvs]
    per = n // blocks
    for blk in range(blocks):
        qs = list(range(blk * per, (blk + 1) * per))
        for j in [(k + run + blk) % len(srvs) for k in range(len(srvs))]:
            bn.run_block(srvs[j], shape, "literal", qs, accs[j])
    out = []
    for j, s in enumerate(srvs):
        r = bn.summarize(shape[0], "literal", s.label, accs[j])
        r["p75_us"] = bn.pct(accs[j]["lat"], 75) * 1e6
        r["run"] = run
        r["load"] = host_state()["loadavg"]
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31970)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = [tuple(s.split("=", 1)) for s in a.arm]
    i64 = kwp.Connection.int64
    results = []
    for run in range(a.runs):
        srvs = [bn.Server(a, lab, b, a.port + k, f"r{run}") for k, (lab, b) in enumerate(arms)]
        for s in srvs:
            for n in SIZES:
                bn.load(s.conn, f"t{n}", n)
                bn.load(s.conn, f"u{n}", n)   # cell 2 update table
                bn.load(s.conn, f"d{n}", n)   # cell 2 delete table
        time.sleep(1.5)
        # ---- cell 1: uncontended
        ops = {200: 1500, 1000: 600, 10000: 120}
        for n in SIZES:
            sc = lambda q, n=n: [i64(1 + (q * 7919) % n)]
            shapes = [
                (f"c1-scan-update-{n}", f"UPDATE t{n} SET v = v + 1 WHERE w = ?", sc, "UPDATED 1", "scan"),
                (f"c1-point-update-{n}", f"UPDATE t{n} SET v = v + 1 WHERE id = ?", sc, "UPDATED 1", "point"),
            ]
            for sh in shapes:
                cnt = int((3000 if "point" in sh[0] else ops[n]) * a.scale)
                for r in run_cell(srvs, sh, cnt, 6, run):
                    r["cell"] = 1
                    r["rows"] = n
                    results.append(r)
        # ---- cell 2: open transaction over every row, the 2nd session scans
        c2ops = {200: 600, 1000: 300, 10000: 60}
        for n in SIZES:
            for kind in ("update", "delete"):
                tbl = f"u{n}" if kind == "update" else f"d{n}"
                holders = []
                for k, s in enumerate(srvs):
                    h = kwp.Connection("127.0.0.1", a.port + k, timeout=120)
                    h.begin()
                    _, _, got, _ = h.execute(f"UPDATE {tbl} SET v = -5")
                    if not got.startswith(f"UPDATED {n}"):
                        raise SystemExit(f"holder: {got}")
                    holders.append(h)
                t_open = time.time()
                sql = (f"UPDATE {tbl} SET v = 0 WHERE v = -1" if kind == "update"
                       else f"DELETE FROM {tbl} WHERE v = -1")
                tag = "UPDATED 0" if kind == "update" else "DELETED 0"
                sh = (f"c2-{kind}-{n}", sql, lambda q: [], tag, "scan")
                for r in run_cell(srvs, sh, int(c2ops[n] * a.scale), 6, run):
                    r["cell"] = 2
                    r["rows"] = n
                    results.append(r)
                el = time.time() - t_open
                if el > 45:
                    raise SystemExit(f"cell 2 {kind} {n} took {el:.0f}s: near the 60 s txn ceiling")
                for h in holders:
                    _, rows, _, _ = h.execute(f"SELECT v FROM {tbl} WHERE id = 1")
                    if not rows or struct.unpack("<q", rows[0][0])[0] != -5:
                        raise SystemExit("holder txn gone or rolled back (ceiling?)")
                    h.rollback()
                    h.close()
        for s in srvs:
            s.stop()
        json.dump(results, open(a.out, "w"))
        open(a.progress, "a").write(f"{time.strftime('%H:%M:%S', time.gmtime())} run {run+1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}\n")
    json.dump(results, open(a.out, "w"))
    open(a.progress, "a").write("ALL DONE\n")


main()
