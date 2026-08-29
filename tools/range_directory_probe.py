#!/usr/bin/env python3
"""RD9(b)/(c) — the range-size sweep and the k-range read, over a subject
that can actually split.

`instructions/v2.5.0/range-directory.md` §7 names the problem this driver
exists to solve: the three named bulk bench relations (`daily_bars`,
`cargos`, `loans`) are all declared `BTREE`, and D1's decline
(`exec::RangeEligible`, `src/exec/range_eligible.cpp:24-26`) means no
btree relation ever splits. §6a's other four gates rule out every relation
carrying an index, a Cabin, a spilling (`varchar`) column, an FK or an
assertion. And `RB2`'s finding (`workplan-range-directory.md` §13b)
narrows it once more: core 0 never leases row ids, so a core-0-owned
relation can never open a range — the subject must be **peer-owned**
(`placement = rotate`, `cores >= 2`).

So this driver's one relation is declared **HEAP** (the default storage
class — no `BTREE` keyword), every column `int64`/`int32`, no `CREATE
INDEX`, no `CABIN`, no `varchar` anywhere in the schema, no `FOREIGN KEY`,
no `CREATE ASSERTION` — the five gates all read `kNone`
(`exec::RangeGate::kNone`) for it, every run. It is created with
`placement = rotate`, which under `AssignOwnerCore`
(`include/kds/catalog/core_placement.hpp:96-105`) lands it on a peer core,
never core 0.

**What this relation is not**: it is not `daily_bars`, `cargos`, `loans`,
or anything resembling them — no secondary index, no assertion, no FK, no
`varchar` (so no inline-cell-width variation), a schema chosen to be the
narrowest shape that clears every gate rather than a workload's own shape.
Every number this driver prints is a number about *that* narrow subject,
and the results file that reports them must say so beside every table
row, not once in a preface — a heap-clustered, ungated relation is what a
bulk bench relation in this engine is not, today, because D1 has not
lifted and §6a's other four gates have no relief date either.

**The second, deeper subject problem this driver was written to confront**
(verified against the source, not merely asserted): under this build,
*every* range a relation opens through the ordinary INSERT path is opened
on the **same** core, forever. RD5's allocator opens a new range for
"the core that asked" — `OpenRangeOnSystemCore(..., owner_core=
header.src_core, ...)` at `src/server/row_id_lease_service.cpp:84-89`,
where `header.src_core` is the relation's own owner core asking for its
own next lease block (`MaybeRefillRowIds`,
`src/server/core_runtime.cpp:1121-1129`) — and R4's insert-spreading
policy, the only mechanism that would ever hand a *later* range to a
*different* core, is out of this work order's scope
(`instructions/v2.5.0/range-directory.md` §1, "Out"). The RD7 fan-in test
itself says as much in its own fixture comment
(`tests/core_runtime_test.cpp:1383-1387`): *"nothing can produce this
state yet [...] a second owner arrives only with R4's insert spreading."*
And `CommandDispatcher`'s own stage-building code merges consecutive
same-owner ranges into one stage before it ever ships anything
(`src/server/command_dispatcher.cpp:6296-6310`: "if (!stages.empty() &&
stages.back().owner == range.owner_core && stages.back().span.hi ==
range.lo) { stages.back().span.hi = range.hi; continue; }") — so a
same-owner k-range relation collapses to **one** remote stage regardless
of k. The consequence for RD9(c): **no driver reachable from this
engine's wire protocol can produce the k>1-stage fan-in HD4 is stated
against.** This driver measures what *is* reachable — a same-owner
k-range relation's local walk (RD6/CD3's per-range chain stepping) and
its one-stage remote read (the pre-existing shipping path, with that walk
now running on the far side) — and the results file must say plainly that
neither is the cross-core fan-in RD7 built, and that nothing in this
build session can produce that state without either R4 or reaching past
the wire protocol into the engine's own C++ test fixtures.

Usage (fresh server + fresh data file per (rows, range_size_ids) cell,
`--reps` repeats each; never overlap with a build on a 2-CPU host):

    tools/range_directory_probe.py --server ~/rd9bench/bin/kds_server-XXXX \
        --workdir ~/rd9bench/run/rangesweep --cores 2 \
        --rows 200,1000,10000 --range-sizes 0,2048,4096,8192 --reps 3 \
        --json ~/rd9bench/run/rangesweep.json
"""

import argparse
import collections
import json
import os
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase  # noqa: E402


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def field(reply, key):
    for tok in reply.split():
        if tok.startswith(key + "="):
            return int(tok[len(key) + 1:])
    raise RuntimeError(f"reply carries no {key}= field: {reply}")


def filesystem_of(path):
    best, fs = "", "?"
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mount = parts[1]
                if ((path == mount or path.startswith(mount.rstrip("/") + "/"))
                        and len(mount) > len(best)):
                    best, fs = mount, parts[2]
    except OSError:
        pass
    return fs


