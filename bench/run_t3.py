#!/usr/bin/env python3
"""T3 - discriminate the four-core-server effect.

`bench/v2.1.0` §7 measured C1: `cores = 4`, `placement = creating`, no peer
listeners - every relation core 0's and every session core 0's, so **nothing
cross-core happens at all** - beating `cores = 1` by 1.071x aggregate and
1.457x on insert p50. That is currently a larger effect than rotation's whole
contribution and §11-3 leaves it undiscriminated, naming three candidates:
four WAL anchors, per-core extent leases, background work moving off core 0.

This does not patch the engine to find out (the run instructions forbid it).
It runs what configuration alone can separate:

  **A. The core-count curve at `creating` placement.** Cells at
  `cores = 1, 2, 3, 4, 8`, identical workload, every one of them with all
  relations and all sessions on core 0. The *shape* discriminates: an effect
  that appears whole at `cores = 2` and stays flat is the multi-core
  machinery existing at all (the system-core role, the lease services, the
  ring transport, a second reactor); one that grows with the core count is a
  per-core resource (anchors, streams, per-core background work).

  **B. The `cores = 1` against `cores = 1` cell**, which is two identical
  servers compared to each other. It measures this harness's own noise floor,
  and every ratio in A is read against it - a 1.07x means nothing until the
  null cell's spread is known.

  **C. Per-core CPU and `SHOW META` per arm.** If background work moved off
  core 0, the peers must show CPU while `creating` placement gives them no
  relation and no session; if it did not, they are at the idle floor and that
  candidate is dead by measurement rather than by argument.

What configuration *cannot* separate is stated in the results file rather
than worked around: `wal_anchor_count` is not a knob - it is a high-water
mark of anchor slots ever published (`superblock.cpp:174`), so the four
anchors cannot be given to a one-core server or taken from a four-core one
without an engine change.

Usage:
    bench/run_t3.py --server build-release/kds_server --workdir ~/mcbench2/t3 \
        --archive bench/v2.1.0/archive/pretasks-<describe>/t3 --cores 1,2,3,4,8 --reps 5
"""

import argparse
import collections
import json
import os
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from run_benchv2 import (  # noqa: E402
    CpuSampler, median, parse_driver_output, wait_quiet,
)
from multicore_benchmark import (  # noqa: E402
    Conn, field, is_retryable, wait_for_port,
)


# ---- A/B: the driver cells -------------------------------------------------

def run_cell(args, cores, rep, port):
    workdir = os.path.join(args.workdir, f"c{cores}-r{rep}")
    os.makedirs(workdir, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, "..", "tools", "multicore_benchmark.py"),
           "--server", args.server, "--cores", str(cores), "--tables", str(args.tables),
           "--rows", str(args.rows), "--placement", "creating",
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
    parsed.update(cores=cores, rep=rep, returncode=rc, timed_out=timed_out,
                  elapsed_s=elapsed, invocation=" ".join(cmd), cpu=sampler.summary())
    parsed["usable"] = (rc == 0 and not timed_out and parsed["ratio"] is not None
                        and not any(c["not_run"] for c in parsed["configs"].values()))
    if not parsed["usable"]:
        parsed["ratio"] = None
    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        base = os.path.join(args.archive, f"T3-c{cores}-r{rep}")
        with open(base + ".stdout.txt", "w") as fh:
            fh.write(" ".join(cmd) + "\n\n" + stdout)
        with open(base + ".json", "w") as fh:
            json.dump(parsed, fh, indent=2)
    shutil.rmtree(workdir, ignore_errors=True)
    time.sleep(args.settle)
    return parsed


# ---- C: per-core CPU and SHOW META, one configuration at a time ------------

def read_cpu_jiffies():
    out = {}
    with open("/proc/stat") as fh:
        for line in fh:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            out[parts[0]] = [int(v) for v in parts[1:9]]
    return out


def busy_between(before, after):
    out = {}
    for cpu, a in after.items():
        b = before.get(cpu)
        if not b:
            continue
        d = [x - y for x, y in zip(a, b)]
        total = sum(d)
        out[cpu] = round((total - d[3] - d[4]) / total, 4) if total else 0.0
    return dict(sorted(out.items(), key=lambda kv: int(kv[0][3:])))


