#!/usr/bin/env python3
"""BI's checkpoint-stall cell: what a checkpoint run does to its core's reactor.

`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` §5.1. One
cell is one fresh server under an insert load with a same-core probe on every
core, classed against the server's own per-run log lines.

* **Load.** `--sessions` processes (4), each a loop of 1024-row named-key
  `INSERT`s into one btree, each its own key range. Processes, not threads:
  the probe's numbers must not carry the load's GIL.
* **Probe, open loop.** One statement session and one `C_PING` session per
  core, found by opening connections until `SHOW META`'s `core=` covers every
  core. Each sends on a fixed schedule (`--probe-ms`, 20) and counts latency
  from the *scheduled* time, so a stall of S seconds is S/interval samples,
  not one.
* **Classing.** The server logs one `checkpoint run:` line per run with its
  core and its CLOCK_MONOTONIC start and end; a sample is *inside* when its
  scheduled time falls in one of its own core's runs. `time.monotonic()` is
  the same clock.
* **After.** `SHOW META` on each probe session (its core's reactor holds and
  last run), then a stop, then the log is parsed.

Writes one JSON file per cell; `--summarize` turns a set of them into the
tables §5.1 asks for.

Usage:
    tools/bi_checkpoint_stall.py --server build-release/kds_server \\
        --workdir ~/bi-bench --cores 4 --durability relaxed --seconds 600 \\
        --out cell.json
    tools/bi_checkpoint_stall.py --summarize cells/*.json
"""

import argparse
import json
import math
import multiprocessing
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ckdbs_cli import ServerConnection  # noqa: E402

ROWS_PER_INSERT = 1024
PROBE_ROWS = 1000
PAD = "x" * 40


def wait_for_port(port, stderr_path, deadline_s=60):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    with open(stderr_path) as f:
        tail = "".join(f.readlines()[-5:])
    raise TimeoutError(f"server did not listen on {port}: {tail}")


