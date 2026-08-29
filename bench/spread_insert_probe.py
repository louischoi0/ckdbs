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

## What R4-M (`instructions/v2.6.0/r4-k-sweep.md`) added, and why

IS7 ran this probe once, at one k, with one arm after another. Two things
had to change before a **sweep** off it could mean anything:

- **Writers are processes, not threads.** The loop is one synchronous
  request and reply per row, so a CPython thread holds the GIL for
  everything between the two socket calls. At k = 2 that is invisible; at
  k = 8 the driver would bind before the engine did and the curve reported
  would be CPython's. `bench/client_ceiling_probe.py` is the control that
  says which - run it at the same thread counts and read any cell sitting
  on its ceiling as unresolved.
- **Arms are rep-interleaved** (`--reps`), the standing rule for this line
  since RD9(a) found a sequential sweep reversed a sign. One rep runs
  every (k, durability, arm) cell once, and the medians come from the
  reps, so drift that arrives mid-run lands on both arms rather than on
  the one that happened to run second.

Usage:
    bench/spread_insert_probe.py --server build-release/kds_server \\
        --workdir ~/spread --cores 3 --rows 4000 --range-size-ids 4096

    # the R4-M k sweep: every k, both arms, both durability classes
    bench/spread_insert_probe.py --server build-release/kds_server \\
        --workdir ~/ksweep --cores 1,2,3,4,6,8 --rows 4000 --reps 3

