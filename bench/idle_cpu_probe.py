#!/usr/bin/env python3
"""Per-core CPU while the engine is doing nothing - PW7's first open item,
observed rather than fixed.

PW7 recorded that the reactor spins whenever a parked coroutine is present,
because `Scheduler::IdleTimeoutMs` returns 0 as soon as any ready queue is
non-empty and a parked task still sits in one (src/sched/scheduler.cpp:196-199,
docs/inflight/in-progress/workplan-peer-writer.md:325). It saw the symptom as a trx-id refill whose
39 ms spanned 108,150 reactor iterations. That was on a host with one writer
core. This measures what it costs at three.

Three windows, each sampled per cpu from /proc/stat:

  baseline   no server at all - what the box itself burns
  mounted    the instance up, not one client connected
  sessions   one idle session per core, no statement issued

The reading is a comparison, not an absolute: `mounted` above `baseline` by
roughly one core per reactor is a spin; `mounted` at `baseline` is a reactor
that blocks in `PollReady` as intended.

Usage:
    bench/idle_cpu_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/idle --cores 4 --seconds 15
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import Conn, collect_connections, wait_for_port  # noqa: E402


def read_cpu_jiffies():
    out = {}
    with open("/proc/stat") as fh:
        for line in fh:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            out[parts[0]] = [int(v) for v in parts[1:9]]
    return out


def busy_over(seconds):
    """Busy fraction per cpu across one window. Busy is everything that is
    not idle and not iowait - a spinning reactor lands in user+system, which
    is exactly what separates it from a blocked one."""
    before = read_cpu_jiffies()
    time.sleep(seconds)
    after = read_cpu_jiffies()
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        delta = [x - y for x, y in zip(a, b)]
        total = sum(delta)
        idle = delta[3] + delta[4]
        out[cpu] = round((total - idle) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--port", type=int, default=15800)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--placement", default="rotate")
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    conf = os.path.join(args.workdir, "idle.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(args.workdir, 'idle.db')}\n"
                f"port = {args.port}\ncores = {args.cores}\n"
                f"placement = {args.placement}\npeer_listeners = on\n"
                f"log_file = idle.log\nlog_dir = {args.workdir}\nlog_level = warn\n")

    findings = dict(cores=args.cores, seconds=args.seconds)

    findings["baseline"] = busy_over(args.seconds)

    stderr_path = os.path.join(args.workdir, "idle.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    try:
        wait_for_port(args.port, stderr_path)
        # Let the mount settle: recovery and the completion checkpoint are
        # real work and are not what this probe is asking about.
        time.sleep(3)
        findings["mounted"] = busy_over(args.seconds)

        got, attempts = collect_connections(
            args.port, {c: 1 for c in range(args.cores)}, args.max_connects)
        conns = [c[0] for c in got.values()]
        findings["session_connect_attempts"] = attempts
        findings["cores_reached"] = sorted(got)
        time.sleep(1)
        findings["sessions"] = busy_over(args.seconds)
        findings["meta"] = conns[0].cmd("SHOW META")

        try:
            conns[0].cmd("STOP")
        except OSError:
            pass
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)

    print(json.dumps(findings, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(findings, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
