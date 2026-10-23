#!/usr/bin/env python3
"""CI-S0 item 1: what CREATE INDEX's relation X costs a concurrent writer.

    ci_s0_premise.py --bin BINARY --workdir DIR --rows N --window S --build-at S
                     --runs R --progress FILE --out OUT.json [--template-only]

Loads a template volume once (`t (id int64, c1, c2, c3 int64) BTREE`, N rows,
c1 a scrambled value), then per run copies it (fresh server, fresh data file
and WAL directory), and runs one arm for `--window` seconds:

  A  a writer (autocommit `UPDATE t SET c2 = c2 + 1 WHERE id = k`, random k)
     and a reader (`SELECT * FROM t WHERE id = k`), each its own connection
  B  the same two, plus a third session issuing `CREATE INDEX ix ON t (c1)`
     `--build-at` seconds after they start

Arms alternate A B A B ... Every statement's start offset and latency is
recorded; the output JSON is the raw series plus the build's elapsed time and
a post-run read of SHOW INDEXES, sys.lock_waits and sys.statement_stats.
The server is pinned to `--pin-server` CPUs, the driver to `--pin-driver`.
"""
import argparse, json, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402


def conf_for(a, data, port, tag):
    path = os.path.join(a.workdir, f"{tag}.conf")
    with open(path, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {a.cores}\n"
                f"durability = {a.durability}\nbuffer_pool_frames = 65536\n"
                f"log_file = {tag}.log\nlog_dir = {a.workdir}\nlog_level = warn\n")
    return path


def start(a, data, port, tag):
    conf = conf_for(a, data, port, tag)
    err = open(os.path.join(a.workdir, f"{tag}.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", a.pin_server, a.bin, "--config", conf],
                            stdout=err, stderr=subprocess.STDOUT)
    deadline = time.time() + 120
    while True:
        try:
            return proc, kwp.Connection("127.0.0.1", port, timeout=300), err
        except OSError:
            if time.time() > deadline or proc.poll() is not None:
                raise SystemExit(f"server {tag} did not listen")
            time.sleep(0.1)


def stop(proc, conn, err):
    try:
        conn.close()
    except OSError:
        pass
    proc.terminate()
    try:
        proc.wait(120)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    err.close()


def must(conn, sql, tag):
    _, _, got, _ = conn.execute(sql)
    if not got.startswith(tag):
        raise SystemExit(f"{sql!r} -> {got!r}")
    return got


def rows_of(conn, sql):
    """(fields as dicts, rows as lists of hex strings or None, tag)."""
    fields, rows, tag, _ = conn.execute(sql)
    fl = [dict(f) if isinstance(f, dict) else str(f) for f in fields]
    return fl, [[None if v is None else bytes(v).hex() for v in r] for r in rows], tag


def scramble(i, n):
    return (i * 2654435761) % 1000003


def make_template(a):
    data = os.path.join(a.workdir, "template.db")
    for p in (data,):
        if os.path.exists(p):
            os.remove(p)
    shutil.rmtree(data + ".wal", ignore_errors=True)
    proc, conn, err = start(a, data, a.port, "template")
    must(conn, "CREATE TABLE t (id int64, c1 int64, c2 int64, c3 int64) BTREE", "CREATE")
    t0 = time.time()
    for lo in range(1, a.rows + 1, 500):
        hi = min(a.rows, lo + 499)
        must(conn, "INSERT INTO t VALUES " + ",".join(
            f"({i},{scramble(i, a.rows)},0,{i})" for i in range(lo, hi + 1)), "INSERT")
    load_s = time.time() - t0
    stop(proc, conn, err)
    return data, load_s


def copy_volume(src, dst):
    for p in (dst,):
        if os.path.exists(p):
            os.remove(p)
    shutil.rmtree(dst + ".wal", ignore_errors=True)
    shutil.copy(src, dst)
    if os.path.isdir(src + ".wal"):
        shutil.copytree(src + ".wal", dst + ".wal")


def session(port, stmt_of, rec, stop_at, t_zero, tag_prefix, errs):
    conn = kwp.Connection("127.0.0.1", port, timeout=300)
    rng = random.Random(12345)
    try:
        while True:
            now = time.perf_counter()
            if now >= stop_at:
                break
            tag, _, _, err = conn.query(stmt_of(rng))
            t1 = time.perf_counter()
            if err is not None or not tag.startswith(tag_prefix):
                errs.append((now - t_zero, tag, str(err)))
            rec.append((now - t_zero, t1 - now))
    finally:
        conn.close()


