#!/usr/bin/env python3
"""R4/IS7 — what id-block-aligned insert spreading costs and buys, on ONE
relation written from several cores.

The existing `tools/multicore_benchmark.py` measures N **non-interfering**
relations, one per core. That is the isolation question and it is already
answered. R4's question is the opposite one: several cores writing the
*same* relation, which before this phase meant every peer shipped its
INSERT to the relation's owner and the owner serialised them.

So the arms differ in exactly one config key, on the same binary and the
same workload:

  arm C (concentrated)  range_size_ids = 0   - peers ship to the owner
  arm S (spread)        range_size_ids = N   - each peer takes a range of
                                               its own and inserts locally

and the **durability** axis is crossed with it, because
`docs/inflight/known-gaps.md` already found that spreading writers over
cores divides the group-commit batch and each core is then capped at the
volume's single-stream fdatasync rate
(`bench/v2.1.0/results-multicore-writers-v2.1.0.md` §6-§7). Under `group`
the prediction is therefore flat-to-slightly-down for a reason that has
nothing to do with ranges; under `relaxed` the fdatasync is off the
critical path and what is left is the thing R4 actually changes. Running
only the first arm would report v2.1.0's finding under R4's name.

One more thing this measures because only a running server can: **the
pump's latency**. A peer's first INSERT into a foreign relation is still
refused - it leaves the demand behind it and the drain tick turns that into
a range - so the driver counts how many retries that costs, per core. That
number is the client-visible price of arming spreading.

Usage:
    bench/spread_insert_probe.py --server build-release/kds_server \\
        --workdir ~/spread --cores 3 --rows 4000 --range-size-ids 4096

Prints one table and a JSON blob. It measures; it decides nothing.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, session_core, stop_server,
    wait_for_port,
)

TABLE = "spread"


def start_server(binary, workdir, tag, cores, port, range_size_ids, durability):
    """One server, one data file. `cores` is pinned into the superblock at
    bootstrap, so every configuration needs its own file - and so does every
    arm, since arm C must never have seen a range row."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(
            f"data_file = {data}\nport = {port}\ncores = {cores}\n"
            # `creating`, so the relation is core 0's and every peer is a
            # foreign writer to it - which is the shape spreading acts on.
            # `rotate` would put it on one peer and make the run measure
            # placement instead.
            "placement = creating\n"
            # Without this the kernel never accepts a session on a peer and
            # there is no foreign writer at all (PW5).
            "peer_listeners = on\n"
            f"range_size_ids = {range_size_ids}\n"
            f"durability = {durability}\n"
            f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc


# The cross-core write refusal used to be permanent for a session on the
# wrong core (`tools/multicore_benchmark.py`'s PERMANENT_TEXTS says so, and
# it was right). **R4/IS1 changes exactly that**: the refusal now leaves a
# row-id lease demand behind it, the drain tick asks core 0, and the retry
# finds a range this core owns. So here it is retryable - and how many
# retries it takes is one of the numbers this probe exists to report.
def is_retryable(reply):
    if not reply.startswith("ERR"):
        return False
    return ("retryable=1" in reply
            or "retry after the refill grant lands" in reply
            or "writes are bound to core" in reply)


def insert_loop(conn, core, rows, barrier, out, deadline_s=30.0):
    """`rows` inserts, retrying the refusals that mean 'again, later'.
    Records per-row latency and the retry count, which is the pump's cost."""
    latencies = []
    retries = 0
    first_ok_after = None
    barrier.wait()
    for n in range(rows):
        started = time.perf_counter()
        attempts = 0
        while True:
            reply = conn.cmd(f"INSERT INTO {TABLE} VALUES ({n})")
            if not reply.startswith("ERR"):
                break
            if not is_retryable(reply) or time.perf_counter() - started > deadline_s:
                out[core] = {"error": reply, "at_row": n}
                return
            retries += 1
            attempts += 1
            time.sleep(0.002)
        if first_ok_after is None:
            first_ok_after = attempts
        latencies.append(time.perf_counter() - started)
    out[core] = {
        "rows": rows,
        "retries": retries,
        "retries_before_first_row": first_ok_after,
        "p50_us": statistics.median(latencies) * 1e6,
        "mean_us": statistics.fmean(latencies) * 1e6,
    }