def start_server(a, tag):
    conf = os.path.join(a.workdir, f"{tag}.conf")
    data = os.path.join(a.workdir, f"{tag}.db")
    for stale in (data, data + ".wal"):
        if os.path.isdir(stale):
            shutil.rmtree(stale)
        elif os.path.exists(stale):
            os.remove(stale)
    cpus = ",".join(str(c) for c in range(a.cores))
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {a.port}\ncores = {a.cores}\n"
                f"reactor_cpus = {cpus}\n"
                f"durability = {a.durability}\n"
                f"checkpoint_interval_ms = {a.checkpoint_ms}\n"
                f"buffer_pool_frames = {a.frames}\n"
                f"log_dir = {a.workdir}\nlog_file = {tag}.log\nlog_level = info\n")
    log = os.path.join(a.workdir, f"{tag}.log")
    if os.path.exists(log):
        os.remove(log)
    stderr_path = os.path.join(a.workdir, f"{tag}.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([a.server, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
    wait_for_port(a.port, stderr_path)
    return proc, log


def meta_fields(reply):
    out = {}
    for tok in reply.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def check(reply, what):
    if reply.startswith("ERR"):
        raise RuntimeError(f"{what}: {reply}")
    return reply


# ---- load ------------------------------------------------------------------

def load_worker(port, session, stop_at, out_q):
    conn = ServerConnection("127.0.0.1", port, timeout=600)
    base = (session + 1) * 1_000_000_000
    k = 0
    lat = []
    while time.monotonic() < stop_at:
        rows = ",".join(f"({base + k + i}, {k + i}, '{PAD}')" for i in range(ROWS_PER_INSERT))
        t = time.monotonic()
        check(conn.send_command(f"INSERT INTO load VALUES {rows}"), "insert")
        lat.append(time.monotonic() - t)
        k += ROWS_PER_INSERT
    conn.close()
    out_q.put({"session": session, "rows": k, "statements": len(lat),
               "p50_ms": 1000 * statistics.median(lat) if lat else None,
               "max_ms": 1000 * max(lat) if lat else None})


# ---- probe -----------------------------------------------------------------

def connections_per_core(port, cores, limit=200):
    """One connection per core, by opening until `core=` covers them all."""
    found, spare = {}, []
    for _ in range(limit):
        c = ServerConnection("127.0.0.1", port, timeout=600)
        core = meta_fields(c.send_command("SHOW META"))["core"]
        if core in found:
            spare.append(c)
        else:
            found[core] = c
        if len(found) == cores:
            break
    for c in spare:
        c.close()
    if len(found) != cores:
        raise RuntimeError(f"only reached cores {sorted(found)} in {limit} connections")
    return found


def probe_loop(kind, conn, core, interval, stop_at, samples, start_at):
    n = 0
    while True:
        sched_t = start_at + n * interval
        if sched_t >= stop_at:
            break
        now = time.monotonic()
        if sched_t > now:
            time.sleep(sched_t - now)
        try:
            if kind == "stmt":
                conn.send_command(f"SELECT * FROM probe WHERE id = {1 + n % PROBE_ROWS}")
            else:
                conn._conn.ping()  # C_PING: answered in the frame loop
            done = time.monotonic()
            samples.append((sched_t, (done - sched_t) * 1000.0))
        except Exception as e:  # recorded, and this probe stops
            samples.append((sched_t, -1.0))
            print(f"probe {kind} core {core}: {e}", file=sys.stderr)
            return
        n += 1


# ---- the log ---------------------------------------------------------------

RUN_RE = re.compile(r"run: core=(\d+) completed=(\d) start_ns=(\d+) end_ns=(\d+) (.*)")


def parse_runs(log_path):
    runs = []
    with open(log_path, errors="replace") as f:
        for line in f:
            m = RUN_RE.search(line)
            if not m:
                continue
            rest = dict(kv.split("=", 1) for kv in m.group(5).split() if "=" in kv)
            runs.append({"core": int(m.group(1)), "completed": m.group(2) == "1",
                         "start": int(m.group(3)) / 1e9, "end": int(m.group(4)) / 1e9,
                         **{k: int(v) for k, v in rest.items()}})
    return runs


def host_state(workdir):
    def read(p):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            return None
    df = subprocess.run(["df", "-T", workdir], capture_output=True, text=True).stdout
    return {"loadavg": read("/proc/loadavg"), "pressure_io": read("/proc/pressure/io"),
            "df": df.strip()}


def run_cell(a):
    os.makedirs(a.workdir, exist_ok=True)
    tag = a.tag or f"bi-c{a.cores}-{a.durability}"
    host_before = host_state(a.workdir)
    proc, log = start_server(a, tag)
    try:
        setup = ServerConnection("127.0.0.1", a.port, timeout=60)
        check(setup.send_command("CREATE TABLE load (id int64, n int64, pad varchar) BTREE"), "create")
        check(setup.send_command("CREATE TABLE probe (id int64, n int64, pad varchar) BTREE"), "create")
        for lo in range(0, PROBE_ROWS, 500):
            rows = ",".join(f"({i + 1}, {i}, '{PAD}')" for i in range(lo, min(lo + 500, PROBE_ROWS)))
            check(setup.send_command(f"INSERT INTO probe VALUES {rows}"), "probe fill")
        setup.close()

        stmt = connections_per_core(a.port, a.cores)
        pings = connections_per_core(a.port, a.cores)
        for c in stmt.values():  # warm the probe relation on every core
            for i in range(1, PROBE_ROWS + 1, 7):
                c.send_command(f"SELECT * FROM probe WHERE id = {i}")

        start_at = time.monotonic() + 1.0
        stop_at = start_at + a.seconds
        q = multiprocessing.Queue()
        loaders = [multiprocessing.Process(target=load_worker, args=(a.port, s, stop_at, q))
                   for s in range(a.sessions)]
        for p in loaders:
            p.start()
        samples = {("stmt", c): [] for c in stmt} | {("ping", c): [] for c in pings}
        threads = []
        for core, c in stmt.items():
            threads.append(threading.Thread(target=probe_loop, args=(
                "stmt", c, core, a.probe_ms / 1000, stop_at, samples[("stmt", core)], start_at)))
        for core, c in pings.items():
            threads.append(threading.Thread(target=probe_loop, args=(
                "ping", c, core, a.probe_ms / 1000, stop_at, samples[("ping", core)], start_at)))
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        load = [q.get(timeout=600) for _ in loaders]
        for p in loaders:
            p.join()

        meta = {}
        for core, c in stmt.items():
            try:
                meta[core] = meta_fields(c.send_command("SHOW META"))
            except Exception as e:  # a dead probe loses its core's META, not the cell
                print(f"SHOW META on core {core}: {e}", file=sys.stderr)
                meta[core] = {}
        for c in list(stmt.values()):
            c.close()
        for c in list(pings.values()):
            c.close()
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=600)
        except subprocess.TimeoutExpired:
            proc.kill()
    runs = parse_runs(log)
    cell = {"tag": tag, "cores": a.cores, "durability": a.durability, "seconds": a.seconds,
            "frames": a.frames, "checkpoint_ms": a.checkpoint_ms, "probe_ms": a.probe_ms,
            "window": [start_at, stop_at], "load": load, "runs": runs,
            "meta": {str(k): v for k, v in meta.items()},
            "samples": {f"{k}:{c}": v for (k, c), v in samples.items()},
            "host_before": host_before, "host_after": host_state(a.workdir),
            "server": a.server}
    with open(a.out, "w") as f:
        json.dump(cell, f)
    print(f"{tag}: {len(runs)} runs, {sum(l['rows'] for l in load)} rows -> {a.out}")


