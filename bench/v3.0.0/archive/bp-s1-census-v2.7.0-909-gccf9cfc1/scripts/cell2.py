#!/usr/bin/env python3
"""BP-H6's CABIN AUTO cell: tools/scenario4_cabinopt_days.py against two fresh
servers (off, on) of `cores = K`, with SHOW META read on the `on` server
before and after, and (K > 1) free-running point-SELECT readers on every core
the scenario's own session is not on.

Not a driver change: the scenario driver runs unmodified; this wrapper starts
its servers, reads the counters and runs the readers.
"""
import argparse, hashlib, json, os, shutil, socket, subprocess, sys, time, threading

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/tools"
sys.path.insert(0, TOOLS)
from bench_common import nearest_rank  # noqa: E402
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


def start(bin_, wd, name, port, cores, cpus, durability):
    os.makedirs(wd, exist_ok=True)
    conf = os.path.join(wd, f"{name}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {wd}/{name}.db\nport = {port}\ncores = {cores}\ndurability = {durability}\n"
                f"buffer_pool_frames = 65536\nlog_file = {name}.log\nlog_dir = {wd}\nlog_level = warn\n"
                f"reactor_cpus = {cpus}\ndecay_half_life = 5\ncabin_optimizer_snapshot_interval_ms = 500\n")
    err = open(os.path.join(wd, f"{name}.stderr"), "w")
    p = subprocess.Popen([bin_, "--config", conf], stdout=err, stderr=subprocess.STDOUT)
    end = time.time() + 30
    while True:
        try:
            ServerConnection("127.0.0.1", port).close()
            return p, err
        except OSError:
            if time.time() > end:
                raise SystemExit(f"{name} did not listen")
            time.sleep(0.1)


def reader(port, target, stop, out, idx):
    """Free-running point SELECTs on a private relation, session pinned to core `target`."""
    conn = None
    for _ in range(2000):
        c = ServerConnection("127.0.0.1", port)
        if meta(c).get("core") == target:
            conn = c
            break
        c.close()
    if conn is None:
        out[idx] = {"error": f"core {target} not reached"}
        return
    name = f"rd_{target}"
    conn.send_command(f"CREATE TABLE {name} (id int64, v int64) BTREE")
    conn.send_command(f"INSERT INTO {name} VALUES " + ",".join(f"({i},{i})" for i in range(1, 501)))
    lat, errs, k = [], 0, 0
    while not stop.is_set():
        k += 1
        t = time.perf_counter()
        r = conn.send_command(f"SELECT * FROM {name} WHERE id = {1 + k % 500}")
        lat.append(time.perf_counter() - t)
        if r.startswith("ERR"):
            errs += 1
    lat.sort()
    out[idx] = {"core": target, "statements": len(lat), "errors": errs,
                **{f"p{q}_us": round(nearest_rank(lat, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, required=True)
    ap.add_argument("--server-cpus", required=True)
    ap.add_argument("--port-off", type=int, required=True)
    ap.add_argument("--port-on", type=int, required=True)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--readers", action="store_true")
    ap.add_argument("--run", type=int, default=0)
    ap.add_argument("--driver-args", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    wd = os.path.join(a.workdir, f"cell2-k{a.cores}-r{a.run}")
    shutil.rmtree(wd, ignore_errors=True)
    res = {"cores": a.cores, "readers": a.readers, "run": a.run, "durability": a.durability,
           "bin_sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "loadavg_before": open("/proc/loadavg").read().strip(),
           "data_device": None}
    off, e1 = start(a.bin, wd, "off", a.port_off, a.cores, a.server_cpus, a.durability)
    on, e2 = start(a.bin, wd, "on", a.port_on, a.cores, a.server_cpus, a.durability)
    res["data_device"] = subprocess.run(["df", "-T", wd], capture_output=True, text=True).stdout.splitlines()[1]
    stop = threading.Event()
    rd_out, rd_threads = {}, []
    try:
        ctl = ServerConnection("127.0.0.1", a.port_on)
        res["meta_before"] = meta(ctl)
        if a.readers:
            for i, target in enumerate(range(1, a.cores)):
                t = threading.Thread(target=reader, args=(a.port_on, target, stop, rd_out, i))
                t.start()
                rd_threads.append(t)
            time.sleep(4)  # readers connected and loaded
            res["meta_readers_up"] = meta(ctl)
        driver = [sys.executable, os.path.join(TOOLS, "scenario4_cabinopt_days.py"),
                  "--port-off", str(a.port_off), "--port-on", str(a.port_on),
                  "--pid-off", str(off.pid), "--pid-on", str(on.pid),
                  "--suffix", f"b{a.run}", "--json", os.path.join(a.out + ".driver.json")] + a.driver_args.split()
        timeline = []
        pstop = threading.Event()

        def poll():
            pc = ServerConnection("127.0.0.1", a.port_on)
            while not pstop.is_set():
                m = meta(pc)
                timeline.append((round(time.time() - t0, 1), {k[len("contention_"):]: m[k] for k in m if k.startswith("contention_") and (
                    "word_moves" in k or "memo_drops" in k or "structural_reruns" in k)}))
                pstop.wait(1.0)
            pc.close()

        t0 = time.time()
        pt = threading.Thread(target=poll)
        pt.start()
        d = subprocess.run(driver, capture_output=True, text=True)
        pstop.set()
        pt.join()
        res["timeline"] = timeline
        res["driver_s"] = round(time.time() - t0, 1)
        res["driver_rc"] = d.returncode
        open(a.out + ".driver.txt", "w").write(d.stdout + "\n--- stderr\n" + d.stderr)
        stop.set()
        for t in rd_threads:
            t.join()
        res["meta_after"] = meta(ctl)
        res["readers_stats"] = rd_out
        # the optimizer's own ledger
        res["optimizer"] = ctl.send_command("SHOW CABIN_OPTIMIZER")[:1500]
        ctl.close()
    finally:
        stop.set()
        for p in (off, on):
            p.terminate()
            try:
                p.wait(30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        e1.close()
        e2.close()
        shutil.rmtree(wd, ignore_errors=True)
    res["loadavg_after"] = open("/proc/loadavg").read().strip()
    json.dump(res, open(a.out, "w"), indent=1)
    print("done", a.out, "rc", res.get("driver_rc"))


if __name__ == "__main__":
    main()