def run_arm(binary, workdir, tag, cores, port, rows, range_size_ids, durability):
    proc = start_server(binary, workdir, tag, cores, port, range_size_ids, durability)
    try:
        setup = Conn(port)
        # DDL is core 0's (PW4), and `collect_connections` is the only way to
        # get a session on a chosen core under SO_REUSEPORT.
        conns, attempts = collect_connections(port, {c: 1 for c in range(cores)},
                                              max_attempts=200 * cores)
        if session_core(setup) != 0:
            setup.close()
            setup = conns[0][0]
        reply = setup.cmd(f"CREATE TABLE {TABLE} (id int64, v int64)")
        if reply.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE: {reply}")

        out = {}
        barrier = threading.Barrier(cores)
        threads = [threading.Thread(target=insert_loop,
                                    args=(conns[c][0], c, rows, barrier, out))
                   for c in range(cores)]
        began = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - began

        meta = {c: conns[c][0].cmd("SHOW META") for c in range(cores)}
        for c in range(cores):
            conns[c][0].close()
        setup.close()
        errors = {c: r for c, r in out.items() if "error" in r}
        placed = sum(r.get("rows", 0) for r in out.values())
        return {
            "tag": tag,
            "cores": cores,
            "rows_per_core": rows,
            "rows_placed": placed,
            "wall_s": wall,
            "inserts_per_s": placed / wall if wall > 0 else 0.0,
            "connection_attempts": attempts,
            "per_core": out,
            "errors": errors,
            "rowid_refills": {
                c: {k: field(meta[c], f"rowid_refill_{k}")
                    for k in ("requests", "grants")}
                for c in range(cores) if "rowid_refill_requests=" in meta[c]
            },
            "range_split_declines": {
                c: field(meta[c], "range_split_declines")
                for c in range(cores) if "range_split_declines=" in meta[c]
            },
        }
    finally:
        stop_server(port)
        proc.wait(timeout=30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=3)
    ap.add_argument("--rows", type=int, default=2000, help="rows per core")
    ap.add_argument("--range-size-ids", type=int, default=4096)
    ap.add_argument("--port", type=int, default=15600)
    ap.add_argument("--durability", default="group,relaxed",
                    help="comma-separated arms; both are reported")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    results = []
    port = args.port
    for durability in args.durability.split(","):
        durability = durability.strip()
        for name, size in (("C-concentrated", 0), ("S-spread", args.range_size_ids)):
            tag = f"{name}-{durability}"
            results.append(run_arm(args.server, args.workdir, tag, args.cores, port,
                                   args.rows, size, durability))
            port += 1

    print(f"\n{'arm':<26} {'ips':>10} {'wall s':>8} {'placed':>8} {'retries':>8}")
    print("-" * 64)
    for r in results:
        retries = sum(p.get("retries", 0) for p in r["per_core"].values())
        print(f"{r['tag']:<26} {r['inserts_per_s']:>10.0f} {r['wall_s']:>8.2f} "
              f"{r['rows_placed']:>8} {retries:>8}")
        if r["errors"]:
            print(f"  ERRORS: {r['errors']}")
    print()
    for durability in args.durability.split(","):
        durability = durability.strip()
        pair = [r for r in results if r["tag"].endswith(durability)]
        if len(pair) == 2 and pair[0]["inserts_per_s"] > 0:
            print(f"{durability}: spread / concentrated = "
                  f"{pair[1]['inserts_per_s'] / pair[0]['inserts_per_s']:.3f}x")
    print()
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
