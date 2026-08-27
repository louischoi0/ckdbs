#!/usr/bin/env python3
"""What a reader pays for sitting on a core that is committing.

`bench/v2.0.0/results-multicore-writers-v2.0.0-67-g952bbb9.md` §11 measured
this and it is the sharpest number in that file: a point-SELECT by pk costs
**37 µs alone and 1,088 µs beside a session committing INSERTs back to
back**, on the same core, with no lock and no shared row between them. The
reader is not waiting for the writer; it is waiting for the *reactor*, which
is inside the WAL drain's `fdatasync` and cannot poll a socket while it is
there. Nothing in the engine's own accounting shows it - the sync is charged
to no scheduling group (`docs/spec/sched.md` §4).

That measurement was taken by hand and left no script, which is why the
number could not be re-checked when the idle policy changed underneath it.
This is the script.

**What it is for.** Two questions, and it answers only the first:

  1. *Did a change to the reactor move this?* Run it on two binaries and
     compare. That is RW-B cell 5's use: "parked is not ready" (RW3) put a
     block where a spin used to be, and the reader beside a committer is the
     shape most likely to notice.
  2. *Why is it 1,088 µs?* It is not - that is the fdatasync, and this probe
     does not decompose it. `run_pw6.py --probes` measures the sync floor on
     the same device, which is what the share is computed against.

Three windows, on one server, in this order, so the controls are taken on
the same mount as the measurement:

  reader alone      the floor: a point-SELECT with nothing else running
  reader + committer  the cell: the same SELECT while a second session on
                    the **same core** commits INSERTs as fast as it can
  committer alone   the writer's own latency, so a change that slowed the
                    writer is not read as a change that slowed the reader

Both sessions are placed on the relation's owner core, so nothing here is
cross-core and no statement is shipped: this is a single-core property that
happens to be measured on a multi-core build.

Usage:
    bench/reader_beside_committer_probe.py --server <copied kds_server> \\
        --workdir ~/rw-b/cell5/c/post-r1 --rows 1200 --json out.json
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
from multicore_benchmark import (  # noqa: E402
    Conn, collect_connections, field, is_retryable, wait_for_port,
)

PCTS = (0, 25, 50, 75, 90, 95, 99, 100)


def pct(values, p):
    """Nearest-rank, so every reported number is one a statement actually
    took rather than an interpolation between two that did not."""
    if not values:
        return 0.0
    ordered = sorted(values)
    if p <= 0:
        return ordered[0]
    if p >= 100:
        return ordered[-1]
    rank = max(1, min(len(ordered), int(-(-p * len(ordered) // 100))))
    return ordered[rank - 1]


def summarize(values):
    out = {f"p{p}": round(pct(values, p), 1) for p in PCTS}
    out["n"] = len(values)
    out["mean"] = round(sum(values) / len(values), 1) if values else 0.0
    return out


def rows_of(reply):
    """A reply's lines. **The newline separator arrives escaped** - the wire
    is one line per reply and a row break is the two characters `\\` and
    `n`, which `tools/multicore_benchmark.py:439` un-escapes the same way.
    Splitting on a real newline instead finds exactly one line, which makes
    every result look empty and every point-SELECT look like a miss: the
    first version of this probe did that and still produced percentiles."""
    return reply.replace("\\n", "\n").strip().splitlines()


def read_keys(conn, table):
    """Every pk currently in `table`, read from the engine rather than
    assumed. An omitted pk is issued from the owning core's row-id block, so
    the ids are whatever that block's base happens to be and are **not**
    1..n - assuming otherwise produced a probe whose every read missed,
    which still returns promptly and therefore still looks like a
    measurement."""
    reply = conn.cmd(f"SELECT id FROM {table}")
    if reply.startswith("ERR"):
        raise RuntimeError(f"reading keys: {reply}")
    keys = []
    for line in rows_of(reply)[1:]:
        line = line.strip()
        if line.isdigit():
            keys.append(int(line))
    if not keys:
        # The raw reply, not just its absence: a probe that cannot say what
        # it was handed sends whoever debugs it back to a server.
        raise RuntimeError(f"no keys parsed from: {reply[:400]!r}")
    return keys


def timed(conn, sql):
    started = time.perf_counter()
    reply = conn.cmd(sql)
    return (time.perf_counter() - started) * 1e6, reply


def reader_loop(conn, table, stop, out, keys):
    """Point-SELECTs by pk until `stop` is set. Latencies only - the reply's
    row count is checked but not reported, because a SELECT that found
    nothing would still be a fast SELECT and must not read as one."""
    lat, errors, empties = [], 0, 0
    i = 0
    while not stop.is_set():
        key = keys[i % len(keys)]
        i += 1
        us, reply = timed(conn, f"SELECT id FROM {table} WHERE id = {key}")
        if reply.startswith("ERR"):
            errors += 1
            continue
        # A miss is a different amount of work from a hit, so it is counted
        # and checked by the caller rather than left to be noticed in a
        # percentile. Header line only = nothing found (see `rows_of` for
        # why the obvious test for that is wrong).
        if len(rows_of(reply)) <= 1:
            empties += 1
        lat.append(us)
    out["reader"] = dict(summarize(lat), errors=errors, empty_replies=empties)


def committer_loop(conn, table, rows, out, stop=None, retry_deadline_s=10.0):
    """Autocommit INSERTs back to back: every one is a group commit that
    parks on `durable_lsn` and is satisfied by the reactor's post-task hook.

    **A retryable refusal is retried, not skipped.** A peer's row-id lease is
    spent at mount until core 0's refill grant lands, so the first inserts
    after a fresh start are refused `TXN_CONFLICT retryable=1` - and a loop
    that counted those as done would seed a table with nothing in it and then
    measure reads that all miss. Refusals are counted separately, so a run
    that spent its window waiting on a lease is visible rather than averaged
    into the latency."""
    lat, errors, retries, inserted = [], 0, 0, 0
    for i in range(rows):
        if stop is not None and stop.is_set():
            break
        deadline = time.perf_counter() + retry_deadline_s
        while True:
            # No column list: the grammar is `INSERT INTO t VALUES (...)`
            # and the pk is omitted so the engine issues it
            # (heap-and-tuple.md §4.1). A column list parses as far as the
            # paren and refuses.
            us, reply = timed(conn, f"INSERT INTO {table} VALUES ('u{i}', {i})")
            if not reply.startswith("ERR"):
                lat.append(us)
                inserted += 1
                break
            if is_retryable(reply) and time.perf_counter() < deadline:
                retries += 1
                time.sleep(0.002)
                continue
            errors += 1
            out.setdefault("first_error", reply)
            break
    out["committer"] = dict(summarize(lat), errors=errors, retries=retries,
                            inserted=inserted)


def run(args):
    workdir = args.workdir
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "probe.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 'probe.db')}\n"
                f"port = {args.port}\ncores = {args.cores}\n"
                f"placement = rotate\npeer_listeners = on\n"
                f"wal_drain_interval_us = {args.wal_drain_interval_us}\n"
                f"log_file = probe.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "probe.stderr")
    result = dict(cores=args.cores, rows=args.rows, reader_seconds=args.reader_seconds,
                  wal_drain_interval_us=args.wal_drain_interval_us,
                  server=os.path.abspath(args.server))
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    try:
        wait_for_port(args.port, stderr_path)
        time.sleep(2)  # let the mount's own work leave the window

        setup, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        setup = setup[0][0]
        reply = setup.cmd("CREATE TABLE rb (id int64, owner varchar, balance int64) BTREE")
        if reply.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE: {reply}")
        owner = int(field(setup.cmd("DESCRIBE rb"), "owner_core"))
        result["owner_core"] = owner

        # **Both sessions on the owner**: the cell is about one reactor, and
        # a session anywhere else would be measuring the wire instead.
        seated, _ = collect_connections(args.port, {owner: 2}, args.max_connects)
        reader, committer = seated[owner][0], seated[owner][1]
        setup.close()

        # Seed rows for the reader to find, from the committing session, so
        # the reader's keys exist before its first window.
        seed = {}
        committer_loop(committer, "rb", args.seed_rows, seed)
        result["seed"] = seed["committer"]
        if "first_error" in seed:
            result["seed_first_error"] = seed["first_error"]
        if seed["committer"]["inserted"] == 0:
            raise RuntimeError(f"seed inserted nothing: {seed.get('first_error')}")

        keys = read_keys(committer, "rb")
        result["keys_seen"] = len(keys)
        result["key_range"] = [min(keys), max(keys)] if keys else []
        if not keys:
            raise RuntimeError("no keys read back from the seeded table")

        # ---- Window 1: the reader alone (the floor) --------------------
        stop = threading.Event()
        alone = {}
        t = threading.Thread(target=reader_loop,
                             args=(reader, "rb", stop, alone, keys))
        t.start()
        time.sleep(args.reader_seconds)
        stop.set()
        t.join()
        result["reader_alone"] = alone["reader"]

        # ---- Window 2: the reader beside a committing session ----------
        stop = threading.Event()
        beside, writing = {}, {}
        rt = threading.Thread(target=reader_loop,
                              args=(reader, "rb", stop, beside, keys))
        wt = threading.Thread(target=committer_loop,
                              args=(committer, "rb", args.rows, writing))
        rt.start()
        wt.start()
        wt.join()          # the writer's row count bounds the window
        stop.set()
        rt.join()
        result["reader_beside_committer"] = beside["reader"]
        result["committer_beside_reader"] = writing["committer"]

        # ---- Window 3: the committer alone -----------------------------
        alone_w = {}
        committer_loop(committer, "rb", args.rows // 2, alone_w)
        result["committer_alone"] = alone_w["committer"]

        result["count"] = rows_of(committer.cmd("SELECT COUNT(*) FROM rb"))[-1:]
        try:
            committer.cmd("STOP")
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
    ap.add_argument("--workdir", required=True, help="on a block device, never tmpfs")
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--rows", type=int, default=1200, help="INSERTs in the busy window")
    ap.add_argument("--seed-rows", type=int, default=200)
    ap.add_argument("--reader-seconds", type=float, default=3.0)
    ap.add_argument("--wal-drain-interval-us", type=int, default=1000)
    ap.add_argument("--port", type=int, default=16800)
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    result = run(args)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)

    r_alone = result["reader_alone"]
    r_beside = result["reader_beside_committer"]
    w_beside = result["committer_beside_reader"]
    w_alone = result["committer_alone"]
    print(f"owner core {result['owner_core']}, drain {args.wal_drain_interval_us} us")
    print(f"  reader alone            p50 {r_alone['p50']:>9.1f} us  p99 {r_alone['p99']:>9.1f}  n={r_alone['n']}")
    print(f"  reader beside committer p50 {r_beside['p50']:>9.1f} us  p99 {r_beside['p99']:>9.1f}  n={r_beside['n']}")
    print(f"  committer beside reader p50 {w_beside['p50']:>9.1f} us  p99 {w_beside['p99']:>9.1f}  n={w_beside['n']}")
    print(f"  committer alone         p50 {w_alone['p50']:>9.1f} us  p99 {w_alone['p99']:>9.1f}  n={w_alone['n']}")
    if r_alone["p50"]:
        print(f"  the cell: reader pays {r_beside['p50'] / r_alone['p50']:.1f}x beside a committer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
