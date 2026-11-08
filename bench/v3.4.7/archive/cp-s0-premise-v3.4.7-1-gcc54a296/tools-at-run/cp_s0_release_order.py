#!/usr/bin/env python3
"""CP-S0 M6, the scripted half: does a `group` commit release its row `X`
before its sync lands, with only the acknowledgement waiting?

The cell (`instructions/v3.2.0/workorder-cp-lock-model-exploration.md`, the
CP-S0 row of section 5, "M6"; section 1.1's last bullet; CP-Q5 (a)) is run
against a real `kds_server`: two sessions on the newline debug surface, the
first committing an `UPDATE` of one row, the second writing the same row the
instant the first's `COMMIT` goes out. It corroborates the two-core rig cell
(`lock_release_before_sync_rig_test.cpp`) and, if it holds, corrects
`docs/inflight/known-gaps.md:1417-1423` ("keeps the row's `X` until its
commit is durable"). Every path:line below was read at cc54a296 (build
inputs 6c4b9a6d, v3.4.7).

Design
------
Session A: `SET DURABILITY <arm>`, then per iteration `BEGIN` /
`UPDATE t SET v = v + 1 WHERE id = 1` / `COMMIT`, with `time.perf_counter_ns()`
taken right after `COMMIT`'s `sendall` returns and right after its reply line
is read. Session B, on another thread, is pre-positioned in `BEGIN`; a
`threading.Event` set right after A's `COMMIT` send releases it, it sends
`UPDATE t SET v = v + 1 WHERE id = 1` and stamps its reply; after A's reply
B sends `ROLLBACK`. ~300 iterations after a short warm-up. A third connection
creates `cp_release_<suffix> (id int64, v int64) BTREE`, inserts row 1, reads
the `sys` views at the end and drops the relation (unless `--keep`).

What the engine does, as read
-----------------------------
* B's `UPDATE` asks the row's tuple `X` (`src/server/command_dispatcher.cpp:10147`,
  `BorrowChain(scope, LockKey::Tuple(oid, id))`) and meets A's undecided
  version: the statement waits instead of refusing
  (`CheckWriteConflictBlocking`, `command_dispatcher.cpp:11616-11640`), parked
  on A's slot (`DispatchAsync`, `:441-450`, `WaitScope(kAppLockWait)`), and
  re-runs from the top at A's release (AX-S2b; `:467-469` "running it
  again"). That wait is one `sys.lock_waits` row (`unit` tuple, `mode` X,
  `outcome` granted) and `app.lock_wait` time in `sys.wait_stats` /
  `sys.session_waits` - **when B's `UPDATE` reaches the row before A's
  release**. B is triggered after A's `COMMIT` has left the socket, and the
  Python wake plus the hop is of the order of the server's append-and-release,
  so some iterations arrive after the release, take the `X` at once and park
  nowhere; those still show the claim (B's write completed while A's ack was
  pending), and the count of `granted` tuple rows on the relation says how
  many parked.
* A's `COMMIT` under `group`: `WalManager::Commit` appends the commit record
  (`src/wal/manager.cpp:425`) and the `kGroup` arm returns **without a sync**
  (`:441-445`; the `kStrict` arm syncs inline at `:431-437`).
  `TransactionManager::Commit` (`src/txn/manager.cpp:366`) then calls
  `FinishCommitBody` (`:398`), which publishes the commit into the instance
  window (`:306`), lifts the marker (`:316`), retires the in-flight entry
  (`:325`), moves the schema word (`:329`) and **releases every borrow**
  (`:336`). Only the acknowledgement waits: `CommitLocal` stages the LSN
  (`command_dispatcher.cpp:10858-10861`; the autocommit twin at
  `:11771-11774`) and `DispatchAsync` parks on `IsDurable` (`:890-914`),
  inside `WaitScope(kCommitWalFlush, lsn, durability)` (`:911-913`) - the
  `commit.wal_flush` event, p2 = 2 for `group`
  (`docs/spec/wait-events.md:39-50`, `include/kds/stats/wait_event.hpp:121`).
  The sync itself is the WAL writer's, driven by the drain cadence
  (`kds.conf.sample:249`, `wal_drain_interval_us = 1000`) and the device's
  fsync; the drain's tick plus the fsync is the acknowledgement's latency.
* B's re-run sees A's version as committed although it is not yet durable:
  `docs/spec/txn.md:434-438` ("a reader sees a commit as soon as the record
  is appended, which under `group` is before the platter has it");
  `PublishCommit` raises the ceiling at the publish
  (`src/txn/instance_visibility.cpp:313-314`), `ReadView::Visible` compares
  the window's commit LSN against the snapshot
  (`include/kds/txn/read_view.hpp:108-115`), and `CheckWriteConflict` passes
  a visible committed writer (`src/txn/manager.cpp:188-196`). B is in an
  explicit transaction, but `READ COMMITTED` is the default
  (`txn.md:113`, `:122`; `include/kds/server/session.hpp:375`) and mints a
  snapshot per statement, so B's `UPDATE` minted after the kick covers A's
  commit. (Under `REPEATABLE READ` B's `BEGIN`-time snapshot would not, and
  the `UPDATE` would be refused as a write conflict - the script checks B's
  `BEGIN` reply names no `REPEATABLE` level.)
* The control arm, `--arm strict`: at `cores > 1` the text session's
  statement is a `DispatchAsync` task (`src/server/tcp_server.cpp:732-736`)
  under `MayParkScope` (`command_dispatcher.cpp:783-787`), the writer has a
  wake registry (`include/kds/wal/writer.hpp:150-152`,
  `include/kds/wal/manager.hpp:335-337`), so `DefersCommit`
  (`command_dispatcher.cpp:1141-1145`) is true: `CommitDeferred`
  (`src/txn/manager.cpp:338-359`) stages the record and **keeps the borrows**,
  and `FinishDeferredCommits` (`command_dispatcher.cpp:1152-1167`) runs
  `FinishDeferredCommit` -> `FinishCommitBody` and `txn_->Release` only once
  `IsDurable` - after the sync (BA-S7). At `cores = 1` `DefersCommit` is
  false and `kStrict` syncs inline before `FinishCommitBody`
  (`wal/manager.cpp:431-437`, `txn/manager.cpp:398`): the release is after
  the sync either way. So under `strict` B's row `X` is granted only when
  the sync has landed.

Interpretation (the script's reading, printed as `verdict`)
-----------------------------------------------------------
Let margin = A_commit_reply - B_update_reply (positive: B answered first)
and ack = A_commit_reply - A_commit_send (the acknowledgement's latency,
which under `group` is the drain tick plus the fsync).

* `group`: if B's `UPDATE` answers before A's `COMMIT` in most iterations
  (fraction >= 0.9) **and** the median margin is of the order of the
  acknowledgement's latency (margin p50 >= 0.5 * ack p50), the release
  precedes the durability point: B's write - which needs A's tuple `X`
  released and A's version visible as committed - completed while A's
  record was still waiting for its sync. B's own `UPDATE` latency is then a
  small fraction of ack (the hop to the release kick, not a sync).
* `strict` (control): the release waits for the sync, so B's reply cannot
  precede A's by more than the hop between A's release and A's reply being
  written - tens of microseconds, not a sync. The control holds when the
  median margin is small against ack (margin p50 <= max(200 us,
  0.2 * ack p50)) and B's `UPDATE` latency is of ack's order (B waited the
  whole sync). The sign of a single iteration is not the test: under
  `strict` B is kicked at `FinishCommitBody`'s release and A's reply is
  written a few microseconds later on the same core, so B may answer first
  by that hop; the magnitude is what separates the arms.
* Both arms together make the cell self-proving: the same script and the
  same statements, differing only in A's session rung (`SET DURABILITY`,
  `docs/spec/protocol.md:127-128`, `command_dispatcher.cpp:10992-10999`),
  give a sync-sized margin under `group` and a hop-sized one under `strict`.

Also reported: `sys.lock_waits` rows on the relation (unit, mode, outcome,
`waited_us` p50/p90/p99; `docs/spec/sys-views.md:183-194`,
`src/server/sys_views.cpp:924-934`, names at `src/txn/lock_table.cpp:35-57`),
`sys.wait_stats` per-core and total rows for `commit.wal_flush`,
`commit.ceiling_wait`, `app.lock_wait`, `app.lock_conflict`
(`sys-views.md:233-236`, `sys_views.cpp:1058-1063`), `sys.wait_histogram`
buckets for the first and third, `sys.session_waits` (`session_id`, `event`,
`count`, `total_us`; `sys-views.md:240-242`, `sys_views.cpp:1118`) - A is the
session with the largest `commit.wal_flush` count, B the one with the
largest `app.lock_wait` count - and `sys.meta`'s `lock_waits_*` counters.
The relation's final `v` must equal warm-up + iterations: A's commits all
landed and B's writes were all rolled back.

Protocol facts the script relies on
-----------------------------------
* The newline debug surface is one line in, one line out
  (`tools/ckdbs_cli.py:2-5`, `TextConnection` at `:129-158`); a `SELECT`'s
  rows come back in that one line with the two-character `\\n` escape, which
  `tools/sys_meta.py:62-68` (`view_rows`) decodes. The port is
  `debug_text_port` (`kds.conf.sample:314`, 0 = closed; loopback only), bound
  once with `reuse_port = false` (`src/server/expeditor.cpp:1778-1786`,
  `include/kds/server/tcp_server.hpp:109`) and attached in
  `Expeditor::Start` (`expeditor.cpp:1591`, `:2044-2045`): **every text
  session runs on core 0**, so A and B share a core and B's kick is a
  same-core kick. `--protocol kwp` instead opens `ckdbs_cli.ServerConnection`
  sessions on the KWP port, which every core accepts (SO_REUSEPORT,
  `src/server/core_runtime.cpp:514`); the kernel picks each session's core,
  the script cannot, and `send_command` is one call, so A's send stamp is
  taken before the call rather than after the socket write. `sys.wait_stats`'
  per-core rows say which cores A (`commit.wal_flush`) and B
  (`app.lock_wait`) ran on.
* Replies: `SET durability=<class>` (`command_dispatcher.cpp:10998`; refused
  inside a transaction, `:10994-10996`), `BEGIN trx_id=.. isolation=..
  durability=<effective>` (`:10759-10762`, the third rung of
  `protocol.md:128`'s chain is `BEGIN ... DURABILITY <class>`, `:10696-10740`),
  `UPDATED <n>` (`:10574`, `:10680`, through `WroteRows`, `:6137`, which
  keeps the text), `COMMIT trx_id=..` (`:10863`), `ROLLBACK trx_id=..`
  (`:10908`), `CREATED oid=<n>` (`:2389`), `INSERTED oid=.. rows=..`
  (`:6786`); every refusal starts with `ERR`. A `SELECT`'s text reply is the
  header line, then `\\n` and one comma row per match, no tag
  (`include/kds/server/result_sink.hpp:109-160`, `TextResultSink`), NULL
  rendered `NULL` (`src/exec/row_codec.cpp:1304`).
* **Over KWP the transaction replies are the client's renderings, not the
  server's lines**: `ckdbs_cli.ServerConnection.send_command` answers
  `BEGIN` with `"BEGIN"` (or `"BEGIN durability=relaxed"`), `COMMIT` with
  `"COMMIT"` and `ROLLBACK` with `"ROLLBACK"` (`tools/ckdbs_cli.py:198-206`;
  `C_TXN_BEGIN`'s durability byte is 0 = session default, `kwp.py:593`). So
  under `--protocol kwp` no `BEGIN` reply carries `trx_id=`, `isolation=` or
  `durability=`, the per-`BEGIN` rung check and the `REPEATABLE` check find
  nothing to read and pass, and the `SET durability=` reply is the one
  confirmation of A's rung. Statements (`SET DURABILITY`, `UPDATE`, `CREATE`,
  `INSERT`, `SELECT`) come back in the newline protocol's shape (`:160-176`).
* The server's configured class is the first rung (`kds.conf.sample:235`,
  `durability = group` is the sample's default); the script pins A's rung
  with `SET DURABILITY <arm>` whatever the server's is, and checks every
  `BEGIN` reply's `durability=` token.

Usage
-----
    cp_s0_release_order.py --port <debug_text_port> --arm group --iterations 300 --json out.json
    cp_s0_release_order.py --port <debug_text_port> --arm strict --iterations 300 --json out.json
    cp_s0_release_order.py --protocol kwp --port <port> --arm group ...

Exit status 0 when the run completed (whatever the verdict), 1 on a protocol
or setup failure. Nothing here is a throughput number: it is an ordering and
a margin, measured through Python's socket cost (tens of microseconds), which
is the floor below which a margin is noise.
"""

