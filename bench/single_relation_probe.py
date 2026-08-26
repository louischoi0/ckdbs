#!/usr/bin/env python3
"""T1b - one relation, N sessions, ascending keys: the serialized baseline.

`bench/v2.1.0` §10 states what its matrix cannot see: *"the case that
motivates the stride-forest proposal - many writers contending on one
relation's ascending key - is not exercised at all"*. Every cell there runs N
non-interfering relations, one session each.

This runs the opposite shape. One relation, N sessions, every session
inserting rows whose Keystone pk the engine issues - so every insert lands at
the same ascending tail. Today all N sessions must sit on the relation's
owner core: rotation places exactly one owner, and a session elsewhere is
refused (`crosscore.md` CC3, and DML shipping is unbuilt), which is precisely
why this is a *serialized* baseline and not a scaling curve. Without the
number there is nothing for stride or statement shipping to be measured
against.

Two arms, chosen by `--arm`:

  multi   `cores = N`, `placement = rotate`, peer listeners on. The relation
          is core 1's, and all `--sessions` sessions are collected there.
  single  `cores = 1`. The relation and every session are core 0's. The
          control that says how much of the multi arm's number is the peer
          write path rather than the contention.

One session count per invocation, one fresh server per invocation: rows
accumulate in the relation and a second run on the same file would measure a
taller btree. The orchestrator sweeps.

Usage:
    bench/single_relation_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/t1b --arm multi --cores 4 --sessions 4 --rows 2000
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


class Inserter(threading.Thread):
    """One session's autocommit INSERTs into the shared relation.

    The latency recorded is the whole wait including retries - what the
    client experienced - and the retries are counted beside it, because a
    peer's lease refusals (PW1b and the trx-id/extent leases when spent) are
    a cost the percentiles would otherwise hide inside the tail.
    """

    def __init__(self, conn, table, rows, tag, barrier, deadline_s=20.0):
        super().__init__()
        self.conn = conn
        self.table = table
        self.rows = rows
        self.tag = tag
        self.barrier = barrier
        self.deadline_s = deadline_s
        self.lat = []
        self.inserted = 0
        self.retries = 0
        self.errors = 0
        self.first_error = None

    def run(self):
        self.barrier.wait()
        for i in range(self.rows):
            stmt = f"INSERT INTO {self.table} VALUES ('{self.tag}', {i})"
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


def pct(values, p):
    return round(nearest_rank(sorted(values), p) * 1e6, 1) if values else None


def count_of(reply):
    try:
        return int(reply.replace("\\n", "\n").split("\n")[-1].split(",")[-1])
    except (ValueError, AttributeError):
        return None


def run_once(args, port):
    multi = args.arm == "multi"
    cores = args.cores if multi else 1
    tag = f"{args.arm}-c{cores}-s{args.sessions}"
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "probe.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 'probe.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                f"placement = {'rotate' if multi else 'creating'}\n"
                f"peer_listeners = {'on' if multi else 'off'}\n"
                f"log_file = probe.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "probe.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(arm=args.arm, cores=cores, sessions=args.sessions, rows=args.rows,
               rows_total=args.rows * args.sessions)
    try:
        wait_for_port(port, stderr_path)
        if multi:
            got, _ = collect_connections(port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)
        r = setup.cmd("CREATE TABLE hot (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE hot: {r}")
        owner = int(field(setup.cmd("DESCRIBE hot"), "owner_core"))
        out["owner_core"] = owner
        if multi:
            per_core, attempts = collect_connections(port, {owner: args.sessions},
                                                     args.max_connects)
            conns = per_core[owner]
            out["connect_attempts"] = attempts
        else:
            conns = [Conn(port) for _ in range(args.sessions)]
        setup.close()

        # The first INSERT on a peer pays the row-id refill and the btree's
        # first extent (PW1b); both refuse retryably until the grant lands.
        # Paid here, before the window, so the sweep's session-1 point is not
        # a refill measurement.
        warm_retries = 0
        end = time.time() + args.retry_deadline
        while True:
            r = conns[0].cmd("INSERT INTO hot VALUES ('warm', 0)")
            if not r.startswith("ERR"):
                break
            if is_retryable(r) and time.time() < end:
                warm_retries += 1
                time.sleep(0.0005)
                continue
            raise RuntimeError(f"warm-up: {r}")
        out["warmup_retries"] = warm_retries

        barrier = threading.Barrier(args.sessions)
        workers = [Inserter(conns[i], "hot", args.rows, f"s{i}", barrier,
                            args.retry_deadline)
                   for i in range(args.sessions)]
        t0 = time.perf_counter()
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        wall = time.perf_counter() - t0

        inserted = sum(w.inserted for w in workers)
        lat = [x for w in workers for x in w.lat]
        out.update(
            wall_s=round(wall, 4),
            inserted=inserted,
            inserts_per_second=round(inserted / wall, 1) if wall else 0.0,
            retries=sum(w.retries for w in workers),
            errors=sum(w.errors for w in workers),
            first_error=next((w.first_error for w in workers if w.first_error), None),
            insert_p50_us=pct(lat, 50), insert_p99_us=pct(lat, 99),
            insert_p0_us=round(min(lat) * 1e6, 1) if lat else None,
            insert_p25_us=pct(lat, 25),
            insert_p75_us=pct(lat, 75),
        )
        # Every session's rows plus the warm-up row, counted from the engine.
        expected = args.rows * args.sessions + 1
        reply = conns[0].cmd("SELECT COUNT(*) FROM hot")
        got = count_of(reply)
        out["verify"] = ("rows as expected" if got == expected
                         else f"expected {expected} got {reply!r}")
        # A peer's refill legs, for the same reason the matrix reads them:
        # this shape puts every session on one core, which is where a refill
        # lag shows up first.
        if multi:
            out["meta"] = conns[0].cmd("SHOW META")
        for c in conns:
            c.close()
        stop = Conn(port)
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
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True, help="on a block device, never tmpfs")
    ap.add_argument("--arm", choices=("multi", "single"), default="multi")
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--sessions", type=int, default=1)
    ap.add_argument("--rows", type=int, default=2000,
                    help="rows per session; the relation takes sessions x rows")
    ap.add_argument("--port", type=int, default=16200)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--retry-deadline", type=float, default=20.0)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    out = run_once(args, args.port)
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
