#!/usr/bin/env python3
"""DJ-S5's close: what DJ costs the paths it touched, the commit DJ opened
from (A) against the closing code (B), with DJ's keys unset and set.

`instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` §5
(DJ-S5), DJ-R1, DJ-R6, DJ-R7. Four arms, each a server of its own, started
afresh in every lifetime:

  A    the opening commit's binary
  B    the closing binary, DJ's keys unset (DJ-R1: the engine as it was)
  Bk   the closing binary with `max_connections = 100000` and
       `idle_session_timeout_s = 3600` - the cap counted and the sweep
       armed, neither ever reached
  A2   A again: |A - A2| is the floor under a delta

Per arm, `cores = 1`, the reactor pinned, a preloaded 100,000-row relation,
three cells of --seconds each: a tuple key point `SELECT` and an autocommit
single-row `INSERT` on one connection (latency percentiles, server CPU a
statement), and a connect, one point read and close loop (server CPU a
connection). The arm order rotates and reverses per lifetime; each cell
holds for a quiet host (no compiler, linker or suite of another session)
and is run again when noise entered it, the hold counted.

Usage: dj_close.py --a BIN --b BIN --workdir D --out OUT.json
[--lifetimes 4] [--seconds 3] [--port 15691]
"""
import argparse, hashlib, json, os, random, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

TICK = os.sysconf("SC_CLK_TCK")
ROWS = 100_000
NOISE = {"cc1plus", "ld", "ctest", "kds_tests", "kds_bs_tests", "ckdbs-sim"}
HELD = [0.0]


def noisy():
    n = 0
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                n += open(f"/proc/{pid}/comm").read().strip() in NOISE
            except OSError:
                pass
    return n


NO_HOLD = [False]


def hold():
    if NO_HOLD[0]:
        return
    t0, seen = time.time(), 0
    while seen < 2:
        seen = seen + 1 if noisy() == 0 else 0
        time.sleep(1.0)
    HELD[0] += time.time() - t0 - 2.0


def cpu(pid):
    f = open(f"/proc/{pid}/stat").read()
    f = f[f.rindex(")") + 2:].split()
    return (int(f[11]) + int(f[12])) / TICK


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {k: us(nearest_rank(s, q)) for k, q in
            (("p25_us", 25), ("p50_us", 50), ("p75_us", 75), ("p95_us", 95), ("p99_us", 99))} | {
               "p0_us": us(s[0]), "n": len(s)}


class Server:
    def __init__(self, a, name, binary, keys):
        data = os.path.join(a.workdir, name + ".db")
        shutil.rmtree(data + ".wal", ignore_errors=True)
        if os.path.exists(data):
            os.remove(data)
        conf = os.path.join(a.workdir, name + ".conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {a.port}\ncores = 1\nreactor_cpus = 4\n"
                    f"durability = group\nbuffer_pool_frames = 65536\nlog_file = {name}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\n")
            if keys:
                f.write("max_connections = 100000\nidle_session_timeout_s = 3600\n")
        self.err = open(os.path.join(a.workdir, name + ".stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", "4-7", binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", a.port, timeout=60)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"{name}: no listener")
                time.sleep(0.1)
        self.conn.execute("CREATE TABLE c (id int64, a int64, b int64, s varchar(32)) BTREE")
        for lo in range(1, ROWS + 1, 500):
            self.conn.execute("INSERT INTO c VALUES " + ",".join(
                f"({i},{i % 97},{i * 7},'row{i}')" for i in range(lo, min(lo + 500, ROWS + 1))))

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


def statements(srv, seconds, make):
    lat, c0, t1 = [], cpu(srv.proc.pid), time.time() + seconds
    while time.time() < t1:
        sql = make()
        s = time.perf_counter()
        srv.conn.execute(sql)
        lat.append(time.perf_counter() - s)
    return {"stmts": len(lat), "server_cpu_us_per_stmt":
            round((cpu(srv.proc.pid) - c0) * 1e6 / len(lat), 2), "lat": pcts(lat)}


def connects(a, srv, seconds):
    n, c0, t1 = 0, cpu(srv.proc.pid), time.time() + seconds
    while time.time() < t1:
        c = kwp.Connection("127.0.0.1", a.port, timeout=60)
        c.execute(f"SELECT * FROM c WHERE id = {1 + n % ROWS}")
        c.close()
        n += 1
    return {"connections": n, "server_cpu_us_per_connection":
            round((cpu(srv.proc.pid) - c0) * 1e6 / n, 2)}


def cell(fn):
    for attempt in range(6):
        hold()
        r = fn()
        if NO_HOLD[0] or noisy() == 0:
            r["attempts"] = attempt + 1
            return r
    r["attempts"], r["noisy"] = 6, True
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a", required=True)
    p.add_argument("--b", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--lifetimes", type=int, default=4)
    p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--port", type=int, default=15691)
    p.add_argument("--status")
    p.add_argument("--no-hold", action="store_true", help="a smoke run: no quiet gate")
    p.add_argument("--lead", action="store_true",
                   help="open every lifetime with a discarded A point cell, so no measured "
                        "arm runs first (the first cell of a lifetime read dearer at DJ-S5)")
    a = p.parse_args()
    NO_HOLD[0] = a.no_hold
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {0})
    arms = {"A": (a.a, False), "B": (a.b, False), "Bk": (a.b, True), "A2": (a.a, False)}
    order = list(arms)
    out = {"args": vars(a), "sha256": {k: hashlib.sha256(open(v[0], "rb").read()).hexdigest()
                                       for k, v in arms.items()}, "cells": []}
    rng = random.Random(20261120)
    nxt = [10_000_000]

    def ins():
        nxt[0] += 1
        return f"INSERT INTO c VALUES ({nxt[0]},1,2,'x')"

    for life in range(a.lifetimes):
        o = order[life % 4:] + order[:life % 4]
        if life % 2:
            o = o[::-1]
        if a.lead:
            lead = Server(a, f"lead{life}", a.a, False)
            try:
                hold()
                statements(lead, a.seconds,
                           lambda: f"SELECT * FROM c WHERE id = {rng.randrange(1, ROWS)}")
            finally:
                lead.stop()
        for arm in o:
            if a.status:
                open(a.status, "a").write(f"{time.strftime('%H:%M:%S')} life {life} {arm}\n")
            binary, keys = arms[arm]
            srv = Server(a, f"{arm}{life}", binary, keys)
            try:
                statements(srv, 0.5, lambda: f"SELECT * FROM c WHERE id = {rng.randrange(1, ROWS)}")
                for name, fn in (
                        ("point", lambda: statements(
                            srv, a.seconds,
                            lambda: f"SELECT * FROM c WHERE id = {rng.randrange(1, ROWS)}")),
                        ("insert", lambda: statements(srv, a.seconds, ins)),
                        ("connect", lambda: connects(a, srv, a.seconds))):
                    r = cell(fn)
                    r.update(arm=arm, life=life, cell=name, load=open("/proc/loadavg").read()[:15])
                    out["cells"].append(r)
                    json.dump(out, open(a.out, "w"), indent=1)
            finally:
                srv.stop()
    out["held_for_quiet_s"] = round(HELD[0], 1)
    json.dump(out, open(a.out, "w"), indent=1)
    print("driver finished")


if __name__ == "__main__":
    main()
