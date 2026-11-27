#!/usr/bin/env python3
"""EE-S0's premise: does a wait for a transaction's decide that has no slot
behind it cost the poll's interval, and does a wait on one hot row end at the
fault net? (`instructions/v3.2.0/workorder-ee-decide-slot.md` §4 EE-Q1, §5.)

---- Cell `wake` ------------------------------------------------------------

Two KWP sessions on different cores. The holder opens a transaction and
writes what the waiter will meet; the waiter's autocommit statement is sent
from a thread and meets it; after `--hold-ms` the holder decides. The lag is
the waiter's reply time less the moment the holder's decide was sent, so it
carries the decide itself, the wake and the re-run. Arms, interleaved round
by round:

- `slot` (the control): both `UPDATE` one row; the waiter parks on the
  tuple unit's slot, which the holder's release flips and kicks
  (`CommandDispatcher::AwaitWriteBlock`). The holder commits.
- `assertion`: a group at its bound less one; the holder's `INSERT` holds
  the group's last unit, the waiter's `INSERT` into the group is refused by
  that reservation and waits for the reserver's decide
  (`AdmitInsert`'s `reserver`, `NoteBlockingWriter`) with no slot - the
  `IsInFlight` poll. The holder rolls back, so the waiter is admitted.
- `purge`: the holder deletes a row and holds the delete; the waiter's
  `PURGE` of that key waits for the deleter's decide
  (`DispatchOutcome::PurgeWait`, polled). The holder commits.

A wait that is not woken ends when the waiter's reactor next polls: at its
idle block's ceiling (`max_idle_block_ms`, 10 ms) when the core is idle.

---- Cell `hot` -------------------------------------------------------------

`--sessions` sessions, each `BEGIN; UPDATE hot row; <think>; COMMIT` for
`--seconds`, the think inside the transaction `--think-ms`. Counted: commits,
refusals by text, and `sys.lock_waits`' outcomes - a `fault_net` row is a
waiter that lost the unit at every flip for the net's length (`TryAcquire`
does not queue).

Run from the repository root; `--binary` is a release `kds_server`.
"""

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ckdbs_cli import ServerConnection  # noqa: E402
import sys_meta  # noqa: E402


class Server:
    def __init__(self, binary, workdir, port, cores, cpus, drain_us=None):
        self.dir = workdir
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port = port
        conf = os.path.join(self.dir, "ee.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/ee.db\nport = {port}\ncores = {cores}\n"
                    f"durability = relaxed\nbuffer_pool_frames = 65536\n"
                    f"log_file = ee.log\nlog_dir = {self.dir}\nlog_level = warn\n"
                    f"reactor_cpus = {','.join(str(c) for c in cpus)}\n")
            if drain_us is not None:
                f.write(f"wal_drain_interval_us = {drain_us}\n")
        self.err = open(os.path.join(self.dir, "ee.stderr"), "w")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                ServerConnection("127.0.0.1", port).close()
                return
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server did not listen on {port}")
                time.sleep(0.1)

    def conn(self):
        return ServerConnection("127.0.0.1", self.port)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def must(conn, stmt, prefix=""):
    reply = conn.send_command(stmt)
    if reply.startswith("ERR") or (prefix and not reply.startswith(prefix)):
        raise SystemExit(f"{stmt[:100]!r} answered {reply[:200]!r}")
    return reply


def core_of(conn):
    return sys_meta.from_text(conn.send_command("SHOW META")).get("core")


def apart(server, tries=32):
    """Two sessions on different cores, as the placer puts contenders."""
    first = server.conn()
    c0 = core_of(first)
    for _ in range(tries):
        second = server.conn()
        if core_of(second) != c0:
            return first, second
        second.close()
    raise SystemExit("no second core reached")


def pct(xs, p):
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def summary(xs):
    return {"n": len(xs), "p0": min(xs), "p25": pct(xs, 25), "p50": pct(xs, 50),
            "p75": pct(xs, 75), "p95": pct(xs, 95), "p100": max(xs),
            "mean": statistics.fmean(xs)}


def wake_cell(server, rounds, hold_ms, cap):
    holder, waiter = apart(server)
    must(holder, "CREATE TABLE ee_row (id int64, v int64) BTREE")
    must(holder, "INSERT INTO ee_row VALUES (1, 0)")
    must(holder, "CREATE TABLE ee_book (id int64, flight int64) BTREE")
    must(holder, f"CREATE ASSERTION ee_cap ON ee_book GROUP BY (flight) CHECK COUNT(*) <= {cap}")
    must(holder, "CREATE TABLE ee_purge (id int64, v int64) BTREE")
    next_id = [1]

    def book(conn, flight, n):
        vals = []
        for _ in range(n):
            vals.append(f"({next_id[0]}, {flight})")
            next_id[0] += 1
        must(conn, "INSERT INTO ee_book VALUES " + ",".join(vals))

    must(holder, "INSERT INTO ee_purge VALUES " +
         ",".join(f"({k}, 0)" for k in range(1, rounds + 1)))
    lags = {"slot": [], "assertion": [], "purge": []}
    replies = {"slot": {}, "assertion": {}, "purge": {}}
    cores = []
    for r in range(rounds):
        flight = 1000 + r
        book(holder, flight, cap - 1)
        order = ["slot", "assertion", "purge"] if r % 2 == 0 else ["purge", "assertion", "slot"]
        for arm in order:
            must(holder, "BEGIN", "BEGIN")
            if arm == "slot":
                must(holder, "UPDATE ee_row SET v = v + 1 WHERE id = 1")
                stmt, decide = "UPDATE ee_row SET v = v + 1 WHERE id = 1", "COMMIT"
            elif arm == "assertion":
                must(holder, f"INSERT INTO ee_book VALUES ({next_id[0]}, {flight})")
                next_id[0] += 1
                stmt, decide = f"INSERT INTO ee_book VALUES ({next_id[0]}, {flight})", "ROLLBACK"
                next_id[0] += 1
            else:
                must(holder, f"DELETE FROM ee_purge WHERE id = {r + 1}")
                stmt, decide = f"PURGE FROM ee_purge WHERE id = {r + 1}", "COMMIT"
            cores.append((arm, core_of(holder), core_of(waiter)))
            out = {}

            def run():
                out["reply"] = waiter.send_command(stmt)
                out["t"] = time.perf_counter()

            th = threading.Thread(target=run)
            th.start()
            time.sleep(hold_ms / 1000)
            t_decide = time.perf_counter()
            must(holder, decide)
            th.join(5)
            if th.is_alive():
                raise SystemExit(f"{arm}: the waiter did not answer within 5 s")
            lags[arm].append((out["t"] - t_decide) * 1e6)
            key = out["reply"].split(" ")[0]
            replies[arm][key] = replies[arm].get(key, 0) + 1
    holder.close()
    waiter.close()
    return {"lag_us": {a: summary(v) for a, v in lags.items()},
            "raw_us": lags, "replies": replies,
            "cores_distinct": sum(1 for _, h, w in cores if h != w), "asks": len(cores)}


