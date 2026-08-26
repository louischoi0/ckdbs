#!/usr/bin/env python3
"""Prices the var-heap write path: what a spilled `varchar` costs per statement.

Why this driver exists. A tuple is fixed-length, so a `varchar` longer than
`inline_cell_width - 3` does not live in the row: it is appended to the
relation's var-heap chain and the cell carries a pointer
(`docs/spec/heap-and-tuple.md` §3.4). That append is three different amounts of
work depending on whether it fits the current tail page:

    fits          one VARHEAP_APPEND record
    tail full     a new page, a link edit on the old tail, and - since the
                  var-heap growth records landed - a kVarHeap PAGE_INIT plus
                  an 8 KiB FULL_PAGE_IMAGE of the linked tail, before the
                  append (`CommandDispatcher::LogSpills`)

A page holds 8144 bytes of values (`varheap::kMaxValueSize`) and a value
costs `len + 4` bytes of it, so `--value-bytes 1600` fills a page every 5
rows: one row in five pays the structural records, and that is the highest
FPI rate a realistic string length can produce. That ratio is the knob -
raise `--value-bytes` to make growth more frequent, lower it to make it rare.

What it measures, five phases, each timed per statement on one connection:

    ping            SHOW META                                the round-trip floor
    insert-spill    INSERT of a `--value-bytes` value        spills, grows
    insert-inline   INSERT of an `--inline-bytes` value       never spills
    update-spill    UPDATE to a fresh `--value-bytes` value   spills, grows
    update-inline   UPDATE to a fresh `--inline-bytes` value  never spills

`ping` is what makes the rest decomposable: it is the client round trip and
the dispatch with no relation under it, so every other phase's p0 minus the
ping's p0 is that path's own floor.

**The two inline phases are the control.** They run the same statement shape
against the same durability class on the same server and touch no var-heap
page at all, so any var-heap logging change is invisible to them by
construction. A delta on a spill phase that is matched by an equal delta on
its inline twin is the host moving, not the engine.

Every var-heap append walks the chain from its root to the tail
(`varheap::ChainAppend`), so the per-statement cost of a spill grows with the
number of pages already in the chain. That is why `--rows` is swept rather
than fixed: at 200 rows the chain is 40 pages, at 10,000 it is 2,000, and the
two numbers do not describe the same statement.

One server per invocation, on its own fresh data file: catalog rows are never
reclaimed and undo never purges, so a second run against one file is not a
repeat of the first.

Usage - one invocation is one (binary, rows, durability) cell:

    tools/varheap_spill_benchmark.py --rows 1000 --durability strict \
        --label head --json head-1000-strict.json

    # A/B against another build's server, same flags, interleaved:
    tools/varheap_spill_benchmark.py --rows 1000 --durability strict \
        --binary /path/to/other/build-release/kds_server --label base \
        --json base-1000-strict.json

Guards, both refusable with --force and both recorded in the JSON: a scratch
directory on tmpfs (fsync is free there and every durability class measures
the same), and a host whose 1- or 5-minute load average says another process
is running.
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bench_common import report, run_phase, write_json
from ckdbs_cli import ServerConnection, format_reply

REPO = Path(__file__).resolve().parent.parent

# Same bar latency_matrix.py applies, and for the same reason: these runs were
# once taken at load average 3.2 on a 2-core box and produced 14 ms outliers
# with no engine work behind them.
MAX_LOAD_PER_CORE = 0.5
MAX_LOAD5_PER_CORE = 1.0

ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"


def abort(message):
    print(f"\n  {message}\n", file=sys.stderr)
    sys.exit(1)


def filesystem_of(path):
    out = subprocess.run(["df", "-T", str(path)], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    lines = out.stdout.strip().splitlines()
    return lines[-1].split()[1] if len(lines) >= 2 else None


def check_host(scratch, force):
    fs = filesystem_of(scratch)
    if fs == "tmpfs" and not force:
        abort(f"{scratch} is on tmpfs, where fsync costs ~0.3us and a WAL record is\n"
              f"  free. This driver measures WAL volume; on tmpfs it measures nothing.\n"
              f"  Point --scratch at a real device, or pass --force on purpose.")
    load1, load5, _ = os.getloadavg()
    cores = os.cpu_count() or 1
    if load1 > MAX_LOAD_PER_CORE * cores and not force:
        abort(f"1-minute load average is {load1:.2f} on {cores} core(s): wait, or --force.")
    if load5 > MAX_LOAD5_PER_CORE * cores and not force:
        abort(f"5-minute load average is {load5:.2f} on {cores} core(s) (1-minute is "
              f"{load1:.2f}):\n  the box is still draining load. Wait, or --force.")
    return {"filesystem": fs, "loadavg_1m": round(load1, 2), "loadavg_5m": round(load5, 2),
            "cores": cores}


def git_state():
    head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True)
    dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain"],
                           capture_output=True, text=True)
    branch = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--abbrev-ref", "HEAD"],
                            capture_output=True, text=True)
    return {"commit": head.stdout.strip(), "branch": branch.stdout.strip(),
            "dirty": bool(dirty.stdout.strip())}


class Server:
    """One kds_server on its own fresh data file, stopped on the way out."""

    def __init__(self, binary, scratch, name, port, durability, extra_lines):
        self.dir = Path(scratch) / f"varheap-{name}"
        if self.dir.exists():
            shutil.rmtree(self.dir)
        (self.dir / "wal").mkdir(parents=True)
        self.log = self.dir / "stdout.log"
        self.wal_dir = self.dir / "wal"
        conf = self.dir / "kds.conf"
        lines = [
            f"data_file = {self.dir / 'kds.db'}",
            f"wal_dir = {self.wal_dir}",
            f"port = {port}",
            f"durability = {durability}",
            "log_level = warn",
            f"log_dir = {self.dir}",
            "log_file = kds.log",
        ] + list(extra_lines)
        conf.write_text("\n".join(lines) + "\n")
        self.binary, self.conf, self.port = Path(binary), conf, port
        self.proc = None
        self.boot_s = 0.0

    def __enter__(self):
        started = time.perf_counter()
        with open(self.log, "w") as out:
            self.proc = subprocess.Popen([str(self.binary), "--config", str(self.conf)],
                                         stdout=out, stderr=subprocess.STDOUT)
        deadline = time.time() + 30.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                abort(f"the server exited during startup:\n{self.log.read_text()[-800:]}")
            if "listening on" in self.log.read_text():
                self.boot_s = time.perf_counter() - started
                return self
            time.sleep(0.05)
        abort(f"the server did not come up within 30s:\n{self.log.read_text()[-800:]}")

    def __exit__(self, *_):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
        return False

    def wal_bytes(self):
        """How far the log has been written, in bytes.

        Nothing reports `append_lsn` to a client and a segment file is created
        at its full size, so neither `du` nor the apparent size moves. What
        does move is the position of the last non-zero byte: the stream is
        strictly append-only within a segment and a fresh segment is created as
        zeroes, so scanning backwards for the first non-zero byte of the last
        segment locates the append point within a record's alignment.

        Read outside every timed region, and the segments are re-read rather
        than tracked, so this costs the run nothing.
        """
        segments = sorted(self.wal_dir.rglob("*"))
        segments = [s for s in segments if s.is_file()]
        if not segments:
            return 0
        total = 0
        for path in segments:
            size = path.stat().st_size
            used = 0
            with open(path, "rb") as f:
                chunk = 1 << 20
                pos = size
                while pos > 0:
                    start = max(0, pos - chunk)
                    f.seek(start)
                    data = f.read(pos - start)
                    stripped = data.rstrip(b"\x00")
                    if stripped:
                        used = start + len(stripped)
                        break
                    pos = start
            # A segment before the tail is whole, whatever its trailing zeroes
            # say: a seal with no room for a PAD leaves them (`wal/stream.cpp`).
            total += size if used and path != segments[-1] else used
        return total


def value(rng, length):
    return "".join(rng.choices(ALPHABET, k=length))


def create(conn, table, clustered):
    clause = "" if clustered == "heap" else f" {clustered.upper()}"
    reply = format_reply(conn.send_command(
        f"CREATE TABLE {table} (id int64, tag int64, payload varchar){clause}"))
    if reply.startswith("ERR"):
        abort(f"CREATE TABLE {table} failed: {reply}")


def insert_commands(table, rows, length, rng):
    for i in range(rows):
        yield f"INSERT INTO {table} VALUES ({i}, '{value(rng, length)}')"


def update_commands(table, ops, rows, length, rng):
    for i in range(ops):
        yield (f"UPDATE {table} SET payload = '{value(rng, length)}' "
               f"WHERE id = {(i % rows) + 1}")


def check(phase):
    if phase.errors:
        abort(f"{phase.name}: {phase.errors} error replies, first: {phase.first_error}")


def verify(conn, spill_table, inline_table, rows, value_bytes, inline_bytes, update_ops):
    """Reads back what the run wrote: every row present, and a spilled value
    resolvable at its full length.

    A throughput number over a workload that lost a value is a measurement of
    nothing, and the var-heap is exactly the structure where a lost write
    looks like a fast one.
    """
    problems = []
    for table in (spill_table, inline_table):
        reply = format_reply(conn.send_command(f"SELECT COUNT(*) FROM {table}"))
        if str(rows) not in reply:
            problems.append(f"{table}: COUNT(*) reply {reply!r} is not {rows}")

    # The last row an UPDATE touched, and one it did not: both must resolve to
    # a full-length value through the var-heap pointer in the cell.
    updated_id = ((update_ops - 1) % rows) + 1 if update_ops else 1
    untouched_id = rows
    for pk in {updated_id, untouched_id}:
        reply = format_reply(conn.send_command(
            f"SELECT payload FROM {spill_table} WHERE id = {pk}"))
        got = reply.strip().splitlines()[-1].strip() if reply.strip() else ""
        if len(got) != value_bytes:
            problems.append(f"{spill_table} id={pk}: payload is {len(got)} bytes, "
                            f"expected {value_bytes}")
    reply = format_reply(conn.send_command(
        f"SELECT payload FROM {inline_table} WHERE id = 1"))
    got = reply.strip().splitlines()[-1].strip() if reply.strip() else ""
    if len(got) != inline_bytes:
        problems.append(f"{inline_table} id=1: payload is {len(got)} bytes, "
                        f"expected {inline_bytes}")
    return problems


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--binary", default=str(REPO / "build-release" / "kds_server"),
                   help="the kds_server to measure (default: this tree's release build)")
    p.add_argument("--label", default="ckdbs", help="name for this cell in the output")
    p.add_argument("--rows", type=int, default=1000,
                   help="rows inserted into each relation (the size sweep knob)")
    p.add_argument("--value-bytes", type=int, default=1600,
                   help="length of a spilled value; 1600 fills a var-heap page every 5 rows")
    p.add_argument("--inline-bytes", type=int, default=32,
                   help="length of the control value; must stay under inline_cell_width - 3")
    p.add_argument("--ping-ops", type=int, default=200,
                   help="statements in the ping phase - the round-trip floor")
    p.add_argument("--update-ops", type=int, default=0,
                   help="UPDATE statements per update phase (0 = min(rows, 1000))")
    p.add_argument("--durability", default="strict", choices=("strict", "group", "relaxed"))
    p.add_argument("--clustered", default="btree", choices=("btree", "heap"),
                   help="btree keeps an UPDATE's read side an O(depth) descent")
    p.add_argument("--port", type=int, default=15461)
    p.add_argument("--scratch", default=str(Path.home() / "varheap-bench"),
                   help="where the data file and WAL live; must not be tmpfs")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--json", default=None)
    p.add_argument("--verify", dest="verify", action="store_true", default=True)
    p.add_argument("--no-verify", dest="verify", action="store_false")
    p.add_argument("--force", action="store_true")
    args = p.parse_args()

    if args.inline_bytes > 60:
        abort(f"--inline-bytes {args.inline_bytes} would spill at the default "
              f"inline_cell_width of 64 (capacity is width - 3), which makes the "
              f"control phases measure the same path as the spill phases.")
    scratch = Path(args.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    host = check_host(scratch, args.force)
    binary = Path(args.binary)
    if not binary.exists():
        abort(f"no server at {binary}")

    update_ops = args.update_ops or min(args.rows, 1000)
    rng = random.Random(args.seed)
    suffix = f"{int(time.time())}_{rng.randrange(1 << 20)}"
    spill_table = f"vh_spill_{suffix}"
    inline_table = f"vh_inline_{suffix}"
    name = f"{args.label}-{args.rows}-{args.durability}"

    with Server(binary, scratch, name, args.port, args.durability, []) as server:
        conn = ServerConnection("127.0.0.1", args.port, timeout=120.0)
        meta_line = format_reply(conn.send_command("SHOW META"))
        wal_before = server.wal_bytes()

        create(conn, spill_table, args.clustered)
        create(conn, inline_table, args.clustered)

        def execute(command):
            return conn.send_command(command)

        phases = []
        wal_by_phase = {}

        # The floor. `SHOW META` reads no relation, opens no transaction and
        # writes nothing, so what it measures is the client's send + the
        # server's read, dispatch and reply, and the client's recv - the wait
        # every other row here also pays and none of them can be smaller than.
        phases.append(run_phase(
            execute, "ping", ("SHOW META" for _ in range(args.ping_ops)),
            detail="SHOW META: round trip + dispatch + reply, no relation touched"))
        check(phases[-1])
        wal_by_phase["ping"] = server.wal_bytes()

        phases.append(run_phase(
            execute, "insert-spill",
            insert_commands(spill_table, args.rows, args.value_bytes, rng),
            detail=f"{args.value_bytes}-byte value, one var-heap page per "
                   f"{max(1, 8148 // (args.value_bytes + 4))} rows"))
        check(phases[-1])
        wal_after_spill = server.wal_bytes()
        wal_by_phase["insert-spill"] = wal_after_spill

        phases.append(run_phase(
            execute, "insert-inline",
            insert_commands(inline_table, args.rows, args.inline_bytes, rng),
            detail=f"{args.inline_bytes}-byte value, inline - the control"))
        check(phases[-1])
        wal_by_phase["insert-inline"] = server.wal_bytes()

        phases.append(run_phase(
            execute, "update-spill",
            update_commands(spill_table, update_ops, args.rows, args.value_bytes, rng),
            detail="each UPDATE appends a new version's value; values are immutable"))
        check(phases[-1])
        wal_by_phase["update-spill"] = server.wal_bytes()

        phases.append(run_phase(
            execute, "update-inline",
            update_commands(inline_table, update_ops, args.rows, args.inline_bytes, rng),
            detail="inline - the control"))
        check(phases[-1])
        wal_by_phase["update-inline"] = server.wal_bytes()

        conn.send_command("SYNC")
        wal_end = server.wal_bytes()
        problems = []
        if args.verify:
            problems = verify(conn, spill_table, inline_table, args.rows,
                              args.value_bytes, args.inline_bytes, update_ops)
        conn.close()

    meta = {
        "engine": f"ckdbs [{args.label}]",
        "columns": 3,
        "rows": args.rows,
        "clustered": args.clustered,
        "host": "127.0.0.1",
        "port": args.port,
        "table": spill_table,
        "label": args.label,
        "binary": str(binary),
        "binary_mtime": time.strftime("%Y-%m-%d %H:%M:%S",
                                     time.localtime(binary.stat().st_mtime)),
        "durability": args.durability,
        "value_bytes": args.value_bytes,
        "inline_bytes": args.inline_bytes,
        "update_ops": update_ops,
        "rows_per_varheap_page": max(1, 8148 // (args.value_bytes + 4)),
        "boot_s": round(server.boot_s, 4),
        "wal_bytes_at_mount": wal_before,
        "wal_bytes_after_insert_spill": wal_after_spill,
        "wal_bytes_at_end": wal_end,
        "wal_bytes_by_phase": wal_by_phase,
        "show_meta": meta_line,
        "git": git_state(),
        "host_check": host,
        "guards_forced": args.force,
        "verified": args.verify,
        "verify_problems": problems,
    }
    footer = [
        f"binary {binary} (mtime {meta['binary_mtime']}), durability {args.durability}",
        f"WAL written: {wal_before} B at mount, {wal_after_spill} B after "
        f"insert-spill ({(wal_after_spill - wal_before) / max(1, args.rows):.0f} B/row), "
        f"{wal_end} B at end",
        "every latency includes Python's socket round trip; sub-10us deltas are noise",
    ]
    if args.force:
        footer.append("host guards overridden with --force; the load average recorded above "
                      "is what the run actually saw")
    if args.verify:
        footer.append("verify: " + ("clean" if not problems else "; ".join(problems)))
    report(phases, meta, footer)
    if args.json:
        write_json(args.json, meta, phases)
    if problems:
        sys.exit(2)


if __name__ == "__main__":
    main()
