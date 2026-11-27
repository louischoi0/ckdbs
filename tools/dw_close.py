#!/usr/bin/env python3
"""DW's milestone-close measurement: what the unasked index purge costs and buys.

`instructions/v3.2.0/workorder-dw-btree-purge-driver.md` section 5. Premise run:
`tools/dw_s0_premise.py`, `bench/v3.5.0/results-dw-s0-premise-v3.4.17-178-g630ecc359.md`.

Three arms, each a COPY of a binary (the driver hashes it), each cell on a fresh
server and a fresh data file, `durability = relaxed`:

    A      the commit the milestone opened from (`630ecc359`)
    A2     a second copy of A: the control, what two runs of one engine differ by
    B-off  the closing commit, `index_purge_driver` absent
    B-on   the closing commit, `index_purge_driver = on`

`part1` - the change with the driver off costs nothing. `cores = 1`. Per round,
the arms A, A2, B-off once each in a rotated order (the six orders of three, so
six rounds put every arm in every position). On each arm, per size (200, 1,000
and 10,000 rows at the start of the phases), a BTREE relation `bt<size>` and a
HEAP relation `hp<size>`, each `(id int64 PRIMARY KEY, k int64, w int64)` with
`CREATE INDEX ix ON t (k)` (one unique `k` a row), and on each relation, in this
order, `--stmts` prepared statements a shape:

    sel_pk   SELECT * FROM t WHERE id = ?            ids cycled over the rows
    sel_ix   SELECT * FROM t WHERE k = ?             one row each
    ins      INSERT INTO t VALUES (?, ?, 0)          new ids above the rows
    upd_nk   UPDATE t SET w = ? WHERE id = ?         a non-indexed column
    upd_ix   UPDATE t SET k = ? WHERE id = ?         an indexed column, a new k
    del      DELETE FROM t WHERE id = ?              the ids `ins` placed (so the
                                                     relation is its start size again)

Server CPU a statement is the median over the phase's batches (`--batch`
statements) of the batch's schedstat delta; client latency beside it. After
`del`, on a heap, the server's CPU over `--tail-s` seconds of idleness is read
per deleted row: the slot release's purge phase runs on core 0's tick, after the
statements.

`part2` - the driver's cost and effect on a foreground mix. One relation
`x (id int64 PRIMARY KEY, k int64, w int64) BTREE` with `CREATE INDEX ix ON x (k)`,
`--n` live rows, one fresh server per cell and arm, a fixed `--mix-seconds`:

    D-rec     per iteration INSERT a new id (k uniform in n/10 values) and DELETE
              the oldest live id; `cores = 1`, one writer
    M-rec     UPDATE x SET k = ? WHERE id = ?  (k uniform in n values); `cores = 1`
    M-flip    UPDATE x SET k = ? WHERE id = ?  on a random live row, alternating that row's k
              between id and id + 1,000,000 (rows load with k = id), so after a row's first
              two updates no new index entry is appended; `cores = 1`
    U-nokey   UPDATE x SET w = ? WHERE id = ?  (no index entry is left); `cores = 1`
    D-rec-2c  D-rec with `cores = 2` and two writer connections, one placed on each
              core (a connection is re-opened until `SHOW META`'s `core` says so)

Writers are subprocesses on E-cores; the orchestrator pauses them (a file) every
`--sample-s` seconds, then reads `sys.indexes`, `SHOW META` and times a prepared
`SELECT * FROM x WHERE k = ?` over live keys (3 x `--probe-stmts` serial reads,
server CPU from schedstat, client wall), then lets them go. Throughput counts a
writer's active time only. At the end: the same probe, `PURGE INDEX ix` (reply,
wall), the entries after and the probe again (the clean reference).

    tools/dw_close.py part1 --run-dir DIR --bin-a A --bin-a2 A2 --bin-b B [--with-on]
    tools/dw_close.py part2 --run-dir DIR --bin-b B

`writer` is the per-connection process part2 starts; it is not run by hand.
"""

import argparse
import hashlib
import json
import os
import random
import socket
import struct
import subprocess
import sys
import time
from array import array
from bisect import bisect_right
from itertools import permutations
from statistics import median

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bn_census import sched_stat  # noqa: E402
from dq_s0_premise import settled_cpu_ns, quiet_gate  # noqa: E402

