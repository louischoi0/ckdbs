#!/usr/bin/env python3
"""DM-S0's premise cells: how often a leaf's coverage check fetches the right
sibling, what the fetch costs, and how often the sibling is not resident.

`instructions/v3.2.0/workorder-dm-leaf-upper-bound.md` section 5 (DM-S0).
Needs a server built from the census spike (branch `worktree-dm-s0-spike`,
never landed), whose `sys.meta` contention block carries the `dm_*` tallies:
sibling fetches by site (the clustered tree's write descent, read-lookup miss
and ordered seek; the secondary tree's write descent, seek and batch), the
fetches that reserved a frame (a device read), the nanoseconds every fetch
and every missing fetch took, and the range walk's leaves past its range.

M1 (cores = 1, a warm pool, one connection): per statement shape, the
tallies' delta over `--m1-stmts` statements, divided by the count.
M2 (cores 1 and `--m2-cores`, `--sessions-per-core` closed-loop sessions a
core, `--seconds`): the time inside the sibling fetch a statement against the
server's CPU a statement (/proc utime + stime over the window).
M3 (cores = 1, `--m3-rows` rows on `--m3-frames` frames, DB's regime): the
indexed insert and the point update with the pool under pressure, the
fetches that missed, and the latency percentiles of the writers and of
point readers on the same core.
M4: the range walk's leaves past its range a statement (in M1).

    dm_s0_premise.py --bin B --workdir D --out OUT.json [--heartbeat F]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402

DM = ["dm_bt_cover_write", "dm_bt_cover_lookup", "dm_bt_cover_seek",
      "dm_ix_cover_write", "dm_ix_cover_seek", "dm_ix_cover_batch",
      "dm_bt_cover_misses", "dm_ix_cover_misses", "dm_cover_us", "dm_cover_miss_us",
      "dm_range_past_leaf", "dm_memo_hits", "dm_memo_fills", "descent_hops", "pool_misses",
      "pool_hits"]
TICK = os.sysconf("SC_CLK_TCK")


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    # bench/README.md rule 4's pattern, not a subset of it.
    try:
        busy = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"],
                              capture_output=True, text=True, timeout=5).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        busy = ["?"]
    return {"loadavg": load, "builds": busy}


def tallies(conn):
    _, rows, tag, _ = conn.execute("SHOW META")
    meta = sys_meta.from_rows(rows, tag)
    out = {}
    for name in DM:
        v = meta.get(name, meta.get("contention_" + name))
        try:
            out[name] = float(v) if v not in (None, "") else None
        except (TypeError, ValueError):
            out[name] = None
    return out


def delta(a, b):
    return {k: (b[k] - a[k]) if a[k] is not None and b[k] is not None else None for k in a}


def cpu_ticks(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])  # utime, stime (fields 14 and 15)


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]


def lat_summary(xs):
    return {"n": len(xs), **{f"p{p}": (round(pct(xs, p) * 1e6, 1) if xs else None)
                             for p in (0, 25, 50, 90, 99, 99.9, 100)}}


class Server:
    # `wrap` prefixes the server's command line - DM-S1's real-device cells
    # run it under `systemd-run --user --scope -p MemoryHigh=...`, which execs
    # it in place, so the pid read for CPU is still the server's.
    def __init__(self, a, cores, tag, frames, fresh=True, env=None, wrap=None):
        self.dir = os.path.join(a.workdir, tag)
        if fresh:
            shutil.rmtree(self.dir, ignore_errors=True)
            os.makedirs(self.dir)
        self.port = a.port
        conf = os.path.join(self.dir, "dm.conf")
        cpus = ",".join(str(4 + i) for i in range(cores)) if cores <= 4 else None
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/dm.db\nport = {a.port}\ncores = {cores}\n"
                    f"durability = relaxed\nbuffer_pool_frames = {frames}\n"
                    f"log_file = dm.log\nlog_dir = {self.dir}\nlog_level = warn\n")
            if cpus:
                f.write(f"reactor_cpus = {cpus}\n")
        self.err = open(os.path.join(self.dir, "dm.stderr"), "w")
        self.proc = subprocess.Popen(list(wrap or []) + [a.bin, "--config", conf],
                                     stdout=self.err, env=env,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                kwp.Connection("127.0.0.1", self.port, timeout=300).close()
                return
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    self.stop()  # no orphan left holding the port and the directory
                    raise SystemExit(f"server did not listen on {self.port}")
                time.sleep(0.1)

    def conn(self):
        return kwp.Connection("127.0.0.1", self.port, timeout=300)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()  # reaped before the next lifetime binds the same port
        self.err.close()


def make(c, name, n, nix, rng):
    cols = ", ".join(f"v{j} int64" for j in range(1, 5))
    c.execute(f"CREATE TABLE {name} (id int64, {cols}, w int64) BTREE")
    for i in range(0, n, 500):
        vals = ",".join("(%d,%s,0)" % (2 * (k + 1), ",".join(str(rng.randrange(1 << 40))
                                                             for _ in range(4)))
                        for k in range(i, min(n, i + 500)))
        c.execute(f"INSERT INTO {name} VALUES {vals}")
    for j in range(1, nix + 1):
        c.execute(f"CREATE INDEX {name}_v{j} ON {name} (v{j})")


def ins(t, rng):
    return (f"INSERT INTO {t} (v1, v2, v3, v4, w) VALUES "
            f"({rng.randrange(1 << 40)}, {rng.randrange(1 << 40)}, "
            f"{rng.randrange(1 << 40)}, {rng.randrange(1 << 40)}, 0)")


def m1(a, beat):
    srv = Server(a, 1, "m1", 131072)
    try:
        c = srv.conn()
        rng = random.Random(20261122)
        n = a.m1_rows
        for name, nix in (("t0", 0), ("t1", 1), ("t4", 4), ("td", 0)):
            make(c, name, n, nix, rng)
        k = a.m1_stmts
        evens = [2 * x for x in rng.sample(range(1, n + 1), k)]
        odd = rng.sample(range(1, 2 * n, 2), k)
        dels = [2 * x for x in rng.sample(range(1, n + 1), k)]
        shapes = {
            "ins_ix0": [ins("t0", rng) for _ in range(k)],
            "ins_ix1": [ins("t1", rng) for _ in range(k)],
            "ins_ix4": [ins("t4", rng) for _ in range(k)],
            "named_mid": [f"INSERT INTO t0 VALUES ({x}, 0, 0, 0, 0, 0)" for x in odd],
            "update_pk": [f"UPDATE t1 SET w = w + 1 WHERE id = {x}" for x in evens],
            "delete_pk": [f"DELETE FROM td WHERE id = {x}" for x in dels],
            "purge_pk": [f"PURGE FROM td WHERE id = {x}" for x in dels],
            "point_hit": [f"SELECT * FROM t1 WHERE id = {x}" for x in evens],
            "point_miss": [f"SELECT * FROM t1 WHERE id = {x + 1}" for x in evens],
            # Both ends, or the compiler gives the step no range and the
            # reverse walk starts at the top (step_compiler.cpp's
            # `TupleKeyRangeOf`: "Both ends, never one").
            "seek_bt_desc": [f"SELECT * FROM t1 WHERE id BETWEEN 1 AND {x} "
                             f"ORDER BY id DESC LIMIT 5" for x in evens],
            # `BtreeFetchLeafAtOrBelow`'s index twin serves the descending
            # ordered walk only; the ascending one seeks with no check.
            "seek_ix_desc": [f"SELECT * FROM t1 WHERE v1 BETWEEN 1 AND {rng.randrange(1 << 40)} "
                             f"ORDER BY v1 DESC LIMIT 5" for _ in range(k)],
            "range20": [f"SELECT * FROM t1 WHERE id BETWEEN {x} AND {x + 40}" for x in evens],
        }
        out = {}
        for name, stmts in shapes.items():
            t0 = tallies(c)
            errs = {}
            for s in stmts:
                try:
                    c.execute(s)
                except kwp.KwpError as e:
                    key = f"{e.code}:{' '.join(str(e.message).split()[:5])}"
                    errs[key] = errs.get(key, 0) + 1
            d = delta(t0, tallies(c))
            out[name] = {"stmts": len(stmts), "errors": errs, "example": stmts[0],
                         "per_stmt": {kk: (round(v / len(stmts), 4) if v is not None else None)
                                      for kk, v in d.items()}}
            beat(f"m1 {name} done errors={sum(errs.values())}")
        c.close()
        return out
    finally:
        srv.stop()


def closed_loop(srv, sessions, seconds, gen_writer, readers=0, gen_reader=None):
    stop = threading.Event()
    lat = [[] for _ in range(sessions)]
    rlat = [[] for _ in range(max(readers, 1))]
    errors = {}
    lock = threading.Lock()

    def run(i, gen, sink):
        c = srv.conn()
        rng = random.Random(7000 + i)
        while not stop.is_set():
            s = gen(rng)
            t = time.perf_counter()
            try:
                c.execute(s)
                sink[i].append(time.perf_counter() - t)
            except kwp.KwpError as e:
                key = f"{e.code}:{' '.join(str(e.message).split()[:5])}"
                with lock:
                    errors[key] = errors.get(key, 0) + 1
        c.close()

    host_before = host_state()
    admin = srv.conn()
    t0 = tallies(admin)
    c0 = cpu_ticks(srv.proc.pid)
    th = [threading.Thread(target=run, args=(i, gen_writer, lat)) for i in range(sessions)]
    th += [threading.Thread(target=run, args=(i, gen_reader, rlat)) for i in range(readers)]
    start = time.time()
    for t in th:
        t.start()
    time.sleep(seconds)
    stop.set()
    # The issue window: throughput is writes over this. `wall` adds the join,
    # i.e. the slowest statement in flight at the stop (up to 0.6 s cold).
    window = time.time() - start
    for t in th:
        t.join()
    wall = time.time() - start
    c1 = cpu_ticks(srv.proc.pid)
    d = delta(t0, tallies(admin))
    admin.close()
    writes = sum(len(x) for x in lat)
    allw = [x for xs in lat for x in xs]
    allr = [x for xs in rlat[:readers] for x in xs]
    cpu_s = (c1 - c0) / TICK
    res = {"sessions": sessions, "readers": readers, "seconds": round(wall, 2),
           "window_s": round(window, 3),
           "writes": writes, "reads": len(allr), "errors": errors, "total": d,
           "server_cpu_s": cpu_s,
           "server_cpu_us_per_write": round(cpu_s * 1e6 / writes, 2) if writes else None,
           "writer_latency_us": lat_summary(allw), "host_before": host_before,
           "host": host_state()}
    if readers:
        res["reader_latency_us"] = lat_summary(allr)
    if writes and d.get("dm_cover_us") is not None:
        res["cover_us_per_write"] = round(d["dm_cover_us"] / writes, 4)
        if res["server_cpu_us_per_write"]:
            res["cover_share_of_cpu"] = round(res["cover_us_per_write"] /
                                              res["server_cpu_us_per_write"], 5)
        for kk in ("dm_bt_cover_write", "dm_ix_cover_write", "dm_bt_cover_misses",
                   "dm_ix_cover_misses", "dm_cover_miss_us", "pool_misses", "dm_memo_hits"):
            if d.get(kk) is not None:
                res[kk + "_per_write"] = round(d[kk] / writes, 5)
    return res


def m2(a, cores, beat):
    srv = Server(a, cores, f"m2c{cores}", 131072)
    try:
        c = srv.conn()
        rng = random.Random(31)
        make(c, "t1", a.m1_rows, 1, rng)
        c.close()
        n = a.m1_rows
        out = {}
        sessions = cores * a.sessions_per_core
        out["ins_ix1"] = closed_loop(srv, sessions, a.seconds, lambda r: ins("t1", r))
        beat(f"m2 cores={cores} ins_ix1 writes={out['ins_ix1']['writes']}")
        out["update_pk"] = closed_loop(
            srv, sessions, a.seconds,
            lambda r: f"UPDATE t1 SET w = w + 1 WHERE id = {2 * r.randrange(1, n + 1)}")
        beat(f"m2 cores={cores} update_pk writes={out['update_pk']['writes']}")
        return out
    finally:
        srv.stop()


def m3(a, beat):
    # Loaded on a pool that holds it (a 1M-row index build is one statement
    # and cannot fit 2,048 frames), then served from `--m3-frames` after a
    # clean restart on the same data file.
    n = a.m3_rows
    loader = Server(a, 1, "m3", 131072)
    try:
        c = loader.conn()
        make(c, "t1", n, 1, random.Random(41))
        c.close()
    finally:
        loader.stop()
    beat("m3 loaded")
    srv = Server(a, 1, "m3", a.m3_frames, fresh=False)
    try:
        out = {"pool_frames": a.m3_frames, "rows": n}
        reader = lambda r: f"SELECT * FROM t1 WHERE id = {2 * r.randrange(1, n + 1)}"
        out["ins_ix1"] = closed_loop(srv, a.sessions_per_core, a.seconds,
                                     lambda r: ins("t1", r), readers=1, gen_reader=reader)
        beat(f"m3 ins_ix1 writes={out['ins_ix1']['writes']}")
        out["update_pk"] = closed_loop(
            srv, a.sessions_per_core, a.seconds,
            lambda r: f"UPDATE t1 SET w = w + 1 WHERE id = {2 * r.randrange(1, n + 1)}",
            readers=1, gen_reader=reader)
        beat(f"m3 update_pk writes={out['update_pk']['writes']}")
        return out
    finally:
        srv.stop()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15751)
    p.add_argument("--m1-rows", type=int, default=50_000)
    p.add_argument("--m1-stmts", type=int, default=3_000)
    p.add_argument("--m2-cores", default="4")
    p.add_argument("--sessions-per-core", type=int, default=4)
    p.add_argument("--seconds", type=float, default=8.0)
    p.add_argument("--m3-rows", type=int, default=1_000_000)
    p.add_argument("--m3-frames", type=int, default=2048)
    p.add_argument("--only", default="m1,m2,m3")
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    sha = hashlib.sha256(open(a.bin, "rb").read()).hexdigest()
    report = {"bin": a.bin, "sha256": sha,
              "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
              "host_before": host_state(), "args": vars(a)}
    only = a.only.split(",")
    beat("start")
    def save():
        with open(a.out, "w") as f:
            json.dump(report, f, indent=1)

    if "m1" in only:
        report["m1"] = m1(a, beat)
        save()
    if "m2" in only:
        report["m2"] = {}
        for k in [1] + [int(x) for x in a.m2_cores.split(",") if x]:
            report["m2"][str(k)] = m2(a, k, beat)
            save()
    if "m3" in only:
        report["m3"] = m3(a, beat)
    report["host_after"] = host_state()
    report["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(report, f, indent=1)
    beat("end")


if __name__ == "__main__":
    main()
