#!/usr/bin/env python3
"""CI-S4 arm A: a writer's cost with no build running, on a relation with one secondary index.

    ci_close_arm_a.py --bin BINARY --workdir DIR --rows N --ops K --out OUT.json

Fresh server and data file; `t (id int64, c1, c2, c3 int64) BTREE` with `CREATE INDEX ix ON t (c1)`
and N preloaded rows (500-row INSERTs). Then, serially on one connection, K statements each of:
  ins   autocommit single-row INSERT (new ids N+1..N+K, scrambled c1)
  upd2  UPDATE t SET c2 = c2 + 1 WHERE id = k      (k uniform 1..N; the indexed column untouched)
  upd1  UPDATE t SET c1 = c1 + 1 WHERE id = k      (the indexed column changes: index maintenance)
Raw latencies and the wal_append_lsn range per phase go to OUT.json. Server pinned to --pin-server, driver to --pin-driver.
"""
import argparse, json, os, random, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402


def wal_bytes(d):
    t = 0
    for r, _, fs in os.walk(d):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(r, f))
            except OSError:
                pass
    return t


def lsn(c):
    _, rows, _, _ = c.execute("SHOW META")
    for r in rows:
        if bytes(r[1]) == b"wal_append_lsn":
            return int(bytes(r[2]))
    return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--ops", type=int, default=8000)
    ap.add_argument("--port", type=int, default=31980)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--pin-server", default="2-5")
    ap.add_argument("--pin-driver", default="6,7")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {int(x) for x in a.pin_driver.split(",")})
    data = os.path.join(a.workdir, "a.db")
    if os.path.exists(data):
        os.remove(data)
    shutil.rmtree(data + ".wal", ignore_errors=True)
    conf = os.path.join(a.workdir, "a.conf")
    open(conf, "w").write(
        f"data_file = {data}\nport = {a.port}\ncores = {a.cores}\ndurability = {a.durability}\n"
        f"buffer_pool_frames = 65536\nlog_file = a.log\nlog_dir = {a.workdir}\nlog_level = warn\n")
    err = open(os.path.join(a.workdir, "a.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", a.pin_server, a.bin, "--config", conf],
                            stdout=err, stderr=subprocess.STDOUT)
    t_end = time.time() + 60
    while True:
        try:
            c = kwp.Connection("127.0.0.1", a.port, timeout=300)
            break
        except OSError:
            if time.time() > t_end or proc.poll() is not None:
                raise SystemExit("no listen")
            time.sleep(0.1)

    def must(sql, tag):
        got = c.execute(sql)[2]
        if not got.startswith(tag):
            raise SystemExit(f"{sql!r} -> {got!r}")

    must("CREATE TABLE t (id int64, c1 int64, c2 int64, c3 int64) BTREE", "CREATE")
    must("CREATE INDEX ix ON t (c1)", "CREATED")
    for lo in range(1, a.rows + 1, 500):
        hi = min(a.rows, lo + 499)
        must("INSERT INTO t VALUES " + ",".join(
            f"({i},{(i * 2654435761) % 1000003},0,{i})" for i in range(lo, hi + 1)), "INSERT")
    rng = random.Random(99)
    for _ in range(300):
        c.query(f"UPDATE t SET c2 = c2 + 1 WHERE id = {rng.randint(1, a.rows)}")
    time.sleep(1.0)
    res = {"rows": a.rows, "ops": a.ops, "phases": {}}

    def phase(name, gen, tagp):
        lat, bad = [], 0
        w0 = lsn(c)
        t0 = time.perf_counter()
        for i in range(a.ops):
            s = time.perf_counter()
            tag, _, _, e = c.query(gen(i))
            lat.append(time.perf_counter() - s)
            if e is not None or not tag.startswith(tagp):
                bad += 1
        el = time.perf_counter() - t0
        res["phases"][name] = {"lat": lat, "elapsed": el, "bad": bad,
                               "wal": [w0, lsn(c)]}

    phase("ins", lambda i: f"INSERT INTO t VALUES ({a.rows + 1 + i},"
          f"{((a.rows + 1 + i) * 2654435761) % 1000003},0,0)", "INSERT")
    phase("upd2", lambda i: f"UPDATE t SET c2 = c2 + 1 WHERE id = {rng.randint(1, a.rows)}", "UPDATED 1")
    phase("upd1", lambda i: f"UPDATE t SET c1 = c1 + 1 WHERE id = {rng.randint(1, a.rows)}", "UPDATED 1")
    for nm, sql in (("lock_waits", "SELECT * FROM sys.lock_waits"),
                    ("indexes", "SELECT * FROM sys.indexes")):
        try:
            _, rows, _, _ = c.execute(sql)
            res[nm] = [[None if v is None else bytes(v).hex() for v in r] for r in rows]
        except Exception as e:  # noqa: BLE001
            res[nm] = f"error: {e}"
    c.close()
    proc.terminate()
    try:
        proc.wait(120)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    err.close()
    os.remove(data)
    shutil.rmtree(data + ".wal", ignore_errors=True)
    json.dump(res, open(a.out, "w"))


if __name__ == "__main__":
    main()