# ---- summary ---------------------------------------------------------------

def pct(xs, p):
    """Nearest rank: the smallest sample with at least p% of samples at or below it."""
    if not xs:
        return None
    xs = sorted(xs)
    rank = math.ceil(round(p * len(xs) / 100, 9))  # the round() keeps 99.9 * 1000 from reading 999.0000001
    return xs[min(len(xs), max(1, rank)) - 1]


def fmt(v, digits=1):
    return "-" if v is None else (f"{v:.{digits}f}" if isinstance(v, float) else str(v))


def phases_of(runs):
    """The phase names, read off the server's own run lines - never a copy
    of `kCheckpointPhaseNames` that could drift from it."""
    names = []
    for r in runs:
        for k in r:
            if k.endswith("_sum_us") and k[:-7] not in names:
                names.append(k[:-7])
    return names


def summarize_cell(cell):
    """Per core: the run record, the reactor holds, and the probe inside/outside."""
    lo, hi = cell["window"]
    phases = phases_of(cell["runs"])
    out = []
    for core in sorted(int(c) for c in cell["meta"]):
        runs = [r for r in cell["runs"] if r["core"] == core and r["end"] >= lo and r["start"] <= hi]
        spans = [(r["start"], r["end"]) for r in runs]
        def inside(t):
            return any(s <= t <= e for s, e in spans)
        row = {"core": core, "runs": len(runs),
               "failed": sum(1 for r in runs if not r["completed"]),
               "start_refused": (cell["meta"][str(core)] or {}).get("checkpoint_start_refused"),
               "steps_p50": pct([r["steps"] for r in runs], 50),
               "pages_p50": pct([r["pages"] for r in runs], 50),
               "pages_max": max((r["pages"] for r in runs), default=None),
               "wall_ms_p50": (pct([r["wall_us"] for r in runs], 50) or 0) / 1000 if runs else None,
               "wall_ms_max": max((r["wall_us"] for r in runs), default=0) / 1000 if runs else None}
        row["step_max_ms"] = max((r.get("step_max_us", 0) for r in runs), default=0) / 1000
        row["step_page_write_max_ms"] = max((r.get("step_page_write_max_us", 0) for r in runs),
                                            default=0) / 1000
        row["phases"] = phases
        for ph in phases:
            row[f"{ph}_sum_ms"] = sum(r.get(f"{ph}_sum_us", 0) for r in runs) / 1000
            row[f"{ph}_max_ms"] = max((r.get(f"{ph}_max_us", 0) for r in runs), default=0) / 1000
        m = cell["meta"][str(core)] or {}
        for where in ("in_run", "out_run"):
            row[f"hold_{where}_longest_ms"] = m.get(f"sched_hold_{where}_longest_us", 0) / 1000
            for b in ("10ms", "100ms", "1s"):
                row[f"hold_{where}_over_{b}"] = m.get(f"sched_hold_{where}_over_{b}")
        polled = sum(m.get(f"sched_{g}_polled_us", 0) for g in ("foreground", "maintenance", "system"))
        row["charged_to_no_group_ms"] = (m.get("sched_wall_us", 0) - polled -
                                         m.get("sched_idle_block_us", 0)) / 1000
        row["system_polled_ms"] = m.get("sched_system_polled_us", 0) / 1000
        for kind in ("stmt", "ping"):
            ss = cell["samples"].get(f"{kind}:{core}", [])
            errs = sum(1 for _, ms in ss if ms < 0)
            ins = [ms for t, ms in ss if ms >= 0 and inside(t)]
            outs = [ms for t, ms in ss if ms >= 0 and not inside(t)]
            row[f"{kind}_errors"] = errs
            for name, xs in (("in", ins), ("out", outs)):
                row[f"{kind}_{name}_n"] = len(xs)
                for p in (50, 99, 99.9):
                    row[f"{kind}_{name}_p{p}"] = pct(xs, p)
                row[f"{kind}_{name}_max"] = max(xs) if xs else None
        ends = sorted(r["end"] for r in runs if r["completed"] and lo <= r["end"] <= hi)
        gaps = [b - a for a, b in zip(ends, ends[1:])]
        row["anchor_gap_s_max"] = max(gaps) if gaps else None
        out.append(row)
    ends = sorted(r["end"] for r in cell["runs"] if r["completed"] and lo <= r["end"] <= hi)
    gaps = [b - a for a, b in zip(ends, ends[1:])]
    return out, (max(gaps) if gaps else None)


