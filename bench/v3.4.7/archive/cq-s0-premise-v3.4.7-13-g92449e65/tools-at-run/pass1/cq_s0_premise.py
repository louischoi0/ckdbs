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
* **hot-last** - the same debit as the last statement (A2: the client
  orders its transaction).
* **hot-last-piped** - the debit last and written to the socket together
  with `COMMIT`, so no client round trip sits inside the hold. This is the
  shape C1's commit-time apply has: the hold runs from the write to the
  decide.
* **disjoint** - the debit first, each session on a row of its own: no
  scheme can beat this one's order of statements.
* **disjoint-piped** - hot-last-piped's shape on a row of the session's
  own. Piping saves a round trip whatever the row, so the contention left
  in hot-last-piped is read against *this* arm, not against disjoint.

Why processes and not threads: eight sessions under one GIL make the client
the bottleneck, and a client-bound run shows hot and disjoint equal however
the server waits. One process per session, the pool pinned by the caller
(`taskset`) away from the server's CPUs.

What a cell reports: committed transactions per second, the transaction's
latency percentiles (p0, p25, p50, p95, p99, max), the same for the debit
statement and for `COMMIT` alone (the client's view of where the wait
sits), refusals by class - decided-newer (`was written by transaction`),
deadlock, fault net, other - and a serial-oracle check: row 1's `c` after
the hot cells fell by exactly the transactions they committed.

`--pause-ms` sleeps in the client after the first statement (M4': a hold
above the lock family's 1 s fault net).

A heartbeat file is touched and a status line appended at every cell
(`--heartbeat`, `--status`); the watchdog polls those, never a process.
"""

import argparse
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kwp  # noqa: E402
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
    return {"loadavg": load, "competing": len(busy.splitlines()) if busy else 0}


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


def exec_tag(conn, sql):
    _fields, _rows, tag, _affected = conn.execute(sql)
    return tag


def piped_update_commit(conn, sql):
    """The debit and `COMMIT` in one write. Returns (statement_error,
    commit_error), each None or a KwpError.

    The statement batch is `Connection.execute`'s (PARSE, BIND, EXECUTE,
    CLOSE, SYNC) with `C_TXN_COMMIT` behind it. A refused statement arms
    skip-to-sync, the SYNC ends it, and the commit frame is then read on its
    own: answered `S_TXN_OK` or refused, and a refusal needs its own SYNC.
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

    stmt_error = None
    while True:
        ftype, _flags, payload = conn._recv_frame()
        if ftype == kwp.S_ERROR:
            stmt_error = conn._error(payload)
        elif ftype == kwp.S_READY:
            break
    commit_error = None
    ftype, _flags, payload = conn._recv_frame()
    if ftype == kwp.S_ERROR:
        commit_error = conn._error(payload)
        conn._send(kwp.C_SYNC)
        while True:
            ftype, _, _ = conn._recv_frame()
            if ftype == kwp.S_READY:
                break
    elif ftype != kwp.S_TXN_OK:
        raise ConnectionError("expected S_TXN_OK after a piped commit, got %d" % ftype)
    return stmt_error, commit_error


def worker(index, a, arm, k, t_start, t_end, pipe):
    out = {"index": index, "committed": 0, "refused": {}, "shapes": {},
           "txn": [], "debit": [], "commit": [], "fatal": None, "core_start": None}
    try:
        conn = kwp.Connection(a.host, a.port, timeout=30.0)
        try:
            _f, rows, _t, _n = conn.execute("SELECT * FROM sys.meta")
            out["meta_rows"] = len(rows)
        except kwp.KwpError:
            pass
        target = HOT_ID if arm.startswith("hot") else OWN_BASE + index
        debit = f"UPDATE {ACCT} SET c = c - 1 WHERE id = {target}"
        work = [f"UPDATE {WORK} SET v = v + 1 WHERE id = {(index + 1) * WORK_STRIDE + j}"
                for j in range(k)]
        first = not arm.endswith("-last") and not arm.endswith("-piped")
        piped = arm.endswith("-piped")
        pause = a.pause_ms / 1000.0

        def note(err):
            cls = classify(err.message)
            out["refused"][cls] = out["refused"].get(cls, 0) + 1
            sh = shape(err.message)
            out["shapes"][sh] = out["shapes"].get(sh, 0) + 1

        while time.time() < t_start:
            time.sleep(0.001)
        while time.time() < t_end:
            t0 = time.perf_counter()
            conn.begin()
            failed = False
            try:
                if first:
                    d0 = time.perf_counter()
                    exec_tag(conn, debit)
                    out["debit"].append(time.perf_counter() - d0)
                    if pause:
                        time.sleep(pause)
                for w in work:
                    exec_tag(conn, w)
                if not first and not piped:
                    d0 = time.perf_counter()
                    exec_tag(conn, debit)
                    out["debit"].append(time.perf_counter() - d0)
            except kwp.KwpError as e:
                note(e)
                failed = True
            if failed:
                try:
                    conn.rollback()
                except kwp.KwpError:
                    pass
                continue
            c0 = time.perf_counter()
            if piped:
                stmt_error, commit_error = piped_update_commit(conn, debit)
                if stmt_error is not None or commit_error is not None:
                    note(stmt_error or commit_error)
                    if commit_error is None or stmt_error is not None:
                        try:
                            conn.rollback()
                        except kwp.KwpError:
                            pass
                    continue
            else:
                try:
                    conn.commit()
                except kwp.KwpError as e:
                    note(e)
                    continue
            now = time.perf_counter()
            out["commit"].append(now - c0)
            out["txn"].append(now - t0)
            out["committed"] += 1
        conn.close()
    except Exception as e:  # a session that died is the cell's finding
        out["fatal"] = repr(e)
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