def check_host(workdir, force):
    fs = filesystem_of(os.path.abspath(workdir))
    if fs == "tmpfs" and not force:
        sys.exit(f"{workdir} is on tmpfs; point --workdir at a real device, or pass --force")
    load1 = os.getloadavg()[0]
    cores = os.cpu_count() or 1
    if load1 > 0.5 * cores and not force:
        sys.exit(f"1-minute load is {load1:.2f} on {cores} core(s); wait for the box to go "
                 f"quiet, or pass --force")
    return fs, load1


def wait_for_port(port, stderr_path, deadline_s=15):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    tail = ""
    try:
        with open(stderr_path) as f:
            tail = "".join(f.readlines()[-8:]).strip()
    except OSError:
        pass
    raise TimeoutError(f"server did not listen on {port}" + (f":\n{tail}" if tail else ""))


def start_server(binary, workdir, tag, cores, port, range_size_ids):
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = rotate\npeer_listeners = on\n"
                f"range_size_ids = {range_size_ids}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = info\n")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc


def collect_connections(port, needed, max_attempts):
    got = {core: [] for core in needed}
    attempts = 0
    try:
        while any(len(got[c]) < n for c, n in needed.items()):
            if attempts >= max_attempts:
                short = {c: n - len(got[c]) for c, n in needed.items() if len(got[c]) < n}
                raise RuntimeError(f"after {attempts} connections the kernel never gave "
                                   f"these cores enough sessions: {short}")
            conn = Conn(port)
            attempts += 1
            core = field(conn.cmd("SHOW META"), "core")
            if core in got and len(got[core]) < needed[core]:
                got[core].append(conn)
            else:
                conn.close()
    except BaseException:
        for conns in got.values():
            for c in conns:
                c.close()
        raise
    return got, attempts


# The row-id lease's own documented refusal (PW1b): the *first* INSERT on
# a peer relation fails retryably while the lease refill is in flight -
# not an error, the contract `single_relation_probe.py`'s `is_retryable`
# and `multicore_benchmark.py`'s `timed()` already retry around. Ranging
# arms it again every `range_size_ids` (D6: the range *is* the grant), so
# a swept cell hits this once per lease block, not only once per relation.
RETRY_TEXTS = ("retry after the refill grant lands",
              "a refill must be granted before it can allocate again")

# A second, narrower class found *by this driver*, not documented as
# retryable anywhere this driver's rules were read from: an early INSERT
# against a freshly-`rotate`-placed relation can race the owner's
# relation-fault extent grant and land
# "DevicePageStore: core N may not write page P; it is not from this
# core's extent lease, carries no write grant" - **without** the wire's
# `retryable=1` bit. Reproduced directly against this session's copy of
# `kds_server-e5ab4f9` (~1 in 20 fresh `CREATE TABLE` + immediate-INSERT
# cells at `range_size_ids` armed, 200 rows): a bare retry of the same
# statement succeeds once the grant lands a moment later, so it reads as
# the same PW1c-4 handoff race the row-id lease already has a documented
# name for, just not (yet) carrying the bit that would make a client
# retry it automatically. Retried here on that evidence; **named as a
# finding in the results file, not silently normalized**, since the wire
# not marking it retryable is itself worth a reader's attention.
RACE_TEXTS = ("may not write page", "carries no write grant")


def is_retryable(reply):
    return reply.startswith("ERR") and ("retryable=1" in reply or
                                        any(t in reply for t in RETRY_TEXTS))


def is_known_race(reply):
    return reply.startswith("ERR") and any(t in reply for t in RACE_TEXTS)


def timed_retry(conn, stmt, deadline_s=20.0, backoff_s=0.0005):
    """One statement, retried **only** while it matches a class already
    understood not to have committed - the documented `retryable=1` lease
    refusal, or the reproduced write-grant race (`RACE_TEXTS`, confirmed
    by its own wording: "may not write page", refused *before* any write).
    Deliberately **not** widened to "any `ERR`": this loop's statement
    omits its pk, so a retry after a reply that reads as an error but
    followed an INSERT that actually committed manufactures a second,
    genuinely duplicate row with a new id - reproduced directly this
    session (a `--rows 200` cell read back 400 once blind-retry was tried,
    §-noted in the results file rather than repeated here). An unrecognized
    `ERR` is therefore counted and left as a lost row, not retried past;
    `unknown_hits` says how often that happened so it stays visible rather
    than silently averaged away. Bounded by `deadline_s` wall clock.
    Returns (latency_s, reply, retries, race_hits, unknown_hits)."""
    t0 = time.perf_counter()
    r = conn.cmd(stmt)
    n = 0
    races = 0
    unknown = 0
    while (is_retryable(r) or is_known_race(r)) and time.perf_counter() - t0 < deadline_s:
        if is_known_race(r):
            races += 1
        n += 1
        time.sleep(backoff_s)
        r = conn.cmd(stmt)
    if r.startswith("ERR") and not is_retryable(r) and not is_known_race(r):
        unknown += 1
    return time.perf_counter() - t0, r, n, races, unknown


