#!/usr/bin/env python3
"""DH-S0's premise measurement: one binary, three configurations, runs from snapshots.

    dh_s0.py --bin COPY_OF_kds_server --workdir DIR --port P --out RUNS.jsonl
             --plan PLAN.txt [--warm 3] [--secs 15] [--resume]
    dh_s0.py ... --snapshot-only        (build the snapshots the plan names, then exit)

`instructions/v3.2.0/workorder-dh-page-image-volume.md` section 5 (DH-S0).
The binary carries a measurement-only switch (`KDS_DH_NO_IMAGES`, read once at
mount; the patch is archived beside the results). Arms:

  A0  environment KDS_DH_NO_IMAGES=1 (image debt never paid)
  B   variable unset (main's behaviour)
  L   B plus `checkpoint_interval_ms = 5000 * cores` (cores > 1 only); the plan gives it a
      longer `warm` so that its first cadence tick (one interval after mount) lands inside
      the steady window

A plan line is whitespace-separated `key=value` words:

  id=c1r1 arm=B shape=update-lit rows=100000 frames=65536 cores=4 clients=4
  dur=group [keep_wal=1]

  scenario lines:  id=s4a1 arm=B shape=scenario2 cores=8 clients=8 dur=group
                   s2args="--organizations 1500 ..."

Micro cells load a table ONCE per (shape, rows, frames) (`tools/db_s0_premise.py`'s
workload `setup`), stop the server cleanly, and start every run from a `cp -r`
copy of the data file and its WAL directory (nothing on disk names a core).
Each run: copy, mount, row-count check, --warm seconds, a --secs steady window,
stop. Per run one JSON line: statements/s, p0/p25/p50/p90/p99 microseconds
(client side, db_close.drive_lat), server CPU a statement, `sys.meta` deltas
(db_close's plus pool_fetch_images), `sys.checkpoints` rows, `sys.wait_stats`
deltas, the loadavg, a process scan. A scenario line runs
`tools/scenario2_freight.py` unmodified (it always loads its own data; there is
no snapshot for it) while a sampler reads `sys.meta` every 250 ms; the booking
window is the span from the driver's read-probe line to its exit.

Progress is appended to DIR/progress.log after every step (a watchdog polls the
file's age, never a process pattern). --resume skips ids already in --out.
"""
import argparse, json, os, shlex, shutil, signal, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import db_close as D  # noqa: E402
import db_s0_premise as P  # noqa: E402

FIELDS = P.FIELDS  # db_close appended pool_fetch_images
WAITS = ["commit.wal_flush", "sched.run_queue", "cpu.on_cpu", "io.page_read"]


class Args:
    pass


def log(wd, msg):
    with open(os.path.join(wd, "progress.log"), "a") as f:
        f.write(time.strftime("%H:%M:%S ") + msg + "\n")


def conf_text(data, port, cores, dur, frames, extra, wd, tag):
    return (f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = {dur}\n"
            f"buffer_pool_frames = {frames}\nlog_file = db-{tag}.log\nlog_dir = {wd}\n"
            f"log_level = warn\n{extra}")


class Srv:
    def __init__(self, binary, data, conf, env_extra, port, errpath):
        env = dict(os.environ)
        env.pop("KDS_DH_NO_IMAGES", None)
        env.update(env_extra)
        self.err = open(errpath, "a")
        t0 = time.time()
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT, env=env)
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300)
                break
            except OSError:
                if time.time() - t0 > 600 or self.proc.poll() is not None:
                    raise SystemExit("server did not listen")
                time.sleep(0.005)
        self.mount_s = time.time() - t0

    def stop(self):
        try:
            self.conn.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(120)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.err.close()


def snap_dir(wd, shape, rows, frames):
    return os.path.join(wd, "snap", f"{shape}-{rows}-{frames}")


