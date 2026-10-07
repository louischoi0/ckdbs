#!/usr/bin/env python3
"""BB-S5: what milestone BB costs the single-row INSERT, A/B across two engines.

`instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` BB-R8, and
`bench/v3.0.0/results-bb-s5-overhead-*.md`. Shaped like
`ap_overhead_benchmark.py`: serial, one session per server, arms alternating
which server goes first per block, `SHOW META` read around every block. Unlike
it, this driver starts the servers itself - a fresh server and data file for
every (cell, rows, run) - from two binary copies named by `--bin-a`/`--bin-b`,
and loops over `--rows` and `--runs`, writing one JSON per (cell, rows, run).

Cells (`--cell`), relation BTREE, `cores = 1`, `durability = relaxed`:

  c1   bb_t (id int64, v int64, w int64): arm `insert` is
       INSERT INTO t VALUES (<n>, <n>), the pk omitted, nothing spilled.
  c1s  bb_s (id int64, v int64, s varchar(16)): INSERT INTO t VALUES (<n>,
       '<40 bytes>'), the pk omitted, the value spilled to the var-heap.
  c2   bb_t again: INSERT INTO t VALUES (<k>, <n>, <n>), k ascending from
       rows + 1, i.e. every key lands at the mark.

Every cell runs the arms `insert` and `insert-again` (the noise floor: the same
shape on a second relation preloaded identically) and `ping` (control:
SHOW META, which no insert path can touch). `--rows N` is the row count the
relation holds when an arm starts (200 / 1000 / 10000), loaded with named
ascending keys in 500-row batches; the arm then adds `--ops` rows.

Placement: `--pin-server CPU` runs both servers under `taskset -c CPU` (the
caller pins the driver itself, `taskset -c 4 tools/bb_overhead_benchmark.py`).

    taskset -c 4 tools/bb_overhead_benchmark.py --cell c1 --rows 200,1000,10000 \\
        --runs 12 --pin-server 2 --bin-a .../kds_server-A --bin-b .../kds_server-B \\
        --workdir ~/bench-runs/bb-s5/c1 --out archive/c1 --port-b 15610 --port-a 15611
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase  # noqa: E402
from ckdbs_cli import ServerConnection  # noqa: E402

BATCH = 500
SPILL = "abcdefghijklmnopqrstuvwxyz0123456789"


def host_state():
    try:
        with open("/proc/loadavg") as f:
            load = f.read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "unavailable"
    return {"loadavg": load, "competing": busy.splitlines() if busy else []}


def meta_all(conn):
    text = conn.send_command("SHOW META").replace("\\n", " ")
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


def must(conn, sql, prefix):
    reply = conn.send_command(sql)
    if not reply.startswith(prefix):
        raise SystemExit(f"setup: {sql!r} -> {reply!r}")
    return reply


def wait_for_load(limit, max_wait):
    t0 = time.time()
    while time.time() - t0 < max_wait:
        with open("/proc/loadavg") as f:
            if float(f.read().split()[0]) <= limit:
                return time.time() - t0
        time.sleep(5)
    return time.time() - t0


class Srv:
    def __init__(self, label, binary, workdir, port, pin, tag):
        self.label = label
        os.makedirs(workdir, exist_ok=True)
        data = os.path.join(workdir, f"{label}-{tag}.db")
        conf = os.path.join(workdir, f"{label}-{tag}.conf")
        self.files = [data]
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = 1\ndurability = relaxed\n"
                    f"log_file = {label}-{tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
        self.files.append(os.path.join(workdir, f"{label}-{tag}.log"))
        self.err = os.path.join(workdir, f"{label}-{tag}.stderr")
        cmd = ([ "taskset", "-c", str(pin)] if pin is not None else []) + [binary, "--config", conf]
        with open(self.err, "w") as e:
            self.proc = subprocess.Popen(cmd, stdout=e, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid
        deadline = time.time() + 30
        while True:
            try:
                self.conn = ServerConnection("127.0.0.1", port)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"server {label} did not listen on {port}")
                time.sleep(0.1)
        self.mount = meta_all(self.conn)

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for f in self.files + [self.err]:
            try:
                os.remove(f)
            except OSError:
                pass


def load_rows(conn, table, rows, row_fn):
    for lo in range(1, rows + 1, BATCH):
        hi = min(rows, lo + BATCH - 1)
        must(conn, f"INSERT INTO {table} VALUES " + ",".join(row_fn(i) for i in range(lo, hi + 1)),
             "INSERTED")


def spill_val(n):
    return f"{SPILL}{n % 10000:04d}"  # 36 + 4 = 40 bytes, distinct per n


def setup(conn, cell, rows):
    for t in ("a", "b"):
        if cell == "c1s":
            must(conn, f"CREATE TABLE bb_{t} (id int64, v int64, s varchar(16)) BTREE", "CREATED")
            load_rows(conn, f"bb_{t}", rows, lambda i: f"({i}, {i}, '{spill_val(i)}')")
        else:
            must(conn, f"CREATE TABLE bb_{t} (id int64, v int64, w int64) BTREE", "CREATED")
            load_rows(conn, f"bb_{t}", rows, lambda i: f"({i}, {i}, {i})")


def run_one(args, cell, rows, run):
    tag = f"{cell}-n{rows}-r{run}"
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    wd = os.path.join(args.workdir, tag)
    # A starts first on even runs, B on odd, so neither engine always gets the
    # first-started process id / first-touched file.
    mk = {"B": (args.bin_b, args.port_b), "A": (args.bin_a, args.port_a)}
    labels = ["B", "A"] if run % 2 == 0 else ["A", "B"]
    servers = []
    try:
        for l in labels:
            servers.append(Srv(l, mk[l][0], wd, mk[l][1], args.pin_server, tag))
        servers.sort(key=lambda s: s.label)  # A first in the table; block order is separate
        for s in servers:
            setup(s.conn, cell, rows)
        per_block = max(1, args.ops // args.blocks)
        ctr = {s.label: {"n": 10_000_000, "k": rows} for s in servers}

        def ins(s, tbl, ph):
            c = ctr[s.label]
            c["n"] += 1
            if cell == "c1":
                sql = f"INSERT INTO {tbl} VALUES ({c['n']}, {c['n']})"
            elif cell == "c1s":
                sql = f"INSERT INTO {tbl} VALUES ({c['n']}, '{spill_val(c['n'])}')"
            else:
                c["k"] += 1
                sql = f"INSERT INTO {tbl} VALUES ({c['k']}, {c['n']}, {c['n']})"
            t0 = time.perf_counter()
            r = s.conn.send_command(sql)
            ph.record(time.perf_counter() - t0, r)
            return r

        def ping(s, tbl, ph):
            t0 = time.perf_counter()
            r = s.conn.send_command("SHOW META")
            ph.record(time.perf_counter() - t0, r)

        arms = [("insert", "bb_a", ins), ("insert-again", "bb_b", ins), ("ping", None, ping)]
        res = {s.label: {} for s in servers}
        eng = {s.label: {} for s in servers}
        for name, tbl, fn in arms:
            for s in servers:
                res[s.label][name] = Phase(name)
                eng[s.label][name] = {"polls": 0.0, "us": 0.0}
            for block in range(args.blocks):
                order = servers if block % 2 == 0 else list(reversed(servers))
                for s in order:
                    ph = res[s.label][name]
                    m0 = meta_all(s.conn)
                    t0 = time.perf_counter()
                    for _ in range(per_block):
                        fn(s, tbl, ph)
                    ph.elapsed += time.perf_counter() - t0
                    m1 = meta_all(s.conn)
                    eng[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                    eng[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
        # count of rows really present, per server (equal work + nothing lost)
        counts = {}
        if cell != "c2" or True:
            for s in servers:
                r = s.conn.send_command("SELECT COUNT(*) FROM bb_a")
                counts[s.label] = r.replace("\\n", " ")
        after = host_state()
        payload = {"cell": cell, "rows": rows, "run": run, "tag": tag, "wait_s": round(wait, 1),
                   "order_first": labels[0], "pin_server": args.pin_server,
                   "host_before": before, "host_after": after,
                   "mount": {s.label: {k: s.mount.get(k) for k in
                                       ("recovery_checkpoint_us", "recovery_analysis_us", "recovery_redo_us")}
                             for s in servers},
                   "pids": {s.label: s.pid for s in servers},
                   "count_bb_a": counts,
                   "arms": {l: {n: p.summary() for n, p in r.items()} for l, r in res.items()},
                   "engine": eng}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(payload, f, indent=1)
        a, b = payload["arms"]["A"]["insert"], payload["arms"]["B"]["insert"]
        pa, pb = payload["arms"]["A"]["ping"], payload["arms"]["B"]["ping"]
        errs = sum(p["errors"] for l in payload["arms"].values() for p in l.values())
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} "
              f"insert p50 A={a['p50_us']} B={b['p50_us']} ping A={pa['p50_us']} B={pb['p50_us']} "
              f"errs={errs} ckpt A={payload['mount']['A']['recovery_checkpoint_us']} "
              f"B={payload['mount']['B']['recovery_checkpoint_us']}", flush=True)
    finally:
        for s in servers:
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cell", choices=("c1", "c1s", "c2"), required=True)
    p.add_argument("--rows", default="200,1000,10000")
    p.add_argument("--runs", type=int, default=12)
    p.add_argument("--ops", type=int, default=3000, help="statements per arm per server")
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--port-a", type=int, required=True)
    p.add_argument("--port-b", type=int, required=True)
    p.add_argument("--pin-server", type=int, default=None)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-load", type=float, default=1.0)
    p.add_argument("--max-wait", type=float, default=120)
    args = p.parse_args()
    for rows in [int(x) for x in args.rows.split(",")]:
        for run in range(args.runs):
            run_one(args, args.cell, rows, run)


if __name__ == "__main__":
    sys.exit(main())
