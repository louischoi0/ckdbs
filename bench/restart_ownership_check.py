#!/usr/bin/env python3
"""Restart-ownership check at >= 3 writer cores: insert into rotated
relations from their owner cores, stop the instance, mount the same data
file again, and read every relation back.

Why this exists. PW1c-7 made ownership survive a restart by the PL-C stamp -
a leased store claims own-stamped pages on the fault, and an unacquired
creation page is re-delivered on request through the publish CREATE TABLE
runs (docs/inflight/in-progress/workplan-peer-writer.md §8). Every exercise of that path so far
ran on a host with one writer core, because `cores` cannot exceed
`hardware_concurrency()` and rotation skips the system core. This is the
first exercise with three.

Correctness, not performance: a release build compiles out `MayWrite` /
`MayFault` (they are `#ifndef NDEBUG`), so the checks a benchmark build does
not make are the ones this script makes by hand - rows in equals rows out,
per relation, before and after the restart; a point-SELECT of a known key
returns the row that was written; the scan count matches the insert count.

Exit status is 0 only when every relation passes both halves.

Usage:
    bench/restart_ownership_check.py --server build-release/kds_server \
        --workdir ~/mcbench/restart --cores 4 --tables 6 --rows 2000
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, DEFAULT_RETRY_DEADLINE_S, collect_connections, field, is_retryable,
    wait_for_port,
)


def write_row(conn, stmt, deadline_s):
    """One statement, retried while the engine says retry. A peer-owned
    relation answers its first INSERT with a lease-refill refusal until the
    grant lands (PW1b, and the three refusals carry `retryable=1` since
    2026-08-25) - treating one as a failure would report a lost row on
    exactly the multi-writer-core host this exercise exists to run on.
    Returns (reply, retries); a refusal that never clears costs the deadline
    and comes back as the error it is."""
    t0 = time.time()
    r = conn.cmd(stmt)
    n = 0
    while is_retryable(r) and time.time() - t0 < deadline_s:
        n += 1
        time.sleep(0.0005)
        r = conn.cmd(stmt)
    return r, n


def write_config(workdir, tag, cores, port, placement, peer_listeners):
    """One config, one data file - and unlike the benchmark driver's, this
    one is written to be mounted twice."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\n"
                f"peer_listeners = {'on' if peer_listeners else 'off'}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    return conf


def start(binary, conf, workdir, tag, port):
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    err = open(stderr_path, "a")
    proc = subprocess.Popen([binary, "--config", conf], stdout=err,
                            stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc


def stop(proc, conn=None):
    """The graceful path: STOP if a session is still open, else SIGTERM. A
    kill would leave the restart measuring recovery instead of ownership."""
    if conn is not None:
        try:
            conn.cmd("STOP")
        except OSError:
            pass
    try:
        proc.wait(timeout=30)
        return "stopped"
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=30)
            return "terminated"
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=15)
            return "killed"


def reply_rows(reply):
    r"""The data rows of a SELECT reply, as lists of field strings.

    A multi-line reply travels as one wire line with `\n` escaped
    (docs/spec/client-manual.md, and the driver's own verify at
    tools/multicore_benchmark.py:437): a comma-separated header line, then
    one line per row. `None` for an error reply, so a caller cannot read a
    refusal as an empty result.

    The token scan this replaced never worked: the escaped reply
    `count(*)\n2000` carries no whitespace at all, so `.split()` yielded the
    single token `count(*)\n2000`, `isdigit()` was false, and every count
    came back None - reporting every relation as having lost every row,
    which is exactly the failure this exercise looks for.
    """
    if reply.startswith("ERR"):
        return None
    lines = [ln for ln in reply.replace("\\n", "\n").splitlines() if ln.strip()]
    return [[f.strip() for f in ln.split(",")] for ln in lines[1:]]


def count_of(reply):
    """`COUNT(*)`'s single value. None unless the reply is exactly one row
    carrying one integer - an error, an empty result and an unexpected shape
    are all None, never 0, because a caller that read None as zero would
    turn a failure into a passing check."""
    rows = reply_rows(reply)
    if not rows or len(rows) != 1:
        return None
    value = rows[0][-1]
    return int(value) if value.lstrip("-").isdigit() else None


