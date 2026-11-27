#!/usr/bin/env python3
"""DW-S0's premise: what a btree relation's secondary index holds after a
delete-heavy or a key-moving mix nothing purges, and what a point read by that
index pays for it against a rebuilt one.

`instructions/v3.2.0/workorder-dw-btree-purge-driver.md` section 5 (DW-S0),
method of `bench/v3.4.17/dd-s0-premise-v3.4.17-1-g054dbc5e.md`.

Four cells, one server each (`cores = 1`, `durability = relaxed`, a fresh data
file, a copy of the binary), three twin relations `x_a`, `x_b`, `x_c`
(`id int64 PRIMARY KEY, k int64, w int64`, BTREE, index `ix_<a|b|c>` on `k`)
receiving the same history, statements rotating across the three:

    D-rec    steady --n live rows: INSERT (named id, k uniform in n/10 values)
             and DELETE the oldest live id, per iteration
    D-fresh  as D-rec, k a monotonically increasing value never reused
    M-rec    --n rows never deleted: UPDATE x SET k = ? WHERE id = ?, a random id,
             k uniform in n values
    M-fresh  as M-rec, k a monotonically increasing value never reused

Phase 1 (the mix, `--mix-seconds`): every `--sample-s` seconds each relation's
index entries (`sys.indexes`), the driver's live-row / insert / delete / move
counts and the statement rate go to `<run>/series-<cell>.jsonl`. A heartbeat
file `<run>/hb-<cell>.json` is rewritten at least every 15 s.

Phase 2 (the read, after the mix, when the orchestrator drops
`<run>/go-<cell>`): `REINDEX INDEX ix_a`; a prepared point read
`SELECT * FROM x_<r> WHERE k = ?` over keys live in every twin, `--reps` x
`--blocks` blocks of `--stmts` statements a relation, the three relations in a
rotating order inside each block, server CPU per statement from the
`/proc/<pid>/task/*/schedstat` delta (the control is x_b against x_c);
then, with `--purge`, `PURGE INDEX ix_b` once (reply, wall time, entries after)
and the same blocks again (a REINDEX cannot take a deleted row's entry
out, so in the D cells the purged index is the reference). At every sample of
the mix `--probe-stmts` prepared point reads on x_c are timed, serially, so the
cost of a probe is a series. Every reply is checked; refusals are
counted by code.

    tools/dw_s0_premise.py --bin <copy of kds_server> --run-dir ~/bench-runs/dw-s0/run \\
        --base-port 15521 --mix-seconds 480

`recheck --cell <cell> --purge-rel c` mounts a finished cell's data file again, purges
one more twin's index and times the three again (the same file the run left).

`worker` is the per-cell process the orchestrator starts; it is not run by hand.
"""

import argparse
import json
import os
import random
import socket
import struct
import subprocess
import sys
import time
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bn_census import sched_stat  # noqa: E402

CELLS = ("D-rec", "D-fresh", "M-rec", "M-fresh")
RELS = ("a", "b", "c")
I64 = struct.Struct("<q")


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    # `pgrep -f` on the words alone matches the caller's own shell; anchor them
    cc = subprocess.run(["pgrep", "-x", "cc1plus"], capture_output=True, text=True).stdout.split()
    mk = subprocess.run(["pgrep", "-f", "^(cmake --build|ctest)"], capture_output=True, text=True).stdout.split()
    return {"loadavg": load, "competing": len(cc) + len(mk), "cc1plus": len(cc), "cmake_or_ctest": len(mk)}


def pcts(lat):
    s = sorted(lat)
    n = len(s)
    at = lambda q: s[min(n - 1, int(q * (n - 1) + 0.5))] * 1e6
    return {"ops": n, "p0": round(s[0] * 1e6, 1), "p25": round(at(.25), 1), "p50": round(at(.5), 1),
            "p95": round(at(.95), 1), "p99": round(at(.99), 1),
            "mean": round(sum(s) / n * 1e6, 1)}


