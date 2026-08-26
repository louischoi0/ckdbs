#!/usr/bin/env python3
"""How fast can this harness go at all - the ceiling every T1a cell is read
against.

T1a's batched cells reach tens of thousands of inserts per second, and at
that rate the question stops being "what can the engine do" and becomes "what
can a CPython driver with N threads do". A number at the harness's own
ceiling measures the harness. This probe finds that ceiling and prints it, so
a cell sitting on it can be **reported as unresolved** rather than quoted as
an engine result.

Three arms, same connections and same thread count as a T1a cell:

  ping    `PING`, which the dispatcher answers before any parsing - the
          floor: socket round trip plus CPython.
  select  `SELECT * FROM <t> WHERE id = 1`, a parsed statement with a btree
          descent and no write - the engine doing real but sync-free work.
  insert  autocommit `INSERT`, for the contrast: one `fdatasync` per row is
          what makes T1a's unbatched cells slow, and it is nowhere near the
          ceiling.

The GIL is the thing being sized, so the arms are run at each thread count
and the *aggregate* is what matters, not the per-thread rate.

Usage:
    bench/client_ceiling_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench2/ceiling --threads 1,2,4,8,14
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
from multicore_benchmark import Conn, check_host, is_retryable, wait_for_port  # noqa: E402


class Looper(threading.Thread):
    def __init__(self, conn, stmt, seconds, barrier):
        super().__init__()
        self.conn = conn
        self.stmt = stmt
        self.seconds = seconds
        self.barrier = barrier
        self.ops = 0
        self.errors = 0
        self.first_error = None

    def run(self):
        self.barrier.wait()
        end = time.perf_counter() + self.seconds
        i = 0
        while time.perf_counter() < end:
            r = self.conn.cmd(self.stmt.format(i=i))
            i += 1
            if r.startswith("ERR"):
                if is_retryable(r):
                    continue
                self.errors += 1
                if self.first_error is None:
                    self.first_error = r
            self.ops += 1


def arm(port, stmt, threads, seconds):
    conns = [Conn(port) for _ in range(threads)]
    barrier = threading.Barrier(threads)
    workers = [Looper(c, stmt, seconds, barrier) for c in conns]
    t0 = time.perf_counter()
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    wall = time.perf_counter() - t0
    for c in conns:
        c.close()
    return dict(threads=threads, seconds=round(wall, 3),
                ops=sum(w.ops for w in workers),
                ops_per_second=round(sum(w.ops for w in workers) / wall, 1),
                errors=sum(w.errors for w in workers),
                first_error=next((w.first_error for w in workers if w.first_error), None))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--threads", default="1,2,4,8,14")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--port", type=int, default=18200)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    workdir = os.path.join(args.workdir, "ceiling")
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\nport = {args.port}\n"
                f"cores = 1\nplacement = creating\npeer_listeners = off\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "s.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = {"arms": {}}
    try:
        wait_for_port(args.port, stderr_path)
        setup = Conn(args.port)
        r = setup.cmd("CREATE TABLE c (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(r)
        for i in range(10):
            setup.cmd(f"INSERT INTO c VALUES ('x', {i})")
        setup.close()
        for t in [int(x) for x in args.threads.split(",") if x.strip()]:
            out["arms"][f"ping-{t}"] = arm(args.port, "PING", t, args.seconds)
            out["arms"][f"select-{t}"] = arm(
                args.port, "SELECT * FROM c WHERE id = 1", t, args.seconds)
            out["arms"][f"insert-{t}"] = arm(
                args.port, "INSERT INTO c VALUES ('y', {i})", t, args.seconds)
            for kind in ("ping", "select", "insert"):
                a = out["arms"][f"{kind}-{t}"]
                print(f"  {kind:<7} threads={t:<3} {a['ops_per_second']:>10,.0f} ops/s "
                      f"errors={a['errors']}", flush=True)
        stop = Conn(args.port)
        stop.cmd("STOP")
        stop.close()
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
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
