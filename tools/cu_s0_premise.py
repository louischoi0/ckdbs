#!/usr/bin/env python3
"""CU-S0's premise cells: what a value list costs today, as one walked
statement, against the same values served by single-value descents.

`instructions/v3.2.0/workorder-cu-key-set-access.md` section 5 (CU-S0) and
CU-Q0. One binary (a copy, hashed), one server per relation size with a fresh
data file, `cores = 1`, one preloaded BTREE relation
`cu_t (id int64, k int64, a int64, b int64)` with `k = 3 * id + 1` and a
secondary index on `k`. One connection. Per cell (rows, column, n) four arms:

  L   one statement, `SELECT * FROM cu_t WHERE <col> IN (v1, ..., vn)` -
      today a walk of the relation with the list as a residual
  D   the same n values as n statements `... WHERE <col> = v`, one after
      another - the single-value descent CU would run once per value. It is
      an UPPER bound on a key set's cost: it pays n parses, n replies and n
      round trips that one statement with a key set would not
  J   one statement that runs n descents: the n values stored as the rows
      of a relation `cu_v_<n> (id, rid, rk)` and joined to `cu_t` on the
      column, which the engine runs as a walk of the n rows and one tuple key
      probe (or index probe) of `cu_t` per row, the join equality kept as
      the probe's filter. It is the closest statement the engine runs today
      to a key set: n descents, one parse, one reply. Its values are one
      fixed draw per cell (they are stored), so its descents revisit the
      same leaves every group, and it pays a walk of `cu_v_<n>` a key set
      would not. The server runs with `join_order = written`, which only
      this arm reads: planned, a list longer than the relation is run as a
      walk of `cu_t` over a build of `cu_v_<n>`, which is no descent
  L2  a second L arm: the noise control. |L - L2| and the repeat-to-repeat
      spread are the floor under any delta against L

The unit is one group: one list answered (one L or J statement, or n D
statements). An L or D group's values are drawn afresh, distinct, from
[1, max(rows, n)], so a list longer than the relation carries misses. Each
arm runs --seconds per repeat, --reps repeats, the arm order rotated per
repeat. Per cell it records the group latency percentiles, groups/s, the
server's CPU per group (utime + stime of the server process, /proc), and the
rows returned per group; once per cell the rows of an L statement are
checked, row for row, against the D statements of the same values and
against the J statement of its stored values. `ANALYZE`'s reply for the L
statement, the J statement and one single-value statement is kept per cell.

Usage: cu_s0_premise.py --bin B --workdir D --out OUT.json [--rows
1000,100000] [--n 1,4,64,1024,4096] [--cols id,k] [--reps 2] [--seconds 2.0]
[--heartbeat F] [--status F]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

TABLE = "cu_t"
TICK = os.sysconf("SC_CLK_TCK")
ARMS = ["L", "D", "J", "L2"]


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
    """utime + stime of the whole server process, in seconds."""
    stat = open(f"/proc/{pid}/stat").read()
    f = stat[stat.rindex(")") + 2:].split()  # past `(comm)`, which may hold spaces
    return (int(f[11]) + int(f[12])) / TICK


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p75_us": us(nearest_rank(s, 75)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def col_value(col, ident):
    return ident if col == "id" else 3 * ident + 1


def draw(rng, rows, n):
    return rng.sample(range(1, max(rows, n) + 1), n)


def stored(rows, n):
    """The one draw `cu_v_<n>` holds."""
    return draw(random.Random(104729 * n + rows), rows, n)


def point_sql(col, ident):
    return f"SELECT * FROM {TABLE} WHERE {col} = {col_value(col, ident)}"


def list_sql(col, idents):
    return (f"SELECT * FROM {TABLE} WHERE {col} IN (" +
            ", ".join(str(col_value(col, i)) for i in idents) + ")")


def join_sql(col, n):
    on = "t.id = v.rid" if col == "id" else "t.k = v.rk"
    return f"SELECT t.id, t.k, t.a, t.b FROM cu_v_{n} AS v JOIN {TABLE} AS t ON {on}"


def answer(conn, arm, col, idents):
    """The rows of one group: one list answered by `arm`."""
    if arm == "L":
        return conn.execute(list_sql(col, idents))[1]
    if arm == "J":
        return conn.execute(join_sql(col, len(idents)))[1]
    rows = []
    for i in idents:
        rows.extend(conn.execute(point_sql(col, i))[1])
    return rows


def run_cell(conn, pid, arm, col, rows, n, seconds, seed):
    rng = random.Random(seed)
    lat, returned = [], 0
    cpu0 = server_cpu(pid)
    t1 = time.time() + seconds
    while time.time() < t1 or not lat:
        idents = stored(rows, n) if arm == "J" else draw(rng, rows, n)
        s = time.perf_counter()
        got = len(answer(conn, arm, col, idents))
        lat.append(time.perf_counter() - s)
        want = sum(1 for i in idents if i <= rows)
        if got != want:
            raise RuntimeError(f"arm {arm} col {col} n {n}: {got} rows, expected {want}")
        returned += got
    cpu = server_cpu(pid) - cpu0
    groups = len(lat)
    return {"groups": groups, "gps": round(groups / sum(lat), 1),
            "server_cpu_us_per_group": round(cpu * 1e6 / groups, 1),
            "server_cpu_s": round(cpu, 2), "rows_per_group": round(returned / groups, 1),
            "lat": pcts(lat)}


def same_rows(conn, col, rows, n, seed):
    """L's rows against D's for one draw, and against J's for the stored one."""
    key = lambda rs: sorted(map(tuple, rs))
    idents = draw(random.Random(seed), rows, n)
    if key(answer(conn, "L", col, idents)) != key(answer(conn, "D", col, idents)):
        return False
    idents = stored(rows, n)
    return key(answer(conn, "L", col, idents)) == key(answer(conn, "J", col, idents))


