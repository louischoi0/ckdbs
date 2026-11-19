#!/usr/bin/env python3
"""DE-S0's premise cells: what a pool miss costs the sessions that did not miss.

`instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` §5. One
relation, `de_big`, of `--rows` wide rows is loaded once and every cell
mounts the same data file read-only in effect (no cell writes a row). Hot
sessions read tuple keys in `[1, --hot-keys]` open-loop, so their leaves stay
resident; cold sessions read tuple keys uniformly over the relation
open-loop, so nearly every one of their reads misses a pool a quarter of the
relation; a scanner walks the relation whole, closed-loop.

Cells (`--cells`, run in the order given, once per `--runs` round):

    i      hot alone; pool a quarter; page cache warm
    ii     hot + cold; pool a quarter; page cache warm (the host's own state
           after the file has been read once)
    iii    hot + cold; pool a quarter; page cache kept from answering (DE-Q2)
    iv     hot + one scanner; pool a quarter; page cache kept from answering
    v      hot + cold; a pool that holds the relation - the ceiling
    vscan  hot + one scanner; a pool that holds the relation - iv's ceiling
    iii4   iii at `cores = 4`: the cold sessions declared `analytic`, which
           holds them on core 0 (BL-S4's partition), the hot ones on 1..3

**How the page cache is kept from answering** (DE-Q2 (a)): the data file's
cache is dropped before the mount (`posix_fadvise(DONTNEED)` on a clean,
synced file; `mincore` confirms), the server runs in a user scope
(`systemd-run --user --scope`), a full walk fills the pool so its anonymous
memory is at its ceiling, and the scope's `memory.max` is then written as
that figure plus `--mem-slack-mb`: the file pages the scope may keep are the
slack. `/proc/<pid>/io`'s `read_bytes` over the window, against the misses
counted by `io.page_read`, says how many reads reached the device.

The cold sessions' rate is `--cold-util` over the device probe's p50 - a
target share of one core's time inside the read - capped at `--cold-rate-cap`,
fixed for every cell of the run once the probe has answered.

Every latency is taken from the open-loop schedule's due instant (`late`) and
from the send (`service`), as `tools/bl_mixed.py` does; this driver borrows
its connect and percentile helpers and is its extension for DE's cells, kept
in a file of its own because the load is done once and shared by every cell.

    tools/de_premise.py --bin build-release/kds_server --port P \\
        --workdir ~/bench-runs/de/work --out bench/<version>/archive/<name>
"""

