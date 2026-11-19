#!/usr/bin/env python3
"""DB's close measurement (DB-Q5): interleaved A/B of two server binaries.

    db_close.py --binA A --binB B --workdir DIR --port P --out OUT.jsonl
                [--cores 1,4] [--rounds 2] [--secs 20] [--warm 3] [--clients 4]
                [--cells select-pk:10000,update-lit:200,...] [--recovery-reps 3]
                [--no-recovery]

Cells and their workloads are `tools/db_s0_premise.py`'s (imported, not
copied). Per cell a fresh server and data file from the named binary; the
cells run A then B, round after round (ABAB...). Per cell it writes one JSON
line carrying: statements and window seconds (client side, tag-checked),
per-statement latencies p0/p25/p50/p90/p99 in microseconds (client side),
server CPU (utime+stime of the server's pid, /proc, over the window) per
statement, the `SHOW META` deltas db_s0_premise reads plus `pool_fetch_images`
(absent from a binary that predates DB), `sys.checkpoints` rows finished in
the window, `sys.wait_stats` deltas, and `/proc/loadavg`.

Recovery: one fixed workload (10,000-row table loaded, then --recovery-stmts
point UPDATEs spread over 4 clients), `kill -9`, restart on the same files,
and the wall time from exec to the first accepted connection is the mount.
Run once with the default checkpoint cadence and once with
`checkpoint_interval_ms = 600000` (the whole workload's log replays).
"""
import argparse, json, os, random, shutil, signal, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import db_s0_premise as P  # noqa: E402

P.FIELDS = P.FIELDS + ["pool_fetch_images"]


def pct(xs, q):
    xs = sorted(xs)
    if not xs:
        return None
    i = min(len(xs) - 1, int(q / 100.0 * len(xs)))
    return xs[i]


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        s = f.read().rsplit(")", 1)[1].split()
    return int(s[11]) + int(s[12])  # utime + stime


def wait_stats(conn):
    _, rows, _, _ = conn.execute("SELECT core, event, count, total_us FROM sys.wait_stats")

    def num(x):
        if x is None:
            return None
        return int.from_bytes(x, "little", signed=True) if len(x) == 8 else int(x.decode())
    tot, per = {}, {}
    for r in rows:
        ev = (r[1] or b"").decode()
        c, t = num(r[2]) or 0, num(r[3]) or 0
        if r[0] is None:
            tot[ev] = (c, t)
        else:
            a = per.get(ev, (0, 0))
            per[ev] = (a[0] + c, a[1] + t)
    return tot or per


def make_server(binary, a, tag, frames, extra=""):
    class A:  # the Server's view of the namespace
        pass
    o = A()
    o.workdir, o.port, o.cores, o.durability, o.bin = a.workdir, a.port, a.cores, a.durability, binary
    return o


