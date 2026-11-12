#!/usr/bin/env python3
"""CQ-S0's premise cells: what a row held by an explicit transaction costs
its other writers, and how much of that a transaction that writes the row
last gets back.

`instructions/v3.2.0/workorder-cq-column-reservation.md` section 6, CQ-S0.
No engine code is measured against another here: one server, one binary,
and the arms differ only in what the client sends.

The transaction, per session, repeated for `--seconds`:

    BEGIN
    UPDATE cq_acct SET c = c - 1 WHERE id = <target>      -- the debit
    UPDATE cq_work SET v = v + 1 WHERE id = <own row j>   -- k of these
    COMMIT

and the five arms are where the debit goes and what it targets:

* **hot** - the debit first, every session on row 1. The row is held from
  the first statement to the decide, across k statements and k + 1 client
  round trips. This is the window a column reservation would close.
* **hot-last** - the same debit as the last statement: the client orders
  its transaction, with a round trip between the debit and `COMMIT`.
* **hot-last-piped** - the debit last and written to the socket together
  with `COMMIT`, so no client round trip sits inside the hold. This is the
  order's A2 as its section 3 states it, and the shape C1's commit-time
  apply has: the hold runs from the write to the decide.
* **disjoint** - the debit first, each session on a row of its own: no row
  is shared. (The rows still share leaf pages - nine `cq_acct` rows on one
  leaf - so this is "no row conflict", not "no contention".)
* **disjoint-piped** - hot-last-piped's shape on a row of the session's
  own, the control for piping itself.

Why processes and not threads: eight sessions under one GIL make the client
the bottleneck, and a client-bound run shows hot and disjoint equal however
the server waits. One process per session, the pool pinned by the caller
(`taskset`) away from the server's CPUs. `--server-pid` adds the server's
CPU time per cell, which is what says whether a cell was server-bound.

What a cell reports: committed transactions per second; the transaction's
latency percentiles (p0, p25, p50, p95, p99, max); the same for the debit
statement and for `COMMIT` alone, over statements that succeeded (the
client's view of where the wait sits); refusals by class - decided-newer
(`was written by transaction`), fault net, deadlock, other; the core each
session was on at its start and its end; and an oracle over every row the
cell could write: `cq_acct` fell and `cq_work` rose by exactly what each
session counted as committed, row by row.

`--pause-ms` sleeps in the client after the first statement (M4': a hold
above the lock family's 1 s fault net).

A heartbeat file is touched and a status line appended at every cell
(`--heartbeat`, `--status`); the watchdog polls those, never a process.
"""

import argparse
import collections
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kwp  # noqa: E402
import sys_meta  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

ACCT = "cq_acct"
WORK = "cq_work"
HOT_ID = 1
OWN_BASE = 1000        # session s debits row OWN_BASE + s in the disjoint arms
WORK_STRIDE = 1000     # session s's work rows are (s + 1) * WORK_STRIDE + j

ARMS = ("hot", "hot-last", "hot-last-piped", "disjoint", "disjoint-piped")


def host_state():
    """bench/README.md rule 4: the load and any competing build, per cell."""
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "unavailable"
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


def server_cpu_s(pid):
    """utime + stime of the server process, in seconds; None without a pid."""
    if not pid:
        return None
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
    except OSError:
        return None
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def classify(message):
    m = message.lower()
    if "was written by transaction" in m:
        return "decided_newer"
    if "fault net" in m:
        return "fault_net"
    if "deadlock" in m:
        return "deadlock"
    return "other"


def shape(message):
    """A refusal's text with its numbers removed, so distinct texts count."""
    return re.sub(r"\d+", "N", message)[:160]


def session_core(conn):
    _fields, rows, tag, _n = conn.execute("SHOW META")
    core = sys_meta.from_rows(rows, tag).get("core")
    return None if core is None else int(core)


def update_one(conn, sql):
    """One point `UPDATE` that must write its row: anything else is the
    cell's failure, not a transaction to count."""
    _fields, _rows, tag, affected = conn.execute(sql)
    if affected != 1:
        raise RuntimeError(f"{sql!r} answered {tag!r} affected={affected}")


