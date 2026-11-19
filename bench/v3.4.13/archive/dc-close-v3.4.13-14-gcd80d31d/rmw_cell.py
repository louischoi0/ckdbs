#!/usr/bin/env python3
"""DC close, descriptive cell: SESSIONS client sessions, ONE shared row, each looping
BEGIN; SELECT v FROM t WHERE id = 1 [FOR UPDATE]; UPDATE t SET v = <read + 1> WHERE id = 1; COMMIT
for a fixed duration.  cores = 4, durability = group, READ COMMITTED (default).  A fresh server and
data file per invocation.  On any error the client sends ROLLBACK and retries the whole transaction.
Reports commits/s, refusals by text, per-transaction latency percentiles, the final v against the
number of committed transactions, and the server's wait views read after the run."""
import argparse, collections, json, os, re, shutil, subprocess, sys, threading, time

sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/dc-select-for-update/tools")
import kwp
from bb_overhead_benchmark import host_state


def classify(msg):
    if "was written by transaction" in msg: return "TXN_CONFLICT written-by"
    if "deadlock" in msg.lower(): return "deadlock"
    if "lock-wait fault net" in msg: return "fault-net"
    return "other: " + re.sub(r"\d+", "N", msg)[:100]


def rows_text(conn, sql):
    f, rows, _t, _a = conn.execute(sql)
    return {"columns": [x["name"] for x in f], "rows": [[kwp.render_value(f[i], x) for i, x in enumerate(r)] for r in rows]}


def session(port, mode, deadline, res, idx):
    c = kwp.Connection("127.0.0.1", port, timeout=60, client_name="rmw")
    sel = "SELECT v FROM t WHERE id = 1" + (" FOR UPDATE" if mode == "fu" else "")
    commits = 0; lock_reads = 0; refused = collections.Counter(); where = collections.Counter(); lat = []
    while time.time() < deadline:
        t0 = time.perf_counter(); stage = "begin"
        try:
            c.execute("BEGIN")
            stage = "select"; lock_reads += 1
            _f, rows, _t, _a = c.execute(sel)
            v = int.from_bytes(rows[0][0], 'little', signed=True)  # int64 cell, raw little-endian
            stage = "update"
            c.execute(f"UPDATE t SET v = {v + 1} WHERE id = 1")
            stage = "commit"
            c.execute("COMMIT")
            commits += 1; lat.append(time.perf_counter() - t0)
        except kwp.KwpError as e:
            refused[classify(e.message)] += 1; where[stage + ":" + classify(e.message)] += 1
            try: c.execute("ROLLBACK")
            except kwp.KwpError: pass
    c.close()
    res[idx] = {"commits": commits, "stmt_reads": lock_reads, "refused": refused, "where": where, "lat": lat}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True); p.add_argument("--wd", required=True)
    p.add_argument("--mode", choices=["fu", "plain"], required=True)
    p.add_argument("--seconds", type=float, default=6); p.add_argument("--sessions", type=int, default=8)
    p.add_argument("--port", type=int, required=True); p.add_argument("--json", required=True)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    data = os.path.join(a.wd, f"rmw-{a.mode}-{a.port}.db")
    shutil.rmtree(data + ".wal", ignore_errors=True)
    if os.path.exists(data): os.remove(data)
    conf = os.path.join(a.wd, f"rmw-{a.mode}-{a.port}.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {a.port}\ncores = 4\ndurability = group\nbuffer_pool_frames = 65536\n"
                f"log_file = rmw-{a.port}.log\nlog_dir = {a.wd}\nlog_level = warn\n")
    err = open(os.path.join(a.wd, f"rmw-{a.mode}-{a.port}.stderr"), "w")
    proc = subprocess.Popen(["taskset", "-c", "0-3", a.binary, "--config", conf], stdout=err, stderr=subprocess.STDOUT)
    t = time.time() + 30
    while True:
        try: ctl = kwp.Connection("127.0.0.1", a.port, timeout=60, client_name="ctl"); break
        except OSError:
            if time.time() > t or proc.poll() is not None: sys.exit("server did not start")
            time.sleep(0.1)
    ctl.execute("CREATE TABLE t (id int64, v int64) BTREE")
    ctl.execute("INSERT INTO t (id, v) VALUES (1, 0)")
    host0 = host_state()
    res = [None] * a.sessions
    deadline = time.time() + a.seconds
    th = [threading.Thread(target=session, args=(a.port, a.mode, deadline, res, i)) for i in range(a.sessions)]
    w0 = time.time()
    for x in th: x.start()
    for x in th: x.join()
    wall = time.time() - w0
    host1 = host_state()
    commits = sum(r["commits"] for r in res)
    final_v = int.from_bytes(ctl.execute("SELECT v FROM t WHERE id = 1")[1][0][0], "little", signed=True)
    refused = collections.Counter(); where = collections.Counter()
    for r in res: refused.update(r["refused"]); where.update(r["where"])
    reads = sum(r["stmt_reads"] for r in res)
    lat = sorted(x for r in res for x in r["lat"])
    at = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))] * 1e6 if lat else None
    waits = {}
    for view in ("sys.wait_stats", "sys.lock_waits"):
        try: waits[view] = rows_text(ctl, f"SELECT * FROM {view}")
        except kwp.KwpError as e: waits[view] = f"ERR {e.message}"
    out = {"mode": a.mode, "sessions": a.sessions, "seconds": a.seconds, "wall": wall, "commits": commits,
           "tps": commits / wall, "final_v": final_v, "lost_increments": commits - final_v,
           "reads_issued": reads, "refused": dict(refused), "refused_where": dict(where),
           "refused_total": sum(refused.values()),
           "p0": at(0), "p25": at(.25), "p50": at(.5), "p95": at(.95), "p99": at(.99), "txn_lat_n": len(lat),
           "host_before": host0, "host_after": host1, "waits": waits}
    json.dump(out, open(a.json, "w"), indent=1)
    print(json.dumps({k: v for k, v in out.items() if k != "waits"}))
    ctl.close(); proc.terminate()
    try: proc.wait(30)
    except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    main()
