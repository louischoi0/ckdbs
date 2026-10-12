#!/usr/bin/env python3
"""BP-H9's cells: a PURGE of 100,000 delete-marked rows, and named-key INSERT loops.

One fresh server per run (copy of the binary, fresh data file), `cores = --cores`,
reactors pinned to --server-cpus, this process pinned by the caller (taskset).
Reports, per phase: wall time, the server's on-CPU time (sum of every thread's
schedstat), and the delta of every numeric SHOW META field.
"""
import argparse, glob, hashlib, json, os, shutil, signal, socket, subprocess, sys, threading, time

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
                try:
                    out[k] = float(v)
                except ValueError:
                    pass
    return out


def cpu_ns(pid):
    tot = 0
    for f in glob.glob(f"/proc/{pid}/task/*/schedstat"):
        try:
            tot += int(open(f).read().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return tot


def delta(a, b):
    return {k: b[k] - a[k] for k in b if k in a and not k.endswith("_longest_us")}


def must(conn, sql):
    r = conn.send_command(sql)
    if r.startswith("ERR"):
        raise SystemExit(f"{sql[:60]!r} -> {r[:300]}")
    return r


def pct(xs):
    xs = sorted(xs)
    return {"n": len(xs), **{f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", type=int, required=True)
    ap.add_argument("--server-cpus", required=True)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--loop", type=int, default=20000)
    ap.add_argument("--run", type=int, default=0)
    ap.add_argument("--sample", action="store_true", help="bracket PURGE with SIGUSR1/SIGUSR2 (kds_sampler wrapper as --bin)")
    ap.add_argument("--waits", type=int, default=20)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    wd = os.path.join(a.workdir, f"inflight-k{a.cores}-r{a.run}")
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    with socket.socket() as s:
        if s.connect_ex(("127.0.0.1", a.port)) == 0:
            raise SystemExit("port in use")
    conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {wd}/d.db\nport = {a.port}\ncores = {a.cores}\ndurability = {a.durability}\n"
                f"buffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n"
                f"reactor_cpus = {a.server_cpus}\n")
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen([a.bin, "--config", conf], stdout=err, stderr=subprocess.STDOUT)
    res = {"cores": a.cores, "run": a.run, "durability": a.durability,
           "bin_sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "loadavg_before": open("/proc/loadavg").read().strip()}
    try:
        t_end = time.time() + 30
        while True:
            try:
                conn = ServerConnection("127.0.0.1", a.port, timeout=600)
                break
            except OSError:
                if time.time() > t_end:
                    raise SystemExit("no listen")
                time.sleep(0.1)
        res["session_core"] = meta(conn).get("core")
        res["data_device"] = subprocess.run(["df", "-T", wd], capture_output=True, text=True).stdout.splitlines()[1]
        # ---- PURGE ----
        must(conn, "CREATE TABLE p (id int64, v int64) BTREE")
        B = 500
        t0 = time.time()
        for lo in range(1, a.rows + 1, B):
            must(conn, "INSERT INTO p VALUES " + ",".join(f"({i},{i})" for i in range(lo, min(lo + B, a.rows + 1))))
        res["load_s"] = round(time.time() - t0, 2)
        m0, c0, t0 = meta(conn), cpu_ns(proc.pid), time.perf_counter()
        r = must(conn, "DELETE FROM p WHERE id >= 1")
        t1 = time.perf_counter()
        res["delete"] = {"reply": r[:80], "wall_ms": round((t1 - t0) * 1e3, 1),
                         "server_cpu_ms": round((cpu_ns(proc.pid) - c0) / 1e6, 1),
                         "inflight": {k: v for k, v in delta(m0, meta(conn)).items() if "inflight" in k}}
        time.sleep(3.0)  # settle: the deleter decided and every view past it
        m0, c0, t0 = meta(conn), cpu_ns(proc.pid), time.perf_counter()
        if a.sample:
            os.kill(proc.pid, signal.SIGUSR1)
        r = must(conn, f"PURGE FROM p WHERE id >= 1 AND id <= {a.rows}")
        t1 = time.perf_counter()
        if a.sample:
            os.kill(proc.pid, signal.SIGUSR2)
        m1 = meta(conn)
        d = delta(m0, m1)
        res["purge"] = {"reply": r[:80], "wall_ms": round((t1 - t0) * 1e3, 1),
                        "server_cpu_ms": round((cpu_ns(proc.pid) - c0) / 1e6, 1),
                        "inflight": {k: v for k, v in d.items() if "inflight" in k},
                        "contention": {k: v for k, v in d.items() if k.startswith("contention_") and v},
                        }
        # ---- named-key INSERT loops (fresh keys, then duplicates of them) ----
        must(conn, "CREATE TABLE n (id int64, v int64) BTREE")
        for phase, key in (("named_fresh", lambda k: k), ("named_duplicate", lambda k: k)):
            m0, c0 = meta(conn), cpu_ns(proc.pid)
            lat, errs, first = [], 0, None
            t0 = time.perf_counter()
            for k in range(1, a.loop + 1):
                ts = time.perf_counter()
                r = conn.send_command(f"INSERT INTO n VALUES ({key(k)}, {k})")
                lat.append(time.perf_counter() - ts)
                if r.startswith("ERR"):
                    errs += 1
                    first = first or r[:120]
            wall = time.perf_counter() - t0
            d = delta(m0, meta(conn))
            res[phase] = {"statements": a.loop, "errors": errs, "first_error": first,
                          "wall_s": round(wall, 3), "stmt_per_s": round(a.loop / wall, 1),
                          "server_cpu_us_per_stmt": round((cpu_ns(proc.pid) - c0) / 1e3 / a.loop, 2),
                          "lat": pct(lat),
                          "inflight": {k: v for k, v in d.items() if "inflight" in k},
                          "contention": {k: v for k, v in d.items() if k.startswith("contention_") and v and "inflight" not in k}}
        # ---- a named key meeting another session's undecided insert of the same key ----
        must(conn, "CREATE TABLE w (id int64, v int64) BTREE")
        other = ServerConnection("127.0.0.1", a.port, timeout=60)
        waits = []
        m0, c0 = meta(conn), cpu_ns(proc.pid)
        for k in range(1, a.waits + 1):
            must(other, "BEGIN")
            must(other, f"INSERT INTO w VALUES ({k}, 0)")
            box = {}

            def go():
                ts = time.perf_counter()
                box["r"] = conn.send_command(f"INSERT INTO w VALUES ({k}, 1)")
                box["t"] = time.perf_counter() - ts

            th = threading.Thread(target=go)
            th.start()
            time.sleep(0.2)
            other.send_command("ROLLBACK")
            th.join()
            waits.append((round(box["t"] * 1e3, 1), box["r"][:40]))
        d = delta(m0, meta(conn))
        res["named_vs_undecided"] = {"waits": a.waits, "hold_ms": 200, "results": waits[:3],
                                     "inflight": {k: v for k, v in d.items() if "inflight" in k},
                                     "server_cpu_ms_total": round((cpu_ns(proc.pid) - c0) / 1e6, 1)}
        other.close()
        res["loadavg_after"] = open("/proc/loadavg").read().strip()
        conn.close()
    finally:
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err.close()
        shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(res, open(a.out, "w"), indent=1)
    print(json.dumps({k: res[k] for k in ("cores", "run", "purge", "named_fresh", "named_duplicate", "named_vs_undecided") if k in res})[:1500])


if __name__ == "__main__":
    main()
