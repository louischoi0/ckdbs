#!/usr/bin/env python3
"""CP-S0 cell M5: what a standing range fence costs the point writers it does
not cover (candidate C3's price, BP-H10's shape).

`instructions/v3.2.0/workorder-cp-lock-model-exploration.md` §3 C3 and the
CP-S0 row. Every `path:line` below is at cc54a296.

---- The cell ----------------------------------------------------------------

The per-statement price a standing range fence puts on the point writes it
does not cover. Two arms, interleaved block by block (even blocks `fenced`
then `unfenced`, odd the reverse, so a host drift favours neither). Fenced:
one session holds `BEGIN; UPDATE t SET v = v + 1 WHERE id > K` open - a
range `X` over `[K + 1, kIdSpaceEnd)`, held to the decide, which is a fence -
while `--writers` sessions each run `--ops` autocommit point UPDATEs on
their own rows `k <= K`, never under the fence: none is refused or parked,
and what they pay is the walk every tuple ask on a fenced relation makes
over the 64 partitions. Unfenced: the same writers, no fence standing. The
fence ends in `ROLLBACK`, not `COMMIT`, so the covered rows gain no versions
block by block and the one thing measured is the fence standing. No
covering arm: a covered ask parks until the fence's transaction decides -
a lifetime as a wait (M3/M4), not the walk.

Per arm: p50 / p90 / p99 / p100 and the mean of the client latency, and the
two C3 numbers. **fenced p50 - unfenced p50**, in us per point UPDATE, read
against each arm's block-to-block p50 spread beside it (a delta inside that
spread is not a finding). **walks per statement**: the `contention_fence_walks`
delta over the arm (`sys.meta`'s instance-wide sum of the tally, readable
from any session; read off the timed path, the fence UPDATE's own asks
excluded) over its statements - 1.0 fenced if the fence stood for every
ask, 0.0 unfenced, below 1.0 fenced when it fell mid-block (the 60 s
ceiling, or a refused UPDATE), and the block says so. `contention_lock_partition_waits`
/ `_wait_us` are bracketed beside it: did the walk's latches meet another
core's asks. At the end, `sys.lock_waits` (seq, core, started, waited_us,
unit, mode, rel_oid, key, waiter_trx, holder_trx, outcome; `docs/spec/sys-views.md`
§6.3), expected empty since nothing here conflicts - a row means the cell
was not the one described.

The walk and its counter: `src/txn/lock_table.cpp` `FenceCoversKey`
(`:653-683`), `kFenceWalks` (`:660`); the order's §1.2 A3/A7 carry the rest.

---- Protocol and sessions ---------------------------------------------------

The connections are `tools/ckdbs_cli.py`'s: `ServerConnection` (KWP/1 on
`port`) and `TextConnection` (the newline debug surface on
`debug_text_port`, loopback, off by default). `--protocol kwp` (the
default) spreads the writers over the cores by SO_REUSEPORT
(`src/server/expeditor.cpp:1700-1718`); **`--protocol text` puts every
session on core 0** - the text listener is opened without `reuse_port` and
attached to core 0's scheduler alone (`expeditor.cpp:1779`, `:2044-2046`) -
so a text run prices the walk with every ask on one reactor and no
cross-core latch contention. Both reach one dispatcher: `BEGIN`/`COMMIT`/
`ROLLBACK` are frames over KWP and lines over text, with reply strings of
one shape (`ckdbs_cli.py:189-212`), and the transaction lifetime ceiling
applies to both listeners (`expeditor.cpp:1743`, `:1784`).

**The 60 s lifetime ceiling** (`txn_lifetime_ceiling_ms`, `kds.conf.sample:647-666`):
the server's sweep rolls the fence transaction back once it is older than
the ceiling, so every fenced block re-`BEGIN`s its fence and the driver
warns when a block held it longer than `--fence-max-s` (50 s); a `ROLLBACK`
after the ceiling answers `ROLLBACK trx_id=<expired>`
(`command_dispatcher.cpp:10870-10875`), which the walks bracket exposes.

The driver creates `cp_fence_<suffix>` (`(id int64, v int64) BTREE`, a
declared int64 tuple key) and never drops a relation it did not create.

---- The server's CPU (optional) ----------------------------------------------

The order's C3 names "the point write's CPU against the no-fence arm", and
a client's wall clock is a proxy for it. With `--server-pid <pid>` the
driver also brackets each arm with the server's `utime + stime`, read per
thread from `/proc/<pid>/task/<tid>/stat` (`tools/assertion_abort_benchmark.py:137`'s
`ServerCpu`, per thread instead of per process): a thread whose
`Cpus_allowed_list` names one CPU is a reactor, which pins
itself (`expeditor.cpp:1460-1470`), keyed `reactor@cpu<k>`; the rest (the
WAL writer, the data-sync thread) are `other`. Reported per arm as seconds
per key and as `total / statements` in us. Two caveats: the kernel accounts
CPU at `CLK_TCK` (100 Hz here), so one tick is 10 ms and an arm of a second
or two resolves to about 1 %; and an idle reactor's blocks (`sched.md` §7,
at most 10 ms each) are in the total, so the kwp cells, where every reactor
serves a writer, read cleaner than the text cells, where three reactors
idle. The latency delta stays the primary number; the CPU delta is beside
it.

Usage:
    tools/cp_s0_fence.py --port 15561 --rows 4096 --writers 4 --ops 3000 \\
        --blocks 8 --json out.json [--server-pid <pid>]
    tools/cp_s0_fence.py --protocol text --port 15562 --writers 1 ...

The server must already be running on its own data file and a **chosen**
port (`bench/README.md` rule 5).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bench_common import NAME_WIDTH, Phase, report, write_json  # noqa: E402
from ckdbs_cli import DEFAULT_HOST, ServerConnection, TextConnection  # noqa: E402
import sys_meta  # noqa: E402

WALKS_KEY = "contention_fence_walks"
PARTITION_KEYS = ("contention_lock_partition_waits", "contention_lock_partition_wait_us")
ARM_FENCED = "update-fenced"
ARM_UNFENCED = "update-unfenced"


# ---- The host's state, per cell (bench/README.md rule 4) -----------------

def host_state(with_pgrep=True):
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    busy = None
    if with_pgrep:
        try:
            busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                                  capture_output=True, text=True, timeout=5).stdout.strip()
            busy = busy.splitlines() if busy else []
        except (OSError, subprocess.SubprocessError):
            busy = "unavailable"
    return {"loadavg": load, "competing": busy, "at_unix": round(time.time(), 3)}


# ---- The server's CPU, per arm (optional: --server-pid) --------------------

class ServerCpu:
    """`utime + stime` of every thread of the server, in seconds, keyed
    `reactor@cpu<k>` for a thread pinned to the one CPU `k` and `other` for
    the rest (the docstring above says why that tells a reactor). Read
    before and after each arm, off the timed path."""

    def __init__(self, pid):
        self.pid = pid
        self._tick = os.sysconf("SC_CLK_TCK")
        if not self():
            raise SystemExit(f"--server-pid {pid}: no readable threads under /proc/{pid}/task")

    def __call__(self):
        out = {}
        base = f"/proc/{self.pid}/task"
        try:
            tids = os.listdir(base)
        except OSError:
            return out
        for tid in tids:
            try:
                with open(f"{base}/{tid}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
                with open(f"{base}/{tid}/status") as f:
                    status = f.read()
            except OSError:
                continue  # a thread that ended between the listing and the read
            cpus = ""
            for line in status.splitlines():
                if line.startswith("Cpus_allowed_list:"):
                    cpus = line.split(":", 1)[1].strip()
            key = f"reactor@cpu{cpus}" if cpus.isdigit() else "other"
            # fields[0] is stat field 3 (state); utime/stime are fields 14/15.
            out[key] = out.get(key, 0.0) + (int(fields[11]) + int(fields[12])) / self._tick
        return out


def cpu_delta(before, after):
    d = {k: round(after.get(k, 0.0) - before.get(k, 0.0), 4)
         for k in sorted(set(before) | set(after))}
    d["total"] = round(sum(after.values()) - sum(before.values()), 4)
    return d


# ---- Connections -----------------------------------------------------------

def connect(host, port, protocol):
    if protocol == "text":
        return TextConnection(host, port)
    return ServerConnection(host, port)


def must(conn, stmt, prefix):
    reply = conn.send_command(stmt)
    if not reply.startswith(prefix):
        raise SystemExit(f"{stmt[:80]!r} answered {reply[:200]!r}, expected {prefix!r}")
    return reply


def meta(conn):
    return sys_meta.from_text(conn.send_command("SHOW META"))


def census(conn):
    """The instance-wide counters this cell brackets, as ints."""
    m = meta(conn)
    if WALKS_KEY not in m:
        raise SystemExit(f"sys.meta has no {WALKS_KEY}; this server does not carry the "
                         f"fence-walk tally (contention.hpp kTallyNames) - the cell cannot "
                         f"be read against it")
    out = {WALKS_KEY: int(m[WALKS_KEY])}
    for k in PARTITION_KEYS:
        out[k] = int(m.get(k, 0))
    return out


def session_core(conn):
    return meta(conn).get("core")


class Writer:
    """One point-writer session and the disjoint rows it owns."""

    def __init__(self, host, port, protocol, index, rows):
        self.index = index
        self.conn = connect(host, port, protocol)
        self.core = session_core(self.conn)
        self.rows = rows

    def close(self):
        try:
            self.conn.close()
        except Exception:
            pass


# ---- Setup -----------------------------------------------------------------

def setup(conn, relation, rows, batch=256):
    created = conn.send_command(f"CREATE TABLE {relation} (id int64, v int64) BTREE")
    if not created.startswith("CREATED"):
        raise SystemExit(f"setup: CREATE TABLE {relation} refused: {created}")
    for lo in range(1, rows + 1, batch):
        hi = min(lo + batch, rows + 1)
        values = ",".join(f"({k}, 0)" for k in range(lo, hi))
        must(conn, f"INSERT INTO {relation} VALUES {values}", "INSERTED")


# ---- One arm ---------------------------------------------------------------

def run_writers(writers, relation, ops, name, detail):
    """`ops` point UPDATEs per writer, every writer at once; one merged Phase
    plus the per-writer phases. A reply other than `UPDATED 1` is counted
    as unexpected (an `ERR` reply is also a Phase error)."""
    per = [Phase(name, detail) for _ in writers]
    unexpected = [0] * len(writers)
    # A writer whose connection raised (a socket timeout, a closed
    # connection) ends its own loop and is named here, rather than dying
    # with a traceback nobody reads while the other writers finish and the
    # arm is reported as if it had run whole.
    failures = [None] * len(writers)
    barrier = threading.Barrier(len(writers))

    def body(slot, writer, phase):
        rows = writer.rows
        n = len(rows)
        conn = writer.conn
        barrier.wait()
        try:
            for i in range(ops):
                k = rows[i % n]
                command = f"UPDATE {relation} SET v = v + 1 WHERE id = {k}"
                t0 = time.perf_counter()
                reply = conn.send_command(command)
                phase.record(time.perf_counter() - t0, reply)
                if not reply.startswith("UPDATED 1"):
                    unexpected[slot] += 1
        except Exception as e:  # noqa: BLE001 - recorded, not swallowed
            failures[slot] = f"writer {slot} (core {writer.core}) stopped after {phase.ops} statements: {e!r}"
            print(f"  {failures[slot]}", file=sys.stderr)

    threads = [threading.Thread(target=body, args=(i, w, per[i]))
               for i, w in enumerate(writers)]
    started = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - started

    merged = Phase(name, detail)
    for phase in per:
        merged.latencies.extend(phase.latencies)
        merged.errors += phase.errors
        if merged.first_error is None:
            merged.first_error = phase.first_error
    merged.elapsed = elapsed
    return merged, sum(unexpected), [f for f in failures if f]


def latency_line(phase):
    """`Phase.summary()`'s latency keys (`tools/bench_common.py:86`) under
    this cell's names, plus the p90 the summary does not carry."""
    s = phase.summary()
    return {
        "ops": s["ops"],
        "mean_us": s["mean_us"],
        "p50_us": s["p50_us"],
        "p90_us": round(phase.percentile(90) * 1e6, 1),
        "p99_us": s["p99_us"],
        "p100_us": s["max_us"],
    }


