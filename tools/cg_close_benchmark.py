#!/usr/bin/env python3
"""CG close (CG-S3): `INSERT ... ON CONFLICT` against the paths it touched.

Against one running server (start it with the config the results file names;
this driver never starts or stops a server). For each --sizes row count, two
fresh BTREE relations are loaded untimed:
    t (id int64, c_int int64, c_text varchar(16))           ids 1..N
    u (id int64, name varchar(16), v int64), a UNIQUE INDEX on name, names n1..nN
then each phase runs --ops single-row autocommit statements from one
connection, timed on the wire:

    insert       INSERT INTO t VALUES (<fresh id>, ...)                 A and B
    dn-insert    ... ON CONFLICT DO NOTHING, a fresh id (placed)         A and B
    dn-skip      ... ON CONFLICT DO NOTHING, a placed id (skipped)       A and B
    update       UPDATE t SET c_int = x WHERE id = <placed id>           A and B
    uq-update    UPDATE u SET v = x WHERE name = '<placed name>'         A and B
    up-insert    ... ON CONFLICT (id) DO UPDATE SET c_int = EXCLUDED.c_int,
                 a fresh id (the insert arm)                             B only
    up-update    the same, a placed id (the update arm)                  B only
    uq-upsert    INSERT INTO u (name, v) VALUES ('<placed name>', x)
                 ON CONFLICT (name) DO UPDATE SET v = EXCLUDED.v         B only

`--engine a` runs the phases A can parse; `--engine b` runs them all. Prints one
JSON object (--json PATH also writes it): per size, per phase, nearest-rank
p0 p25 p50 p90 p99 max and the mean, in microseconds (bench_common).

With `--server-pid PID`, each phase also carries the server's on-CPU time and
run-queue wait per statement (every thread's /proc/PID/task/*/schedstat, in
us), and with `--watch-cpus 4,5` each listed CPU's busy share over the phase
(/proc/stat). On-CPU time tells a waiting server from a working one, not more
work from a slower CPU; the sibling's share names one cause of the latter.
/proc/stat counts 10 ms ticks, so a share is noise over a phase much under 1 s.

    tools/cg_close_benchmark.py --port 15901 --engine b --sizes 1000,10000
"""

import argparse
import json
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_common import nearest_rank  # noqa: E402
from ckdbs_cli import ServerConnection  # noqa: E402

PHASES_A = ["insert", "dn-insert", "dn-skip", "update", "uq-update"]
PHASES_B = PHASES_A + ["up-insert", "up-update", "uq-upsert"]


def must(conn, stmt):
    r = conn.send_command(stmt)
    if r.startswith("ERR"):
        raise RuntimeError(f"{stmt[:90]} -> {r[:200]}")
    return r


def dist(lat):
    o = sorted(lat)
    f = lambda p: round(nearest_rank(o, p) * 1e6, 1)  # noqa: E731
    return {"n": len(o), "p0": round(o[0] * 1e6, 1), "p25": f(25), "p50": f(50),
            "p90": f(90), "p99": f(99), "max": round(o[-1] * 1e6, 1),
            "mean": round(sum(o) / len(o) * 1e6, 1)}


def sched(pid):
    """(on-CPU ns, run-queue wait ns) summed over the process's threads."""
    cpu = wait = 0
    base = Path(f"/proc/{pid}/task")
    for task in base.iterdir():
        try:
            v = (task / "schedstat").read_text().split()
            cpu += int(v[0])
            wait += int(v[1])
        except (OSError, IndexError):
            pass
    return cpu, wait


def cpu_ticks(cpus):
    """{cpu: (busy, total)} jiffies from /proc/stat; busy is all but idle and iowait."""
    out = {}
    for line in Path("/proc/stat").read_text().splitlines():
        name = line.split()[0]
        if name.startswith("cpu") and name[3:].isdigit() and int(name[3:]) in cpus:
            v = [int(x) for x in line.split()[1:]]
            total = sum(v[:8])  # guest and guest_nice are already in user and nice
            out[int(name[3:])] = (total - v[3] - v[4], total)
    return out


def timed(conn, stmt):
    t = time.perf_counter()
    r = conn.send_command(stmt)
    d = time.perf_counter() - t
    if r.startswith("ERR"):
        raise RuntimeError(f"{stmt[:90]} -> {r[:200]}")
    return d