def hot_cell(server, sessions, seconds, think_ms):
    setup = server.conn()
    must(setup, "CREATE TABLE ee_hot (id int64, v int64) BTREE")
    must(setup, "INSERT INTO ee_hot VALUES (1, 0)")
    before = sys_meta.view_rows(setup.send_command("SELECT * FROM sys.lock_waits"))
    stop_at = time.perf_counter() + seconds
    counts = [dict() for _ in range(sessions)]
    durations = [[] for _ in range(sessions)]

    def worker(i):
        conn = server.conn()
        c = counts[i]
        while time.perf_counter() < stop_at:
            t0 = time.perf_counter()
            conn.send_command("BEGIN")
            reply = conn.send_command("UPDATE ee_hot SET v = v + 1 WHERE id = 1")
            if reply.startswith("ERR"):
                key = "refused: " + reply[:60]
                c[key] = c.get(key, 0) + 1
                conn.send_command("ROLLBACK")
                continue
            if think_ms:
                time.sleep(think_ms / 1000)
            done = conn.send_command("COMMIT")
            key = "commit" if done.startswith("COMMIT") else "commit refused: " + done[:60]
            c[key] = c.get(key, 0) + 1
            durations[i].append((time.perf_counter() - t0) * 1e6)
        conn.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(sessions)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    rows = sys_meta.view_rows(setup.send_command("SELECT * FROM sys.lock_waits"))
    seen = max((int(r["seq"]) for r in before), default=0)
    fresh = [r for r in rows if int(r["seq"]) > seen]
    outcomes, waited = {}, []
    for row in fresh:
        outcomes[row.get("outcome")] = outcomes.get(row.get("outcome"), 0) + 1
        if row.get("waited_us") not in (None, "NULL"):
            waited.append(float(row["waited_us"]))
    total = {}
    for c in counts:
        for k, v in c.items():
            total[k] = total.get(k, 0) + v
    m = sys_meta.from_text(setup.send_command("SHOW META"))
    rel = [r for r in sys_meta.view_rows(setup.send_command(
        "SELECT * FROM sys.relation_stats")) if r.get("name", r.get("relation")) == "ee_hot"]
    setup.close()
    flat = [d for ds in durations for d in ds]
    return {"sessions": sessions, "seconds": seconds, "think_ms": think_ms,
            "outcomes": total, "lock_waits": outcomes,
            "lock_waits_rows": len(fresh),
            "waited_us": summary(waited) if waited else None,
            "txn_us": summary(flat) if flat else None,
            "meta_wait_aborts": m.get("txn.wait_aborts", m.get("wait_aborts")),
            "relation_locks": {k: v for r in rel for k, v in r.items() if k.startswith("lock_")}}


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--binary", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--port", type=int, default=15991)
    p.add_argument("--cell", choices=["wake", "hot"], required=True)
    p.add_argument("--cores", type=int, default=2)
    p.add_argument("--cpus", default="0,1")
    p.add_argument("--rounds", type=int, default=40)
    p.add_argument("--hold-ms", type=float, default=20.0)
    p.add_argument("--cap", type=int, default=4)
    p.add_argument("--sessions", type=int, default=8)
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--think-ms", type=float, default=0.0)
    p.add_argument("--drain-us", type=int, default=None,
                   help="wal_drain_interval_us; unset keeps the default (1000)")
    p.add_argument("--json", default=None)
    args = p.parse_args()
    server = Server(args.binary, args.workdir, args.port, args.cores,
                    [int(c) for c in args.cpus.split(",")], args.drain_us)
    try:
        if args.cell == "wake":
            result = wake_cell(server, args.rounds, args.hold_ms, args.cap)
        else:
            result = hot_cell(server, args.sessions, args.seconds, args.think_ms)
    finally:
        server.stop()
    result["config"] = vars(args)
    text = json.dumps(result, indent=1, default=str)
    if args.json:
        with open(args.json, "w") as f:
            f.write(text)
    shown = {k: v for k, v in result.items() if k != "raw_us"}
    print(json.dumps(shown, indent=1, default=str))


if __name__ == "__main__":
    main()