import argparse
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
    #        cold  scan  pool        cache   limit  cores
    "i":     (False, False, "quarter", "warm", False, 1),
    "ii":    (True,  False, "quarter", "warm", False, 1),
    "iii":   (True,  False, "quarter", "drop", True,  1),
    "iv":    (False, True,  "quarter", "drop", True,  1),
    "v":     (True,  False, "all",     "warm", False, 1),
    "vscan": (False, True,  "all",     "warm", False, 1),
    "iii4":  (True,  False, "quarter", "drop", True,  4),
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
    proc, err, be, _pid, _log = start_server(args, wd, "load", 1, args.frames_all, False)
    t0 = time.time()
    must(be, f"CREATE TABLE de_big (id int64, c0 int64, pad char({args.pad})) BTREE")
    pad = "x" * args.pad
    for at in range(1, args.rows + 1, args.load_batch):
        must(be, "INSERT INTO de_big VALUES " + ", ".join(
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
            sql = f"SELECT id, c0 FROM de_big WHERE id = {rng.randint(1, top)}"
            t0 = time.time()
            _r, kind, err = be.run(sql)
            t1 = time.time()
            if due >= measure_from:
                out["lat"].append(t1 - t0)
                out["late"].append(t1 - due)
                note(kind, err)
            due += interval
    else:
        # A scanner: unindexed and matching nothing, so its outermost walk
        # reads every leaf and reaches its slice boundary every 64 pages.
        while time.time() < stop_at:
            t0 = time.time()
            _r, kind, err = be.run("SELECT id FROM de_big WHERE c0 = -1")
            t1 = time.time()
            if t0 >= measure_from and t1 <= stop_at:
                out["lat"].append(t1 - t0)
                note(kind, err)
    # The core answering now: the placer may have moved the session since
    # its connect (`tools/bl_mixed.py`'s `core_end`).
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
    cold_on, scan_on, pool, cache, limit, cores = CELLS[cell]
    tag = f"de-{cell}-r{run}"
    frames = args.frames_quarter if pool == "quarter" else args.frames_all
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    cached0 = drop_cache(args.data) if cache == "drop" else warm_cache(args.data)
    procs, proc, err = [], None, None
    try:
        proc, err, ctl, pid, log = start_server(args, wd, tag, cores, frames, limit)
        # One whole walk before the window: it fills a quarter pool, so the
        # server's anonymous memory is at its ceiling when the limit is
        # written, and it makes the whole relation resident in the ceiling
        # cells.
        t0 = time.time()
        query(ctl, "SELECT id FROM de_big WHERE c0 = -1")
        prewarm_s = time.time() - t0
        mem_max = None
        if limit:
            mem_max = rss_anon_kb(pid) * 1024 + args.mem_slack_mb * (1 << 20)
            with open(os.path.join(cgroup_dir(pid), "memory.max"), "w") as f:
                f.write(str(mem_max))
        roles = ["hot"] * args.hot + (["cold"] * args.cold if cold_on else []) + \
            (["scan"] if scan_on else [])
        rates = {"hot": args.hot_rate / args.hot, "cold": cold_rate / args.cold}
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
            slot = i if role == "hot" else i - args.hot
            cl = [sys.executable, os.path.abspath(__file__), "--client", "--role", role,
                  "--port", str(args.port), "--index", str(i), "--slot", str(slot),
                  "--peers", str(args.hot if role == "hot" else args.cold),
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
        if limit:
            cg = cgroup_dir(pid)
            for name in ("memory.current", "memory.max", "memory.stat", "memory.events"):
                with open(os.path.join(cg, name)) as f:
                    mem[name] = f.read().strip()
        outs = []
        for p, role in zip(procs, roles):
            if role == "scan":
                # Blocked in a walk that outlives the window; what it finished
                # inside the window it has not printed, so its samples are the
                # server's (`sys.wait_stats`), not the client's.
                p.kill()
                p.wait()
                outs.append({"role": "scan", "lat": [], "late": [], "ok": 0, "errors": 0,
                             "first_error": None, "core": None})
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
        hot = [o for o in outs if o["role"] == "hot"]
        cold = [o for o in outs if o["role"] == "cold"]
        res = {
            "cell": cell, "run": run, "cores": cores, "frames": frames, "pool": pool,
            "cache": cache, "cached_fraction_before_mount": round(cached0, 4),
            "limited": limit, "memory_max": mem_max, "mem_slack_mb": args.mem_slack_mb,
            "cgroup": mem, "prewarm_s": round(prewarm_s, 2),
            "hot_sessions": args.hot, "hot_rate": args.hot_rate, "hot_keys": args.hot_keys,
            "cold_sessions": args.cold if cold_on else 0, "cold_rate": cold_rate if cold_on else 0,
            "scanners": 1 if scan_on else 0, "rows": args.rows,
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
            "read_share_of_one_core": round(read_us / (span * 1e6), 4),
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
        if os.path.exists(os.path.join(wd, f"{tag}.s.log")):
            os.makedirs(args.out, exist_ok=True)
            shutil.copy2(os.path.join(wd, f"{tag}.s.log"), os.path.join(args.out, f"{tag}.s.log"))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cells", default="i,ii,iii,iv,v,vscan,iii4")
    p.add_argument("--runs", type=int, default=2)
    p.add_argument("--rows", type=int, default=900_000)
    p.add_argument("--pad", type=int, default=1000, help="the pad column's width: about 7 rows a page at 1000")
    p.add_argument("--load-batch", type=int, default=100)
    p.add_argument("--frames-quarter", type=int, default=32768)
    p.add_argument("--frames-all", type=int, default=160_000)
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
    p.add_argument("--client-cpus", default="2,3,6,7")
    p.add_argument("--client-cpus4", default="4,5,6,7",
                   help="the clients' CPUs in a `cores = 4` cell (DE-S0 ran it on --client-cpus)")
    p.add_argument("--bin")
    p.add_argument("--port", type=int)
    p.add_argument("--workdir", default=os.path.expanduser("~/bench-runs/de/work"))
    p.add_argument("--out", default=None)
    p.add_argument("--keep", action="store_true", help="keep the loaded data file for another invocation")
    p.add_argument("--max-load", type=float, default=1.5)
    p.add_argument("--max-wait", type=float, default=120)
    p.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--role", default="hot", help=argparse.SUPPRESS)
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
    copy =os.path.join(args.workdir, "kds_server.de")
    shutil.copy2(args.bin, copy)
    args.bin = copy
    with open(copy, "rb") as f:
        args.bin_sha256 = hashlib.sha256(f.read()).hexdigest()
    args.data = os.path.join(args.workdir, "de.db")
    t_all = time.time()
    summary = {"argv": sys.argv[1:], "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if not os.path.exists(args.data):
        summary["load"] = load(args, args.workdir)
        print(f"load {summary['load']}", flush=True)
    drop_cache(args.data)
    summary["probe_o_direct_8k_qd1"] = probe_device(args.data, args.probe_reads, 1)
    p50 = summary["probe_o_direct_8k_qd1"]["p50_us"] / 1e6
    cold_rate = args.cold_rate or min(args.cold_rate_cap, args.cold_util / p50)
    summary["cold_rate"] = round(cold_rate, 1)
    print(f"probe {summary['probe_o_direct_8k_qd1']} cold_rate={cold_rate:.1f}/s", flush=True)
    for run in range(args.runs):
        for cell in cells:
            run_cell(args, cell, run, cold_rate, args.workdir)
    summary["total_s"] = round(time.time() - t_all, 1)
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(f"done in {summary['total_s']} s", flush=True)
    if not args.keep:
        # The data file and its log directory go together: a later load over
        # a log that outlived its data file does not mount.
        shutil.rmtree(args.workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
