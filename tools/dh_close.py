#!/usr/bin/env python3
"""The DH milestone's close measurement: A (opening commit) against B (closing commit), with
two attribution arms from one patched binary, and the cost of the instance-wide cadence.

    dh_close.py --binA A/kds_server --binB B/kds_server --binX X/kds_server
                --workdir DIR --port P --out RUNS.jsonl --plan PLAN.txt --arch ARCHDIR [--resume]
    dh_close.py ... --snapshot-only         (copy/build the snapshots the plan names, then exit)

Arms: A = binary A; B = binary B; B3 = binary X with KDS_DH_NO_PACK=1 (images verbatim, the
instance's cadence alone); A0 = binary X with KDS_DH_NO_IMAGES=1 (the image debt never paid).
X is the closing commit plus the measurement-only switches patch; with neither variable set it
is B and is never used as B. The same default config in every arm (no `checkpoint_interval_ms`).

Built on `tools/dh_s0.py` (imported): snapshots copied per run, a 3 s warm-up and a 15 s window,
`sys.meta` / `sys.wait_stats` / `sys.checkpoints` deltas, the server's stderr and its own log
kept. This driver adds, per run: a `pgrep`-style process scan BEFORE the run (a foreign compiler,
linker, test runner or another `kds_server` waits out a quiet gate of 30 s, the hold is recorded),
a poll every 3 s DURING the run, and an AFTER scan; a run with a foreign process seen at any of
the three is discarded to `<out>.contaminated` and re-run (at most 3 attempts). It adds per-core
checkpoint runs, `pool_fetch_image_bytes`, and the recovery cell (plan shape `recovery`): run
`--rsecs` seconds at the cell's cores and clients, read the WAL directory's size and
`sys.meta`, `kill -9`, time the mount (exec to first accepted connection) on the same files,
count the rows, read the mount's `recovery_*` fields. A recovery run's directory is deleted.

Plan lines are `key=value` words as in dh_s0.py (`id= arm= shape= rows= frames= cores= clients=
dur= [s2args="..."]`). shape `scenario2` runs `tools/scenario2_freight.py` unmodified.
"""
import argparse, json, os, shutil, signal, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import db_close as D  # noqa: E402
import db_s0_premise as P  # noqa: E402
import dh_s0 as S  # noqa: E402

for mod in (P, S):
    if "pool_fetch_image_bytes" not in mod.FIELDS:
        mod.FIELDS = mod.FIELDS + ["pool_fetch_image_bytes"]

ARM_BIN = {"A": "A", "B": "B", "B3": "X", "A0": "X"}
ARM_ENV = {"A": {}, "B": {}, "B3": {"KDS_DH_NO_PACK": "1"}, "A0": {"KDS_DH_NO_IMAGES": "1"}}
S.arm_env = lambda arm, r=None: dict(ARM_ENV[arm])  # dh_s0's runners ask this

DEADLINE = [None]  # epoch seconds: no run starts and no hold waits past it


class PastDeadline(Exception):
    pass


COMPILERS = {"cc1plus", "cc1", "as", "ld", "collect2", "cmake", "ctest", "make", "ninja", "gcc", "g++", "c++"}
SANDBOX_PID = 560863  # the web sandbox's idle server, left alone


def foreign():
    """Processes that are neither this run's nor the resident idle sandbox server."""
    out = subprocess.run(["ps", "-eo", "pid,ppid,comm,args"], capture_output=True, text=True).stdout.splitlines()[1:]
    bad = []
    for l in out:
        parts = l.split(None, 3)
        if len(parts) < 3:
            continue
        pid, ppid, comm = int(parts[0]), int(parts[1]), parts[2]
        args = parts[3] if len(parts) > 3 else ""
        if ppid == os.getpid():  # this driver's own children (its servers, its scenario driver)
            continue
        if comm in COMPILERS:
            bad.append(f"{pid} {comm}")
        elif comm.startswith("kds_") and pid != SANDBOX_PID and "/dh-close/" not in args:
            bad.append(f"{pid} {comm}")
    return bad


def wait_quiet(a, tag):
    """Block until no foreign process has been seen for 30 s; returns seconds held."""
    t0, quiet_since, first = time.time(), None, True
    while True:
        if DEADLINE[0] and time.time() > DEADLINE[0]:
            raise PastDeadline()
        f = foreign()
        if f:
            if first:
                S.log(a.workdir, f"HOLD {tag}: foreign {f[:3]}")
                first = False
            quiet_since = None
        else:
            if quiet_since is None:
                quiet_since = time.time()
            if time.time() - quiet_since >= (0 if first else 30):
                return time.time() - t0 if not first else 0.0
        time.sleep(3 if f else 1)