def ensure_snapshot(a, shape, rows, frames):
    d = snap_dir(a.workdir, shape, rows, frames)
    if os.path.exists(os.path.join(d, "DONE")):
        return d
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    data = os.path.join(d, "snap.db")
    conf = os.path.join(d, "snap.conf")
    with open(conf, "w") as f:
        f.write(conf_text(data, a.port, 1, "group", frames, "", d, "snap"))
    t0 = time.time()
    srv = Srv(a.bin, data, conf, {"KDS_DH_NO_IMAGES": "1"}, a.port, os.path.join(d, "snap.stderr"))
    args = Args()
    args.clients = 1
    setup, _, _ = P.workload(shape, rows, args)
    setup(srv.conn)
    srv.stop()
    open(os.path.join(d, "DONE"), "w").write("ok\n")
    log(a.workdir, f"snapshot {shape}:{rows}:{frames} built in {time.time()-t0:.0f}s")
    return d


def copy_snapshot(snap, run):
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    shutil.copy2(os.path.join(snap, "snap.db"), os.path.join(run, "run.db"))
    shutil.copytree(os.path.join(snap, "snap.db.wal"), os.path.join(run, "run.db.wal"))


def arm_env(arm, r=None):
    """A0 switches the image off; a plan line's `envx=K=V;K=V` adds spike variables (DH-S1's
    P arms: KDS_DH_IMAGE_BYTES, KDS_DH_IMAGE_SPIN_US - a log so written cannot be recovered)."""
    e = {"KDS_DH_NO_IMAGES": "1"} if arm == "A0" else {}
    if r and r.get("envx"):
        e.update(kv.split("=", 1) for kv in r["envx"].split(";"))
    return e


def arm_extra(arm, cores, r=None, wd=None):
    x = ""
    if arm == "L" and cores > 1:
        x += f"checkpoint_interval_ms = {5000 * cores}\n"
    if r is not None and r.get("archive") == "1":  # census runs: the WAL archive stops the recycler
        d = os.path.join(wd, "arch", r["id"])
        os.makedirs(d, exist_ok=True)
        x += f"archive_dir = {d}\n"
    return x


def finish_run(a, r, srv, run):
    """Stop the server, copy its stderr and its own log (db-run.log, written into the run
    directory: where a `bad` statement's reason would be) to --keep-stderr, move the WAL aside
    when the plan line says keep_wal=1, and delete the run directory (a spike arm's log cannot
    be recovered, so nothing in it is ever mounted again)."""
    srv.stop()
    if getattr(a, "keep_stderr", None):
        os.makedirs(a.keep_stderr, exist_ok=True)
        base = os.path.join(a.keep_stderr, f"{r['id']}-{r['arm']}")
        for src, dst in (("run.stderr", ".stderr"), ("db-run.log", ".log")):
            if os.path.exists(os.path.join(run, src)):
                shutil.copy(os.path.join(run, src), base + dst)
    if r.get("keep_wal") == "1":
        keep = os.path.join(a.workdir, "wal-keep", r["id"])
        shutil.rmtree(keep, ignore_errors=True)
        os.makedirs(os.path.dirname(keep), exist_ok=True)
        shutil.move(os.path.join(run, "run.db.wal"), keep)
    shutil.rmtree(run, ignore_errors=True)


def ck_keys(conn):
    """The (core, seq) pairs of `sys.checkpoints`: `seq` counts per core, so a maximum of it
    over cores is not a count of runs; a pair is one row."""
    return {(x["core"], x["seq"]) for x in P.checkpoints(conn)}


def procscan():
    out = subprocess.run(["ps", "-eo", "pid,pcpu,comm", "--sort=-pcpu"], capture_output=True,
                         text=True).stdout.splitlines()[1:6]
    return [x.strip() for x in out]


