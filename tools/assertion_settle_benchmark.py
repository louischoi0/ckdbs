#!/usr/bin/env python3
"""What `AssertionEnforcer::CommitTxn`'s settle costs per commit, base binary
against a spike binary, interleaved in one window (CK-S0 part 4, H3).

`instructions/v3.2.0/workorder-ck-assertion-commit-settle.md` §1.2/§1.7: at
commit the settle fetches each page a transaction's reservations landed on,
appends one `ASSERT_COMMIT` per (assertion, page), clears the reserved bits
and stamps the page, all before the commit record. The spike binary's
`CommitTxn` only erases the pending list. This driver prices the gap.
`assertion_benchmark.py` autocommits every statement and has no
transaction-size or assertion-count knob, and `assertion_abort_benchmark.py`
is the abort half, so this is a sibling rather than a change to either.

The relations, all BTREE, identical preload (`--rows`, grp = i % 64):

    stl_a0   no assertion - the control. The settle has nothing to do here,
             so its base-vs-spike delta is the noise floor and must not move.
    stl_a1   one assertion  : CHECK COUNT(*) <= N GROUP BY (grp)
    stl_a4   four assertions: two COUNT ceilings and two SUM ceilings, all
             GROUP BY (grp), so one INSERT makes four reservations.

The unit measured is one explicit transaction `BEGIN; K x INSERT; COMMIT`.
Every INSERT names a group no earlier row has (a counter above the preload's
groups), so each INSERT is one fresh reservation per assertion, and the
ceilings (`--ceiling`) are far above anything reachable, so nothing is
refused (errors are counted and a non-zero count fails the run).

One *window* is `--window-stmts` statements' worth of such transactions on
one (side, relation, K): server CPU (utime+stime of the server's pid, read
around the whole window so one 10 ms tick is a rounding error) divided by the
transactions in it, plus every statement's wall latency. Windows are
interleaved across sides and relations; the side order alternates every round
(base,spike / spike,base) so a position effect cancels. Round 0 is a warm-up
and is discarded. Run-to-run spread = the range of the per-round
spike-minus-base delta over the measured rounds.

The servers are started by this driver from the copies of the two binaries,
pinned with `taskset -c --server-cpus`, and the driver should itself be run
under `taskset -c <other cpus>` so it shares no CPU with either. Both servers
stay up for the whole configuration (fresh data file each), but only one is
driven at a time; an undriven reactor blocks (bounded), it does not spin.

    taskset -c 2,3 python3 tools/assertion_settle_benchmark.py \\
        --base-bin RUN/kds_server.base --spike-bin RUN/kds_server.spike \\
        --workdir RUN/w --cores 1 --durability relaxed --json RUN/c1-relaxed.json
"""

import argparse
import json
import os
import shutil
import socket
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_common
import sys_meta
from ckdbs_cli import ServerConnection

AMOUNT_MAX = 1000
PRELOAD_GROUPS = 64
FRESH_GROUP_BASE = 1_000_000
SIDES = ("base", "spike")


class Conn(ServerConnection):
    def __init__(self, port, timeout):
        super().__init__("127.0.0.1", port, timeout=timeout)

    def send(self, line):
        return self.send_command(line)

    def must(self, line):
        reply = self.send(line)
        if reply.startswith("ERR"):
            raise RuntimeError("%s -> %s" % (line, reply))
        return reply


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    tick = os.sysconf("SC_CLK_TCK")
    return (int(fields[11]) / tick, int(fields[12]) / tick)  # (user, system)


