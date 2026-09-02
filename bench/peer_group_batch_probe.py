#!/usr/bin/env python3
"""Is a *peer* core's group-commit batch readable, and does it move?

`SHOW META`'s `wal_group_commits` / `wal_group_batches` /
`wal_mean_group_batch` were added 2026-09-02 so an operator could see the
number AF-T5 §3b's write cost turns on. The field is **core-local**: it
answers from the dispatcher the session is attached to, so on a
single-listener instance every reading is core 0's, and the cores actually
paying un-batched syncs - the owners a shipped write commits on - cannot be
read at all. `docs/spec/client-manual.md` says the way in is
`peer_listeners = on`. This checks that it is.

Two claims, and the second is the one worth checking:

  1. **Readable.** A session the kernel accepted on core `c` gets core `c`'s
     counters back from `SHOW META`, not core 0's.
  2. **It moves, on that core, for the reason claimed.** One session
     committing serially on core `c` gives `wal_mean_group_batch = 1.000` -
     the cliff, every commit paying its own device sync. Two sessions
     committing concurrently on the same core give more than 1. If the field
     cannot tell those two apart on a peer, it does not do the job it was
     added for.

The two arms differ in **one** thing: how many sessions are committing on
the core at once. Same core, same relation count, same statement count, same
durability class - so a difference in the batch is the concurrency and
nothing else.

**A client cannot choose its core** under SO_REUSEPORT (`kds.conf.sample`),
so the probe opens connections until the kernel has given it enough on the
core it needs, asks each `SHOW META` for its `core=`, and reports how many
it had to open. A core that never accepts is reported, not spun on.

Counters are cumulative from mount, so every reading here is a **delta**
across the phase.

Usage:
    bench/peer_group_batch_probe.py --server build-release/kds_server \\
        --workdir ~/mcbench/peerbatch --cores 4 --rows 400
"""

import argparse
import json
import os
import shutil
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, DEFAULT_RETRY_DEADLINE_S, check_host, collect_connections, field,
    is_retryable, session_core, start_server,
)

FIELDS = ("wal_group_commits", "wal_group_batches", "wal_syncs")


def batch_fields(conn):
    """The three counters plus the mean, off one `SHOW META`."""
    meta = conn.cmd("SHOW META")
    if meta.startswith("ERR"):
        raise RuntimeError(f"SHOW META: {meta}")
    out = {k: field(meta, k) for k in FIELDS}
    out["core"] = field(meta, "core")
    # The mean is printed as a float, which `field()` cannot parse.
    at = meta.find(" wal_mean_group_batch=")
    if at < 0:
        raise RuntimeError(f"SHOW META carries no wal_mean_group_batch: {meta}")
    start = at + len(" wal_mean_group_batch=")
    out["wal_mean_group_batch"] = float(meta[start:meta.find(" ", start)]
                                        if meta.find(" ", start) > 0 else meta[start:])
    return out


def delta(before, after):
    d = {k: after[k] - before[k] for k in FIELDS}
    d["mean_group_batch_over_phase"] = (
        d["wal_group_commits"] / d["wal_group_batches"]
        if d["wal_group_batches"] else 0.0)
    return d


def retrying(conn, stmt, deadline_s):
    """A statement, retried while the engine says retry - the row-id lease a
    peer answers on a relation's first INSERT is a wait, not a refusal."""
    end = time.time() + deadline_s
    backoff = 0.0005
    while True:
        reply = conn.cmd(stmt)
        if not is_retryable(reply) or time.time() >= end:
            return reply
        time.sleep(backoff)
        backoff = min(backoff * 2, 0.05)


