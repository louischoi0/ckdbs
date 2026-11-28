#!/usr/bin/env python3
"""EB's milestone-close measurement: the whole code change, base against close.

`instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` section 5 (EB-S6).
Arms are `LABEL=BINARY`: the commit EB opened from (`base`), EB's closing commit
(`close`), and `ctl`, a second server started from the base binary (the
in-run noise floor). Each arm is a copy of its binary (hashed) serving a fresh
data file, `cores = 1`, `durability = relaxed`, `join_order = planned`, one KWP
connection. The three servers are up together and never busy at once.

Pinning (the host is a P/E hybrid): the driver on CPU 1, an idle server's
threads parked on CPU 0, and the arm being measured moved onto one measuring
CPU for its block. The measuring CPU is the same for every arm of a repeat and
rotates across the P-cores 2..7 between repeats, so a slow CPU slot cannot
sit under one arm only. Arm order is a permutation of the three, rotated per
repeat (six permutations: --reps a multiple of 6 balances them).

Relations, --rows rows each (keys dense 1..N), the same in every arm:
  bt (id int64, v int64) BTREE                       tuple key id
  hp (k int64, v int64) HEAP                         hidden Keystone, keyidentity
  e  (a int32, b int16, v int64, PRIMARY KEY (a, b)) embedded key; a = (i-1)//1000, b = (i-1)%1000
  w  (k varchar(40), n int64, v int64, PRIMARY KEY (k, n))   hashed key; k = 'key-%06d', n = i % 1000
  bi (id int64, v int64) BTREE                       the insert target (bt stays unchanged)
  c  (id int64, a int64, b int64, v int64) BTREE, index c_ab (a, b), a = id % 10, b = id
  o  (id int64, x int64, a int64, b int64, ea int32, eb int16)   100 rows, the outer side

Cells: OH-* overhead (a point read and a point write by every key form, an issued-key
INSERT, an ON CONFLICT DO NOTHING that conflicts); G-* the four built arms (R1, J1, J2, O1).
Per cell and repeat, every arm runs the same statements from one seed (equal
work) and the per-statement row counts / write tags are compared across arms.
Recorded per arm: server CPU per statement (on-CPU ns over the server's threads,
/proc/<pid>/task/*/schedstat), client latency percentiles, the host's load per block.

Usage: eb_close.py --arm base=B0 --arm close=B1 --arm ctl=B0 --workdir D --out OUT.json
--status F [--rows 100000] [--reps 12] [--port 31960] [--cells PREFIX,...] [--scale 1.0]
       eb_close.py --render OUT.json     (markdown tables from a finished run)
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

DRIVER_CPU, PARK_CPU, MEASURE_CPUS = 1, 0, [2, 3, 4, 5, 6, 7]
PERMS = [(0, 1, 2), (2, 1, 0), (1, 2, 0), (0, 2, 1), (2, 0, 1), (1, 0, 2)]


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    busy = 0
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/comm") as f:
                    if f.read().strip() in ("cc1plus", "ctest", "ld", "cc1"):
                        busy += 1
            except OSError:
                pass
    return {"loadavg": load, "build_or_ctest_processes": busy}


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


def pin(pid, cpu):
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            os.sched_setaffinity(int(tid), {cpu})
        except OSError:
            pass


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p95_us": us(nearest_rank(s, 95)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def must(conn, sql, tag):
    got = conn.execute(sql)[2]
    if not got.startswith(tag):
        raise SystemExit(f"{sql[:90]!r} -> {got!r}")
    return got


class Arm:
    def __init__(self, a, label, source, port):
        self.label = label
        self.binary = os.path.join(a.workdir, f"kds_server.{label}")
        shutil.copy2(source, self.binary)
        self.sha = hashlib.sha256(open(self.binary, "rb").read()).hexdigest()
        self.mtime = os.path.getmtime(source)
        data = os.path.join(a.workdir, f"eb-{label}.db")
        if os.path.exists(data):
            os.remove(data)
        shutil.rmtree(data + ".wal", ignore_errors=True)
        conf = os.path.join(a.workdir, f"eb-{label}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = 1\ndurability = relaxed\n"
                    f"buffer_pool_frames = 131072\nlog_file = eb-{label}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\njoin_order = planned\n")
        self.err = open(os.path.join(a.workdir, f"eb-{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(PARK_CPU), self.binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=900)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"arm {label} did not listen on {port}")
                time.sleep(0.1)

    def stop(self):
        try:
            self.conn.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def load(arm, n, status):
    c = arm.conn
    for sql in [
        "CREATE TABLE bt (id int64, v int64) BTREE",
        "CREATE TABLE hp (k int64, v int64) HEAP",
        "CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))",
        "CREATE TABLE w (k varchar(40), n int64, v int64, PRIMARY KEY (k, n))",
        "CREATE TABLE bi (id int64, v int64) BTREE",
        "CREATE TABLE c (id int64, a int64, b int64, v int64) BTREE",
        "CREATE TABLE o (id int64, x int64, a int64, b int64, ea int32, eb int16) BTREE",
    ]:
        must(c, sql, "CREATE")
    for lo in range(1, n + 1, 1000):
        ids = range(lo, min(n, lo + 999) + 1)
        must(c, "INSERT INTO bt VALUES " + ",".join(f"({i}, {i})" for i in ids), "INSERT")
        must(c, "INSERT INTO bi VALUES " + ",".join(f"({i}, {i})" for i in ids), "INSERT")
        must(c, "INSERT INTO hp (k, v) VALUES " + ",".join(f"({i}, {i})" for i in ids), "INSERT")
        must(c, "INSERT INTO e VALUES " + ",".join(
            f"({(i - 1) // 1000}, {(i - 1) % 1000}, {i})" for i in ids), "INSERT")
        must(c, "INSERT INTO w VALUES " + ",".join(
            f"('key-{i:06d}', {i % 1000}, {i})" for i in ids), "INSERT")
        must(c, "INSERT INTO c VALUES " + ",".join(f"({i}, {i % 10}, {i}, {i})" for i in ids), "INSERT")
        if lo % 20_000 == 1:
            status(f"{arm.label}: loading {lo}/{n}")
    must(c, "CREATE INDEX c_ab ON c (a, b)", "CREATE")
    rng = random.Random(4099)
    mid = (n // 1000) // 2
    outer = []
    for j in range(1, 101):
        i = rng.randrange(1, n + 1)
        outer.append(f"({j}, {i}, {i % 10}, {i}, {mid}, {rng.randrange(1000)})")
    must(c, "INSERT INTO o VALUES " + ",".join(outer), "INSERT")


def cells(n):
    """(name, kind, statements a block, sql(rng)); kind 'ro' compares row counts, 'rw' write tags."""
    def key(r):
        return r.randrange(1, n + 1)

    def ea_eb(r):
        i = key(r)
        return (i - 1) // 1000, (i - 1) % 1000

    def wk(r):
        i = key(r)
        return f"k = 'key-{i:06d}' AND n = {i % 1000}"

    oh = [
        ("OH-bt-read", "ro", 12000, lambda r: f"SELECT v FROM bt WHERE id = {key(r)}"),
        ("OH-bt-write", "rw", 9000, lambda r: f"UPDATE bt SET v = v + 1 WHERE id = {key(r)}"),
        ("OH-hp-read", "ro", 12000, lambda r: f"SELECT v FROM hp WHERE keyidentity = {key(r)}"),
        ("OH-hp-write", "rw", 9000, lambda r: f"UPDATE hp SET v = v + 1 WHERE keyidentity = {key(r)}"),
        ("OH-e-read", "ro", 12000, lambda r: "SELECT v FROM e WHERE a = %d AND b = %d" % ea_eb(r)),
        ("OH-e-write", "rw", 9000, lambda r: "UPDATE e SET v = v + 1 WHERE a = %d AND b = %d" % ea_eb(r)),
        ("OH-w-read", "ro", 12000, lambda r: f"SELECT v FROM w WHERE {wk(r)}"),
        ("OH-w-write", "rw", 9000, lambda r: f"UPDATE w SET v = v + 1 WHERE {wk(r)}"),
        ("OH-bt-conflict", "rw", 9000,
         lambda r: f"INSERT INTO bt VALUES ({key(r)}, 0) ON CONFLICT DO NOTHING"),
        ("OH-bi-insert", "rw", 6000, lambda r: f"INSERT INTO bi (v) VALUES ({r.randrange(1 << 30)})"),
    ]
    g = [
        ("G-R1", "ro", 200, lambda r: (f"SELECT id, v FROM bt WHERE id >= {r.randrange(1, n - 200)} "
                                      "ORDER BY id LIMIT 100")),
        ("G-J1", "ro", 6, lambda r: "SELECT o.id, c.v FROM o JOIN c ON c.a = o.a WHERE c.b = o.b"),
        ("G-J2", "ro", 8, lambda r: "SELECT o.id, e.v FROM o JOIN e ON e.a = o.ea WHERE e.b = o.eb"),
        ("G-O1", "ro", 100, lambda r: "SELECT a, b, v FROM e ORDER BY a, b LIMIT 100"),
    ]
    return oh + g


def run_block(arm, sql_of, kind, count, seed):
    rng = random.Random(seed)
    lat, seen = [], []
    cpu0 = sched_ns(arm.proc.pid)
    for _ in range(count):
        sql = sql_of(rng)
        t0 = time.perf_counter()
        fields, rows, tag, _ = arm.conn.execute(sql)
        lat.append(time.perf_counter() - t0)
        seen.append(len(rows) if kind == "ro" else tag)
    return lat, sched_ns(arm.proc.pid) - cpu0, seen


def zlib_seed(name):
    return int(hashlib.sha256(name.encode()).hexdigest()[:6], 16)


TCRIT = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306,
         9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 17: 2.110, 23: 2.069}


def mean_ci(xs):
    n = len(xs)
    m = sum(xs) / n
    if n < 2:
        return m, 0.0
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    return m, TCRIT.get(n - 1, 2.0) * sd / math.sqrt(n)


def render(path):
    """Markdown tables from a result JSON: --render OUT.json (derived, no new measurement)."""
    d = json.load(open(path))
    print("| cell | base CPU/stmt | close CPU/stmt | ctl CPU/stmt | close vs base (paired, 95% CI) | ctl vs base (paired, 95% CI) | base / close / ctl server-CPU-bound throughput |")
    print("|---|---|---|---|---|---|---|")
    for c in d["cells"]:
        b, k, t = (c["arms"][x]["cpu_us_per_stmt_by_rep"] for x in ("base", "close", "ctl"))
        mb, cb = mean_ci(b)
        mk, ck = mean_ci(k)
        mt, ct = mean_ci(t)
        dk = mean_ci([(x / y - 1) * 100 for x, y in zip(k, b)])
        dt = mean_ci([(x / y - 1) * 100 for x, y in zip(t, b)])
        tp = lambda m: f"{1e6 / m:,.0f}/s"
        print(f"| {c['cell']} | {mb:.2f} +-{cb:.2f} us | {mk:.2f} +-{ck:.2f} us | {mt:.2f} +-{ct:.2f} us | "
              f"{dk[0]:+.1f} % +-{dk[1]:.1f} | {dt[0]:+.1f} % +-{dt[1]:.1f} | {tp(mb)} / {tp(mk)} / {tp(mt)} |")
    print()
    print("| cell | arm | n | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|---|")
    for c in d["cells"]:
        for arm in ("base", "close", "ctl"):
            l = c["arms"][arm]["latency"]
            print(f"| {c['cell']} | {arm} | {l['n']:,} | {l['p0_us']:,} us | {l['p25_us']:,} us | "
                  f"{l['p50_us']:,} us | {l['p95_us']:,} us | {l['p99_us']:,} us | {l['max_us']:,} us |")
    print()
    loads = [float(h["loadavg"].split()[0]) for c in d["cells"] for a in c["arms"].values() for h in a["host"]]
    busy = sum(h["build_or_ctest_processes"] for c in d["cells"] for a in c["arms"].values() for h in a["host"])
    print(f"load average (1 min) over {len(loads)} blocks: min {min(loads)}, max {max(loads)}, "
          f"mean {sum(loads) / len(loads):.2f}; build/ctest processes seen: {busy}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--render":
        return render(sys.argv[2])
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rows", type=int, default=100_000)
    ap.add_argument("--reps", type=int, default=18)
    ap.add_argument("--port", type=int, default=31960)
    ap.add_argument("--cells", default="")
    ap.add_argument("--scale", type=float, default=1.0, help="multiplies every block's statement count")
    a = ap.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {DRIVER_CPU})

    def status(text):
        with open(a.status, "w") as f:
            f.write(f"{time.time():.0f} {text}\n")

    arms = []
    result = {"rows": a.rows, "reps": a.reps, "scale": a.scale, "arms": {}, "cells": [],
              "host_start": host_state()}
    try:
        for k, spec in enumerate(a.arm):
            label, source = spec.split("=", 1)
            arm = Arm(a, label, source, a.port + k)
            arms.append(arm)
            t0 = time.time()
            load(arm, a.rows, status)
            result["arms"][label] = {"binary_sha256": arm.sha, "binary_mtime": arm.mtime,
                                     "load_s": round(time.time() - t0, 1)}
        time.sleep(1.5)
        wanted = [x for x in a.cells.split(",") if x]
        for name, kind, count, sql_of in cells(a.rows):
            if wanted and not any(name.startswith(w) for w in wanted):
                continue
            count = max(2, int(count * a.scale))
            cell = {"cell": name, "kind": kind, "statements_per_block": count, "arms": {},
                    "analyze": {}, "sql_example": sql_of(random.Random(1))}
            seed0 = zlib_seed(name)
            for arm in arms:
                pin(arm.proc.pid, MEASURE_CPUS[0])
                if name.startswith("G-"):
                    try:
                        rows = arm.conn.execute("ANALYZE " + sql_of(random.Random(1)))[1]
                        cell["analyze"][arm.label] = [
                            (r[0].decode() if isinstance(r[0], bytes) else str(r[0]))
                            for r in rows if r and r[0]][:30]
                    except kwp.KwpError as e:
                        cell["analyze"][arm.label] = "refused: " + str(e)[:200]
                run_block(arm, sql_of, kind, max(2, count // 5), 11)  # warm
                pin(arm.proc.pid, PARK_CPU)
            acc = {arm.label: {"lat": [], "cpu": [], "host": [], "seen": {}} for arm in arms}
            for rep in range(a.reps):
                cpu = MEASURE_CPUS[rep % len(MEASURE_CPUS)]
                for idx in PERMS[rep % 6]:
                    arm = arms[idx]
                    status(f"{name} rep={rep} arm={arm.label} cpu={cpu}")
                    acc[arm.label]["host"].append(host_state())
                    pin(arm.proc.pid, cpu)
                    lat, ns, seen = run_block(arm, sql_of, kind, count, seed0 + 7919 * (rep + 1))
                    pin(arm.proc.pid, PARK_CPU)
                    x = acc[arm.label]
                    x["lat"] += lat
                    x["cpu"].append(ns / count / 1e3)
                    x["seen"][rep] = seen
            for rep in range(a.reps):  # equal answers across arms, per statement
                ref = acc[arms[0].label]["seen"][rep]
                for arm in arms[1:]:
                    if acc[arm.label]["seen"][rep] != ref:
                        raise SystemExit(f"{name} rep {rep}: {arm.label} answered differently "
                                         f"from {arms[0].label}")
            cell["answer_sample"] = acc[arms[0].label]["seen"][0][:3]
            cell["answers_equal_across_arms"] = True
            for label, x in acc.items():
                cell["arms"][label] = {"statements": count * a.reps,
                                       "cpu_us_per_stmt_by_rep": [round(v, 3) for v in x["cpu"]],
                                       "latency": pcts(x["lat"]), "host": x["host"]}
            result["cells"].append(cell)
            with open(a.out, "w") as f:
                json.dump(result, f, indent=1)
    finally:
        for arm in arms:
            arm.stop()
    result["host_end"] = host_state()
    with open(a.out, "w") as f:
        json.dump(result, f, indent=1)
    status("done")


if __name__ == "__main__":
    main()
