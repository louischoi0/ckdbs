#!/usr/bin/env python3
"""EA-S0's premise (`instructions/v3.2.0/workorder-ea-prepared-statement-reuse.md`
section 5): what the parse of a prepared statement costs the server.

One server, `cores = 1`, `relaxed` durability. Per shape - a point `SELECT`, a
point `UPDATE`, a three-relation join by key - the same statements run two
ways, interleaved per round with the order alternating: `text` sends the
statement with its literal (`PARSE` of the text, `BIND` with no parameters,
`EXECUTE`), `prepared` binds a named statement's `?` holes (`execute_prepared`).

Server CPU per statement is `/proc/<pid>/task/*/schedstat` over the timed
block. The parts - parse, the `C_PARSE` fingerprint lex, `ArgumentsOf`, the
`C_BIND` substitution, the placeholder count, `exec::Compile` - are read from
the EA-S0 spike's counters (a build carrying `include/kds/base/ea_spike.hpp`
answers them after `PING`); against a build without the spike only the CPU
and the latencies are taken.

    ea_s0_premise.py --bin B --workdir D --out OUT.json [--rounds 8] [--k 2000]
                     [--n 10000] [--port 15881] [--server-cpu 2] [--client-cpu 5]
                     [--heartbeat F]
"""
import argparse, hashlib, json, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import Server, cpu_ns, pin, lat_summary, host_state  # noqa: E402

PARTS = ("parse", "fp", "args", "subst", "count", "compile")


def med(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if len(xs) % 2 else (xs[len(xs) // 2 - 1] + xs[len(xs) // 2]) / 2


def spike(c):
    _, _, tag, _ = c.execute("PING")
    got = dict(re.findall(r"(\w+)=(\d+)", tag))
    return {k: int(v) for k, v in got.items()}


def load_rows(c, table, rows):
    for i in range(0, len(rows), 500):
        c.execute(f"INSERT INTO {table} VALUES " +
                  ",".join("(" + ",".join(str(x) for x in r) + ")" for r in rows[i:i + 500]))


def setup(c, n):
    c.execute("CREATE TABLE t (id int64, v int64, w int64) BTREE")
    load_rows(c, "t", [(i, i * 7, 0) for i in range(1, n + 1)])
    c.execute("CREATE TABLE a (id int64, bid int64, v int64) BTREE")
    c.execute("CREATE TABLE b (id int64, cid int64, v int64) BTREE")
    c.execute("CREATE TABLE cc (id int64, v int64, w int64) BTREE")
    load_rows(c, "a", [(i, i, i) for i in range(1, n + 1)])
    load_rows(c, "b", [(i, i, i) for i in range(1, n + 1)])
    load_rows(c, "cc", [(i, i, i) for i in range(1, n + 1)])


SHAPES = {
    "point": ("SELECT * FROM t WHERE id = {0}", 1),
    "update": ("UPDATE t SET w = {0} WHERE id = {1}", 2),
    "join3": ("SELECT a.v, b.v, cc.v FROM a JOIN b ON a.bid = b.id JOIN cc ON b.cid = cc.id "
              "WHERE a.id = {0}", 1),
}


def args_for(shape, rng, n, i):
    if shape == "update":
        return (i, rng.randrange(1, n + 1))
    return (rng.randrange(1, n + 1),)


def cell(c, pid, shape, path, k, n, seed):
    text, _ = SHAPES[shape]
    rng = random.Random(seed)
    work = [args_for(shape, rng, n, i) for i in range(k)]
    before_spike = spike(c)
    lats = []
    t0 = cpu_ns(pid)
    if path == "text":
        for a in work:
            q = text.format(*a)
            t = time.perf_counter()
            c.execute(q)
            lats.append(time.perf_counter() - t)
    else:
        for a in work:
            params = [kwp.Connection.int64(x) for x in a]
            t = time.perf_counter()
            c.execute_prepared(shape, params)
            lats.append(time.perf_counter() - t)
    cpu_us = (cpu_ns(pid) - t0) / 1000.0 / k
    after_spike = spike(c)
    parts = {}
    for p in PARTS:
        if p + "_ns" in after_spike:
            parts[p + "_us"] = (after_spike[p + "_ns"] - before_spike[p + "_ns"]) / 1000.0 / k
            parts[p + "_calls"] = (after_spike[p + "_calls"] - before_spike[p + "_calls"]) / k
    return {"cpu_us": cpu_us, "lat": lat_summary(lats), "host": host_state(), **parts}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--k", type=int, default=2000)
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--port", type=int, default=15881)
    ap.add_argument("--server-cpu", type=int, default=2)
    ap.add_argument("--client-cpu", type=int, default=5)
    ap.add_argument("--heartbeat")
    ap.add_argument("--shapes", default="point,update,join3")
    a = ap.parse_args()

    def beat(line):
        if a.heartbeat:
            with open(a.heartbeat, "w") as f:
                f.write(f"{time.time():.0f} {line}\n")

    os.sched_setaffinity(0, {a.client_cpu})
    with open(a.bin, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    shapes = a.shapes.split(",")
    meta = {"bin": a.bin, "sha256": sha, "rounds": a.rounds, "k": a.k, "n": a.n,
            "host_before": host_state(), "started": time.strftime("%Y-%m-%d %H:%M:%S UTC",
                                                                  time.gmtime())}
    beat("starting")
    srv = Server(a.bin, a.workdir, "ea-s0", a.port, 1, [a.server_cpu])
    pin(srv.proc.pid, a.server_cpu)
    results = {s: {"text": [], "prepared": []} for s in shapes}
    try:
        c = srv.conn()
        setup(c, a.n)
        for s in shapes:
            c.prepare(s, SHAPES[s][0].format(*(["?"] * SHAPES[s][1])))
        for s in shapes:  # warm-up, both paths
            cell(c, srv.proc.pid, s, "text", 500, a.n, 99)
            cell(c, srv.proc.pid, s, "prepared", 500, a.n, 98)
        for r in range(a.rounds):
            for s in shapes:
                order = ("text", "prepared") if r % 2 == 0 else ("prepared", "text")
                for path in order:
                    beat(f"round {r} {s} {path}")
                    results[s][path].append(cell(c, srv.proc.pid, s, path, a.k, a.n,
                                                 1000 * r + hash(s) % 997))
        c.close()
    finally:
        srv.stop()
    meta["host_after"] = host_state()
    meta["finished"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    summary = {}
    for s in shapes:
        summary[s] = {}
        for path in ("text", "prepared"):
            cells = results[s][path]
            keys = [k for k in cells[0] if k not in ("lat", "host")]
            summary[s][path] = {k: round(med([x[k] for x in cells]), 3) for k in keys}
            summary[s][path]["lat_p50_med"] = round(med([x["lat"]["p50"] for x in cells]), 1)
    with open(a.out, "w") as f:
        json.dump({"meta": meta, "summary": summary, "rounds": results}, f, indent=1)
    beat("done")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