def summarize(paths):
    for path in paths:
        with open(path) as f:
            cell = json.load(f)
        rows, instance_gap = summarize_cell(cell)
        rows_total = sum(l["rows"] for l in cell["load"])
        print(f"\n### {os.path.basename(path)}: cores={cell['cores']} "
              f"durability={cell['durability']} {cell['seconds']} s, "
              f"{rows_total} rows inserted, instance anchor gap max "
              f"{fmt(instance_gap, 2)} s")
        print(f"host before: load {cell['host_before']['loadavg']}; "
              f"io pressure {cell['host_before']['pressure_io'].splitlines()[0] if cell['host_before']['pressure_io'] else '-'}")
        print("\n| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | "
              "longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | "
              "anchor gap max s | no-group ms |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in rows:
            print(f"| {r['core']} | {r['runs']} | {r['failed']} | {fmt(r['start_refused'])} | "
                  f"{fmt(r['steps_p50'])} | {fmt(r['pages_p50'])}/{fmt(r['pages_max'])} | "
                  f"{fmt(r['wall_ms_p50'])}/{fmt(r['wall_ms_max'])} | "
                  f"{fmt(r['hold_in_run_longest_ms'])}/{fmt(r['hold_out_run_longest_ms'])} | "
                  f"{fmt(r['hold_in_run_over_10ms'])}/{fmt(r['hold_out_run_over_10ms'])} | "
                  f"{fmt(r['hold_in_run_over_100ms'])}/{fmt(r['hold_out_run_over_100ms'])} | "
                  f"{fmt(r['hold_in_run_over_1s'])}/{fmt(r['hold_out_run_over_1s'])} | "
                  f"{fmt(r['anchor_gap_s_max'], 2)} | {fmt(r['charged_to_no_group_ms'])} |")
        print("\n| core | phase sums ms (max one occurrence ms) |")
        print("|---|---|")
        for r in rows:
            parts = [f"{ph} {r[f'{ph}_sum_ms']:.0f} ({r[f'{ph}_max_ms']:.1f})" for ph in r["phases"]]
            parts.append(f"one step max {r['step_max_ms']:.1f}, its page writes max "
                         f"{r['step_page_write_max_ms']:.1f}")
            print(f"| {r['core']} | " + "; ".join(parts) + " |")
        print("\n| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | "
              "max in/out ms | errors |")
        print("|---|---|---|---|---|---|---|---|")
        for r in rows:
            for kind in ("stmt", "ping"):
                k = kind
                print(f"| {r['core']} | {kind} | {r[f'{k}_in_n']}/{r[f'{k}_out_n']} | "
                      f"{fmt(r[f'{k}_in_p50'], 2)}/{fmt(r[f'{k}_out_p50'], 2)} | "
                      f"{fmt(r[f'{k}_in_p99'], 2)}/{fmt(r[f'{k}_out_p99'], 2)} | "
                      f"{fmt(r[f'{k}_in_p99.9'], 1)}/{fmt(r[f'{k}_out_p99.9'], 1)} | "
                      f"{fmt(r[f'{k}_in_max'], 1)}/{fmt(r[f'{k}_out_max'], 1)} | "
                      f"{r[f'{k}_errors']} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server")
    ap.add_argument("--workdir")
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--durability", default="relaxed")
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--sessions", type=int, default=4)
    ap.add_argument("--port", type=int, default=25480)
    ap.add_argument("--frames", type=int, default=786432)
    ap.add_argument("--checkpoint-ms", type=int, default=5000)
    ap.add_argument("--probe-ms", type=float, default=20)
    ap.add_argument("--tag")
    ap.add_argument("--out")
    ap.add_argument("--summarize", nargs="+")
    a = ap.parse_args()
    if a.summarize:
        summarize(a.summarize)
        return
    if not (a.server and a.workdir and a.out):
        ap.error("--server, --workdir and --out are required for a cell")
    run_cell(a)


if __name__ == "__main__":
    main()
