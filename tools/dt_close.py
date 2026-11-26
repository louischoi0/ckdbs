#!/usr/bin/env python3
"""DT's close: DT-S0's cells (a copy of `tools/dt_s0_premise.py`, which stays as
it was) plus three: `ivu` (hot sessions beside a walker whose `--walk-sql`
statement WRITES rows), `aw`/`au`/`ar` (no hot sessions: the no-match write
walk, the writing walk, the read walk, each alone). Used with `--arm A|B`
and `--bin` per arm, one invocation a round, `--keep` and one shared
`--workdir` template. Everything below is DT-S0's own docstring.

DT-S0's premise cells: what a write that never gives its core back costs the
sessions that never miss.

`instructions/v3.2.0/workorder-dt-write-path-yield.md` §5 (DT-S0). Adapted from
`tools/de_premise.py` (DE-S0), which stays as it was. One relation, `dt_big`,
of `--rows` wide rows is loaded once into a template directory; **every cell
mounts its own fresh copy of it** (these cells write rows). Hot sessions read
tuple keys in `[1, --hot-keys]` open-loop, so their leaves stay resident.

Cells (`--cells`, run in the order given, once per `--runs` round):

    i      hot alone; pool a quarter; page cache warm. The floor.
    iiw    hot + cold writers (UPDATE by uniform random tuple key, autocommit,
           open-loop); pool a quarter; page cache warm. A write's CPU, no device
    iiiw   iiw with the page cache kept from answering. THE COLD-WRITE STALL
    iiir   hot + cold point SELECTs; pool a quarter; cache kept from answering
           (DE's cell iii): what the read side is with DE built
    ivw    hot + one walker, closed-loop: `UPDATE dt_big SET c0 = c0 WHERE
           c0 = -1` (a whole-relation write walk matching nothing); pool holds
           the relation; cache warm. THE LONG-WALK STALL
    ivr    hot + one scanner `SELECT id FROM dt_big WHERE c0 = -1`; same pool,
           cache warm: the yielding read walk, ivw's reference
    ivwc   ivw with a quarter pool and the cache kept from answering

How the page cache is kept from answering: as `tools/de_premise.py`'s docstring
(`posix_fadvise(DONTNEED)` before the mount, `mincore` to confirm, the server in
a `systemd-run --user --scope` whose `memory.max` is its anonymous memory after
a full walk plus `--mem-slack-mb`). `mincore` is read again after the window.

The cold rate is `--cold-util` over the device probe's p50, capped at
`--cold-rate-cap`, fixed for every cell of the run. Every latency is taken from
the open-loop schedule's due instant (`late`) and from the send (`service`).
A cold writer counts the replies it got by text, so a refused or zero-row
UPDATE is visible. A walker prints each walk's start and end as it completes.

    tools/dt_s0_premise.py --bin build-release/kds_server --port P \\
        --workdir ~/bench-runs/dt/work --out bench/<version>/archive/<name>
"""

import argparse
import collections
import ctypes
import hashlib
import json
import mmap
import os
import random
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bb_overhead_benchmark import host_state, meta_all, wait_for_load  # noqa: E402
from ba_census import Kds, data_device  # noqa: E402
from bl_mixed import connect_on, must, pct  # noqa: E402

PAGE = 8192
CELLS = {
    #       cold    kind  walker  pool        cache   limit  cores
    "i":    (False, "",   "",     "quarter", "warm", False, 1),
    "iiw":  (True,  "w",  "",     "quarter", "warm", False, 1),
    "iiiw": (True,  "w",  "",     "quarter", "drop", True,  1),
    "iiir": (True,  "r",  "",     "quarter", "drop", True,  1),
    "ivw":  (False, "",   "w",    "all",     "warm", False, 1),
    "ivr":  (False, "",   "r",    "all",     "warm", False, 1),
    "ivwc": (False, "",   "w",    "quarter", "drop", True,  1),
    # DT's close (tools/dt_close.py): "u" is the walker whose statement writes
    # rows (`--walk-sql`); a cell named `a...` has no hot sessions (the walk's
    # own cost, alone).
    "ivu":  (False, "",   "u",    "all",     "warm", False, 1),
    "aw":   (False, "",   "w",    "all",     "warm", False, 1),
    "au":   (False, "",   "u",    "all",     "warm", False, 1),
    "ar":   (False, "",   "r",    "all",     "warm", False, 1),
}
OPEN_LOOP = ("hot", "cold")