Prints one table and a JSON blob. It measures; it decides nothing.
"""

import argparse
import json
import multiprocessing
import os
import statistics
import subprocess
import sys
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
            # **`creating` on both arms**, which is the shape §6b describes
            # and the one that spreads widest: the relation is core 0's, so
            # every peer is a foreign writer that takes a range of its own
            # once armed, and core 0 keeps its direct allocator. Under
            # `rotate` the relation lands on a *peer*, and on a host with
            # only one peer there would be no second writer at all -
            # `rotate` measures placement, not spreading.
            #
            # This pairing was refused at startup until 2026-08-29; the
            # refusal's premise ("a peer session could serve nothing") had
            # been false since statement shipping and is doubly false with
            # ranges. `expeditor.cpp` carries the argument.
            "placement = creating\n"
            # Without this the kernel never accepts a session on a peer and
            # there is no foreign writer at all (PW5).
            #
            # **Omitted at `cores = 1`, where the engine refuses it** -
            # *"peer_listeners = on with cores = 1 has no peer to listen;
            # the only effect would be losing the exclusive bind on the one
            # socket"*. So the k = 1 cell runs today's single-core
            # configuration exactly, which is what HK5 asks of it: it is the
            # unmoved baseline and not a one-core copy of the spread arm.
            + ("peer_listeners = on\n" if cores > 1 else "")
            +
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


def insert_loop(port, core, rows, barrier, queue, deadline_s=30.0):
    """`rows` inserts on a session this process finds on `core`, retrying the
    refusals that mean 'again, later'. Records per-row latency and the retry
    count, which is the pump's cost.

    **One process per writer core.** This loop is a synchronous send-and-read
    per row, so a thread would hold the GIL for everything between the two
    socket calls and the driver would become the ceiling before the engine
    did - invisible at k = 2, decisive at k = 8. Each process therefore opens
    its own connections and keeps the one the kernel put on its target core;
    nothing is shared but the barrier and the result queue.

    Clock is `time.monotonic`, not `perf_counter`: the parent computes the
    aggregate wall from the children's own stamps, and only CLOCK_MONOTONIC
    is comparable between processes on this platform.
    """
    conn = None
    try:
        conns, attempts = collect_connections(port, {core: 1}, max_attempts=400)
        conn = conns[core][0]
        latencies = []
        retries = 0
        first_ok_after = None
        barrier.wait()
        began = time.monotonic()
        for n in range(rows):
            started = time.monotonic()
            tries = 0
            while True:
                reply = conn.cmd(f"INSERT INTO {TABLE} VALUES ({n})")
                if not reply.startswith("ERR"):
                    break
                if not is_retryable(reply) or time.monotonic() - started > deadline_s:
                    queue.put((core, {"error": reply, "at_row": n,
                                      "began": began, "ended": time.monotonic()}))
                    return
                retries += 1
                tries += 1
                time.sleep(0.002)
            if first_ok_after is None:
                first_ok_after = tries
            latencies.append(time.monotonic() - started)
        ended = time.monotonic()
        queue.put((core, {
            "rows": rows,
            "retries": retries,
            "retries_before_first_row": first_ok_after,
            "connection_attempts": attempts,
            "began": began,
            "ended": ended,
            "p50_us": statistics.median(latencies) * 1e6,
            "mean_us": statistics.fmean(latencies) * 1e6,
            # p99 as well as p50: the pump's retry and the group committer's
            # batch both live in the tail, and a median hides each.
            "p99_us": sorted(latencies)[min(len(latencies) - 1,
                                            int(len(latencies) * 0.99))] * 1e6,
        }))
    except BaseException as exc:  # a child must never die silently
        queue.put((core, {"error": f"driver: {type(exc).__name__}: {exc}"}))
    finally:
        if conn is not None:
            conn.close()


def run_arm(binary, workdir, tag, cores, port, rows, range_size_ids, durability):
    proc = start_server(binary, workdir, tag, cores, port, range_size_ids, durability)
    try:
        setup = Conn(port)
        # DDL is core 0's (PW4), and `collect_connections` is the only way to
        # get a session on a chosen core under SO_REUSEPORT.
        if session_core(setup) != 0:
            probe, attempts = collect_connections(port, {0: 1}, max_attempts=200 * cores)
            setup.close()
            setup = probe[0][0]
        else:
            attempts = 1
        reply = setup.cmd(f"CREATE TABLE {TABLE} (id int64, v int64)")
        if reply.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE: {reply}")

        ctx = multiprocessing.get_context("fork")
        queue = ctx.Queue()
        barrier = ctx.Barrier(cores)
        writers = [ctx.Process(target=insert_loop,
                               args=(port, c, rows, barrier, queue))
                   for c in range(cores)]
        for w in writers:
            w.start()
        out = {}
        for _ in writers:
            core, result = queue.get(timeout=600)
            out[core] = result
        for w in writers:
            w.join(timeout=60)

        # The aggregate wall is first barrier release to last row placed,
        # across processes - not the parent's spawn-to-join, which would
        # charge `fork` and the per-child connection hunt to the engine.
        stamps = [r for r in out.values() if "began" in r and "ended" in r]
        wall = (max(r["ended"] for r in stamps) - min(r["began"] for r in stamps)
                if stamps else 0.0)

        meta = {}
        for c in range(cores):
            peek, _ = collect_connections(port, {c: 1}, max_attempts=200 * cores)
            meta[c] = peek[c][0].cmd("SHOW META")
            peek[c][0].close()
        # **rows in = rows out** (order §6): asked of the server, not summed
        # from the driver's own bookkeeping, because RB6's driver produced
        # duplicate rows by retrying an ERR that followed a commit and its
        # own count agreed with itself throughout.
        #
        # `next_id` and not `COUNT(*)`, and the reason is a finding rather
        # than a convenience: **a relation spread under `placement =
        # creating` cannot be read from any core.** The fan-in route
        # requires the reader not to be the relation's `owner_core`, which
        # under `creating` is core 0 for every relation, and a peer has no
        # fan-in client at all (`expeditor.cpp` builds `remote_reads_` for
        # core 0 alone). So `COUNT(*)` here answers a refusal on the spread
        # arm and a number on the control, which would compare nothing.
        # `DESCRIBE` reads `sys.tables` and is answered on any core, so it
        # is the one server-side witness both arms share: ids issued must
        # equal rows placed plus what the run burnt, and a driver that
        # double-counted a retried commit shows up as the two disagreeing.
        counted = setup.cmd(f"SELECT COUNT(*) FROM {TABLE}")[:160]
        described = setup.cmd(f"DESCRIBE {TABLE}")
        next_id = field(described, "next_id") if "next_id=" in described else None
        setup.close()
        errors = {c: r for c, r in out.items() if "error" in r}
        placed = sum(r.get("rows", 0) for r in out.values())
        return {
            "tag": tag,
            "cores": cores,
            "rows_per_core": rows,
            "rows_placed": placed,
            "rows_counted": counted,
            "next_id": next_id,
            "ids_burnt": (next_id - 1 - placed) if isinstance(next_id, int) else None,
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
    ap.add_argument("--cores", default="3",
                    help="writer cores; a comma list sweeps k (e.g. 1,2,4,8)")
    ap.add_argument("--rows", type=int, default=2000, help="rows per core")
    ap.add_argument("--range-size-ids", type=int, default=4096)
    ap.add_argument("--port", type=int, default=15600)
    ap.add_argument("--durability", default="group,relaxed",
                    help="comma-separated arms; both are reported")
    ap.add_argument("--reps", type=int, default=1,
                    help="repetitions; the cells are interleaved across them "
                         "and the medians come from the reps (RD9(a)'s rule)")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    ks = [int(k) for k in args.cores.split(",")]
    durabilities = [d.strip() for d in args.durability.split(",")]
    arms = (("C-concentrated", 0), ("S-spread", args.range_size_ids))

    # **Rep-interleaved**: the outer loop is the rep, so one pass touches
    # every cell once and drift arriving mid-run lands on both arms. A
    # sweep that ran all of arm C and then all of arm S is the shape RD9(a)
    # found reversing a sign.
    results = []
    port = args.port
    for rep in range(args.reps):
        for k in ks:
            for durability in durabilities:
                for name, size in arms:
                    tag = f"{name}-{durability}-k{k}"
                    row = run_arm(args.server, args.workdir, f"{tag}-r{rep}", k, port,
                                  args.rows, size, durability)
                    row["cell"] = tag
                    row["rep"] = rep
                    row["durability"] = durability
                    row["arm"] = name
                    row["range_size_ids"] = size
                    results.append(row)
                    port += 1
                    # Ports are never reused inside a run: a server that has
                    # just been stopped can hold its port in TIME_WAIT, and
                    # the next arm would then measure a failed bind.

    def cell(k, durability, arm):
        return [r for r in results
                if r["cores"] == k and r["durability"] == durability and r["arm"] == arm]

    print(f"\n{'cell':<34} {'rep':>3} {'ips':>10} {'wall s':>8} {'placed':>8} "
          f"{'next_id':>9} {'burnt':>7} {'retries':>8}")
    print("-" * 92)
    for r in results:
        retries = sum(p.get("retries", 0) for p in r["per_core"].values())
        print(f"{r['cell']:<34} {r['rep']:>3} {r['inserts_per_s']:>10.0f} "
              f"{r['wall_s']:>8.2f} {r['rows_placed']:>8} {str(r['next_id']):>9} "
              f"{str(r['ids_burnt']):>7} {retries:>8}")
        if r["errors"]:
            print(f"  ERRORS: {r['errors']}")
        if r["rows_placed"] != r["cores"] * r["rows_per_core"]:
            print(f"  SHORT: placed {r['rows_placed']} of "
                  f"{r['cores'] * r['rows_per_core']} asked")

    print(f"\n{'k':>3} {'durability':<10} {'concentrated':>13} {'spread':>10} "
          f"{'S/C':>7} {'reps':>5}")
    print("-" * 54)
    summary = []
    for k in ks:
        for durability in durabilities:
            c = [r["inserts_per_s"] for r in cell(k, durability, "C-concentrated")]
            s = [r["inserts_per_s"] for r in cell(k, durability, "S-spread")]
            if not c or not s:
                continue
            cm, sm = statistics.median(c), statistics.median(s)
            ratio = sm / cm if cm > 0 else 0.0
            summary.append({"k": k, "durability": durability, "concentrated_ips": cm,
                            "spread_ips": sm, "ratio": ratio, "reps": len(c)})
            print(f"{k:>3} {durability:<10} {cm:>13.0f} {sm:>10.0f} {ratio:>7.3f} "
                  f"{len(c):>5}")
    print()
    print(json.dumps({"summary": summary, "cells": results}, indent=2))


if __name__ == "__main__":
    main()