I64 = struct.Struct("<q")
i64 = kwp.Connection.int64
SERVER_CPUS = {1: "0-1", 2: "0-3"}      # P-cores
DRIVER_CPU = "8"
GOV = None


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    cc = subprocess.run(["pgrep", "-x", "cc1plus"], capture_output=True, text=True).stdout.split()
    mk = subprocess.run(["pgrep", "-f", "^(cmake --build|ctest)"], capture_output=True, text=True).stdout.split()
    return {"loadavg": load, "competing": len(cc) + len(mk)}


def beat(run_dir, **kw):
    kw["t"] = round(time.time(), 1)
    tmp = os.path.join(run_dir, "hb.json.tmp")
    with open(tmp, "w") as f:
        json.dump(kw, f)
    os.replace(tmp, os.path.join(run_dir, "hb.json"))


def pcts(lat, scale=1e6):
    s = sorted(lat)
    n = len(s)
    at = lambda q: round(s[min(n - 1, int(q * (n - 1) + 0.5))] * scale, 1)
    return {"ops": n, "p0": at(0), "p25": at(.25), "p50": at(.5), "p95": at(.95), "p99": at(.99),
            "p99.9": at(.999), "max": round(s[-1] * scale, 1), "mean": round(sum(s) / n * scale, 1)}


