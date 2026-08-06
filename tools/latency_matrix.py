#!/usr/bin/env python3
"""The latency matrix: one workload, several engine configurations, one table.

W0 of the p50-p99 plan, and it exists because of a specific mistake. The
in-process benchmark (`bench/txn_layers_bench.cpp`) runs the four statements
against a `CommandDispatcher` with **no reactor, no checkpointer and one
connection**, so it cannot see either of the two things that actually cause
ckdbs's tail - a checkpoint step forcing a WAL fsync on the statement thread,
and every commit performing its own drain. It blamed the storage device
instead, which was wrong.

So this drives the **server**, over a socket, the way a client does, and it
varies exactly the configuration knobs that were shown to matter:

    durability = group | relaxed | strict     the commit path
    checkpoint_interval_ms                    the maintenance path

and the trader count, because a group committer that cannot batch is
invisible at one connection - which is the whole of W1.

Each configuration gets a **fresh data file**: catalog rows are never
reclaimed (there is no DROP TABLE) and a warm relation is not the same
measurement as a cold one.

---- The two ways this measurement goes wrong -----------------------------

Both have already happened here, so both are refused rather than documented:

1. **tmpfs.** fsync costs ~0.3 us there and every durability class measures
   the same. The scratch directory's filesystem is checked, and a run on
   tmpfs aborts.
2. **A busy host**, checked on *two* averages. These runs were once taken on
   a 2-core box at load average 3.2, which produced 14 ms outliers with no
   engine work behind them - so the one-minute average is checked. Then a
   run was admitted at a one-minute average of 0.73 while the five-minute
   was 4.77, and PostgreSQL, measured as the control in the same run, came
   out 17-26% worse on p99 than the run before it: a one-minute figure dips
   between bursts, and a box still draining sustained load reads as quiet
   for exactly long enough. Both are checked, both are recorded in the
   output and the JSON, and `--force` overrides them.

Usage:

    tools/latency_matrix.py                       # the default matrix
    tools/latency_matrix.py --traders 1,4         # the W1 case
    tools/latency_matrix.py --configs group,relaxed-nockpt
    tools/latency_matrix.py --pg                  # add a PostgreSQL row
    tools/latency_matrix.py --json before.json    # then diff after a change
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The phases worth comparing. `txn` is the business transaction - four
# statements - and the other two are its halves; a change that moves one and
# not the others is a change that needs explaining.
PHASES = ("txn", "trade-insert", "account-update")

# name -> extra kds.conf lines. The first is what the server ships with, and
# every other row exists to answer "how much of the tail is this component".
CONFIGS = {
    "group": [],
    "group-nockpt": ["checkpoint_interval_ms = 3600000"],
    "relaxed": ["durability = relaxed"],
    "relaxed-nockpt": ["durability = relaxed", "checkpoint_interval_ms = 3600000"],
    "strict": ["durability = strict"],
}

DEFAULT_CONFIGS = "group,group-nockpt,relaxed,relaxed-nockpt"

# A 2-core box building the tree while measuring it is not a measurement.
# One runnable process per core is already generous.
MAX_LOAD_PER_CORE = 0.6

# **And the five-minute average, which is the check that was missing.** A
# one-minute average dips between bursts, so a box still draining sustained
# load reads as quiet for long enough to admit a run. That happened: a matrix
# was admitted at a one-minute average of 0.73 while the five- and
# fifteen-minute averages were 4.77 and 5.93, and PostgreSQL - unchanged code
# in a separate process, measured as the control - came out 17-26% worse on
# p99 than in the run before it. Every ckdbs number in that run was
# unattributable.
#
# Looser per core than the one-minute bound because it is a *trailing*
# figure: a machine that was busy four minutes ago and is idle now should be
# allowed to measure, and one that is still working should not.
MAX_LOAD5_PER_CORE = 1.0


def abort(message):
    print(f"latency-matrix aborted: {message}", file=sys.stderr)
    sys.exit(1)


def filesystem_of(path):
    """The filesystem type backing `path`, or None when it cannot be told."""
    out = subprocess.run(["df", "-T", str(path)], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    lines = out.stdout.strip().splitlines()
    if len(lines) < 2:
        return None
    return lines[-1].split()[1]


def check_host(scratch, force):
    fs = filesystem_of(scratch)
    if fs == "tmpfs" and not force:
        abort(f"{scratch} is on tmpfs, where fsync is free and every durability class\n"
              f"  measures the same. Point --scratch at a real device (df -T tells you\n"
              f"  which), or pass --force to measure something else on purpose.")

    load1, load5, _ = os.getloadavg()
    cores = os.cpu_count() or 1
    if load1 > MAX_LOAD_PER_CORE * cores and not force:
        abort(f"load average is {load1:.2f} on {cores} core(s): a statement preempted by\n"
              f"  another process produces outliers with no engine work behind them, and\n"
              f"  this run would attribute them to the engine. Wait for the box to go\n"
              f"  quiet, or pass --force.")
    if load5 > MAX_LOAD5_PER_CORE * cores and not force:
        abort(f"the 5-minute load average is {load5:.2f} on {cores} core(s) (1-minute is\n"
              f"  {load1:.2f}): the box is still draining sustained load, and a one-minute\n"
              f"  average dips between bursts long enough to look quiet. A run admitted\n"
              f"  this way once produced a control - PostgreSQL, unchanged - that was\n"
              f"  17-26% worse than the run before it. Wait, or pass --force.")
    return {"filesystem": fs, "loadavg_1m": round(load1, 2), "loadavg_5m": round(load5, 2),
            "cores": cores}


def git_commit():
    out = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else "unknown"


def find_server(explicit):
    if explicit:
        return Path(explicit)
    # Release first: a Debug server measures the compiler, not the engine.
    for candidate in ("build-release/kds_server", "build/kds_server"):
        path = REPO / candidate
        if path.exists():
            if candidate.startswith("build/"):
                print("  note: using the Debug build; numbers from it are not comparable "
                      "to a release run", flush=True)
            return path
    abort("no kds_server found; build one with\n"
          "  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && "
          "cmake --build build-release -j")


class Server:
    """One kds_server on its own fresh data file, stopped on the way out."""

    def __init__(self, binary, scratch, name, port, extra_lines):
        self.dir = Path(scratch) / f"matrix-{name}"
        if self.dir.exists():
            shutil.rmtree(self.dir)
        (self.dir / "wal").mkdir(parents=True)
        self.log = self.dir / "stdout.log"
        conf = self.dir / "kds.conf"
        lines = [
            f"data_file = {self.dir / 'kds.db'}",
            f"wal_dir = {self.dir / 'wal'}",
            f"port = {port}",
            # Warn, not info: a per-statement debug line is a write per
            # statement, which is the thing being measured.
            "log_level = warn",
            f"log_dir = {self.dir}",
            "log_file = kds.log",
        ] + extra_lines
        conf.write_text("\n".join(lines) + "\n")
        self.binary, self.conf, self.port = binary, conf, port
        self.proc = None

    def __enter__(self):
        with open(self.log, "w") as out:
            self.proc = subprocess.Popen([str(self.binary), "--config", str(self.conf)],
                                         stdout=out, stderr=subprocess.STDOUT)
        deadline = time.time() + 20.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                abort(f"the server exited during startup:\n{self.log.read_text()[-600:]}")
            if "listening on" in self.log.read_text():
                return self
            time.sleep(0.1)
        abort(f"the server did not come up within 20s:\n{self.log.read_text()[-600:]}")

    def __exit__(self, *_):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
        return False


def run_stress(script, args, json_path, extra):
    command = [sys.executable, str(REPO / "tools" / script),
               "--users", str(args.users), "--assets", str(args.assets),
               "--seconds", str(args.seconds), "--traders", str(args.traders_current),
               "--verify", "0", "--no-profit", "--seed", str(args.seed),
               "--json", str(json_path)] + extra
    out = subprocess.run(command, capture_output=True, text=True, cwd=str(REPO / "tools"))
    if out.returncode != 0 or not json_path.exists():
        abort(f"{script} failed:\n{out.stdout[-1500:]}\n{out.stderr[-800:]}")
    return json.loads(json_path.read_text())


def row_from(result, label):
    phases = {p["phase"]: p for p in result["phases"]}
    row = {"config": label, "tps": result["meta"].get("tps", 0.0)}
    for name in PHASES:
        p = phases.get(name)
        if p is None:
            continue
        row[name] = {"p50": p["p50_us"], "p95": p["p95_us"], "p99": p["p99_us"],
                     "max": p["max_us"],
                     "ratio": round(p["p99_us"] / p["p50_us"], 1) if p["p50_us"] else 0.0}
    return row


def print_table(rows, traders):
    print()
    print(f"  latency matrix - {traders} trader connection(s), microseconds")
    print("  " + "-" * 96)
    print(f"  {'configuration':<18}{'TPS':>9}   " +
          "".join(f"{name:>28}" for name in PHASES))
    print(f"  {'':<18}{'':>9}   " +
          "".join(f"{'p50':>7}{'p95':>7}{'p99':>7}{'p99/p50':>7}" for _ in PHASES))
    for row in rows:
        line = f"  {row['config']:<18}{row['tps']:>9,.1f}   "
        for name in PHASES:
            cell = row.get(name)
            if cell is None:
                line += f"{'-':>28}"
                continue
            line += (f"{cell['p50']:>7.0f}{cell['p95']:>7.0f}{cell['p99']:>7.0f}"
                     f"{cell['ratio']:>6.1f}x")
        print(line)
    print()
    print("  p99/p50 is the number this plan is about: PostgreSQL answers ~1.3x on this")
    print("  workload, and a configuration far above that is spending its tail somewhere")
    print("  the median does not show.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scratch", default=str(Path.home() / "kds-bench-scratch"),
                        help="where data files go; must not be tmpfs "
                             "(default: ~/kds-bench-scratch)")
    parser.add_argument("--server", default=None,
                        help="kds_server binary (default: build-release, then build)")
    parser.add_argument("--configs", default=DEFAULT_CONFIGS,
                        help=f"comma-separated, from {','.join(CONFIGS)} "
                             f"(default: {DEFAULT_CONFIGS})")
    parser.add_argument("--traders", default="1",
                        help="comma-separated connection counts; the matrix runs once per "
                             "value (default: 1). Use 1,4 for the group-commit case")
    parser.add_argument("--users", type=int, default=300)
    parser.add_argument("--assets", type=int, default=100)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--port-base", type=int, default=15700)
    parser.add_argument("--pg", action="store_true",
                        help="also run PostgreSQL as the reference row; needs a cluster "
                             "(tools/pg_setup.sh init)")
    parser.add_argument("--pg-port", type=int, default=15433)
    parser.add_argument("--json", metavar="PATH", help="write every result for later diffing")
    parser.add_argument("--force", action="store_true",
                        help="run despite tmpfs or a busy host, and say so in the output")
    args = parser.parse_args()

    names = [c.strip() for c in args.configs.split(",") if c.strip()]
    for name in names:
        if name not in CONFIGS:
            abort(f"unknown configuration '{name}'; known: {', '.join(CONFIGS)}")
    trader_counts = [int(t) for t in args.traders.split(",") if t.strip()]

    scratch = Path(args.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    host = check_host(scratch, args.force)
    binary = find_server(args.server)

    print(f"latency matrix @ {git_commit()}")
    print(f"  server     {binary}")
    print(f"  scratch    {scratch}  ({host['filesystem']})")
    print(f"  host       {host['cores']} core(s), load {host['loadavg_1m']} (1m) / "
          f"{host['loadavg_5m']} (5m)" + ("  [--force]" if args.force else ""))
    print(f"  workload   users={args.users} assets={args.assets} "
          f"seconds={args.seconds:g} reporter=off")

    everything = {"commit": git_commit(), "host": host, "runs": []}
    port = args.port_base

    for traders in trader_counts:
        args.traders_current = traders
        rows = []
        for name in names:
            print(f"\n  running {name} at {traders} connection(s)...", flush=True)
            port += 1
            with Server(binary, scratch, f"{name}-{traders}", port, CONFIGS[name]):
                result = run_stress("scenario0_stockmarket.py", args,
                                    scratch / f"matrix-{name}-{traders}.json",
                                    ["--port", str(port)])
            rows.append(row_from(result, name))
            everything["runs"].append({"engine": "ckdbs", "config": name,
                                       "traders": traders, "result": rows[-1]})

        if args.pg:
            print(f"\n  running postgresql at {traders} connection(s)...", flush=True)
            result = run_stress("pg_scenario0_stockmarket.py", args,
                                scratch / f"matrix-pg-{traders}.json",
                                ["--port", str(args.pg_port),
                                 "--synchronous-commit", "on"])
            rows.append(row_from(result, "postgresql"))
            everything["runs"].append({"engine": "postgresql", "config": "sync_commit=on",
                                       "traders": traders, "result": rows[-1]})

        print_table(rows, traders)

    if args.json:
        Path(args.json).write_text(json.dumps(everything, indent=2))
        print(f"  wrote {args.json}")


if __name__ == "__main__":
    main()
