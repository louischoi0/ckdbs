#!/usr/bin/env python3
"""T1 - the two workload cells `bench/v2.1.0` could not measure, swept over
`--cores 2,4,8` and repeated.

T1a  `bench/txn_batch_probe.py`, batch sweep 1/10/100/1000: one commit per
     batch instead of one per row, which is the only thing that steps outside
     §6's per-core `fdatasync` law.
T1b  `bench/single_relation_probe.py`, session sweep 1/2/4/8: one relation,
     N sessions, ascending keys, every session on the owner core - the
     serialized baseline the stride proposal claims to beat.

It runs the cells, archives every invocation's JSON, prints medians, and
decides nothing. A repetition that did not finish contributes no number and
is listed by name, because a median over 3 of 5 reps and one over 5 of 5
print identically.

Usage:
    bench/run_t1.py --server build-release/kds_server --workdir ~/mcbench2/t1 \
        --archive bench/v2.1.0/archive/pretasks-<describe>/t1 --reps 5
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def loadavg():
    return os.getloadavg()[0]


def wait_quiet(limit, timeout_s=180.0):
    start = time.time()
    while time.time() - start < timeout_s:
        la = loadavg()
        if la <= limit:
            return la
        time.sleep(5)
    return loadavg()


def median(vals):
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    mid = len(vals) // 2
    return vals[mid] if len(vals) % 2 else (vals[mid - 1] + vals[mid]) / 2


def load_archived(archive, name):
    """A repetition already on disk, or None. Resume exists because a sweep
    of this size is interrupted for reasons that have nothing to do with the
    engine - a mis-set load threshold, in the run this was added for - and
    re-running finished cells would replace measured numbers with fresh ones
    for no reason. Only a clean, parseable run is reused; anything else is
    re-run."""
    if not archive:
        return None
    path = os.path.join(archive, name + ".json")
    if not os.path.exists(path):
        return None
    try:
        with open(path) as fh:
            rec = json.load(fh)
        if rec.get("returncode") != 0:
            return None
        return json.loads(rec["stdout"])
    except (OSError, ValueError, KeyError):
        return None


def run(cmd, timeout, archive, name):
    """One probe invocation. Its stdout is JSON; a non-zero exit or a
    non-JSON stdout is recorded as a failed repetition rather than raised -
    one cell failing must not discard every cell still to run."""
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        stdout, stderr, rc = proc.stdout, proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        stdout = (e.stdout.decode() if isinstance(e.stdout, bytes) else e.stdout) or ""
        stderr = (e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr) or ""
        rc = None
    data = None
    if rc == 0:
        try:
            data = json.loads(stdout)
        except json.JSONDecodeError:
            data = None
    if archive:
        os.makedirs(archive, exist_ok=True)
        with open(os.path.join(archive, name + ".json"), "w") as fh:
            json.dump(dict(invocation=" ".join(cmd), returncode=rc,
                           stdout=stdout, stderr=stderr[-4000:]), fh, indent=2)
    return rc, data, stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--archive", default="")
    ap.add_argument("--cores", default="2,4,8")
    ap.add_argument("--batches", default="1,10,100,1000")
    ap.add_argument("--sessions", default="1,2,4,8")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--rows-t1a", type=int, default=2000)
    ap.add_argument("--rows-t1b", type=int, default=1000,
                    help="rows per session; the relation takes sessions x rows")
    ap.add_argument("--cells", default="T1a,T1b")
    ap.add_argument("--port", type=int, default=16500)
    ap.add_argument("--timeout", type=float, default=1800.0)
    ap.add_argument("--quiet-load", type=float, default=0.0,
                    help="start a cell only below this 1-minute load; 0 means "
                         "0.4 x this machine's CPU count. A fixed 0.6 was the "
                         "default until it stalled an 8-CPU sweep in "
                         "wait_quiet's 180 s timeout on every cell - the load "
                         "a benchmark leaves behind scales with the box, and a "
                         "threshold that does not will always be tripped by the "
                         "previous cell")
    ap.add_argument("--settle", type=float, default=2.0)
    args = ap.parse_args()
    if args.quiet_load <= 0:
        args.quiet_load = 0.4 * (os.cpu_count() or 1)

    cores_list = [int(c) for c in args.cores.split(",") if c.strip()]
    batches = [int(b) for b in args.batches.split(",") if b.strip()]
    sessions = [int(s) for s in args.sessions.split(",") if s.strip()]
    cells = [c.strip() for c in args.cells.split(",") if c.strip()]
    server = os.path.abspath(args.server)
    os.makedirs(args.workdir, exist_ok=True)

    runs = []
    port = args.port

    if "T1a" in cells:
        for cores in cores_list:
            for batch in batches:
                for rep in range(1, args.reps + 1):
                    name = f"T1a-c{cores}-b{batch}-r{rep}"
                    wd = os.path.join(args.workdir, name)
                    cmd = [sys.executable, os.path.join(HERE, "txn_batch_probe.py"),
                           "--server", server, "--workdir", wd,
                           "--cores", str(cores), "--rows", str(args.rows_t1a),
                           "--batch", str(batch), "--port", str(port)]
                    data = load_archived(args.archive, name)
                    rc, err = (0, "") if data else (None, "")
                    if data is None:
                        wait_quiet(args.quiet_load)
                        rc, data, err = run(cmd, args.timeout, args.archive, name)
                    port += 6
                    runs.append(dict(cell="T1a", cores=cores, batch=batch, rep=rep,
                                     rc=rc, data=data))
                    ok = data is not None
                    print(f"  {name}: rc={rc} ratio="
                          f"{(data or {}).get('ips_ratio')} "
                          f"multi_ips={((data or {}).get('multi') or {}).get('inserts_per_second')} "
                          f"single_ips={((data or {}).get('single') or {}).get('inserts_per_second')}"
                          + ("" if ok else f"  FAILED: {err.strip()[-300:]}"), flush=True)
                    shutil.rmtree(wd, ignore_errors=True)
                    time.sleep(args.settle)

    if "T1b" in cells:
        # The single-core arm does not depend on `--cores`: it is one core
        # whatever the multi arm's server has. Run it once per session count.
        for s in sessions:
            for rep in range(1, args.reps + 1):
                name = f"T1b-single-s{s}-r{rep}"
                wd = os.path.join(args.workdir, name)
                cmd = [sys.executable, os.path.join(HERE, "single_relation_probe.py"),
                       "--server", server, "--workdir", wd, "--arm", "single",
                       "--sessions", str(s), "--rows", str(args.rows_t1b),
                       "--port", str(port)]
                data = load_archived(args.archive, name)
                rc, err = (0, "") if data else (None, "")
                if data is None:
                    wait_quiet(args.quiet_load)
                    rc, data, err = run(cmd, args.timeout, args.archive, name)
                port += 6
                runs.append(dict(cell="T1b", arm="single", cores=1, sessions=s,
                                 rep=rep, rc=rc, data=data))
                print(f"  {name}: rc={rc} ips={(data or {}).get('inserts_per_second')} "
                      f"p50={(data or {}).get('insert_p50_us')}"
                      + ("" if data else f"  FAILED: {err.strip()[-300:]}"), flush=True)
                shutil.rmtree(wd, ignore_errors=True)
                time.sleep(args.settle)
        for cores in cores_list:
            for s in sessions:
                for rep in range(1, args.reps + 1):
                    name = f"T1b-multi-c{cores}-s{s}-r{rep}"
                    wd = os.path.join(args.workdir, name)
                    cmd = [sys.executable,
                           os.path.join(HERE, "single_relation_probe.py"),
                           "--server", server, "--workdir", wd, "--arm", "multi",
                           "--cores", str(cores), "--sessions", str(s),
                           "--rows", str(args.rows_t1b), "--port", str(port)]
                    data = load_archived(args.archive, name)
                    rc, err = (0, "") if data else (None, "")
                    if data is None:
                        wait_quiet(args.quiet_load)
                        rc, data, err = run(cmd, args.timeout, args.archive, name)
                    port += 6
                    runs.append(dict(cell="T1b", arm="multi", cores=cores,
                                     sessions=s, rep=rep, rc=rc, data=data))
                    print(f"  {name}: rc={rc} ips={(data or {}).get('inserts_per_second')} "
                          f"p50={(data or {}).get('insert_p50_us')}"
                          + ("" if data else f"  FAILED: {err.strip()[-300:]}"),
                          flush=True)
                    shutil.rmtree(wd, ignore_errors=True)
                    time.sleep(args.settle)

    print("\n=== T1a medians (ratio = cores=N / cores=1 insert throughput) ===")
    summary = {"T1a": {}, "T1b": {}}
    for cores in cores_list:
        for batch in batches:
            sel = [r for r in runs if r["cell"] == "T1a" and r["cores"] == cores
                   and r["batch"] == batch]
            good = [r["data"] for r in sel if r["data"]]
            row = dict(
                reps=len(sel), ok_reps=len(good),
                ratio_median=median([d.get("ips_ratio") for d in good]),
                ratio_min=min([d["ips_ratio"] for d in good if d.get("ips_ratio")],
                              default=None),
                ratio_max=max([d["ips_ratio"] for d in good if d.get("ips_ratio")],
                              default=None),
                multi_ips=median([d["multi"]["inserts_per_second"] for d in good]),
                single_ips=median([d["single"]["inserts_per_second"] for d in good]),
                multi_commits_s=median([d["multi"]["commits_per_second"] for d in good]),
                single_commits_s=median([d["single"]["commits_per_second"] for d in good]),
                multi_commit_p50_us=median([d["multi"]["commit_p50_us"] for d in good]),
                single_commit_p50_us=median([d["single"]["commit_p50_us"] for d in good]),
                multi_insert_p50_us=median([d["multi"]["insert_p50_us"] for d in good]),
                single_insert_p50_us=median([d["single"]["insert_p50_us"] for d in good]),
                batch_retries=sum(d["multi"]["batch_retries"] + d["single"]["batch_retries"]
                                  for d in good),
                errors=sum(d["multi"]["errors"] + d["single"]["errors"] for d in good),
                verify_bad=[d["multi"]["verify"] for d in good
                            if d["multi"]["verify"] != "rows as expected"]
                + [d["single"]["verify"] for d in good
                   if d["single"]["verify"] != "rows as expected"],
            )
            summary["T1a"][f"c{cores}-b{batch}"] = row
            print(f"  cores={cores} batch={batch:>4}: ratio={row['ratio_median']} "
                  f"({row['ratio_min']}..{row['ratio_max']}) "
                  f"multi={row['multi_ips']} single={row['single_ips']} ips  "
                  f"commit p50 {row['multi_commit_p50_us']}/{row['single_commit_p50_us']} us  "
                  f"reps={row['ok_reps']}/{row['reps']} errors={row['errors']}")

    print("\n=== T1b medians (one relation, N sessions on its owner core) ===")
    for arm, core_opts in [("single", [1])] + [("multi", cores_list)]:
        for cores in core_opts:
            for s in sessions:
                sel = [r for r in runs if r["cell"] == "T1b" and r["arm"] == arm
                       and r["cores"] == cores and r["sessions"] == s]
                good = [r["data"] for r in sel if r["data"]]
                row = dict(
                    reps=len(sel), ok_reps=len(good),
                    ips=median([d["inserts_per_second"] for d in good]),
                    ips_min=min([d["inserts_per_second"] for d in good], default=None),
                    ips_max=max([d["inserts_per_second"] for d in good], default=None),
                    p50_us=median([d["insert_p50_us"] for d in good]),
                    p99_us=median([d["insert_p99_us"] for d in good]),
                    p0_us=median([d["insert_p0_us"] for d in good]),
                    p25_us=median([d["insert_p25_us"] for d in good]),
                    p75_us=median([d["insert_p75_us"] for d in good]),
                    retries=sum(d["retries"] for d in good),
                    errors=sum(d["errors"] for d in good),
                    owner_cores=sorted({d["owner_core"] for d in good}),
                    verify_bad=[d["verify"] for d in good
                                if d["verify"] != "rows as expected"],
                )
                summary["T1b"][f"{arm}-c{cores}-s{s}"] = row
                print(f"  {arm:<6} cores={cores} sessions={s}: ips={row['ips']} "
                      f"({row['ips_min']}..{row['ips_max']}) p50={row['p50_us']}us "
                      f"p99={row['p99_us']}us reps={row['ok_reps']}/{row['reps']} "
                      f"errors={row['errors']}")

    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        with open(os.path.join(args.archive, "summary.json"), "w") as fh:
            json.dump(dict(summary=summary, runs=runs), fh, indent=2)
        print(f"\narchived to {args.archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
