#!/usr/bin/env python3
"""T4 - what a reactor's time is actually spent on, per core, read from
outside the process for the first time.

`docs/spec/sched.md` §4's last bullet: *"reactor time spent outside task polls (the
drain, the idle block) is charged to no group"*. `bench/v2.1.0` §11-5 could
not report on it - the counter was private and `SHOW META` did not print it -
and §8 could only infer the spin from a lease-refill leg spanning
19,000-24,000 reactor iterations.

With T4's instrument the question is a subtraction. Per core:

    sched_wall_us - Σ sched_<group>_polled_us   = time charged to no group
    sched_<group>_polls / wall                  = poll rate
    polled_us / polls                           = what a poll costs

and a **spin** has a signature of its own: polls climbing while polled time
does not, because a parked coroutine answers `kSuspended` in nanoseconds.

Two arms on one mount, so the comparison is within a server:

  idle      nothing running, `--idle-seconds`
  loaded    the T1b workload - `--sessions` sessions inserting into one
            relation on its owner core - for `--seconds`

The loaded arm is where the engine creates its own parked population: every
lease refill parks a coroutine on the core waiting for it. That is the
population statement shipping would turn from occasional into steady-state,
and this is what it costs today.

Usage:
    bench/reactor_accounting_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench2/t4a --cores 4 --sessions 4
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from bench_common import nearest_rank  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, is_retryable, wait_for_port,
)

GROUPS = ("foreground", "maintenance", "system")


def read_cpu_jiffies():
    out = {}
    with open("/proc/stat") as fh:
        for line in fh:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            out[parts[0]] = [int(v) for v in parts[1:9]]
    return out


def busy_between(before, after):
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        d = [x - y for x, y in zip(a, b)]
        total = sum(d)
        out[cpu] = round((total - d[3] - d[4]) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


def sched_fields(meta):
    out = {}
    for tok in meta.split():
        if tok.startswith("sched_") and "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def accounting(before, after):
    """The subtraction §4 asks for, over one window on one core."""
    d = {k: after[k] - before.get(k, 0) for k in after}
    wall = d.get("sched_wall_us", 0)
    polled = sum(d.get(f"sched_{g}_polled_us", 0) for g in GROUPS)
    polls = sum(d.get(f"sched_{g}_polls", 0) for g in GROUPS)
    d["polled_us_total"] = polled
    d["polls_total"] = polls
    d["unaccounted_us"] = wall - polled
    d["unaccounted_fraction"] = round((wall - polled) / wall, 4) if wall else None
    d["polls_per_second"] = round(polls / (wall / 1e6), 1) if wall else None
    d["ns_per_poll"] = round(polled * 1000 / polls, 1) if polls else None
    return d


class Inserter(threading.Thread):
    def __init__(self, conn, tag, stop, deadline_s=20.0):
        super().__init__()
        self.conn = conn
        self.tag = tag
        self.stop = stop
        self.deadline_s = deadline_s
        self.lat = []
        self.inserted = self.retries = self.errors = 0
        self.first_error = None

    def run(self):
        i = 0
        while not self.stop.is_set():
            stmt = f"INSERT INTO hot VALUES ('{self.tag}', {i})"
            i += 1
            t0 = time.perf_counter()
            end = time.time() + self.deadline_s
            while True:
                r = self.conn.cmd(stmt)
                if not r.startswith("ERR"):
                    self.inserted += 1
                    break
                if is_retryable(r) and time.time() < end:
                    self.retries += 1
                    continue
                self.errors += 1
                if self.first_error is None:
                    self.first_error = r
                break
            self.lat.append(time.perf_counter() - t0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--sessions", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--idle-seconds", type=float, default=5.0)
    ap.add_argument("--port", type=int, default=23000)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    workdir = os.path.join(args.workdir, f"c{args.cores}-s{args.sessions}")
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\nport = {args.port}\n"
                f"cores = {args.cores}\nplacement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    err_path = os.path.join(workdir, "s.stderr")
    with open(err_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(cores=args.cores, sessions=args.sessions, seconds=args.seconds)
    try:
        wait_for_port(args.port, err_path)
        got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        setup = got[0][0]
        r = setup.cmd("CREATE TABLE hot (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(r)
        owner = int(field(setup.cmd("DESCRIBE hot"), "owner_core"))
        out["owner_core"] = owner

        # One reading session per core, kept for the whole run: SHOW META is
        # core-local, so an accounting reading needs one session per core and
        # the *same* session each time, or the delta spans two reactors.
        readers = {}
        per_core, _ = collect_connections(args.port, {c: 1 for c in range(args.cores)},
                                          args.max_connects)
        for core, cs in per_core.items():
            readers[core] = cs[0]

        writers = collect_connections(args.port, {owner: args.sessions},
                                      args.max_connects)[0][owner]
        end = time.time() + 20
        while True:
            r = writers[0].cmd("INSERT INTO hot VALUES ('warm', 0)")
            if not r.startswith("ERR"):
                break
            if is_retryable(r) and time.time() < end:
                time.sleep(0.001)
                continue
            raise RuntimeError(f"warm-up: {r}")

        # ---- idle arm ----------------------------------------------------
        before = {c: sched_fields(rd.cmd("SHOW META")) for c, rd in readers.items()}
        cpu_before = read_cpu_jiffies()
        time.sleep(args.idle_seconds)
        out["idle"] = {
            "busy": busy_between(cpu_before, read_cpu_jiffies()),
            "per_core": {str(c): accounting(before[c], sched_fields(rd.cmd("SHOW META")))
                         for c, rd in readers.items()},
        }

        # ---- loaded arm --------------------------------------------------
        before = {c: sched_fields(rd.cmd("SHOW META")) for c, rd in readers.items()}
        cpu_before = read_cpu_jiffies()
        stop = threading.Event()
        workers = [Inserter(c, f"s{i}", stop) for i, c in enumerate(writers)]
        t0 = time.perf_counter()
        for w in workers:
            w.start()
        time.sleep(args.seconds)
        stop.set()
        for w in workers:
            w.join()
        wall = time.perf_counter() - t0
        busy = busy_between(cpu_before, read_cpu_jiffies())
        after = {c: sched_fields(rd.cmd("SHOW META")) for c, rd in readers.items()}
        lat = sorted(x for w in workers for x in w.lat)
        out["loaded"] = {
            "seconds": round(wall, 4),
            "inserted": sum(w.inserted for w in workers),
            "ips": round(sum(w.inserted for w in workers) / wall, 1) if wall else 0.0,
            "retries": sum(w.retries for w in workers),
            "errors": sum(w.errors for w in workers),
            "first_error": next((w.first_error for w in workers if w.first_error), None),
            "p50_us": round(nearest_rank(lat, 50) * 1e6, 1) if lat else None,
            "p99_us": round(nearest_rank(lat, 99) * 1e6, 1) if lat else None,
            "busy": busy,
            "per_core": {str(c): accounting(before[c], after[c]) for c in readers},
        }
        for rd in readers.values():
            rd.close()
        for w in writers:
            w.close()
        setup.cmd("STOP")
        setup.close()
    finally:
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
