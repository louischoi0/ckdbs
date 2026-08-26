#!/usr/bin/env python3
"""T3's discriminator: is the four-core-server effect the engine, or the host?

`bench/v2.1.0` §11-3 records a `cores = 4` server beating a `cores = 1`
server by 1.071x aggregate and 1.457x on insert p50 **with everything on core
0 and nothing cross-core happening at all**, and leaves it undiscriminated,
naming three candidates: four WAL anchors, per-core extent leases, background
work moving off core 0. On this host the same effect is larger and **grows
with the core count** (§7's curve), which is what a per-core resource looks
like — and also what a *fourth* candidate looks like, one that list does not
name:

  **The peer reactors are idle, but they are not asleep.** A reactor with
  nothing to do blocks in `PollReady` for at most `max_idle_block_ms` = 10 ms
  (`include/kds/sched/scheduler.hpp`), so every peer core wakes ~100 times a
  second forever. On a modern server CPU that is the difference between a
  core in a deep C-state and a core that is merely idle, and it moves the
  package's residency, its uncore frequency and its wake latency — none of
  which the engine gets credit for.

This probe holds the engine at `cores = 1` and supplies that wake pattern
from **outside the process**: K helper processes, one pinned per other CPU,
each sleeping `--wake-ms` in a loop and doing nothing else. If a `cores = 1`
server with wakers reaches a `cores = N` server's throughput, the effect is
the host's idle behaviour and not the engine's multi-core machinery. If it
does not, the candidate is dead and the remaining three stand.

It decides nothing and patches nothing: the engine is the same binary in
every arm, and the only variable is whether other CPUs are being woken.

Usage:
    bench/idle_wakers_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench2/t3w --wakers 0,3,7 --tables 6 --rows 2000
"""

import argparse
import json
import multiprocessing
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from bench_common import nearest_rank  # noqa: E402
from multicore_benchmark import Conn, check_host, is_retryable, wait_for_port  # noqa: E402


def waker(cpu, wake_ms, stop):
    """One helper, pinned to `cpu`, waking every `wake_ms` and doing nothing.
    Deliberately not a spin: the thing being reproduced is an idle reactor's
    *wake cadence*, not its CPU use, and a spinner would confound the two."""
    try:
        os.sched_setaffinity(0, {cpu})
    except OSError:
        pass
    period = wake_ms / 1000.0
    while not stop.is_set():
        time.sleep(period)


def insert_worker(conn, table, rows, out, index, deadline_s=20.0):
    lat = []
    inserted = errors = retries = 0
    first_error = None
    for i in range(rows):
        stmt = f"INSERT INTO {table} VALUES ('w', {i})"
        t0 = time.perf_counter()
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
        lat.append(time.perf_counter() - t0)
    out[index] = dict(table=table, inserted=inserted, errors=errors,
                      retries=retries, first_error=first_error, lat=lat)


