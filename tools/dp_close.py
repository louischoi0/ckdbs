#!/usr/bin/env python3
"""DP-S4's close measurement (Q2-Q4): what reading through a SQL view costs.

`instructions/v3.2.0/workorder-dp-view.md` section 5 (DP-S4), DP-R10 and DP-R11.
Run against ONE binary (the DP build, B). The primary number is server CPU per
statement (`/proc/<pid>/task/*/schedstat` over the timed block, all server
threads, compile and run together), median of `--rounds` rounds; client wall
latency per statement is kept for the percentile tables. Every server runs
`cores = 1` with `relaxed` durability on a data file under `--workdir`.

  q2  a point read of a plain table on three servers up at once: N1 (no view
      has ever existed on the instance), N2 (the same, the same-binary noise
      floor) and V (one unrelated view exists, so every FROM name is asked as
      a view first). Server-to-CPU mapping rotated every round.
  q3  one server; per size, a point read by tuple key, a two-relation key
      join and a range (`--range` rows, capped at the size), each written by
      hand on table(s) T and through an equivalent absorbable view; H2 is the
      hand statement on an identical copy (the in-run floor).
  q4j the same chain where every level joins one more table (a (d+1)-way join
      once absorbed), m_d against the hand-written join, H1b the floor.
  q4  one server; a point read through a view chain of depth `--depths`
      (n1 over a table, nK over n(K-1)) against the same statement over the
      hand-written table (H) and a second identical copy (H2, the floor).

    dp_close.py --bin B --workdir D --out OUT.json [--q q2,q3,q4]
                [--sizes 200,1000,10000] [--rounds 24] [--k 2000]
                [--k-join 1000] [--k-range 100] [--range 1000]
                [--depths 1,2,4,8] [--port 15771] [--heartbeat F]

The binary must be a copy (bench/README.md rule 3); the driver hashes it.
"""
import argparse, hashlib, json, os, random, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import Server, cpu_ns, pin, lat_summary, host_state, load  # noqa: E402


