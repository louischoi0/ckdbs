#!/usr/bin/env python3
"""R4-M — the driver's own ceiling, in the driver's own process model.

`bench/client_ceiling_probe.py` answers this question for a **thread**-based
harness against a one-core server, which is what T1a's cells used. R4-M's k
sweep uses one **process** per writer core against a k-core server, so its
ceiling is a different number and the existing probe cannot supply it.

Why it has to exist at all: the sweep runs k engine reactors *and* k client
processes on one host, and this host has eight logical CPUs on four
physical cores with reactors pinned one per CPU index
(`expeditor.cpp`'s `CPU_SET(core_id)`). Past k = 4 a new reactor shares a
physical core with an existing one, and past that the clients are competing
with reactors for every CPU there is. **A sweep cell whose throughput sits
on this curve is measuring the harness**, and is reported as unresolved
rather than quoted as an engine result.

Three arms, each k processes each on its own core's session:

  ping    `PING`, answered before any parsing - socket round trip plus
          CPython, and the ceiling proper.
  select  a pk lookup - the engine doing real, sync-free work.
  insert  what the sweep actually runs, `durability = relaxed`, so the
          gap between this and `ping` is the engine's share.

Usage:
    bench/spread_client_ceiling.py --server build-release/kds_server \\
        --workdir ~/ceilingctl --cores 1,2,4,8 --seconds 3
"""

import argparse
import json
import multiprocessing
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, session_core, stop_server, wait_for_port,
)

TABLE = "ceil"


def looper(port, core, stmt, seconds, barrier, queue):
    conn = None
    try:
        conns, _ = collect_connections(port, {core: 1}, max_attempts=400)
        conn = conns[core][0]
        barrier.wait()
        ops = errors = 0
        end = time.monotonic() + seconds
        began = time.monotonic()
        i = 0
        while time.monotonic() < end:
            reply = conn.cmd(stmt.format(i=i))
            i += 1
            if reply.startswith("ERR"):
                errors += 1
            ops += 1
        queue.put((core, {"ops": ops, "errors": errors, "began": began,
                          "ended": time.monotonic()}))
    except BaseException as exc:
        queue.put((core, {"ops": 0, "error": f"{type(exc).__name__}: {exc}"}))
    finally:
        if conn is not None:
            conn.close()


def run(port, cores, stmt, seconds):
    ctx = multiprocessing.get_context("fork")
    queue = ctx.Queue()
    barrier = ctx.Barrier(cores)
    procs = [ctx.Process(target=looper, args=(port, c, stmt, seconds, barrier, queue))
             for c in range(cores)]
    for p in procs:
        p.start()
    out = {}
    for _ in procs:
        core, result = queue.get(timeout=seconds + 120)
        out[core] = result
    for p in procs:
        p.join(timeout=30)
    stamps = [r for r in out.values() if "began" in r]
    wall = (max(r["ended"] for r in stamps) - min(r["began"] for r in stamps)
            if stamps else 0.0)
    ops = sum(r.get("ops", 0) for r in out.values())
    return {"cores": cores, "wall_s": round(wall, 3), "ops": ops,
            "ops_per_s": round(ops / wall, 1) if wall > 0 else 0.0,
            "errors": sum(r.get("errors", 0) for r in out.values()),
            "per_core": out}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", default="1,2,3,4,5,6,7,8")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--port", type=int, default=16500)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    results = []
    port = args.port
    for k in [int(x) for x in args.cores.split(",")]:
        tag = f"ceil-k{k}"
        conf = os.path.join(args.workdir, f"{tag}.conf")
        stderr_path = os.path.join(args.workdir, f"{tag}.stderr")
        with open(conf, "w") as f:
            f.write(f"data_file = {args.workdir}/{tag}.db\nport = {port}\ncores = {k}\n"
                    "placement = creating\n"
                    + ("peer_listeners = on\n" if k > 1 else "")
                    + "durability = relaxed\n"
                    f"log_dir = {args.workdir}\nlog_file = {tag}.log\nlog_level = warn\n")
        subprocess.run(["rm", "-rf", f"{args.workdir}/{tag}.db",
                        f"{args.workdir}/{tag}.db.wal"], check=False)
        with open(stderr_path, "w") as err:
            proc = subprocess.Popen([args.server, "--config", conf], stdout=err,
                                    stderr=subprocess.STDOUT)
        try:
            wait_for_port(port, stderr_path)
            setup = Conn(port)
            if session_core(setup) != 0:
                probe, _ = collect_connections(port, {0: 1}, max_attempts=200 * k)
                setup.close()
                setup = probe[0][0]
            # BTREE, so the `select` arm is a descent rather than a scan and
            # the relation stays unsplittable - this probe measures the
            # harness, and a relation that spread would put the engine's own
            # refusals into the control.
            reply = setup.cmd(f"CREATE TABLE {TABLE} (id int64, v int64) BTREE")
            if reply.startswith("ERR"):
                raise RuntimeError(reply)
            for i in range(10):
                setup.cmd(f"INSERT INTO {TABLE} VALUES ({i})")
            setup.close()
            for name, stmt in (("ping", "PING"),
                               ("select", f"SELECT * FROM {TABLE} WHERE id = 1"),
                               ("insert", f"INSERT INTO {TABLE} VALUES ({{i}})")):
                row = run(port, k, stmt, args.seconds)
                row["arm"] = name
                results.append(row)
                print(f"  {name:<7} k={k:<2} {row['ops_per_s']:>12,.0f} ops/s  "
                      f"errors={row['errors']}", flush=True)
        finally:
            stop_server(port)
            proc.wait(timeout=30)
        port += 1

    print(f"\n{'k':>3} {'ping ops/s':>13} {'select ops/s':>14} {'insert ops/s':>14}")
    print("-" * 48)
    for k in [int(x) for x in args.cores.split(",")]:
        by = {r["arm"]: r for r in results if r["cores"] == k}
        print(f"{k:>3} {by['ping']['ops_per_s']:>13,.0f} "
              f"{by['select']['ops_per_s']:>14,.0f} {by['insert']['ops_per_s']:>14,.0f}")
    print()
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
