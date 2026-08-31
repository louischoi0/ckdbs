#!/usr/bin/env python3
"""XE4's substitute cell: cross-owner **commit** latency alone, no reads.

**Why this file exists rather than a plain rerun of `wal_sync_decomposition_probe.py`.**
`instructions/v2.7.1/workorder-xd.md`'s XE4 asks for that driver's scenario-2
booking cells under `peer_listeners = on`. As of this session
(`eecda94`, "Milestone KW: KWP/1 is the protocol the server speaks",
merged into this branch ahead of XE1) that shape cannot run at all: every
booking's first statement (`book_once`'s cargo lookup,
`tools/scenario2_freight.py`) is a `SELECT` the booker's own session must
ship to the relation's owner, and `CommandDispatcher::ShipStatement`
refuses a shipped **read** to a session carrying a KWP result sink
(`command_dispatcher.cpp` sec.4256-4262, "a shipped read cannot answer a
typed client" - `known-gaps.md`'s own documented, acknowledged gap, not
an XE1 regression: reproducible identically on the pre-XE1 binary, the
XE1 binary and current HEAD). Confirmed empirically before this file was
written - `tools/scenario2_freight.py --bookers 1 --bookings N` against a
`peer_listeners = on` instance refuses every attempt at the cargo lookup,
under every server this session tried.

A cross-owner **write-only** transaction is unaffected - `ShipStatement`
ships a write's answer as a completion tag unchanged, which is what a
completion tag is for (same comment, same lines). So this file measures
the one thing XE4 actually asks about - the participant's decide-commit
latency under D2 - with the shape `txn_2pc_kill_matrix_probe.py` already
uses for the correctness matrix: `BEGIN`; one `INSERT` into each of two
relations owned by two different peer cores; `COMMIT`, coordinated from a
session on neither owner's core. No `SELECT` anywhere in the measured
path, so nothing here can hit the refusal above.

This is **not** scenario 2 and does not claim to be; it does not exercise
the per-statement shipping cost a real booking pays for every one of its
6-8 statements (`results-scenario2-cores` sec.5), only the last of them -
`COMMIT` - which is the leg XE1 changed. Read the commit-latency numbers
here as a lower bound on what a real booking's cross-owner ratio would
be, not as a replacement for `results-xd-commit-decomposition`'s own
booking numbers, which remain the standing measurement of that shape (at
`951a91a`, before `eecda94` landed).

Usage (`--cores 2` for the one-participant shape comparable to XD's
scenario-2 booking; `--cores 3`+ for R6-B's `xowner-2` two-participant one):
    bench/xe4_crossowner_commit_probe.py --server /path/to/kds_server-<sha> \\
        --workdir ~/bench-xe/xowner --label xe4-group-b1-base --port 15960 \\
        --cores 2 --durability group --concurrency 1 --txns 2000 \\
        --json out.json
"""

import argparse
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)

from bench_common import Phase  # noqa: E402
from ckdbs_cli import ServerConnection  # noqa: E402


def write_conf(workdir, port, cores, durability):
    conf = os.path.join(workdir, "kds.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                # `rotate` never places on core 0 (`core_placement.hpp`), so
                # both relations land on two peer cores and core 0 stays
                # free to coordinate - the same shape
                # `txn_2pc_kill_matrix_probe.py` uses.
                f"placement = rotate\npeer_listeners = on\n"
                f"durability = {durability}\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = info\n")
    return conf


