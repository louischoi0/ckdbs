#!/usr/bin/env python3
"""T2 - locate the crossover: the ratio curve against sessions per writer core.

`bench/v2.1.0` §7 established the two ends and §11-1 says the middle is
bracketed, not located: rotation wins **1.751x** at one writing session per
writer core (C2's control-corrected H3) and **0.989x** at two (§7's direct
cross-cell comparison). The boundary between them is the number any placement
policy needs, and the number statement shipping needs to decide when to ship
rather than refuse under load.

The sweep is by table count, because `tools/multicore_benchmark.py` gives each
relation exactly one writing session and rotation spreads relations over the
`cores - 1` non-system cores (`core_placement.hpp:96-104`). So

    sessions per writer core = tables / (cores - 1)

and the four points per core count are `tables` = W, ~1.33W, ~1.67W, 2W for
W = cores - 1 writer cores. At `cores = 2` there is one writer core and no
fractional point exists at all - `tables` is the sessions-per-core figure -
so that arm contributes the integer points and says so rather than
interpolating.

**The imbalance is real and is reported, not hidden**: at W = 3, `tables = 4`
puts two sessions on one writer core and one on each of the others. That is
what a fractional average *is* on a thread-per-core engine, and it is why the
curve's middle points are read as "the average crossed 1.33", never as "every
core carried 1.33".

Usage:
    bench/run_t2.py --server build-release/kds_server --workdir ~/mcbench2/t2 \
        --archive bench/v2.1.0/archive/pretasks-<describe>/t2 --cores 2,4,8 --reps 5
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_benchv2 import (  # noqa: E402
    CpuSampler, median, parse_driver_output, wait_quiet,
)


def table_points(writer_cores):
    """The four sessions-per-writer-core points, as integer table counts.
    Deduplicated and sorted: at W = 1 the four collapse to two, which is a
    property of the shape and not something to paper over with a repeat."""
    w = writer_cores
    return sorted({w, round(w * 4 / 3), round(w * 5 / 3), 2 * w})


def run_cell(args, cores, tables, rep, port):
    workdir = os.path.join(args.workdir, f"c{cores}-t{tables}-r{rep}")
    os.makedirs(workdir, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, "..", "tools", "multicore_benchmark.py"),
           "--server", args.server, "--cores", str(cores), "--tables", str(tables),
           "--rows", str(args.rows), "--placement", "rotate", "--peer-listeners",
           "--workdir", workdir, "--port", str(port)]
    wait_quiet(args.quiet_load)
    sampler = CpuSampler()
    sampler.start()
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        stdout, stderr, rc = proc.stdout, proc.stderr, proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired as e:
        stdout = (e.stdout.decode() if isinstance(e.stdout, bytes) else e.stdout) or ""
        stderr = (e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr) or ""
        rc, timed_out = None, True
    elapsed = time.time() - t0
    sampler.stop()
    parsed = parse_driver_output(stdout)
    parsed.update(cores=cores, tables=tables, rep=rep, returncode=rc,
                  timed_out=timed_out, elapsed_s=elapsed, invocation=" ".join(cmd),
                  cpu=sampler.summary(),
                  sessions_per_writer_core=round(tables / max(1, cores - 1), 4))
    parsed["usable"] = (rc == 0 and not timed_out and parsed["ratio"] is not None
                        and not any(c["not_run"] for c in parsed["configs"].values()))
    if not parsed["usable"]:
        parsed["ratio"] = None
    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        base = os.path.join(args.archive, f"T2-c{cores}-t{tables}-r{rep}")
        with open(base + ".stdout.txt", "w") as fh:
            fh.write(" ".join(cmd) + "\n\n" + stdout)
        if stderr.strip():
            with open(base + ".stderr.txt", "w") as fh:
                fh.write(stderr)
        with open(base + ".json", "w") as fh:
            json.dump(parsed, fh, indent=2)
    shutil.rmtree(workdir, ignore_errors=True)
    time.sleep(args.settle)
    return parsed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--archive", default="")
    ap.add_argument("--cores", default="2,4,8")
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--port", type=int, default=17000)
    ap.add_argument("--timeout", type=float, default=1800.0)
    ap.add_argument("--quiet-load", type=float, default=0.6)
    ap.add_argument("--settle", type=float, default=3.0)
    args = ap.parse_args()
    args.server = os.path.abspath(args.server)
    cores_list = [int(c) for c in args.cores.split(",") if c.strip()]
    os.makedirs(args.workdir, exist_ok=True)

    runs = []
    port = args.port
    for cores in cores_list:
        for tables in table_points(cores - 1):
            for rep in range(1, args.reps + 1):
                r = run_cell(args, cores, tables, rep, port)
                port += 6
                runs.append(r)
                mc = r["configs"].get("multi-core") or {}
                sc = r["configs"].get("single-core") or {}
                print(f"  c{cores} t{tables} (s/core={r['sessions_per_writer_core']}) "
                      f"r{rep}: ratio={r['ratio']} multi={mc.get('aggregate_stmt_s')} "
                      f"single={sc.get('aggregate_stmt_s')} errors={mc.get('errors')} "
                      f"{r['elapsed_s']:.0f}s", flush=True)

    print("\n=== T2 curve: ratio against sessions per writer core ===")
    summary = {}
    for cores in cores_list:
        for tables in table_points(cores - 1):
            sel = [r for r in runs if r["cores"] == cores and r["tables"] == tables]
            good = [r for r in sel if r["usable"]]
            row = dict(
                cores=cores, tables=tables,
                sessions_per_writer_core=round(tables / max(1, cores - 1), 4),
                reps=len(sel), ok_reps=len(good),
                ratio_median=median([r["ratio"] for r in sel]),
                ratio_min=min([r["ratio"] for r in good], default=None),
                ratio_max=max([r["ratio"] for r in good], default=None),
                multi_aggregate=median([r["configs"]["multi-core"].get("aggregate_stmt_s")
                                        for r in good]),
                single_aggregate=median([r["configs"]["single-core"].get("aggregate_stmt_s")
                                         for r in good]),
                multi_insert_p50_us=median(
                    [r["configs"]["multi-core"]["phases"].get("insert", {}).get("p50_us")
                     for r in good]),
                single_insert_p50_us=median(
                    [r["configs"]["single-core"]["phases"].get("insert", {}).get("p50_us")
                     for r in good]),
                errors=sum((c.get("errors") or 0) for r in good
                           for c in r["configs"].values()),
                verify_bad=[r["configs"]["multi-core"].get("verify") for r in good
                            if r["configs"]["multi-core"].get("verify")
                            and "as expected" not in r["configs"]["multi-core"]["verify"]],
            )
            summary[f"c{cores}-t{tables}"] = row
            print(f"  cores={cores} tables={tables:>2} s/core="
                  f"{row['sessions_per_writer_core']:<5} ratio={row['ratio_median']} "
                  f"({row['ratio_min']}..{row['ratio_max']}) "
                  f"multi={row['multi_aggregate']} single={row['single_aggregate']} "
                  f"reps={row['ok_reps']}/{row['reps']} errors={row['errors']}")

    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        with open(os.path.join(args.archive, "summary.json"), "w") as fh:
            json.dump(dict(summary=summary, runs=runs), fh, indent=2)
        print(f"\narchived to {args.archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
