#!/usr/bin/env python3
"""CY close diagnostic: why does a looping GATHER at cores > 1 slow an OLTP session?
Starts one server (B, the close binary), loads a small copy of the workload, then reads
sys.meta plan_stats_* / plan_memo_* / lock counters and the sessions' cores before and after
N seconds of a GATHER loop from a second, background-priority session, with the OLTP
session running point reads throughout.
usage: diag.py <tools> <server-binary> <wd> <cores> <cpus> <port> <seconds>"""
import os, subprocess, sys, time, random, shutil
tools, binary, wd, cores, cpus, port, secs = sys.argv[1:8]
sys.path.insert(0, tools)
import kwp
os.makedirs(wd, exist_ok=True)
data = os.path.join(wd, "D.db")
shutil.rmtree(data + ".wal", ignore_errors=True)
if os.path.exists(data): os.remove(data)
conf = os.path.join(wd, "D.conf")
open(conf, "w").write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = group\n"
                      f"buffer_pool_frames = 65536\nlog_file = D.log\nlog_dir = {wd}\nlog_level = warn\n")
proc = subprocess.Popen(["taskset", "-c", cpus, binary, "--config", conf], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
for _ in range(100):
    try: oltp = kwp.Connection("127.0.0.1", int(port), timeout=300, client_name="diag"); break
    except OSError: time.sleep(0.1)
adm = kwp.Connection("127.0.0.1", int(port), timeout=300, client_name="diag_adm")
def must(c, q):
    t, n, _b, e = c.query(q)
    if e is not None: sys.exit(f"{q}: {e}")
def text(c, q):
    _f, rows, _t, _n = c.execute(q)
    return [" | ".join((x.decode(errors="replace") if x is not None else "NULL") for x in r) for r in rows]
must(oltp, "CREATE TABLE big (id int64, v int64) BTREE")
must(oltp, "CREATE TABLE small (id int64, v int64) BTREE")
for i in range(0, 60000, 500):
    must(oltp, "INSERT INTO big (id, v) VALUES " + ", ".join(f"({j}, {j})" for j in range(i + 1, i + 501)))
must(oltp, "INSERT INTO small (id, v) VALUES " + ", ".join(f"({j}, {j})" for j in range(1, 101)))
must(adm, "SET PRIORITY background")
def meta(tag):
    keys = ("plan_stats_generation", "plan_stats_publishes", "plan_memo_hits", "plan_memo_misses", "captured_plans")
    rows = [l for l in text(oltp, "SELECT * FROM sys.meta") if any(k in l for k in keys) or "| core |" in l]
    print(tag, [r.split(" | ", 1)[1] for r in rows], flush=True)
def sessions(tag):
    core = lambda c: [l for l in text(c, "SELECT * FROM sys.meta") if l.startswith("volume | core")]
    print(tag, "oltp session core:", core(oltp), "admin session core:", core(adm), flush=True)
def reads(n):
    r = random.Random(1); lat = []
    for _ in range(n):
        t0 = time.perf_counter(); must(oltp, f"SELECT a.id, b.v FROM small AS a JOIN small AS b ON a.v = b.id JOIN small AS c ON b.v = c.id WHERE a.id BETWEEN {r.randint(1, 50)} AND 60"); lat.append(time.perf_counter() - t0)
    lat.sort(); return lat[len(lat) // 2] * 1e6, lat[int(len(lat) * .99)] * 1e6
print("idle  p50/p99 us", reads(3000)); meta("before"); sessions("before")
g = subprocess.Popen(["taskset", "-c", "5", sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gatherer.py"), port, "big", tools] + ([os.environ["GSUFFIX"]] if "GSUFFIX" in os.environ else []),
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
g.stdout.readline(); g.stdin.write("on\n"); g.stdin.flush()
t0 = time.time(); print("ON    p50/p99 us", reads(3000)); sessions("during"); meta("after")
g.stdin.write("off\n"); g.stdin.flush(); print("gathers", g.stdout.readline().strip())
print("OFF   p50/p99 us", reads(3000)); meta("after-off")
g.stdin.write("quit\n"); g.stdin.flush(); proc.terminate(); proc.wait()