def probe_id(reply):
    r"""The primary key of the single row a point-SELECT returned, or None.

    Compared exactly, never as a substring of the reply: `balance` is
    `id * 10`, so at `--rows 2000` the reply for the wrong row id=200
    (balance 2000) still contains the text "2000" and a substring test
    passes on a row that was never asked for.
    """
    rows = reply_rows(reply)
    if not rows or len(rows) != 1:
        return None
    return rows[0][0]


def owner_of(conn, name):
    """DESCRIBE's `owner_core`, or None when the relation does not answer.
    A relation missing after the restart is the finding; raising here would
    discard every finding collected before it."""
    reply = conn.cmd(f"DESCRIBE {name}")
    if reply.startswith("ERR"):
        return None
    try:
        return field(reply, "owner_core")
    except RuntimeError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15700)
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--placement", default="rotate")
    # BooleanOptionalAction, not store_true: with `default=True` the flag
    # could only ever be set, so the three `if args.peer_listeners` branches
    # below had unreachable `else` arms and the core-0-only shape they exist
    # to run was not reachable from the command line.
    ap.add_argument("--peer-listeners", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="one session per owner core; --no-peer-listeners for "
                         "the core-0-only shape")
    ap.add_argument("--retry-deadline", type=float,
                    default=DEFAULT_RETRY_DEADLINE_S,
                    help="seconds an INSERT is retried while the engine says "
                         "retry before it is recorded as a refusal")
    ap.add_argument("--json", default="", help="write the findings here")
    args = ap.parse_args()
    if args.tables < 1 or args.rows < 1:
        ap.error("--tables and --rows must both be at least 1")

    os.makedirs(args.workdir, exist_ok=True)
    conf = write_config(args.workdir, "restart", args.cores, args.port,
                        args.placement, args.peer_listeners)

    findings = dict(cores=args.cores, tables=args.tables, rows=args.rows,
                    placement=args.placement,
                    peer_listeners=bool(args.peer_listeners),
                    before={}, after={}, owner_cores={}, problems=[])

    # ---- first mount: create, write, verify --------------------------------
    proc = start(args.server, conf, args.workdir, "restart", args.port)
    try:
        if args.peer_listeners:
            got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(args.port)

        names = [f"rst{i}" for i in range(args.tables)]
        owner_cores = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} "
                          f"(id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                sys.exit(f"{name}: {r}")
            owner_cores[name] = field(setup.cmd(f"DESCRIBE {name}"), "owner_core")
        findings["owner_cores"] = {k: int(v) for k, v in owner_cores.items()}

        if args.peer_listeners:
            needed = collections.Counter(owner_cores.values())
            per_core, attempts = collect_connections(args.port, needed,
                                                     args.max_connects)
            writers = {n: per_core[owner_cores[n]].pop() for n in names}
            findings["writer_connect_attempts"] = attempts
        else:
            writers = {n: Conn(args.port) for n in names}

        t0 = time.time()
        retries = collections.Counter()
        for name in names:
            w = writers[name]
            for i in range(1, args.rows + 1):
                r, n = write_row(w, f"INSERT INTO {name} VALUES ('o{i % 7}', {i * 10})",
                                 args.retry_deadline)
                retries[name] += n
                if r.startswith("ERR"):
                    findings["problems"].append(
                        f"{name}: INSERT id={i} refused: {r}")
                    break
        findings["insert_seconds"] = time.time() - t0
        # Recorded, never hidden: a relation that needed thousands of retries
        # passed the row count and still says something about the refill path.
        findings["insert_retries"] = {k: v for k, v in retries.items() if v}

        # The known key every half re-reads. The pk is engine-issued, so it
        # is *discovered* rather than assumed: the last row written carries a
        # unique `balance`, and the id it was given is what the point-SELECT
        # after the restart must return. Assuming `id == args.rows` would be
        # an untested premise standing in for the thing under test.
        probe_balance = args.rows * 10
        probe_keys = {}
        for name in names:
            w = writers[name]
            cnt = count_of(w.cmd(f"SELECT COUNT(*) FROM {name}"))
            found = w.cmd(f"SELECT id, owner, balance FROM {name} "
                          f"WHERE balance = {probe_balance}")
            key = probe_id(found)
            probe_keys[name] = key
            probe = (w.cmd(f"SELECT id, owner, balance FROM {name} "
                           f"WHERE id = {key}") if key is not None else found)
            findings["before"][name] = dict(count=cnt, probe=probe,
                                            probe_key=key)
            if cnt != args.rows:
                findings["problems"].append(
                    f"{name}: before restart rows in={args.rows} out={cnt}")
            if key is None:
                findings["problems"].append(
                    f"{name}: before restart could not find the probe row "
                    f"(balance={probe_balance}) -> {found!r}")
            elif probe_id(probe) != key:
                findings["problems"].append(
                    f"{name}: before restart probe key {key} -> {probe!r}")
        findings["probe_keys"] = probe_keys

        setup.close()
        first_conn = writers[names[0]]
        findings["shutdown"] = stop(proc, first_conn)
        # The claim this exercise makes is about a *graceful* stop: a SIGTERM
        # or a SIGKILL turns the second mount into a recovery test, which is
        # a different (and weaker) statement about ownership. Exiting 0 on it
        # would report the untested claim as tested.
        if findings["shutdown"] != "stopped":
            findings["problems"].append(
                f"the first mount did not stop on STOP ({findings['shutdown']}): "
                f"the second mount then exercises crash recovery, not restart "
                f"ownership")
        for w in writers.values():
            w.close()
    finally:
        if proc.poll() is None:
            stop(proc)

    # ---- second mount: read every relation back ----------------------------
    proc = start(args.server, conf, args.workdir, "restart", args.port)
    try:
        if args.peer_listeners:
            needed = collections.Counter(owner_cores.values())
            per_core, attempts = collect_connections(args.port, needed,
                                                     args.max_connects)
            readers = {n: per_core[owner_cores[n]].pop() for n in names}
            findings["reader_connect_attempts"] = attempts
        else:
            readers = {n: Conn(args.port) for n in names}

        meta = readers[names[0]].cmd("SHOW META")
        findings["meta_after_restart"] = meta

        for name in names:
            c = readers[name]
            after_owner = owner_of(c, name)
            cnt = count_of(c.cmd(f"SELECT COUNT(*) FROM {name}"))
            key = probe_keys.get(name)
            probe = (c.cmd(f"SELECT id, owner, balance FROM {name} "
                           f"WHERE id = {key}") if key is not None else "ERR no key")
            scan = count_of(c.cmd(f"SELECT COUNT(*) FROM {name} WHERE balance >= 0"))
            findings["after"][name] = dict(count=cnt, probe=probe, scan=scan,
                                           owner_core=after_owner)
            if cnt != args.rows:
                findings["problems"].append(
                    f"{name}: after restart rows in={args.rows} out={cnt}")
            if scan != args.rows:
                findings["problems"].append(
                    f"{name}: after restart scan={scan} != inserted {args.rows}")
            if key is None or probe_id(probe) != key:
                findings["problems"].append(
                    f"{name}: after restart probe key {key} -> {probe!r}")
            if after_owner is None:
                findings["problems"].append(
                    f"{name}: after restart DESCRIBE reports no owner_core - "
                    f"the relation did not survive the mount")
            elif after_owner != findings["owner_cores"][name]:
                findings["problems"].append(
                    f"{name}: owner_core moved across the restart: "
                    f"{findings['owner_cores'][name]} -> {after_owner}")

        findings["shutdown2"] = stop(proc, readers[names[0]])
    finally:
        if proc.poll() is None:
            stop(proc)

    print(json.dumps(findings, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(findings, fh, indent=2)

    if findings["problems"]:
        print(f"\nFAIL: {len(findings['problems'])} problem(s)", file=sys.stderr)
        for p in findings["problems"]:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print(f"\nPASS: {args.tables} relations x {args.rows} rows survived the "
          f"restart on owner cores {sorted(set(findings['owner_cores'].values()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