class Server:
    def __init__(self, binpath, workdir, port, cores, driver_on, frames):
        self.dir, self.port, self.bin = workdir, port, binpath
        os.makedirs(workdir, exist_ok=True)
        self.conf = os.path.join(workdir, "s.conf")
        extra = "index_purge_driver = on\n" if driver_on else ""
        with open(self.conf, "w") as f:
            f.write(f"data_file = {workdir}/d.db\nport = {port}\ncores = {cores}\n"
                    f"durability = relaxed\nbuffer_pool_frames = {frames}\n"
                    f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n{extra}")
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                raise SystemExit(f"port {port} is in use")
        self.err = open(os.path.join(workdir, "s.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", SERVER_CPUS[cores], binpath, "--config", self.conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid

    def connect(self, want_core=None, tries=40):
        deadline = time.time() + 60
        while True:
            try:
                c = kwp.Connection("127.0.0.1", self.port, timeout=300)
                break
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise SystemExit(f"server did not listen; see {self.dir}/s.stderr")
                time.sleep(0.1)
        if want_core is not None:
            for _ in range(tries):
                if meta(c).get("core") == str(want_core):
                    return c
                c.close()
                c = kwp.Connection("127.0.0.1", self.port, timeout=300)
            raise SystemExit(f"no connection landed on core {want_core}")
        return c

    def stop(self, c):
        try:
            c.query("STOP")
        except Exception:
            pass
        try:
            self.proc.wait(120)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def meta(c):
    _f, rows, _t, _n = c.execute("SHOW META")
    return {r[1].decode(): r[2].decode() for r in rows}


def must(c, sql, params=None, stmt=None):
    try:
        return c.execute(sql, params, statement=stmt)
    except kwp.KwpError as e:
        raise SystemExit(f"refused: {sql or stmt!r}: {e.code} {e}")


def cpu_now(pid):
    return sched_stat(pid)[0]


# ============================================================================= part 1
SIZES = (200, 1000, 10000)
KINDS = ("bt", "hp")
SHAPES = ("sel_pk", "sel_ix", "ins", "upd_nk", "upd_ix", "del")


def p1_phase(c, pid, name, plist, want_rows, want_affected, batch, ipc=False):
    cpus, lat = [], []
    t_all = time.perf_counter()
    for at in range(0, len(plist), batch):
        part = plist[at:at + batch]
        c0 = settled_cpu_ns(pid)
        for params in part:
            a = time.perf_counter()
            try:
                _f, rows, _t, aff = c.execute(None, params, statement=name)
            except kwp.KwpError as e:
                raise SystemExit(f"refused {name}: {e.code} {e}")
            lat.append(time.perf_counter() - a)
            if want_rows is not None and len(rows) != want_rows:
                raise SystemExit(f"{name}: {len(rows)} rows, want {want_rows}")
            if want_affected is not None and aff != want_affected:
                raise SystemExit(f"{name}: affected {aff}, want {want_affected}")
        c1 = settled_cpu_ns(pid)
        cpus.append((c1 - c0) / len(part) / 1e3)
    wall = time.perf_counter() - t_all
    res = {"cpu_us_med": round(median(cpus), 3), "cpu_us_min": round(min(cpus), 3),
           "cpu_us_max": round(max(cpus), 3), "lat": pcts(lat),
           "qps_client": round(len(plist) / (sum(lat)), 1), "wall_s": round(wall, 3)}
    if ipc:   # the driver's counters after the phase (key on): a pass inside a block shows here
        res["ip"] = {k: int(v) for k, v in meta(c).items() if k.startswith("index_purge")}
    return res


def p1_arm(args, label, binpath, rnd, port):
    wd = os.path.join(args.run_dir, "p1", f"r{rnd}-{label}")
    on = label == "B-on"
    srv = Server(binpath, wd, port, 1, on, args.frames)
    c = srv.connect()
    out = {"arm": label, "round": rnd, "bin_sha": sha(binpath), "cells": {}}
    for kind in KINDS:
        for size in SIZES:
            t = f"{kind}{size}"
            must(c, f"CREATE TABLE {t} (id int64 PRIMARY KEY, k int64, w int64) {'BTREE' if kind == 'bt' else 'HEAP'}")
            must(c, f"CREATE INDEX ix_{t} ON {t} (k)")
            for at in range(1, size + 1, 500):
                vals = ", ".join(f"({i},{i * 7},0)" for i in range(at, min(at + 500, size + 1)))
                must(c, f"INSERT INTO {t} VALUES {vals}")
            c.prepare(f"{t}_sel_pk", f"SELECT * FROM {t} WHERE id = ?")
            c.prepare(f"{t}_sel_ix", f"SELECT * FROM {t} WHERE k = ?")
            c.prepare(f"{t}_ins", f"INSERT INTO {t} VALUES (?, ?, 0)")
            c.prepare(f"{t}_upd_nk", f"UPDATE {t} SET w = ? WHERE id = ?")
            c.prepare(f"{t}_upd_ix", f"UPDATE {t} SET k = ? WHERE id = ?")
            c.prepare(f"{t}_del", f"DELETE FROM {t} WHERE id = ?")
    time.sleep(2)           # the loads' cadence work settles
    S = args.stmts
    for kind in KINDS:
        for size in SIZES:
            t = f"{kind}{size}"
            cell = {}
            ids = [(q % size) + 1 for q in range(S)]
            newids = [size + 1 + q for q in range(S)]
            cell["sel_pk"] = p1_phase(c, srv.pid, f"{t}_sel_pk", [[i64(i)] for i in ids], 1, None, args.batch, on)
            cell["sel_ix"] = p1_phase(c, srv.pid, f"{t}_sel_ix", [[i64(i * 7)] for i in ids], 1, None, args.batch, on)
            cell["ins"] = p1_phase(c, srv.pid, f"{t}_ins", [[i64(i), i64(i * 7)] for i in newids], 0, 1, args.batch, on)
            cell["upd_nk"] = p1_phase(c, srv.pid, f"{t}_upd_nk", [[i64(q), i64(i)] for q, i in enumerate(ids)],
                                      0, 1, args.batch, on)
            cell["upd_ix"] = p1_phase(c, srv.pid, f"{t}_upd_ix",
                                      [[i64(10 ** 7 + q), i64(i)] for q, i in enumerate(ids)], 0, 1, args.batch, on)
            cell["del"] = p1_phase(c, srv.pid, f"{t}_del", [[i64(i)] for i in newids], 0, 1, args.batch, on)
            c0 = cpu_now(srv.pid)
            time.sleep(args.tail_s)
            cell["tail_cpu_us_per_deleted_row"] = round((cpu_now(srv.pid) - c0) / 1e3 / S, 3)
            out["cells"][t] = cell
        beat(args.run_dir, phase="part1", arm=label, round=rnd, kind=kind)
    srv.stop(c)
    return out


def part1(args):
    os.makedirs(args.run_dir, exist_ok=True)
    arms = {"A": args.bin_a, "A2": args.bin_a2, "B-off": args.bin_b}
    if args.with_on:
        arms["B-on"] = args.bin_b
    shas = {k: sha(v) for k, v in arms.items()}
    held = quiet_gate(lambda *a, **k: None)
    json.dump({"started": time.time(), "shas": shas, "gate_hold_s": held, "host": host_state()},
              open(os.path.join(args.run_dir, "part1-meta.json"), "w"))
    if args.with_on:    # the four cyclic rotations: every arm in every position once in four rounds
        base = ("A", "B-off", "A2", "B-on")
        perms = [base[i:] + base[:i] for i in range(4)]
    else:
        perms = list(permutations(("A", "A2", "B-off")))
    results = []
    port = args.base_port
    for rnd in range(args.rounds):
        for label in perms[rnd % len(perms)]:
            quiet_gate(lambda *a, **k: None)
            r = p1_arm(args, label, arms[label], rnd, port)
            r["order_in_round"] = perms[rnd % len(perms)].index(label)
            r["host"] = host_state()
            results.append(r)
            json.dump(results, open(os.path.join(args.run_dir, "part1.json"), "w"))
            beat(args.run_dir, phase="part1-arm-done", arm=label, round=rnd)
    print("part1 done")


# ============================================================================= part 2
MIXES = {"D-rec": 1, "M-rec": 1, "U-nokey": 1, "M-flip": 1, "D-rec-2c": 2}


def load_x(c, mix, n, seed):
    rng = random.Random(seed)
    must(c, "CREATE TABLE x (id int64 PRIMARY KEY, k int64, w int64) BTREE")
    must(c, "CREATE INDEX ix ON x (k)")
    dom = max(1, n // 10) if mix.startswith("D") else n
    for at in range(1, n + 1, 500):
        if mix == "M-flip":
            vals = ", ".join(f"({i},{i},0)" for i in range(at, min(at + 500, n + 1)))
        else:
            vals = ", ".join(f"({i},{rng.randint(1, dom)},0)" for i in range(at, min(at + 500, n + 1)))
        must(c, f"INSERT INTO x VALUES {vals}")
    return dom


def writer(args):
    """One writer connection: the mix until its deadline, a pause on the pause file."""
    rd = args.run_dir
    deadline_connect = time.time() + 60
    want = args.wid if args.want_core else None
    c = None
    while c is None:
        try:
            c = kwp.Connection("127.0.0.1", args.port, timeout=300)
        except OSError:
            if time.time() > deadline_connect:
                raise SystemExit("writer cannot connect")
            time.sleep(0.1)
    if want is not None:
        for _ in range(60):
            if meta(c).get("core") == str(want):
                break
            c.close()
            c = kwp.Connection("127.0.0.1", args.port, timeout=300)
        else:
            raise SystemExit(f"writer {args.wid}: no connection landed on core {want}")
    core0 = meta(c).get("core")
    n, W, wid = args.n, args.nw, args.wid
    rng = random.Random(args.seed * 100 + wid)
    mix = args.mix.split("@")[0]
    dom = max(1, n // 10) if mix.startswith("D") else n
    kind = mix[0]
    if kind == "D":
        c.prepare("ins", "INSERT INTO x VALUES (?, ?, 0)")
        c.prepare("del", "DELETE FROM x WHERE id = ?")
    elif mix in ("M-rec", "M-flip"):
        c.prepare("upd", "UPDATE x SET k = ? WHERE id = ?")
    else:
        c.prepare("upd", "UPDATE x SET w = ? WHERE id = ?")
    refused, wrong = {}, [0]
    lat = array("d")
    kinds = array("b")
    bucket = {}
    top = []
    own = [i for i in range(1, n + 1) if i % W == wid]          # D: this writer's oldest-first live ids
    qi = 0
    flip = bytearray(n + 1)
    nid = n + wid + 1
    counter = 0

    def stmt(name, params):
        for _ in range(5):
            try:
                _f, _r, _t, aff = c.execute(None, params, statement=name)
                return aff
            except kwp.KwpError as e:
                refused[str(e.code)] = refused.get(str(e.code), 0) + 1
                if not e.retryable:
                    return None
        return None

    pause = os.path.join(rd, "pause")
    ack = os.path.join(rd, f"ack-{wid}")
    open(os.path.join(rd, f"ready-{wid}"), "w").write(str(core0))
    start_file = os.path.join(rd, "start")
    while not os.path.exists(start_file):
        time.sleep(0.01)
    t_start = float(open(start_file).read())
    deadline = t_start + args.seconds
    paused = 0.0
    pc = time.perf_counter
    done = False
    while not done:
        if os.path.exists(pause):
            p0 = time.time()
            open(ack, "w").close()
            while os.path.exists(pause):
                time.sleep(0.005)
            os.remove(ack)
            paused += time.time() - p0
        now = time.time()
        if now >= deadline:
            break
        for _ in range(200):
            if kind == "D":
                a = pc()
                aff = stmt("ins", [i64(nid), i64(rng.randint(1, dom))])
                b = pc()
                if aff != 1:
                    wrong[0] += 1
                own.append(nid)
                nid += W
                old = own[qi]
                qi += 1
                aff = stmt("del", [i64(old)])
                c_ = pc()
                if aff != 1:
                    wrong[0] += 1
                lat.append(b - a)
                kinds.append(0)
                lat.append(c_ - b)
                kinds.append(1)
                ops = 2
                if len(own) > 200000:
                    own = own[qi:]
                    qi = 0
            else:
                i = rng.randint(1, n)
                counter += 1
                if mix == "M-flip":
                    flip[i] ^= 1
                    v = i + 1000000 if flip[i] else i
                else:
                    v = rng.randint(1, n) if mix == "M-rec" else counter
                a = pc()
                aff = stmt("upd", [i64(v), i64(i)])
                b = pc()
                if aff != 1:
                    wrong[0] += 1
                lat.append(b - a)
                kinds.append(2)
                ops = 1
            sec = int((time.time() - t_start - paused) // 10)
            bucket[sec] = bucket.get(sec, 0) + ops
    t_end = time.time()
    open(os.path.join(rd, f"done-{wid}"), "w").close()       # before the post-processing: the orchestrator's end sample
    sl = sorted(lat)
    res = {"wid": wid, "core_start": core0, "core_end": meta(c).get("core"),
           "statements": len(lat), "active_s": round(t_end - t_start - paused, 3), "paused_s": round(paused, 3),
           "latency_us": pcts(lat), "refused": refused, "wrong_affected": wrong[0],
           "buckets_10s": [bucket.get(j, 0) for j in range(max(bucket) + 1)] if bucket else [],
           "over_ms": {th: len(sl) - bisect_right(sl, th / 1e3) for th in (1, 2, 5, 10, 50)},
           "top20_us": [round(x * 1e6, 1) for x in sl[-20:]]}
    if kind == "D":
        for kk, nm in ((0, "ins"), (1, "del")):
            res[f"latency_us_{nm}"] = pcts([x for x, y in zip(lat, kinds) if y == kk])
    json.dump(res, open(os.path.join(rd, f"writer-{wid}.json"), "w"))


def read_meta(c):
    return meta(c)


def index_entries(c):
    _f, rows, _t, _n = c.execute("SELECT * FROM sys.indexes")
    names = [f["name"] for f in _f]
    for r in rows:
        d = dict(zip(names, r))
        if d["rel_name"] == b"x":
            return I64.unpack(d["entries"])[0], I64.unpack(d["height"])[0]
    raise SystemExit("no index row for x")


def probe(c, srv, rng, nb, per):
    """`nb` batches of `per` prepared point reads by k over live keys; the batch
    median of server CPU a statement, and the client's latency."""
    lo = I64.unpack(c.execute("SELECT id FROM x LIMIT 1")[1][0][0])[0]
    hi = I64.unpack(c.execute("SELECT id FROM x ORDER BY id DESC LIMIT 1")[1][0][0])[0]
    start = rng.randint(lo, max(lo, hi - nb * per))
    rows = c.execute(f"SELECT id, k FROM x WHERE id >= {start} LIMIT {nb * per}")[1]
    ks = [I64.unpack(r[1])[0] for r in rows]
    if len(ks) < nb * per:
        ks = (ks * (nb * per // max(1, len(ks)) + 1))[:nb * per]
    cpus, lat, got = [], [], 0
    for b in range(nb):
        part = ks[b * per:(b + 1) * per]
        c0 = settled_cpu_ns(srv.pid)
        for k in part:
            a = time.perf_counter()
            r = c.execute(None, [i64(k)], statement="rd")
            lat.append(time.perf_counter() - a)
            got += len(r[1])
        cpus.append((settled_cpu_ns(srv.pid) - c0) / len(part) / 1e3)
    return {"cpu_us_med": round(median(cpus), 3), "cpu_us_min": round(min(cpus), 3),
            "cpu_us_max": round(max(cpus), 3), "lat": pcts(lat), "rows_per_probe": round(got / len(ks), 2)}


def run_cell(args, mix, arm, seq):
    cores = MIXES[mix]
    on = arm == "B-on"
    cell_id = f"{seq:02d}-{mix}-{arm}"
    rd = os.path.join(args.run_dir, "p2", cell_id)
    os.makedirs(rd, exist_ok=True)
    port = args.base_port + 10 + seq
    srv = Server(args.bin_b, rd, port, cores, on, args.frames)
    c = srv.connect(want_core=0)
    base_mix = mix.replace("-2c", "")
    load_x(c, base_mix, args.n, args.seed)
    c.prepare("rd", "SELECT * FROM x WHERE k = ?")
    time.sleep(2)
    rng = random.Random(args.seed + seq)
    series = []
    nw = 2 if mix == "D-rec-2c" else 1
    wp = []
    for w in range(nw):
        cmd = ["taskset", "-c", str(9 + w), sys.executable, os.path.abspath(__file__), "writer",
               "--run-dir", rd, "--port", str(port), "--mix", mix, "--wid", str(w), "--nw", str(nw),
               "--n", str(args.n), "--seconds", str(args.mix_seconds), "--seed", str(args.seed)]
        if cores > 1:
            cmd.append("--want-core")
        wp.append(subprocess.Popen(cmd, stdout=open(os.path.join(rd, f"writer-{w}.log"), "w"),
                                   stderr=subprocess.STDOUT))
    while not all(os.path.exists(os.path.join(rd, f"ready-{w}")) for w in range(nw)):
        for p in wp:
            if p.poll() is not None:
                raise SystemExit(f"writer exited early; see {rd}/writer-*.log")
        time.sleep(0.05)

    def sample(t_rel, label):
        nonlocal c
        # the `index_purge` section is core 0's `SHOW META` only, and a session's core is
        # re-decided at a transaction head: with the driver on, read from a session on core 0
        for _ in range(40):
            m = meta(c)
            if not on or "index_purge_passes" in m:
                break
            c.close()
            c = srv.connect(want_core=0)
            c.prepare("rd", "SELECT * FROM x WHERE k = ?")
        entries, height = index_entries(c)
        pr = probe(c, srv, rng, 3, args.probe_stmts)
        rec = {"t_s": round(t_rel, 1), "label": label, "entries": entries, "height": height,
               "pages_allocated": int(m["pages_allocated"]),
               "index_purge": {k: int(v) for k, v in m.items() if k.startswith("index_purge")},
               "probe": pr}
        series.append(rec)
        return rec

    sample(0, "start")
    time.sleep(1)
    cpu_start = cpu_now(srv.pid)
    t_start = time.time() + 0.5
    open(os.path.join(rd, "start"), "w").write(str(t_start))
    paused_cpu = 0
    next_sample = t_start + args.sample_s
    last_hb = 0
    def all_done():
        return all(os.path.exists(os.path.join(rd, f"done-{w}")) for w in range(nw))

    while not all_done():
        for p in wp:
            if p.poll() not in (None, 0):
                raise SystemExit(f"writer failed ({p.returncode}); see {rd}/writer-*.log")
        now = time.time()
        if now - last_hb > 10:
            beat(args.run_dir, phase="part2", cell=cell_id, elapsed_s=round(now - t_start, 1))
            last_hb = now
        if now >= next_sample and now < t_start + args.mix_seconds - 3:
            open(os.path.join(rd, "pause"), "w").close()
            while not all(os.path.exists(os.path.join(rd, f"ack-{w}")) for w in range(nw)):
                if any(p.poll() is not None for p in wp):
                    break
                time.sleep(0.01)
            c0 = settled_cpu_ns(srv.pid)
            sample(now - t_start, "mix")
            paused_cpu += settled_cpu_ns(srv.pid) - c0
            os.remove(os.path.join(rd, "pause"))
            next_sample = time.time() + args.sample_s
        time.sleep(0.2)
    cpu_end = settled_cpu_ns(srv.pid)
    end = sample(args.mix_seconds, "end")
    for p in wp:
        p.wait()
        if p.returncode != 0:
            raise SystemExit(f"writer failed ({p.returncode}); see {rd}/writer-*.log")
    wres = [json.load(open(os.path.join(rd, f"writer-{w}.json"))) for w in range(nw)]
    counters_end = dict(end["index_purge"])
    t0 = time.perf_counter()
    _f, _r, tag, _n = c.execute("PURGE INDEX ix")
    purge = {"tag": tag, "wall_ms": round((time.perf_counter() - t0) * 1e3, 1)}
    time.sleep(1)
    clean = sample(args.mix_seconds + 1, "after-purge")
    stmts = sum(w["statements"] for w in wres)
    active = max(w["active_s"] for w in wres)
    out = {"cell": cell_id, "mix": mix, "arm": arm, "cores": cores, "driver_on": on,
           "bin_sha": sha(args.bin_b), "statements": stmts, "active_s": active,
           "stmts_per_s": round(stmts / active, 1),
           "server_cpu_us_per_stmt": round((cpu_end - cpu_start - paused_cpu) / 1e3 / stmts, 3),
           "server_cpu_s_total": round((cpu_end - cpu_start - paused_cpu) / 1e9, 3),
           "writers": wres, "series": series, "purge_at_end": purge, "counters_end": counters_end,
           "host": host_state()}
    json.dump(out, open(os.path.join(args.run_dir, "p2", f"{cell_id}.json"), "w"))
    srv.stop(c)
    beat(args.run_dir, phase="part2-cell-done", cell=cell_id)
    return out


def part2(args):
    os.makedirs(os.path.join(args.run_dir, "p2"), exist_ok=True)
    held = quiet_gate(lambda *a, **k: None)
    json.dump({"started": time.time(), "sha_b": sha(args.bin_b), "gate_hold_s": held, "host": host_state()},
              open(os.path.join(args.run_dir, "part2-meta.json"), "w"))
    order = []
    for i, mix in enumerate(args.mixes.split(",")):
        first_off = (i % 2 == 0) != args.flip
        order += [(mix, "B-off"), (mix, "B-on")] if first_off else [(mix, "B-on"), (mix, "B-off")]
    for seq, (mix, arm) in enumerate(order):
        quiet_gate(lambda *a, **k: None)
        run_cell(args, mix, arm, seq + args.seq_base)
    print("part2 done")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=("part1", "part2", "writer"))
    p.add_argument("--run-dir", required=True)
    p.add_argument("--bin-a")
    p.add_argument("--bin-a2")
    p.add_argument("--bin-b")
    p.add_argument("--base-port", type=int, default=15601)
    p.add_argument("--frames", type=int, default=262144)
    p.add_argument("--stmts", type=int, default=2000, help="part1: statements a shape")
    p.add_argument("--batch", type=int, default=100, help="part1: statements a CPU batch")
    p.add_argument("--rounds", type=int, default=6)
    p.add_argument("--with-on", action="store_true", help="part1: a fourth arm, B-on (the key on), and four rotations of the four arms")
    p.add_argument("--tail-s", type=float, default=1.5)
    p.add_argument("--n", type=int, default=50000)
    p.add_argument("--mix-seconds", type=int, default=120)
    p.add_argument("--sample-s", type=int, default=20)
    p.add_argument("--probe-stmts", type=int, default=200)
    p.add_argument("--seed", type=int, default=20261127)
    p.add_argument("--mixes", default="D-rec,M-rec,U-nokey,D-rec-2c")
    p.add_argument("--port", type=int)
    p.add_argument("--mix")
    p.add_argument("--wid", type=int, default=0)
    p.add_argument("--nw", type=int, default=1)
    p.add_argument("--seconds", type=int, default=120)
    p.add_argument("--want-core", action="store_true")
    p.add_argument("--flip", action="store_true", help="part2: the first mix runs on-arm first (a repeat in the opposite order)")
    p.add_argument("--seq-base", type=int, default=0, help="part2: cell numbering starts here (a repeat run beside the first)")
    a = p.parse_args()
    {"part1": part1, "part2": part2, "writer": writer}[a.mode](a)


if __name__ == "__main__":
    main()
