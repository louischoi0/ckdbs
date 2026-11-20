#!/usr/bin/env python3
"""CV-S0's premise cells: what `ORDER BY ... LIMIT` costs today, against a
statement the engine already runs that does the work an ordered walk would.

`instructions/v3.2.0/workorder-cv-ordered-index-walk.md` section 5 (CV-S0)
and CV-Q0. One binary (a copy, hashed), one server, `cores = 1`, one
preloaded BTREE relation `cv_t (id int64, user_id int64, created_at int64,
a int64)` with an index on `(user_id, created_at)`. User 1 owns 100 rows,
user 2 10,000 and user 3 100,000, interleaved through the tuple key order so
that a user's rows are spread over the clustered tree; `created_at` is a
random permutation, so its order is not the tuple key's. One connection.

Per cell two arms, and a third that repeats the first (the noise floor):

  T   today's statement: the shape CV elides (`... ORDER BY created_at DESC
      LIMIT 20`, `ORDER BY id DESC LIMIT 20`, ...), which today reads every
      match and keeps the page in a top-N heap
  P   the PROXY of the walk CV would run: the same statement with a bound
      that leaves exactly the rows the ordered walk would read (`AND
      created_at BETWEEN <the 20th greatest of the user> AND <above every
      stamp>`, `WHERE id BETWEEN <max - 19> AND <max>`; both ends, because a
      one-sided bound enters neither tree), run today as a bounded index range or tuple key range, the 20
      rows resolved and the heap sorting 20. It is not the walk: it pays a
      sort of 20 the walk would not, and not the walk's re-descent per leaf
      (CV-Q1 (a)), which is priced by
  D   one tuple key point descent (`WHERE id = <k>`), the reverse step's
      descent and the statement around it - an upper bound on what one
      re-descent adds, since it carries a parse, a reply and a round trip
  T2  T again, its arm order rotated: |T - T2| is the floor under a delta

Each arm runs --seconds per repeat, --reps repeats, the order rotated and
reversed per repeat. Per cell: latency percentiles, statements/s and the
server's CPU per statement (utime + stime, /proc). Once per cell the rows of
T and P are checked equal, and both statements' `ANALYZE` is kept.

Usage: cv_s0_premise.py --bin B --workdir D --out OUT.json [--reps 2]
[--seconds 2.0] [--port 15641] [--heartbeat F] [--status F]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

TABLE = "cv_t"
TICK = os.sysconf("SC_CLK_TCK")
USERS = {1: 100, 2: 10_000, 3: 100_000}
ARMS = ["T", "P", "D", "T2"]


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-c", "cc1plus"], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": load, "cc1plus": busy}


def server_cpu(pid):
    stat = open(f"/proc/{pid}/stat").read()
    f = stat[stat.rindex(")") + 2:].split()
    return (int(f[11]) + int(f[12])) / TICK


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p75_us": us(nearest_rank(s, 75)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def rows_of():
    """The relation: (id, user_id, created_at, a), users interleaved."""
    total = sum(USERS.values())
    rng = random.Random(20261120)
    owners = [u for u, n in USERS.items() for _ in range(n)]
    rng.shuffle(owners)
    stamps = rng.sample(range(1, 10 * total), total)
    return [(i + 1, owners[i], stamps[i], i % 97) for i in range(total)]


def cells(data):
    """(name, T sql, P sql) per cell."""
    by_user = {}
    for r in data:
        by_user.setdefault(r[1], []).append(r[2])
    out = []
    for u, n in USERS.items():
        floor = sorted(by_user[u])[-20]
        t = f"SELECT * FROM {TABLE} WHERE user_id = {u} ORDER BY created_at DESC LIMIT 20"
        p = (f"SELECT * FROM {TABLE} WHERE user_id = {u} AND created_at BETWEEN {floor} AND "
             f"{10 * len(data)} "
             f"ORDER BY created_at DESC LIMIT 20")
        out.append((f"latest20_user{n}", t, p))
    top = len(data)
    out.append(("id_desc_limit20",
                f"SELECT * FROM {TABLE} ORDER BY id DESC LIMIT 20",
                f"SELECT * FROM {TABLE} WHERE id BETWEEN {top - 19} AND {top} ORDER BY id DESC "
                f"LIMIT 20"))
    out.append(("id_desc_limit20_offset1000",
                f"SELECT * FROM {TABLE} ORDER BY id DESC LIMIT 20 OFFSET 1000",
                f"SELECT * FROM {TABLE} WHERE id BETWEEN {top - 1019} AND {top} ORDER BY id DESC "
                f"LIMIT 20 OFFSET 1000"))
    # Already elided today (one ascending tuple key): the walk stops at 1,020
    # rows. Measured as the order lists it; P is the same statement bounded.
    out.append(("id_asc_limit20_offset1000",
                f"SELECT * FROM {TABLE} ORDER BY id LIMIT 20 OFFSET 1000",
                f"SELECT * FROM {TABLE} WHERE id BETWEEN 1 AND 1020 ORDER BY id LIMIT 20 "
                f"OFFSET 1000"))
    return out


def run_arm(conn, pid, sql, seconds):
    lat = []
    cpu0 = server_cpu(pid)
    t1 = time.time() + seconds
    while time.time() < t1 or not lat:
        s = time.perf_counter()
        conn.execute(sql)
        lat.append(time.perf_counter() - s)
    cpu = server_cpu(pid) - cpu0
    return {"stmts": len(lat), "sps": round(len(lat) / sum(lat), 1),
            "server_cpu_us_per_stmt": round(cpu * 1e6 / len(lat), 1), "lat": pcts(lat)}


def text_rows(rows):
    return [" | ".join(v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)
                       for v in row)[:300] for row in rows]


def start_server(a):
    data = os.path.join(a.workdir, "cv.db")
    shutil.rmtree(data + ".wal", ignore_errors=True)
    if os.path.exists(data):
        os.remove(data)
    conf = os.path.join(a.workdir, "cv.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {a.port}\ncores = 1\ndurability = group\n"
                f"buffer_pool_frames = 65536\nlog_file = cv.log\nlog_dir = {a.workdir}\n"
                f"log_level = warn\n")
    err = open(os.path.join(a.workdir, "cv.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", "4", a.bin, "--config", conf], stdout=err,
                            stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    while True:
        try:
            return proc, kwp.Connection("127.0.0.1", a.port, timeout=300), err
        except OSError:
            if time.time() > deadline or proc.poll() is not None:
                raise SystemExit(f"server did not listen on {a.port}")
            time.sleep(0.1)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15641)
    p.add_argument("--reps", type=int, default=2)
    p.add_argument("--seconds", type=float, default=2.0)
    p.add_argument("--heartbeat")
    p.add_argument("--status")
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {0, 1, 2, 3})

    def beat(line):
        if a.heartbeat:
            open(a.heartbeat, "w").write(str(time.time()))
        if a.status:
            open(a.status, "a").write(time.strftime("%H:%M:%S ", time.gmtime()) + line + "\n")
        print(line, flush=True)

    out = {"args": vars(a), "bin_sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "cells": [], "plans": [], "checks": []}
    data = rows_of()
    proc, conn, err = start_server(a)
    try:
        beat("load")
        conn.execute(f"CREATE TABLE {TABLE} (id int64, user_id int64, created_at int64, "
                     f"a int64) BTREE")
        for lo in range(0, len(data), 500):
            conn.execute(f"INSERT INTO {TABLE} VALUES " +
                         ",".join(f"({i},{u},{c},{x})" for i, u, c, x in data[lo:lo + 500]))
        conn.execute(f"CREATE INDEX cv_uc ON {TABLE} (user_id, created_at)")
        point = f"SELECT * FROM {TABLE} WHERE id = 55555"
        for name, t_sql, p_sql in cells(data):
            for arm, sql in (("T", t_sql), ("P", p_sql), ("D", point)):
                out["plans"].append({"cell": name, "arm": arm, "sql": sql,
                                     "analyze": text_rows(conn.execute("ANALYZE " + sql)[1])})
            p_plan = out["plans"][-2]["analyze"]
            if "examined=" + str(len(data)) in p_plan[0] or (
                    name.startswith("latest") and "IndexRange" not in " ".join(p_plan)):
                raise RuntimeError(f"{name}: the proxy does not narrow: {p_plan[:3]}")
            same = conn.execute(t_sql)[1] == conn.execute(p_sql)[1]
            out["checks"].append({"cell": name, "same_rows": same})
            if not same:
                raise RuntimeError(f"{name}: T and P disagree on the rows")
            sqls = {"T": t_sql, "P": p_sql, "D": point, "T2": t_sql}
            for arm in ("T", "P", "D"):
                run_arm(conn, proc.pid, sqls[arm], 0.3)
            for rep in range(a.reps):
                order = ARMS[rep % 4:] + ARMS[:rep % 4]
                if rep % 2:
                    order = order[::-1]
                for arm in order:
                    beat(f"{name} rep {rep} {arm}")
                    c = run_arm(conn, proc.pid, sqls[arm], a.seconds)
                    c.update(cell=name, arm=arm, rep=rep, host=host_state())
                    out["cells"].append(c)
                    json.dump(out, open(a.out, "w"), indent=1)
    finally:
        conn.close()
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
        err.close()
    json.dump(out, open(a.out, "w"), indent=1)
    beat("driver finished")


if __name__ == "__main__":
    main()
