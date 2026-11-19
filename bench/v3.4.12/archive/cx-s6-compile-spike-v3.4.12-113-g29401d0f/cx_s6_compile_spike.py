#!/usr/bin/env python3
"""CX-S6 first cell: the compile's share of a repeated statement's latency.

One server per core count (fresh data file), the binary copy named by --bin.
Phase A (no plan): cell 1 point read, cell 2 two-relation join, cell 3
three-relation join.  Phase B (two plans created): cell 4 = cell 2's statement
under a two-variant plan, cell 5 = a one-relation statement under a
one-relation plan, 5b = the same statement on a twin table with no plan.
Each cell: warm-up, ROUNDS timed passes (client wall latency per statement).
sys.statement_stats is read before and after each pass (outside the timed
loop); compile_us in this build is a SUM over compiled calls, so the per-call
compile is delta(compile_us) / delta(calls) over the pass.
"""
import argparse, json, os, random, struct, subprocess, sys, time

sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/cx-plan-change-option/tools")
import kwp

I64 = struct.Struct("<q")


def stmt1(r): return f"SELECT * FROM pb_bars WHERE id = {r.randint(1, 10000)}"
def stmt2(r): return ("SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
                      f"ON l.user_id = u.id WHERE u.id = {r.randint(1, 200)}")
def stmt3(r): return ("SELECT u.member_code, b.title FROM pc_users AS u JOIN pc_loans AS l "
                      "ON l.user_id = u.id JOIN pc_books AS b ON l.book_id = b.id "
                      f"WHERE u.id = {r.randint(1, 200)}")
def stmt5(r): return f"SELECT * FROM t5 WHERE v = {r.randint(1, 50)}"
def stmt5b(r): return f"SELECT * FROM t5b WHERE v = {r.randint(1, 50)}"


# name, generator, marker in shape_text, marker that must be absent
CELLS = {
    "1-pk-point-read": (stmt1, "FROM pb_bars WHERE", None),
    "2-join-2rel": (stmt2, "JOIN pc_loans", "pc_books"),
    "3-join-3rel": (stmt3, "JOIN pc_books", None),
    "4-join-2rel-plan2var": (stmt2, "JOIN pc_loans", "pc_books"),
    "5-one-rel-plan": (stmt5, "FROM t5 WHERE", None),
    "5b-one-rel-no-plan": (stmt5b, "FROM t5b WHERE", None),
}
PHASE_A = ["1-pk-point-read", "2-join-2rel", "3-join-3rel"]
PHASE_B = ["4-join-2rel-plan2var", "5-one-rel-plan", "5b-one-rel-no-plan"]

SCHEMA = {
    "pb_bars": "id int64, symbol_id int64, session_no int32, open int64, high int64, low int64, close int64, volume int64",
    "pc_users": "id int64, member_code varchar",
    "pc_loans": "id int64, user_id int64, book_id int64",
    "pc_books": "id int64, title varchar",
    "t5": "id int64, v int64",
    "t5b": "id int64, v int64",
}


def rows_for():
    rng = random.Random(1)
    R = {"pb_bars": [f"({i}, {1 + i % 8}, {i % 1000}, {rng.randint(9000, 11000)}, {rng.randint(9000, 11000)}, "
                     f"{rng.randint(9000, 11000)}, {rng.randint(9000, 11000)}, {rng.randint(1000, 50000000)})"
                     for i in range(1, 10001)],
         "pc_users": [f"({i}, 'member{i:05d}')" for i in range(1, 201)],
         "pc_loans": [f"({i}, {rng.randint(1, 200)}, {rng.randint(1, 2000)})" for i in range(1, 10001)],
         "pc_books": [f"({i}, 'title{i:05d}')" for i in range(1, 2001)],
         "t5": [f"({i}, {(i - 1) % 50 + 1})" for i in range(1, 201)],
         "t5b": [f"({i}, {(i - 1) % 50 + 1})" for i in range(1, 201)]}
    return R


def status(a, line):
    tmp = a.status + ".tmp"
    with open(tmp, "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, a.status)


def host_state():
    la = open("/proc/loadavg").read().strip()
    p = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True)
    mine = [l for l in p.stdout.splitlines() if "pgrep" not in l]
    return {"loadavg": la, "competing": mine}