def micro_run(a, r):
    cores, clients = int(r["cores"]), int(r["clients"])
    shape, rows, frames = r["shape"], int(r["rows"]), int(r.get("frames", 65536))
    snap = ensure_snapshot(a, shape, rows, frames)
    run = os.path.join(a.workdir, "run", r["id"])
    t_cp = time.time()
    copy_snapshot(snap, run)
    cp_s = time.time() - t_cp
    data = os.path.join(run, "run.db")
    conf = os.path.join(run, "run.conf")
    with open(conf, "w") as f:
        f.write(conf_text(data, a.port, cores, r.get("dur", "group"), frames,
                          arm_extra(r["arm"], cores, r, a.workdir), run, "run"))
    load1 = open("/proc/loadavg").read().split()[0]
    scan = procscan()
    srv = Srv(a.bin, data, conf, arm_env(r["arm"], r), a.port, os.path.join(run, "run.stderr"))
    try:
        m_mount = P.meta(srv.conn)
        cnt = None
        if r["shape"] != "insert":
            _, rws, _, _ = srv.conn.execute("SELECT COUNT(*) FROM t")
            cnt = rws[0][0]
            cnt = int.from_bytes(cnt, "little", signed=True) if len(cnt) == 8 else int(cnt.decode())
            if cnt != rows:
                raise SystemExit(f"row count {cnt} != {rows} after mount from copy")
        args = Args()
        args.clients = clients
        _, needle, make = P.workload(shape, rows, args)
        bad = [0]
        conns, ths, _ = D.drive_lat(make, clients, a.port, float(r.get('warm', a.warm)), bad)
        for t in ths:
            t.join()
        for c in conns:
            c.close()
        ck0 = ck_keys(srv.conn)
        m0, s0, w0s = P.meta(srv.conn), P.stmt_stats(srv.conn, needle), D.wait_stats(srv.conn)
        cpu0, t0 = D.cpu_ticks(srv.proc.pid), time.time()
        conns, ths, lats = D.drive_lat(make, clients, a.port, a.secs, bad)
        for t in ths:
            t.join()
        t1, cpu1 = time.time(), D.cpu_ticks(srv.proc.pid)
        for c in conns:
            c.close()
        m1, s1, w1s = P.meta(srv.conn), P.stmt_stats(srv.conn, needle), D.wait_stats(srv.conn)
        ck = [x for x in P.checkpoints(srv.conn) if (x["core"], x["seq"]) not in ck0]
        allv = [x for L in lats for x in L]
        n = len(allv)
        tick = os.sysconf("SC_CLK_TCK")
        d = {k: (m1[k] - m0[k]) if m1[k] is not None and m0[k] is not None else None
             for k in FIELDS}
        d.update({"stmt_" + k: s1[k] - s0[k] for k in s1})
        waits = {}
        for ev in w1s:
            c1, tt1 = w1s[ev]
            c0, tt0 = w0s.get(ev, (0, 0))
            if c1 - c0 or tt1 - tt0:
                waits[ev] = [c1 - c0, tt1 - tt0]
        res = dict(r)
        res.update({
            "kind": "micro", "window_s": t1 - t0, "statements": n, "bad": bad[0],
            "stmt_per_s": n / (t1 - t0), "mount_s": srv.mount_s, "copy_s": cp_s,
            "count_after_mount": cnt, "lsn_at_mount": m_mount["wal_append_lsn"],
            "lsn_at_window_start": m0["wal_append_lsn"], "images_total_since_mount": m1["pool_fetch_images"],
            "lat_us": {q: D.pct(allv, q) * 1e6 for q in (0, 25, 50, 90, 99)} if n else None,
            "lat_mean_us": sum(allv) / n * 1e6 if n else None,
            "cpu_s": (cpu1 - cpu0) / tick, "cpu_us_per_stmt": (cpu1 - cpu0) / tick * 1e6 / n if n else None,
            "loadavg_before": load1, "loadavg_after": open("/proc/loadavg").read().split()[0],
            "procscan_before": scan, "delta": d, "checkpoints": ck, "waits_us": waits})
        log(a.workdir, f"{r['id']} {r['arm']} {shape}:{rows} c{cores} {r.get('dur','group')}: "
            f"{n} stmts {res['stmt_per_s']:.0f}/s bad={bad[0]} ck={len(ck)} "
            f"img={d['pool_fetch_images']} load1={load1}")
        return res
    finally:
        finish_run(a, r, srv, run)


def sampler(port, stop, out):
    try:
        c = kwp.Connection("127.0.0.1", port, timeout=60)
    except OSError:
        return
    while not stop.is_set():
        try:
            m = P.meta(c)
            out.append((time.time(), {k: m[k] for k in FIELDS}, len(ck_keys(c))))
        except Exception:  # noqa
            pass
        stop.wait(0.25)