def text_rows(rows):
    """A reply's rows as text lines, each cut at 300 characters (a list's
    filter line is as long as the list)."""
    return [" | ".join(v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)
                       for v in row)[:300] for row in rows]


def start_server(a, binary, tag):
    data = os.path.join(a.workdir, f"cu-{tag}.db")
    shutil.rmtree(data + ".wal", ignore_errors=True)
    if os.path.exists(data):
        os.remove(data)
    conf = os.path.join(a.workdir, f"cu-{tag}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {a.port}\ncores = 1\ndurability = group\n"
                f"buffer_pool_frames = 65536\nlog_file = cu-{tag}.log\nlog_dir = {a.workdir}\n"
                f"log_level = warn\njoin_order = written\n")
    err = open(os.path.join(a.workdir, f"cu-{tag}.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", "4", binary, "--config", conf], stdout=err,
                            stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    while True:
        try:
            return proc, kwp.Connection("127.0.0.1", a.port, timeout=300), err
        except OSError:
            if time.time() > deadline or proc.poll() is not None:
                raise SystemExit(f"server {tag} did not listen on {a.port}")
            time.sleep(0.1)


def load(conn, rows, ns):
    conn.execute(f"CREATE TABLE {TABLE} (id int64, k int64, a int64, b int64) BTREE")
    for lo in range(1, rows + 1, 500):
        conn.execute(f"INSERT INTO {TABLE} VALUES " +
                     ",".join(f"({i},{3 * i + 1},{i},{i})"
                              for i in range(lo, min(rows, lo + 499) + 1)))
    conn.execute(f"CREATE INDEX cu_k ON {TABLE} (k)")
    for n in ns:
        conn.execute(f"CREATE TABLE cu_v_{n} (id int64, rid int64, rk int64) BTREE")
        vals = [f"({j},{i},{3 * i + 1})" for j, i in enumerate(stored(rows, n), 1)]
        for lo in range(0, n, 500):
            conn.execute(f"INSERT INTO cu_v_{n} VALUES " + ",".join(vals[lo:lo + 500]))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15631)
    p.add_argument("--rows", default="1000,100000")
    p.add_argument("--n", default="1,4,64,1024,4096")
    p.add_argument("--cols", default="id,k")
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
    ns = [int(x) for x in a.n.split(",")]
    for rows in [int(x) for x in a.rows.split(",")]:
        tag = f"r{rows}"
        proc, conn, err = start_server(a, a.bin, tag)
        try:
            beat(f"{tag} load")
            load(conn, rows, ns)
            for col in a.cols.split(","):
                for n in ns:
                    seed = 7919 * n + rows
                    for arm, sql in (("L", list_sql(col, draw(random.Random(seed), rows, n))),
                                     ("J", join_sql(col, n)), ("D", point_sql(col, 77))):
                        plan = text_rows(conn.execute("ANALYZE " + sql)[1])
                        out["plans"].append({"rows": rows, "col": col, "n": n, "arm": arm,
                                             "sql": sql[:300], "analyze": plan})
                        want = "step 1 " + ("Probe" if col == "id" else "IndexProbe") + " cu_t"
                        if arm == "J" and not any(line.startswith(want) for line in plan):
                            raise RuntimeError(f"{tag} {col} n {n}: J is not a probe: {plan}")
                    ok = same_rows(conn, col, rows, n, seed)
                    out["checks"].append({"rows": rows, "col": col, "n": n, "same_rows": ok})
                    if not ok:
                        raise RuntimeError(f"{tag} {col} n {n}: the arms disagree on the rows")
                    for arm in ARMS[:3]:  # warm up: the pool, the plan memo
                        run_cell(conn, proc.pid, arm, col, rows, n, 0.3, seed)
                    for rep in range(a.reps):
                        order = ARMS[rep % 4:] + ARMS[:rep % 4]
                        if rep % 2:
                            order = order[::-1]
                        for arm in order:
                            beat(f"{tag} col {col} n {n} rep {rep} {arm}")
                            c = run_cell(conn, proc.pid, "L" if arm == "L2" else arm, col, rows, n,
                                         a.seconds, seed + 1000 * rep + len(arm))
                            c.update(arm=arm, rep=rep, rows=rows, col=col, n=n, host=host_state())
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
