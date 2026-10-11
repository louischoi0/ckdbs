#!/usr/bin/env python3
"""Cells 2 and 3 runner: cores = 2, one server at a time, fresh server + data file per run.

usage: bw_run23.py CELL TRIPLETS ROWS N RESDIR
  CELL  c3c  writer (strict) + mix (core 1) + bystander (pk SELECTs, commits nothing, core 1, client on CPU 3)
        c2   writer (relaxed INSERTs back to back, core 0) + reader (pk SELECTs on t, core 1)
        c3   writer (strict commits back to back, core 0) + mix (SELECT + relaxed INSERT, core 1)
        c3b  writer (strict, core 0) + reader (pk SELECTs only, core 1)      control: no later commit in the sync
  Each triplet runs three servers in a rotating order: A, B, A2 (A2 is binary A again: the floor).
  ROWS = rows of the read relation t (200 / 1000 / 10000); N = reader's SELECTs (c2, c3b) or mix iterations (c3).
Server: taskset 4,6; `cores = 2`, `reactor_cpus = 6,4` (core 0 on CPU 6, core 1 on CPU 4), `durability = relaxed`.
Clients: the core-0 client on CPU 7, the core-1 client on CPU 5.
"""
import itertools, json, os, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bw_host
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-strict-marker-other-session/tools")
from ckdbs_cli import ServerConnection

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = "/home/cdkbs/bench-runs/bw"
CLIENT = os.path.join(HERE, "bw_client.py")
PORT = 15780
CELL, TRIPLETS, ROWS, N, RES = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
WAIT = int(os.environ.get("BW_WAIT", "180"))
BINARY = {"A": "A", "B": "B", "A2": "A"}
os.makedirs(RES, exist_ok=True)
PERMS = list(itertools.permutations(["A", "B", "A2"]))


def wait_port(port, t=30):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return
        except OSError:
            time.sleep(0.2)
    raise SystemExit("port never opened")


def one(r, slot, pos):
    tag = f"{CELL}-n{ROWS}-t{r}-{slot}"
    if os.path.exists(f"{RES}/{tag}.host.json"):
        return
    d = f"{BIN}/work23/{tag}"
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    with open(f"{d}/s.conf", "w") as f:
        f.write(f"data_file = {d}/kds.db\ncores = 2\nreactor_cpus = 6,4\nport = {PORT}\n"
                f"durability = relaxed\nbuffer_pool_frames = 131072\n"
                f"log_dir = {d}\nlog_file = kds.log\nlog_level = warn\n")
    waited = 0
    comp = bw_host.competing()
    while comp and waited < WAIT:
        time.sleep(5); waited += 5
        comp = bw_host.competing()
    la0 = open("/proc/loadavg").read().strip()
    q0 = bw_host.cpu_stat(); time.sleep(1.0); q1 = bw_host.cpu_stat()
    quiet = bw_host.busy_frac(q0, q1)
    proc = subprocess.Popen(["taskset", "-c", "4,6", f"{BIN}/kds_server-{BINARY[slot]}", "--config", f"{d}/s.conf"],
                            stdout=open(f"{d}/stdout", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        wait_port(PORT)
        c = ServerConnection("127.0.0.1", PORT, timeout=30.0)
        for sql in ("CREATE TABLE t (id int64, g int64, v int64) BTREE",
                    "CREATE TABLE w (id int64, g int64, v int64) BTREE",
                    "CREATE TABLE m (id int64, g int64, v int64) BTREE"):
            assert c.send_command(sql).startswith("CREATED"), sql
        for lo in range(1, ROWS + 1, 250):
            rows = ",".join(f"({i},{i},0)" for i in range(lo, min(ROWS, lo + 249) + 1))
            assert c.send_command(f"INSERT INTO t VALUES {rows}").startswith("INSERTED")
        c.close()
        stop = f"{d}/stop"
        start_at = time.time() + 6.0
        smp = bw_host.Sampler().start()
        common = ["--port", str(PORT), "--rows", str(ROWS), "--stop", stop, "--start-at", str(start_at)]
        if CELL == "c2":
            wr = ["--role", "writer", "--mode", "relaxed"]
            rd = ["--role", "reader", "--n", str(N)]
        elif CELL == "c3":
            wr = ["--role", "writer", "--mode", "strict"]
            rd = ["--role", "mix", "--n", str(N)]
        elif CELL == "c3c":
            wr = ["--role", "writer", "--mode", "strict"]
            rd = ["--role", "mix", "--n", str(N)]
        else:
            wr = ["--role", "writer", "--mode", "strict"]
            rd = ["--role", "reader", "--n", str(N)]
        pw = subprocess.Popen(["taskset", "-c", "7", "python3", "-u", CLIENT, "--want-core", "0",
                               "--out", f"{RES}/{tag}.writer.json.gz"] + common + wr, cwd=d,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        pr = subprocess.Popen(["taskset", "-c", "5", "python3", "-u", CLIENT, "--want-core", "1",
                               "--out", f"{RES}/{tag}.reader.json.gz"] + common + rd, cwd=d,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        pb = None
        if CELL == "c3c":
            pb = subprocess.Popen(["taskset", "-c", "3", "python3", "-u", CLIENT, "--want-core", "1",
                                   "--out", f"{RES}/{tag}.bystander.json.gz", "--role", "bystander"] + common, cwd=d,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        outr = pr.communicate(timeout=900)[0]
        outw = pw.communicate(timeout=120)[0]
        if pb is not None:
            outw += pb.communicate(timeout=120)[0]
        hs = smp.stop()
        comp_end = bw_host.competing()
        rec = {"tag": tag, "cell": CELL, "slot": slot, "binary": BINARY[slot], "triplet": r, "pos": pos, "rows": ROWS, "n": N,
               "start_utc": time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t0)), "loadavg_start": la0,
               "loadavg_end": open("/proc/loadavg").read().strip(), "competing_start": comp, "competing_end": comp_end,
               "waited_s": waited, "duration_s": round(time.time() - t0, 1), "cpu_busy_1s_before": quiet,
               "cpu_busy_mean": hs["mean"], "cpu_busy_max_1s": hs["max"], "samples": hs["n"],
               "rc_reader": pr.returncode, "rc_bystander": pb.returncode if pb else None, "rc_writer": pw.returncode, "client_out": (outr + outw)[-600:]}
        json.dump(rec, open(f"{RES}/{tag}.host.json", "w"))
        oth = [v for c_, v in hs["mean"].items() if c_ not in (3, 4, 5, 6, 7)]
        print(tag, "rc", pr.returncode, pw.returncode, "load", la0.split()[0], "dur %.0fs" % rec["duration_s"],
              "cpu4-7=%s" % [hs["mean"].get(i) for i in (4, 5, 6, 7)], "others_mean_max=%s" % max(oth),
              "comp=%s" % (len(comp) + len(comp_end)), flush=True)
        if pr.returncode or pw.returncode:
            print(outr[-500:], outw[-500:], flush=True)
    finally:
        proc.terminate()
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
        shutil.rmtree(d, ignore_errors=True)


for r in range(1, TRIPLETS + 1):
    order = PERMS[(r - 1) % 6]
    for pos, slot in enumerate(order):
        one(r, slot, pos)
print("ALLDONE")