class Srv:
    def __init__(self, a, cores, port, cpus):
        self.cores, self.port = cores, port
        wd = a.wd
        self.data = os.path.join(wd, f"c{cores}.db")
        subprocess.run(["rm", "-rf", self.data, self.data + ".wal"])
        conf = os.path.join(wd, f"c{cores}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = {cores}\ndurability = group\n"
                    f"buffer_pool_frames = 16384\nlog_file = c{cores}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.conf = open(conf).read()
        self.err = open(os.path.join(wd, f"c{cores}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", cpus, a.bin, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cx_s6"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit("server did not start")
                time.sleep(0.1)

    def must(self, sql):
        tag, rows, _b, err = self.conn.query(sql)
        if err is not None: sys.exit(f"{sql[:100]} -> {err}")
        return rows

    def load(self, R):
        for t, cols in SCHEMA.items():
            self.must(f"CREATE TABLE {t} ({cols}) BTREE")
        for t, rows in R.items():
            cols = ", ".join(c.split()[0] for c in SCHEMA[t].split(", "))
            for i in range(0, len(rows), 500):
                self.must(f"INSERT INTO {t} ({cols}) VALUES " + ", ".join(rows[i:i + 500]))
        self.must("CREATE INDEX pc_loans_user ON pc_loans (user_id)")
        for t, rows in R.items():
            n = self.must(f"SELECT * FROM {t}")
            if n != len(rows): sys.exit(f"{t}: {n} != {len(rows)}")

    def stats(self):
        fields, rows, _t, _n = self.conn.execute("SELECT * FROM sys.statement_stats")
        names = [f["name"] for f in fields]
        out = []
        for r in rows:
            d = {}
            for n, v in zip(names, r):
                if v is None: d[n] = None
                elif n in ("kind", "plan_source", "plan_name", "variant", "shape_text"): d[n] = v.decode(errors="replace")
                elif len(v) == 8: d[n] = I64.unpack(v)[0]
                else: d[n] = v
            out.append(d)
        return out

    def spike(self):
        rows = self.conn.execute("SELECT * FROM sys.meta")[2 - 1]
        d = {}
        for r in rows:
            if r[1] in (b"spike_compile_ns", b"spike_compile_calls"): d[r[1].decode()] = int(r[2])
        return d

    def row(self, mark, absent):
        for d in self.stats():
            t = d["shape_text"]
            if d["kind"] == "SELECT" and mark in t and (absent is None or absent not in t):
                return d
        return None

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def run_cell(a, s, name, rnd_seed):
    make, mark, absent = CELLS[name]
    host = host_state()
    warm = random.Random(f"{name}-warm")
    for _ in range(a.warm):
        if s.conn.query(make(warm))[3] is not None: sys.exit(f"warm {name} failed")
    def an():
        return [" | ".join(x.decode(errors="replace") if x is not None else "NULL" for x in row)
                for row in s.conn.execute("ANALYZE " + make(random.Random("an")))[1]]
    analyze, polls = an(), 0
    if name in ("2-join-2rel", "3-join-3rel"):
        # a captured order needs every relation's prior published (a steward step): poll up to 40 s
        while not any("order=captured" in l for l in analyze) and polls < 40:
            time.sleep(1); polls += 1
            for _ in range(50): s.conn.query(make(warm))
            analyze = an()
    rounds = []
    for rnd in range(a.rounds):
        status(a, f"cores={s.cores} {name} round {rnd + 1}/{a.rounds}")
        rng = random.Random(f"{name}-{rnd}-{rnd_seed}")
        stmts = [make(rng) for _ in range(a.ops)]
        lat = []
        before = s.row(mark, absent)
        sp0 = s.spike()
        w0 = time.perf_counter()
        for q in stmts:
            t0 = time.perf_counter()
            _t, n, _b, err = s.conn.query(q)
            lat.append((time.perf_counter() - t0) * 1e6)
            if err is not None: sys.exit(f"{name}: {err}")
        wall = time.perf_counter() - w0
        sp1 = s.spike()
        after = s.row(mark, absent)
        dl = {k: after[k] - before[k] for k in ("calls", "total_us", "timed_calls", "cpu_us", "compile_us", "rows")}
        rounds.append({"ops": a.ops, "qps": a.ops / wall, "p0": pct(lat, 0), "p25": pct(lat, .25),
                       "p50": pct(lat, .5), "p95": pct(lat, .95), "p99": pct(lat, .99),
                       "mean": sum(lat) / len(lat), "server_delta": dl,
                       "spike_ns": sp1["spike_compile_ns"] - sp0["spike_compile_ns"],
                       "spike_calls": sp1["spike_compile_calls"] - sp0["spike_compile_calls"],
                       "compile_us_per_call": dl["compile_us"] / dl["calls"],
                       "server_total_us_per_call": dl["total_us"] / dl["calls"],
                       "server_cpu_us_per_call": dl["cpu_us"] / dl["timed_calls"]})
    return {"cell": name, "cores": s.cores, "host": host, "analyze": analyze, "capture_polls_s": polls,
            "rounds": rounds, "final_stats_row": s.row(mark, absent)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin"); p.add_argument("--wd"); p.add_argument("--json"); p.add_argument("--status")
    p.add_argument("--cores", type=int); p.add_argument("--port", type=int); p.add_argument("--cpus")
    p.add_argument("--rounds", type=int, default=3); p.add_argument("--ops", type=int, default=10000)
    p.add_argument("--warm", type=int, default=2000); 
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    s = Srv(a, a.cores, a.port, a.cpus)
    status(a, "load"); s.load(rows_for())
    out = {"cores": a.cores, "conf": s.conf, "cells": []}
    for c in PHASE_A:
        out["cells"].append(run_cell(a, s, c, 1)); json.dump(out, open(a.json, "w"), indent=1)
    s.must("CREATE PLAN p FOR SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
           "ON l.user_id = u.id WHERE u.id = 100 VARIANT a WHEN ARG(1) < 100 ORDER (u, l) "
           "VARIANT b ELSE ORDER (l, u)")
    s.must("CREATE PLAN q FOR SELECT * FROM t5 WHERE v = 7 VARIANT v ELSE ORDER (t5) ACCESS t5 SCAN")
    out["plans"] = [[x.decode(errors="replace") if x is not None else "NULL" for x in r]
                    for r in s.conn.execute("SELECT * FROM sys.plans")[1]]
    for c in PHASE_B:
        out["cells"].append(run_cell(a, s, c, 1)); json.dump(out, open(a.json, "w"), indent=1)
    s.stop(); status(a, "DONE")


if __name__ == "__main__":
    main()