import argparse
import datetime as _dt
import json
import os
import socket
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import bench_common  # noqa: E402  (nearest_rank, tools/bench_common.py:35-42)
import sys_meta  # noqa: E402  (view_rows, tools/sys_meta.py:62-68)

UPDATE_SQL = "UPDATE {t} SET v = v + 1 WHERE id = 1"
EVENTS = ("commit.wal_flush", "commit.ceiling_wait", "app.lock_wait", "app.lock_conflict")
HIST_EVENTS = ("commit.wal_flush", "app.lock_wait")


class ProtocolError(RuntimeError):
    pass


class TextLine:
    """One connection on the newline debug surface: `send` returns the stamp
    taken right after `sendall`, `recv` returns the reply line and the stamp
    taken right after its newline arrived (same framing as
    `ckdbs_cli.TextConnection`, `tools/ckdbs_cli.py:129-158`)."""

    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\n")
        return time.perf_counter_ns()

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", errors="replace"), time.perf_counter_ns()

    def command(self, line):
        t0 = self.send(line)
        reply, t1 = self.recv()
        return reply, t0, t1

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class KwpLine:
    """One KWP/1 session through `ckdbs_cli.ServerConnection`, whose
    `send_command` is one call: the send stamp is taken before it, the reply
    stamp after it. `BEGIN`/`COMMIT`/`ROLLBACK` go out as transaction frames
    and come back as the client's words `BEGIN` / `COMMIT` / `ROLLBACK`
    without the server's `trx_id=`/`durability=` tokens
    (`tools/ckdbs_cli.py:196-206`); `SET DURABILITY` goes as a statement and
    its `SET durability=<class>` reply is the rung's only confirmation."""

    def __init__(self, host, port, timeout):
        import ckdbs_cli
        self.conn = ckdbs_cli.ServerConnection(host, port, timeout=timeout)
        self.pending = None

    def send(self, line):
        self.pending = line
        return time.perf_counter_ns()

    def recv(self):
        line, self.pending = self.pending, None
        reply = self.conn.send_command(line)
        return reply, time.perf_counter_ns()

    def command(self, line):
        t0 = self.send(line)
        reply, t1 = self.recv()
        return reply, t0, t1

    def close(self):
        try:
            self.conn.close()
        except Exception:  # noqa: BLE001  - best effort at teardown
            pass