def run_arm(args, cores, wakers, port):
    """One (cores, wakers) arm: fresh server, fresh data file, insert phase
    only, with `wakers` helper processes pinned to the CPUs this server does
    not use."""
    tag = f"c{cores}-w{wakers}"
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\nport = {port}\n"
                f"cores = {cores}\nplacement = creating\npeer_listeners = off\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "s.stderr")

    stop = multiprocessing.Event()
    helpers = []
    # Pinned to the CPUs the server's own reactors do not take: a waker on
    # core 0 would compete with the one thread doing the work.
    for cpu in range(cores, cores + wakers):
        p = multiprocessing.Process(target=waker, args=(cpu, args.wake_ms, stop),
                                    daemon=True)
        p.start()
        helpers.append(p)

    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(cores=cores, wakers=wakers, wake_ms=args.wake_ms,
               tables=args.tables, rows=args.rows)
    try:
        wait_for_port(port, stderr_path)
        time.sleep(2)
        setup = Conn(port)
        names = [f"w{i}" for i in range(args.tables)]
        for n in names:
            r = setup.cmd(f"CREATE TABLE {n} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{n}: {r}")
        conns = {n: Conn(port) for n in names}
        setup.close()

        res = [None] * len(names)
        threads = [threading.Thread(target=insert_worker,
                                    args=(conns[n], n, args.rows, res, i))
                   for i, n in enumerate(names)]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - t0
        lat = sorted(x for r in res if r for x in r["lat"])
        inserted = sum(r["inserted"] for r in res if r)
        out.update(wall_s=round(wall, 4), inserted=inserted,
                   inserts_per_second=round(inserted / wall, 1) if wall else 0.0,
                   errors=sum(r["errors"] for r in res if r),
                   retries=sum(r["retries"] for r in res if r),
                   insert_p50_us=round(nearest_rank(lat, 50) * 1e6, 1) if lat else None,
                   insert_p99_us=round(nearest_rank(lat, 99) * 1e6, 1) if lat else None)
        for c in conns.values():
            c.close()
        stopper = Conn(port)
        stopper.cmd("STOP")
        stopper.close()
    finally:
        stop.set()
        for p in helpers:
            p.join(timeout=5)
            if p.is_alive():
                p.terminate()
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
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--wakers", default="0,3,7",
                    help="helper counts to run at cores=1 (0 is the control)")
    ap.add_argument("--cores-arms", default="1,4,8",
                    help="server core counts to run with no wakers, for the "
                         "curve the waker arms are compared against")
    ap.add_argument("--wake-ms", type=float, default=10.0,
                    help="a helper's wake period; 10 ms is the reactor's "
                         "max_idle_block_ms")
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--port", type=int, default=21000)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    port = args.port
    runs = []
    # Interleaved by repetition, not blocked by arm: drift across the run
    # would otherwise land wholly on one arm of the comparison, which is the
    # artifact the fdatasync probe's own header warns about.
    for rep in range(1, args.reps + 1):
        for cores in [int(c) for c in args.cores_arms.split(",") if c.strip()]:
            r = run_arm(args, cores, 0, port)
            port += 4
            r["rep"] = rep
            runs.append(r)
            print(f"  rep{rep} cores={cores} wakers=0: ips={r['inserts_per_second']} "
                  f"p50={r['insert_p50_us']} errors={r['errors']}", flush=True)
        for w in [int(x) for x in args.wakers.split(",") if x.strip()]:
            if w == 0:
                continue
            r = run_arm(args, 1, w, port)
            port += 4
            r["rep"] = rep
            runs.append(r)
            print(f"  rep{rep} cores=1 wakers={w}: ips={r['inserts_per_second']} "
                  f"p50={r['insert_p50_us']} errors={r['errors']}", flush=True)

    print("\n=== medians ===")
    summary = {}
    keys = sorted({(r["cores"], r["wakers"]) for r in runs})
    for key in keys:
        sel = [r for r in runs if (r["cores"], r["wakers"]) == key]
        ips = sorted(r["inserts_per_second"] for r in sel)
        p50 = sorted(r["insert_p50_us"] for r in sel)
        mid = len(ips) // 2
        row = dict(cores=key[0], wakers=key[1], reps=len(sel),
                   ips=ips[mid], ips_min=ips[0], ips_max=ips[-1], p50_us=p50[mid],
                   errors=sum(r["errors"] for r in sel))
        summary[f"c{key[0]}-w{key[1]}"] = row
        print(f"  cores={key[0]} wakers={key[1]}: ips={row['ips']} "
              f"({row['ips_min']}..{row['ips_max']}) p50={row['p50_us']}us "
              f"errors={row['errors']}")

    base = summary.get("c1-w0")
    if base:
        for k, row in summary.items():
            row["vs_c1_w0"] = round(row["ips"] / base["ips"], 4) if base["ips"] else None
        print("\n  against cores=1 with no wakers:")
        for k, row in summary.items():
            print(f"    {k}: {row['vs_c1_w0']}x")
    out = dict(summary=summary, runs=runs)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