class Cell:
    """One cell's server and its one driver session."""

    def __init__(self, args, name):
        self.args, self.name = args, name
        self.dir = os.path.join(args.run_dir, name)
        os.makedirs(self.dir, exist_ok=True)
        self.port = args.base_port + CELLS.index(name)
        self.refused = {}
        self.statements = 0
        self.proc = None
        self.conn = None

    # -- server ---------------------------------------------------------------
    def start(self):
        conf = os.path.join(self.dir, "s.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/d.db\nport = {self.port}\ncores = 1\n"
                    f"durability = relaxed\nbuffer_pool_frames = {self.args.frames}\n"
                    f"log_file = s.log\nlog_dir = {self.dir}\nlog_level = warn\n")
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", self.port)) == 0:
                raise SystemExit(f"port {self.port} is in use")
        err = open(os.path.join(self.dir, "s.stderr"), "w")
        cpus = self.args.server_cpus[CELLS.index(self.name)]
        self.proc = subprocess.Popen(["taskset", "-c", cpus, self.args.bin, "--config", conf],
                                     stdout=err, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", self.port, timeout=120)
                return
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise SystemExit(f"server did not listen; see {self.dir}/s.stderr")
                time.sleep(0.1)

    def stop(self):
        try:
            self.conn.query("STOP")
        except Exception:
            pass
        try:
            self.proc.wait(60)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    # -- statements -------------------------------------------------------------
    def run(self, sql, params=None, stmt=None, tries=5):
        """One statement, its error counted by code and retried if retryable."""
        for _ in range(tries):
            try:
                self.statements += 1
                return self.conn.execute(sql, params, statement=stmt)
            except kwp.KwpError as e:
                code = str(e.code)
                self.refused[code] = self.refused.get(code, 0) + 1
                if not e.retryable:
                    break
        return None

    def must(self, sql):
        r = self.run(sql)
        if r is None:
            raise SystemExit(f"{sql!r} refused: {self.refused}")
        return r

    def view(self, sql):
        fields, rows, _tag, _n = self.must(sql)
        names = [f["name"] for f in fields]
        out = []
        for row in rows:
            d = {}
            for n, f, v in zip(names, fields, row):
                d[n] = None if v is None else (I64.unpack(v)[0] if f["type_oid"] == 5 and len(v) == 8
                                               else v.decode(errors="replace"))
            out.append(d)
        return out

    def index_stats(self):
        return {r["rel_name"]: r for r in self.view("SELECT * FROM sys.indexes")
                if r["rel_name"].startswith("x_")}

    def meta(self):
        _f, rows, _t, _n = self.must("SHOW META")
        return {r[1].decode(): r[2].decode() for r in rows}

    def heartbeat(self, phase, **kw):
        kw.update(phase=phase, t=round(time.time(), 1), cell=self.name,
                  refused=self.refused, statements=self.statements)
        tmp = os.path.join(self.args.run_dir, f"hb-{self.name}.json.tmp")
        with open(tmp, "w") as f:
            json.dump(kw, f)
        os.replace(tmp, os.path.join(self.args.run_dir, f"hb-{self.name}.json"))


def worker(args):
    cell = Cell(args, args.cell)
    kind = args.cell[0]            # D or M
    fresh = args.cell.endswith("fresh")
    n = args.n
    rng = random.Random(args.seed + CELLS.index(args.cell))
    cell.start()
    cell.heartbeat("load")
    c = cell
    for r in RELS:
        c.must(f"CREATE TABLE x_{r} (id int64 PRIMARY KEY, k int64, w int64) BTREE")
        c.must(f"CREATE INDEX ix_{r} ON x_{r} (k)")
    dom = max(1, n // 10) if kind == "D" else n
    nextk = [10 ** 9]      # fresh values: above any recurring value

    def draw():
        if fresh:
            nextk[0] += 1
            return nextk[0]
        return rng.randint(1, dom)

    # the same history in all three twins
    live = {}                 # id -> k
    queue = deque()           # D: ids oldest first
    for at in range(1, n + 1, 500):
        rows = []
        for i in range(at, min(at + 500, n + 1)):
            k = i if (kind == "M" and fresh) else draw()
            rows.append((i, k))
            live[i] = k
            if kind == "D":
                queue.append(i)
        vals = ", ".join(f"({i},{k},0)" for i, k in rows)
        for r in RELS:
            c.must(f"INSERT INTO x_{r} VALUES {vals}")
    i64 = kwp.Connection.int64
    for r in RELS:
        if kind == "D":
            c.conn.prepare(f"ins_{r}", f"INSERT INTO x_{r} VALUES (?, ?, 0)")
            c.conn.prepare(f"del_{r}", f"DELETE FROM x_{r} WHERE id = ?")
        else:
            c.conn.prepare(f"upd_{r}", f"UPDATE x_{r} SET k = ? WHERE id = ?")
        c.conn.prepare(f"rd_{r}", f"SELECT * FROM x_{r} WHERE k = ?")

    series = open(os.path.join(args.run_dir, f"series-{args.cell}.jsonl"), "w")
    sess2 = kwp.Connection("127.0.0.1", cell.port, timeout=120)   # sampler's own session
    c0 = cell.conn
    cell.conn = sess2
    base = c.index_stats()
    base_meta = c.meta()
    cell.conn = c0
    ops = {"inserts": 0, "deletes": 0, "moves": 0, "same_value_moves": 0}
    nid = n
    t0 = time.time()
    last_sample, last_hb = t0, t0
    last_stmts = cell.statements
    sample_no = 0

    def probe():
        """`--probe-stmts` prepared point reads on x_c over live keys, serial: the
        cost of a probe now (server CPU from schedstat, client wall), no mix
        statement inside."""
        ks = list(live.values())
        ks = [ks[rng.randrange(len(ks))] for _ in range(args.probe_stmts)]
        cpu0, _w = sched_stat(cell.proc.pid)
        t1 = time.perf_counter()
        got = 0
        for k in ks:
            res = c.run(None, [i64(k)], stmt="rd_c", tries=1)
            got += len(res[1]) if res else 0
        wall = time.perf_counter() - t1
        cpu1, _w = sched_stat(cell.proc.pid)
        return {"cpu_us": round((cpu1 - cpu0) / 1e3 / len(ks), 3), "wall_us": round(wall / len(ks) * 1e6, 3),
                "rows": round(got / len(ks), 3)}

    def sample(now):
        nonlocal last_sample, last_stmts, sample_no
        pr = probe()
        cell.conn = sess2
        st = c.index_stats()
        m = c.meta()
        cell.conn = c0
        rec = {"t_s": round(now - t0, 1), "sample": sample_no, "ops": dict(ops),
               "live_rows": len(live),
               "stmt_rate_per_s": (round((cell.statements - last_stmts) / (now - last_sample), 1) if sample_no else None),
               "statements": cell.statements,
               "entries": {r: st[f"x_{r}"]["entries"] for r in RELS},
               "height": {r: st[f"x_{r}"]["height"] for r in RELS},
               "pages_allocated": int(m["pages_allocated"]), "refused": dict(cell.refused),
               "probe_x_c": pr}
        series.write(json.dumps(rec) + "\n")
        series.flush()
        last_sample, last_stmts = time.time(), cell.statements   # the probe's time is not a mix rate
        sample_no += 1

    sample(t0)
    deadline = t0 + args.mix_seconds
    while True:
        now = time.time()
        if now >= deadline:
            break
        if now - last_sample >= args.sample_s:
            sample(now)
        if now - last_hb >= 15:
            cell.heartbeat("mix", elapsed_s=round(now - t0, 1), **ops)
            last_hb = now
        for _ in range(100):
            if kind == "D":
                nid += 1
                k = draw()
                for r in RELS:
                    c.run(None, [i64(nid), i64(k)], stmt=f"ins_{r}")
                live[nid] = k
                queue.append(nid)
                old = queue.popleft()
                for r in RELS:
                    c.run(None, [i64(old)], stmt=f"del_{r}")
                del live[old]
                ops["inserts"] += 1
                ops["deletes"] += 1
            else:
                i = rng.randint(1, n)
                k = draw()
                if live[i] == k:
                    ops["same_value_moves"] += 1
                for r in RELS:
                    c.run(None, [i64(k), i64(i)], stmt=f"upd_{r}")
                live[i] = k
                ops["moves"] += 1
    sample(time.time())
    cell.heartbeat("mix-done", elapsed_s=round(time.time() - t0, 1), **ops)
    json.dump({"base_entries": {r: base[f"x_{r}"]["entries"] for r in RELS},
               "base_height": {r: base[f"x_{r}"]["height"] for r in RELS},
               "base_pages_allocated": int(base_meta["pages_allocated"]),
               "n": n, "domain": dom, "kind": kind, "fresh": fresh,
               "mix_wall_s": round(time.time() - t0, 1), "ops": ops,
               "refused": cell.refused, "live_rows": len(live)},
              open(os.path.join(args.run_dir, f"mix-{args.cell}.json"), "w"))

    # ---- wait for the orchestrator, then the read phase -----------------------
    go = os.path.join(args.run_dir, f"go-{args.cell}")
    while not os.path.exists(go):
        time.sleep(1)
        cell.heartbeat("mix-done-waiting")
    out = read_phase(args, cell, live, rng)
    json.dump(out, open(os.path.join(args.run_dir, f"read-{args.cell}.json"), "w"), indent=1)
    cell.heartbeat("read-done")
    cell.stop()


class Timer:
    """The timed point reads: blocks of prepared `SELECT * FROM x_<r> WHERE k = ?`
    over a key pool, the relations in a rotating order, server CPU from schedstat."""

    def __init__(self, c, kl, perkey, args):
        self.c, self.kl, self.perkey, self.args = c, kl, perkey, args
        self.pid = c.proc.pid

    def block(self, rel, stmts, wrong):
        c, kl, perkey = self.c, self.kl, self.perkey
        i64 = kwp.Connection.int64
        cpu0, wait0 = sched_stat(self.pid)
        lat = []
        rows_got = 0
        for q in range(stmts):
            k = kl[q % len(kl)]
            t1 = time.perf_counter()
            res = c.run(None, [i64(k)], stmt=f"rd_{rel}", tries=1)
            lat.append(time.perf_counter() - t1)
            if res is None:
                continue
            rows_got += len(res[1])
            if len(res[1]) != perkey[k]:
                wrong[0] += 1
        cpu1, wait1 = sched_stat(self.pid)
        return {"cpu_us_per_stmt": (cpu1 - cpu0) / 1e3 / stmts, "wait_us_per_stmt": (wait1 - wait0) / 1e3 / stmts,
                "wall_us_per_stmt": sum(lat) / stmts * 1e6, "rows_per_stmt": rows_got / stmts}, lat

    def warm(self):
        wrong = [0]
        for r in RELS:
            self.block(r, self.args.stmts, wrong)

    def timed(self, tag, reps, blocks):
        c = self.c
        wrong = [0]
        data = {r: {"blocks": [], "lat": []} for r in RELS}
        b = 0
        for rep in range(reps):
            for _ in range(blocks):
                order = RELS[b % 3:] + RELS[:b % 3]
                for r in order:
                    res, lat = self.block(r, self.args.stmts, wrong)
                    res["rep"] = rep
                    data[r]["blocks"].append(res)
                    data[r]["lat"].extend(lat)
                b += 1
            c.heartbeat(f"read-{tag}", rep=rep)
        summ = {}
        for r in RELS:
            bl = data[r]["blocks"]
            per_rep = []
            for rep in range(reps):
                xs = [x for x in bl if x["rep"] == rep]
                per_rep.append({k: round(sum(x[k] for x in xs) / len(xs), 3)
                                for k in ("cpu_us_per_stmt", "wait_us_per_stmt", "wall_us_per_stmt",
                                          "rows_per_stmt")})
            summ[r] = {"per_rep": per_rep,
                       "cpu_us_per_stmt": round(sum(x["cpu_us_per_stmt"] for x in bl) / len(bl), 3),
                       "latency_us": pcts(data[r]["lat"]),
                       "block_cpu": [round(x["cpu_us_per_stmt"], 2) for x in bl]}
        return {"summary": summ, "wrong_row_counts": wrong[0]}


def recheck(args):
    """Mount a finished cell's data file again (its clean STOP wrote every page back),
    `PURGE INDEX` the twin `--purge-rel` and time the three again: whether a purge
    alone moves a relation whose key runs hold no stale entry."""
    cell = Cell(args, args.cell)
    cell.start()
    c = cell
    for r in RELS:
        c.conn.prepare(f"rd_{r}", f"SELECT * FROM x_{r} WHERE k = ?")
    rows = c.must("SELECT id, k FROM x_a")[1]
    perkey = {}
    for row in rows:
        k = I64.unpack(row[1])[0]
        perkey[k] = perkey.get(k, 0) + 1
    rng = random.Random(args.seed)
    keys = sorted(perkey)
    kl = [keys[rng.randrange(len(keys))] for _ in range(args.key_pool)]
    tm = Timer(c, kl, perkey, args)
    out = {"cell": args.cell, "host_before": host_state(), "entries_before": {
        r: c.index_stats()[f"x_{r}"]["entries"] for r in RELS}}
    tm.warm()
    out["before"] = tm.timed("recheck-before", args.reps, args.blocks)
    t = time.perf_counter()
    _f, _r, tag, _n = c.must(f"PURGE INDEX ix_{args.purge_rel}")
    out["purge"] = {"rel": args.purge_rel, "tag": tag, "wall_ms": round((time.perf_counter() - t) * 1e3, 1)}
    out["entries_after"] = {r: c.index_stats()[f"x_{r}"]["entries"] for r in RELS}
    tm.warm()
    out["after"] = tm.timed("recheck-after", args.reps, args.blocks)
    out["refused"] = dict(c.refused)
    json.dump(out, open(os.path.join(args.run_dir, f"recheck-{args.cell}.json"), "w"), indent=1)
    cell.stop()


def read_phase(args, cell, live, rng):
    c = cell
    i64 = kwp.Connection.int64
    out = {"cell": args.cell, "host_before": host_state()}
    st = c.index_stats()
    out["entries_before"] = {r: st[f"x_{r}"]["entries"] for r in RELS}
    out["height_before"] = {r: st[f"x_{r}"]["height"] for r in RELS}
    rs = {r["name"]: r for r in c.view("SELECT * FROM sys.relation_stats") if r["name"].startswith("x_")}
    out["relation_stats"] = {r: {k: rs[f"x_{r}"][k] for k in
                                 ("leaves", "est_rows", "deletes", "updates", "inserts", "est_delete_marked",
                                  "leaf_divisions", "leaf_compactions", "height")} for r in RELS}
    # live rows per key value (the driver's own count: the twins hold one history)
    perkey = {}
    for k in live.values():
        perkey[k] = perkey.get(k, 0) + 1
    out["live_distinct_keys"] = len(perkey)
    out["live_rows_per_key_mean"] = round(sum(perkey.values()) / len(perkey), 3)

    # ANALYZE: the read uses the index
    sample_k = next(iter(perkey))
    for r in RELS:
        _f, rows, _t, _n = c.must(f"ANALYZE SELECT * FROM x_{r} WHERE k = {sample_k}")
        out.setdefault("analyze", {})[r] = [x[0].decode() for x in rows]

    # REINDEX x_a
    m0 = c.meta()
    t = time.perf_counter()
    _f, _r, tag, _n = c.must("REINDEX INDEX ix_a")
    out["reindex"] = {"tag": tag, "wall_ms": round((time.perf_counter() - t) * 1e3, 1)}
    time.sleep(1)
    st = c.index_stats()
    out["entries_after_reindex"] = {r: st[f"x_{r}"]["entries"] for r in RELS}
    out["height_after_reindex"] = {r: st[f"x_{r}"]["height"] for r in RELS}
    m1 = c.meta()
    out["pages_allocated_delta_reindex"] = int(m1["pages_allocated"]) - int(m0["pages_allocated"])

    keys = sorted(perkey)
    kl = [keys[rng.randrange(len(keys))] for _ in range(args.key_pool)]
    out["key_pool"] = len(kl)
    out["rows_per_probe_mean"] = round(sum(perkey[k] for k in kl) / len(kl), 3)
    tm = Timer(c, kl, perkey, args)
    tm.warm()
    out["after_reindex"] = tm.timed("after-reindex", args.reps, args.blocks)
    out["host_after_timed"] = host_state()
    out["refused_during_read"] = dict(c.refused)

    if args.purge:
        m0 = c.meta()
        t = time.perf_counter()
        _f, _r, tag, _n = c.must("PURGE INDEX ix_b")
        out["purge"] = {"tag": tag, "wall_ms": round((time.perf_counter() - t) * 1e3, 1)}
        st = c.index_stats()
        out["entries_after_purge"] = {r: st[f"x_{r}"]["entries"] for r in RELS}
        out["height_after_purge"] = {r: st[f"x_{r}"]["height"] for r in RELS}
        out["pages_allocated_delta_purge"] = int(c.meta()["pages_allocated"]) - int(m0["pages_allocated"])
        tm.warm()
        out["after_purge"] = tm.timed("after-purge", args.reps, args.blocks)
    return out


def orchestrate(args):
    os.makedirs(args.run_dir, exist_ok=True)
    state = {"started": time.time(), "host": host_state()}
    json.dump(state, open(os.path.join(args.run_dir, "orchestrator.json"), "w"))
    procs = {}
    for i, name in enumerate(CELLS):
        cmd = ["taskset", "-c", str(8 + i), sys.executable, os.path.abspath(__file__), "worker",
               "--cell", name, "--run-dir", args.run_dir, "--bin", args.bin, "--base-port", str(args.base_port),
               "--n", str(args.n), "--frames", str(args.frames), "--mix-seconds", str(args.mix_seconds),
               "--sample-s", str(args.sample_s), "--seed", str(args.seed), "--reps", str(args.reps),
               "--blocks", str(args.blocks), "--stmts", str(args.stmts), "--key-pool", str(args.key_pool), "--probe-stmts", str(args.probe_stmts),
               "--server-cpus", ",".join(args.server_cpus)] + (["--purge"] if args.purge else [])
        log = open(os.path.join(args.run_dir, f"worker-{name}.log"), "w")
        procs[name] = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    # wait for every mix to finish
    while any(not os.path.exists(os.path.join(args.run_dir, f"mix-{n}.json")) for n in CELLS):
        for n, p in procs.items():
            if p.poll() is not None and not os.path.exists(os.path.join(args.run_dir, f"mix-{n}.json")):
                raise SystemExit(f"worker {n} exited early; see worker-{n}.log")
        with open(os.path.join(args.run_dir, "orchestrator.hb"), "w") as f:
            f.write(str(time.time()))
        time.sleep(5)
    # the read phase: one cell at a time, nothing else running
    for n in CELLS:
        open(os.path.join(args.run_dir, f"go-{n}"), "w").close()
        while procs[n].poll() is None:
            with open(os.path.join(args.run_dir, "orchestrator.hb"), "w") as f:
                f.write(f"read {n} {time.time()}")
            time.sleep(2)
        if not os.path.exists(os.path.join(args.run_dir, f"read-{n}.json")):
            raise SystemExit(f"worker {n} left no read result; see worker-{n}.log")
    print("done")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", nargs="?", default="run", choices=("run", "worker", "recheck"))
    p.add_argument("--cell", choices=CELLS)
    p.add_argument("--bin", required=True, help="a COPY of build-release/kds_server")
    p.add_argument("--run-dir", required=True, help="on a block device, never tmpfs (data files)")
    p.add_argument("--base-port", type=int, required=True, help="four consecutive ports; never 15432")
    p.add_argument("--n", type=int, default=50000, help="live rows per relation (D) / rows (M)")
    p.add_argument("--frames", type=int, default=262144, help="buffer_pool_frames")
    p.add_argument("--mix-seconds", type=int, default=480)
    p.add_argument("--sample-s", type=int, default=30)
    p.add_argument("--seed", type=int, default=20261127)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--blocks", type=int, default=4, help="blocks per repetition")
    p.add_argument("--stmts", type=int, default=1000, help="statements per block per relation")
    p.add_argument("--key-pool", type=int, default=500)
    p.add_argument("--purge-rel", default="c", choices=RELS, help="recheck: the twin whose index is purged")
    p.add_argument("--probe-stmts", type=int, default=300, help="point reads on x_c at every sample")
    p.add_argument("--purge", action="store_true", help="PURGE INDEX ix_b after the timed blocks")
    p.add_argument("--server-cpus", default="0-1,2-3,4-5,6-7",
                   help="taskset list per cell, comma-separated in cell order (P-cores)")
    a = p.parse_args()
    a.server_cpus = a.server_cpus.split(",") if isinstance(a.server_cpus, str) else a.server_cpus
    # a CPU range itself holds a comma-free '-' only; a list of 4 groups
    if a.mode == "worker":
        worker(a)
    elif a.mode == "recheck":
        recheck(a)
    else:
        orchestrate(a)


if __name__ == "__main__":
    main()
