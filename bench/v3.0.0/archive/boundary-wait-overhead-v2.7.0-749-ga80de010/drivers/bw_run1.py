#!/usr/bin/env python3
"""Cell 1 runner: fresh servers + data files per (rows, run), both servers pinned to CPU 6, client to CPU 7.

usage: bw_run1.py RUNS ROWS[,ROWS...] RESDIR [extra driver args...]
BW_SWAP=1: the A slot runs binary B and vice versa (the slot-swap pass).
Before each run: waits (up to BW_WAIT s) until no compile/test process is live; the host state is
recorded at the run's start and end, and a run that started beside one is flagged, never silently kept.
"""
import json, os, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bw_host

BIN = "/home/cdkbs/bench-runs/bw"
DRV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bw_ab.py")
PORT = {"A": 15761, "B": 15760}
RUNS = int(sys.argv[1])
SIZES = [int(x) for x in sys.argv[2].split(",")]
RES = sys.argv[3]
EXTRA = sys.argv[4:]
WAIT = int(os.environ.get("BW_WAIT", "180"))
os.makedirs(RES, exist_ok=True)
SWAPMAP = {"A": "B", "B": "A"} if os.environ.get("BW_SWAP") == "1" else {"A": "A", "B": "B"}


def wait_port(port, t=30):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return
        except OSError:
            time.sleep(0.2)
    raise SystemExit(f"port {port} never opened")


for r in range(1, RUNS + 1):
    for n in SIZES:
        tag = f"n{n}-run{r}"
        if os.path.exists(f"{RES}/{tag}.json"):
            continue
        d = f"{BIN}/work1/{tag}"
        shutil.rmtree(d, ignore_errors=True)
        for lab in ("A", "B"):
            os.makedirs(f"{d}/{lab}")
            with open(f"{d}/{lab}.conf", "w") as f:
                f.write(f"data_file = {d}/{lab}/kds.db\ncores = 1\nport = {PORT[lab]}\n"
                        f"durability = relaxed\nbuffer_pool_frames = 131072\n"
                        f"log_dir = {d}/{lab}\nlog_file = kds.log\nlog_level = warn\n")
        waited = 0
        comp = bw_host.competing()
        while comp and waited < WAIT:
            time.sleep(5); waited += 5
            comp = bw_host.competing()
        la0 = open("/proc/loadavg").read().strip()
        q0 = bw_host.cpu_stat(); time.sleep(1.0); q1 = bw_host.cpu_stat()
        quiet = bw_host.busy_frac(q0, q1)
        procs = []
        for lab in ("A", "B"):
            procs.append(subprocess.Popen(
                ["taskset", "-c", "6", f"{BIN}/kds_server-{SWAPMAP[lab]}", "--config", f"{d}/{lab}.conf"],
                stdout=open(f"{d}/{lab}/stdout", "w"), stderr=subprocess.STDOUT))
        t0 = time.time()
        smp = bw_host.Sampler().start()
        try:
            wait_port(PORT["A"]); wait_port(PORT["B"])
            p = subprocess.run(
                ["taskset", "-c", "7", "python3", "-u", DRV, "--port", str(PORT["B"]), "--label", "B",
                 "--ab-port", str(PORT["A"]), "--ab-label", "A", "--rows", str(n),
                 "--json", f"{RES}/{tag}.json"] + EXTRA,
                capture_output=True, text=True, cwd=d)
            hs = smp.stop()
            comp_end = bw_host.competing()
            open(f"{RES}/{tag}.driver.txt", "w").write(p.stdout + p.stderr)
            la1 = open("/proc/loadavg").read().strip()
            rec = {"tag": tag, "start_utc": time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t0)),
                   "loadavg_start": la0, "loadavg_end": la1, "competing_start": comp, "competing_end": comp_end,
                   "waited_s": waited, "duration_s": round(time.time() - t0, 1), "cpu_busy_1s_before": quiet,
                   "cpu_busy_mean": hs["mean"], "cpu_busy_max_1s": hs["max"], "samples": hs["n"],
                   "swap": os.environ.get("BW_SWAP") == "1", "rc": p.returncode}
            json.dump(rec, open(f"{RES}/{tag}.host.json", "w"))
            oth = {c: v for c, v in hs["mean"].items() if c not in (6, 7)}
            print(tag, "rc", p.returncode, "load", la0.split()[0], "->", la1.split()[0], "dur %.0fs" % rec["duration_s"],
                  "cpu6/7=%s/%s" % (hs["mean"].get(6), hs["mean"].get(7)),
                  "others_mean_max=%s" % max(oth.values()), "others_1s_max=%s" % max(
                      v for c, v in hs["max"].items() if c not in (6, 7)),
                  "comp=%s" % (len(comp) + len(comp_end)), flush=True)
            if p.returncode:
                print(p.stdout[-800:], p.stderr[-800:], flush=True)
        finally:
            for pr in procs:
                pr.terminate()
            for pr in procs:
                try:
                    pr.wait(15)
                except subprocess.TimeoutExpired:
                    pr.kill(); pr.wait()
            shutil.rmtree(d, ignore_errors=True)
print("ALLDONE")
