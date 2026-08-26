#!/usr/bin/env python3
"""Prices a mount: how long a server takes to become able to answer, and why.

Recovery runs at every mount since RC11, so the cost of starting a server is
now the cost of reading the log back. This driver measures that end to end and
then attributes it, because the two questions have different answers:

    wall time      Popen -> the "listening on" line in the server's own log.
                   The server prints that line *before* it binds, so the
                   number is everything from exec to the last thing the mount
                   does and excludes bind()/listen() - microseconds, and the
                   reason `connect()` below retries a refusal instead of
                   trusting the line.
    attribution    `SHOW META`'s per-phase counters, which RC09 added:
                   recovery_analysis_us, recovery_redo_us,
                   recovery_high_water_us, recovery_undo_us and
                   recovery_checkpoint_us

The gap between the two is everything recovery does not own - opening the data
file, the buffer pool, the catalog scan, binding the listener.

Three things this is built to tell apart:

1. **Segment-size cost vs record-count cost.** `wal::ScanLog` allocates and
   reads a segment's whole body in one call, and analysis and redo each run
   their own scan, so a mount's scan cost is a function of `segment_size`
   (64 MiB by default) rather than of how much was logged. `--rows 0` gives an
   empty log and `--rows N` a populated one; if the two mount in the same time,
   the cost is the segment and not the records.
2. **What a crash costs the mount after it.** `--crash` SIGKILLs the loader
   instead of stopping it cleanly, so the first mount has winners to redo and
   losers to roll back where a clean stop leaves nothing.
3. **What RC08's completion checkpoint buys.** Each mount ends by publishing
   an anchor past everything it replayed, so `--mounts N` with N > 2 shows the
   first mount paying for the crash and the later ones starting from the
   anchor. `recovery_records` per mount is the count that says whether the
   anchor moved.

Every mount is measured on the *same* data file, which is the point - a fresh
file per mount would measure a create, not a recovery. The file is created
fresh at the start of each invocation.

Usage - one invocation is one (binary, rows, crash) cell:

    tools/mount_cost_benchmark.py --mounts 9 --rows 0 --label head-empty
    tools/mount_cost_benchmark.py --mounts 9 --rows 2000 --crash \
        --label head-crash --json head-crash.json

    # the same file shape against another build's server:
    tools/mount_cost_benchmark.py --mounts 9 --rows 0 --label base \
        --binary /path/to/other/build-release/kds_server

A server that does not report recovery counters at all - one built before
RC09 - is handled rather than refused: the attribution columns read `-` and
the wall time still stands, which is what makes a before/after comparison
possible.
"""

import argparse
import atexit
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bench_common import Phase
from ckdbs_cli import ServerConnection, format_reply

REPO = Path(__file__).resolve().parent.parent
ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"

COUNTERS = ("recovery_records", "recovery_committed", "recovery_rolled_back",
            "recovery_redo_applied", "recovery_pages_healed", "recovery_analysis_us",
            "recovery_redo_us", "recovery_high_water_us", "recovery_undo_us",
            "recovery_checkpoint_us")


def abort(message):
    print(f"\n  {message}\n", file=sys.stderr)
    sys.exit(1)


def filesystem_of(path):
    out = subprocess.run(["df", "-T", str(path)], capture_output=True, text=True)
    lines = out.stdout.strip().splitlines()
    return lines[-1].split()[1] if len(lines) >= 2 else None


def parse_meta(line):
    """Pulls the recovery counters out of a `SHOW META` reply.

    Absent rather than zero when the server did not report one: a build with
    no recovery has not recovered nothing, it has no answer.
    """
    found = {}
    for key in COUNTERS:
        m = re.search(rf"\b{key}=(\d+)", line)
        if m:
            found[key] = int(m.group(1))
    return found


# Every server this process started and has not stopped. An abort() below is a
# sys.exit, which would otherwise leave the server running and hold its port -
# and the next run's bind() failure looks nothing like the cause.
LIVE = []


def _stop_live():
    for mount in list(LIVE):
        mount.stop()


atexit.register(_stop_live)


