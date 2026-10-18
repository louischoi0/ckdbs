#!/usr/bin/env python3
"""Catalog-ceiling A/B (scratch; imports tools/bn_census.py and kwp.py unmodified).
Arms LABEL=BINARY. Fresh server per arm per run, pinned to one CPU, one busy at a time,
blocks rotate order. Shapes: create (8 x int64 tables), insert (2-col btree), select-pk x3 sizes."""
import argparse, json, os, sys, time, types
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-catalog-page-ceiling/tools")
import kwp, bn_census as bn

COLS8 = ", ".join(f"c{i} int64" for i in range(8))
SIZES = [200, 1000, 10000]


def timed_block(srv, label_shape, sqls, tag, acc, check=None):
    conn = srv.conn
    s0, c0, w0 = bn.sched_ns(srv.proc.pid), bn.self_ns(), time.perf_counter()
    for sql in sqls:
        t0 = time.perf_counter()
        _, rows, got, _ = conn.execute(sql)
        acc["lat"].append(time.perf_counter() - t0)
        if not got.startswith(tag) or (check and len(rows) != check):
            acc["bad"] += 1
            acc.setdefault("first_bad", f"{sql!r} -> {got!r}")
    acc["wall"] += time.perf_counter() - w0
    acc["server_ns"] += bn.sched_ns(srv.proc.pid) - s0
    acc["driver_ns"] += bn.self_ns() - c0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31970)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--blocks", type=int, default=10)
    ap.add_argument("--creates", type=int, default=500)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--sel-ops", type=int, default=2000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    a = ap.parse_args()
    a.pin_server, a.cores, a.durability = "2", 1, "relaxed"
    os.sched_setaffinity(0, {4})
    arms = [tuple(s.split("=", 1)) for s in a.arm]
    results = []
    for run in range(a.runs):
        srv = []
        for k, (lab, b) in enumerate(arms):
            s = bn.Server(a, lab, b, a.port + k, f"r{run}")
            for n in SIZES:
                bn.load(s.conn, f"t{n}", n, cols="id int64, v int64", row=lambda i: f"({i}, {i})")
            bn.must(s.conn, "CREATE TABLE ins (id int64, v int64) BTREE", "CREATE")
            srv.append(s)
        time.sleep(1.5)
        shapes = [("create", a.creates, "CREATE")]
        shapes += [("insert", a.ops, "INSERT")]
        shapes += [(f"select-{n}", a.sel_ops, "") for n in SIZES]
        for name, n, tag in shapes:
            load = open("/proc/loadavg").read().split()[0]
            accs = [{"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0} for _ in srv]
            per = n // a.blocks
            for blk in range(a.blocks):
                qs = range(blk * per, (blk + 1) * per)
                order = [(j + run + blk) % len(srv) for j in range(len(srv))]
                for j in order:
                    if name == "create":
                        sqls = [f"CREATE TABLE ct{q} ({COLS8}) BTREE" for q in qs]
                    elif name == "insert":
                        sqls = [f"INSERT INTO ins VALUES ({10_000_000 + q}, {q})" for q in qs]
                    else:
                        sz = int(name.split("-")[1])
                        sqls = [f"SELECT * FROM t{sz} WHERE id = {1 + (q * 7919) % sz}" for q in qs]
                    timed_block(srv[j], name, sqls, tag, accs[j], check=1 if tag == "" else None)
            for j, s in enumerate(srv):
                r = bn.summarize(name, "literal", s.label, accs[j])
                r["run"] = run
                r["load"] = load
                r["first_bad"] = accs[j].get("first_bad")
                results.append(r)
        # catalog state after: SHOW META not needed
        for s in srv:
            s.stop()
        with open(a.progress, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S', time.gmtime())} run {run + 1}/{a.runs} done load={open('/proc/loadavg').read().split()[0]}\n")
        json.dump(results, open(a.out, "w"))
    with open(a.progress, "a") as f:
        f.write("DONE\n")


main()