class Side:
    def __init__(self, label, binary, wd, port, args):
        self.label = label
        os.makedirs(wd, exist_ok=True)
        data = os.path.join(wd, "d.db")
        conf = os.path.join(wd, "s.conf")
        with open(conf, "w") as f:
            f.write("data_file = %s\nport = %d\ncores = %d\ndurability = %s\n"
                    "buffer_pool_frames = %d\nlog_file = s.log\nlog_dir = %s\n"
                    "log_level = warn\nreactor_cpus = %s\n"
                    % (data, port, args.cores, args.durability, args.frames, wd,
                       ",".join(str(c) for c in args.reactor_cpus)))
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                raise SystemExit("port %d already in use" % port)
        self.err = open(os.path.join(wd, "s.stderr"), "w")
        self.proc = subprocess.Popen(
            ["taskset", "-c", args.server_cpus, binary, "--config", conf],
            stdout=self.err, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = Conn(port, args.timeout)
                break
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise SystemExit("server did not listen; see %s/s.stderr" % wd)
                time.sleep(0.1)
        self.pid = self.proc.pid
        self.next_group = FRESH_GROUP_BASE
        self.names = {a: "stl_a%d" % a for a in args.assertions}
        self.expected_rows = {a: args.rows for a in args.assertions}
        self.errors = 0

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def declare(side, args):
    for a, name in side.names.items():
        side.conn.must("CREATE TABLE %s (id int64, grp int64, amount int64, "
                       "note varchar) BTREE" % name)
    side.conn.must("BEGIN")
    for i in range(args.rows):
        for name in side.names.values():
            side.conn.must("INSERT INTO %s VALUES (%d, %d, 'r%d')"
                           % (name, i % PRELOAD_GROUPS, (i * 7) % AMOUNT_MAX, i))
        if (i + 1) % 200 == 0:
            side.conn.must("COMMIT")
            side.conn.must("BEGIN")
    side.conn.must("COMMIT")
    ceil = args.ceiling
    defs = {
        1: ["CHECK COUNT(*) <= %d" % ceil],
        4: ["CHECK COUNT(*) <= %d" % ceil, "CHECK SUM(amount) <= %d" % (ceil * AMOUNT_MAX),
            "CHECK COUNT(*) <= %d" % (ceil + 1),
            "CHECK SUM(amount) <= %d" % (ceil * AMOUNT_MAX + 1)],
    }
    made = []
    for a, checks in defs.items():
        if a not in side.names:
            continue
        for n, check in enumerate(checks):
            nm = "stl_as%d_%d" % (a, n)
            side.conn.must("CREATE ASSERTION %s ON %s GROUP BY (grp) %s"
                           % (nm, side.names[a], check))
            made.append(nm)
    reply = side.conn.must("SELECT name, enforcing FROM sys.assertions")
    state = {r["name"]: r["enforcing"] for r in sys_meta.view_rows(reply)}
    bad = [n for n in made if state.get(n) != "1"]
    if bad:
        raise RuntimeError("not enforcing: %s" % bad)
    return len(made)


class Meter:
    def __init__(self):
        self.lat = {}      # (side, a, k, arm) -> [seconds]
        self.cpu = {}      # (round, side, a, k) -> (user_s, sys_s, txns, wall_s)

    def add(self, key, v):
        self.lat.setdefault(key, []).append(v)


def window(side, meter, rnd, a, k, txns, record):
    name = side.names[a]
    # the statements are built before the clock starts; formatting is not server time
    batches = []
    for _ in range(txns):
        rows = []
        for _ in range(k):
            side.next_group += 1
            rows.append("INSERT INTO %s VALUES (%d, %d, 'x')"
                        % (name, side.next_group, side.next_group % AMOUNT_MAX))
        batches.append(rows)
    side.expected_rows[a] += txns * k
    send = side.conn.send
    perf = time.perf_counter
    lat_begin, lat_ins, lat_commit, lat_txn = [], [], [], []
    u0, s0 = cpu_seconds(side.pid)
    w0 = perf()
    for rows in batches:
        t0 = perf()
        r = send("BEGIN")
        t1 = perf()
        lat_begin.append(t1 - t0)
        if r.startswith("ERR"):
            side.errors += 1
        for stmt in rows:
            t = perf()
            r = send(stmt)
            lat_ins.append(perf() - t)
            if r.startswith("ERR"):
                side.errors += 1
        t2 = perf()
        r = send("COMMIT")
        t3 = perf()
        lat_commit.append(t3 - t2)
        lat_txn.append(t3 - t0)
        if r.startswith("ERR"):
            side.errors += 1
    wall = perf() - w0
    u1, s1 = cpu_seconds(side.pid)
    if record:
        meter.cpu[(rnd, side.label, a, k)] = (u1 - u0, s1 - s0, txns, wall)
        for arm, v in (("begin", lat_begin), ("insert", lat_ins),
                       ("commit", lat_commit), ("txn", lat_txn)):
            for x in v:
                meter.add((side.label, a, k, arm), x)


def pctl(vals):
    p = bench_common.Phase("x")
    p.latencies = vals
    s = p.summary()
    return {key: s[key] for key in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "mean_us")} | {"ops": s["ops"]}


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                          capture_output=True, text=True).stdout.strip()
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base-bin", required=True)
    p.add_argument("--spike-bin", required=True)
    p.add_argument("--workdir", required=True, help="on a block device, never tmpfs")
    p.add_argument("--ports", default="17301,17302", help="base,spike")
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--server-cpus", default="0,1", help="taskset list for the server")
    p.add_argument("--reactor-cpus", default="0", help="reactor_cpus, exactly --cores CPUs")
    p.add_argument("--assertions", default="0,1,4")
    p.add_argument("--reservations", default="1,10,100")
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--rounds", type=int, default=5, help="measured rounds (plus one warm-up)")
    p.add_argument("--window-stmts", type=int, default=3000,
                   help="statements per window (transactions = this // (K+2))")
    p.add_argument("--ceiling", type=int, default=10 ** 9)
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--json", default=None)
    args = p.parse_args()
    args.assertions = [int(x) for x in args.assertions.split(",")]
    args.reservations = [int(x) for x in args.reservations.split(",")]
    args.reactor_cpus = [int(x) for x in args.reactor_cpus.split(",")]
    if len(args.reactor_cpus) != args.cores:
        raise SystemExit("--reactor-cpus must name exactly --cores CPUs")
    ports = [int(x) for x in args.ports.split(",")]
    bins = {"base": args.base_bin, "spike": args.spike_bin}

    sides = {}
    out = {"args": vars(args), "host_before": host_state(), "cells": []}
    try:
        for label, port in zip(SIDES, ports):
            sides[label] = Side(label, bins[label], os.path.join(args.workdir, label), port, args)
            n = declare(sides[label], args)
        meter = Meter()
        arms = [(a, k) for k in args.reservations for a in args.assertions]
        for rnd in range(args.rounds + 1):
            record = rnd > 0
            order = SIDES if rnd % 2 == 0 else SIDES[::-1]
            for k in args.reservations:
                txns = max(10, args.window_stmts // (k + 2))
                for a in args.assertions:
                    for label in order:
                        window(sides[label], meter, rnd, a, k, txns, record)
            print("round %d done (%s) load %s" % (rnd, "measured" if record else "warm-up",
                  open("/proc/loadavg").read().split()[0]), flush=True)
        out["host_after"] = host_state()
        # verification: row counts, and zero errors
        problems = []
        for label, side in sides.items():
            if side.errors:
                problems.append("%s: %d refused statements" % (label, side.errors))
            for a, name in side.names.items():
                r = side.conn.must("SELECT COUNT(*) FROM %s" % name)
                got = int("".join(c for c in r.split("\\n")[-1] if c.isdigit()))
                if got != side.expected_rows[a]:
                    problems.append("%s/%s COUNT(*)=%d expected %d" % (label, name, got, side.expected_rows[a]))
        out["verify_problems"] = problems
        # ping floor
        for label, side in sides.items():
            v = []
            for _ in range(500):
                t = time.perf_counter()
                side.conn.send("SHOW META")
                v.append(time.perf_counter() - t)
            out.setdefault("ping_us", {})[label] = pctl(v)
        # cells
        for k in args.reservations:
            for a in args.assertions:
                cell = {"a": a, "k": k, "cores": args.cores, "durability": args.durability}
                for label in SIDES:
                    lat = {arm: pctl(meter.lat[(label, a, k, arm)])
                           for arm in ("begin", "insert", "commit", "txn")}
                    per_round = []
                    for rnd in range(1, args.rounds + 1):
                        u, s, t, w = meter.cpu[(rnd, label, a, k)]
                        per_round.append({"cpu_us_per_txn": (u + s) / t * 1e6,
                                          "user_us": u / t * 1e6, "sys_us": s / t * 1e6,
                                          "wall_us_per_txn": w / t * 1e6, "txns": t,
                                          "tps": t / w})
                    cell[label] = {"lat": lat, "rounds": per_round}
                out["cells"].append(cell)
    finally:
        for side in sides.values():
            side.stop()
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)
    print(json.dumps({"verify_problems": out.get("verify_problems")}))
    return 1 if out.get("verify_problems") else 0


if __name__ == "__main__":
    sys.exit(main())