class Mount:
    """One start/measure/stop cycle against an existing data file."""

    def __init__(self, binary, conf, log_path, port):
        self.binary, self.conf, self.log_path, self.port = binary, conf, log_path, port
        self.proc = None

    def start(self):
        # The log is truncated first so "listening on" cannot be a previous
        # mount's line: the wall time is measured off exactly this one.
        self.log_path.write_text("")
        started = time.perf_counter()
        with open(self.log_path, "w") as out:
            self.proc = subprocess.Popen([str(self.binary), "--config", str(self.conf)],
                                         stdout=out, stderr=subprocess.STDOUT)
        LIVE.append(self)
        deadline = time.time() + 60.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                abort(f"the server exited during startup:\n{self.log_path.read_text()[-900:]}")
            if "listening on" in self.log_path.read_text():
                return time.perf_counter() - started
            time.sleep(0.002)
        abort(f"no listener within 60s:\n{self.log_path.read_text()[-900:]}")

    def connect(self, timeout=60.0):
        """Connects, retrying a refusal.

        The server prints "listening on" from the thread that then binds, so a
        connect racing that line loses it once every dozen mounts. Retried
        rather than slept past, because a fixed sleep would land inside the
        measured wall time above.
        """
        deadline = time.time() + timeout
        while True:
            try:
                return ServerConnection("127.0.0.1", self.port, timeout=timeout)
            except ConnectionRefusedError:
                if time.time() > deadline:
                    raise
                time.sleep(0.01)

    def stop(self, crash=False):
        if self in LIVE:
            LIVE.remove(self)
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--binary", default=str(REPO / "build-release" / "kds_server"))
    p.add_argument("--label", default="ckdbs")
    p.add_argument("--mounts", type=int, default=9,
                   help="measured mounts after the load (the create is not one of them)")
    p.add_argument("--rows", type=int, default=0,
                   help="rows written before the mount sweep; 0 leaves an empty log")
    p.add_argument("--value-bytes", type=int, default=1600,
                   help="value length of the loaded rows; over 61 spills to the var-heap")
    p.add_argument("--crash", action="store_true",
                   help="SIGKILL the loader, so the first mount has work to replay")
    p.add_argument("--assertion", action="store_true",
                   help="declare a group ceiling over the loaded relation, so the "
                        "load leaves one Bound Cabin entry per row and every "
                        "measured mount runs RC07's assertion revival and "
                        "AttachEntriesFromPages over those entries. Without it "
                        "no assertion exists and that walk does nothing")
    p.add_argument("--assert-groups", type=int, default=64,
                   help="distinct GROUP BY values under --assertion; the entry "
                        "count is --rows regardless, this only sets how many "
                        "group headers the walk attributes them to")
    p.add_argument("--assert-rollback-every", type=int, default=0,
                   help="under --assertion, every Nth row is inserted in its own "
                        "transaction and rolled back, so its entry is left "
                        "orphaned on the page. 0 rolls back nothing")
    p.add_argument("--durability", default="relaxed", choices=("strict", "group", "relaxed"))
    p.add_argument("--port", type=int, default=15481)
    p.add_argument("--scratch", default=str(Path.home() / "mount-bench"))
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--json", default=None)
    p.add_argument("--force", action="store_true")
    args = p.parse_args()

    scratch = Path(args.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    fs = filesystem_of(scratch)
    if fs == "tmpfs" and not args.force:
        abort(f"{scratch} is on tmpfs: a segment read from page cache with no device\n"
              f"  under it is not a mount cost. Point --scratch elsewhere, or --force.")
    load1, load5, _ = os.getloadavg()
    binary = Path(args.binary)
    if not binary.exists():
        abort(f"no server at {binary}")

    run_dir = scratch / f"mount-{args.label}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    (run_dir / "wal").mkdir(parents=True)
    conf = run_dir / "kds.conf"
    conf.write_text("\n".join([
        f"data_file = {run_dir / 'kds.db'}",
        f"wal_dir = {run_dir / 'wal'}",
        f"port = {args.port}",
        f"durability = {args.durability}",
        "log_level = warn",
        f"log_dir = {run_dir}",
        "log_file = kds.log",
    ]) + "\n")
    server_log = run_dir / "stdout.log"

    rng = random.Random(args.seed)
    table = f"mount_{int(time.time())}_{rng.randrange(1 << 20)}"

    # ---- The create, plus the load. Not one of the measured mounts: it
    # formats a data file and creates the WAL segment, which no later mount
    # does.
    loader = Mount(binary, conf, server_log, args.port)
    create_s = loader.start()
    if args.rows:
        conn = loader.connect(timeout=120.0)
        # Under --assertion the relation carries the grouped column the
        # ceiling is declared over; without it the shape is unchanged, so a
        # run with no assertion measures exactly what it measured before.
        columns = ("id int64, grp int64, payload varchar" if args.assertion
                   else "id int64, payload varchar")
        reply = format_reply(conn.send_command(
            f"CREATE TABLE {table} ({columns}) BTREE"))
        if reply.startswith("ERR"):
            abort(f"CREATE TABLE failed: {reply}")
        if args.assertion:
            # Declared before the load, so the load's inserts go through the
            # reserve path and every row leaves its own entry - the walk at
            # mount then has --rows entries to attribute. A ceiling of 2x the
            # loosest bound can never trip.
            reply = format_reply(conn.send_command(
                f"CREATE ASSERTION {table}_lim ON {table} GROUP BY (grp) "
                f"CHECK COUNT(*) <= {2 * args.rows}"))
            if reply.startswith("ERR"):
                abort(f"CREATE ASSERTION failed: {reply}")
        for i in range(args.rows):
            text = "".join(rng.choices(ALPHABET, k=args.value_bytes))
            # One value, not two: the pk is ASSIGNED, so a row supplies the
            # columns after it (`docs/spec/heap-and-tuple.md` §4.1).
            values = (f"{i % args.assert_groups}, '{text}'" if args.assertion
                      else f"'{text}'")
            rolled = (args.assertion and args.assert_rollback_every
                      and i % args.assert_rollback_every == 0)
            if rolled:
                conn.send_command("BEGIN")
            r = conn.send_command(f"INSERT INTO {table} VALUES ({values})")
            if r.startswith("ERR"):
                abort(f"INSERT {i} failed: {r}")
            if rolled:
                r = conn.send_command("ROLLBACK")
                if r.startswith("ERR"):
                    abort(f"ROLLBACK {i} failed: {r}")
        if not args.crash:
            conn.send_command("SYNC")
        conn.close()
    loader.stop(crash=args.crash)

    # ---- The measured mounts, all on that one file.
    phase = Phase("mount", f"{args.mounts} mounts of one data file")
    rows = []
    for n in range(args.mounts):
        mount = Mount(binary, conf, server_log, args.port)
        wall = mount.start()
        conn = mount.connect(timeout=60.0)
        meta_line = format_reply(conn.send_command("SHOW META"))
        conn.close()
        mount.stop()
        counters = parse_meta(meta_line)
        phase.record(wall, "OK")
        rows.append({"mount": n + 1, "wall_ms": round(wall * 1e3, 2), **counters})

    # ---- Report.
    print()
    print(f"mount cost [{args.label}] - {args.rows} rows loaded, "
          f"{'SIGKILL' if args.crash else 'clean stop'} before the sweep, "
          f"durability {args.durability}")
    print(f"binary {binary} (mtime "
          f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(binary.stat().st_mtime))})")
    print(f"create+load mount: {create_s * 1e3:.1f} ms   load average "
          f"{load1:.2f} (1m) / {load5:.2f} (5m)   filesystem {fs}")
    print()
    header = (f"{'mount':>6}{'wall ms':>10}{'analysis':>10}{'redo':>9}{'highwtr':>9}"
              f"{'undo':>8}{'ckpt':>8}{'records':>9}{'redone':>8}{'healed':>8}")
    print(header)
    print("-" * len(header))
    for r in rows:
        def us(key):
            v = r.get(key)
            return f"{v / 1000:.1f}" if v is not None else "-"
        print(f"{r['mount']:>6}{r['wall_ms']:>10.2f}{us('recovery_analysis_us'):>10}"
              f"{us('recovery_redo_us'):>9}{us('recovery_high_water_us'):>9}"
              f"{us('recovery_undo_us'):>8}{us('recovery_checkpoint_us'):>8}"
              f"{str(r.get('recovery_records', '-')):>9}"
              f"{str(r.get('recovery_redo_applied', '-')):>8}"
              f"{str(r.get('recovery_pages_healed', '-')):>8}")
    print()
    s = phase.summary()
    print(f"{'':>6}{'mean':>10}{'p0':>10}{'p25':>9}{'p50':>9}{'p95':>8}{'p99':>8}{'max':>9}")
    print(f"{'ms':>6}{s['mean_us'] / 1000:>10.2f}{s['p0_us'] / 1000:>10.2f}"
          f"{s['p25_us'] / 1000:>9.2f}{s['p50_us'] / 1000:>9.2f}"
          f"{s['p95_us'] / 1000:>8.2f}{s['p99_us'] / 1000:>8.2f}{s['max_us'] / 1000:>9.2f}")
    print()
    print("  the ms columns after `wall` are SHOW META's own per-phase counters; `-` "
          "means this\n  server reports none, which is not the same as zero")
    print()

    if args.json:
        meta = {
            "label": args.label, "binary": str(binary), "rows": args.rows,
            "value_bytes": args.value_bytes, "crash": args.crash,
            "durability": args.durability, "mounts": args.mounts,
            "create_mount_ms": round(create_s * 1e3, 2),
            "filesystem": fs, "loadavg_1m": round(load1, 2), "loadavg_5m": round(load5, 2),
        }
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "mounts": rows, "phase": s}, f, indent=2)
        print(f"  wrote {args.json}")


if __name__ == "__main__":
    main()