def scenario_run(a, r):
    cores = int(r["cores"])
    run = os.path.join(a.workdir, "run", r["id"])
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    data = os.path.join(run, "run.db")
    conf = os.path.join(run, "run.conf")
    with open(conf, "w") as f:
        f.write(conf_text(data, a.port, cores, r.get("dur", "group"), 65536,
                          arm_extra(r["arm"], cores, r, a.workdir), run, "run"))
    load1 = open("/proc/loadavg").read().split()[0]
    scan = procscan()
    srv = Srv(a.bin, data, conf, arm_env(r["arm"], r), a.port, os.path.join(run, "run.stderr"))
    try:
        stop = threading.Event()
        samples = []
        th = threading.Thread(target=sampler, args=(a.port, stop, samples))
        th.start()
        js = os.path.join(a.arch, f"{r['id']}-{r['arm']}.json")
        cmd = [sys.executable, "-u", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 "scenario2_freight.py"),
               "--port", str(a.port), "--sync", "--seed", "1", "--verify", "100",
               "--bookers", str(r["clients"]), "--json", js] + shlex.split(r["s2args"])
        cpu0 = D.cpu_ticks(srv.proc.pid)
        wall0 = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t_probe = [None]
        lines = []
        for line in p.stdout:
            lines.append(line)
            if t_probe[0] is None and "read probe" in line:
                t_probe[0] = time.time()
        p.wait()
        wall1 = time.time()
        cpu1 = D.cpu_ticks(srv.proc.pid)
        stop.set()
        th.join()
        open(os.path.join(a.arch, f"{r['id']}-{r['arm']}.out"), "w").write("".join(lines))
        tick = os.sysconf("SC_CLK_TCK")
        summ = json.load(open(js)) if os.path.exists(js) else None
        # booking window: first sample at/after the probe line to the last sample
        win = [s for s in samples if t_probe[0] and s[0] >= t_probe[0]]
        res = dict(r)
        res.update({"kind": "scenario", "rc": p.returncode, "wall_s": wall1 - wall0,
                    "cpu_s_total": (cpu1 - cpu0) / tick, "loadavg_before": load1,
                    "procscan_before": scan, "mount_s": srv.mount_s,
                    "loadavg_after": open("/proc/loadavg").read().split()[0]})
        if win and len(win) >= 2:
            w0, w1 = win[0], win[-1]
            res["window"] = {"t0": w0[0], "t1": w1[0], "span_s": w1[0] - w0[0],
                             "delta": {k: (w1[1][k] - w0[1][k]) if w1[1][k] is not None
                                       and w0[1][k] is not None else None for k in FIELDS},
                             "checkpoint_rows": w1[2] - w0[2]}  # (core, seq) rows added
        if summ:
            res["summary"] = summ
        log(a.workdir, f"{r['id']} {r['arm']} scenario2 c{cores} {r.get('dur','group')}: rc={p.returncode} "
            f"tps={summ['meta'].get('tps') if summ else None} wall={wall1-wall0:.0f}s load1={load1}")
        return res
    finally:
        finish_run(a, r, srv, run)


def parse_plan(path):
    out = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if line:
            out.append({k: v for k, v in (w.split("=", 1) for w in shlex.split(line))})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--arch", required=True, help="directory for scenario JSON/logs")
    ap.add_argument("--warm", type=float, default=3)
    ap.add_argument("--secs", type=float, default=15)
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--keep-stderr", default=None, help="copy each run's server stderr and server log here")
    ap.add_argument("--snapshot-only", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    os.makedirs(a.arch, exist_ok=True)
    plan = parse_plan(a.plan)
    if a.snapshot_only:
        seen = set()
        for r in plan:
            if r["shape"] != "scenario2":
                k = (r["shape"], int(r["rows"]), int(r.get("frames", 65536)))
                if k not in seen:
                    seen.add(k)
                    ensure_snapshot(a, *k)
        log(a.workdir, "snapshots done")
        return
    done = set()
    if a.resume and os.path.exists(a.out):
        done = {json.loads(l)["id"] + json.loads(l)["arm"] for l in open(a.out)}
    with open(a.out, "a") as out:
        for r in plan:
            if r["id"] + r["arm"] in done:
                continue
            res = scenario_run(a, r) if r["shape"] == "scenario2" else micro_run(a, r)
            out.write(json.dumps(res) + "\n")
            out.flush()
    log(a.workdir, "ALL DONE")


if __name__ == "__main__":
    main()