def piped_update_commit(conn, sql):
    """The debit and `COMMIT` in one write. Returns None when both
    succeeded, else the KwpError that refused the transaction; either way
    the session is left with no transaction open.

    The statement batch is `Connection.execute`'s (PARSE, BIND, EXECUTE,
    CLOSE, SYNC) with `C_TXN_COMMIT` behind it. A refused statement arms
    skip-to-sync and the SYNC ends it; the commit frame then meets a failed
    transaction and is refused, leaving the transaction open, so the SYNC
    for that refusal and the `C_TXN_ABORT` go out in one write - the piped
    arm's failure path costs the round trips the unpiped one's does. A
    `COMMIT` refused after a debit that succeeded has already ended the
    transaction server-side.
    """
    portal = "q%d" % conn._next_portal
    conn._next_portal += 1
    batch = kwp.frame(kwp.C_PARSE, kwp._Writer().s("").text(sql).take())
    batch += kwp.frame(kwp.C_BIND, kwp._Writer().s(portal).s("").u16(0).take())
    batch += kwp.frame(kwp.C_EXECUTE, kwp._Writer().s(portal).u32(0).take())
    batch += kwp.frame(kwp.C_CLOSE, kwp._Writer().u8(2).s(portal).take())
    batch += kwp.frame(kwp.C_SYNC)
    batch += kwp.frame(kwp.C_TXN_COMMIT)
    conn.sock.sendall(batch)

    stmt_error, affected = None, None
    while True:
        ftype, _flags, payload = conn._recv_frame()
        if ftype == kwp.S_ERROR:
            stmt_error = conn._error(payload)
        elif ftype == kwp.S_COMPLETE:
            r = kwp._Reader(payload)
            r.text()
            affected = r.u64()
        elif ftype == kwp.S_READY:
            break
    ftype, _flags, payload = conn._recv_frame()
    if ftype == kwp.S_TXN_OK:
        if stmt_error is not None or affected != 1:
            raise RuntimeError(f"piped commit acknowledged after {stmt_error!r} "
                               f"affected={affected}")
        return None
    if ftype != kwp.S_ERROR:
        raise ConnectionError("expected S_TXN_OK or S_ERROR after a piped commit, got %d" % ftype)
    commit_error = conn._error(payload)
    if stmt_error is None:
        conn._send(kwp.C_SYNC)
    else:
        conn.sock.sendall(kwp.frame(kwp.C_SYNC) + kwp.frame(kwp.C_TXN_ABORT))
    while True:
        ftype, _, _ = conn._recv_frame()
        if ftype == kwp.S_READY:
            break
    if stmt_error is not None:
        conn._await_txn_ok()
    return stmt_error or commit_error


def worker(index, a, arm, k, t_start, t_end, pipe):
    out = {"index": index, "committed": 0, "refused": collections.Counter(),
           "shapes": collections.Counter(), "txn": [], "debit": [], "commit": [],
           "fatal": None, "core_start": None, "core_end": None}
    try:
        conn = kwp.Connection(a.host, a.port, timeout=30.0)
        out["core_start"] = session_core(conn)
        target = HOT_ID if arm.startswith("hot") else OWN_BASE + index
        debit = f"UPDATE {ACCT} SET c = c - 1 WHERE id = {target}"
        work = [f"UPDATE {WORK} SET v = v + 1 WHERE id = {(index + 1) * WORK_STRIDE + j}"
                for j in range(k)]
        piped = arm.endswith("-piped")
        first = not piped and not arm.endswith("-last")
        pause = a.pause_ms / 1000.0

        def timed_debit():
            d0 = time.perf_counter()
            update_one(conn, debit)
            out["debit"].append(time.perf_counter() - d0)

        while time.time() < t_start:
            time.sleep(0.001)
        while time.time() < t_end:
            t0 = time.perf_counter()
            conn.begin()
            error = None
            open_txn = True
            try:
                if first:
                    timed_debit()
                    if pause:
                        time.sleep(pause)
                for w in work:
                    update_one(conn, w)
                if not first and not piped:
                    timed_debit()
                c0 = time.perf_counter()
                if piped:
                    error = piped_update_commit(conn, debit)
                    open_txn = False
                else:
                    open_txn = False   # a refused COMMIT ends the transaction
                    conn.commit()
            except kwp.KwpError as e:
                error = e
            if error is not None:
                out["refused"][classify(error.message)] += 1
                out["shapes"][shape(error.message)] += 1
                if open_txn:
                    try:
                        conn.rollback()
                    except kwp.KwpError:
                        pass
                continue
            now = time.perf_counter()
            out["commit"].append(now - c0)
            out["txn"].append(now - t0)
            out["committed"] += 1
        out["core_end"] = session_core(conn)
        conn.close()
    except Exception as e:  # a session that died is the cell's finding
        out["fatal"] = repr(e)
    out["refused"] = dict(out["refused"])
    out["shapes"] = dict(out["shapes"])
    pipe.send(out)
    pipe.close()