def wait_up(port, proc, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        if proc.poll() is not None:
            return False, f"exited rc={proc.returncode} before listening"
        try:
            c = ServerConnection("127.0.0.1", port, timeout=2.0)
            c.close()
            return True, None
        except OSError:
            time.sleep(0.05)
    return False, f"no listener within {deadline_s}s"


def field(reply, key):
    for tok in reply.split():
        if tok.startswith(key + "="):
            return tok[len(key) + 1:]
    return None


def meta_fields(meta, prefixes=("wal_", "xowner_")):
    out = {}
    for tok in meta.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if any(k.startswith(p) for p in prefixes):
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def session_on_core(port, core, tries=200):
    """A KWP connection guaranteed to land on `core` - retried the same way
    `txn_2pc_kill_matrix_probe.py`'s own helper does, since
    `peer_listeners = on` spreads a fresh connection over every core's
    listener under `SO_REUSEPORT` and a client cannot choose which one it
    is accepted on."""
    spare = []
    for _ in range(tries):
        c = ServerConnection("127.0.0.1", port, timeout=10.0)
        if field(c.send_command("SHOW META"), "core") == str(core):
            for s in spare:
                s.close()
            return c
        spare.append(c)
    for s in spare:
        s.close()
    raise RuntimeError(f"no session landed on core {core}")


def worker(index, port, txns, result_q):
    """One coordinator connection, `txns` cross-owner transactions run back
    to back, each timed around `COMMIT` alone - the leg XE1 changed."""
    try:
        conn = session_on_core(port, 0)
    except (RuntimeError, ConnectionError, OSError) as e:
        result_q.put({"index": index, "fatal": f"{type(e).__name__}: {e}"})
        return
    latencies = []
    committed = 0
    errors = 0
    retries = 0
    first_error = None
    try:
        i = 0
        # Equal work is a **committed** count (rule 7), not an attempted
        # one: a transient lease-refill conflict (`TXN_CONFLICT
        # retryable=1`, the row-id lease grant's own cadence, unrelated to
        # XE1) is retried in place rather than counted as the booking's
        # cost. Bounded per attempt so a genuine, non-transient refusal
        # still surfaces as an error rather than spinning.
        while committed < txns:
            i += 1
            if i > txns * 20:
                break  # a real ceiling, never reached on a healthy path
            r = conn.send_command("BEGIN")
            if r.startswith("ERR"):
                if "retryable=1" in r:
                    retries += 1
                    continue
                errors += 1
                first_error = first_error or r
                continue
            key = index * 10_000_000 + i
            # The id column is omitted, letting each owner core issue its
            # own - a caller-supplied pk is refused on a peer core by
            # design (`workplan-peer-writer.md` sec.7a: admitting one would
            # write the relation's catalog row, the system core's page).
            r0 = conn.send_command(f"INSERT INTO t0 VALUES ('w', {key})")
            r1 = conn.send_command(f"INSERT INTO t1 VALUES ('w', {key})") if not r0.startswith("ERR") else ""
            if r0.startswith("ERR") or r1.startswith("ERR"):
                conn.send_command("ROLLBACK")
                bad = r0 if r0.startswith("ERR") else r1
                if "retryable=1" in bad:
                    retries += 1
                    continue
                errors += 1
                first_error = first_error or bad
                continue
            t0 = time.perf_counter()
            r = conn.send_command("COMMIT")
            dt = time.perf_counter() - t0
            if r.startswith("ERR"):
                if "retryable=1" in r:
                    retries += 1
                    continue
                errors += 1
                first_error = first_error or r
                continue
            latencies.append(dt)
            committed += 1
    finally:
        conn.close()
    result_q.put({"index": index, "latencies": latencies, "committed": committed,
                 "errors": errors, "retries": retries, "first_error": first_error})


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", type=int, default=3)
    ap.add_argument("--durability", choices=("group", "strict", "relaxed"), default="group")
    ap.add_argument("--concurrency", type=int, default=1, help="number of coordinator connections (b)")
    ap.add_argument("--txns", type=int, default=2000, help="total committed transactions, split across workers")
    ap.add_argument("--mount-timeout", type=float, default=30.0)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    cell_dir = os.path.join(args.workdir, args.label)
    if os.path.exists(cell_dir):
        shutil.rmtree(cell_dir)
    os.makedirs(cell_dir, exist_ok=True)
    conf = write_conf(cell_dir, args.port, args.cores, args.durability)

    err_path = os.path.join(cell_dir, "s.stderr")
    with open(err_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)

    out = {"label": args.label, "cores": args.cores, "durability": args.durability,
          "concurrency": args.concurrency, "txns": args.txns}
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        if not up:
            out["error"] = why
            print(json.dumps(out, indent=2))
            return 1

        setup = session_on_core(args.port, 0)
        for t in ("t0", "t1"):
            r = setup.send_command(f"CREATE TABLE {t} (id int64, tag varchar, n int64) BTREE")
            if r.startswith("ERR"):
                out["error"] = f"CREATE {t}: {r[:200]}"
                print(json.dumps(out, indent=2))
                return 1
        owners = {t: field(setup.send_command(f"DESCRIBE {t}"), "owner_core") for t in ("t0", "t1")}
        out["owners"] = owners
        # Both must be peer-owned (never core 0, the coordinator's own
        # core); whether they share **one** owner (a single participant -
        # `cores = 2`, `rotate` has nowhere else to put the second table,
        # which is the shape XD's own 1-participant booking measured) or
        # land on **two** (`cores = 3`+, R6-B's `xowner-2` shape) is a
        # property of `--cores`, recorded rather than required.
        out["participants"] = len(set(owners.values()))
        if "0" in owners.values():
            out["error"] = f"a relation landed on core 0 (the coordinator's own): {owners}"
            print(json.dumps(out, indent=2))
            return 1

        before = {}
        for core in sorted(set(int(v) for v in owners.values()) | {0}):
            c = session_on_core(args.port, core)
            before[core] = meta_fields(c.send_command("SHOW META"))
            c.close()

        per_worker = -(-args.txns // args.concurrency)
        result_q = multiprocessing.Queue()
        workers = [multiprocessing.Process(target=worker, args=(i, args.port, per_worker, result_q))
                  for i in range(args.concurrency)]
        t_run = time.perf_counter()
        for w in workers:
            w.start()
        results = [result_q.get() for _ in workers]
        for w in workers:
            w.join()
        run_elapsed = time.perf_counter() - t_run

        fatal = [r for r in results if "fatal" in r]
        if fatal:
            out["error"] = f"worker {fatal[0]['index']} fatal: {fatal[0]['fatal']}"
            print(json.dumps(out, indent=2))
            return 1

        phase = Phase("commit")
        phase.elapsed = run_elapsed
        committed = 0
        errors = 0
        retries = 0
        first_error = None
        for r in results:
            committed += r["committed"]
            errors += r["errors"]
            retries += r.get("retries", 0)
            if r["errors"] and first_error is None:
                first_error = r["first_error"]
            for lat in r["latencies"]:
                phase.record(lat, "OK")
        out["committed"] = committed
        out["errors"] = errors
        out["retries"] = retries
        out["first_error"] = first_error
        out["run_elapsed_s"] = round(run_elapsed, 4)
        out["tps"] = committed / run_elapsed if run_elapsed > 0 else 0.0
        out["commit"] = phase.summary()

        after = {}
        for core in before:
            c = session_on_core(args.port, core)
            after[core] = meta_fields(c.send_command("SHOW META"))
            c.close()
        total_syncs_delta = sum(after[c].get("wal_syncs", 0) - before[c].get("wal_syncs", 0)
                                for c in before)
        out["total_wal_syncs_delta"] = total_syncs_delta
        out["syncs_per_commit"] = total_syncs_delta / committed if committed else None

        # **XF4's per-leg times**, one block per core that reported any
        # (`commit_phase_stats.hpp`; absent on a core that walked no leg,
        # which is the engine's absent-rather-than-zeroed rule and is why
        # this loop tests for the `_n` field rather than defaulting it).
        #
        # `_n` and `_us` are counters and are differenced; `_max_us` is a
        # running maximum and is taken from `after` as-is - every cell here
        # starts a fresh server, so the process's max *is* the run's max.
        # A leg with no walks is omitted rather than reported as 0/0.
        legs = {}
        for core in before:
            names = sorted({k[:-2] for k in after[core] if k.endswith("_n")})
            per_core = {}
            for name in names:
                n = after[core].get(name + "_n", 0) - before[core].get(name + "_n", 0)
                if n <= 0:
                    continue
                us = after[core].get(name + "_us", 0) - before[core].get(name + "_us", 0)
                per_core[name] = {
                    "n": n,
                    "total_us": us,
                    "mean_us": round(us / n, 1),
                    "max_us": after[core].get(name + "_max_us", 0),
                }
            if per_core:
                legs[str(core)] = per_core
        if legs:
            out["commit_legs"] = legs

        try:
            setup.send_command("STOP")
        except (ConnectionError, OSError):
            pass  # KWP's STOP is unreachable (known-gaps.md); `finally` below force-stops
        setup.close()
    finally:
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
