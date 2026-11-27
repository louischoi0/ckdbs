#!/usr/bin/env python3
"""DV close cells 1-2: interleaved A/B, server CPU per unit. Modelled on tools/du_close.py
(Arm, run_block, pcts, host_state copied; shapes are DV's). Not a tools/ driver."""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time
sys.path.insert(0, "/home/ukjin/workspaces/ckdbs/.claude/worktrees/dv-unreachable-page-reclaim/tools")
import kwp
from bench_common import nearest_rank
SMALL = 1000

def host_state():
    load = open("/proc/loadavg").read().strip()
    busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True).stdout.strip()
    busy = [l for l in busy.splitlines() if "pgrep" not in l]
    return {"loadavg": load, "busy": len(busy)}
def sched_ns(pid):
    t = 0
    for tid in os.listdir(f"/proc/{pid}/task"):
        try: t += int(open(f"/proc/{pid}/task/{tid}/schedstat").read().split()[0])
        except OSError: pass
    return t
def pcts(xs):
    s = sorted(xs); us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)), "p50_us": us(nearest_rank(s, 50)),
            "p95_us": us(nearest_rank(s, 95)), "p99_us": us(nearest_rank(s, 99)), "max_us": us(s[-1])}
def dec(x):
    if isinstance(x, bytes):
        if len(x) == 8: return int.from_bytes(x, 'little', signed=True)
        return x.decode(errors='replace')
    return x
def must(conn, sql, tag):
    got = conn.execute(sql)[2]
    if not got.startswith(tag): raise SystemExit(f"{sql[:90]!r} -> {got!r}")
    return got

class Arm:
    def __init__(self, wd, label, source, port):
        self.label = label
        self.binary = os.path.join(wd, f"kds_server.{label}")
        shutil.copy2(source, self.binary)
        self.sha = hashlib.sha256(open(self.binary, "rb").read()).hexdigest()
        self.mtime = os.path.getmtime(source)
        data = os.path.join(wd, f"ab-{label}.db")
        for p in (data,): 
            if os.path.exists(p): os.remove(p)
        shutil.rmtree(data + ".wal", ignore_errors=True)
        conf = os.path.join(wd, f"ab-{label}.conf")
        open(conf, "w").write(f"data_file = {data}\nport = {port}\ncores = 1\ndurability = relaxed\n"
            f"buffer_pool_frames = 131072\nlog_file = ab-{label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(os.path.join(wd, f"ab-{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", "0-7", self.binary, "--config", conf], stdout=self.err, stderr=subprocess.STDOUT)
        dl = time.time() + 30
        while True:
            try: self.conn = kwp.Connection("127.0.0.1", port, timeout=600); break
            except OSError:
                if time.time() > dl or self.proc.poll() is not None: raise SystemExit(f"{label} no listen")
                time.sleep(0.1)
        self.next_id = 1; self.ctr = 0
    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: self.proc.kill()
        self.err.close()

def load(arm):
    c = arm.conn
    must(c, "CREATE TABLE s (id int64, owner int64, v int64) BTREE", "CREATE")
    for lo in range(1, SMALL + 1, 500):
        must(c, "INSERT INTO s VALUES " + ",".join(f"({i}, {i % 100}, 0)" for i in range(lo, lo + 500)), "INSERT")
    must(c, "CREATE TABLE i (id int64, k int64, v int64) BTREE", "CREATE")

# a shape: (name, units a block, fn(arm, rng) -> runs one unit; statements per unit)
def u_insert(arm, r):
    n = arm.next_id; arm.next_id += 1
    got = arm.conn.execute(f"INSERT INTO i VALUES ({n}, {n % 97}, 0)")[2]
    assert got.startswith("INSERTED"), got
def u_sel(arm, r): arm.conn.execute(f"SELECT v FROM s WHERE id = {r.randrange(1, SMALL + 1)}")
def u_upd(arm, r):
    got = arm.conn.execute(f"UPDATE s SET v = v + 1 WHERE id = {r.randrange(1, SMALL + 1)}")[2]
    assert got.split()[1] == "1", got
def u_cd(arm, r):
    arm.ctr += 1; n = f"ct_{arm.ctr}"
    must(arm.conn, f"CREATE TABLE {n} (id int64, v int64) BTREE", "CREATE")
    must(arm.conn, f"DROP TABLE {n}", "DROP")
def u_bcr(arm, r):
    arm.ctr += 1; n = f"cr_{arm.ctr}"
    must(arm.conn, "BEGIN", "BEGIN")
    must(arm.conn, f"CREATE TABLE {n} (id int64, v int64) BTREE", "CREATE")
    must(arm.conn, "ROLLBACK", "ROLLBACK")

def run_block(arm, fn, count, rng):
    lat = []; c0 = sched_ns(arm.proc.pid)
    for _ in range(count):
        t0 = time.perf_counter(); fn(arm, rng); lat.append(time.perf_counter() - t0)
    return lat, sched_ns(arm.proc.pid) - c0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True); ap.add_argument("--workdir", required=True)
    ap.add_argument("--out", required=True); ap.add_argument("--status", required=True)
    ap.add_argument("--reps", type=int, default=7); ap.add_argument("--port", type=int, default=31990)
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args()
    os.makedirs(a.workdir, exist_ok=True); os.sched_setaffinity(0, {8})
    def status(t): open(a.status, "w").write(f"{time.time():.0f} {t}\n")
    shapes = [("insert-keyed", 5000, u_insert), ("point-select", 5000, u_sel), ("point-update", 5000, u_upd),
              ("create-drop", 150, u_cd), ("begin-create-rollback", 150, u_bcr)]
    arms = []; res = {"reps": a.reps, "arms": {}, "shapes": []}
    try:
        for k, spec in enumerate(a.arm):
            label, src = spec.split("=", 1); arm = Arm(a.workdir, label, src, a.port + k); arms.append(arm); load(arm)
            res["arms"][label] = {"sha256": arm.sha, "mtime": arm.mtime}
        time.sleep(1)
        for name, count, fn in shapes:
            count = int(count * a.scale)
            for arm in arms: run_block(arm, fn, max(5, count // 5), random.Random(11))
            acc = {x.label: {"lat": [], "cpu": [], "host": []} for x in arms}
            for rep in range(a.reps):
                order = arms[rep % len(arms):] + arms[:rep % len(arms)]
                for arm in order:
                    status(f"{name} rep={rep} arm={arm.label}")
                    acc[arm.label]["host"].append(host_state())
                    lat, ns = run_block(arm, fn, count, random.Random(7919 * (rep + 1)))
                    acc[arm.label]["lat"] += lat; acc[arm.label]["cpu"].append(round(ns / count / 1e3, 2))
            cell = {"shape": name, "units_per_block": count, "arms": {}}
            for l, x in acc.items(): cell["arms"][l] = {"cpu_us_per_unit_per_rep": x["cpu"], "latency": pcts(x["lat"]), "host": x["host"]}
            res["shapes"].append(cell)
            json.dump(res, open(a.out, "w"), indent=1)
        for arm in arms:
            try: res["arms"][arm.label]["wait_stats"] = [[dec(x) for x in r] for r in arm.conn.execute("SELECT core, event, class, count, total_us, max_us FROM sys.wait_stats")[1]]
            except Exception as e: res["arms"][arm.label]["wait_stats"] = f"err {e}"
        json.dump(res, open(a.out, "w"), indent=1)
    finally:
        for arm in arms: arm.stop()
    status("done")
main()
