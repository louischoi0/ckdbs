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
  2. **It moves with concurrency, on that core.** The sweep runs 1, 2, 4 and
     8 sessions committing at once against the same core and reads the batch
     off each. What it found, on a peer and on core 0 alike: **1.000 at one
     *and at two* sessions, 2.000 at four, ~4.000 at eight** - the batch is
     `n/2` from four upward and flatly 1 below it, and throughput follows
     (804 -> 818 -> 1597 -> 2890 commits/s). So D2 batches, and **two
     concurrent committers on a core get none of it**.

     Stopping at two sessions would have called that threshold an absence,
     which is exactly the mistake this probe's first run made.

Every sweep point differs in **one** thing: how many sessions are committing
on the core at once. Same core, same relations, same statement count, same
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
    ap.add_argument("--sweep", type=int, nargs="*", default=[1, 2, 4, 8],
                    help="concurrent committing sessions to sweep on the owner core")
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
        need = max([3] + list(args.sweep))
        got, attempts = collect_connections(args.port, {owner: need}, args.max_connects)
        print(f"opened {attempts} connections to get {need} sessions on core {owner}")
        out["connect_attempts"] = attempts
        conns = got[owner]
        try:
            # ---- Claim 1: the reading is the peer's, not core 0's --------
            reading = batch_fields(conns[0])
            print(f"SHOW META on that session reports core={reading['core']}")
            out["reported_core"] = reading["core"]
            out["reads_the_peer"] = reading["core"] == owner

            errors = []

            # ---- The sweep: how many concurrent committers it takes ------
            #
            # Two sessions is the boundary case and a bad place to stop. The
            # post-task hook batches whatever was staged **in one reactor
            # iteration**, and the sync runs on the reactor thread *after*
            # the tasks - so a request arriving during a sync is only polled
            # on the next iteration, after the sync it just did. With two
            # closed-loop clients that can anti-phase into one commit per
            # iteration forever; with more, arrivals should pile up during a
            # sync and the next iteration should stage all of them at once.
            # This sweep is what tells those two apart, and stopping at two
            # would have called a threshold an absence.
            sweep = []
            for n in args.sweep:
                if n > len(conns):
                    continue
                before = batch_fields(conns[0])
                t0 = time.time()
                threads = [threading.Thread(
                    target=insert_loop,
                    args=(conns[i], "a" if i % 2 == 0 else "b", args.rows,
                          args.retry_deadline, errors))
                    for i in range(n)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
                d = delta(before, batch_fields(conns[0]))
                d["sessions"] = n
                d["wall_s"] = time.time() - t0
                d["commits_per_s"] = (n * args.rows) / d["wall_s"]
                sweep.append(d)
                print(f"  {n:2d} session(s): batch={d['mean_group_batch_over_phase']:6.3f} "
                      f"commits={d['wal_group_commits']:6d} syncs={d['wal_syncs']:6d} "
                      f"{d['commits_per_s']:8.1f} commits/s")
            out["sweep"] = sweep

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
        # The verdict is the *sweep's*, not the two-session pair's: the
        # batch is flat at 1 through two sessions and only moves at four, so
        # a verdict read off one and two alone reports "it never moves".
        by_n = {d["sessions"]: d["mean_group_batch_over_phase"] for d in out.get("sweep", [])}
        moved = [n for n, b in sorted(by_n.items()) if b > 1.0]
        print("claim 2 - the batch against concurrency:      "
              + ", ".join(f"{n}->{by_n[n]:.3f}" for n in sorted(by_n))
              + (f"  -> it moves, first at {moved[0]} sessions"
                 if moved else "  -> it never moved in this sweep"))
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
