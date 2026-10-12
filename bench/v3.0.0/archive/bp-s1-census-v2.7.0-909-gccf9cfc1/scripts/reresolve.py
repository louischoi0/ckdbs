#!/usr/bin/env python3
"""What one catalog-memo drop costs the statement that meets it (BP-H6).

Server `cores = 2` (reactors pinned). Session A (core 0) times a statement S;
session B (core 1) moves the instance's schema word between A's statements by
`CREATE TABLE junk_i` (a catalog write: word_moves_schema + word_moves_resolve).
Iterations alternate: BUMP (B creates a table, A then runs S cold, then S twice
warm) and CONTROL (B runs SHOW META, which moves nothing; A runs S three times).
A's cold - warm per iteration, and the server's own on-CPU time around the cold
statement, are the drop's cost. The CONTROL arm's first-minus-rest is the noise.
"""
import argparse, glob, json, os, shutil, socket, statistics, subprocess, sys, time

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


def cpu_ns(pid):
    tot = 0
    for f in glob.glob(f"/proc/{pid}/task/*/schedstat"):
        try:
            tot += int(open(f).read().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return tot


def q(xs):
    xs = sorted(xs)
    return {"n": len(xs), **{f"p{p}_us": round(nearest_rank(xs, p) * 1e6, 1) for p in (0, 25, 50, 95, 99)}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--iters", type=int, default=300)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    wd = os.path.join(a.workdir, "reresolve")
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    with open(os.path.join(wd, "s.conf"), "w") as f:
        f.write(f"data_file = {wd}/d.db\nport = {a.port}\ncores = 2\ndurability = relaxed\n"
                f"buffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\nreactor_cpus = 0,2\n")
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen([a.bin, "--config", os.path.join(wd, "s.conf")], stdout=err, stderr=subprocess.STDOUT)
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
        conns = {}
        for _ in range(500):
            c = ServerConnection("127.0.0.1", a.port, timeout=60)
            core = meta(c).get("core")
            if core in conns:
                c.close()
            else:
                conns[core] = c
            if len(conns) == 2:
                break
        A, B = conns[0], conns[1]
        for sql in ("CREATE TABLE t (id int64, v int64) BTREE", "CREATE TABLE u (id int64, v int64) BTREE",
                    "CREATE TABLE w (id int64, v int64) BTREE"):
            assert not A.send_command(sql).startswith("ERR")
        for name in ("t", "u", "w"):
            A.send_command(f"INSERT INTO {name} VALUES " + ",".join(f"({i},{i})" for i in range(1, 1001)))
        stmts = {
            "point SELECT (1 relation)": lambda n: f"SELECT * FROM t WHERE id = {1 + n % 1000}",
            "point UPDATE (1 relation)": lambda n: f"UPDATE t SET v = {n} WHERE id = {1 + n % 1000}",
            "pk join SELECT (2 relations)": lambda n: f"SELECT * FROM t JOIN u ON t.id = u.id WHERE t.id = {1 + n % 1000}",
        }
        res = {"loadavg": open("/proc/loadavg").read().strip(), "bin_sha256": None, "stmts": {}}
        junk = 0
        for label, fn in stmts.items():
            for _ in range(200):  # warm
                A.send_command(fn(1))
            cold, warm, ccold, cwarm, srvcold, srvwarm = [], [], [], [], [], []
            csrvcold, csrvwarm = [], []
            drops = 0
            for it in range(a.iters):
                for mode in ("bump", "control"):
                    if mode == "bump":
                        junk += 1
                        r = B.send_command(f"CREATE TABLE junk_{junk} (id int64, v int64) BTREE")
                        assert not r.startswith("ERR"), r
                    else:
                        B.send_command("SHOW META")
                    time.sleep(0.003)  # B's reply is in; A's core sees the word at its next head
                    m0 = meta(A).get("contention_catalog_memo_drops")
                    # the first statement after (meta() itself is a statement, so the drop may land in it:
                    # read META before, then count drops across the cold statement alone)
                    c0 = cpu_ns(proc.pid)
                    t = time.perf_counter(); A.send_command(fn(it * 3 + 1)); t1 = time.perf_counter() - t
                    c1 = cpu_ns(proc.pid)
                    t = time.perf_counter(); A.send_command(fn(it * 3 + 2)); t2 = time.perf_counter() - t
                    c2 = cpu_ns(proc.pid)
                    t = time.perf_counter(); A.send_command(fn(it * 3 + 3)); t3 = time.perf_counter() - t
                    (cold if mode == "bump" else ccold).append(t1)
                    (warm if mode == "bump" else cwarm).append((t2 + t3) / 2)
                    if mode == "bump":
                        srvcold.append((c1 - c0) / 1e9)
                        srvwarm.append((c2 - c1) / 1e9)
                    else:
                        csrvcold.append((c1 - c0) / 1e9)
                        csrvwarm.append((c2 - c1) / 1e9)
            drops_total = meta(A).get("contention_catalog_memo_drops")
            diffs = [c - w for c, w in zip(cold, warm)]
            cdiffs = [c - w for c, w in zip(ccold, cwarm)]
            sdiffs = [c - w for c, w in zip(srvcold, srvwarm)]
            csdiffs = [c - w for c, w in zip(csrvcold, csrvwarm)]
            res["stmts"][label] = {
                "bump_cold": q(cold), "bump_warm": q(warm), "control_first": q(ccold), "control_rest": q(cwarm),
                "bump_cold_minus_warm_median_us": round(statistics.median(diffs) * 1e6, 1),
                "control_first_minus_rest_median_us": round(statistics.median(cdiffs) * 1e6, 1),
                "server_cpu_cold_minus_warm_median_us": round(statistics.median(sdiffs) * 1e6, 1),
                "control_server_cpu_first_minus_rest_median_us": round(statistics.median(csdiffs) * 1e6, 1),
                "server_cpu_warm_median_us": round(statistics.median(srvwarm) * 1e6, 1),
                "memo_drops_total_cumulative": drops_total,
                "bump_cold_minus_warm_median_us_by_third": [
                    round(statistics.median(diffs[i * len(diffs) // 3:(i + 1) * len(diffs) // 3]) * 1e6, 1) for i in range(3)],
                "control_first_minus_rest_median_us_by_third": [
                    round(statistics.median(cdiffs[i * len(cdiffs) // 3:(i + 1) * len(cdiffs) // 3]) * 1e6, 1) for i in range(3)],
            }
            print(label, json.dumps(res["stmts"][label]["bump_cold_minus_warm_median_us"]),
                  res["stmts"][label]["control_first_minus_rest_median_us"],
                  res["stmts"][label]["server_cpu_cold_minus_warm_median_us"], flush=True)
        res["junk_tables_created"] = junk
        res["memo_drops_final"] = meta(A).get("contention_catalog_memo_drops")
        res["word_moves"] = {k: meta(A).get(k) for k in ("contention_word_moves_schema", "contention_word_moves_resolve")}
        json.dump(res, open(a.out, "w"), indent=1)
    finally:
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err.close()
        shutil.rmtree(wd, ignore_errors=True)


if __name__ == "__main__":
    main()
