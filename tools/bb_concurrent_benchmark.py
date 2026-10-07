#!/usr/bin/env python3
"""BB-S5, cells C3 and C4: concurrent omitted-pk INSERT, A/B across two engines.

`instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` BB-R8. One server at
a time (A or B, alternating which goes first per run), a fresh server and data
file per (cell, run), `--clients` client processes each opening one session
and inserting `--count` rows of `INSERT INTO t VALUES (<n>, <n>)` (the pk
omitted) after a common start time. A `TXN_CONFLICT retryable=1` reply is
counted and the same statement retried, so every client completes `--count`
rows: equal work, not equal time.

  --shape one    C3: every client inserts into one relation, bb_t0
  --shape eight  C4: client i inserts into its own relation, bb_t<i>
                 (needs --clients = the number of relations wanted)

Reported per run: rows/s (rows over the span from the earliest client start to
the latest client end), per-statement latency over every client's statements
(retries included in the statement that needed them) as p0/p25/p50/p95/p99,
the refusal count, each client's CPU use (a client above 80 % of one CPU makes
the cell client-bound: a floor, not the engine's ceiling), the core each
session landed on, and `SHOW META`'s `core=`-0 counters around the run.

Placement: `--server-cpus LIST` runs the server under `taskset -c LIST` (peer
reactor k pins itself to CPU k whatever the mask; core 0 inherits the mask);
`--client-cpus LIST` pins client i to LIST[i % len]. Neither given = the
kernel places everything.

    tools/bb_concurrent_benchmark.py --shape one --cores 8 --clients 8 \\
        --bin-a .../kds_server-A --bin-b .../kds_server-B --port-a 15611 \\
        --port-b 15610 --workdir ~/bench-runs/bb-s5/work --out archive/c3
"""

import argparse
import json
import os
import resource
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import nearest_rank  # noqa: E402
from ckdbs_cli import ServerConnection  # noqa: E402
from bb_overhead_benchmark import host_state, meta_all, must, wait_for_load  # noqa: E402


def client(args):
    conn = ServerConnection("127.0.0.1", args.port)
    core = meta_all(conn).get("core")
    # `core=` is a name=value token; meta_all keeps only numeric ones
    table = f"bb_t{args.client_index if args.shape == 'eight' else 0}"
    lat, refusals, errors, first_err = [], 0, 0, None
    cpu0 = resource.getrusage(resource.RUSAGE_SELF)
    while time.time() < args.start_at:
        time.sleep(0.0005)
    t_start = time.time()
    base = 10_000_000 * (args.client_index + 1)
    for i in range(args.count):
        n = base + i
        sql = f"INSERT INTO {table} VALUES ({n}, {n})"
        t0 = time.perf_counter()
        for _attempt in range(2000):
            r = conn.send_command(sql)
            if r.startswith("ERR") and "TXN_CONFLICT" in r and "retryable=1" in r:
                refusals += 1
                continue
            break
        lat.append(time.perf_counter() - t0)
        if r.startswith("ERR"):
            errors += 1
            first_err = first_err or r
    t_end = time.time()
    cpu1 = resource.getrusage(resource.RUSAGE_SELF)
    cpu = (cpu1.ru_utime + cpu1.ru_stime) - (cpu0.ru_utime + cpu0.ru_stime)
    print(json.dumps({"index": args.client_index, "core": core, "t_start": t_start, "t_end": t_end,
                      "cpu_s": cpu, "refusals": refusals, "errors": errors, "first_error": first_err,
                      "lat": lat}))


def session_core(conn):
    text = conn.send_command("SHOW META").replace("\\n", " ")
    for tok in text.split():
        if tok.startswith("core="):
            return tok[5:]
    return None


