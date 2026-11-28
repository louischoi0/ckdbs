#!/usr/bin/env python3
"""EM-S0's premise (`instructions/v3.2.0/workorder-em-front-end-reuse-without-quiet-wrong.md`
section 6, EM-S0 (iii)): what the statement front end's repeated work costs.

One server, `cores = 1`, `relaxed` durability, the server pinned to one CPU and
the client to another. Per shape the same statements run up to three ways,
interleaved per round with the order rotating:

- `text`: the statement with its literals (`C_PARSE` of the unnamed statement,
  `C_BIND` with no parameter, `C_EXECUTE`) - `tools/kwp.py`'s `execute`;
- `prepared`: a named statement's `?` holes bound (`execute_prepared`);
- `named0`: the statement with its literals parsed under a name at every
  execution and bound with no parameter - a zero-hole `KeptParse` built and
  materialized per statement, the mechanism EM-Q8 (a) proposes for C1, priced
  as it stands today (it pays one more round trip than `text`; only the spike's
  parts are read from it).

Cells beside those: a text `SELECT` the query layer answers (EM-R7), a
qualified `ns1.t2` beside the same `t2` under `USE ns1`, with no view and then
with one view in the instance, and with 100 more relations; the join under a
two-variant administrator's plan; and a walked `UPDATE` long enough to yield.

Server CPU per statement is `/proc/<pid>/task/*/schedstat` over the timed
block, every server thread. The parts are read from the EM-S0 spike's counters
(`em_spike.hpp`, appended to `PING`'s reply); against a build without the
spike only the CPU and the latencies are taken.

    em_s0_premise.py --bin B --workdir D --out OUT.json [--rounds 8] [--k 2000]
                     [--n 10000] [--port 15883] [--server-cpu 2] [--client-cpu 5]
                     [--heartbeat F]
"""
import argparse, hashlib, json, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import Server, cpu_ns, pin, lat_summary, host_state  # noqa: E402

PARTS = ("parse", "fp", "args", "subst", "count", "compile", "ns", "keptof", "holes", "bind",
         "mat")

JOIN3 = ("SELECT a.v, b.v, cc.v FROM a JOIN b ON a.bid = b.id JOIN cc ON b.cid = cc.id "
         "WHERE a.id = {0}")

# shape -> (text, holes, paths)
SHAPES = {
    "point": ("SELECT * FROM t WHERE id = {0}", 1, ("text", "prepared", "named0")),
    "update": ("UPDATE t SET w = {0} WHERE id = {1}", 2, ("text", "prepared", "named0")),
    "insert": ("INSERT INTO ti VALUES ({0}, {1}, 0)", 2, ("text", "prepared", "named0")),
    "delete": ("DELETE FROM td WHERE id = {0}", 1, ("text", "prepared", "named0")),
    "join3": (JOIN3, 1, ("text", "prepared", "named0")),
    "qlayer": ("SELECT v * 2 FROM t WHERE id = {0}", 1, ("text",)),
    "nsq": ("SELECT * FROM ns1.t2 WHERE id = {0}", 1, ("text",)),
    "nsu": ("SELECT * FROM t2 WHERE id = {0}", 1, ("text",)),
}

PLAN = ("CREATE PLAN em_p FOR " + JOIN3.format(1) +
        " VARIANT lo WHEN ARG(1) <= 5000 ORDER (a, b, cc) VARIANT hi ELSE ORDER (a, b, cc)")


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


class Keys:
    """Unique keys for the insert and delete shapes across every cell."""

    def __init__(self, n):
        self.next_insert = 1
        self.next_delete = 1
        self.n_delete = n

    def take(self, shape):
        if shape == "insert":
            self.next_insert += 1
            return self.next_insert - 1
        self.next_delete += 1
        assert self.next_delete <= self.n_delete + 1, "td exhausted"
        return self.next_delete - 1