def open_session(args):
    cls = TextLine if args.protocol == "text" else KwpLine
    return cls(args.host, args.port, args.timeout)


def expect(reply, prefix, what):
    if not reply.startswith(prefix):
        raise ProtocolError(f"{what}: expected a reply starting '{prefix}', got: {reply}")
    return reply


def token(reply, key):
    """The value of `key=` in a reply line, or None."""
    for part in reply.split():
        if part.startswith(key + "="):
            return part[len(key) + 1:]
    return None


class Follower(threading.Thread):
    """Session B's writer: parked on `go`, it sends one statement and stamps
    the reply, then signals `done`. One thread for the whole run."""

    def __init__(self, conn, statement):
        super().__init__(name="session-B", daemon=True)
        self.conn = conn
        self.statement = statement
        self.go = threading.Event()
        self.done = threading.Event()
        self.stop = False
        self.result = None

    def run(self):
        while True:
            self.go.wait()
            self.go.clear()
            if self.stop:
                return
            try:
                t_send = self.conn.send(self.statement)
                reply, t_reply = self.conn.recv()
                self.result = {"reply": reply, "send": t_send, "reply_at": t_reply, "error": None}
            except Exception as e:  # noqa: BLE001  - reported, ends the run
                self.result = {"reply": None, "send": 0, "reply_at": 0, "error": repr(e)}
            self.done.set()

    def fire(self):
        """Arms one shot: clears the last result. The trigger is `go.set()`,
        which the caller fires itself right after A's `COMMIT` has left the
        socket - never here, or B would send before A's `COMMIT` and the
        trigger hop (`b_send_after_a_send_us`) would read negative."""
        self.result = None
        self.done.clear()

    def finish(self):
        self.stop = True
        self.go.set()