def write_progress(path, payload):
    if not path:
        return
    tmp = path + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(payload, f)
        os.replace(tmp, path)
    except OSError:
        pass


# ---- Main ------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True,
                   help="the port --protocol listens on: KWP/1 `port`, or `debug_text_port`")
    p.add_argument("--protocol", choices=["kwp", "text"], default="kwp",
                   help="kwp spreads sessions over the cores; text puts every session on core 0")
    p.add_argument("--rows", type=int, default=4096)
    p.add_argument("--writers", type=int, default=4, help="point-writer sessions, 1 or 4 in S0")
    p.add_argument("--ops", type=int, default=2000, help="point UPDATEs per writer per arm per block")
    p.add_argument("--blocks", type=int, default=6, help="interleaved blocks, each both arms")
    p.add_argument("--fence-above", type=int, default=None,
                   help="K: the fence is `WHERE id > K`, writers stay in [1, K] (default rows - 16)")
    p.add_argument("--fence-max-s", type=float, default=50.0,
                   help="warn when a fenced block held its fence longer than this (ceiling 60 s)")
    p.add_argument("--label", default=None, help="a name for the run in the output")
    p.add_argument("--suffix", default=None, help="relation suffix (default: pid and time)")
    p.add_argument("--progress", default=None,
                   help="a JSON file rewritten after every arm, for a watchdog to poll")
    p.add_argument("--json", default=None, help="write the raw summaries here")
    p.add_argument("--server-pid", type=int, default=None,
                   help="the kds_server pid: bracket each arm with its threads' utime+stime "
                        "(/proc/<pid>/task/*/stat), reported per reactor and per statement")
    args = p.parse_args()

    if args.writers < 1:
        raise SystemExit("--writers must be at least 1")
    rows = args.rows
    K = args.fence_above if args.fence_above is not None else rows - 16
    if K < args.writers:
        raise SystemExit(f"--fence-above {K} leaves fewer rows than writers")
    if K > rows:
        raise SystemExit(f"--fence-above {K} is above --rows {rows}; the fence would be empty")
    suffix = args.suffix or f"{os.getpid()}_{int(time.time()) % 100000}"
    if not re.fullmatch(r"\w+", suffix, re.ASCII):
        # A hyphen here cost S0 its first four M5 cells: the name is refused at
        # the CREATE, after the server is up. Refused before anything connects.
        raise SystemExit(f"--suffix {suffix!r}: letters, digits and '_' only")
    relation = f"cp_fence_{suffix}"
    label = args.label or f"{args.protocol}-w{args.writers}"

    host_before = host_state()
    cpu = ServerCpu(args.server_pid) if args.server_pid else None

    control = connect(args.host, args.port, args.protocol)   # setup, SHOW META, sys views
    fence = connect(args.host, args.port, args.protocol)     # the fence holder
    control_core = session_core(control)
    fence_core = session_core(fence)

    t_setup = time.perf_counter()
    setup(control, relation, rows)
    setup_s = time.perf_counter() - t_setup

    # Disjoint rows per writer, all inside [1, K] - never under the fence.
    writers = []
    for w in range(args.writers):
        owned = [k for k in range(1, K + 1) if k % args.writers == w]
        writers.append(Writer(args.host, args.port, args.protocol, w, owned))

    fence_stmt = f"UPDATE {relation} SET v = v + 1 WHERE id > {K}"
    arms_merged = {ARM_FENCED: Phase(ARM_FENCED, f"fence `id > {K}` held by another session: every tuple ask walks 64 partitions"),
                   ARM_UNFENCED: Phase(ARM_UNFENCED, "no fence standing: the counter reads 0, no walk")}
    blocks = []
    census_start = census(control)
    run_started = time.perf_counter()

    for b in range(args.blocks):
        order = (ARM_FENCED, ARM_UNFENCED) if b % 2 == 0 else (ARM_UNFENCED, ARM_FENCED)
        for arm in order:
            entry = {"block": b, "arm": arm, "loadavg": host_state(with_pgrep=False)["loadavg"]}
            fence_held_s = None
            fence_rows = None
            rollback_reply = None
            if arm == ARM_FENCED:
                begin = must(fence, "BEGIN", "BEGIN")
                t_fence = time.perf_counter()
                reply = must(fence, fence_stmt, "UPDATED")
                fence_rows = int(reply.split()[1])
                entry["begin_reply"] = begin
                entry["fence_update_reply"] = reply
            before = census(control)
            cpu_before = cpu() if cpu else None
            phase, unexpected, failures = run_writers(writers, relation, args.ops, arm, "")
            cpu_after = cpu() if cpu else None
            after = census(control)
            if arm == ARM_FENCED:
                fence_held_s = time.perf_counter() - t_fence
                rollback_reply = fence.send_command("ROLLBACK")
                entry["rollback_reply"] = rollback_reply
                if not rollback_reply.startswith("ROLLBACK"):
                    print(f"  block {b} {arm}: ROLLBACK answered {rollback_reply!r}", file=sys.stderr)
            statements = phase.ops
            walks = after[WALKS_KEY] - before[WALKS_KEY]
            cpu_d = cpu_delta(cpu_before, cpu_after) if cpu else None
            entry.update({
                "statements": statements,
                "errors": phase.errors,
                "unexpected_replies": unexpected,
                "writer_failures": failures,
                "elapsed_s": round(phase.elapsed, 4),
                "server_cpu_s": cpu_d,
                "server_cpu_us_per_statement": (round(cpu_d["total"] / statements * 1e6, 2)
                                                if cpu_d and statements else None),
                "fence_walks_delta": walks,
                "walks_per_statement": round(walks / statements, 4) if statements else None,
                "lock_partition_waits_delta": after[PARTITION_KEYS[0]] - before[PARTITION_KEYS[0]],
                "lock_partition_wait_us_delta": after[PARTITION_KEYS[1]] - before[PARTITION_KEYS[1]],
                "fence_rows_updated": fence_rows,
                "fence_held_s": round(fence_held_s, 3) if fence_held_s is not None else None,
                "latency": latency_line(phase),
            })
            if arm == ARM_FENCED:
                if fence_held_s is not None and fence_held_s > args.fence_max_s:
                    entry["warning"] = (f"fence held {fence_held_s:.1f} s, past --fence-max-s "
                                        f"{args.fence_max_s:g}; the 60 s ceiling may have ended it")
                elif statements and walks < statements:
                    entry["warning"] = (f"only {walks} walks for {statements} statements: the fence "
                                        f"did not stand for every ask")
            elif walks != 0:
                entry["warning"] = f"{walks} fence walks in the unfenced arm: another fence stood"
            if failures and not entry.get("warning"):
                entry["warning"] = f"{len(failures)} writer(s) stopped early; see writer_failures"
            blocks.append(entry)
            merged = arms_merged[arm]
            merged.latencies.extend(phase.latencies)
            merged.errors += phase.errors
            if merged.first_error is None:
                merged.first_error = phase.first_error
            merged.elapsed += phase.elapsed
            # The whole block table so far rides in the progress file, so a
            # cell `timeout` ends mid-run still leaves every finished arm.
            write_progress(args.progress, {"label": label, "relation": relation, "block": b,
                                           "of_blocks": args.blocks, "arm": arm,
                                           "at_unix": round(time.time(), 3), "pid": os.getpid(),
                                           "walks_per_statement": entry["walks_per_statement"],
                                           "p50_us": entry["latency"]["p50_us"],
                                           "blocks": blocks})
            if entry.get("warning"):
                print(f"  block {b} {arm}: {entry['warning']}", file=sys.stderr)

    run_s = time.perf_counter() - run_started
    census_end = census(control)
    lock_waits = sys_meta.view_rows(control.send_command("SELECT * FROM sys.lock_waits"))
    meta_end = meta(control)
    host_after = host_state()

    # ---- Report --------------------------------------------------------------
    run_meta = {
        "engine": "ckdbs",
        "cell": "CP-S0 M5",
        "label": label,
        "columns": 2,
        "rows": rows,
        "clustered": "BTREE",
        "connections": args.writers,
        "host": args.host,
        "port": args.port,
        "protocol": args.protocol,
        "table": relation,
        "fence_above": K,
        "fence_statement": fence_stmt,
        "writers": args.writers,
        "ops_per_writer_per_arm": args.ops,
        "blocks": args.blocks,
        "setup_s": round(setup_s, 3),
        "run_s": round(run_s, 3),
        "control_core": control_core,
        "fence_core": fence_core,
        "server_pid": args.server_pid,
        "writer_cores": [w.core for w in writers],
        "writer_rows": [len(w.rows) for w in writers],
        "census_start": census_start,
        "census_end": census_end,
        "host_before": host_before,
        "host_after": host_after,
        "lock_waits_rows": lock_waits,
        "lock_waits_counters": {k: meta_end.get(k) for k in
                                ("lock_waits_recorded", "lock_waits_overwritten", "lock_waits_dropped")},
        "block_table": blocks,
    }

    phases = [arms_merged[ARM_FENCED], arms_merged[ARM_UNFENCED]]
    footer = [
        f"protocol {args.protocol}: {'every session on core 0 (text listener, expeditor.cpp:1779/2045)' if args.protocol == 'text' else 'sessions spread by SO_REUSEPORT (expeditor.cpp:1718)'}",
        f"fence session core {fence_core}; writer cores {[w.core for w in writers]} (initial; an autocommit session may move between transactions, BL-S3)",
        "latency is the client's wall clock around one send + one receive; sub-10us deltas are inside the harness floor",
    ]
    report(phases, run_meta, footer)

    # The C3 answer: per-arm p50/p90/p99/p100, the delta, the spread, the walks.
    fenced = latency_line(arms_merged[ARM_FENCED])
    unfenced = latency_line(arms_merged[ARM_UNFENCED])
    per_block = {arm: [e["latency"]["p50_us"] for e in blocks if e["arm"] == arm]
                 for arm in (ARM_FENCED, ARM_UNFENCED)}
    # An arm with no statement (every writer failed) has no ratio; left out
    # of the min/max rather than crashing the report after the run.
    walks = {arm: [e["walks_per_statement"] for e in blocks
                   if e["arm"] == arm and e["walks_per_statement"] is not None]
             for arm in (ARM_FENCED, ARM_UNFENCED)}
    cpu_per_stmt = {arm: [e["server_cpu_us_per_statement"] for e in blocks
                          if e["arm"] == arm and e["server_cpu_us_per_statement"] is not None]
                    for arm in (ARM_FENCED, ARM_UNFENCED)}
    cpu_arm_us = {}
    for arm in (ARM_FENCED, ARM_UNFENCED):
        # One ratio over the arm's blocks: total CPU over total statements.
        tot_s = sum(e["server_cpu_s"]["total"] for e in blocks
                    if e["arm"] == arm and e.get("server_cpu_s"))
        tot_n = sum(e["statements"] for e in blocks if e["arm"] == arm and e.get("server_cpu_s"))
        cpu_arm_us[arm] = (tot_s / tot_n * 1e6) if tot_n else None
    print(f"{'arm':<{NAME_WIDTH}}{'ops':>8}{'mean us':>10}{'p50':>9}{'p90':>9}{'p99':>9}{'p100':>10}"
          f"{'walks/stmt':>12}{'p50 spread':>22}")
    for arm, line in ((ARM_FENCED, fenced), (ARM_UNFENCED, unfenced)):
        spread = per_block[arm]
        w = walks[arm]
        print(f"{arm:<{NAME_WIDTH}}{line['ops']:>8}{line['mean_us']:>10.1f}{line['p50_us']:>9.1f}"
              f"{line['p90_us']:>9.1f}{line['p99_us']:>9.1f}{line['p100_us']:>10.1f}"
              f"{(min(w) if w else 0):>6.2f}..{(max(w) if w else 0):<5.2f}"
              f"{(min(spread) if spread else 0):>9.1f}..{(max(spread) if spread else 0):<10.1f}")
    delta_p50 = fenced["p50_us"] - unfenced["p50_us"]
    delta_mean = fenced["mean_us"] - unfenced["mean_us"]
    floor = max((max(s) - min(s)) if s else 0.0 for s in per_block.values())
    print()
    print(f"  C3: fenced - unfenced = {delta_p50:+.1f} us at p50, {delta_mean:+.1f} us at the mean, "
          f"per point UPDATE on an uncovered row ({args.writers} writer{'s' if args.writers != 1 else ''}, "
          f"{args.protocol}); block-to-block p50 spread within one arm up to {floor:.1f} us")
    if cpu is not None and cpu_arm_us[ARM_FENCED] is not None and cpu_arm_us[ARM_UNFENCED] is not None:
        cf, cu = cpu_arm_us[ARM_FENCED], cpu_arm_us[ARM_UNFENCED]
        spread = max((max(s) - min(s)) if s else 0.0 for s in cpu_per_stmt.values())
        print(f"  C3 (server CPU, pid {args.server_pid}, every thread): fenced {cf:.2f} us/stmt, "
              f"unfenced {cu:.2f} us/stmt, delta {cf - cu:+.2f} us/stmt; block-to-block spread "
              f"within one arm up to {spread:.2f} us (CLK_TCK {os.sysconf('SC_CLK_TCK')} Hz)")
    print(f"  walks: {census_end[WALKS_KEY] - census_start[WALKS_KEY]} fence walks over the run; "
          f"fenced arm {min(walks[ARM_FENCED]) if walks[ARM_FENCED] else 0:.3f}..{max(walks[ARM_FENCED]) if walks[ARM_FENCED] else 0:.3f} per statement, "
          f"unfenced arm {min(walks[ARM_UNFENCED]) if walks[ARM_UNFENCED] else 0:.3f}..{max(walks[ARM_UNFENCED]) if walks[ARM_UNFENCED] else 0:.3f}")
    print(f"  lock_partition latch waits over the run: "
          f"{census_end[PARTITION_KEYS[0]] - census_start[PARTITION_KEYS[0]]} waits, "
          f"{census_end[PARTITION_KEYS[1]] - census_start[PARTITION_KEYS[1]]} us")
    print()
    print(f"  per block: block arm statements walks/stmt p50 p99 partition-waits fence_rows "
          f"fence_held_s cpu_us/stmt")
    for e in blocks:
        cpu_col = e.get("server_cpu_us_per_statement")
        print(f"    {e['block']:>3} {e['arm']:<16} {e['statements']:>7} "
              f"{(e['walks_per_statement'] if e['walks_per_statement'] is not None else 0):>8.3f} "
              f"{e['latency']['p50_us']:>8.1f} {e['latency']['p99_us']:>8.1f} "
              f"{e['lock_partition_waits_delta']:>8} "
              f"{str(e['fence_rows_updated'] if e['fence_rows_updated'] is not None else '-'):>6} "
              f"{str(e['fence_held_s'] if e['fence_held_s'] is not None else '-'):>8} "
              f"{(f'{cpu_col:.2f}' if cpu_col is not None else '-'):>8}"
              + (f"  ! {e['warning']}" if e.get("warning") else ""))
    print()
    print(f"  sys.lock_waits: {len(lock_waits)} row(s) "
          f"(recorded {run_meta['lock_waits_counters']['lock_waits_recorded']}, "
          f"overwritten {run_meta['lock_waits_counters']['lock_waits_overwritten']}, "
          f"dropped {run_meta['lock_waits_counters']['lock_waits_dropped']})")
    for row in lock_waits[:50]:
        print("    " + " ".join(f"{k}={row.get(k)}" for k in
                                ("seq", "core", "waited_us", "unit", "mode", "rel_oid", "key",
                                 "waiter_trx", "holder_trx", "outcome")))
    if len(lock_waits) > 50:
        print(f"    ... {len(lock_waits) - 50} more")
    warned = [e for e in blocks if e.get("warning")]
    if warned:
        print(f"  {len(warned)} block arm(s) carried a warning; read them before quoting a number")

    if args.json:
        write_json(args.json, run_meta, phases)

    for w in writers:
        w.close()
    for c in (fence, control):
        try:
            c.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