def run_one(args, run, label, binary, port, tag):
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    wd = os.path.join(args.workdir, tag)
    os.makedirs(wd, exist_ok=True)
    data = os.path.join(wd, "d.db")
    conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {args.cores}\ndurability = {args.durability}\n"
                f"log_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
    cmd = (["taskset", "-c", args.server_cpus] if args.server_cpus else []) + [binary, "--config", conf]
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 30
        while True:
            try:
                ctl = ServerConnection("127.0.0.1", port)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit("server did not listen")
                time.sleep(0.1)
        mount = meta_all(ctl)
        ntab = 1 if args.shape == "one" else args.clients
        for t in range(ntab):
            must(ctl, f"CREATE TABLE bb_t{t} (id int64, v int64, w int64) BTREE", "CREATED")
        m0 = meta_all(ctl)
        start_at = time.time() + 4.0
        procs = []
        for i in range(args.clients):
            cl = ["python3", os.path.abspath(__file__), "--client", "--port", str(port),
                  "--client-index", str(i), "--count", str(args.count), "--shape", args.shape,
                  "--start-at", repr(start_at)]
            if args.client_cpus:
                cpus = args.client_cpus.split(",")
                cl = ["taskset", "-c", cpus[i % len(cpus)]] + cl
            procs.append(subprocess.Popen(cl, stdout=subprocess.PIPE, text=True))
        outs = []
        for p in procs:
            o, _ = p.communicate(timeout=600)
            outs.append(json.loads(o.strip().splitlines()[-1]))
        m1 = meta_all(ctl)
        counts = []
        for t in range(ntab):
            counts.append(ctl.send_command(f"SELECT COUNT(*) FROM bb_t{t}").replace("\\n", " "))
        after = host_state()
        lat = sorted(x for o in outs for x in o["lat"])
        span = max(o["t_end"] for o in outs) - min(o["t_start"] for o in outs)
        rows = sum(len(o["lat"]) for o in outs)
        res = {"label": label, "run": run, "shape": args.shape, "cores": args.cores, "clients": args.clients,
               "count": args.count, "durability": args.durability, "server_cpus": args.server_cpus,
               "client_cpus": args.client_cpus, "wait_s": round(wait, 1), "host_before": before,
               "host_after": after, "pid": proc.pid,
               "mount": {k: mount.get(k) for k in ("recovery_checkpoint_us",)},
               "rows": rows, "span_s": round(span, 4), "rows_per_s": round(rows / span, 1),
               "p0_us": round(lat[0] * 1e6, 1), "p25_us": round(nearest_rank(lat, 25) * 1e6, 1),
               "p50_us": round(nearest_rank(lat, 50) * 1e6, 1), "p95_us": round(nearest_rank(lat, 95) * 1e6, 1),
               "p99_us": round(nearest_rank(lat, 99) * 1e6, 1), "max_us": round(lat[-1] * 1e6, 1),
               "refusals": sum(o["refusals"] for o in outs), "errors": sum(o["errors"] for o in outs),
               "first_error": next((o["first_error"] for o in outs if o["first_error"]), None),
               "client_cpu_share": [round(o["cpu_s"] / (o["t_end"] - o["t_start"]), 2) for o in outs],
               "client_cores": [o["core"] for o in outs],
               "counts": counts, "meta_delta": {k: m1[k] - m0.get(k, 0) for k in m1 if k.startswith("sched_foreground")
                                                or k in ("wal_syncs", "wal_ring_full", "wal_ring_full_refusals")}}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(res, f, indent=1)
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} rows/s={res['rows_per_s']} "
              f"p50={res['p50_us']} p99={res['p99_us']} refusals={res['refusals']} errors={res['errors']} "
              f"clientcpu_max={max(res['client_cpu_share'])} cores={sorted(set(map(str, res['client_cores'])))} "
              f"counts={counts}", flush=True)
        ctl.close()
    finally:
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err.close()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--client", action="store_true")
    p.add_argument("--client-index", type=int, default=0)
    p.add_argument("--start-at", type=float, default=0.0)
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--shape", choices=("one", "eight"), default="one")
    p.add_argument("--cores", type=int, default=8)
    p.add_argument("--clients", type=int, default=8)
    p.add_argument("--count", type=int, default=2000, help="rows per client")
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--bin-a")
    p.add_argument("--bin-b")
    p.add_argument("--port-a", type=int)
    p.add_argument("--port-b", type=int)
    p.add_argument("--server-cpus", default=None)
    p.add_argument("--client-cpus", default=None)
    p.add_argument("--workdir")
    p.add_argument("--out")
    p.add_argument("--tag", default="c")
    p.add_argument("--max-load", type=float, default=1.3)
    p.add_argument("--max-wait", type=float, default=120)
    args = p.parse_args()
    if args.client:
        client(args)
        return
    for run in range(args.runs):
        order = [("B", args.bin_b, args.port_b), ("A", args.bin_a, args.port_a)]
        if run % 2:
            order.reverse()
        for label, binary, port in order:
            run_one(args, run, label, binary, port, f"{args.tag}-{label}-r{run}")


if __name__ == "__main__":
    sys.exit(main())
