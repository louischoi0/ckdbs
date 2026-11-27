#!/usr/bin/env python3
"""DV close cells 3-4 helpers. build: A-built volume snapshots at 0.25/1/4 GiB; mount: time start->listening."""
import hashlib, os, shutil, socket, subprocess, sys, time, json
sys.path.insert(0, "/home/ukjin/workspaces/ckdbs/.claude/worktrees/dv-unreachable-page-reclaim/tools")
import kwp
os.sched_setaffinity(0, {9})
R = "/home/ukjin/.cache/dv-close-run"
PORT = 31995
def conf(d, extra=""):
    p = f"{d}/m.conf"
    open(p, "w").write(f"data_file = {d}/m.db\nport = {PORT}\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 131072\n"
                       f"log_file = m.log\nlog_dir = {d}\nlog_level = info\n{extra}")
    return p
def start(binary, d, extra=""):
    t0 = time.perf_counter()
    p = subprocess.Popen(["taskset", "-c", "0-7", binary, "--config", conf(d, extra)], stdout=open(f"{d}/m.stderr", "w"), stderr=subprocess.STDOUT)
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1); s.close(); break
        except OSError:
            if p.poll() is not None: raise SystemExit("server exited " + open(f"{d}/m.stderr").read()[-300:])
            time.sleep(0.002)
    return p, time.perf_counter() - t0
def stop(p):
    p.terminate(); p.wait(120)
def must(c, sql, tag):
    g = c.execute(sql)[2]
    if not g.startswith(tag): raise SystemExit(f"{sql[:80]!r} -> {g!r}")
def conn(): return kwp.Connection("127.0.0.1", PORT, timeout=600)
PAD = "x" * 1000
def waystone(c, first):
    if first:
        for t in range(16):
            must(c, f"CREATE TABLE w{t} (id int64, v int64) BTREE", "CREATE")
            must(c, "INSERT INTO w%d VALUES " % t + ",".join(f"({i}, {i})" for i in range(1, 1001)), "INSERT")
            print(c.execute(f"WAYSTONE ON SELECT v FROM w{t} WHERE id = 5")[2], flush=True)
    for t in range(16):
        for k in range(1, 41):
            for _ in range(2): c.execute(f"SELECT v FROM w{t} WHERE id = {k * 37 % 1000 + 1}")
def build():
    d = f"{R}/build"; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    shutil.copy2(f"{R}/kds_server.A", f"{d}/srv")
    next_id = 1; targets = [0.25, 1.0, 4.0]; first = True
    for tg in targets:
        p, t = start(f"{d}/srv", d); c = conn()
        if first:
            must(c, "CREATE TABLE big (id int64, k int64, pad char(1000)) BTREE", "CREATE")
        waystone(c, first); first = False
        t0 = time.time()
        while os.path.getsize(f"{d}/m.db") < tg * 2**30 * 1.0:
            must(c, "INSERT INTO big VALUES " + ",".join(f"({i}, {i % 1000}, '{PAD}')" for i in range(next_id, next_id + 1000)), "INSERT")
            next_id += 1000
            open(f"{R}/build.status", "w").write(f"{time.time():.0f} rows={next_id} size={os.path.getsize(f'{d}/m.db')}\n")
        waystone(c, False)
        c.close(); stop(p)
        snap = f"{R}/vol{tg}"; shutil.rmtree(snap, ignore_errors=True); shutil.copytree(d, snap, ignore=shutil.ignore_patterns("srv"))
        print("snapshot", tg, next_id, os.path.getsize(f"{d}/m.db"), "loadsecs", round(time.time() - t0), flush=True)
def mount(label, binary, vol, extra="", n=1):
    out = []
    for i in range(n):
        d = f"{R}/mnt-{label}"; shutil.rmtree(d, ignore_errors=True); shutil.copytree(vol, d)
        shutil.copy2(binary, f"{d}/srv")
        p, t = start(f"{d}/srv", d, extra); stop(p)
        log = open(f"{d}/m.log").read()
        out.append((round(t * 1000, 1), [l for l in log.splitlines() if "census" in l]))
        shutil.rmtree(d)
    return out
if __name__ == "__main__":
    if sys.argv[1] == "build": build()