def monitor(stop, seen):
    while not stop.wait(3):
        f = foreign()
        if f:
            seen.append((time.time(), f))


def with_guard(a, r, fn):
    """Run fn() with scans before/during/after; discard and re-run a run a foreign process touched."""
    hold = 0.0
    attempt = 0
    while True:  # a contaminated run is never kept: re-run until clean or the deadline
        attempt += 1
        hold += wait_quiet(a, f"{r['id']}-{r['arm']}")
        scan_before = S.procscan()
        stop, seen = threading.Event(), []
        th = threading.Thread(target=monitor, args=(stop, seen))
        th.start()
        try:
            res = fn()
        finally:
            stop.set()
            th.join()
        after = foreign()
        res["foreign_during"] = [x[1] for x in seen]
        res["foreign_after"] = after
        res["hold_s"] = hold
        res["attempt"] = attempt
        res["scan_before_top5"] = scan_before
        if not seen and not after:
            return res
        S.log(a.workdir, f"DISCARD {r['id']}-{r['arm']} attempt {attempt}: foreign {(seen[0][1] if seen else after)[:3]}")
        with open(a.out + ".contaminated", "a") as f:
            f.write(json.dumps(res) + "\n")


def prepare(a, r):
    a.bin = getattr(a, "bin" + ARM_BIN[r["arm"]])


def per_core(res):
    c = {}
    for x in res.get("checkpoints", []):
        c[str(x["core"])] = c.get(str(x["core"]), 0) + 1
    res["ckpt_by_core"] = c
    return res


