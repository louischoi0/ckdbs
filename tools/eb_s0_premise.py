#!/usr/bin/env python3
"""EB-S0's premise cells: what each key-form family costs today, against
the cheapest statement or schema that returns the same rows through an arm
the engine already has.

`instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` section 5
(EB-S0) and EB-Q1 (a): a family whose cost today is under 2x its proxy's is
recorded and left. One binary (a copy, hashed), one server and a fresh data
file per row count, `cores = 1`, `durability = relaxed`, one connection.

Relations (n rows each, keys dense 1..n):
  bt (id int64, k int64, v int64)       btree; index bt_k (k), k a permutation of id
  hp (k int64, v int64) HEAP            hidden Keystone, keyidentity 1..n
  e  (a int32, b int16, v int64, PRIMARY KEY (a, b))   embedded key, b in 0..999
  c  (id int64, a int64, b int64, v int64)  index c_ab (a, b), a = id % 10
  c1 the twin of c with index c1_ba (b, a): the selective column leading
  o  (id int64, x int64, a int64, b int64, ea int32, eb int16)  100 rows, the outer side

Every cell is a pair: T, the shape as written today, and B, the proxy that
returns the same rows (asserted per statement by row count) through an arm
that exists. The unit is server CPU per statement (on-CPU ns summed over
the server's threads, /proc/<pid>/task/*/schedstat) and the client latency
percentiles; the arm order alternates per repeat. A third arm, T2, repeats
T for the noise floor.

Usage: eb_s0_premise.py --bin B --workdir D --out OUT.json --status F
[--rows 100000,1000000] [--reps 3] [--stmts 40] [--port 31991] [--arm-budget-s 3]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

KMAX = (1 << 48) - 1


def host_state():
    try:
        return open("/proc/loadavg").read().strip()
    except OSError:
        return "unavailable"


def sched_ns(pid):
    total = 0
    base = f"/proc/{pid}/task"
    for tid in os.listdir(base):
        try:
            with open(f"{base}/{tid}/schedstat") as f:
                total += int(f.read().split()[0])
        except OSError:
            pass
    return total


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p75_us": us(nearest_rank(s, 75)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def must(conn, sql, tag):
    got = conn.execute(sql)[2]
    if not got.startswith(tag):
        raise SystemExit(f"{sql[:80]!r} -> {got!r}")
    return got


def cells(n):
    """name -> (family, {arm: f(rng) -> sql}); T and B draw from equal seeds."""
    groups = n // 1000  # e.a values
    return {
        "R1 page forward, btree tuple key": ("S1", {
            "T": lambda r: (f"SELECT id, v FROM bt WHERE id >= {r.randrange(1, n - 200)} "
                            "ORDER BY id LIMIT 100"),
            "B": lambda r: (f"SELECT id, v FROM bt WHERE id >= {r.randrange(1, n - 200)} "
                            f"AND id <= {KMAX} ORDER BY id LIMIT 100")}),
        "R2 page backward, btree tuple key": ("S1", {
            "T": lambda r: (f"SELECT id, v FROM bt WHERE id < {r.randrange(200, n)} "
                            "ORDER BY id DESC LIMIT 20"),
            "B": lambda r: (f"SELECT id, v FROM bt WHERE id >= 1 AND id <= "
                            f"{r.randrange(200, n) - 1} ORDER BY id DESC LIMIT 20")}),
        "R3 page forward, heap Keystone": ("S1", {
            "T": lambda r: (f"SELECT keyidentity, v FROM hp WHERE keyidentity >= "
                            f"{r.randrange(1, n - 200)} ORDER BY keyidentity LIMIT 100"),
            "B": lambda r: (lambda x: "SELECT keyidentity, v FROM hp WHERE keyidentity IN (" +
                            ", ".join(str(x + i) for i in range(100)) +
                            ") ORDER BY keyidentity")(r.randrange(1, n - 200))}),
        "R3s strict window, heap Keystone": ("S1", {
            "T": lambda r: (lambda x: f"SELECT keyidentity, v FROM hp WHERE keyidentity > {x - 1} "
                            f"AND keyidentity < {x + 100}")(r.randrange(1, n - 200)),
            "B": lambda r: (lambda x: f"SELECT keyidentity, v FROM hp WHERE keyidentity >= {x} "
                            f"AND keyidentity <= {x + 99}")(r.randrange(1, n - 200))}),
        "R4s strict window, index leading key": ("S1", {
            "T": lambda r: (lambda x: f"SELECT id, k FROM bt WHERE k > {x - 1} AND k < {x + 100}")(
                r.randrange(1, n - 200)),
            "B": lambda r: (lambda x: f"SELECT id, k FROM bt WHERE k >= {x} AND k <= {x + 99}")(
                r.randrange(1, n - 200))}),
        "R4 page forward, index leading key": ("S1", {
            "T": lambda r: (f"SELECT id, k FROM bt WHERE k >= {r.randrange(1, n - 200)} "
                            "ORDER BY k LIMIT 100"),
            "B": lambda r: (lambda x: "SELECT id, k FROM bt WHERE k IN (" +
                            ", ".join(str(x + i) for i in range(100)) + ") ORDER BY k")(
                                r.randrange(1, n - 200))}),
        "J1 join by a whole composite index key": ("S2", {
            "T": lambda r: "SELECT o.id, c.v FROM o JOIN c ON c.a = o.a WHERE c.b = o.b",
            "B": lambda r: "SELECT o.id, c1.v FROM o JOIN c1 ON c1.b = o.b WHERE c1.a = o.a"}),
        "J2 join by a whole embedded key": ("S2", {
            "T": lambda r: "SELECT o.id, e.v FROM o JOIN e ON e.a = o.ea WHERE e.b = o.eb",
            "B": lambda r: "SELECT o.id, bt.v FROM o JOIN bt ON bt.id = o.x"}),
        "J3 embedded key, literal prefix and a joined column": ("S2", {
            "T": lambda r: (f"SELECT o.id, e.v FROM o JOIN e ON e.b = o.eb "
                            f"WHERE e.a = {groups // 2}"),
            "B": lambda r: "SELECT o.id, bt.v FROM o JOIN bt ON bt.id = o.x"}),
        "O1 order by an embedded key": ("S3", {
            "T": lambda r: "SELECT a, b, v FROM e ORDER BY a, b LIMIT 100",
            "B": lambda r: "SELECT id, v FROM bt ORDER BY id LIMIT 100"}),
    }


REFUSALS = [
    ("S4", "INSERT INTO c VALUES (1, 1, 1, 0) ON CONFLICT (a, b) DO NOTHING"),
    ("S4", "INSERT INTO e VALUES (1, 1, 0) ON CONFLICT (a, b) DO NOTHING"),
    ("S4", "INSERT INTO e VALUES (1, 1, 0) ON CONFLICT DO NOTHING"),
    ("S4", "INSERT INTO hp (keyidentity, k, v) VALUES (1, 1, 0) ON CONFLICT (keyidentity) "
           "DO NOTHING"),
]


def answer(conn, sql):
    try:
        fields, rows, tag, _ = conn.execute(sql)
        return f"{tag} ({len(rows)} rows)"
    except Exception as exc:  # the refusal is the datum
        return f"refused: {exc}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rows", default="100000,1000000")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--stmts", type=int, default=40)
    ap.add_argument("--port", type=int, default=31991)
    ap.add_argument("--arm-budget-s", type=float, default=3.0)
    ap.add_argument("--cells", default="", help="comma-separated cell name prefixes; all if empty")
    a = ap.parse_args()

    def status(text):
        with open(a.status, "w") as f:
            f.write(f"{time.time():.0f} {text}\n")

    os.makedirs(a.workdir, exist_ok=True)
    binary = os.path.join(a.workdir, "kds_server.measured")
    shutil.copy2(a.bin, binary)
    sha = hashlib.sha256(open(binary, "rb").read()).hexdigest()
    result = {"binary_sha256": sha, "binary_mtime": os.path.getmtime(a.bin),
              "reps": a.reps, "stmts": a.stmts, "runs": []}

    for n in [int(x) for x in a.rows.split(",")]:
        data = os.path.join(a.workdir, f"eb-{n}.db")
        for p in (data,):
            if os.path.exists(p):
                os.remove(p)
        shutil.rmtree(data + ".wal", ignore_errors=True)
        conf = os.path.join(a.workdir, f"eb-{n}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {a.port}\ncores = 1\ndurability = relaxed\n"
                    f"buffer_pool_frames = 262144\nlog_file = eb-{n}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\njoin_order = planned\n")
        err = open(os.path.join(a.workdir, f"eb-{n}.stderr"), "w")
        proc = subprocess.Popen(["taskset", "-c", "2", binary, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 30
            while True:
                try:
                    conn = kwp.Connection("127.0.0.1", a.port, timeout=600)
                    break
                except OSError:
                    if time.time() > deadline or proc.poll() is not None:
                        raise SystemExit("the server did not listen")
                    time.sleep(0.1)
            groups = n // 1000
            for sql in [
                "CREATE TABLE bt (id int64, k int64, v int64) BTREE",
                "CREATE TABLE hp (k int64, v int64) HEAP",
                "CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))",
                "CREATE TABLE c (id int64, a int64, b int64, v int64) BTREE",
                "CREATE TABLE c1 (id int64, a int64, b int64, v int64) BTREE",
                "CREATE TABLE o (id int64, x int64, a int64, b int64, ea int32, eb int16) BTREE",
            ]:
                must(conn, sql, "CREATE")
            perm = list(range(1, n + 1))
            random.Random(n).shuffle(perm)
            for lo in range(1, n + 1, 1000):
                hi = min(n, lo + 999)
                ids = range(lo, hi + 1)
                must(conn, "INSERT INTO bt VALUES " +
                     ",".join(f"({i}, {perm[i - 1]}, {i})" for i in ids), "INSERT")
                must(conn, "INSERT INTO hp (k, v) VALUES " +
                     ",".join(f"({i}, {i})" for i in ids), "INSERT")
                must(conn, "INSERT INTO e VALUES " +
                     ",".join(f"({(i - 1) // 1000}, {(i - 1) % 1000}, {i})" for i in ids), "INSERT")
                for t in ("c", "c1"):
                    must(conn, f"INSERT INTO {t} VALUES " +
                         ",".join(f"({i}, {i % 10}, {i}, {i})" for i in ids), "INSERT")
                if lo % 100_000 == 1:
                    status(f"n={n} loading {lo}/{n}")
            must(conn, "CREATE INDEX bt_k ON bt(k)", "CREATE")
            must(conn, "CREATE INDEX c_ab ON c(a, b)", "CREATE")
            must(conn, "CREATE INDEX c1_ba ON c1(b, a)", "CREATE")
            rng = random.Random(4099)
            mid = groups // 2
            outer = []
            for j in range(1, 101):
                i = rng.randrange(1, n + 1)
                outer.append(f"({j}, {i}, {i % 10}, {i}, {mid}, {rng.randrange(1000)})")
            must(conn, "INSERT INTO o VALUES " + ",".join(outer), "INSERT")
            run = {"rows": n, "cells": [], "refusals": [], "analyze": {}}
            for fam, sql in REFUSALS:
                run["refusals"].append({"family": fam, "sql": sql, "answer": answer(conn, sql)})
            table = cells(n)
            wanted = [c for c in a.cells.split(",") if c]
            for name, (fam, arms) in table.items():
                if wanted and not any(name.startswith(w + " ") for w in wanted):
                    continue
                status(f"n={n} cell={name}")
                arms = {"T": arms["T"], "B": arms["B"], "T2": arms["T"]}
                for arm in ("T", "B"):
                    probe = arms[arm](random.Random(1))
                    try:
                        plan = conn.execute("ANALYZE " + probe)
                        run["analyze"][f"{name} / {arm}"] = [
                            [kwp.render_value(f, v) for f, v in zip(plan[0], row)]
                            for row in plan[1]][:40]
                    except Exception as exc:
                        run["analyze"][f"{name} / {arm}"] = f"refused: {exc}"
                refused = None
                for arm in arms:  # warm: every page resident, every statement parsed once
                    try:
                        conn.execute(arms[arm](random.Random(1)))
                    except kwp.KwpError as exc:  # a refusal today is the cell's datum
                        refused = {"arm": arm, "error": str(exc)[:400]}
                        break
                if refused is not None:
                    run["cells"].append({"name": name, "family": fam, "refused": refused,
                                         "sql": {x: arms[x](random.Random(1))[:300]
                                                 for x in ("T", "B")}})
                    continue
                acc = {arm: {"lat": [], "ns": 0, "stmts": 0, "rows": 0, "load": []} for arm in arms}
                for rep in range(a.reps):
                    order = list(arms) if rep % 2 == 0 else list(reversed(list(arms)))
                    for arm in order:
                        acc[arm]["load"].append(host_state())
                        rng = random.Random(7919 * (rep + 1))  # one draw sequence for every arm
                        cpu0 = sched_ns(proc.pid)
                        counts = []
                        t_arm = time.perf_counter()
                        for done in range(a.stmts):
                            # A slow arm stops at the budget, never below 3 statements.
                            if done >= 3 and time.perf_counter() - t_arm > a.arm_budget_s:
                                break
                            sql = arms[arm](rng)
                            t0 = time.perf_counter()
                            rows = conn.execute(sql)[1]
                            acc[arm]["lat"].append(time.perf_counter() - t0)
                            counts.append(len(rows))
                        acc[arm]["ns"] += sched_ns(proc.pid) - cpu0
                        acc[arm]["stmts"] += len(counts)
                        acc[arm]["rows"] += sum(counts)
                if len({acc[x]["rows"] / acc[x]["stmts"] for x in acc}) != 1:
                    raise SystemExit(f"{name}: arms returned different row counts "
                                     f"{ {x: acc[x]['rows'] for x in acc} }")
                cell = {"name": name, "family": fam, "sql": {x: arms[x](random.Random(1))[:300]
                                                             for x in ("T", "B")}, "arms": {}}
                for arm, v in acc.items():
                    cell["arms"][arm] = {"server_cpu_us_per_stmt": round(v["ns"] / v["stmts"] / 1e3, 1),
                                         "rows_per_stmt": v["rows"] / v["stmts"],
                                         "latency": pcts(v["lat"]), "load": v["load"]}
                t, b = cell["arms"]["T"]["server_cpu_us_per_stmt"], cell["arms"]["B"]["server_cpu_us_per_stmt"]
                cell["ratio_T_over_B"] = round(t / b, 2) if b else None
                run["cells"].append(cell)
                result["runs_partial"] = run
                with open(a.out, "w") as f:
                    json.dump(result, f, indent=1)
            result.pop("runs_partial", None)
            result["runs"].append(run)
            conn.close()
        finally:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
            err.close()
        with open(a.out, "w") as f:
            json.dump(result, f, indent=1)
    status("done")


if __name__ == "__main__":
    main()
