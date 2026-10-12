#!/usr/bin/env python3
"""BP-H10's cell: tools/lock_contention_benchmark.py (unmodified) against a fresh
server of `cores = K`, optionally with one range fence held on the benchmark's
relation (`c3_lock`) for the whole run by an explicit transaction that writes a
key window no benchmark statement touches.

The fence: `BEGIN; UPDATE c3_lock SET v = 0 WHERE id >= lo AND id <= hi` declares
[lo, hi] in X for the transaction. Two windows alternate (a new one is taken before
the old one is rolled back) so a fence is held throughout despite the 60 s
transaction ceiling.
"""
import argparse, glob, hashlib, json, os, shutil, subprocess, sys, time, threading


def cpu_ns(pid):
    tot = 0
    for f in glob.glob(f"/proc/{pid}/task/*/schedstat"):
        try:
            tot += int(open(f).read().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return tot

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/tools"
sys.path.insert(0, TOOLS)
from ckdbs_cli import ServerConnection  # noqa: E402


def meta(conn):
    text = conn.send_command("SHOW META").replace("\\n", " ")
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def holder(port, stop, log):
    wins = [(10_000_000, 10_000_010), (20_000_000, 20_000_010)]
    conn = None
    # wait for the benchmark's relation to be loaded: its last setup row is id 256
    while not stop.is_set():
        try:
            c = ServerConnection("127.0.0.1", port)
            r = c.send_command("SELECT * FROM c3_lock WHERE id = 256")
            if not r.startswith("ERR") and "256" in r:
                conn = c
                break
            c.close()
        except Exception:
            pass
        time.sleep(0.05)
    if conn is None:
        return
    cur = None
    k = 0
    started = time.time()
    while not stop.is_set():
        lo, hi = wins[k % 2]
        nxt = ServerConnection("127.0.0.1", port)
        r1 = nxt.send_command("BEGIN")
        r2 = nxt.send_command(f"UPDATE c3_lock SET v = 0 WHERE id >= {lo} AND id <= {hi}")
        log.append((round(time.time() - started, 1), r1[:40], r2[:80]))
        if cur is not None:
            cur.send_command("ROLLBACK")
            cur.close()
        cur = nxt
        k += 1
        t_end = time.time() + 30
        while time.time() < t_end and not stop.is_set():
            time.sleep(0.2)
    if cur is not None:
        cur.send_command("ROLLBACK")
        cur.close()
    conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", type=int, required=True)
    ap.add_argument("--server-cpus", required=True)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--fence", action="store_true")
    ap.add_argument("--sessions", type=int, default=8)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--run", type=int, default=0)
    ap.add_argument("--out", required=True)  # prefix
    a = ap.parse_args()
    tag = f"locks-{a.durability}-k{a.cores}-{'fence' if a.fence else 'plain'}-r{a.run}"
    wd = os.path.join(a.workdir, tag)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {wd}/d.db\nport = {a.port}\ncores = {a.cores}\ndurability = {a.durability}\n"
                f"buffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n"
                f"reactor_cpus = {a.server_cpus}\n")
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen([a.bin, "--config", conf], stdout=err, stderr=subprocess.STDOUT)
    res = {"tag": tag, "cores": a.cores, "fence": a.fence, "run": a.run, "durability": a.durability,
           "bin_sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "loadavg_before": open("/proc/loadavg").read().strip()}
    stop = threading.Event()
    hlog = []
    try:
        end = time.time() + 30
        while True:
            try:
                ServerConnection("127.0.0.1", a.port).close()
                break
            except OSError:
                if time.time() > end:
                    raise SystemExit("no listen")
                time.sleep(0.1)
        res["data_device"] = subprocess.run(["df", "-T", wd], capture_output=True, text=True).stdout.splitlines()[1]
        ctl = ServerConnection("127.0.0.1", a.port)
        res["meta_before"] = meta(ctl)
        cpu0 = cpu_ns(proc.pid)
        ht = None
        if a.fence:
            ht = threading.Thread(target=holder, args=(a.port, stop, hlog))
            ht.start()
        jpath = a.out + f"-{tag}.bench.json"
        cmd = [sys.executable, os.path.join(TOOLS, "lock_contention_benchmark.py"),
               "--port", str(a.port), "--sessions", str(a.sessions), "--rows", "256",
               "--ops", str(a.ops), "--blocks", "4", "--json", jpath]
        t0 = time.time()
        d = subprocess.run(cmd, capture_output=True, text=True, cwd=TOOLS)
        res["bench_s"] = round(time.time() - t0, 1)
        res["bench_rc"] = d.returncode
        open(a.out + f"-{tag}.txt", "w").write(d.stdout + "\n--- stderr\n" + d.stderr)
        res["meta_after"] = meta(ctl)
        res["server_cpu_s"] = round((cpu_ns(proc.pid) - cpu0) / 1e9, 3)
        stop.set()
        if ht:
            ht.join()
        res["holder_log"] = hlog
        ctl.close()
    finally:
        stop.set()
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err.close()
        shutil.rmtree(wd, ignore_errors=True)
    res["loadavg_after"] = open("/proc/loadavg").read().strip()
    json.dump(res, open(a.out + f"-{tag}.json", "w"), indent=1)
    print(tag, "rc", res.get("bench_rc"), "s", res.get("bench_s"))


if __name__ == "__main__":
    main()