def insert_loop(conn, table, rows, deadline_s, errors):
    for i in range(1, rows + 1):
        reply = retrying(conn, f"INSERT INTO {table} VALUES ({i})", deadline_s)
        if reply.startswith("ERR"):
            errors.append(f"{table}: {reply}")
            if len(errors) > 4:
                return


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", default=os.path.expanduser("~/mcbench/peerbatch"))
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--rows", type=int, default=400)
    ap.add_argument("--port", type=int, default=15900)
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--placement", default="namespace",
                    help="`namespace` co-locates the pair on a peer, which is the case under "
                         "test. `creating` puts them on core 0 - the control that says whether "
                         "a result is about *peers* or about the engine.")
    ap.add_argument("--durability", default="group")
    ap.add_argument("--allow-core-zero", action="store_true",
                    help="do not refuse when the relations land on core 0; needed for the "
                         "`creating` control.")
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--retry-deadline", type=float, default=DEFAULT_RETRY_DEADLINE_S)
    args = ap.parse_args()

    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    out = {"config": vars(args)}
    proc = start_server(args.server, args.workdir, "peerbatch", args.cores, args.port,
                        placement=args.placement, peer_listeners=True,
                        durability=args.durability)
    try:
        # DDL is core 0's, and a peer ships it there - but the setup session
        # is found the same way every other one is, since the kernel decides.
        setup = Conn(args.port)
        try:
            print(f"setup session on core {session_core(setup)}")
            # **One namespace, two relations.** Co-located by NS10, so both
            # land on one peer core - which is exactly the placement AF-T5
            # §3b found paying un-batched syncs, and the one this field
            # exists to make visible.
            prefix = "grp." if args.placement == "namespace" else ""
            stmts = ["CREATE TABLE " + prefix + "a (id int64, v int64)",
                     "CREATE TABLE " + prefix + "b (id int64, v int64)"]
            if args.placement == "namespace":
                stmts.insert(0, "CREATE NAMESPACE grp")
            for stmt in stmts:
                reply = setup.cmd(stmt)
                if not reply.startswith("CREATED"):
                    raise RuntimeError(f"{stmt}: {reply}")
            owners = {t: field(setup.cmd(f"DESCRIBE {t}"), "owner_core") for t in ("a", "b")}
        finally:
            setup.close()

        print(f"owners: {owners}")
        out["owners"] = owners
        if owners["a"] != owners["b"]:
            raise RuntimeError(f"the namespace did not co-locate its relations: {owners}")
        owner = owners["a"]
        if owner == 0 and not args.allow_core_zero:
            raise RuntimeError("the relations landed on core 0, so there is no peer to read")

        # Three sessions on the owner: two for the concurrent arm, one kept
        # for the serial arm and for the readings.
        got, attempts = collect_connections(args.port, {owner: 3}, args.max_connects)
        print(f"opened {attempts} connections to get 3 sessions on core {owner}")
        out["connect_attempts"] = attempts
        conns = got[owner]
        try:
            # ---- Claim 1: the reading is the peer's, not core 0's --------
            reading = batch_fields(conns[0])
            print(f"SHOW META on that session reports core={reading['core']}")
            out["reported_core"] = reading["core"]
            out["reads_the_peer"] = reading["core"] == owner

            errors = []

            # ---- Arm A: one session committing on this core --------------
            before = batch_fields(conns[0])
            t0 = time.time()
            insert_loop(conns[0], "a", args.rows, args.retry_deadline, errors)
            serial = delta(before, batch_fields(conns[0]))
            serial["wall_s"] = time.time() - t0
            serial["commits_per_s"] = args.rows / serial["wall_s"]
            print(f"serial   (1 session ): {serial}")

            # ---- Arm B: two sessions committing on this core at once -----
            before = batch_fields(conns[0])
            t0 = time.time()
            threads = [threading.Thread(target=insert_loop,
                                        args=(conns[1], "a", args.rows,
                                              args.retry_deadline, errors)),
                       threading.Thread(target=insert_loop,
                                        args=(conns[2], "b", args.rows,
                                              args.retry_deadline, errors))]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            concurrent = delta(before, batch_fields(conns[0]))
            concurrent["wall_s"] = time.time() - t0
            concurrent["commits_per_s"] = (2 * args.rows) / concurrent["wall_s"]
            print(f"concurrent (2 sessions): {concurrent}")

            out["serial"] = serial
            out["concurrent"] = concurrent
            out["errors"] = errors[:5]
            out["error_count"] = len(errors)
        finally:
            for c in conns:
                c.close()

        print()
        print(f"claim 1 - a peer session reads its own core:  "
              f"{'YES' if out['reads_the_peer'] else 'NO'} "
              f"(asked core {owner}, got core {out['reported_core']})")
        one = serial["mean_group_batch_over_phase"]
        two = concurrent["mean_group_batch_over_phase"]
        print(f"claim 2 - the batch moves with concurrency:   "
              f"1 session = {one:.3f}, 2 sessions = {two:.3f}"
              + ("  -> the field distinguishes them"
                 if two > one else "  -> IT DOES NOT"))
        if errors:
            print(f"errors: {len(errors)}")
            for e in errors[:5]:
                print(f"   {e}")
    finally:
        proc.terminate()
        proc.wait()

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