def setup(c, n, n_delete, n_walk):
    c.execute("CREATE TABLE t (id int64, v int64, w int64) BTREE")
    load_rows(c, "t", [(i, i * 7, 0) for i in range(1, n + 1)])
    c.execute("CREATE TABLE ti (id int64, v int64, w int64) BTREE")
    c.execute("CREATE TABLE td (id int64, v int64, w int64) BTREE")
    load_rows(c, "td", [(i, i, 0) for i in range(1, n_delete + 1)])
    c.execute("CREATE TABLE a (id int64, bid int64, v int64) BTREE")
    c.execute("CREATE TABLE b (id int64, cid int64, v int64) BTREE")
    c.execute("CREATE TABLE cc (id int64, v int64, w int64) BTREE")
    load_rows(c, "a", [(i, i, i) for i in range(1, n + 1)])
    load_rows(c, "b", [(i, i, i) for i in range(1, n + 1)])
    load_rows(c, "cc", [(i, i, i) for i in range(1, n + 1)])
    c.execute("CREATE NAMESPACE ns1")
    c.execute("CREATE TABLE ns1.t2 (id int64, v int64, w int64) BTREE")
    load_rows(c, "ns1.t2", [(i, i * 7, 0) for i in range(1, n + 1)])
    c.execute("CREATE TABLE tw (id int64, v int64, w int64) BTREE")
    load_rows(c, "tw", [(i, i, 0) for i in range(1, n_walk + 1)])


def args_for(shape, rng, n, i, keys):
    if shape == "update":
        return (i, rng.randrange(1, n + 1))
    if shape == "insert":
        key = keys.take(shape)
        return (key, key)
    if shape == "delete":
        return (keys.take(shape),)
    return (rng.randrange(1, n + 1),)


def run_block(c, shape, path, work):
    text, holes, _ = SHAPES[shape]
    lats = []
    if shape == "nsu":
        c.execute("USE ns1")
    if path == "text":
        for a in work:
            q = text.format(*a)
            t = time.perf_counter()
            c.execute(q)
            lats.append(time.perf_counter() - t)
    elif path == "prepared":
        for a in work:
            params = [kwp.Connection.int64(x) for x in a]
            t = time.perf_counter()
            c.execute_prepared(shape, params)
            lats.append(time.perf_counter() - t)
    else:  # named0
        for a in work:
            q = text.format(*a)
            t = time.perf_counter()
            c.prepare("z0", q)
            c.execute_prepared("z0", [])
            lats.append(time.perf_counter() - t)
    if shape == "nsu":
        c.execute("USE public")
    return lats


def cell(c, pid, shape, path, k, n, seed, keys):
    rng = random.Random(seed)
    work = [args_for(shape, rng, n, i, keys) for i in range(k)]
    before = spike(c)
    t0 = cpu_ns(pid)
    lats = run_block(c, shape, path, work)
    cpu_us = (cpu_ns(pid) - t0) / 1000.0 / k
    after = spike(c)
    parts = {}
    for p in PARTS:
        if p + "_ns" in after:
            parts[p + "_us"] = (after[p + "_ns"] - before[p + "_ns"]) / 1000.0 / k
            parts[p + "_calls"] = (after[p + "_calls"] - before[p + "_calls"]) / k
    return {"cpu_us": cpu_us, "lat": lat_summary(lats), "host": host_state(), **parts}


def walk_cell(c, pid, k):
    """A walked UPDATE over `tw` (no key in its WHERE), `k` statements."""
    before = spike(c)
    t0 = cpu_ns(pid)
    lats = []
    for i in range(k):
        t = time.perf_counter()
        c.execute(f"UPDATE tw SET w = {i} WHERE v >= 0")
        lats.append(time.perf_counter() - t)
    cpu_us = (cpu_ns(pid) - t0) / 1000.0 / k
    after = spike(c)
    parts = {}
    for p in PARTS:
        if p + "_ns" in after:
            parts[p + "_us"] = (after[p + "_ns"] - before[p + "_ns"]) / 1000.0 / k
            parts[p + "_calls"] = (after[p + "_calls"] - before[p + "_calls"]) / k
    return {"cpu_us": cpu_us, "lat": lat_summary(lats), "host": host_state(), **parts}