def dist_us(values_ns):
    if not values_ns:
        return {"n": 0}
    us = sorted(v / 1000.0 for v in values_ns)
    return {
        "n": len(us),
        "min": round(us[0], 1),
        "p50": round(bench_common.nearest_rank(us, 50), 1),
        "p90": round(bench_common.nearest_rank(us, 90), 1),
        "p99": round(bench_common.nearest_rank(us, 99), 1),
        "max": round(us[-1], 1),
        "mean": round(statistics.fmean(us), 1),
    }


def dist_plain_us(values_us):
    return dist_us([v * 1000.0 for v in values_us])


def num(text):
    if text is None or text == "NULL":
        return None
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def read_view(conn, sql):
    reply = conn.command(sql)[0]
    if reply.startswith("ERR"):
        return None, reply
    try:
        return sys_meta.view_rows(reply), None
    except Exception as e:  # noqa: BLE001  - a view that does not parse is reported, not fatal
        return None, f"unparsed reply ({e!r}): {reply[:200]}"


def read_views(conn, rel_oid):
    out = {"errors": {}}

    rows, err = read_view(conn, "SELECT * FROM sys.lock_waits")
    if err:
        out["errors"]["lock_waits"] = err
    else:
        on_rel = [r for r in rows if num(r.get("rel_oid")) == rel_oid]
        groups = {}
        for r in on_rel:
            k = (r.get("unit"), r.get("mode"), r.get("outcome"))
            groups.setdefault(k, []).append(num(r.get("waited_us")) or 0)
        out["lock_waits_on_relation"] = {
            "rows": len(on_rel),
            "by_unit_mode_outcome": [
                {"unit": k[0], "mode": k[1], "outcome": k[2], "count": len(v),
                 "waited_us": dist_plain_us(v)}
                for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))
            ],
            "raw": on_rel,
        }
        all_groups = {}
        for r in rows:
            k = (r.get("unit"), r.get("mode"), r.get("outcome"))
            all_groups[k] = all_groups.get(k, 0) + 1
        out["lock_waits_all"] = {
            "rows": len(rows),
            "counts": [{"unit": k[0], "mode": k[1], "outcome": k[2], "count": c}
                       for k, c in sorted(all_groups.items(), key=lambda kv: -kv[1])],
        }

    rows, err = read_view(conn, "SELECT * FROM sys.wait_stats")
    if err:
        out["errors"]["wait_stats"] = err
    else:
        picked = [r for r in rows if r.get("event") in EVENTS]
        out["wait_stats"] = {
            "total": [r for r in picked if r.get("core") == "NULL"],
            "per_core": [r for r in picked if r.get("core") != "NULL" and (num(r.get("count")) or 0) > 0],
        }
        # The DB-time denominator, from the NULL-core total rows (sys-views.md:247-250).
        db_time = 0
        for r in rows:
            if r.get("core") != "NULL":
                continue
            ev = r.get("event", "")
            if ev.startswith("idle."):
                continue
            if ev == "cpu.on_cpu" or ev == "sched.run_queue" or (num(r.get("count")) or 0) > 0:
                db_time += num(r.get("total_us")) or 0
        out["wait_stats"]["db_time_us_total_rows"] = db_time

    rows, err = read_view(conn, "SELECT * FROM sys.wait_histogram")
    if err:
        out["errors"]["wait_histogram"] = err
    else:
        out["wait_histogram"] = [r for r in rows
                                 if r.get("event") in HIST_EVENTS and (num(r.get("count")) or 0) > 0]

    rows, err = read_view(conn, "SELECT * FROM sys.session_waits")
    if err:
        out["errors"]["session_waits"] = err
    else:
        picked = [r for r in rows if r.get("event") in EVENTS and (num(r.get("count")) or 0) > 0]
        out["session_waits"] = picked

        def top(event):
            best = None
            for r in picked:
                if r.get("event") == event:
                    if best is None or (num(r.get("count")) or 0) > (num(best.get("count")) or 0):
                        best = r
            return best
        out["session_a_by_wal_flush"] = top("commit.wal_flush")
        out["session_b_by_lock_wait"] = top("app.lock_wait")

    rows, err = read_view(conn, "SELECT * FROM sys.meta")
    if err:
        out["errors"]["meta"] = err
    else:
        fields = {r.get("field"): r.get("value") for r in rows if "field" in r}
        out["meta"] = {k: v for k, v in fields.items()
                       if k and (k.startswith("lock_waits_") or k in ("core", "cores", "version"))}
    return out


