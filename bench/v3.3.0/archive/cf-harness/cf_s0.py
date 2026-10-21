#!/usr/bin/env python3
"""CF-S0: does Waystone replay pay? (instructions/v3.2.0/workorder-cf-waystone-premise.md §5)

One server per (arm, rep), `cores = 1`, one connection, a serial driver. Three
arms (CF-Q2 (a)):

    A  waystone_recording = true,  waystone_replay = true   (the default)
    B  waystone_recording = true,  waystone_replay = false
    C  waystone_recording = false, waystone_replay = false

run in rotating order (ABC, BCA, CAB, repeated) so an arm's position is not
its identity. Each server loads the same relations and runs the same cells:

    chain3   a JOIN b JOIN c on tuple keys, WHERE a.id = k   (kLookup, kProbe, kProbe)
    chain4   chain3 JOIN d                                    (one more kProbe)
    pk       SELECT * FROM a WHERE id = k                     (kLookup)
    scanprobe t FilterScan on session_no, JOIN bar and sym on tuple keys
             (scenario 1's backtest cross-section, reproduced on synthetic
             relations; the probe rows are recordable, the scan rows are not)
    control  t FilterScan on session_no alone - no recordable step, so the
             trail is never read or written; any arm difference is position or
             noise

Every cell sweeps a working set of instances (`--keys` < 4,096, the per-core
sighting table) for `--passes` passes. Pass 1 is each instance's first
sighting, pass 2 records (n = 2), and replay starts at pass 3; passes 1 and 2
are warm-up and excluded. What is reported per cell, over the measured
passes: statements, wall-clock throughput, server CPU per statement (every
server thread's /proc/<pid>/task/*/schedstat on-CPU time), and a digest of
the last pass's rows so the three arms can be checked byte-identical
(invariant 8). After the sweep one `ANALYZE` of the cell's first key reports
`replays=` - a cell whose arm A shows none is void.

Writes `OUT/<arm>-r<rep>.json`; a result already there is skipped, so a
relaunch resumes. `OUT/progress` is touched per cell for the watchdog.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "tools")
sys.path.insert(0, os.path.abspath(TOOLS))
import kwp  # noqa: E402

ARMS = {
    "A": ("true", "true"),
    "B": ("true", "false"),
    "C": ("false", "false"),
}
ROTATION = ["ABC", "BCA", "CAB"]


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


def host_state():
    with open("/proc/loadavg") as f:
        load = f.read().strip()
    busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest "],
                          capture_output=True, text=True).stdout.strip()
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


def touch(path):
    with open(path, "a"):
        os.utime(path, None)


class Server:
    def __init__(self, a, arm, rep):
        rec, rep_on = ARMS[arm]
        tag = f"{arm}-r{rep}"
        self.data = os.path.join(a.data, f"cf-{tag}.db")
        if os.path.exists(self.data):
            os.remove(self.data)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        conf = os.path.join(a.out, f"{tag}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {a.port}\ncores = 1\n"
                    f"durability = relaxed\nbuffer_pool_frames = 65536\n"
                    f"log_file = {tag}.log\nlog_dir = {a.out}\nlog_level = warn\n"
                    f"waystone_recording = {rec}\nwaystone_replay = {rep_on}\n")
        self.err = open(os.path.join(a.out, f"{tag}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(a.pin_server), a.binary,
                                      "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", a.port, timeout=120)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server {tag} did not listen on {a.port}")
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
            self.proc.wait()
        self.err.close()
        if os.path.exists(self.data):
            os.remove(self.data)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)


def must(conn, sql, tag):
    _, _, got, _ = conn.execute(sql)
    if not got.startswith(tag):
        raise SystemExit(f"setup: {sql!r} -> {got!r}")


def load(conn, name, cols, rows, row):
    must(conn, f"CREATE TABLE {name} ({cols}) BTREE", "CREATE")
    for lo in range(1, rows + 1, 500):
        hi = min(rows, lo + 499)
        must(conn, f"INSERT INTO {name} VALUES " + ",".join(row(i) for i in range(lo, hi + 1)),
             "INSERT")


def setup(a, conn):
    n, m = a.rows, a.small
    # Each a-row names a b-row, each b-row a c-row, each c-row a d-row, by a
    # scrambled tuple key so a chain does not walk adjacent leaves.
    load(conn, "a", "id int64, b_id int64, pad int64", n,
         lambda i: f"({i}, {(i * 7919) % n + 1}, {i})")
    load(conn, "b", "id int64, c_id int64, pad int64", n,
         lambda i: f"({i}, {(i * 104729) % m + 1}, {i})")
    load(conn, "c", "id int64, d_id int64, v int64", m,
         lambda i: f"({i}, {(i * 31) % m + 1}, {i})")
    load(conn, "d", "id int64, w int64", m, lambda i: f"({i}, {i})")
    # Scenario 1's shape: t is daily_stats (session_no unindexed), bar and sym
    # are reached by tuple key from each surviving row.
    s = a.sessions
    per = a.per_session
    load(conn, "t", "id int64, session_no int64, bar_id int64, sym_id int64, ret int64",
         s * per, lambda i: f"({i}, {(i - 1) // per}, {(i * 7919) % n + 1}, "
                            f"{(i - 1) % per + 1}, {i})")
    load(conn, "bar", "id int64, close int64", n, lambda i: f"({i}, {i * 3})")
    load(conn, "sym", "id int64, ticker int64", per, lambda i: f"({i}, {i})")


def cells(a):
    step = max(1, a.rows // a.keys)
    keys = [1 + i * step for i in range(a.keys)]
    sess = list(range(0, a.sessions, max(1, a.sessions // a.scan_keys)))[:a.scan_keys]
    return [
        ("chain3", keys, lambda k: (
            f"SELECT a.id, b.id, c.v FROM a JOIN b ON a.b_id = b.id "
            f"JOIN c ON b.c_id = c.id WHERE a.id = {k}")),
        ("chain4", keys, lambda k: (
            f"SELECT a.id, b.id, c.v, d.w FROM a JOIN b ON a.b_id = b.id "
            f"JOIN c ON b.c_id = c.id JOIN d ON c.d_id = d.id WHERE a.id = {k}")),
        ("pk", keys, lambda k: f"SELECT * FROM a WHERE id = {k}"),
        ("scanprobe", sess, lambda k: (
            f"SELECT t.sym_id, t.ret, bar.close, sym.ticker FROM t "
            f"JOIN bar ON t.bar_id = bar.id JOIN sym ON t.sym_id = sym.id "
            f"WHERE t.session_no = {k}")),
        ("control", sess, lambda k: f"SELECT t.sym_id, t.ret FROM t WHERE t.session_no = {k}"),
    ]


def run_cell(a, srv, name, keys, sql_of):
    conn = srv.conn
    digest = hashlib.sha256()
    lat = []
    for p in range(a.passes):
        measured = p >= 2
        if p == 2:
            s0, w0 = sched_ns(srv.proc.pid), time.perf_counter()
        for k in keys:
            t0 = time.perf_counter()
            _, rows, tag, _ = conn.execute(sql_of(k))
            if measured:
                lat.append(time.perf_counter() - t0)
                if p == a.passes - 1:
                    for r in sorted(rows, key=lambda row: [x or b"" for x in row]):
                        digest.update(b"|".join(x or b"\0" for x in r) + b"\n")
            if not tag.startswith("SELECT"):
                raise SystemExit(f"{name}: {tag!r}")
        touch(os.path.join(a.out, "progress"))
    wall = time.perf_counter() - w0
    server_ns = sched_ns(srv.proc.pid) - s0
    _, rows, tag, _ = conn.execute("ANALYZE " + sql_of(keys[0]))
    plan = "\n".join(" ".join((x or b"").decode(errors="replace") for x in r) for r in rows) + "\n" + tag
    replays = sum(int(v) for v in re.findall(r"replays=(\d+)", plan))
    lat.sort()
    ops = len(lat)
    return {"cell": name, "ops": ops, "qps": ops / wall,
            "server_cpu_us": server_ns / ops / 1e3,
            "p50_us": lat[ops // 2] * 1e6, "p99_us": lat[min(ops - 1, ops * 99 // 100)] * 1e6,
            "digest": digest.hexdigest()[:16], "analyze_replays": replays, "plan": plan}


def one_run(a, arm, rep):
    path = os.path.join(a.out, f"{arm}-r{rep}.json")
    if os.path.exists(path):
        return
    # Quiet gate: no build or suite on the host while a server runs. Waits up
    # to --quiet-wait seconds, touching progress so the watchdog sees a live
    # wait, and records what it waited for.
    waited = 0
    pre = host_state()
    while pre["competing"] and waited < a.quiet_wait:
        touch(os.path.join(a.out, "progress"))
        time.sleep(10)
        waited += 10
        pre = host_state()
    pre["quiet_waited_s"] = waited
    srv = Server(a, arm, rep)
    try:
        setup(a, srv.conn)
        results = [run_cell(a, srv, n, k, f) for n, k, f in cells(a)]
    finally:
        srv.stop()
    with open(path + ".tmp", "w") as f:
        json.dump({"arm": arm, "rep": rep, "precheck": pre, "cells": results,
                   "finished": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())}, f, indent=1)
    os.replace(path + ".tmp", path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--data", required=True)
    p.add_argument("--port", type=int, default=15790)
    p.add_argument("--reps", type=int, default=6)
    p.add_argument("--rows", type=int, default=20000)
    p.add_argument("--small", type=int, default=2000)
    p.add_argument("--keys", type=int, default=1000)
    p.add_argument("--passes", type=int, default=22)
    p.add_argument("--sessions", type=int, default=100)
    p.add_argument("--per-session", type=int, default=100)
    p.add_argument("--scan-keys", type=int, default=50)
    p.add_argument("--pin-server", type=int, default=2)
    p.add_argument("--pin-driver", type=int, default=3)
    p.add_argument("--quiet-wait", type=int, default=900)
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.data, exist_ok=True)
    os.sched_setaffinity(0, {a.pin_driver})
    for r in range(1, a.reps + 1):
        for arm in ROTATION[(r - 1) % 3]:
            one_run(a, arm, r)
    touch(os.path.join(a.out, "done"))


if __name__ == "__main__":
    main()
