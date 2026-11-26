#!/usr/bin/env python3
"""DU's close: the milestone's whole code change, base against closing commit,
on the writes DU serves and on the writes it does not.

`instructions/v3.2.0/workorder-du-keyed-write.md` section 5 (DU-S4). Arms are
`LABEL=BINARY`, two of them: the commit DU opened from and the commit that
closes it. Each arm is one server (a copy of its binary, hashed) on a fresh
data file, `cores = 1`, `durability = relaxed`, both pinned to one CPU and
never busy at once; the driver is pinned to another. One connection an arm.

Two relations an arm, the same in both:

  s   1,000 rows `(id, owner, v)`, `owner = id % 100`, an index on `owner`:
      the statements whose fixed cost DU could have moved
  b   --rows rows `(id, o10, o1k, o10k, o50k, v)`, each `oN` a column whose
      values each hold N rows spread over the relation (`id % (rows / N)`),
      each indexed: the statements DU finds by a key

Shapes, each one statement a unit, the rows it writes checked every time:

  point        UPDATE s SET v = v + 1 WHERE id = K
  list3        UPDATE s SET v = v + 1 WHERE id IN (K, K2, K3)
  walk-small   UPDATE s SET v = v + 1 WHERE v = -1          (no arm: a walk of
               1,000 rows that writes none)
  delete-miss  DELETE FROM s WHERE id = <no such key>
  read-point   SELECT v FROM s WHERE id = K                 (the control: no
               write path is on it)
  idx-10 .. idx-50k   UPDATE b SET v = v + 1 WHERE oN = K   (walked at the
               base; through the index at the close)
  range-1k     UPDATE b SET v = v + 1 WHERE id BETWEEN A AND A + 999
  pinned-50k   UPDATE b SET v = v - 1 WHERE o50k = K        (the close arm
               holds a plan pinning this shape to SCAN; the base arm, which
               has no such plan, walks it as it walks every write)

Per shape: --reps repeats, the arm order rotated per repeat, a fixed count of
statements an arm a repeat (equal work). Recorded per arm: the statement's
latency percentiles, the server's CPU a statement (on-CPU ns summed over the
server's threads, /proc/<pid>/task/*/schedstat), and the host's load before
the block. `ANALYZE` of each large shape is kept once per arm where the arm
answers it.

Usage: du_close.py --arm base=B0 --arm close=B1 --workdir D --out OUT.json
--status F [--rows 1000000] [--reps 3] [--port 31980]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

SMALL = 1000
PER_KEY = [10, 1000, 10_000, 50_000]


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-c", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": load, "busy_processes": busy}


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
        raise SystemExit(f"{sql[:90]!r} -> {got!r}")
    return got


class Arm:
    def __init__(self, a, label, source, port):
        self.label = label
        self.binary = os.path.join(a.workdir, f"kds_server.{label}")
        shutil.copy2(source, self.binary)
        self.sha = hashlib.sha256(open(self.binary, "rb").read()).hexdigest()
        self.mtime = os.path.getmtime(source)
        data = os.path.join(a.workdir, f"du-{label}.db")
        if os.path.exists(data):
            os.remove(data)
        shutil.rmtree(data + ".wal", ignore_errors=True)
        conf = os.path.join(a.workdir, f"du-{label}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = 1\ndurability = relaxed\n"
                    f"buffer_pool_frames = 131072\nlog_file = du-{label}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\n")
        self.err = open(os.path.join(a.workdir, f"du-{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", "2", self.binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=600)
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


def load(arm, rows, status):
    c = arm.conn
    must(c, "CREATE TABLE s (id int64, owner int64, v int64) BTREE", "CREATE")
    for lo in range(1, SMALL + 1, 500):
        must(c, "INSERT INTO s VALUES " +
             ",".join(f"({i}, {i % 100}, 0)" for i in range(lo, lo + 500)), "INSERT")
    must(c, "CREATE INDEX s_owner ON s (owner)", "CREATE")
    must(c, "CREATE TABLE b (id int64, o10 int64, o1k int64, o10k int64, o50k int64, v int64) "
            "BTREE", "CREATE")
    for lo in range(1, rows + 1, 1000):
        hi = min(rows, lo + 999)
        must(c, "INSERT INTO b VALUES " + ",".join(
            "(" + ", ".join([str(i)] + [str(i % (rows // n)) for n in PER_KEY] + ["0"]) + ")"
            for i in range(lo, hi + 1)), "INSERT")
        if lo % 100_000 == 1:
            status(f"{arm.label}: loading {lo}/{rows}")
    for n in PER_KEY:
        col = col_of(n)
        must(c, f"CREATE INDEX b_{col} ON b ({col})", "CREATE")
    # The plan the pinned shape runs under, where the arm takes one (DU-S3):
    # the base arm refuses a write's plan, and that refusal is kept.
    try:
        got = c.execute("CREATE PLAN du_pinned FOR UPDATE b SET v = v - 1 WHERE o50k = 1 "
                        "VARIANT walk ELSE ORDER (b) ACCESS b SCAN")[2]
    except kwp.KwpError as e:
        got = "refused: " + str(e)[:120]
    return got


def col_of(n):
    return {10: "o10", 1000: "o1k", 10_000: "o10k", 50_000: "o50k"}[n]


def shapes(rows):
    """(name, statements a block, sql(rng), expected rows or None for a read)."""
    out = [
        ("point", 3000, lambda r: f"UPDATE s SET v = v + 1 WHERE id = {r.randrange(1, SMALL + 1)}", 1),
        ("list3", 2000, lambda r: "UPDATE s SET v = v + 1 WHERE id IN (" +
         ", ".join(str(k) for k in r.sample(range(1, SMALL + 1), 3)) + ")", 3),
        ("walk-small", 1000, lambda r: "UPDATE s SET v = v + 1 WHERE v = -1", 0),
        ("delete-miss", 3000, lambda r: f"DELETE FROM s WHERE id = {SMALL + 1 + r.randrange(1000)}", 0),
        ("read-point", 3000, lambda r: f"SELECT v FROM s WHERE id = {r.randrange(1, SMALL + 1)}", None),
    ]
    for n in PER_KEY:
        col = col_of(n)
        out.append((f"idx-{n}", 12,
                    lambda r, col=col, n=n: f"UPDATE b SET v = v + 1 WHERE {col} = {r.randrange(rows // n)}",
                    n))
    out.append(("range-1k", 12,
                lambda r: (lambda a: f"UPDATE b SET v = v + 1 WHERE id BETWEEN {a} AND {a + 999}")(
                    r.randrange(1, rows - 1000)), 1000))
    out.append(("pinned-50k", 12,
                lambda r: f"UPDATE b SET v = v - 1 WHERE o50k = {r.randrange(rows // 50_000)}", 50_000))
    return out


def run_block(arm, sql_of, expected, count, rng):
    lat = []
    cpu0 = sched_ns(arm.proc.pid)
    for _ in range(count):
        sql = sql_of(rng)
        t0 = time.perf_counter()
        reply = arm.conn.execute(sql)
        lat.append(time.perf_counter() - t0)
        if expected is not None:
            wrote = int(reply[2].split()[1])
            if wrote != expected:
                raise SystemExit(f"{arm.label}: {sql[:80]!r} wrote {wrote}, not {expected}")
    return lat, sched_ns(arm.proc.pid) - cpu0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--port", type=int, default=31980)
    a = ap.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {4})

    def status(text):
        with open(a.status, "w") as f:
            f.write(f"{time.time():.0f} {text}\n")

    arms = []
    result = {"rows": a.rows, "reps": a.reps, "arms": {}, "shapes": []}
    try:
        for k, spec in enumerate(a.arm):
            label, source = spec.split("=", 1)
            arm = Arm(a, label, source, a.port + k)
            arms.append(arm)
            plan = load(arm, a.rows, status)
            result["arms"][label] = {"binary_sha256": arm.sha, "binary_mtime": arm.mtime,
                                     "create_plan": plan, "analyze": {}}
        time.sleep(1.5)
        for name, count, sql_of, expected in shapes(a.rows):
            # A warm block an arm, and the arm's own word for the shape.
            for arm in arms:
                run_block(arm, sql_of, expected, max(2, count // 6), random.Random(11))
                if name.startswith(("idx-", "range-", "pinned-")):
                    try:
                        rows = arm.conn.execute("ANALYZE " + sql_of(random.Random(12)))[1]
                        said = " | ".join(r[0].decode() for r in rows if r and r[0])
                    except kwp.KwpError as e:
                        said = "refused: " + str(e)[:160]
                    result["arms"][arm.label]["analyze"][name] = said
            acc = {arm.label: {"lat": [], "server_ns": 0, "n": 0, "host": []} for arm in arms}
            for rep in range(a.reps):
                order = arms[rep % len(arms):] + arms[:rep % len(arms)]
                for arm in order:
                    status(f"{name} rep={rep} arm={arm.label}")
                    acc[arm.label]["host"].append(host_state())
                    lat, ns = run_block(arm, sql_of, expected, count,
                                        random.Random(7919 * (rep + 1)))
                    acc[arm.label]["lat"] += lat
                    acc[arm.label]["server_ns"] += ns
                    acc[arm.label]["n"] += count
            cell = {"shape": name, "statements_per_block": count, "rows_per_statement": expected,
                    "arms": {}}
            for label, x in acc.items():
                cell["arms"][label] = {
                    "statements": x["n"],
                    "server_cpu_us_per_statement": round(x["server_ns"] / x["n"] / 1e3, 2),
                    "latency": pcts(x["lat"]), "host": x["host"]}
            result["shapes"].append(cell)
            with open(a.out, "w") as f:
                json.dump(result, f, indent=1)
    finally:
        for arm in arms:
            arm.stop()
    status("done")


if __name__ == "__main__":
    main()
