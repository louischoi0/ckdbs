#!/usr/bin/env python3
"""Which cores actually do work, attributed to one phase on one configuration.

The matrix orchestrator samples per-core CPU across a whole
`tools/multicore_benchmark.py` invocation, which covers both the `cores = 1`
and the `cores = N` configuration plus two server starts - so its mean cannot
say what a *writer* core did while it was writing. This runs one
configuration, one phase, and samples only that window.

Two questions it answers, both of which the run instructions ask for and
neither of which a throughput ratio can:

  1. Is core 0 idle or a bottleneck under rotation? `AssignOwnerCore` rotates
     over the non-system cores only (include/kds/catalog/core_placement.hpp:96-104),
     so core 0 owns no relation - but it still carries DDL, the catalog and
     `AllocateRowIdRange` block carving, and a saturated core 0 would cap
     every writer.
  2. Are the writer cores CPU-bound or waiting? A writer core at 20% while
     throughput refuses to scale means the constraint is not the core.

It measures; it decides nothing.

Usage:
    bench/percore_insert_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/percore --cores 4 --tables 6 --rows 2000
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, collect_connections, field, is_retryable, wait_for_port,
)


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
        delta = [x - y for x, y in zip(a, b)]
        total = sum(delta)
        idle = delta[3] + delta[4]
        out[cpu] = round((total - idle) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


class Window:
    """Per-core busy over exactly the block it wraps."""

    def __enter__(self):
        self.before = read_cpu_jiffies()
        self.t0 = time.time()
        return self

    def __exit__(self, *exc):
        self.busy = busy_between(self.before, read_cpu_jiffies())
        self.seconds = time.time() - self.t0
        return False


def insert_worker(conn, name, rows, out, index, deadline_s=10.0):
    """One relation's INSERTs from its own session. Latencies are not the
    point here - the CPU window around every worker is.

    A peer answers its first INSERT with a lease-refill refusal until the
    grant lands, and those carry retryable=1 (PW1b); the benchmark driver
    retries them for exactly this reason. Without the retry this probe lost
    rows on the peer arm and still divided by the row count it *meant* to
    insert, overstating that arm's throughput.
    """
    errors = 0
    retries = 0
    inserted = 0
    first_error = None
    t0 = time.time()
    for i in range(1, rows + 1):
        # The Keystone pk is implicit - a column list is refused
        # (tools/multicore_benchmark.py:288). Only the non-pk values go here.
        stmt = f"INSERT INTO {name} VALUES ('o{i % 7}', {i * 10})"
        end = time.time() + deadline_s
        while True:
            r = conn.cmd(stmt)
            if not r.startswith("ERR"):
                inserted += 1
                break
            if is_retryable(r) and time.time() < end:
                retries += 1
                continue
            errors += 1
            if first_error is None:
                first_error = r
            break
    out[index] = dict(name=name, seconds=time.time() - t0, errors=errors,
                      retries=retries, inserted=inserted,
                      first_error=first_error)


def run_config(args, cores, placement, listeners, tag, port):
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, f"{tag}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, tag + '.db')}\n"
                f"port = {port}\ncores = {cores}\nplacement = {placement}\n"
                f"peer_listeners = {'on' if listeners else 'off'}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    result = dict(tag=tag, cores=cores, placement=placement,
                  peer_listeners=listeners)
    try:
        wait_for_port(port, stderr_path)
        time.sleep(2)  # let the mount's own work drain out of the window

        if listeners:
            got, _ = collect_connections(port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)

        names = [f"pc{i}" for i in range(args.tables)]
        owners = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} "
                          f"(id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owners[name] = int(field(setup.cmd(f"DESCRIBE {name}"), "owner_core"))
        result["owner_cores"] = owners

        if listeners:
            needed = collections.Counter(owners.values())
            per_core, _ = collect_connections(port, needed, args.max_connects)
            writers = {n: per_core[owners[n]].pop() for n in names}
        else:
            writers = {n: Conn(port) for n in names}
        setup.close()

        # An idle window on the same mount, for the comparison that makes the
        # busy window readable.
        with Window() as idle:
            time.sleep(args.idle_seconds)
        result["idle_busy"] = idle.busy

        out = [None] * len(names)
        threads = [threading.Thread(target=insert_worker,
                                    args=(writers[n], n, args.rows, out, i))
                   for i, n in enumerate(names)]
        with Window() as w:
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        result["insert_busy"] = w.busy
        result["insert_seconds"] = w.seconds
        result["per_relation"] = [r for r in out if r]
        # Divide by rows that actually landed: a lost row must lower this
        # number, never leave it flattering the arm that lost it.
        inserted = sum(r["inserted"] for r in out if r)
        result["inserted"] = inserted
        result["inserts_per_second"] = (
            inserted / w.seconds if w.seconds else 0.0)
        result["errors"] = sum(r["errors"] for r in out if r)
        result["retries"] = sum(r["retries"] for r in out if r)

        counts = {}
        for n in names:
            counts[n] = writers[n].cmd(f"SELECT COUNT(*) FROM {n}")
        result["counts"] = counts

        try:
            writers[names[0]].cmd("STOP")
        except OSError:
            pass
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
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15900)
    ap.add_argument("--idle-seconds", type=float, default=5.0)
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    findings = {"cores": args.cores, "tables": args.tables, "rows": args.rows}
    findings["multi"] = run_config(args, args.cores, "rotate", True,
                                   "multi", args.port)
    findings["single"] = run_config(args, 1, "creating", False,
                                    "single", args.port + 2)

    m, s = findings["multi"], findings["single"]
    findings["insert_ips_ratio"] = (
        m["inserts_per_second"] / s["inserts_per_second"]
        if s["inserts_per_second"] else None)

    print(json.dumps(findings, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(findings, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