def pcts(samples):
    if not samples:
        return None
    s = sorted(samples)
    us = lambda x: round(x * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p95_us": us(nearest_rank(s, 95)),
            "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}


def read_rows(conn, table):
    """{id: value} for every row of a two-column driver relation."""
    fields, rows, _t, _n = conn.execute(f"SELECT * FROM {table}")
    render = [kwp.renderer_for(f) for f in fields]
    return {int(render[0](r[0])): int(render[1](r[1])) for r in rows}


def oracle(arm, k, sessions, per_session, acct0, acct1, work0, work1):
    """Every row the cell could write, against what the sessions counted."""
    bad = []
    want_acct = dict(acct0)
    for s in range(sessions):
        want_acct[HOT_ID if arm.startswith("hot") else OWN_BASE + s] -= per_session[s]
    for rid, v in want_acct.items():
        if acct1.get(rid) != v:
            bad.append(f"{ACCT} id={rid}: {acct1.get(rid)} != {v}")
    for s in range(sessions):
        for rid, v0 in work0.items():
            if rid // WORK_STRIDE != s + 1:
                continue
            j = rid % WORK_STRIDE
            v = v0 + (per_session[s] if j < k else 0)
            if work1.get(rid) != v:
                bad.append(f"{WORK} id={rid}: {work1.get(rid)} != {v}")
    return {"ok": not bad, "rows_checked": len(acct0) + len(work0), "mismatches": bad[:8]}


def lock_wait_sample(conn, seen):
    """The sys.lock_waits rows this cell added: outcomes, units and
    waited_us. `seen` maps a core to the highest `seq` already counted and is
    advanced here. A ring of 4096 per core, so a cell that parks more than
    that on one core keeps only its last 4096 there: a sample biased to the
    cell's end, and `rows_in_ring` says how full the ring is."""
    try:
        fields, rows, _t, _n = conn.execute("SELECT * FROM sys.lock_waits")
    except kwp.KwpError as e:
        return {"error": e.message}
    names = [f["name"] for f in fields]
    renders = [kwp.renderer_for(f) for f in fields]
    out = {"rows_in_ring": len(rows), "by_outcome": collections.Counter(),
           "by_unit_mode": collections.Counter()}
    waited = []
    top = dict(seen)
    for row in rows:
        rec = {n: (None if raw is None else r(raw)) for n, r, raw in zip(names, renders, row)}
        core, seq = rec.get("core"), int(rec.get("seq"))
        if seq <= seen.get(core, -1):
            continue
        top[core] = max(top.get(core, -1), seq)
        out["by_outcome"][rec.get("outcome")] += 1
        out["by_unit_mode"]["%s/%s" % (rec.get("unit"), rec.get("mode"))] += 1
        waited.append(float(rec.get("waited_us")))
    seen.clear()
    seen.update(top)
    waited.sort()
    out["by_outcome"] = dict(out["by_outcome"])
    out["by_unit_mode"] = dict(out["by_unit_mode"])
    out["new_rows"] = len(waited)
    if waited:
        out["waited_p50_us"] = nearest_rank(waited, 50)
        out["waited_p99_us"] = nearest_rank(waited, 99)
        out["waited_max_us"] = waited[-1]
    return out


def run_cell(a, admin, arm, k, rep, seen):
    acct0, work0 = read_rows(admin, ACCT), read_rows(admin, WORK)
    host = host_state()
    cpu0 = server_cpu_s(a.server_pid)
    t_start = time.time() + 0.6
    t_end = t_start + a.seconds
    procs, pipes = [], []
    for i in range(a.sessions):
        parent, child = mp.Pipe(duplex=False)
        p = mp.Process(target=worker, args=(i, a, arm, k, t_start, t_end, child))
        p.start()
        child.close()
        procs.append(p)
        pipes.append(parent)
    results = sorted((pipe.recv() for pipe in pipes), key=lambda r: r["index"])
    for p in procs:
        p.join()
    cpu1 = server_cpu_s(a.server_pid)
    acct1, work1 = read_rows(admin, ACCT), read_rows(admin, WORK)

    committed = sum(r["committed"] for r in results)
    refused, shapes = collections.Counter(), collections.Counter()
    for r in results:
        refused.update(r["refused"])
        shapes.update(r["shapes"])
    per_session = [r["committed"] for r in results]
    return {
        "arm": arm, "k": k, "rep": rep, "sessions": a.sessions, "seconds": a.seconds,
        "pause_ms": a.pause_ms, "label": a.label,
        "committed": committed, "tps": round(committed / a.seconds, 1),
        "refused": dict(refused),
        "refusal_shapes": dict(shapes.most_common(4)),
        "txn": pcts([x for r in results for x in r["txn"]]),
        "debit": pcts([x for r in results for x in r["debit"]]),
        "commit": pcts([x for r in results for x in r["commit"]]),
        "per_session_committed": per_session,
        "cores_start": [r["core_start"] for r in results],
        "cores_end": [r["core_end"] for r in results],
        "fatal": [r["fatal"] for r in results if r["fatal"]],
        "host": host,
        # The server's CPU over the cell, spawn and joins included: cores
        # busy = this / (seconds + ~1). A lower bound on the busy share.
        "server_cpu_s": None if cpu0 is None or cpu1 is None else round(cpu1 - cpu0, 3),
        "lock_waits": lock_wait_sample(admin, seen),
        "oracle": oracle(arm, k, a.sessions, per_session, acct0, acct1, work0, work1),
    }


def setup(admin, sessions, kmax):
    for t in (ACCT, WORK):
        try:
            admin.execute(f"DROP TABLE {t}")
        except kwp.KwpError:
            pass
    admin.execute(f"CREATE TABLE {ACCT} (id int64, c int64) BTREE")
    admin.execute(f"CREATE TABLE {WORK} (id int64, v int64) BTREE")
    admin.execute(f"INSERT INTO {ACCT} VALUES ({HOT_ID}, 0)")
    for s in range(sessions):
        admin.execute(f"INSERT INTO {ACCT} VALUES ({OWN_BASE + s}, 0)")
        for j in range(kmax):
            admin.execute(f"INSERT INTO {WORK} VALUES ({(s + 1) * WORK_STRIDE + j}, 0)")


def arm_order(arms, rep):
    """Each repetition starts from a different arm (a rotation by two), and
    odd repetitions run backwards, so no arm is always first or always
    behind the same one."""
    n = len(arms)
    shift = (2 * rep) % n
    order = arms[shift:] + arms[:shift]
    return order[::-1] if rep % 2 else order


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True, help="the server's KWP/1 port, chosen")
    p.add_argument("--server-pid", type=int, default=0,
                   help="the server's pid, for its CPU time per cell")
    p.add_argument("--label", default="", help="written into every cell (e.g. the durability)")
    p.add_argument("--sessions", type=int, default=8)
    p.add_argument("--k", default="0,8", help="work statements per transaction, a list")
    p.add_argument("--arms", default=",".join(ARMS))
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--seconds", type=float, default=6.0, help="per cell")
    p.add_argument("--pause-ms", type=float, default=0.0,
                   help="client sleep after the first statement (M4')")
    p.add_argument("--json", required=True)
    p.add_argument("--heartbeat", default=None)
    p.add_argument("--status", default=None)
    a = p.parse_args()

    ks = [int(x) for x in a.k.split(",")]
    arms = [x for x in a.arms.split(",") if x]
    for arm in arms:
        if arm not in ARMS:
            sys.exit(f"unknown arm {arm}")

    admin = kwp.Connection(a.host, a.port, timeout=30.0)
    setup(admin, a.sessions, max(ks) if ks else 0)

    def beat(line):
        if a.heartbeat:
            with open(a.heartbeat, "w") as f:
                f.write(str(time.time()))
        if a.status:
            with open(a.status, "a") as f:
                f.write(time.strftime("%H:%M:%S ", time.gmtime()) + line + "\n")

    cells = []
    seen = {}
    lock_wait_sample(admin, seen)
    for rep in range(a.reps):
        for k in ks:
            for arm in arm_order(arms, rep):
                beat(f"{a.label} rep {rep} k {k} {arm} start")
                cell = run_cell(a, admin, arm, k, rep, seen)
                cells.append(cell)
                beat(f"{a.label} rep {rep} k {k} {arm} tps {cell['tps']} "
                     f"refused {cell['refused']} oracle {cell['oracle']['ok']}")
                print(json.dumps({x: cell[x] for x in ("label", "arm", "k", "rep", "tps",
                                                       "refused")}), flush=True)
                with open(a.json, "w") as f:
                    json.dump({"args": vars(a), "cells": cells}, f, indent=1)
    admin.close()
    beat(f"{a.label} driver finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