def read_value(conn, table, col, row_id):
    fields, rows, _t, _n = conn.execute(f"SELECT {col} FROM {table} WHERE id = {row_id}")
    if not rows or rows[0][0] is None:
        return None
    return int(kwp.renderer_for(fields[0])(rows[0][0]))


def lock_wait_sample(conn, seen):
    """The sys.lock_waits rows this cell added: outcomes, units and
    waited_us. `seen` maps a core to the highest `seq` already counted and is
    advanced here. A ring of 4096 per core, so a cell that parks more than
    that on one core is under-counted: a sample, and `rows_in_ring` says how
    full the ring is."""
    try:
        fields, rows, _t, _n = conn.execute("SELECT * FROM sys.lock_waits")
    except kwp.KwpError as e:
        return {"error": e.message}
    names = [f["name"] for f in fields]
    renders = [kwp.renderer_for(f) for f in fields]
    out = {"rows_in_ring": len(rows), "by_outcome": {}, "by_unit_mode": {}}
    waited = []
    top = dict(seen)
    for row in rows:
        rec = {n: (None if raw is None else r(raw)) for n, r, raw in zip(names, renders, row)}
        core, seq = rec.get("core"), int(rec.get("seq"))
        if seq <= seen.get(core, -1):
            continue
        top[core] = max(top.get(core, -1), seq)
        out["by_outcome"][rec.get("outcome")] = out["by_outcome"].get(rec.get("outcome"), 0) + 1
        key = "%s/%s" % (rec.get("unit"), rec.get("mode"))
        out["by_unit_mode"][key] = out["by_unit_mode"].get(key, 0) + 1
        waited.append(float(rec.get("waited_us")))
    seen.clear()
    seen.update(top)
    waited.sort()
    out["new_rows"] = len(waited)
    if waited:
        out["waited_p50_us"] = nearest_rank(waited, 50)
        out["waited_p99_us"] = nearest_rank(waited, 99)
        out["waited_max_us"] = waited[-1]
    return out


def run_cell(a, admin, arm, k, rep, seen):
    before_hot = read_value(admin, ACCT, "c", HOT_ID)
    host = host_state()
    wall0 = time.time()
    t_start = wall0 + 0.6
    t_end = t_start + a.seconds
    procs, pipes = [], []
    for i in range(a.sessions):
        parent, child = mp.Pipe(duplex=False)
        p = mp.Process(target=worker, args=(i, a, arm, k, t_start, t_end, child))
        p.start()
        child.close()
        procs.append(p)
        pipes.append(parent)
    results = [pipe.recv() for pipe in pipes]
    for p in procs:
        p.join()
    after_hot = read_value(admin, ACCT, "c", HOT_ID)

    committed = sum(r["committed"] for r in results)
    refused, shapes = {}, {}
    for r in results:
        for c, n in r["refused"].items():
            refused[c] = refused.get(c, 0) + n
        for s, n in r["shapes"].items():
            shapes[s] = shapes.get(s, 0) + n
    cell = {
        "arm": arm, "k": k, "rep": rep, "sessions": a.sessions, "seconds": a.seconds,
        "pause_ms": a.pause_ms, "label": a.label,
        "committed": committed, "tps": round(committed / a.seconds, 1),
        "refused": refused,
        "refusal_shapes": dict(sorted(shapes.items(), key=lambda kv: -kv[1])[:4]),
        "txn": pcts([x for r in results for x in r["txn"]]),
        "debit": pcts([x for r in results for x in r["debit"]]),
        "commit": pcts([x for r in results for x in r["commit"]]),
        "per_session_committed": [r["committed"] for r in results],
        "fatal": [r["fatal"] for r in results if r["fatal"]],
        "host": host,
        "lock_waits": lock_wait_sample(admin, seen),
    }
    if arm.startswith("hot"):
        cell["oracle"] = {"hot_before": before_hot, "hot_after": after_hot,
                          "fell_by": before_hot - after_hot, "committed": committed,
                          "ok": before_hot - after_hot == committed}
    return cell


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


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True, help="the server's KWP/1 port, chosen")
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
        # Interleaved: each repetition walks the arms from a different start,
        # and odd repetitions backwards, so no arm always follows the same one.
        order = arms[rep % len(arms):] + arms[:rep % len(arms)]
        if rep % 2:
            order = order[::-1]
        for k in ks:
            for arm in order:
                beat(f"{a.label} rep {rep} k {k} {arm} start")
                cell = run_cell(a, admin, arm, k, rep, seen)
                cells.append(cell)
                beat(f"{a.label} rep {rep} k {k} {arm} tps {cell['tps']} refused {cell['refused']}")
                print(json.dumps({x: cell[x] for x in ("label", "arm", "k", "rep", "tps",
                                                       "refused")}), flush=True)
                with open(a.json, "w") as f:
                    json.dump({"args": vars(a), "cells": cells}, f, indent=1)
    admin.close()
    beat(f"{a.label} driver finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