def insert_worker(conn, name, rows, out, index, deadline_s=20.0):
    errors = retries = inserted = 0
    first_error = None
    for i in range(1, rows + 1):
        stmt = f"INSERT INTO {name} VALUES ('o{i % 7}', {i * 10})"
        end = time.time() + deadline_s
        while True:
            r = conn.cmd(stmt)
            if not r.startswith("ERR"):
                inserted += 1
                break
            if is_retryable(r) and time.time() < end:
                retries += 1
                continue
            errors += 1
            if first_error is None:
                first_error = r
            break
    out[index] = dict(name=name, inserted=inserted, retries=retries,
                      errors=errors, first_error=first_error)


def attributed_run(args, cores, port):
    """One `creating`-placement configuration, insert phase only, with an
    idle window on the same mount for comparison and `SHOW META` before and
    after. Every relation and every session is core 0's whatever `cores`
    says, so any CPU on cpu1..N is work the engine put there by itself."""
    tag = f"attr-c{cores}"
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\nport = {port}\n"
                f"cores = {cores}\nplacement = creating\npeer_listeners = off\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "s.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(cores=cores, tables=args.tables, rows=args.rows)
    try:
        wait_for_port(port, stderr_path)
        time.sleep(2)          # let the mount's own work drain out of the window
        setup = Conn(port)
        out["meta_before"] = setup.cmd("SHOW META")
        names = [f"a{i}" for i in range(args.tables)]
        owners = {}
        for n in names:
            r = setup.cmd(f"CREATE TABLE {n} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{n}: {r}")
            owners[n] = int(field(setup.cmd(f"DESCRIBE {n}"), "owner_core"))
        out["owner_cores"] = owners
        writers = {n: Conn(port) for n in names}

        before = read_cpu_jiffies()
        t0 = time.time()
        time.sleep(args.idle_seconds)
        out["idle_busy"] = busy_between(before, read_cpu_jiffies())
        out["idle_seconds"] = round(time.time() - t0, 3)

        res = [None] * len(names)
        threads = [threading.Thread(target=insert_worker,
                                    args=(writers[n], n, args.rows, res, i))
                   for i, n in enumerate(names)]
        before = read_cpu_jiffies()
        t0 = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        secs = time.time() - t0
        out["insert_busy"] = busy_between(before, read_cpu_jiffies())
        out["insert_seconds"] = round(secs, 4)
        inserted = sum(r["inserted"] for r in res if r)
        out.update(inserted=inserted,
                   inserts_per_second=round(inserted / secs, 1) if secs else 0.0,
                   retries=sum(r["retries"] for r in res if r),
                   errors=sum(r["errors"] for r in res if r),
                   first_error=next((r["first_error"] for r in res if r and r["first_error"]),
                                    None))
        out["meta_after"] = setup.cmd("SHOW META")
        setup.cmd("STOP")
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
    shutil.rmtree(workdir, ignore_errors=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--archive", default="")
    ap.add_argument("--cores", default="1,2,3,4,8")
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--attr-reps", type=int, default=3)
    ap.add_argument("--idle-seconds", type=float, default=5.0)
    ap.add_argument("--port", type=int, default=17400)
    ap.add_argument("--timeout", type=float, default=1800.0)
    ap.add_argument("--quiet-load", type=float, default=0.6)
    ap.add_argument("--settle", type=float, default=3.0)
    args = ap.parse_args()
    args.server = os.path.abspath(args.server)
    cores_list = [int(c) for c in args.cores.split(",") if c.strip()]
    os.makedirs(args.workdir, exist_ok=True)

    runs, attrs = [], []
    port = args.port
    for cores in cores_list:
        for rep in range(1, args.reps + 1):
            r = run_cell(args, cores, rep, port)
            port += 6
            runs.append(r)
            mc = r["configs"].get("multi-core") or {}
            sc = r["configs"].get("single-core") or {}
            print(f"  A c{cores} r{rep}: ratio={r['ratio']} "
                  f"n-core={mc.get('aggregate_stmt_s')} one-core={sc.get('aggregate_stmt_s')} "
                  f"insert p50 {(mc.get('phases') or {}).get('insert', {}).get('p50_us')}/"
                  f"{(sc.get('phases') or {}).get('insert', {}).get('p50_us')} us "
                  f"errors={mc.get('errors')}", flush=True)

    for cores in cores_list:
        for rep in range(1, args.attr_reps + 1):
            wait_quiet(args.quiet_load)
            a = attributed_run(args, cores, port)
            port += 6
            a["rep"] = rep
            attrs.append(a)
            busy = a.get("insert_busy", {})
            idle = a.get("idle_busy", {})
            print(f"  C c{cores} r{rep}: ips={a['inserts_per_second']} "
                  f"busy={{" + ", ".join(f"{k}:{v:.3f}" for k, v in busy.items()) + "}} "
                  f"idle={{" + ", ".join(f"{k}:{v:.3f}" for k, v in idle.items()) + "}}",
                  flush=True)
            time.sleep(args.settle)

    print("\n=== T3-A: the core-count curve at creating placement ===")
    summary = {"A": {}, "C": {}}
    for cores in cores_list:
        sel = [r for r in runs if r["cores"] == cores]
        good = [r for r in sel if r["usable"]]
        row = dict(
            cores=cores, reps=len(sel), ok_reps=len(good),
            ratio_median=median([r["ratio"] for r in sel]),
            ratio_min=min([r["ratio"] for r in good], default=None),
            ratio_max=max([r["ratio"] for r in good], default=None),
            n_core_aggregate=median([r["configs"]["multi-core"].get("aggregate_stmt_s")
                                     for r in good]),
            one_core_aggregate=median([r["configs"]["single-core"].get("aggregate_stmt_s")
                                       for r in good]),
            n_core_insert_p50=median(
                [r["configs"]["multi-core"]["phases"].get("insert", {}).get("p50_us")
                 for r in good]),
            one_core_insert_p50=median(
                [r["configs"]["single-core"]["phases"].get("insert", {}).get("p50_us")
                 for r in good]),
            errors=sum((c.get("errors") or 0) for r in good for c in r["configs"].values()),
        )
        row["insert_p50_gain"] = (round(row["one_core_insert_p50"] / row["n_core_insert_p50"], 4)
                                  if row["n_core_insert_p50"] else None)
        summary["A"][f"c{cores}"] = row
        print(f"  cores={cores}: ratio={row['ratio_median']} "
              f"({row['ratio_min']}..{row['ratio_max']}) insert p50 gain="
              f"{row['insert_p50_gain']} n-core={row['n_core_aggregate']} "
              f"one-core={row['one_core_aggregate']} reps={row['ok_reps']}/{row['reps']}")

    print("\n=== T3-C: per-core CPU, everything on core 0 ===")
    for cores in cores_list:
        sel = [a for a in attrs if a["cores"] == cores]
        if not sel:
            continue
        cpus = sorted(sel[0].get("insert_busy", {}), key=lambda c: int(c[3:]))
        row = dict(
            cores=cores, reps=len(sel),
            ips=median([a["inserts_per_second"] for a in sel]),
            insert_busy={c: median([a["insert_busy"].get(c) for a in sel]) for c in cpus},
            idle_busy={c: median([a["idle_busy"].get(c) for a in sel]) for c in cpus},
            meta_after=sel[0].get("meta_after"),
            errors=sum(a["errors"] for a in sel),
        )
        summary["C"][f"c{cores}"] = row
        print(f"  cores={cores}: ips={row['ips']} busy=" +
              " ".join(f"{c}={row['insert_busy'][c]}" for c in cpus))

    if args.archive:
        os.makedirs(args.archive, exist_ok=True)
        with open(os.path.join(args.archive, "summary.json"), "w") as fh:
            json.dump(dict(summary=summary, runs=runs, attributed=attrs), fh, indent=2)
        print(f"\narchived to {args.archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