def med(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if len(xs) % 2 else (xs[len(xs) // 2 - 1] + xs[len(xs) // 2]) / 2


def mk_table(c, name, n, seed=7):
    rng = random.Random(seed)
    c.execute(f"CREATE TABLE {name} (id int64, v int64, w int64) BTREE")
    vs = rng.sample(range(1, 4 * n + 1), n)
    load(c, name, [(2 * (i + 1), vs[i], i) for i in range(n)])


def timed(c, pid, stmts):
    lats = []
    t0 = cpu_ns(pid)
    for q in stmts:
        t = time.perf_counter()
        c.execute(q)
        lats.append(time.perf_counter() - t)
    return (cpu_ns(pid) - t0) / 1000.0 / len(stmts), lats


def summarize(cpu, lat, base):
    out = {}
    for x in cpu:
        out[x] = {"cpu_us_median": round(med(cpu[x]), 3), "cpu_us_min": round(min(cpu[x]), 3),
                  "cpu_us_max": round(max(cpu[x]), 3), "lat": lat_summary(lat[x])}
    for x in cpu:
        if x != base:
            d = [a - b for a, b in zip(cpu[x], cpu[base])]
            out[x]["delta_vs_" + base] = {"median_us": round(med(d), 3), "min_us": round(min(d), 3),
                                          "max_us": round(max(d), 3)}
    return out


def rotated(arms, r):
    o = arms[r % len(arms):] + arms[:r % len(arms)]
    return o[::-1] if r % 2 else o


def q2(a, beat):
    arms = ["N1", "N2", "V"]
    srv = {x: Server(a.bin, a.workdir, f"q2{x}", a.port + i, 1, [4 + i]) for i, x in enumerate(arms)}
    conns = {x: srv[x].conn() for x in arms}
    out = {}
    try:
        for x in arms:
            mk_table(conns[x], "other", 100)
        conns["V"].execute("CREATE VIEW unrelated AS SELECT id FROM other")
        for n in [int(s) for s in a.sizes.split(",")]:
            for x in arms:
                mk_table(conns[x], f"t{n}", n)
            rng = random.Random(n)
            stm = [f"SELECT * FROM t{n} WHERE id = {2 * rng.randrange(1, n + 1)}" for _ in range(a.k)]
            cpu = {x: [] for x in arms}
            lat = {x: [] for x in arms}
            host = host_state()
            for r in range(a.rounds):
                for i, x in enumerate(arms):
                    pin(srv[x].proc.pid, 4 + (i + r) % 3)
                for x in rotated(arms, r):
                    cc, l = timed(conns[x], srv[x].proc.pid, stm)
                    cpu[x].append(cc)
                    lat[x].extend(l)
            out[str(n)] = {"k": a.k, "rounds": a.rounds, "host_before": host, "host_after": host_state(),
                           "arms": summarize(cpu, lat, "N1"), "cpu_rounds": cpu}
            beat(f"q2 n={n} done")
        return out
    finally:
        for c in conns.values():
            c.close()
        for s in srv.values():
            s.stop()


def q3(a, beat):
    srv = Server(a.bin, a.workdir, "q3", a.port, 1, [4])
    c = srv.conn()
    pid = srv.proc.pid
    out = {}
    try:
        for n in [int(s) for s in a.sizes.split(",")]:
            for t in ("t", "t2", "b", "b2"):
                mk_table(c, f"{t}_{n}", n)
            c.execute(f"CREATE VIEW v1_{n} (id, v) AS SELECT id, v FROM t_{n}")
            c.execute(f"CREATE VIEW vj_{n} (id, av, bv) AS SELECT t_{n}.id, t_{n}.v, b_{n}.v "
                      f"FROM t_{n} JOIN b_{n} ON t_{n}.id = b_{n}.id")
            rng = random.Random(n)
            rg = min(a.range, n)
            shapes = {}
            ks = [2 * rng.randrange(1, n + 1) for _ in range(a.k)]
            shapes["point"] = {
                "H": [f"SELECT v FROM t_{n} WHERE id = {k}" for k in ks],
                "H2": [f"SELECT v FROM t2_{n} WHERE id = {k}" for k in ks],
                "VIEW": [f"SELECT v FROM v1_{n} WHERE id = {k}" for k in ks]}
            kj = ks[:a.k_join]
            shapes["join"] = {
                "H": [f"SELECT t_{n}.v, b_{n}.v FROM t_{n} JOIN b_{n} ON t_{n}.id = b_{n}.id WHERE t_{n}.id = {k}" for k in kj],
                "H2": [f"SELECT t2_{n}.v, b2_{n}.v FROM t2_{n} JOIN b2_{n} ON t2_{n}.id = b2_{n}.id WHERE t2_{n}.id = {k}" for k in kj],
                "VIEW": [f"SELECT av, bv FROM vj_{n} WHERE id = {k}" for k in kj]}
            los = [2 * rng.randrange(1, n - rg + 2) for _ in range(a.k_range)]
            shapes["range"] = {
                "H": [f"SELECT v FROM t_{n} WHERE id >= {lo} AND id < {lo + 2 * rg}" for lo in los],
                "H2": [f"SELECT v FROM t2_{n} WHERE id >= {lo} AND id < {lo + 2 * rg}" for lo in los],
                "VIEW": [f"SELECT v FROM v1_{n} WHERE id >= {lo} AND id < {lo + 2 * rg}" for lo in los]}
            for shape, stm in shapes.items():
                arms = ["H", "H2", "VIEW"]
                cpu = {x: [] for x in arms}
                lat = {x: [] for x in arms}
                host = host_state()
                for r in range(a.rounds):
                    for x in rotated(arms, r):
                        cc, l = timed(c, pid, stm[x])
                        cpu[x].append(cc)
                        lat[x].extend(l)
                rows = len(c.execute(stm["VIEW"][0])[1])
                out.setdefault(str(n), {})[shape] = {
                    "k": len(stm["H"]), "rounds": a.rounds, "rows_per_stmt": rows,
                    "host_before": host, "host_after": host_state(),
                    "arms": summarize(cpu, lat, "H"), "cpu_rounds": cpu,
                    "sample": {x: stm[x][0] for x in arms}}
                beat(f"q3 n={n} {shape} done")
        return out
    finally:
        c.close()
        srv.stop()


def q4(a, beat):
    srv = Server(a.bin, a.workdir, "q4", a.port, 1, [4])
    c = srv.conn()
    pid = srv.proc.pid
    n = 1000
    out = {}
    try:
        mk_table(c, "t", n)
        mk_table(c, "t2", n)
        depths = [int(d) for d in a.depths.split(",")]
        for d in range(1, max(depths) + 1):
            src = "t" if d == 1 else f"n{d - 1}"
            c.execute(f"CREATE VIEW n{d} (id, v) AS SELECT id, v FROM {src}")
        try:
            c.execute(f"CREATE VIEW n{max(depths) + 1} (id, v) AS SELECT id, v FROM n{max(depths)}")
            out["depth_past_bound"] = "accepted"
        except kwp.KwpError as e:
            out["depth_past_bound"] = f"{e.code:#x} {e.message}"[:200]
        rng = random.Random(4)
        ks = [2 * rng.randrange(1, n + 1) for _ in range(a.k)]
        stm = {"H": [f"SELECT v FROM t WHERE id = {k}" for k in ks],
               "H2": [f"SELECT v FROM t2 WHERE id = {k}" for k in ks]}
        for d in depths:
            stm[f"n{d}"] = [f"SELECT v FROM n{d} WHERE id = {k}" for k in ks]
        arms = list(stm)
        cpu = {x: [] for x in arms}
        lat = {x: [] for x in arms}
        host = host_state()
        for r in range(a.rounds):
            for x in rotated(arms, r):
                cc, l = timed(c, pid, stm[x])
                cpu[x].append(cc)
                lat[x].extend(l)
        out["depths"] = {"k": a.k, "rounds": a.rounds, "host_before": host, "host_after": host_state(),
                         "arms": summarize(cpu, lat, "H"), "cpu_rounds": cpu}
        beat("q4 done")
        return out
    finally:
        c.close()
        srv.stop()


def q4j(a, beat):
    """A chain whose every level joins one more table: mK = m(K-1) JOIN jK on the key
    (the absorbed block of depth d is a (d+1)-way join, the worst compile at the bound)."""
    srv = Server(a.bin, a.workdir, "q4j", a.port, 1, [4])
    c = srv.conn()
    pid = srv.proc.pid
    n = 1000
    out = {}
    try:
        depths = [int(d) for d in a.depths.split(",")]
        top = max(depths)
        for j in range(top + 1):
            mk_table(c, f"j{j}", n)
        mk_table(c, "j0b", n)
        c.execute("CREATE VIEW m1 (id, v) AS SELECT j0.id, j1.v FROM j0 JOIN j1 ON j0.id = j1.id")
        for d in range(2, top + 1):
            c.execute(f"CREATE VIEW m{d} (id, v) AS SELECT m{d - 1}.id, j{d}.v FROM m{d - 1} "
                      f"JOIN j{d} ON m{d - 1}.id = j{d}.id")
        rng = random.Random(5)
        ks = [2 * rng.randrange(1, n + 1) for _ in range(a.k_join)]

        def hand(d, k, first="j0"):
            joins = " ".join(f"JOIN j{i} ON {first}.id = j{i}.id" for i in range(1, d + 1))
            return f"SELECT j{d}.v FROM {first} {joins} WHERE {first}.id = {k}"
        stm = {}
        for d in depths:
            stm[f"H{d}"] = [hand(d, k) for k in ks]
            stm[f"m{d}"] = [f"SELECT v FROM m{d} WHERE id = {k}" for k in ks]
        stm["H1b"] = [hand(1, k, "j0b") for k in ks]
        arms = list(stm)
        cpu = {x: [] for x in arms}
        lat = {x: [] for x in arms}
        host = host_state()
        for r in range(a.rounds):
            for x in rotated(arms, r):
                cc, l = timed(c, pid, stm[x])
                cpu[x].append(cc)
                lat[x].extend(l)
        rows = {x: len(c.execute(stm[x][0])[1]) for x in arms}
        out["depths"] = {"k": a.k_join, "rounds": a.rounds, "host_before": host, "host_after": host_state(),
                         "rows_per_stmt": rows, "sample": {x: stm[x][0] for x in arms},
                         "arms": summarize(cpu, lat, "H1"), "cpu_rounds": cpu}
        beat("q4j done")
        return out
    finally:
        c.close()
        srv.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--q", default="q2,q3,q4,q4j")
    p.add_argument("--sizes", default="200,1000,10000")
    p.add_argument("--rounds", type=int, default=24)
    p.add_argument("--k", type=int, default=2000, help="point reads a block (q2, q3, q4)")
    p.add_argument("--k-join", type=int, default=1000)
    p.add_argument("--k-range", type=int, default=100)
    p.add_argument("--range", type=int, default=1000, help="rows a range statement returns, capped at the size")
    p.add_argument("--depths", default="1,2,4,8")
    p.add_argument("--port", type=int, default=15771)
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    try:
        os.sched_setaffinity(0, {0, 1, 2, 3})
    except OSError:
        pass

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    rep = {"bin": a.bin, "sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
           "host_before": host_state(), "args": vars(a)}
    beat("start")
    for q, fn in (("q2", q2), ("q3", q3), ("q4", q4), ("q4j", q4j)):
        if q in a.q.split(","):
            rep[q] = fn(a, beat)
            with open(a.out, "w") as f:
                json.dump(rep, f, indent=1)
    rep["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(rep, f, indent=1)
    beat("end")


if __name__ == "__main__":
    main()
