#!/usr/bin/env python3
"""The >=3-writer-core matrix: drive `tools/multicore_benchmark.py` over the
cells the run's instructions name, repeat each, sample per-core CPU while it
runs, and archive every raw driver stdout beside a machine-readable summary.

PW6 could price the peer write path but never measure speedup: its host had
2 CPUs, `cores` cannot exceed `hardware_concurrency()`, and at `cores = 2`
rotation puts every relation on core 1 - one writer core. This orchestrator
is for a host with >= 3 writer cores, where `AssignOwnerCore`'s rotation over
the non-system cores (include/kds/catalog/core_placement.hpp:96-104) finally
has more than one peer to rotate over.

It decides nothing. It runs the cells, records what came back, and leaves
every reading to the results document.

Cells (`--cells` takes a comma-separated subset):

  H1  cores=4 tables=6 rows=2000   rotate + peer-listeners   the headline
  H2  H1 at rows=20000                                       fixed costs amortised
  H3  H1 at tables=3                                         one relation per writer core
  H4a cores=2 tables=2                                       curve point, 1 writer core
  H4b cores=3 tables=4                                       curve point, 2 writer cores
  C1  cores=4 tables=6 placement=creating, no listeners      the control

`--tables` per cell is two relations per writer core at every curve point,
which is what keeps the three H4/H1 points comparable: the run instructions
say `3 * (cores - 1)` "or nearest", but a curve whose per-core load changes
between points measures load as well as cores. Every cell's table count is
still a multiple of `cores - 1`, which is the requirement that matters -
otherwise one writer core carries an extra relation and the imbalance reads
as poor scaling.

Usage:
    bench/run_benchv2.py --server build-release/kds_server \
        --workdir ~/mcbench --archive bench/v2.0.0/archive/<name> --reps 5
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

# Two relations per writer core at every point; see the module docstring.
CELLS = {
    "H1":  dict(cores=4, tables=6, rows=2000,  placement="rotate",   listeners=True),
    "H2":  dict(cores=4, tables=6, rows=20000, placement="rotate",   listeners=True),
    "H3":  dict(cores=4, tables=3, rows=2000,  placement="rotate",   listeners=True),
    "H4a": dict(cores=2, tables=2, rows=2000,  placement="rotate",   listeners=True),
    "H4b": dict(cores=3, tables=4, rows=2000,  placement="rotate",   listeners=True),
    "C1":  dict(cores=4, tables=6, rows=2000,  placement="creating", listeners=False),
    # The H3-shaped control: same 4-core server, same 3 relations, but every
    # relation on core 0. Without it H3's ratio cannot be attributed to
    # rotation rather than to running a 4-core server at all - C1 showed the
    # 6-relation control is not at parity.
    "C2":  dict(cores=4, tables=3, rows=2000,  placement="creating", listeners=False),
    # PW7's own shape: four writing sessions on ONE peer core, which is the
    # condition its lease-refill collapse needed (docs/inflight/in-progress/workplan-peer-writer.md:325,
    # "four writers on core 1"). H1 spreads two sessions over three cores and
    # does not provoke it, so a before/after on H1 tests nothing.
    "P7":  dict(cores=2, tables=4, rows=2000,  placement="rotate",   listeners=True),
}

PHASES = ("insert", "point-select", "update", "delete", "scan")


# ---- per-core CPU sampling -------------------------------------------------

def read_cpu_jiffies():
    """Per-cpu (user, nice, system, idle, iowait, irq, softirq, steal) from
    /proc/stat, keyed `cpu0`..`cpuN`. Busy is everything but idle+iowait -
    which counts `steal` as busy. On a virtualised host that is time the
    hypervisor took, not work this engine did, so a high busy fraction with
    a low throughput is worth checking against `steal` before it is read as
    the engine being busy. guest/guest_nice are excluded because the kernel
    already counts them inside user/nice."""
    out = {}
    with open("/proc/stat") as fh:
        for line in fh:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            out[parts[0]] = [int(v) for v in parts[1:9]]
    return out


def cpu_busy_fraction(before, after):
    """Busy fraction per cpu between two /proc/stat samples."""
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        delta = [x - y for x, y in zip(a, b)]
        total = sum(delta)
        idle = delta[3] + delta[4]  # idle + iowait
        out[cpu] = (total - idle) / total if total else 0.0
    return out


class CpuSampler(threading.Thread):
    """Samples per-cpu busy fraction on an interval for the life of a run, so
    a cell can be read as `which cores actually did work` and not only as a
    throughput number."""

    def __init__(self, interval=0.5):
        super().__init__(daemon=True)
        self.interval = interval
        self.samples = []
        self._stop = threading.Event()

    def run(self):
        prev = read_cpu_jiffies()
        while not self._stop.wait(self.interval):
            cur = read_cpu_jiffies()
            self.samples.append((time.time(), cpu_busy_fraction(prev, cur)))
            prev = cur

    def stop(self):
        self._stop.set()
        self.join(timeout=5)

    def summary(self):
        """Mean and peak busy fraction per cpu over the sampled window."""
        if not self.samples:
            return {}
        cpus = sorted(self.samples[0][1], key=lambda c: int(c[3:]))
        out = {}
        for cpu in cpus:
            vals = [s[1].get(cpu, 0.0) for s in self.samples]
            out[cpu] = dict(mean=sum(vals) / len(vals), peak=max(vals),
                            samples=len(vals))
        return out


# ---- parsing the driver's stdout -------------------------------------------

CONFIG_RE = re.compile(r"^== (single-core|multi-core): cores=(\d+)")
PHASE_RE = re.compile(r"^\s+(\S+)\s+n=\s*(\d+)\s+p50=\s*(\d+)us\s+p99=\s*(\d+)us")
WALL_RE = re.compile(r"^\s+wall=([\d.]+)s\s+aggregate=([\d,]+) stmt/s\s+errors=(\d+)")
RATIO_RE = re.compile(r"multi-core / single-core throughput: ([\d.]+)x")
VERIFY_RE = re.compile(r"^\s+verify: (.*)$")
PLACEMENT_RE = re.compile(r"^\s+placement: (.*)$")
# The driver prints one `refills:` line carrying every core, joined by "; "
# (tools/multicore_benchmark.py:423) - not one line per core. The per-core
# shape this used to match never appears, so every refill number was being
# dropped: the PW7 instrument read as absent on a cell that reported it.
REFILL_RE = re.compile(r"^\s+refills: (.*)$")
RETRY_RE = re.compile(r"^\s+(retries: .*)$")
FIRSTERR_RE = re.compile(r"^\s+first error: (.*)$")
NOTRUN_RE = re.compile(r"^\s+NOT RUN")


def parse_driver_output(text):
    """The comparison table back as data. Absence is recorded, never guessed:
    a config that did not run keeps `not_run` and no phase numbers."""
    result = {"configs": {}, "ratio": None, "raw_lines": len(text.splitlines())}
    cur = None
    for line in text.splitlines():
        m = CONFIG_RE.match(line)
        if m:
            cur = m.group(1)
            result["configs"][cur] = dict(cores=int(m.group(2)), phases={},
                                          verify=None, placement=None,
                                          refills=None, retries=None,
                                          first_error=None, not_run=False)
            continue
        if cur is None:
            continue
        cfg = result["configs"][cur]
        if NOTRUN_RE.match(line):
            cfg["not_run"] = True
            continue
        m = PLACEMENT_RE.match(line)
        if m:
            cfg["placement"] = m.group(1)
            continue
        m = VERIFY_RE.match(line)
        if m:
            cfg["verify"] = m.group(1)
            continue
        m = REFILL_RE.match(line)
        if m:
            cfg["refills"] = m.group(1)
            continue
        m = RETRY_RE.match(line)
        if m:
            cfg["retries"] = m.group(1)
            continue
        m = FIRSTERR_RE.match(line)
        if m:
            cfg["first_error"] = m.group(1)
            continue
        m = WALL_RE.match(line)
        if m:
            cfg["wall_s"] = float(m.group(1))
            cfg["aggregate_stmt_s"] = float(m.group(2).replace(",", ""))
            cfg["errors"] = int(m.group(3))
            continue
        m = PHASE_RE.match(line)
        if m and m.group(1) in PHASES:
            cfg["phases"][m.group(1)] = dict(n=int(m.group(2)),
                                             p50_us=int(m.group(3)),
                                             p99_us=int(m.group(4)))
            continue
    m = RATIO_RE.search(text)
    if m:
        result["ratio"] = float(m.group(1))
    return result


# ---- running ---------------------------------------------------------------

def loadavg():
    return os.getloadavg()[0]


def wait_quiet(limit, timeout_s=180.0):
    """Do not start a cell on a busy box. Returns the load it started at."""
    start = time.time()
    while time.time() - start < timeout_s:
        la = loadavg()
        if la <= limit:
            return la
        time.sleep(5)
    return loadavg()


def run_cell(args, name, rep, port):
    cell = CELLS[name]
    workdir = os.path.join(args.workdir, f"{name}-r{rep}")
    os.makedirs(workdir, exist_ok=True)
    cmd = [sys.executable, "tools/multicore_benchmark.py",
           "--server", args.server,
           "--cores", str(cell["cores"]),
           "--tables", str(cell["tables"]),
           "--rows", str(cell["rows"]),
           "--placement", cell["placement"],
           "--workdir", workdir,
           "--port", str(port)]
    if cell["listeners"]:
        cmd.append("--peer-listeners")

    load_before = wait_quiet(args.quiet_load)
    sampler = CpuSampler()
    sampler.start()
    t0 = time.time()
    # A timed-out driver used to raise out of the whole matrix, discarding
    # every cell still to run and the summary of every cell already done.
    # One cell failing is a finding about that cell; it is not a reason to
    # lose the run.
    timed_out = False
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=args.timeout)
        stdout, stderr, rc = proc.stdout, proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        timed_out = True
        stdout = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        stderr = e.stderr.decode() if isinstance(e.stderr, bytes) else (e.stderr or "")
        rc = None
    elapsed = time.time() - t0
    sampler.stop()
    load_after = loadavg()

    parsed = parse_driver_output(stdout)
    parsed.update(dict(cell=name, rep=rep, invocation=" ".join(cmd),
                       returncode=rc, timed_out=timed_out, elapsed_s=elapsed,
                       load_before=load_before, load_after=load_after,
                       cpu=sampler.summary()))
    # A rep the driver did not finish cleanly contributes no number. Its
    # ratio may well have been printed before the failure, and a partial
    # run's ratio in a median is a number nobody measured.
    parsed["usable"] = (rc == 0 and not timed_out and parsed["ratio"] is not None
                        and not any(c["not_run"] for c in parsed["configs"].values()))
    if not parsed["usable"]:
        parsed["ratio"] = None

    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        base = os.path.join(args.archive, f"{name}-r{rep}")
        with open(base + ".stdout.txt", "w") as fh:
            fh.write(" ".join(cmd) + "\n\n" + stdout)
        if stderr.strip():
            with open(base + ".stderr.txt", "w") as fh:
                fh.write(stderr)
        with open(base + ".json", "w") as fh:
            json.dump(parsed, fh, indent=2)

    # The data files are the run's bulk and are never archived; drop them so
    # a 20k-row cell repeated five times does not fill the device.
    if not args.keep_data:
        shutil.rmtree(workdir, ignore_errors=True)
        if os.path.exists(workdir):
            print(f"    WARNING: could not remove {workdir}", flush=True)
        # Freeing a 20k-row cell's file is deferred work the filesystem does
        # after rmtree returns, and `wait_quiet` reads a 1-minute load
        # average that cannot see it. Let it drain rather than start the
        # next repetition on top of it.
        time.sleep(args.settle)

    return parsed


def median(vals):
    """Median over the values that exist. Nones are dropped, which is why
    every caller must also report `usable`: a median over 3 of 5 reps and a
    median over 5 of 5 print identically, and the first is a run where two
    repetitions produced no number at all."""
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    mid = len(vals) // 2
    return vals[mid] if len(vals) % 2 else (vals[mid - 1] + vals[mid]) / 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True,
                    help="under a block device, never tmpfs")
    ap.add_argument("--archive", default="",
                    help="directory for raw driver stdout and per-run JSON")
    ap.add_argument("--cells", default="H1,H2,H3,H4a,H4b,C1")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--port", type=int, default=15600)
    ap.add_argument("--quiet-load", type=float, default=0.6)
    ap.add_argument("--timeout", type=float, default=3600.0)
    ap.add_argument("--settle", type=float, default=5.0,
                    help="seconds to let the previous cell's deleted data file "
                         "finish being freed before the next one starts")
    ap.add_argument("--keep-data", action="store_true")
    args = ap.parse_args()

    names = [c.strip() for c in args.cells.split(",") if c.strip()]
    for n in names:
        if n not in CELLS:
            sys.exit(f"unknown cell {n}; known: {', '.join(sorted(CELLS))}")

    all_runs = []
    port = args.port
    for name in names:
        for rep in range(1, args.reps + 1):
            print(f"--- {name} rep {rep}/{args.reps} (port {port}) ---", flush=True)
            r = run_cell(args, name, rep, port)
            # Each invocation takes two ports; step well clear of TIME_WAIT.
            port += 4
            all_runs.append(r)
            mc = r["configs"].get("multi-core") or {}
            print(f"    ratio={r.get('ratio')}  rc={r['returncode']}  "
                  f"usable={r['usable']}  {r['elapsed_s']:.1f}s  "
                  f"errors={mc.get('errors')}  verify={mc.get('verify')}",
                  flush=True)
            if mc.get("retries") and mc["retries"] != "retries: none":
                print(f"    {mc['retries']}", flush=True)
            if mc.get("first_error"):
                print(f"    first error: {mc['first_error']}", flush=True)

    print("\n=== medians ===")
    summary = {}
    for name in names:
        runs = [r for r in all_runs if r["cell"] == name]
        ratios = [r["ratio"] for r in runs]
        usable = [r for r in ratios if r is not None]
        summary[name] = dict(
            cell=CELLS[name], reps=len(runs), ratios=ratios,
            # `reps` is how many were run, `ratio_reps` how many produced a
            # number. They differ exactly when a repetition failed, and a
            # median quoted without the second overstates its own sample.
            ratio_reps=len(usable),
            ratio_median=median(ratios),
            ratio_min=min(usable, default=None),
            ratio_max=max(usable, default=None),
            failed_reps=[dict(rep=x["rep"], returncode=x["returncode"],
                              timed_out=x["timed_out"],
                              not_run=[k for k, c in x["configs"].items()
                                       if c["not_run"]])
                         for x in runs if not x["usable"]],
            errors_total=sum((c.get("errors") or 0) for x in runs
                             for c in x["configs"].values()),
            invocation=runs[0]["invocation"] if runs else None,
        )
        for cfg in ("single-core", "multi-core"):
            per_phase = {}
            for ph in PHASES:
                p50 = [r["configs"].get(cfg, {}).get("phases", {}).get(ph, {}).get("p50_us")
                       for r in runs]
                p99 = [r["configs"].get(cfg, {}).get("phases", {}).get(ph, {}).get("p99_us")
                       for r in runs]
                per_phase[ph] = dict(p50_median=median(p50), p99_median=median(p99))
            walls = [r["configs"].get(cfg, {}).get("wall_s") for r in runs]
            aggs = [r["configs"].get(cfg, {}).get("aggregate_stmt_s") for r in runs]
            summary[name][cfg] = dict(
                phases=per_phase, wall_median=median(walls),
                aggregate_median=median(aggs),
                refills=[r["configs"].get(cfg, {}).get("refills") for r in runs],
                retries=[r["configs"].get(cfg, {}).get("retries") for r in runs])
        row = summary[name]
        print(f"  {name:<5} ratio median={row['ratio_median']} "
              f"spread={row['ratio_min']}..{row['ratio_max']} "
              f"reps={row['ratio_reps']}/{len(runs)} errors={row['errors_total']}"
              + ("  ** UNUSABLE REPS: " + str(row["failed_reps"])
                 if row["failed_reps"] else ""))

    if args.archive:
        with open(os.path.join(args.archive, "summary.json"), "w") as fh:
            json.dump(dict(summary=summary, runs=all_runs), fh, indent=2)
        print(f"\narchived to {args.archive}")


if __name__ == "__main__":
    main()
