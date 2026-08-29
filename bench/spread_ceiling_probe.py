#!/usr/bin/env python3
"""R4-M / IM3 — the read ceiling of a spread relation, measured.

`workplan-insert-spreading.md` §3 priced this before any code was written:

    A range is a lease grant, so a relation taking n blocks has n ranges.
    The fan-in opens one stage per maximal contiguous run of ranges on one
    core, and spreading is exactly the case where consecutive ranges have
    different owners, so `stages == ranges`. `kMaxFanInUpstreams` is 64,
    and above it a read is refused rather than degraded. So a spread
    relation is readable up to 64 x `range_size_ids` rows.

Arithmetic from two ratified choices. This probe turns it into a number.

## The instrument, and why no engine change is needed

Nothing exposes a relation's range count: `sys.ranges` has no column
definitions, so `SELECT * FROM ranges` answers `ERR no columns for this
rel_id`, and `SHOW META` carries only the *decline* counters. IS7 recorded
that as a gap and used `rowid_refill_grants` as a proxy.

The refusal itself is the better instrument, and it was already there:

    ERR ... relation 'spread' needs N stages, above the fan-in ceiling of 64

(`command_dispatcher.cpp`, the `stages.size() > kMaxFanInUpstreams` arm.)
So the first refusal reports **the stage count in the message and the row
count in the poll that preceded it** - which is §3's ceiling and, in the
same pair of numbers, how often IS5's suppression fired: with no
suppression the refusal arrives at about 64 x `range_size_ids` ids, and
every suppressed carve pushes it further out.

## What IS5 does to this, read from the source before running

`range_alloc.cpp`'s suppression is **top-owner identity**, not contiguity:
if the core asking for a block already owns the relation's top range, no
boundary opens. With exactly one peer that is every carve after the first,
so a relation contended by one peer settles at two ranges and **has no
ceiling at all**. Two or more peers take turns, the top owner changes
almost every carve, and the ranges accumulate. `--cores` below 3 therefore
measures the absence of the effect, which is a result and is reported as
one rather than treated as a failed run.

## Why this probe runs `placement = rotate` and reads only `SELECT *`

Measured on this tree before the sweep, because the first draft of this
probe ran `creating` and polled `COUNT(*)`, and **every poll was refused
from the first cross-owner range onward** - never reaching the ceiling
after two million rows. Two constraints sit in front of §3's arithmetic
and neither is in §3:

- **The fan-in client is core 0's alone.** `expeditor.cpp` constructs it
  as `remote_reads_.emplace(/*core_id=*/0, ...)` and calls
  `SetRemoteReads` on that one dispatcher; a `CoreRuntime` peer's
  dispatcher is never given one. So a session on any peer takes
  `CheckReadAffinity`'s refusal for a relation not wholly its own, at any
  range count.
- **The route also requires the reader not to be the relation's
  `owner_core`.** Under `placement = creating` every relation is core 0's,
  so core 0 fails that test and the peers have no client - and a relation
  spread under `creating` is **unreadable from every core**. Under
  `rotate` the owner is a peer, so a core-0 session takes the route.

And the route admits one shape: `chain.star()` with no aggregate, no sort,
no `LIMIT`/`OFFSET`, no sub-chain. Measured from a core-0 session under
`rotate`, `SELECT *`, `SELECT * WHERE id = 1`, `SELECT * WHERE v = 1` and
`SELECT * ORDER BY id ASC` answer; `SELECT id`, `SELECT COUNT(*)` and
`SELECT * LIMIT 2` are refused. So the ceiling read here is `SELECT *`,
and it is the only whole-relation shape that can reach the ceiling at all.

Usage:
    bench/spread_ceiling_probe.py --server build-release/kds_server \\
        --workdir ~/ceiling --cores 4 --range-size-ids 1024,4096,16384
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
    Conn, check_host, collect_connections, field, session_core, stop_server,
    wait_for_port,
)

TABLE = "spread"
# The read whose refusal is the measurement. It must be a shape the fan-in
# route admits, which `COUNT(*)`, `SELECT id` and `LIMIT` are not - the
# route's guard is `steps.size() == 1 && hoisted.empty() && star() &&
# !aggregated() && !sorted() && !limit && offset == 0`
# (`command_dispatcher.cpp`), so an aggregated or projected chain never
# reaches the ceiling test at all and its refusal says something else.
#
# **`WHERE v = -1`, which no row satisfies.** Verified against the source
# rather than assumed: the stage set is built from *every* row of
# `TableAccess::ranges` with no predicate narrowing whatever, and
# `stages.size() > kMaxFanInUpstreams` is tested before a single
# `remote_reads_->Open`. So a residual predicate changes nothing about
# which refusal arrives - `WHERE v = -1` is `SELECT *`'s twin here, and it
# is only the *reply* that shrinks to a header instead of a megabyte. (The
# same reading says a pk equality would meet this ceiling too: nothing on
# this path resolves `WHERE id = ?` to one range. It is not used, because
# a shape whose answer is one row is a poor witness for a whole-relation
# read, not because it would be exempt.)
#
# At `range_size_ids = 4096` the ceiling arrives near 262,144 rows and the
# poll runs a few hundred times; a bare `SELECT *` would have shipped tens
# of millions of rows over the wire to learn one boolean. `v` is the
# INSERT's only value and counts up from 0 in every writer, so the
# predicate is vacuous by construction and not by luck.
CEILING_READ = f"SELECT * FROM {TABLE} WHERE v = -1"
# K-g's read, timed at geometric checkpoints rather than every poll: the
# whole relation, which is what a client actually pays once a relation is
# spread over k owners.
COST_READ = f"SELECT * FROM {TABLE}"


def start_server(binary, workdir, tag, cores, port, range_size_ids, durability,
                 placement="rotate", data=None):
    """`data` names the file to mount; omitted, the tag names a fresh one.
    The remount below passes the first mount's file back in - a second mount
    of a *new* file would measure nothing, which is what the first draft of
    this probe did."""
    conf = os.path.join(workdir, f"{tag}.conf")
    fresh = data is None
    data = data or os.path.join(workdir, f"{tag}.db")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\npeer_listeners = on\n"
                f"range_size_ids = {range_size_ids}\n"
                f"durability = {durability}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    if fresh:
        # **Only when this call owns the file.** A tag repeats between runs
        # over one `--workdir`, and a file left by a crashed run already
        # carries `sys.ranges` rows - which would start the cell some
        # unknown number of stages in and make `rows_placed_at_refusal`
        # fiction. Guarded on `fresh`, because the remount below passes the
        # first mount's file back in and deleting *that* would erase the
        # measurement it exists to take.
        subprocess.run(["rm", "-rf", data, data + ".wal"], check=False)
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc, data, conf


def stage_count(reply):
    """The stage count out of the fan-in ceiling refusal, or None if this
    reply is not one. The message is
    `relation '<x>' needs N stages, above the fan-in ceiling of 64`, and the
    count in it is the **only** way this engine reports a relation's range
    count from outside the process: `sys.ranges` has no column definitions,
    so `SELECT * FROM ranges` answers `ERR no columns for this rel_id`."""
    if not reply.startswith("ERR") or "stages, above the fan-in ceiling" not in reply:
        return None
    words = reply.split()
    for i, word in enumerate(words):
        if word == "stages," and i > 0 and words[i - 1].isdigit():
            return int(words[i - 1])
    return None


def is_retryable(reply):
    if not reply.startswith("ERR"):
        return False
    return ("retryable=1" in reply
            or "retry after the refill grant lands" in reply
            or "writes are bound to core" in reply)


def writer(port, core, counter, stop, queue, deadline_s=30.0):
    """Inserts until `stop` is set, counting placed rows into `counter` and
    reporting how it ended on `queue`.

    `counter` is a lock-free `Value`: the parent only ever reads it to
    bracket the refusal, so a torn increment costs a row of precision and
    a per-row lock would cost the writer far more than that. The final
    record goes on the queue instead, where it is exact.
    """
    conn = None
    n = 0
    outcome = {"rows": 0}
    try:
        conns, _ = collect_connections(port, {core: 1}, max_attempts=400)
        conn = conns[core][0]
        retries = 0
        while not stop.is_set():
            started = time.monotonic()
            while True:
                reply = conn.cmd(f"INSERT INTO {TABLE} VALUES ({n})")
                if not reply.startswith("ERR"):
                    break
                retries += 1
                if not is_retryable(reply):
                    outcome = {"rows": n, "retries": retries, "error": reply,
                               "kind": "refused"}
                    return
                if time.monotonic() - started > deadline_s:
                    # A retryable refusal that never clears is a **finding**,
                    # not a slow row: the grant it names was requested and
                    # did not arrive inside the deadline.
                    outcome = {"rows": n, "retries": retries, "error": reply,
                               "kind": "retryable-never-cleared",
                               "waited_s": round(time.monotonic() - started, 2)}
                    return
                time.sleep(0.002)
            n += 1
            counter.value = n
        outcome = {"rows": n, "retries": retries}
    except BaseException as exc:
        outcome = {"rows": n, "error": f"driver: {type(exc).__name__}: {exc}",
                   "kind": "driver"}
    finally:
        if conn is not None:
            conn.close()
        queue.put((core, outcome))


def run_cell(binary, workdir, cores, port, range_size_ids, durability, poll_s,
             max_rows, max_seconds, placement):
    tag = f"ceiling-k{cores}-r{range_size_ids}"
    proc, data, conf = start_server(binary, workdir, tag, cores, port,
                                    range_size_ids, durability, placement)
    try:
        setup = Conn(port)
        if session_core(setup) != 0:
            probe, _ = collect_connections(port, {0: 1}, max_attempts=200 * cores)
            setup.close()
            setup = probe[0][0]
        reply = setup.cmd(f"CREATE TABLE {TABLE} (id int64, v int64)")
        if reply.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE: {reply}")

        ctx = multiprocessing.get_context("fork")
        stop = ctx.Event()
        queue = ctx.Queue()
        counters = [ctx.Value("l", 0, lock=False) for _ in range(cores)]
        writers = [ctx.Process(target=writer, args=(port, c, counters[c], stop, queue))
                   for c in range(cores)]
        for w in writers:
            w.start()

        began = time.monotonic()
        polls = []
        costs = []
        refusal = None
        last_ok_rows = 0
        # **Polled on rows, not on wall clock, and coarsely until the
        # ceiling is near.** The transition to measure is one range wide, so
        # a time-based poll brackets it as loosely as the writers happen to
        # be fast: the first draft polled every 0.25 s and caught the refusal
        # at 85 stages, twenty past the 65 that is the answer. A quarter of a
        # block is the resolution that makes the bracket narrower than the
        # thing being measured - but paying it from row zero would run a
        # thousand polls to reach a ceiling the arithmetic already brackets,
        # so the dense phase starts at 85% of `64 x range_size_ids`.
        dense_from = int(0.85 * 64 * range_size_ids)
        next_poll_at = 0
        next_cost_at = 2048
        while True:
            placed = sum(max(c.value, 0) for c in counters)
            if placed >= next_cost_at:
                # K-g (RD9(c)): what a client pays for the whole relation at
                # this many owners and this block size. Geometric, because
                # the shape is what the cell is for and each of these ships
                # the entire relation.
                next_cost_at = placed * 2
                cost_began = time.monotonic()
                whole = setup.cmd(COST_READ)
                costs.append({"rows_placed": placed,
                              "est_stages": max(1, placed // range_size_ids),
                              "read_ms": round((time.monotonic() - cost_began) * 1e3, 2),
                              "reply_bytes": len(whole),
                              "refused": whole.startswith("ERR")})
            if placed < next_poll_at:
                if time.monotonic() - began > max_seconds:
                    break
                time.sleep(poll_s)
                continue
            next_poll_at = placed + (max(1, range_size_ids // 4) if placed >= dense_from
                                     else max(1, range_size_ids * 4))
            answer = setup.cmd(CEILING_READ)
            stages = stage_count(answer)
            polls.append({"t_s": round(time.monotonic() - began, 2),
                          "rows_placed": placed, "stages": stages,
                          "reply": answer[:160] if answer.startswith("ERR") else "ok"})
            if stages is not None:
                refusal = {"rows_placed_at_refusal": placed,
                           "stages_at_refusal": stages,
                           "rows_last_readable": last_ok_rows,
                           "reply": answer}
                break
            if not answer.startswith("ERR"):
                last_ok_rows = placed
            if placed >= max_rows or time.monotonic() - began > max_seconds:
                break
        stop.set()
        outcomes = {}
        for _ in writers:
            try:
                core, result = queue.get(timeout=120)
                outcomes[core] = result
            except Exception:  # a writer that never reported is itself a finding
                break
        for w in writers:
            w.join(timeout=60)
            if w.is_alive():
                w.terminate()

        # **A writer that never reported must not silently vanish from the
        # total.** The `break` above leaves `outcomes` short, and summing
        # only what reported gave a `rows_placed` missing that core's whole
        # contribution - which then went on to *overstate* `ids_burnt` by
        # exactly the rows it dropped, with `writer_errors` empty and
        # nothing printed. The exact count is the queue's; the approximate
        # one is the shared counter, which is what an absent writer leaves
        # behind. Take each where it exists and name the ones that did not
        # report, so a cell built on the second kind says so.
        unreported = [c for c in range(cores) if c not in outcomes]
        placed = (sum(r.get("rows", 0) for r in outcomes.values())
                  + sum(max(counters[c].value, 0) for c in unreported))
        # The burn side (CK3's other half): what the 40-bit space has been
        # charged for a relation that placed `placed` rows. `next_id` is
        # the high-water mark, so the difference is the unissued remainder
        # of every live block plus every id a refused statement burnt.
        describe = setup.cmd(f"DESCRIBE {TABLE}")
        next_id = field(describe, "next_id")
        meta = {}
        for c in range(cores):
            peek, _ = collect_connections(port, {c: 1}, max_attempts=200 * cores)
            meta[c] = peek[c][0].cmd("SHOW META")
            peek[c][0].close()
        setup.close()

        cell = {
            "cores": cores,
            "range_size_ids": range_size_ids,
            "durability": durability,
            "placement": placement,
            "rows_placed": placed,
            "per_core": outcomes,
            "writer_errors": {c: r for c, r in outcomes.items() if "error" in r},
            # Not folded into `writer_errors`: a writer that never reported
            # has no error to report, and its rows above came off the
            # lock-free counter rather than off its own tally.
            "writers_unreported": unreported,
            "next_id": next_id,
            "ids_burnt": (next_id - 1 - placed) if isinstance(next_id, int) else None,
            "refusal": refusal,
            "polls": polls,
            "read_costs": costs,
            "rowid_refill_grants": {
                c: field(meta[c], "rowid_refill_grants")
                for c in range(cores) if "rowid_refill_grants=" in meta[c]
            },
        }
    finally:
        stop_server(port)
        proc.wait(timeout=30)

    # **The restart half of the burn**, measured rather than argued: a mount
    # forgets every lease, so every live block's remainder is burnt and the
    # next id issued is the mark, not the next free id in a block.
    proc, _, _ = start_server(binary, workdir, f"{tag}-remount", cores, port + 1,
                              range_size_ids, durability, placement, data=data)
    try:
        after = Conn(port + 1)
        cell["next_id_after_remount"] = field(after.cmd(f"DESCRIBE {TABLE}"), "next_id")
        after.close()
    finally:
        stop_server(port + 1)
        proc.wait(timeout=30)
    return cell


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", default="4",
                    help="writer cores; a comma list sweeps k")
    ap.add_argument("--range-size-ids", default="1024,4096,16384",
                    help="comma list; the sweep D6's value is taken on")
    ap.add_argument("--durability", default="relaxed",
                    help="relaxed by default: the subject is the ceiling, not "
                         "the commit path, and relaxed reaches it ~28x sooner")
    ap.add_argument("--placement", default="rotate", choices=("rotate", "creating"),
                    help="rotate by default, and the docstring says why: under "
                         "`creating` the relation is core 0's, so no core can "
                         "read it once it spreads and the ceiling is unreachable")
    ap.add_argument("--port", type=int, default=15700)
    ap.add_argument("--poll-s", type=float, default=0.25)
    ap.add_argument("--max-rows", type=int, default=2_000_000)
    ap.add_argument("--max-seconds", type=float, default=240.0)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    cells = []
    port = args.port
    for k in [int(x) for x in args.cores.split(",")]:
        for size in [int(x) for x in args.range_size_ids.split(",")]:
            print(f"=== k={k} range_size_ids={size} ===", file=sys.stderr)
            cells.append(run_cell(args.server, args.workdir, k, port, size,
                                  args.durability, args.poll_s, args.max_rows,
                                  args.max_seconds, args.placement))
            port += 2

    print(f"\n{'k':>3} {'range_size':>11} {'last readable':>14} {'refused at':>11} "
          f"{'stages':>7} {'64 x size':>11} {'ids/stage':>10} {'burnt':>8} "
          f"{'burnt/mount':>12}")
    print("-" * 92)
    for c in cells:
        ref = c["refusal"]
        stages = at = ok = ""
        per_stage = ""
        if ref:
            at = str(ref["rows_placed_at_refusal"])
            ok = str(ref["rows_last_readable"])
            stages = str(ref["stages_at_refusal"])
            if isinstance(c["next_id"], int) and ref["stages_at_refusal"]:
                # ids per stage against `range_size_ids`: equal means every
                # block opened a range and IS5's suppression never fired
                # (HK4); above means it did, and by how much.
                per_stage = f"{(c['next_id'] - 1) / ref['stages_at_refusal']:.0f}"
        else:
            at = "not reached"
            ok = str(c["rows_placed"])
        remount = c.get("next_id_after_remount")
        burnt_mount = (remount - c["next_id"]
                       if isinstance(remount, int) and isinstance(c["next_id"], int) else "")
        print(f"{c['cores']:>3} {c['range_size_ids']:>11} {ok:>14} {at:>11} "
              f"{stages:>7} {64 * c['range_size_ids']:>11} {per_stage:>10} "
              f"{str(c['ids_burnt']):>8} {str(burnt_mount):>12}")
        # Printed, not left in the JSON: a cell whose row total came partly
        # off the lock-free counter, or whose writer stopped on a refusal,
        # is one to read differently - and the table is what gets quoted.
        if c["writers_unreported"]:
            print(f"    UNRESOLVED: writers {c['writers_unreported']} never reported; "
                  f"their rows are the shared counter's reading, not their own")
        for core, err in c["writer_errors"].items():
            print(f"    WRITER {core} {err.get('kind', 'error')}: "
                  f"{str(err.get('error'))[:100]}")
    # K-g: read cost against the range count that produced it. Printed as a
    # short series per cell rather than every poll, because the shape is
    # what the cell is for and the JSON below carries the rest.
    print(f"\n{'k':>3} {'range_size':>11}   read cost by estimated stage count")
    print("-" * 78)
    for c in cells:
        ok = [p for p in c["read_costs"] if not p["refused"]]
        if not ok:
            continue
        series = "  ".join(f"{p['est_stages']}st:{p['read_ms']:.0f}ms/"
                           f"{p['reply_bytes'] // 1024}KiB" for p in ok)
        print(f"{c['cores']:>3} {c['range_size_ids']:>11}   {series}")
    print()
    print(json.dumps(cells, indent=2))


if __name__ == "__main__":
    main()