def stop_server(port, proc):
    try:
        conn = Conn(port)
        try:
            conn.cmd("STOP")
        finally:
            conn.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=15)


# The subject relation: heap (default storage, no BTREE), all int columns,
# unindexed, uncabined, FK-free, unasserted. Every RangeEligible gate reads
# kNone for it (source-read, src/exec/range_eligible.cpp).
CREATE_SQL = "CREATE TABLE rd9b (id int64, a int64, b int64, c int32, d int32) HEAP"


def run_cell(binary, workdir, tag, cores, port, rows, range_size_ids,
            scan_ops, point_ops, force):
    fs, load1 = check_host(workdir, force)
    proc = start_server(binary, workdir, tag, cores, port, range_size_ids)
    result = {"tag": tag, "rows": rows, "range_size_ids": range_size_ids,
             "fs": fs, "load1_start": load1}
    try:
        got, ddl_attempts = collect_connections(port, {0: 1}, 512)
        ddl = got[0][0]
        r = ddl.cmd(CREATE_SQL)
        if r.startswith("ERR"):
            raise RuntimeError(f"CREATE TABLE refused: {r}")
        owner_core = field(ddl.cmd("DESCRIBE rd9b"), "owner_core")
        result["owner_core"] = owner_core
        result["ddl_attempts"] = ddl_attempts
        # A foreign session for the remote-read arm: any peer core that is
        # not the owner. With cores=2 and rotate, the owner is always core
        # 1 and core 0 (the DDL session's own core) is the only foreign
        # core there is - reuse it rather than hunting for a third.
        foreign_core = 0 if owner_core != 0 else None

        needed = {owner_core: 1}
        if foreign_core is not None:
            needed[foreign_core] = needed.get(foreign_core, 0) + 1
        got2, writer_attempts = collect_connections(port, needed, 512)
        writer = got2[owner_core].pop()
        foreign = got2[foreign_core].pop() if foreign_core is not None else None
        ddl.close()
        result["writer_attempts"] = writer_attempts

        insert_phase = Phase("insert")
        insert_retries = 0
        insert_race_hits = 0
        insert_unknown_hits = 0
        t0 = time.perf_counter()
        for i in range(1, rows + 1):
            lat, r, n, races, unknown = timed_retry(
                writer, f"INSERT INTO rd9b VALUES ({i * 2}, {i * 3}, {i % 97}, {i % 13})")
            insert_retries += n
            insert_race_hits += races
            insert_unknown_hits += unknown
            insert_phase.record(lat, r)
        insert_phase.elapsed = time.perf_counter() - t0
        result["insert_ops"] = insert_phase.ops
        result["insert_retries"] = insert_retries
        result["insert_race_hits"] = insert_race_hits
        result["insert_unknown_hits"] = insert_unknown_hits
        errors = insert_phase.errors
        result["insert_errors"] = errors
        result["insert_first_error"] = insert_phase.first_error

        count_reply = writer.cmd("SELECT COUNT(*) FROM rd9b")
        try:
            got_count = int(count_reply.replace("\\n", "\n").split("\n")[-1].split(",")[-1])
        except ValueError:
            got_count = None
        result["count_verify"] = {"expected": rows, "got": got_count,
                                  "ok": got_count == rows, "reply": count_reply}

        # **The discriminator when the count is wrong.** An over-count has
        # two possible causes and they call for opposite fixes: rows read
        # twice (a walk visiting one page from two ranges' heads - a read
        # defect) or rows written twice (the load actually inserting more
        # than it counted - a driver or write defect). Duplicate *ids*
        # tell them apart, and only a failing cell pays for the scan.
        if got_count is not None and got_count != rows:
            ids_reply = writer.cmd("SELECT id FROM rd9b")
            body = ids_reply.replace("\\n", "\n").split("\n")[1:]
            ids = [line.split(",")[0] for line in body if line]
            uniq = set(ids)
            dups = sorted(ids.count(x) for x in uniq if ids.count(x) > 1)[-5:] if len(uniq) != len(ids) else []
            result["count_verify"]["rows_returned"] = len(ids)
            result["count_verify"]["distinct_ids"] = len(uniq)
            result["count_verify"]["duplicate_id_multiplicities"] = dups
            print(f"   !! DIAGNOSIS rows_returned={len(ids)} distinct_ids={len(uniq)} "
                  f"-> {'ROWS READ TWICE' if len(uniq) < len(ids) else 'ROWS WRITTEN TWICE'}")

        # SHOW META off the writer (owner-local counters): row-id refill
        # grants (source-read cross-check for the analytic range count),
        # and the absence of `range_split_decline` (the five gates held).
        meta = writer.cmd("SHOW META")
        result["range_split_decline_present"] = "range_split_decline" in meta
        for key in ("rowid_refill_requests", "rowid_refill_grants"):
            if f"{key}=" in meta:
                result[key] = field(meta, key)

        # Local scan (owner's own session) - the RD6/14a per-range walk,
        # same core as the data.
        scan_local = Phase("scan-local")
        t0 = time.perf_counter()
        for _ in range(scan_ops):
            tt0 = time.perf_counter()
            r = writer.cmd("SELECT * FROM rd9b")
            scan_local.record(time.perf_counter() - tt0, r)
        scan_local.elapsed = time.perf_counter() - t0

        point_local = Phase("point-local")
        for k in range(point_ops):
            pk = 1 + (k * max(1, rows // max(1, point_ops)))
            pk = min(pk, rows)
            tt0 = time.perf_counter()
            r = writer.cmd(f"SELECT * FROM rd9b WHERE id = {pk}")
            point_local.record(time.perf_counter() - tt0, r)

        scan_remote = None
        if foreign is not None:
            scan_remote = Phase("scan-remote")
            for _ in range(scan_ops):
                tt0 = time.perf_counter()
                r = foreign.cmd("SELECT * FROM rd9b")
                scan_remote.record(time.perf_counter() - tt0, r)

        writer.close()
        if foreign is not None:
            foreign.close()

        result["phases"] = {"scan_local": scan_local.summary(),
                            "point_local": point_local.summary()}
        if scan_remote is not None:
            result["phases"]["scan_remote"] = scan_remote.summary()
        result["insert_summary"] = insert_phase.summary()
        result["load1_end"] = os.getloadavg()[0]
        return result
    finally:
        stop_server(port, proc)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=2)
    ap.add_argument("--rows", default="200,1000,10000")
    ap.add_argument("--range-sizes", default="0,2048,4096,8192")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--scan-ops", type=int, default=20)
    ap.add_argument("--point-ops", type=int, default=30)
    ap.add_argument("--port", type=int, default=17600)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    rows_list = [int(x) for x in args.rows.split(",")]
    sizes = [int(x) for x in args.range_sizes.split(",")]

    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    binary = os.path.abspath(args.server)

    all_results = []
    jsonl_path = (args.json + ".jsonl") if args.json else os.path.join(args.workdir, "cells.jsonl")
    open(jsonl_path, "w").close()  # fresh grid, fresh file - never append across runs
    port = args.port
    for rows in rows_list:
        for size in sizes:
            for rep in range(1, args.reps + 1):
                tag = f"r{rows}-s{size}-rep{rep}"
                print(f"== {tag} ==", flush=True)
                port += 1
                # The host-quiet gate raises SystemExit(1) rather than
                # returning - fine for a single-cell invocation, fatal for
                # a 36-cell grid if left to propagate (an earlier run this
                # session lost 33 completed cells' results this way, since
                # --json is written once at the end). Waited out here
                # instead, bounded, so one busy moment costs a wait, not
                # the grid.
                for attempt in range(60):
                    try:
                        res = run_cell(binary, args.workdir, tag, args.cores, port,
                                      rows, size, args.scan_ops, args.point_ops, args.force)
                        break
                    except SystemExit as e:
                        print(f"   host busy ({e}), waiting 5s (attempt {attempt + 1})",
                             flush=True)
                        time.sleep(5)
                else:
                    raise RuntimeError(f"{tag}: host never went quiet after 60 waits")
                res["rep"] = rep
                all_results.append(res)
                # Written after every cell, not only at the end: a crash or
                # a kill loses at most the in-flight cell, never the grid.
                with open(jsonl_path, "a") as jf:
                    jf.write(json.dumps(res) + "\n")
                sl = res["phases"]["scan_local"]
                print(f"   owner_core={res['owner_core']} count_ok={res['count_verify']['ok']} "
                     f"decline={res['range_split_decline_present']} "
                     f"rowid_grants={res.get('rowid_refill_grants')} "
                     f"race_hits={res.get('insert_race_hits')} "
                     f"unknown_hits={res.get('insert_unknown_hits')} "
                     f"scan_local p50={sl['p50_us']:.0f}us p99={sl['p99_us']:.0f}us", flush=True)
                if not res['count_verify']['ok']:
                    print(f"   !! count mismatch: {res['count_verify']} "
                         f"first_error={res.get('insert_first_error')!r}", flush=True)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"rows": rows_list, "range_sizes": sizes, "reps": args.reps,
                       "cells": all_results}, f, indent=2)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