def load(conn, size, suffix):
    t, u = f"t_{size}_{suffix}", f"u_{size}_{suffix}"
    must(conn, f"CREATE TABLE {t} (id int64, c_int int64, c_text varchar(16)) BTREE")
    must(conn, f"CREATE TABLE {u} (id int64, name varchar(16), v int64) BTREE")
    must(conn, f"CREATE UNIQUE INDEX {u}_name ON {u} (name)")
    for at in range(1, size + 1, 500):
        hi = min(at + 500, size + 1)
        must(conn, f"INSERT INTO {t} VALUES " +
             ", ".join(f"({i}, {i}, 'x{i}')" for i in range(at, hi)))
        must(conn, f"INSERT INTO {u} VALUES " +
             ", ".join(f"({i}, 'n{i}', {i})" for i in range(at, hi)))
    return t, u


def run_size(conn, size, phases, ops, rng, suffix, pid=None, cpus=()):
    t, u = load(conn, size, suffix)
    for _ in range(50):  # warm the path once per size
        conn.send_command(f"SELECT * FROM {t} WHERE id = {rng.randint(1, size)}")
    fresh = size + 1
    out = {}
    for phase in phases:
        s0 = sched(pid) if pid else None
        c0 = cpu_ticks(cpus) if cpus else None
        l0 = Path("/proc/loadavg").read_text().split()[0]
        lat = []
        for _ in range(ops):
            x = rng.randint(0, 10**6)
            placed = rng.randint(1, size)
            if phase in ("insert", "dn-insert", "up-insert"):
                key, fresh = fresh, fresh + 1
            if phase == "insert":
                stmt = f"INSERT INTO {t} VALUES ({key}, {x}, 'y')"
            elif phase == "dn-insert":
                stmt = f"INSERT INTO {t} VALUES ({key}, {x}, 'y') ON CONFLICT DO NOTHING"
            elif phase == "dn-skip":
                stmt = f"INSERT INTO {t} VALUES ({placed}, {x}, 'y') ON CONFLICT DO NOTHING"
            elif phase == "update":
                stmt = f"UPDATE {t} SET c_int = {x} WHERE id = {placed}"
            elif phase == "uq-update":
                stmt = f"UPDATE {u} SET v = {x} WHERE name = 'n{placed}'"
            elif phase == "up-insert":
                stmt = (f"INSERT INTO {t} VALUES ({key}, {x}, 'y') "
                        f"ON CONFLICT (id) DO UPDATE SET c_int = EXCLUDED.c_int")
            elif phase == "up-update":
                stmt = (f"INSERT INTO {t} VALUES ({placed}, {x}, 'y') "
                        f"ON CONFLICT (id) DO UPDATE SET c_int = EXCLUDED.c_int")
            else:  # uq-upsert
                stmt = (f"INSERT INTO {u} (name, v) VALUES ('n{placed}', {x}) "
                        f"ON CONFLICT (name) DO UPDATE SET v = EXCLUDED.v")
            lat.append(timed(conn, stmt))
        out[phase] = dist(lat)
        if s0:
            s1 = sched(pid)
            out[phase]["server_cpu_us"] = round((s1[0] - s0[0]) / ops / 1e3, 1)
            out[phase]["server_wait_us"] = round((s1[1] - s0[1]) / ops / 1e3, 1)
        if c0:
            c1 = cpu_ticks(cpus)
            out[phase]["cpu_busy_pct"] = {
                str(c): round(100.0 * (c1[c][0] - c0[c][0]) / max(1, c1[c][1] - c0[c][1]), 1)
                for c in cpus}
        out[phase]["load1"] = [float(l0), float(Path("/proc/loadavg").read_text().split()[0])]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--engine", choices=("a", "b"), required=True)
    ap.add_argument("--sizes", default="1000,10000")
    ap.add_argument("--ops", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json")
    ap.add_argument("--server-pid", type=int)
    ap.add_argument("--watch-cpus", default="")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    cpus = tuple(int(c) for c in args.watch_cpus.split(",") if c)
    conn = ServerConnection("127.0.0.1", args.port, timeout=120)
    phases = PHASES_A if args.engine == "a" else PHASES_B
    res = {str(s): run_size(conn, s, phases, args.ops, rng, "r", args.server_pid, cpus)
           for s in [int(x) for x in args.sizes.split(",")]}
    conn.close()
    text = json.dumps(res, indent=1)
    print(text)
    if args.json:
        Path(args.json).write_text(text)


if __name__ == "__main__":
    main()