def summarize(cells):
    keys = [k for k in cells[0] if k not in ("lat", "host")]
    out = {k: round(med([x[k] for x in cells]), 3) for k in keys}
    out["cpu_us_min"] = round(min(x["cpu_us"] for x in cells), 3)
    out["cpu_us_max"] = round(max(x["cpu_us"] for x in cells), 3)
    out["lat_p50_med"] = round(med([x["lat"]["p50"] for x in cells]), 1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--k", type=int, default=2000)
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--walk-rows", type=int, default=60000)
    ap.add_argument("--walk-k", type=int, default=10)
    ap.add_argument("--port", type=int, default=15883)
    ap.add_argument("--server-cpu", type=int, default=2)
    ap.add_argument("--client-cpu", type=int, default=5)
    ap.add_argument("--heartbeat")
    a = ap.parse_args()

    def beat(line):
        if a.heartbeat:
            with open(a.heartbeat, "w") as f:
                f.write(f"{time.time():.0f} {line}\n")

    os.sched_setaffinity(0, {a.client_cpu})
    with open(a.bin, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    meta = {"bin": a.bin, "sha256": sha, "rounds": a.rounds, "k": a.k, "n": a.n,
            "walk_rows": a.walk_rows, "walk_k": a.walk_k, "host_before": host_state(),
            "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())}
    beat("starting")
    srv = Server(a.bin, a.workdir, "em-s0", a.port, 1, [a.server_cpu])
    pin(srv.proc.pid, a.server_cpu)
    pid = srv.proc.pid
    # Every phase's cells: (phase, shape, path) -> [cell per round].
    results = {}
    # Phase 1 runs every shape; phase 2 the namespace pair with a view in the
    # instance; phase 3 the pair with 100 more relations; phase 4 the join
    # under a two-variant plan; the walked UPDATE last.
    n_delete = a.k * a.rounds * 3 + 3 * 300 + 1000  # three paths, their warm-ups, a margin
    keys = Keys(n_delete)
    try:
        c = srv.conn()
        beat("setup")
        setup(c, a.n, n_delete, a.walk_rows)
        for s, (text, holes, paths) in SHAPES.items():
            if "prepared" in paths:
                c.prepare(s, text.format(*(["?"] * holes)))

        def phase(name, shapes, k):
            for s in shapes:  # warm-up, every path
                for path in SHAPES[s][2]:
                    cell(c, pid, s, path, 300, a.n, 99, keys)
            for r in range(a.rounds):
                for s in shapes:
                    paths = list(SHAPES[s][2])
                    order = paths[r % len(paths):] + paths[:r % len(paths)]
                    for path in order:
                        beat(f"{name} round {r} {s} {path}")
                        results.setdefault(f"{name}/{s}/{path}", []).append(
                            cell(c, pid, s, path, k, a.n, 1000 * r + len(s), keys))

        phase("base", ["point", "update", "insert", "delete", "join3", "qlayer", "nsq", "nsu"],
              a.k)
        c.execute("CREATE VIEW em_v AS SELECT id, v FROM t")
        phase("view", ["nsq", "nsu"], a.k)
        for i in range(100):
            c.execute(f"CREATE TABLE pad{i} (id int64, v int64) BTREE")
        phase("pad100", ["nsq", "nsu"], a.k)
        c.execute(PLAN)
        phase("plan", ["join3"], a.k)
        beat("walk")
        walk_cell(c, pid, 2)  # warm-up
        results["walk/update/text"] = [walk_cell(c, pid, a.walk_k) for _ in range(3)]
        c.close()
    finally:
        srv.stop()
    meta["host_after"] = host_state()
    meta["finished"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    summary = {key: summarize(cells) for key, cells in results.items()}
    with open(a.out, "w") as f:
        json.dump({"meta": meta, "summary": summary, "rounds": results}, f, indent=1)
    beat("done")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