def recovery_run(a, r):
    cores, clients = int(r["cores"]), int(r["clients"])
    shape, rows, frames = r["shape"].split("-")[0], int(r["rows"]), int(r.get("frames", 65536))
    snap = S.ensure_snapshot(a, "update-lit", rows, frames)
    run = os.path.join(a.workdir, "run", r["id"])
    S.copy_snapshot(snap, run)
    data, conf = os.path.join(run, "run.db"), os.path.join(run, "run.conf")
    with open(conf, "w") as f:
        f.write(S.conf_text(data, a.port, cores, r.get("dur", "group"), frames, "", run, "run"))
    srv = S.Srv(a.bin, data, conf, S.arm_env(r["arm"], r), a.port, os.path.join(run, "run.stderr"))
    args = S.Args()
    args.clients = clients
    _, needle, make = P.workload("update-lit", rows, args)
    bad = [0]
    m0 = P.meta(srv.conn)
    t0 = time.time()
    conns, ths, lats = D.drive_lat(make, clients, a.port, a.rsecs, bad)
    for t in ths:
        t.join()
    t1 = time.time()
    for c in conns:
        c.close()
    m1 = P.meta(srv.conn)
    # the whole meta text, for wal_first_segment and the rest
    _, rws, tag, _ = srv.conn.execute("SHOW META")
    import sys_meta
    full = sys_meta.from_rows(rws, tag)
    rows_ck = P.checkpoints(srv.conn)
    walsz = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(data + ".wal") for f in fs)
    nseg = sum(len(fs) for _, _, fs in os.walk(data + ".wal"))
    n = sum(len(L) for L in lats)
    by = {}
    for x in rows_ck:
        by[str(x["core"])] = by.get(str(x["core"]), 0) + 1
    srv.conn.close()
    srv.proc.send_signal(signal.SIGKILL)
    srv.proc.wait()
    srv.err.close()
    # the mount on the same files
    srv2 = S.Srv(a.bin, data, conf, S.arm_env(r["arm"], r), a.port, os.path.join(run, "run2.stderr"))
    mount_s = srv2.mount_s
    cnt = None
    try:
        _, rr, _, _ = srv2.conn.execute("SELECT COUNT(*) FROM t")
        v = rr[0][0]
        cnt = int.from_bytes(v, "little", signed=True) if len(v) == 8 else int(v.decode())
    except Exception as e:  # noqa
        cnt = repr(e)
    _, rws2, tag2, _ = srv2.conn.execute("SHOW META")
    after = sys_meta.from_rows(rws2, tag2)
    rec = {k: v for k, v in after.items() if k.startswith("recovery_")}
    walsz2 = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(data + ".wal") for f in fs)
    res = dict(r)
    res.update({"kind": "recovery", "window_s": t1 - t0, "statements": n, "bad": bad[0],
                "stmt_per_s": n / (t1 - t0), "wal_dir_bytes_at_kill": walsz, "wal_segments_at_kill": nseg,
                "wal_first_segment": full.get("wal_first_segment"), "wal_segments_removed": full.get("wal_segments_removed"),
                "wal_append_lsn_start": m0["wal_append_lsn"], "wal_append_lsn_kill": m1["wal_append_lsn"],
                "wal_durable_lsn_kill": m1["wal_durable_lsn"], "log_bytes_written": m1["wal_append_lsn"] - m0["wal_append_lsn"],
                "ckpt_runs_counter": m1["contention_checkpoint_runs"] - m0["contention_checkpoint_runs"],
                "ckpt_rows_by_core": by, "ckpt_last_started": [x.get("seq") for x in rows_ck][-8:],
                "images": (m1["pool_fetch_images"] or 0) - (m0["pool_fetch_images"] or 0),
                "image_bytes": None if m1.get("pool_fetch_image_bytes") is None else m1["pool_fetch_image_bytes"] - (m0["pool_fetch_image_bytes"] or 0),
                "mount_s": mount_s, "count_after": cnt, "recovery_fields": rec, "wal_dir_bytes_after_mount": walsz2,
                "loadavg_after": open("/proc/loadavg").read().split()[0]})
    S.log(a.workdir, f"{r['id']} {r['arm']} recovery c{cores}: mount {mount_s:.2f}s wal {walsz/1e6:.0f}MB count={cnt} "
          f"records={rec.get('recovery_records')} redo={rec.get('recovery_redo_applied')} {n/(t1-t0):.0f}/s")
    srv2.stop()
    os.makedirs(a.keep_stderr, exist_ok=True)
    for src in ("run.stderr", "run2.stderr", "db-run.log"):
        if os.path.exists(os.path.join(run, src)):
            shutil.copy(os.path.join(run, src), os.path.join(a.keep_stderr, f"{r['id']}-{r['arm']}-{src}"))
    shutil.rmtree(run, ignore_errors=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    for x in ("A", "B", "X"):
        ap.add_argument("--bin" + x, required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--arch", required=True)
    ap.add_argument("--warm", type=float, default=3)
    ap.add_argument("--secs", type=float, default=15)
    ap.add_argument("--rsecs", type=float, default=50)
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--until", type=float, default=None, help="epoch seconds: stop starting runs after it")
    ap.add_argument("--snapshot-only", action="store_true")
    ap.add_argument("--seed-snaps", default=None, help="a snap directory to copy ready snapshots from")
    a = ap.parse_args()
    DEADLINE[0] = a.until
    a.keep_stderr = os.path.join(a.arch, "logs")
    os.makedirs(a.workdir, exist_ok=True)
    os.makedirs(a.arch, exist_ok=True)
    plan = S.parse_plan(a.plan)
    if a.seed_snaps:
        os.makedirs(os.path.join(a.workdir, "snap"), exist_ok=True)
        for d in os.listdir(a.seed_snaps):
            dst = os.path.join(a.workdir, "snap", d)
            if not os.path.exists(dst) and os.path.exists(os.path.join(a.seed_snaps, d, "DONE")):
                shutil.copytree(os.path.join(a.seed_snaps, d), dst)
    a.bin = a.binA  # snapshots are loaded by the opening commit's binary: the older log format
    seen = set()
    for r in plan:
        if r["shape"] != "scenario2":
            k = ("update-lit" if r["shape"].startswith("recovery") else r["shape"], int(r["rows"]), int(r.get("frames", 65536)))
            if k not in seen:
                seen.add(k)
                S.ensure_snapshot(a, *k)
    if a.snapshot_only:
        S.log(a.workdir, "snapshots done")
        return
    done = set()
    if a.resume and os.path.exists(a.out):
        done = {json.loads(l)["id"] + json.loads(l)["arm"] for l in open(a.out)}
    with open(a.out, "a") as out:
        for r in plan:
            if r["id"] + r["arm"] in done:
                continue
            prepare(a, r)
            if r["shape"] == "scenario2":
                fn = lambda: S.scenario_run(a, r)  # noqa: E731
            elif r["shape"] == "recovery":
                fn = lambda: recovery_run(a, r)  # noqa: E731
            else:
                fn = lambda: per_core(S.micro_run(a, r))  # noqa: E731
            try:
                res = with_guard(a, r, fn)
            except PastDeadline:
                S.log(a.workdir, f"DEADLINE reached before {r['id']}-{r['arm']}")
                break
            res["bin"] = a.bin
            out.write(json.dumps(res) + "\n")
            out.flush()
    S.log(a.workdir, "ALL DONE")


if __name__ == "__main__":
    main()
