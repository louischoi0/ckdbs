#!/usr/bin/env python3
"""Runs the A/B pairs: fresh servers + data files per (rows, run).

usage: nh_run.py RUNS ROWS[,ROWS...] [OUTDIR] [extra driver args...]
Server CPU pin 6 (both servers; one is idle while the other is timed), client pin 7.
"""
import os, shutil, socket, subprocess, sys, time

OUT = os.environ.get("NH_OUT", "/home/cdkbs/bench-runs/nhf")
DRV = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nhf/nh_ab.py"
PORT = {"A": 15621, "B": 15620}
RUNS = int(sys.argv[1])
SIZES = [int(x) for x in sys.argv[2].split(",")]
RES = sys.argv[3] if len(sys.argv) > 3 else f"{OUT}/res"
EXTRA = sys.argv[4:]
os.makedirs(RES, exist_ok=True)
# NH_SWAP=1: the A slot (port 15621, driver label A) runs the B binary and vice versa.
SWAPMAP = {"A": "B", "B": "A"} if os.environ.get("NH_SWAP") == "1" else {"A": "A", "B": "B"}


def wait_port(port, t=30):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return
        except OSError:
            time.sleep(0.2)
    raise SystemExit(f"port {port} never opened")


def cpu_busy():
    out = {}
    with open("/proc/stat") as f:
        for l in f:
            if l.startswith("cpu") and l[3].isdigit():
                p = l.split()
                v = [int(x) for x in p[1:]]
                idle = v[3] + v[4]
                out[int(l[3:].split()[0])] = (sum(v), idle)
    return out


def busy_frac(a, b):
    r = {}
    for c in a:
        dt = b[c][0] - a[c][0]
        r[c] = round(1 - (b[c][1] - a[c][1]) / dt, 3) if dt else 0.0
    return r


for r in range(1, RUNS + 1):
    for n in SIZES:
        tag = f"n{n}-run{r}"
        if os.path.exists(f"{RES}/{tag}.json"):
            continue
        d = f"{OUT}/work/{tag}"
        shutil.rmtree(d, ignore_errors=True)
        for lab in ("A", "B"):
            os.makedirs(f"{d}/{lab}")
            with open(f"{d}/{lab}.conf", "w") as f:
                f.write(f"data_file = {d}/{lab}/kds.db\ncores = 1\nport = {PORT[lab]}\n"
                        f"durability = relaxed\nbuffer_pool_frames = 131072\n"
                        f"log_dir = {d}/{lab}\nlog_file = kds.log\nlog_level = warn\n")
        la0 = open("/proc/loadavg").read().strip()
        comp = subprocess.run("pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | cut -c1-120 || true",
                              shell=True, capture_output=True, text=True).stdout.strip()
        procs = []
        for lab in ("A", "B"):
            procs.append(subprocess.Popen(
                ["taskset", "-c", "6", f"{OUT}/kds_server-{SWAPMAP[lab]}", "--config", f"{d}/{lab}.conf"],
                stdout=open(f"{d}/{lab}/stdout", "w"), stderr=subprocess.STDOUT))
        t0 = time.time()
        c0 = cpu_busy()
        try:
            wait_port(PORT["A"]); wait_port(PORT["B"])
            p = subprocess.run(
                ["taskset", "-c", "7", "python3", "-u", DRV, "--port", str(PORT["B"]), "--label", "B",
                 "--ab-port", str(PORT["A"]), "--ab-label", "A", "--rows", str(n),
                 "--json", f"{RES}/{tag}.json"] + EXTRA,
                capture_output=True, text=True, cwd=d)
            c1 = cpu_busy()
            open(f"{RES}/{tag}.driver.txt", "w").write(p.stdout + p.stderr)
            la1 = open("/proc/loadavg").read().strip()
            with open(f"{RES}/{tag}.host.txt", "w") as f:
                f.write(f"loadavg_start={la0}\nloadavg_end={la1}\ncompeting={comp or 'none'}\n"
                        f"duration_s={time.time()-t0:.1f}\ncpu_busy={busy_frac(c0, c1)}\n")
            print(tag, "rc", p.returncode, "load", la0.split()[0], "->", la1.split()[0],
                  "dur %.0fs" % (time.time() - t0), "cpu6/7=%s/%s" % (busy_frac(c0, c1)[6], busy_frac(c0, c1)[7]),
                  flush=True)
            if p.returncode:
                print(p.stderr[-800:], flush=True)
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
