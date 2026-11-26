#!/usr/bin/env python3
"""DU-S0's premise cells: what an UPDATE by a non-unique index key costs
today, as one walked statement, against the same rows found through the
index and written by tuple key.

`instructions/v3.2.0/workorder-du-keyed-write.md` section 5 (DU-S0) and
DU-Q1 (a): under a 5x saving DU stops at the bounded walk. One binary (a
copy, hashed), one server, a fresh data file, `cores = 1`, `durability =
relaxed`, one preloaded BTREE relation `du_t (id int64, owner int64, v
int64)` of --rows rows with a secondary index on `owner`, and one
connection. Per cell (m = rows per owner value; `owner = id % (rows / m)`,
so one value's rows are spread over the relation) three arms:

  W   one statement, `UPDATE du_t SET v = v + 1 WHERE owner = K` - today a
      walk of every leaf under a write latch, the equality a residual
  K   the same rows as two statements: `SELECT id FROM du_t WHERE owner = K`,
      an index probe, then `UPDATE du_t SET v = v + 1 WHERE id IN (...)`
      over the ids it returned, one descent per id (CU-S3). It is the
      mechanism DU-S2 proposes - collect through the index, write by the
      point arm - run today, and an UPPER bound on its cost: it pays a
      second parse, a reply carrying the ids and a second round trip that
      one index-driven statement would not
  W2  a second W arm: the noise control. |W - W2| is the floor under any
      delta against W

The unit is one group: one owner value's rows updated. Each arm runs
--groups groups per repeat (equal work), --reps repeats, the arm order
rotated per repeat, K drawn afresh per group. Per arm it records the group
latency percentiles, the server's CPU per group (on-CPU ns summed over the
server's threads, /proc/<pid>/task/*/schedstat) and the rows written per
group, and the host's load before each arm.

Usage: du_s0_premise.py --bin B --workdir D --out OUT.json --status F
[--rows 1000000] [--m 10,1000] [--reps 3] [--groups 12] [--port 31990]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

ARMS = ["W", "K", "W2"]


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": load, "busy": busy.splitlines()[:4]}


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


def group(conn, arm, owner):
    """One owner value's rows updated; the rows the engine said it wrote."""
    if arm in ("W", "W2"):
        return int(must(conn, f"UPDATE du_t SET v = v + 1 WHERE owner = {owner}",
                        "UPDATED").split()[1])
    fields, rows, _, _ = conn.execute(f"SELECT id FROM du_t WHERE owner = {owner}")
    ids = [kwp.render_value(fields[0], r[0]) for r in rows]
    if not ids:
        return 0
    return int(must(conn, "UPDATE du_t SET v = v + 1 WHERE id IN (" + ", ".join(ids) + ")",
                    "UPDATED").split()[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--m", default="10,1000")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--groups", type=int, default=12)
    ap.add_argument("--port", type=int, default=31990)
    a = ap.parse_args()

    def status(text):
        with open(a.status, "w") as f:
            f.write(f"{time.time():.0f} {text}\n")

    os.makedirs(a.workdir, exist_ok=True)
    binary = os.path.join(a.workdir, "kds_server.measured")
    shutil.copy2(a.bin, binary)
    sha = hashlib.sha256(open(binary, "rb").read()).hexdigest()
    result = {"binary_sha256": sha, "binary_mtime": os.path.getmtime(a.bin), "rows": a.rows,
              "reps": a.reps, "groups": a.groups, "cells": []}

    for m in [int(x) for x in a.m.split(",")]:
        owners = a.rows // m
        data = os.path.join(a.workdir, f"du-m{m}.db")
        if os.path.exists(data):
            os.remove(data)
        shutil.rmtree(data + ".wal", ignore_errors=True)
        conf = os.path.join(a.workdir, f"du-m{m}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {a.port}\ncores = 1\ndurability = relaxed\n"
                    f"buffer_pool_frames = 131072\nlog_file = du-m{m}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\n")
        err = open(os.path.join(a.workdir, f"du-m{m}.stderr"), "w")
        proc = subprocess.Popen(["taskset", "-c", "2", binary, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 30
            while True:
                try:
                    conn = kwp.Connection("127.0.0.1", a.port, timeout=300)
                    break
                except OSError:
                    if time.time() > deadline or proc.poll() is not None:
                        raise SystemExit("the server did not listen")
                    time.sleep(0.1)
            must(conn, "CREATE TABLE du_t (id int64, owner int64, v int64) BTREE", "CREATE")
            for lo in range(1, a.rows + 1, 1000):
                hi = min(a.rows, lo + 999)
                must(conn, "INSERT INTO du_t VALUES " +
                     ",".join(f"({i}, {i % owners}, 0)" for i in range(lo, hi + 1)), "INSERT")
                if lo % 100_000 == 1:
                    status(f"m={m} loading {lo}/{a.rows}")
            must(conn, "CREATE INDEX du_ix_owner ON du_t(owner)", "CREATE")
            # The index probe arm K's SELECT runs as, in `ANALYZE`'s own words.
            analyze = repr(conn.execute("ANALYZE SELECT id FROM du_t WHERE owner = 7")[1])[:4000]
            rng = random.Random(7919 * m)
            for arm in ARMS:  # a warm pass: every leaf resident, each arm's statements parsed
                group(conn, arm, rng.randrange(owners))
            arms = {arm: {"lat": [], "server_ns": 0, "rows": 0, "groups": 0, "host": []}
                    for arm in ARMS}
            for rep in range(a.reps):
                order = ARMS[rep % len(ARMS):] + ARMS[:rep % len(ARMS)]
                for arm in order:
                    status(f"m={m} rep={rep} arm={arm}")
                    acc = arms[arm]
                    acc["host"].append(host_state())
                    cpu0 = sched_ns(proc.pid)
                    for _ in range(a.groups):
                        owner = rng.randrange(owners)
                        t0 = time.perf_counter()
                        wrote = group(conn, arm, owner)
                        acc["lat"].append(time.perf_counter() - t0)
                        if wrote != m:
                            raise SystemExit(f"arm {arm} owner {owner}: wrote {wrote}, not {m}")
                        acc["rows"] += wrote
                        acc["groups"] += 1
                    acc["server_ns"] += sched_ns(proc.pid) - cpu0
            # Every arm wrote the same statement count per row's owner draw: the
            # relation's sum of v is the rows every arm wrote, the warm pass's too.
            total = conn.execute("SELECT SUM(v) FROM du_t")
            cell = {"m": m, "owners": owners, "analyze": analyze,
                    "sum_v": kwp.render_value(total[0][0], total[1][0][0]), "arms": {}}
            for arm, acc in arms.items():
                cell["arms"][arm] = {
                    "groups": acc["groups"], "rows": acc["rows"],
                    "server_cpu_us_per_group": round(acc["server_ns"] / acc["groups"] / 1e3, 1),
                    "latency": pcts(acc["lat"]), "host": acc["host"]}
            result["cells"].append(cell)
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