def rows_of(reply):
    """A rendered SELECT reply as a list of dicts (header, then `\\n` rows)."""
    if reply.startswith("ERR"):
        raise SystemExit(f"query failed: {reply!r}")
    lines = reply.split("\\n")
    names = lines[0].split(",")
    return [dict(zip(names, line.split(","))) for line in lines[1:] if line]


def query(be, sql):
    r, kind, err = be.run(sql)
    if kind:
        raise SystemExit(f"{sql[:80]!r} -> {err!r}")
    return rows_of(r)


# ---- the host's page cache --------------------------------------------------

def cached_fraction(path):
    """The fraction of `path`'s pages the page cache holds, by `mincore`."""
    size = os.path.getsize(path)
    if size == 0:
        return 0.0
    # Through libc, not the `mmap` module: `mincore` needs the mapping's
    # address, which a read-only Python mapping does not expose.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    n = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
    vec = (ctypes.c_ubyte * n)()
    fd = os.open(path, os.O_RDONLY)
    try:
        p = libc.mmap(None, size, mmap.PROT_READ, mmap.MAP_SHARED, fd, 0)
        if p is None or p == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_errno(), "mmap")
        try:
            if libc.mincore(p, size, vec) != 0:
                raise OSError(ctypes.get_errno(), "mincore")
        finally:
            libc.munmap(p, size)
    finally:
        os.close(fd)
    return sum(b & 1 for b in vec) / n


