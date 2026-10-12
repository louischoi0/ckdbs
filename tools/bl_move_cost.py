#!/usr/bin/env python3
"""BL §5.1's move-cost cell: what one move costs a transaction.

`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`.
One session runs a prepared pk point read in a closed loop against two
servers of one binary: one where no session moves, and one started with
`KDS_BL_PROBE_MOVES` set, which a measurement build (never committed: the
patch is `bench/v3.0.0/archive/bl-s1-v2.7.0-857-g3552a03c/probe-moves.patch`)
reads to move every transaction's head to the next core. The p50 difference
is what one move costs a statement; the dwell amortises it (BL-R4). Blocks
alternate between the two servers so host drift lands on both. **A build
without the patch moves nothing** - the one session is its core's whole load,
which `LoadPlacer` never moves - so the tool refuses a run whose move arm
counted no move (`load_c<k>_moves_out`).

    tools/bl_move_cost.py --bin <probe build> --port P --cores 2 --blocks 20
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import nearest_rank  # noqa: E402
from bb_overhead_benchmark import host_state, meta_all  # noqa: E402
from ba_census import data_device  # noqa: E402
from kwp import Connection  # noqa: E402


class _Shim:
    """`meta_all`'s connection: one text reply for one statement."""

    def __init__(self, conn):
        self.conn = conn

    def send_command(self, sql):
        _f, rows, tag, _a = self.conn.execute(sql)
        return " ".join(str(c) for r in rows for c in r) if rows else tag


def start(args, wd, port, moves):
    os.makedirs(wd, exist_ok=True)
    conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {wd}/d.db\nport = {port}\ncores = {args.cores}\n"
                f"durability = relaxed\nbuffer_pool_frames = 65536\nlog_dir = {wd}\n"
                f"log_level = warn\nreactor_cpus = {args.server_cpus}\n")
    env = dict(os.environ)
    env.pop("KDS_BL_PROBE_MOVES", None)
    if moves:
        env["KDS_BL_PROBE_MOVES"] = "1"
    proc = subprocess.Popen([args.bin, "--config", conf], env=env,
                            stdout=open(os.path.join(wd, "s.stderr"), "w"), stderr=subprocess.STDOUT)
    for _ in range(300):
        try:
            c = Connection("127.0.0.1", port)
            return proc, c
        except OSError:
            time.sleep(0.1)
    raise SystemExit("server did not listen")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--cores", type=int, default=2)
    p.add_argument("--server-cpus", default="0,2")
    p.add_argument("--blocks", type=int, default=20)
    p.add_argument("--per-block", type=int, default=2000)
    p.add_argument("--workdir", default=os.path.expanduser("~/bench-runs/bl/work-move"))
    p.add_argument("--out", required=True)
    args = p.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    copy = os.path.join(args.workdir, "kds_server.move")
    shutil.copy2(args.bin, copy)
    args.bin = copy
    sha = hashlib.sha256(open(copy, "rb").read()).hexdigest()
    servers = {}
    lat = {"stay": [], "move": []}
    try:
        for arm, moves, port in (("stay", False, args.port), ("move", True, args.port + 1)):
            proc, c = start(args, os.path.join(args.workdir, arm), port, moves)
            c.execute("CREATE TABLE mc (id int64, v int64)")
            c.execute("INSERT INTO mc VALUES (1), (2), (3), (4)")
            c.prepare("q", "SELECT v FROM mc WHERE id = 2")
            servers[arm] = (proc, c)
        before = host_state()
        for b in range(args.blocks):
            order = ("stay", "move") if b % 2 == 0 else ("move", "stay")
            for arm in order:
                c = servers[arm][1]
                for _ in range(args.per_block):
                    t0 = time.perf_counter()
                    c.execute_prepared("q")
                    lat[arm].append(time.perf_counter() - t0)
        after = host_state()
        moved = sum(v for k, v in meta_all(_Shim(servers["move"][1])).items() if k.endswith("_moves_out"))
        if moved < args.blocks * args.per_block / 2:
            raise SystemExit(f"the move arm counted {moved:.0f} moves: not a probe build")
        res = {"cores": args.cores, "blocks": args.blocks, "per_block": args.per_block,
               "bin_sha256": sha, "data_device": data_device(args.workdir),
               "host_before": before, "host_after": after}
        for arm, xs in lat.items():
            xs.sort()
            res[arm] = {f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)}
            res[arm]["n"] = len(xs)
        res["move_minus_stay_p50_us"] = round(res["move"]["p50_us"] - res["stay"]["p50_us"], 1)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(res, f, indent=1)
        print(json.dumps({k: res[k] for k in ("stay", "move", "move_minus_stay_p50_us")}))
    finally:
        for proc, c in servers.values():
            proc.terminate()
            proc.wait(30)


if __name__ == "__main__":
    sys.exit(main())