def utc_now():
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True,
                    help="debug_text_port (--protocol text, the default) or the KWP port (--protocol kwp)")
    ap.add_argument("--protocol", choices=["text", "kwp"], default="text")
    ap.add_argument("--iterations", type=int, default=300)
    ap.add_argument("--warmup", type=int, default=5, help="unrecorded iterations first")
    ap.add_argument("--arm", choices=["group", "strict"], required=True,
                    help="session A's durability rung, set with SET DURABILITY")
    ap.add_argument("--json", default=None, help="write the full result here")
    ap.add_argument("--timeout", type=float, default=10.0, help="socket timeout in seconds")
    ap.add_argument("--table-suffix", default=None,
                    help="cp_release_<suffix>; default: the arm and the pid")
    ap.add_argument("--keep", action="store_true", help="do not DROP the relation at the end")
    args = ap.parse_args()

    suffix = args.table_suffix or f"{args.arm}_{os.getpid()}"
    table = f"cp_release_{suffix}"
    update_sql = UPDATE_SQL.format(t=table)
    result = {
        "meta": {
            "cell": "CP-S0 M6 (scripted half)",
            "started_utc": utc_now(),
            "host": args.host, "port": args.port, "protocol": args.protocol,
            "arm": args.arm, "iterations": args.iterations, "warmup": args.warmup,
            "table": table, "statement_b": update_sql,
            "python": sys.version.split()[0],
            "b_trigger": "threading.Event set once per iteration, right after A's COMMIT sendall "
                         "returned; B's UPDATE is sent on that wake",
            "note_text_port_core": ("every text session runs on core 0 (expeditor.cpp:1778-1786, "
                                    ":2044-2045)" if args.protocol == "text" else
                                    "KWP sessions land on cores by SO_REUSEPORT (core_runtime.cpp:514)"),
        }
    }

    setup = a_conn = b_conn = None
    follower = None
    rc = 0
    try:
        setup = open_session(args)
        created = expect(setup.command(f"CREATE TABLE {table} (id int64, v int64) BTREE")[0],
                         "CREATED", "CREATE TABLE")
        rel_oid = num(token(created, "oid"))
        if rel_oid is None:
            rows, err = read_view(setup, "SELECT * FROM sys.tables")
            if rows:
                for r in rows:
                    if r.get("name") == table:
                        rel_oid = num(r.get("oid"))
        result["meta"]["rel_oid"] = rel_oid
        expect(setup.command(f"INSERT INTO {table} VALUES (1, 0)")[0], "INSERTED", "INSERT")

        a_conn = open_session(args)
        b_conn = open_session(args)
        set_reply = expect(a_conn.command(f"SET DURABILITY {args.arm}")[0], "SET durability=",
                           "SET DURABILITY on A")
        if token(set_reply, "durability") != args.arm:
            raise ProtocolError(f"A's rung is not {args.arm}: {set_reply}")
        result["meta"]["a_set_durability_reply"] = set_reply

        follower = Follower(b_conn, update_sql)
        follower.start()

        records = []
        a_errors = b_errors = 0
        first_a_error = first_b_error = None
        total = args.warmup + args.iterations
        for i in range(total):
            b_begin = expect(b_conn.command("BEGIN")[0], "BEGIN", "B's BEGIN")
            if "REPEATABLE" in b_begin.upper():
                raise ProtocolError(f"B is not READ COMMITTED: {b_begin}")
            a_begin = expect(a_conn.command("BEGIN")[0], "BEGIN", "A's BEGIN")
            a_rung = token(a_begin, "durability")
            if a_rung is not None and a_rung != args.arm:
                raise ProtocolError(f"A's BEGIN names durability={a_rung}, wanted {args.arm}: {a_begin}")
            expect(a_conn.command(update_sql)[0], "UPDATED 1", "A's UPDATE")

            follower.fire()  # armed, not triggered
            t_a_send = a_conn.send("COMMIT")
            # B's trigger, the one `go.set()` of the iteration: the instant
            # the COMMIT's bytes have left.
            follower.go.set()
            a_reply, t_a_reply = a_conn.recv()
            if not follower.done.wait(args.timeout + 5.0):
                raise ProtocolError("B's UPDATE did not answer within the timeout")
            b = follower.result
            if b["error"]:
                raise ProtocolError(f"B's UPDATE failed on the socket: {b['error']}")
            b_rollback = expect(b_conn.command("ROLLBACK")[0], "ROLLBACK", "B's ROLLBACK")

            a_ok = a_reply.startswith("COMMIT")
            b_ok = b["reply"].startswith("UPDATED 1")
            if not a_ok:
                a_errors += 1
                first_a_error = first_a_error or a_reply
            if not b_ok:
                b_errors += 1
                first_b_error = first_b_error or b["reply"]
            if i >= args.warmup:
                records.append({
                    "a_commit_send": t_a_send, "a_commit_reply": t_a_reply,
                    "b_update_send": b["send"], "b_update_reply": b["reply_at"],
                    "a_ok": a_ok, "b_ok": b_ok,
                    "a_reply": a_reply[:40], "b_reply": b["reply"][:40],
                    "b_rollback": b_rollback[:40],
                })

        follower.finish()

        # The relation's state: every A commit landed, every B write rolled back.
        rows, err = read_view(setup, f"SELECT v FROM {table} WHERE id = 1")
        final_v = num(rows[0].get("v")) if rows else None
        result["final_v"] = {"value": final_v, "expected": total, "error": err,
                             "ok": final_v == total}

        good = [r for r in records if r["a_ok"] and r["b_ok"]]
        margins = [r["a_commit_reply"] - r["b_update_reply"] for r in good]
        b_first = sum(1 for m in margins if m > 0)
        ack = [r["a_commit_reply"] - r["a_commit_send"] for r in good]
        b_lat = [r["b_update_reply"] - r["b_update_send"] for r in good]
        trigger = [r["b_update_send"] - r["a_commit_send"] for r in good]
        summary = {
            "recorded": len(records), "usable": len(good),
            "a_commit_errors": a_errors, "first_a_error": first_a_error,
            "b_update_errors": b_errors, "first_b_error": first_b_error,
            "fraction_b_reply_before_a_reply": round(b_first / len(good), 4) if good else None,
            "margin_us_a_reply_minus_b_reply": dist_us(margins),
            "a_commit_ack_latency_us": dist_us(ack),
            "b_update_latency_us": dist_us(b_lat),
            "b_send_after_a_send_us": dist_us(trigger),
        }
        if good:
            m50 = summary["margin_us_a_reply_minus_b_reply"]["p50"]
            ack50 = summary["a_commit_ack_latency_us"]["p50"]
            bl50 = summary["b_update_latency_us"]["p50"]
            summary["ratio_margin_p50_over_ack_p50"] = round(m50 / ack50, 3) if ack50 else None
            summary["ratio_b_latency_p50_over_ack_p50"] = round(bl50 / ack50, 3) if ack50 else None
            frac = summary["fraction_b_reply_before_a_reply"]
            if args.arm == "group":
                holds = frac >= 0.9 and m50 >= 0.5 * ack50
                verdict = {
                    "arm": "group",
                    "claim": "the row X is released before the commit's sync lands; only the "
                             "acknowledgement waits (section 1.1's last bullet; corrects "
                             "known-gaps.md:1417-1423)",
                    "rule": "fraction >= 0.9 and margin p50 >= 0.5 * ack p50",
                    "shown": bool(holds),
                }
            else:
                holds = m50 <= max(200.0, 0.2 * ack50) and bl50 >= 0.5 * ack50
                verdict = {
                    "arm": "strict",
                    "claim": "control: borrows are held across the sync (DefersCommit, "
                             "command_dispatcher.cpp:1141-1145; or the inline sync at "
                             "wal/manager.cpp:431-437), so B waits the whole sync",
                    "rule": "margin p50 <= max(200 us, 0.2 * ack p50) and B latency p50 >= 0.5 * ack p50",
                    "shown": bool(holds),
                }
            if a_errors or b_errors or not result["final_v"]["ok"]:
                verdict["shown"] = False
                verdict["invalidated_by"] = {"a_commit_errors": a_errors, "b_update_errors": b_errors,
                                             "final_v_ok": result["final_v"]["ok"]}
        else:
            verdict = {"arm": args.arm, "shown": False, "reason": "no usable iteration"}
        result["summary"] = summary
        result["verdict"] = verdict
        result["iterations_raw"] = records

        result["views"] = read_views(setup, rel_oid)
        if not args.keep:
            drop = setup.command(f"DROP TABLE {table}")[0]
            result["meta"]["drop_reply"] = drop[:80]
    except (ProtocolError, ConnectionError, OSError, socket.timeout) as e:
        result["failure"] = repr(e)
        rc = 1
    finally:
        if follower is not None:
            follower.finish()
        for c in (a_conn, b_conn, setup):
            if c is not None:
                c.close()
    result["meta"]["finished_utc"] = utc_now()

    # ---- report ---------------------------------------------------------------
    print(f"CP-S0 M6 scripted half - arm={args.arm} protocol={args.protocol} "
          f"{args.host}:{args.port} table={table} oid={result['meta'].get('rel_oid')}")
    if "failure" in result:
        print(f"  FAILED: {result['failure']}")
    s = result.get("summary")
    if s:
        print(f"  usable iterations      {s['usable']}/{s['recorded']}  "
              f"(A commit errors {s['a_commit_errors']}, B update errors {s['b_update_errors']})")
        print(f"  B reply before A reply {s['fraction_b_reply_before_a_reply']}")
        for key, label in (("margin_us_a_reply_minus_b_reply", "margin A_reply - B_reply us"),
                           ("a_commit_ack_latency_us", "A COMMIT ack latency us"),
                           ("b_update_latency_us", "B UPDATE latency us"),
                           ("b_send_after_a_send_us", "B send after A send us")):
            d = s[key]
            if d.get("n"):
                print(f"  {label:<28} p50 {d['p50']:>9.1f}  p90 {d['p90']:>9.1f}  p99 {d['p99']:>9.1f}  "
                      f"min {d['min']:>8.1f}  max {d['max']:>9.1f}")
        print(f"  margin p50 / ack p50   {s.get('ratio_margin_p50_over_ack_p50')}   "
              f"B latency p50 / ack p50 {s.get('ratio_b_latency_p50_over_ack_p50')}")
        fv = result.get("final_v", {})
        print(f"  final v                {fv.get('value')} (expected {fv.get('expected')}, ok={fv.get('ok')})")
        v = result.get("verdict", {})
        print(f"  verdict                shown={v.get('shown')}  [{v.get('rule', v.get('reason', ''))}]")
    views = result.get("views")
    if views:
        lw = views.get("lock_waits_on_relation")
        if lw:
            print(f"  sys.lock_waits on the relation: {lw['rows']} rows")
            for g in lw["by_unit_mode_outcome"]:
                d = g["waited_us"]
                print(f"    {g['unit']:<9}{g['mode']:<4}{g['outcome']:<10} n={g['count']:<5} "
                      f"waited_us p50 {d.get('p50')} p90 {d.get('p90')} p99 {d.get('p99')} max {d.get('max')}")
        ws = views.get("wait_stats")
        if ws:
            for r in ws["total"]:
                print(f"  sys.wait_stats total   {r.get('event'):<22} count {r.get('count'):>7} "
                      f"total_us {r.get('total_us'):>10} max_us {r.get('max_us')}")
            for r in ws["per_core"]:
                print(f"  sys.wait_stats core {r.get('core'):<3}{r.get('event'):<22} count {r.get('count'):>7} "
                      f"total_us {r.get('total_us'):>10}")
        for key, label in (("session_a_by_wal_flush", "A (commit.wal_flush)"),
                           ("session_b_by_lock_wait", "B (app.lock_wait)")):
            r = views.get(key)
            if r:
                print(f"  sys.session_waits {label:<22} session_id {r.get('session_id')} "
                      f"count {r.get('count')} total_us {r.get('total_us')}")
        if views.get("meta"):
            print(f"  sys.meta               {views['meta']}")
        if views.get("errors"):
            print(f"  view errors            {views['errors']}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1, default=str)
        print(f"  wrote {args.json}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
