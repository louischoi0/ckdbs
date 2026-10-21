#!/usr/bin/env python3
"""BS-S3's I/O sweep (instructions/v3.0.0/workorder-bs-test-hardening.md).

For every committed sim seed, mode, phase and device kind, fail the 1st,
2nd, ... operation of that kind, one `ckdbs-sim --fail-nth KIND:N[@mount]`
run per point, until a point's fault no longer fires - the phase has fewer
operations of the kind than N. The phase is the workload's I/O or the
mount's (BS-A7). Each run asserts its obligation itself (sim/faults.hpp's
`NthFault`: in the workload F5's - a truthful Status, no wrong answer, the
oracle's indeterminate set honoured, the quiescence probe; at the mount, a
refused mount the next fault-free one recovers), so a point fails exactly
when `ckdbs-sim` exits non-zero.

    scripts/bs/io_sweep.py BIN [--ops 300] [--modes clean,crash]
                           [--phases workload,mount] [--jobs N]

A failing point is reproduced by the command it prints (BS-R3). Never run by
the push hook (the order's §6.1); scripts/bs/nightly.sh runs it.
"""
import argparse
import concurrent.futures
import itertools
import os
import re
import subprocess
import sys
import time

KINDS = ["page-fail-read", "page-fail-write", "page-fail-sync", "page-fail-grow",
         "log-fail-write", "log-fail-sync", "power-loss-at-page-write"]
SEEDS_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "testdata",
                          "sim_seeds.txt")


def committed_seeds():
    with open(SEEDS_FILE) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def run_point(binary, seed, mode, ops, kind, phase, n):
    at = "@mount" if phase == "mount" else ""
    cmd = [binary, "--seed", seed, "--ops", str(ops), "--iterations", "1", "--mode", mode,
           "--profile", "uniform", "--fail-nth", f"{kind}:{n}{at}"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        # A hang after an injected fault is a finding, not the sweep's end.
        return n, "timeout", -1, " ".join(cmd), ["timed out after 600 s"]
    out = p.stdout + p.stderr
    m = re.search(r" fired=(\d+)", out)
    fired = int(m.group(1)) if m else -1
    return n, p.returncode, fired, " ".join(cmd), out.strip().splitlines()[-1:] or [""]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--ops", type=int, default=300)
    ap.add_argument("--modes", default="clean,crash")
    # The workload's I/O, and the mount's (BS-A7): recovery is where pages are read.
    ap.add_argument("--phases", default="workload,mount")
    ap.add_argument("--kinds", default=",".join(KINDS))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--max-n", type=int, default=5000)
    args = ap.parse_args()
    # A misspelt kind is a usage error at every N, which would sweep to --max-n.
    unknown = set(args.kinds.split(",")) - set(KINDS)
    if unknown:
        ap.error(f"unknown --kinds {sorted(unknown)}; known: {','.join(KINDS)}")

    started = time.time()
    points = 0
    fired_points = 0
    failures = []
    series = itertools.product(committed_seeds(), args.modes.split(","), args.phases.split(","),
                               args.kinds.split(","))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for seed, mode, phase, kind in series:
            n, last_fired = 1, 0
            while n <= args.max_n:
                batch = list(range(n, min(n + args.jobs, args.max_n + 1)))
                results = list(pool.map(
                    lambda k: run_point(args.binary, seed, mode, args.ops, kind, phase, k), batch))
                points += len(results)
                exhausted = False
                for k, rc, fired, cmd, tail in sorted(results):
                    if rc != 0:
                        failures.append((cmd, tail[0]))
                    if fired == 0:
                        exhausted = True
                    elif fired > 0:
                        last_fired = k
                        fired_points += 1
                if exhausted:
                    break
                n = batch[-1] + 1
            else:
                # Points past --max-n were never swept: say so.
                failures.append((f"seed={seed} mode={mode} {phase} {kind}",
                                 f"no fault failed to fire by --max-n {args.max_n}"))
            print(f"seed={seed} mode={mode} {phase} {kind}: {last_fired} point(s) fired",
                  flush=True)
    for cmd, tail in failures:
        print(f"FAIL {cmd}\n     {tail}")
    print(f"io-sweep: {points} run(s), {fired_points} point(s) fired, {len(failures)} failure(s), "
          f"{(time.time() - started) / 60:.1f} min")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