class Srv(P.Server):
    def __init__(self, binary, workdir, port, cores, tag, frames, extra=""):
        os.makedirs(workdir, exist_ok=True)
        self.data = os.path.join(workdir, f"db-{tag}.db")
        conf = os.path.join(workdir, f"db-{tag}.conf")
        for p in (self.data,):
            if os.path.exists(p):
                os.remove(p)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = {cores}\n"
                    f"durability = group\nbuffer_pool_frames = {frames}\n"
                    f"log_file = db-{tag}.log\nlog_dir = {workdir}\nlog_level = warn\n{extra}")
        self.conf, self.workdir, self.port, self.bin, self.tag = conf, workdir, port, binary, tag
        self.start()

    def start(self):
        self.err = open(os.path.join(self.workdir, f"db-{self.tag}.stderr"), "a")
        t0 = time.time()
        self.proc = subprocess.Popen([self.bin, "--config", self.conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", self.port, timeout=300)
                break
            except OSError:
                if time.time() - t0 > 300 or self.proc.poll() is not None:
                    raise SystemExit("server did not listen")
                time.sleep(0.005)
        self.mount_s = time.time() - t0


class Args:
    pass


def drive_lat(make, clients, port, seconds, bad):
    lats = [[] for _ in range(clients)]
    conns = [kwp.Connection("127.0.0.1", port, timeout=300) for _ in range(clients)]
    end = time.time() + seconds
    pc = time.perf_counter

    def run(tid):
        one, tag = make(tid)
        c, L = conns[tid], lats[tid]
        while time.time() < end:
            t0 = pc()
            t, _, _, err = one(c)
            L.append(pc() - t0)
            if not t.startswith(tag):
                bad[0] += 1
    ths = [threading.Thread(target=run, args=(t,)) for t in range(clients)]
    for t in ths:
        t.start()
    return conns, ths, lats


def cell(binary, label, a, cores, spec, progress):
    parts = spec.split(":")
    shape, rows = parts[0], int(parts[1])
    frames = int(parts[2]) if len(parts) > 2 else 65536
    tag = f"{label}-{shape}-{rows}-c{cores}"
    srv = Srv(binary, a.workdir, a.port, cores, tag, frames)
    try:
        args = Args()
        args.clients = a.clients
        setup, needle, make = P.workload(shape, rows, args)
        t_load = time.time()
        setup(srv.conn)
        load_s = time.time() - t_load
        bad = [0]
        conns, ths, _ = drive_lat(make, a.clients, a.port, a.warm, bad)
        for t in ths:
            t.join()
        for c in conns:
            c.close()
        ck0 = max([r["seq"] for r in P.checkpoints(srv.conn)] or [0])
        m0, s0, w0s = P.meta(srv.conn), P.stmt_stats(srv.conn, needle), wait_stats(srv.conn)
        cpu0, w0 = cpu_ticks(srv.proc.pid), time.time()
        conns, ths, lats = drive_lat(make, a.clients, a.port, a.secs, bad)
        for t in ths:
            t.join()
        w1, cpu1 = time.time(), cpu_ticks(srv.proc.pid)
        for c in conns:
            c.close()
        m1, s1, w1s = P.meta(srv.conn), P.stmt_stats(srv.conn, needle), wait_stats(srv.conn)
        ck = [r for r in P.checkpoints(srv.conn) if r["seq"] > ck0]
        allv = [x for L in lats for x in L]
        n = len(allv)
        tick = os.sysconf("SC_CLK_TCK")
        d = {k: (m1[k] - m0[k]) if m1[k] is not None and m0[k] is not None else None
             for k in P.FIELDS}
        d.update({"stmt_" + k: s1[k] - s0[k] for k in s1})
        waits = {}
        for ev in w1s:
            c1, t1 = w1s[ev]
            c0, t0 = w0s.get(ev, (0, 0))
            if c1 - c0 or t1 - t0:
                waits[ev] = [c1 - c0, t1 - t0]
        res = {"label": label, "cell": f"{shape}-{rows}-c{cores}", "shape": shape, "rows": rows,
               "cores": cores, "window_s": w1 - w0, "statements": n, "bad": bad[0],
               "stmt_per_s": n / (w1 - w0), "load_s": load_s,
               "lat_us": {q: pct(allv, q) * 1e6 for q in (0, 25, 50, 90, 99)} if n else None,
               "lat_mean_us": sum(allv) / n * 1e6 if n else None,
               "cpu_s": (cpu1 - cpu0) / tick, "cpu_us_per_stmt": (cpu1 - cpu0) / tick * 1e6 / n if n else None,
               "loadavg": open("/proc/loadavg").read().split()[0],
               "delta": d, "checkpoints": ck, "waits_us": waits}
        progress(f"{tag}: {n} stmts {n/(w1-w0):.0f}/s cpu/stmt {res['cpu_us_per_stmt']:.1f}us "
                 f"bad={bad[0]} ck={len(ck)} load1={res['loadavg']}")
        return res
    finally:
        srv.stop()


def recovery(binary, label, a, rep, interval_ms, progress):
    tag = f"{label}-recovery-{a.recovery_rows}-{interval_ms}-r{rep}"
    extra = f"checkpoint_interval_ms = {interval_ms}\n" if interval_ms else ""
    srv = Srv(binary, a.workdir, a.port, 1, tag, 65536, extra)
    args = Args()
    args.clients = a.clients
    setup, needle, make = P.workload("update-lit", a.recovery_rows, args)
    setup(srv.conn)
    per = a.recovery_stmts // a.clients
    bad = [0]
    conns = [kwp.Connection("127.0.0.1", a.port, timeout=300) for _ in range(a.clients)]

    def run(tid):
        one, t = make(tid)
        for _ in range(per):
            r = one(conns[tid])
            if not r[0].startswith(t):
                bad[0] += 1
    ths = [threading.Thread(target=run, args=(t,)) for t in range(a.clients)]
    t0 = time.time()
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    work_s = time.time() - t0
    m = P.meta(srv.conn)
    for c in conns:
        c.close()
    try:
        srv.conn.close()
    except OSError:
        pass
    srv.proc.send_signal(signal.SIGKILL)
    srv.proc.wait()
    srv.err.close()
    walsz = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(srv.data + ".wal")
                for f in fs) if os.path.isdir(srv.data + ".wal") else None
    srv.start()  # the mount: exec to first accepted connection
    mount_s = srv.mount_s
    m2 = P.meta(srv.conn)
    try:
        _, rows, tg, _ = srv.conn.execute("SELECT COUNT(*) FROM t")
    except Exception as e:  # noqa
        rows = repr(e)
    res = {"label": label, "recovery": True, "interval_ms": interval_ms or "default", "rep": rep, "rows": a.recovery_rows,
           "statements": per * a.clients, "bad": bad[0], "work_s": work_s,
           "wal_append_lsn_at_kill": m["wal_append_lsn"], "wal_dir_bytes": walsz,
           "mount_s": mount_s, "count_after": repr(rows)[:80],
           "pool_fetch_images_before_kill": m.get("pool_fetch_images")}
    progress(f"{tag}: work {work_s:.1f}s lsn {m['wal_append_lsn']} mount {mount_s:.3f}s")
    srv.stop()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binA", required=True)
    ap.add_argument("--binB", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", default="1,4")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--warm", type=float, default=3)
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--cells", default="select-pk:10000,update-lit:200,update-lit:1000,"
                    "update-lit:10000,update-lit:100000,update-spill-assign:10000,insert:0")
    ap.add_argument("--pressure", default="update-lit:1000000:2048")
    ap.add_argument("--recovery-reps", type=int, default=3)
    ap.add_argument("--recovery-stmts", type=int, default=40000)
    ap.add_argument("--recovery-rows", type=int, default=10000)
    ap.add_argument("--recovery-intervals", default="0,600000")
    ap.add_argument("--no-recovery", action="store_true")
    ap.add_argument("--only-recovery", action="store_true")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    a.durability = "group"
    os.makedirs(a.workdir, exist_ok=True)
    plog = os.path.join(a.workdir, "progress.log")

    def progress(msg):
        with open(plog, "a") as f:
            f.write(time.strftime("%H:%M:%S ") + msg + "\n")
    bins = [("A", a.binA), ("B", a.binB)]
    with open(a.out, "a") as out:
        if not a.only_recovery:
            for rnd in range(1, a.rounds + 1):
                plan = [(c, s) for c in a.cores.split(",") for s in a.cells.split(",") if s]
                plan += [(1, a.pressure)] if a.pressure else []
                for cores, spec in plan:
                    for label, b in bins:
                        r = cell(b, label, a, int(cores), spec, progress)
                        r["round"] = rnd
                        out.write(json.dumps(r) + "\n")
                        out.flush()
        if not a.no_recovery:
            for rep in range(1, a.recovery_reps + 1):
                for iv in [int(x) for x in a.recovery_intervals.split(",")]:
                    for label, b in bins:
                        r = recovery(b, label, a, rep, iv, progress)
                        out.write(json.dumps(r) + "\n")
                        out.flush()
    progress("ALL DONE")


if __name__ == "__main__":
    main()
