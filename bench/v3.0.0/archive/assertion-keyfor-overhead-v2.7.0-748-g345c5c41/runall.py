#!/usr/bin/env python3
"""Runs the A/B pairs: fresh servers + data files per (gtype, preload, run)."""
import os, shutil, socket, subprocess, sys, time

OUT = "/home/cdkbs/bench-runs/keyfor"
DRV = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/drv/assertion_keyfor_ab.py"
PORT = {"A": 15601, "B": 15600}
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 7
CELLS = [("int", 200), ("int", 1000), ("int", 10000), ("date", 200), ("date", 1000), ("date", 10000)]
if len(sys.argv) > 2:
    CELLS = [c for c in CELLS if c[0] in sys.argv[2].split(",")]


def wait_port(port, t=30):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return
        except OSError:
            time.sleep(0.2)
    raise SystemExit(f"port {port} never opened")


def quiet():
    # bounded wait for a quiet host
    for _ in range(60):
        la = float(open("/proc/loadavg").read().split()[0])
        if la < 1.2:
            return la
        time.sleep(2)
    return la


for r in range(1, RUNS + 1):
    for gtype, n in CELLS:
        d = f"{OUT}/{gtype}-n{n}-run{r}"
        if os.path.exists(d + "/result.json"):
            continue
        shutil.rmtree(d, ignore_errors=True)
        procs = []
        for lab in ("A", "B"):
            os.makedirs(f"{d}/{lab}")
            with open(f"{d}/{lab}.conf", "w") as f:
                f.write(f"data_file = {d}/{lab}/kds.db\ncores = 1\nport = {PORT[lab]}\n"
                        f"durability = relaxed\nbuffer_pool_frames = 131072\n"
                        f"log_dir = {d}/{lab}\nlog_file = kds.log\nlog_level = warn\n")
        la = quiet()
        host = subprocess.run("echo loadavg_at_start=%s; cat /proc/loadavg; pgrep -a -f 'cc1plus|cmake --build|ctest' || echo no-competing" % la,
                              shell=True, capture_output=True, text=True).stdout
        open(f"{d}/host.txt", "w").write(host)
        for lab in ("A", "B"):
            procs.append(subprocess.Popen(
                ["taskset", "-c", "2", f"{OUT}/kds_server-{lab}", "--config", f"{d}/{lab}.conf"],
                stdout=open(f"{d}/{lab}/stdout", "w"), stderr=subprocess.STDOUT))
        try:
            wait_port(PORT["A"]); wait_port(PORT["B"])
            p = subprocess.run(
                ["taskset", "-c", "4", "python3", "-I" if False else "-u", DRV, "--port", str(PORT["B"]), "--label", "B",
                 "--ab-port", str(PORT["A"]), "--ab-label", "A", "--gtype", gtype, "--preload", str(n),
                 "--ops", "8000", "--blocks", "16", "--json", f"{d}/result.json"],
                capture_output=True, text=True, cwd=d)
            open(f"{d}/driver.txt", "w").write(p.stdout + p.stderr)
            print(gtype, n, r, "rc", p.returncode, flush=True)
        finally:
            for pr in procs:
                pr.terminate()
            for pr in procs:
                try:
                    pr.wait(15)
                except subprocess.TimeoutExpired:
                    pr.kill(); pr.wait()
            # keep configs/results, drop data and WAL
            for lab in ("A", "B"):
                shutil.rmtree(f"{d}/{lab}", ignore_errors=True)
print("ALLDONE")