def drop_cache(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
    return cached_fraction(path)


def warm_cache(path):
    with open(path, "rb", buffering=0) as f:
        while f.read(8 << 20):
            pass
    return cached_fraction(path)


def probe_device(path, n, seed):
    """`n` 8 KiB reads at random page offsets, `O_DIRECT`, queue depth 1."""
    pages = os.path.getsize(path) // PAGE
    rng = random.Random(seed)
    buf = mmap.mmap(-1, PAGE)  # page-aligned, as O_DIRECT requires
    fd = os.open(path, os.O_RDONLY | os.O_DIRECT)
    lat = []
    try:
        for _ in range(n):
            off = rng.randrange(pages) * PAGE
            t0 = time.perf_counter()
            got = os.preadv(fd, [buf], off)
            lat.append(time.perf_counter() - t0)
            if got != PAGE:
                raise SystemExit(f"probe: short read {got} at {off}")
    finally:
        os.close(fd)
        buf.close()
    return pct(lat)


# ---- the server -------------------------------------------------------------

def server_pid(conf):
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open(f"/proc/{d}/cmdline", "rb") as f:
                argv = f.read().split(b"\0")
        except OSError:
            continue
        if conf.encode() in argv and b"--config" in argv and not argv[0].endswith(b"systemd-run"):
            return int(d)
    raise SystemExit("server pid not found")


def proc_io(pid):
    with open(f"/proc/{pid}/io") as f:
        return {k: int(v) for k, v in (line.strip().split(": ") for line in f)}


def rss_anon_kb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("RssAnon:"):
                return int(line.split()[1])
    raise SystemExit("RssAnon not found")


def cgroup_dir(pid):
    with open(f"/proc/{pid}/cgroup") as f:
        return "/sys/fs/cgroup" + f.read().strip().split("::", 1)[1]


def start_server(args, wd, tag, cores, frames, scoped):
    conf = os.path.join(wd, f"{tag}.conf")
    log = f"{tag}.s.log"
    with open(conf, "w") as f:
        f.write(f"data_file = {args.data}\nport = {args.port}\ncores = {cores}\n"
                f"durability = relaxed\nbuffer_pool_frames = {frames}\n"
                f"log_file = {log}\nlog_dir = {wd}\nlog_level = warn\n")
        if args.conf_extra:
            f.write(args.conf_extra.replace(";", "\n") + "\n")
        cpus = args.server_cpus4 if cores > 1 else args.server_cpu
        if cpus:
            f.write(f"reactor_cpus = {cpus}\n")
    with socket.socket() as s:
        if s.connect_ex(("127.0.0.1", args.port)) == 0:
            raise SystemExit(f"port {args.port} is already in use; choose another with --port")
    err = open(os.path.join(wd, f"{tag}.stderr"), "w")
    cmd = [args.bin, "--config", conf]
    if scoped:
        cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemorySwapMax=0", "--"] + cmd
    proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    deadline = time.time() + 60
    while True:
        try:
            be = Kds(args.port)
            return proc, err, be, server_pid(conf), os.path.join(wd, log)
        except OSError:
            if proc.poll() is not None or time.time() > deadline:
                err.close()
                raise SystemExit(f"server did not listen; see {wd}/{tag}.stderr")
            time.sleep(0.1)


def stop_server(proc, err, be=None):
    """`STOP` when a session is given - the clean shutdown that writes every
    page back - and SIGTERM otherwise."""
    if be is not None:
        try:
            be.conn.send_command("STOP")
        except (OSError, ConnectionError):
            pass
    else:
        proc.terminate()
    try:
        proc.wait(120)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    err.close()


def load(args, wd):
    args.data = os.path.join(wd, "dt.db")
    proc, err, be, _pid, _log = start_server(args, wd, "load", 1, args.frames_all, False)
    t0 = time.time()
    must(be, f"CREATE TABLE dt_big (id int64, c0 int64, pad char({args.pad})) BTREE")
    pad = "x" * args.pad
    for at in range(1, args.rows + 1, args.load_batch):
        must(be, "INSERT INTO dt_big VALUES " + ", ".join(
            f"({i}, {i}, '{pad}')" for i in range(at, min(at + args.load_batch, args.rows + 1))))
    took = time.time() - t0
    stop_server(proc, err, be)
    return {"load_s": round(took, 1), "data_bytes": os.path.getsize(args.data),
            "data_pages": os.path.getsize(args.data) // PAGE}


# ---- one client -------------------------------------------------------------

def client(args):
    be = connect_on(args.port, args.core)
    if args.level:
        must(be, f"SET PRIORITY {args.level}")
    out = {"role": args.role, "index": args.index, "core": meta_all(be.conn).get("core"),
           "lat": [], "late": [], "ok": 0, "errors": 0, "first_error": None}
    while time.time() < args.start_at:
        time.sleep(0.0005)
    measure_from = args.start_at + args.warmup
    stop_at = measure_from + args.seconds
    rng = random.Random(args.index * 7919 + 1)
    replies = collections.Counter()

    def note(kind, err):
        if kind:
            out["errors"] += 1
            out["first_error"] = out["first_error"] or err
        else:
            out["ok"] += 1

    if args.role in OPEN_LOOP:
        interval = 1.0 / args.rate
        top = args.hot_keys if args.role == "hot" else args.rows
        due = args.start_at + interval * args.slot / max(1, args.peers)
        while True:
            now = time.time()
            if now >= stop_at:
                break
            if now < due:
                time.sleep(due - now)
            key = rng.randint(1, top)
            if args.role == "cold" and args.kind == "w":
                sql = f"UPDATE dt_big SET c0 = c0 + 1 WHERE id = {key}"
            else:
                sql = f"SELECT id, c0 FROM dt_big WHERE id = {key}"
            t0 = time.time()
            _r, kind, err = be.run(sql)
            t1 = time.time()
            # By completion instant, not by due instant: a session that the
            # core starves falls behind its schedule, and gating on `due`
            # would drop every sample of exactly the cell that stalls.
            if t1 >= measure_from:
                out["lat"].append(t1 - t0)
                out["late"].append(t1 - due)
                note(kind, err)
                if args.role == "cold":
                    txt = (_r if _r is not None else str(err))[:80]
                    if txt in replies or len(replies) < 6:
                        replies[txt] += 1
            due += interval
    else:
        # A walker or scanner: unindexed and matching nothing, so the walk
        # reads every leaf. Each walk is printed as it completes.
        sql = {"w": "UPDATE dt_big SET c0 = c0 WHERE c0 = -1",
               "u": args.walk_sql,
               "r": "SELECT id FROM dt_big WHERE c0 = -1"}[args.kind]
        while time.time() < stop_at:
            t0 = time.time()
            _r, kind, err = be.run(sql)
            t1 = time.time()
            print(json.dumps({"walk": [t0, t1], "reply": (_r or str(err))[:80]}), flush=True)
            if t0 >= measure_from and t1 <= stop_at:
                out["lat"].append(t1 - t0)
            note(kind, err)
    # The core answering now: the placer may have moved the session since
    # its connect (`tools/bl_mixed.py`'s `core_end`).
    out["replies"] = dict(replies)
    out["core_end"] = meta_all(be.conn).get("core")
    print(json.dumps(out), flush=True)
    # Kept connected so the control session can read this session's waits.
    time.sleep(max(0.0, stop_at + args.linger - time.time()))


# ---- one cell ---------------------------------------------------------------

def snapshot(ctl, pid):
    waits = {r["event"]: {"count": int(r["count"]), "total_us": int(r["total_us"]),
                          "max_us": int(r["max_us"])}
             for r in query(ctl, "SELECT * FROM sys.wait_stats") if r["core"] == "NULL"}
    # One row per core: summed, so a bucket is the instance's.
    hist = {}
    for r in query(ctl, "SELECT * FROM sys.wait_histogram"):
        if r["event"].startswith("io."):
            key = (r["event"], int(r["bucket"]))
            hist[key] = hist.get(key, 0) + int(r["count"])
    return {"t": time.time(), "meta": meta_all(ctl.conn), "waits": waits, "hist": hist,
            "io": proc_io(pid), "rss_anon_kb": rss_anon_kb(pid)}


def run_cell(args, cell, run, cold_rate, wd):
    cold_on, ckind, wkind, pool, cache, limit, cores = CELLS[cell]
    scan_on = bool(wkind)
    tag = f"dt-{cell}{'-' + args.arm if args.arm else ''}-r{run}"
    frames = args.frames_quarter if pool == "quarter" else args.frames_all
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    # A fresh copy of the template for this cell: these cells write.
    cdir = os.path.join(wd, "cell")
    shutil.rmtree(cdir, ignore_errors=True)
    shutil.copytree(os.path.join(wd, "tpl"), cdir)
    args.data = os.path.join(cdir, "dt.db")
    wd_cell = cdir
    cached0 = drop_cache(args.data) if cache == "drop" else warm_cache(args.data)
    procs, proc, err = [], None, None
    try:
        proc, err, ctl, pid, log = start_server(args, wd_cell, tag, cores, frames, limit)
        # One whole walk before the window: it fills a quarter pool, so the
        # server's anonymous memory is at its ceiling when the limit is
        # written, and it makes the whole relation resident in the ceiling
        # cells.
        t0 = time.time()
        query(ctl, "SELECT id FROM dt_big WHERE c0 = -1")
        prewarm_s = time.time() - t0
        mem_max = None
        if limit:
            mem_max = rss_anon_kb(pid) * 1024 + args.mem_slack_mb * (1 << 20)
            with open(os.path.join(cgroup_dir(pid), "memory.max"), "w") as f:
                f.write(str(mem_max))
        nhot = 0 if cell.startswith("a") else args.hot
        roles = ["hot"] * nhot + (["cold"] * args.cold if cold_on else []) + \
            (["scan"] if scan_on else [])
        rates = {"hot": args.hot_rate / max(1, nhot), "cold": cold_rate / args.cold}
        # At `cores = 4` the reactors take one CPU of each physical core, so
        # the clients get the siblings and share no CPU with a reactor.
        client_cpus = args.client_cpus4 if cores > 1 else args.client_cpus
        cpus = client_cpus.split(",") if client_cpus else []
        start_at = time.time() + 3.0 + 0.2 * len(roles) * cores
        stop_at = start_at + args.warmup + args.seconds
        env = dict(os.environ, KDS_CLIENT_TIMEOUT="900")
        for i, role in enumerate(roles):
            if cores == 1:
                core, level = 0, ""
            elif role == "hot":
                core, level = 1 + i % (cores - 1), ""
            else:
                core, level = -1, "analytic"
            slot = i if role == "hot" else i - nhot
            cl = [sys.executable, os.path.abspath(__file__), "--client", "--role", role, "--kind", ckind if role == "cold" else wkind,
                  "--port", str(args.port), "--index", str(i), "--slot", str(slot),
                  "--peers", str(nhot if role == "hot" else args.cold),
                  "--walk-sql", args.walk_sql,
                  "--core", str(core), "--level", level,
                  "--rate", str(rates.get(role, 0.0)), "--rows", str(args.rows),
                  "--hot-keys", str(args.hot_keys), "--start-at", repr(start_at),
                  "--warmup", str(args.warmup), "--seconds", str(args.seconds),
                  "--linger", str(args.linger)]
            if cpus:
                cl = ["taskset", "-c", cpus[i % len(cpus)]] + cl
            procs.append(subprocess.Popen(cl, stdout=subprocess.PIPE, text=True, env=env))
        time.sleep(max(0.0, start_at + args.warmup - time.time()))
        s0 = snapshot(ctl, pid)
        time.sleep(max(0.0, stop_at - time.time()))
        s1 = snapshot(ctl, pid)
        # Every client is still connected (`--linger`): each session's own
        # blocking reads, the hot sessions' among them.
        sess = {}
        for r in query(ctl, "SELECT * FROM sys.session_waits"):
            if r["event"] in ("io.page_read", "cpu.on_cpu"):
                sess.setdefault(r["session_id"], {})[r["event"]] = \
                    {"count": int(r["count"]), "total_us": int(r["total_us"])}
        mem = {}
        cached1 = cached_fraction(args.data)
        if limit:
            cg = cgroup_dir(pid)
            for name in ("memory.current", "memory.max", "memory.stat", "memory.events"):
                with open(os.path.join(cg, name)) as f:
                    mem[name] = f.read().strip()
        outs = []
        for p, role in zip(procs, roles):
            if role == "scan":
                # Each walk is printed as it ends. One still running past the
                # linger is killed: what it did not finish is not counted.
                try:
                    txt, _ = p.communicate(timeout=args.linger + 60)
                except subprocess.TimeoutExpired:
                    p.kill()
                    txt, _ = p.communicate()
                walks = [json.loads(l) for l in txt.splitlines() if l.startswith('{"walk"')]
                fin = [json.loads(l) for l in txt.splitlines() if l.startswith('{"role"')]
                o = fin[0] if fin else {"role": "scan", "lat": [], "late": [], "ok": 0,
                                        "errors": 0, "first_error": None, "core": None}
                o["walks"] = walks
                outs.append(o)
                continue
            line = p.stdout.readline()
            outs.append(json.loads(line))
        after = host_state()
        span = s1["t"] - s0["t"]
        wd_ = {e: {"count": s1["waits"][e]["count"] - s0["waits"].get(e, {}).get("count", 0),
                   "total_us": s1["waits"][e]["total_us"] - s0["waits"].get(e, {}).get("total_us", 0),
                   "max_us_since_start": s1["waits"][e]["max_us"]}
               for e in s1["waits"]
               if s1["waits"][e]["count"] != s0["waits"].get(e, {}).get("count", 0)
               or s1["waits"][e]["total_us"] != s0["waits"].get(e, {}).get("total_us", 0)}
        hist = {}
        for (e, b), c in s1["hist"].items():
            d = c - s0["hist"].get((e, b), 0)
            if d:
                hist.setdefault(e, {})[str(b)] = d
        io = {k: s1["io"][k] - s0["io"][k] for k in s1["io"]}
        reads = sum(wd_.get(e, {}).get("count", 0)
                    for e in ("io.page_read", "io.var_heap_read", "io.scan_ring_read"))
        read_us = sum(wd_.get(e, {}).get("total_us", 0)
                      for e in ("io.page_read", "io.var_heap_read", "io.scan_ring_read"))
        walks = [w for o in outs if o["role"] == "scan" for w in o.get("walks", [])]
        in_win = [w for w in walks if w["walk"][0] >= s0["t"] - 0.5 and w["walk"][1] <= s1["t"] + 0.5]
        walk_s = sorted(w["walk"][1] - w["walk"][0] for w in walks)
        hot = [o for o in outs if o["role"] == "hot"]
        cold = [o for o in outs if o["role"] == "cold"]
        res = {
            "cell": cell, "arm": args.arm, "run": run, "cores": cores, "frames": frames,
            "pool": pool,
            "cache": cache, "cached_fraction_before_mount": round(cached0, 4),
            "limited": limit, "memory_max": mem_max, "mem_slack_mb": args.mem_slack_mb,
            "cgroup": mem, "prewarm_s": round(prewarm_s, 2),
            "hot_sessions": nhot, "conf_extra": args.conf_extra, "walk_sql": args.walk_sql, "hot_rate": args.hot_rate, "hot_keys": args.hot_keys,
            "cold_sessions": args.cold if cold_on else 0, "cold_rate": cold_rate if cold_on else 0,
            "scanners": 1 if scan_on else 0, "walker_kind": wkind, "cold_kind": ckind if cold_on else "",
            "walks_total": len(walks), "walks_in_window": len(in_win),
            "walk_seconds": [round(x, 3) for x in walk_s],
            "walk_replies": sorted({w["reply"] for w in walks}),
            "cold_replies": [o.get("replies") for o in outs if o["role"] == "cold"],
            "cached_fraction_after_cell": round(cached1, 4), "rows": args.rows,
            "seconds": args.seconds, "warmup": args.warmup, "window_s": round(span, 3),
            "wait_s": round(wait, 1), "host_before": before, "host_after": after,
            "hot_from_schedule": pct([x for o in hot for x in o["late"]]),
            "hot_service": pct([x for o in hot for x in o["lat"]]),
            "cold_from_schedule": pct([x for o in cold for x in o["late"]]),
            "cold_service": pct([x for o in cold for x in o["lat"]]),
            "hot_by_core": {str(c): pct([x for o in hot if o["core"] == c for x in o["late"]])
                            for c in sorted({o["core"] for o in hot})},
            "cores_seen": [o["core"] for o in outs],
            "cores_end": [o.get("core_end") for o in outs],
            "errors": sum(o["errors"] for o in outs),
            "first_error": next((o["first_error"] for o in outs if o["first_error"]), None),
            "waits_delta": wd_, "io_hist_delta": hist,
            "reads_in_window": reads, "read_us_in_window": read_us,
            # Before DE this is the core's stall; where a read leaves its
            # reactor (DE-S1) it is time parked, and the core's own time is
            # `on_cpu_share_of_one_core` beside it.
            "read_share_of_one_core": round(read_us / (span * 1e6), 4),
            "on_cpu_share_of_one_core": round(
                wd_.get("cpu.on_cpu", {}).get("total_us", 0) / (span * 1e6), 4),
            "pool_would_wait": s1["meta"].get("pool_would_wait", 0) - s0["meta"].get("pool_would_wait", 0),
            "miss_reruns": s1["meta"].get("contention_miss_reruns", 0)
            - s0["meta"].get("contention_miss_reruns", 0),
            "proc_io_delta": io,
            "device_bytes_per_read": round(io["read_bytes"] / reads, 1) if reads else None,
            "rss_anon_kb_end": s1["rss_anon_kb"],
            "session_waits_end": sess,
            "meta_delta": {k: s1["meta"][k] - s0["meta"].get(k, 0) for k in s1["meta"]},
            "bin_sha256": args.bin_sha256, "data_device": data_device(args.data),
        }
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(res, f, indent=1)
        h = res["hot_from_schedule"]
        print(f"{tag} load {before['loadavg'].split()[0]} cached={cached0:.2f} "
              f"hot p50={h.get('p50_us')} p95={h.get('p95_us')} p99={h.get('p99_us')} "
              f"max={h.get('max_us')} n={h.get('n')} cold p50={res['cold_service'].get('p50_us')} "
              f"reads={reads} read_share={res['read_share_of_one_core']} "
              f"dev_bytes/read={res['device_bytes_per_read']} errors={res['errors']}", flush=True)
        return res
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if proc is not None:
            stop_server(proc, err)
        if os.path.exists(os.path.join(cdir, f"{tag}.s.log")):
            os.makedirs(args.out, exist_ok=True)
            shutil.copy2(os.path.join(cdir, f"{tag}.s.log"), os.path.join(args.out, f"{tag}.s.log"))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cells", default="i,iiw,iiiw,iiir,ivw,ivr,ivwc")
    p.add_argument("--runs", type=int, default=2)
    p.add_argument("--run-base", type=int, default=0,
                   help="the first run's number, for an invocation that is one round of several")
    p.add_argument("--arm", default="",
                   help="a label for the binary, written into every file's name and JSON "
                        "(an A/B driven by one invocation an arm a round)")
    p.add_argument("--rows", type=int, default=900_000)
    p.add_argument("--pad", type=int, default=1000, help="the pad column's width: about 7 rows a page at 1000")
    p.add_argument("--load-batch", type=int, default=100)
    p.add_argument("--frames-quarter", type=int, default=32768)
    p.add_argument("--frames-all", type=int, default=160_000)
    p.add_argument("--walk-sql", default="UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 0",
                   help="the writing walker's statement (kind u)")
    p.add_argument("--conf-extra", default="",
                   help="extra server config lines, `;`-separated (a probe, e.g. "
                        "`checkpoint_interval_ms = 0`); empty for every cell that counts")
    p.add_argument("--hot", type=int, default=4)
    p.add_argument("--hot-rate", type=float, default=1000.0, help="point reads per second, all hot sessions")
    p.add_argument("--hot-keys", type=int, default=700)
    p.add_argument("--cold", type=int, default=4)
    p.add_argument("--cold-util", type=float, default=0.3,
                   help="the cold rate is this share of one core over the probe's p50 read time")
    p.add_argument("--cold-rate-cap", type=float, default=2000.0)
    p.add_argument("--cold-rate", type=float, default=0.0, help="fixes the cold rate (0: from the probe)")
    p.add_argument("--mem-slack-mb", type=int, default=64)
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--warmup", type=float, default=4.0)
    p.add_argument("--linger", type=float, default=4.0)
    p.add_argument("--probe-reads", type=int, default=2000)
    p.add_argument("--server-cpu", default="1")
    p.add_argument("--server-cpus4", default="0,1,2,3")
    p.add_argument("--client-cpus", default="2,3,4,5,6,7")
    p.add_argument("--client-cpus4", default="4,5,6,7",
                   help="the clients' CPUs in a `cores = 4` cell (DE-S0 ran it on --client-cpus)")
    p.add_argument("--bin")
    p.add_argument("--port", type=int)
    p.add_argument("--workdir", default=os.path.expanduser("~/bench-runs/dt/work"))
    p.add_argument("--out", default=None)
    p.add_argument("--keep", action="store_true", help="keep the loaded data file for another invocation")
    p.add_argument("--max-load", type=float, default=1.5)
    p.add_argument("--max-wait", type=float, default=120)
    p.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--role", default="hot", help=argparse.SUPPRESS)
    p.add_argument("--kind", default="", help=argparse.SUPPRESS)
    p.add_argument("--level", default="", help=argparse.SUPPRESS)
    p.add_argument("--index", type=int, default=0, help=argparse.SUPPRESS)
    p.add_argument("--slot", type=int, default=0, help=argparse.SUPPRESS)
    p.add_argument("--peers", type=int, default=1, help=argparse.SUPPRESS)
    p.add_argument("--core", type=int, default=0, help=argparse.SUPPRESS)
    p.add_argument("--rate", type=float, default=0.0, help=argparse.SUPPRESS)
    p.add_argument("--start-at", type=float, default=0.0, help=argparse.SUPPRESS)
    args = p.parse_args()
    if args.client:
        client(args)
        return 0
    if not (args.bin and args.port and args.out):
        p.error("needs --bin, --port and --out")
    cells = args.cells.split(",")
    for c in cells:
        if c not in CELLS:
            p.error(f"unknown cell {c!r}")
    # A whole walk of a relation the page cache does not hold outlasts the
    # client's default 5 s reply timeout.
    os.environ.setdefault("KDS_CLIENT_TIMEOUT", "900")
    os.makedirs(args.workdir, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    copy = os.path.join(args.workdir, f"kds_server.dt{args.arm}")
    shutil.copy2(args.bin, copy)
    args.bin = copy
    with open(copy, "rb") as f:
        args.bin_sha256 = hashlib.sha256(f.read()).hexdigest()
    tpl = os.path.join(args.workdir, "tpl")
    args.data = os.path.join(tpl, "dt.db")
    t_all = time.time()
    summary = {"argv": sys.argv[1:], "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if not os.path.exists(args.data):
        os.makedirs(tpl, exist_ok=True)
        summary["load"] = load(args, tpl)
        print(f"load {summary['load']}", flush=True)
    drop_cache(args.data)
    summary["probe_o_direct_8k_qd1"] = probe_device(args.data, args.probe_reads, 1)
    p50 = summary["probe_o_direct_8k_qd1"]["p50_us"] / 1e6
    cold_rate = args.cold_rate or min(args.cold_rate_cap, args.cold_util / p50)
    summary["cold_rate"] = round(cold_rate, 1)
    print(f"probe {summary['probe_o_direct_8k_qd1']} cold_rate={cold_rate:.1f}/s", flush=True)
    for run in range(args.run_base, args.run_base + args.runs):
        for cell in cells:
            with open(os.path.join(args.out, "heartbeat"), "w") as hb:
                hb.write(f"{time.strftime('%H:%M:%S')} run {run} cell {cell} starting\n")
            run_cell(args, cell, run, cold_rate, args.workdir)
            with open(os.path.join(args.out, "heartbeat"), "w") as hb:
                hb.write(f"{time.strftime('%H:%M:%S')} run {run} cell {cell} done\n")
    summary["total_s"] = round(time.time() - t_all, 1)
    with open(os.path.join(args.out, f"summary{'-' + args.arm if args.arm else ''}"
                                     f"-r{args.run_base}.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(f"done in {summary['total_s']} s", flush=True)
    if not args.keep:
        # The data file and its log directory go together: a later load over
        # a log that outlived its data file does not mount.
        shutil.rmtree(args.workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