def run_arm(a, arm, tmpl, run):
    tag = f"r{run}{arm}"
    data = os.path.join(a.workdir, f"{tag}.db")
    copy_volume(tmpl, data)
    proc, ctl, err = start(a, data, a.port + 1, tag)
    n = a.rows
    # warm: touch the pool the way the window will
    rng = random.Random(7)
    for _ in range(500):
        ctl.query(f"UPDATE t SET c2 = c2 + 1 WHERE id = {rng.randint(1, n)}")
    time.sleep(1.0)
    cpu0 = sum(int(open(f"/proc/{proc.pid}/task/{t}/schedstat").read().split()[0])
               for t in os.listdir(f"/proc/{proc.pid}/task"))
    w_rec, r_rec, w_err, r_err, b_res = [], [], [], [], {}
    meta_before = rows_of(ctl, "SHOW META") if arm == "B" else None
    t_zero = time.perf_counter()
    stop_at = t_zero + a.window

    def build():
        time.sleep(a.build_at)
        c = kwp.Connection("127.0.0.1", a.port + 1, timeout=600)
        t0 = time.perf_counter()
        got = c.execute("CREATE INDEX ix ON t (c1)")[2]
        t1 = time.perf_counter()
        b_res.update(start=t0 - t_zero, end=t1 - t_zero, elapsed=t1 - t0, tag=got)
        c.close()

    ths = [threading.Thread(target=session, args=(
               a.port + 1, lambda g: f"UPDATE t SET c2 = c2 + 1 WHERE id = {g.randint(1, n)}",
               w_rec, stop_at, t_zero, "UPDATED 1", w_err)),
           threading.Thread(target=session, args=(
               a.port + 1, lambda g: f"SELECT * FROM t WHERE id = {g.randint(1, n)}",
               r_rec, stop_at, t_zero, "SELECT", r_err))]
    if arm == "B":
        ths.append(threading.Thread(target=build))
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    cpu1 = sum(int(open(f"/proc/{proc.pid}/task/{t}/schedstat").read().split()[0])
               for t in os.listdir(f"/proc/{proc.pid}/task"))
    out = {"arm": arm, "run": run, "writer": w_rec, "reader": r_rec,
           "writer_errs": w_err[:5], "reader_errs": r_err[:5], "build": b_res,
           "server_cpu_s": (cpu1 - cpu0) / 1e9}
    if arm == "B":
        out["meta_before"] = meta_before
        out["meta_after"] = rows_of(ctl, "SHOW META")
        for name, sql in (("indexes", "SELECT * FROM sys.indexes"),
                          ("lock_waits", "SELECT * FROM sys.lock_waits"),
                          ("statement_stats", "SELECT * FROM sys.statement_stats"),
                          ("relation_stats", "SELECT * FROM sys.relation_stats")):
            try:
                out[name] = rows_of(ctl, sql)
            except Exception as e:  # noqa: BLE001
                out[name] = f"error: {e}"
        time.sleep(0.2)
    out["data_bytes"] = os.path.getsize(data)
    stop(proc, ctl, err)
    os.remove(data)
    shutil.rmtree(data + ".wal", ignore_errors=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--window", type=float, default=40.0)
    ap.add_argument("--build-at", type=float, default=5.0)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--port", type=int, default=31970)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--pin-server", default="2-5")
    ap.add_argument("--pin-driver", default="6,7")
    ap.add_argument("--progress", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--template-only", action="store_true")
    ap.add_argument("--reuse-template", action="store_true")
    ap.add_argument("--run-offset", type=int, default=0)
    ap.add_argument("--keep-template", action="store_true")
    ap.add_argument("--arms", default="AB")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {int(x) for x in a.pin_driver.split(",")})

    def prog(msg):
        with open(a.progress, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S', time.gmtime())} {msg}\n")

    if a.reuse_template:
        tmpl, load_s = os.path.join(a.workdir, "template.db"), 0.0
    else:
        tmpl, load_s = make_template(a)
    prog(f"template loaded rows={a.rows} load_s={load_s:.1f} bytes={os.path.getsize(tmpl)}")
    results = {"rows": a.rows, "load_s": load_s, "template_bytes": os.path.getsize(tmpl),
               "cores": a.cores, "durability": a.durability, "window": a.window,
               "build_at": a.build_at, "runs": []}
    if a.template_only:
        # one trial build, no load
        r = run_arm(a, "B", tmpl, 0)
        results["runs"].append(r)
        prog(f"trial build elapsed={r['build'].get('elapsed')}")
    else:
        for run in range(a.run_offset, a.run_offset + a.runs):
            for arm in a.arms:
                prog(f"start run {run} arm {arm} load={open('/proc/loadavg').read().split()[0]}")
                results["runs"].append(run_arm(a, arm, tmpl, run))
                json.dump(results, open(a.out, "w"))
                prog(f"done run {run} arm {arm} build={results['runs'][-1]['build'].get('elapsed')}")
    json.dump(results, open(a.out, "w"))
    if not a.keep_template:
        os.remove(tmpl)
        shutil.rmtree(tmpl + ".wal", ignore_errors=True)
    prog("finished")


if __name__ == "__main__":
    main()
