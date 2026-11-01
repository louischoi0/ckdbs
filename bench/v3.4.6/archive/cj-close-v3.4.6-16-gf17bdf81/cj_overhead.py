#!/usr/bin/env python3
"""CJ close: four-arm interleaved overhead driver (scratch; tools/ drivers are two-arm).

Arms A (v3.4.6 base), B (closing commit, ASH on), C (instrumentation compiled out),
O (B's binary, ash_enabled = off). One fresh server + data file per arm per (cell, rows, run),
cores = 1, default durability, each server pinned to its own CPU (rotated per run).
Blocks rotate the arm order (Latin square). Primary measure: server process CPU
(utime+stime from /proc/<pid>/stat) per statement; also client QPS and latency percentiles.
"""
import argparse, json, os, shutil, subprocess, sys, time

TOOLS = "/home/louis/ckdbs/.claude/worktrees/cj-wait-events-ash/tools"
sys.path.insert(0, TOOLS)
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import sys_meta  # noqa

TICK = os.sysconf("SC_CLK_TCK")
ARMS = ["A", "B", "C", "O"]


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        s = f.read().rsplit(")", 1)[1].split()
    return int(s[11]) + int(s[12])  # utime + stime


def host():
    with open("/proc/loadavg") as f:
        load = f.read().strip()
    b = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True).stdout.strip()
    return {"loadavg": load, "competing": b.splitlines() if b else []}


def meta(conn):
    return sys_meta.numeric(sys_meta.from_text(conn.send_command("SHOW META")), float)


def must(conn, sql, prefix):
    r = conn.send_command(sql)
    if not r.startswith(prefix):
        raise SystemExit(f"setup {sql!r} -> {r!r}")


class Srv:
    def __init__(self, arm, binary, wd, port, cpu, ash_off, tag):
        os.makedirs(wd, exist_ok=True)
        self.arm = arm
        data = os.path.join(wd, f"{arm}.db")
        conf = os.path.join(wd, f"{arm}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = 1\n"
                    f"log_file = {arm}.log\nlog_dir = {wd}\nlog_level = warn\nbuffer_pool_frames = 4096\n")
            if ash_off:
                f.write("ash_enabled = off\n")
        self.err = open(os.path.join(wd, f"{arm}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid
        dl = time.time() + 30
        while True:
            try:
                self.conn = ServerConnection("127.0.0.1", port)
                break
            except OSError:
                if time.time() > dl:
                    raise SystemExit(f"{arm} not listening on {port}")
                time.sleep(0.1)

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait()


def run_one(a, cell, rows, run, status):
    tag = f"{cell}-n{rows}-r{run}"
    wd = os.path.join(a.workdir, tag)
    bins = {"A": a.bin_a, "B": a.bin_b, "C": a.bin_c, "O": a.bin_b}
    cpus = [int(x) for x in a.cpus.split(",")]
    order0 = ARMS[run % 4:] + ARMS[:run % 4]  # which arm gets which CPU rotates
    srv = {}
    h0 = host()
    try:
        for i, arm in enumerate(order0):
            srv[arm] = Srv(arm, bins[arm], wd, a.base_port + ARMS.index(arm), cpus[i], arm == "O", tag)
        for s in srv.values():
            must(s.conn, "CREATE TABLE t (id int64, v int64, w int64) BTREE", "CREATED")
            for lo in range(1, rows + 1, 500):
                hi = min(rows, lo + 499)
                must(s.conn, "INSERT INTO t VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
        ph = {arm: Phase(cell) for arm in ARMS}
        cpu = {arm: 0 for arm in ARMS}
        pol = {arm: [0.0, 0.0] for arm in ARMS}
        nxt = {arm: 10_000_000 for arm in ARMS}
        per = a.ops // a.blocks
        k = 0
        for blk in range(a.blocks):
            order = ARMS[blk % 4:] + ARMS[:blk % 4]
            if blk % 2:
                order = list(reversed(order))
            for arm in order:
                s = srv[arm]
                m0 = meta(s.conn); c0 = cpu_ticks(s.pid)
                t0 = time.perf_counter()
                for _ in range(per):
                    k += 1
                    if cell == "read":
                        sql = f"SELECT * FROM t WHERE id = {1 + (k * 7919) % rows}"
                    else:
                        nxt[arm] += 1
                        sql = f"INSERT INTO t VALUES ({nxt[arm]}, {nxt[arm]})"
                    t1 = time.perf_counter()
                    r = s.conn.send_command(sql)
                    ph[arm].record(time.perf_counter() - t1, r)
                ph[arm].elapsed += time.perf_counter() - t0
                c1 = cpu_ticks(s.pid); m1 = meta(s.conn)
                cpu[arm] += c1 - c0
                pol[arm][0] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                pol[arm][1] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
        ok = {}
        for arm in ARMS:
            ok[arm] = srv[arm].conn.send_command("SELECT COUNT(*) FROM t").replace("\\n", " ")
        out = {"cell": cell, "rows": rows, "run": run, "cpu_of": {arm: cpus[order0.index(arm)] for arm in ARMS},
               "host_before": h0, "host_after": host(), "count": ok,
               "arms": {arm: {**ph[arm].summary(), "server_cpu_s": cpu[arm] / TICK,
                              "cpu_us_per_op": cpu[arm] / TICK * 1e6 / max(1, ph[arm].ops),
                              "fg_polls": pol[arm][0], "fg_polled_us": pol[arm][1]} for arm in ARMS}}
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, f"{tag}.json"), "w") as f:
            json.dump(out, f, indent=1)
        line = f"{time.strftime('%H:%M:%S')} {tag} load={h0['loadavg'].split()[0]} " + " ".join(
            f"{arm}:qps={out['arms'][arm]['qps']:.0f},cpu={out['arms'][arm]['cpu_us_per_op']:.1f}us,e={out['arms'][arm]['errors']}" for arm in ARMS)
        print(line, flush=True)
        status.write(line + "\n"); status.flush()
    finally:
        for s in srv.values():
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cell", choices=("read", "insert"), required=True)
    p.add_argument("--rows", default="200,1000,10000")
    p.add_argument("--runs", type=int, default=8)
    p.add_argument("--ops", type=int, default=24000, help="statements per arm per run")
    p.add_argument("--blocks", type=int, default=24)
    p.add_argument("--bin-a", required=True); p.add_argument("--bin-b", required=True); p.add_argument("--bin-c", required=True)
    p.add_argument("--cpus", default="0,1,3,4")
    p.add_argument("--base-port", type=int, default=15710)
    p.add_argument("--workdir", required=True); p.add_argument("--out", required=True)
    p.add_argument("--status", required=True)
    a = p.parse_args()
    with open(a.status, "a") as st:
        for run in range(a.runs):
            for rows in [int(x) for x in a.rows.split(",")]:
                run_one(a, a.cell, rows, run, st)
        st.write("DONE\n")


if __name__ == "__main__":
    main()
