#!/usr/bin/env python3
"""CX close: interleaved A/B over the CX milestone (opening commit vs close),
derived from the CB close driver (cb_close2.py).  Modes:
  main  : A, A2 (control), B (join_order=fixed default), Bp (join_order=planned)
  probe : four servers of B: P0 (no plan), P0b (control), P1 (enabled plan for an
          unrelated join shape), P2 (enabled one-relation plan for an unrelated statement)"""
import argparse, json, os, random, shutil, statistics, subprocess, sys, time

sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/cx-plan-change-option/tools")
import kwp
from bn_census import sched_ns
from bb_overhead_benchmark import host_state

SESSIONS, SYMBOLS = 7560, 8
BARS = SESSIONS * SYMBOLS


def bars_stmt(r, a): return f"SELECT * FROM pb_bars WHERE id = {r.randint(1, BARS)}"
def pj_stmt(r, a):
    return ("SELECT b.id, b.close, s.ticker, e.code FROM pb_bars AS b "
            "JOIN pb_symbols AS s ON b.symbol_id = s.id "
            "JOIN pb_exchanges AS e ON s.exchange_id = e.id "
            f"WHERE b.id = {r.randint(1, BARS)}")
def pj2_stmt(r, a):
    return ("SELECT b.id, b.close, s.ticker FROM pb_bars AS b "
            f"JOIN pb_symbols AS s ON b.symbol_id = s.id WHERE b.id = {r.randint(1, BARS)}")
JNL = ("SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
       "ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16")
JNL_REV = ("SELECT l.book_id, u.member_code FROM pc_loans AS l JOIN pc_users AS u "
           "ON u.id = l.user_id WHERE u.id BETWEEN 1 AND 16")
MJ = ("SELECT r.period_no, r.pnl_bp, m.name FROM pc_results AS r JOIN pc_models AS m "
      "ON r.model_id = m.id WHERE r.model_id = 3")

CELLS = {
    "pk-point-read": (1.0, bars_stmt),
    "point-join-2rel": (1.0, pj2_stmt),
    "point-join-3rel": (1.0, pj_stmt),
    "join-no-literal": (1.0, lambda r, a: JNL),
    "join-no-literal-rev": (0.5, lambda r, a: JNL_REV),
    "model-join": (1.0, lambda r, a: MJ),
}
CAPTURE_CELLS = ("join-no-literal", "join-no-literal-rev", "model-join")

SCHEMA = {
    "pb_exchanges": "id int64, code varchar, country varchar",
    "pb_symbols": "id int64, ticker varchar, exchange_id int64",
    "pb_bars": "id int64, symbol_id int64, session_no int32, open int64, high int64, low int64, close int64, volume int64",
    "pc_users": "id int64, member_code varchar",
    "pc_loans": "id int64, user_id int64, book_id int64",
    "pc_models": "id int64, name varchar",
    "pc_results": "id int64, model_id int64, period_no int64, pnl_bp int64",
}


def rows_for(seed):
    rng = random.Random(seed)
    R = {"pb_exchanges": [f"({i}, 'X{i}', 'C{i}')" for i in range(1, 3)],
         "pb_symbols": [f"({i}, 'SYM{i}', {1 + i % 2})" for i in range(1, SYMBOLS + 1)],
         "pb_bars": [], "pc_users": [f"({i}, 'member{i:05d}')" for i in range(1, 201)],
         "pc_loans": [f"({i}, {rng.randint(1, 200)}, {rng.randint(1, 2000)})" for i in range(1, 10001)],
         "pc_models": [f"({i}, 'model{i}')" for i in range(1, 9)],
         "pc_results": [f"({i}, {(i - 1) % 8 + 1}, {(i - 1) // 8 + 1}, {rng.randint(-500, 500)})" for i in range(1, 2873)]}
    for s in range(1, SYMBOLS + 1):
        close = 10000
        for n in range(SESSIONS):
            close = max(100, close + rng.randint(-200, 200))
            lo, hi = close - rng.randint(0, 100), close + rng.randint(0, 100)
            R["pb_bars"].append(f"({len(R['pb_bars']) + 1}, {s}, {n}, {close + rng.randint(-50, 50)}, {hi}, {lo}, {close}, {rng.randint(1000, 50000000)})")
    return R


class Srv:
    def __init__(self, label, binary, port, wd, cpu, cores, extra=""):
        self.label, self.port = label, port
        self.data = os.path.join(wd, f"{label}.db")
        conf = os.path.join(wd, f"{label}.conf")
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        if os.path.exists(self.data): os.remove(self.data)
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = {cores}\ndurability = group\n"
                    f"buffer_pool_frames = 65536\n{extra}log_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(os.path.join(wd, f"{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cx_close"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def must(self, sql):
        tag, rows, _b, err = self.conn.query(sql)
        if err is not None: sys.exit(f"{self.label}: {sql[:100]} -> {err}")
        return rows

    def analyze(self, q):
        _f, rows, _t, _n = self.conn.execute("ANALYZE " + q)
        return [" | ".join((x.decode(errors="replace") if x is not None else "NULL") for x in row) for row in rows]

    def load(self, R):
        for t, cols in SCHEMA.items():
            self.must(f"CREATE TABLE {t} ({cols}) BTREE")
        for t, rows in R.items():
            cols = ", ".join(c.split()[0] for c in SCHEMA[t].split(", "))
            for i in range(0, len(rows), 500):
                self.must(f"INSERT INTO {t} ({cols}) VALUES " + ", ".join(rows[i:i + 500]))
        for t, rows in R.items():
            n = self.must(f"SELECT * FROM {t}")
            if n != len(rows): sys.exit(f"{self.label} {t}: {n} != {len(rows)}")

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def run_cell(s, name, ops, seed, rnd):
    mult, make = CELLS[name]
    ops = max(1, int(ops * mult))
    warm = random.Random(f"{seed}-{name}-{rnd}-warm")
    for _ in range(max(1, ops // 10)):
        if s.conn.query(make(warm, None))[3] is not None: sys.exit(f"{s.label} warm {name}")
    rng = random.Random(f"{seed}-{name}-{rnd}")
    stmts = [make(rng, None) for _ in range(ops)]
    lat, counts = [], []
    cpu0 = sched_ns(s.proc.pid); w0 = time.perf_counter()
    for q in stmts:
        t0 = time.perf_counter()
        _t, n, _b, err = s.conn.query(q)
        lat.append(time.perf_counter() - t0)
        if err is not None: sys.exit(f"{s.label} {name}: {err}")
        counts.append(n)
    wall = time.perf_counter() - w0
    cpu = (sched_ns(s.proc.pid) - cpu0) / 1e3 / ops
    lat.sort()
    at = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))] * 1e6
    return {"engine": s.label, "cell": name, "round": rnd, "ops": ops, "rows_total": sum(counts),
            "qps": ops / wall, "mean_us": statistics.fmean(lat) * 1e6, "p0": at(0), "p25": at(.25),
            "p50": at(.5), "p95": at(.95), "p99": at(.99), "server_cpu_us": cpu}


def status(a, line):
    tmp = a.status + ".tmp"
    with open(tmp, "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, a.status)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", choices=["main", "probe", "attr"], default="main")
    p.add_argument("--bin-a"); p.add_argument("--bin-b"); p.add_argument("--bin-x"); p.add_argument("--bin-y"); p.add_argument("--wd")
    p.add_argument("--rounds", type=int, default=8); p.add_argument("--ops", type=int, default=5000)
    p.add_argument("--cells", default=",".join(CELLS)); p.add_argument("--json"); p.add_argument("--status")
    p.add_argument("--plans"); p.add_argument("--server-cpu", default="2"); p.add_argument("--cores", type=int, default=1)
    p.add_argument("--base-port", type=int, default=25601)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    bp = a.base_port
    if a.mode == "main":
        specs = [("A", a.bin_a, ""), ("B", a.bin_b, ""), ("A2", a.bin_a, ""), ("Bp", a.bin_b, "join_order = planned\n")]
    elif a.mode == "attr":
        specs = [("A", a.bin_a, ""), ("X", a.bin_x, ""), ("A2", a.bin_a, ""), ("Y", a.bin_y, ""), ("B", a.bin_b, "")]
    else:
        specs = [("P0", a.bin_b, ""), ("P1", a.bin_b, ""), ("P0b", a.bin_b, ""), ("P2", a.bin_b, "")]
    srvs = [Srv(l, b, bp + i, a.wd, a.server_cpu, a.cores, e) for i, (l, b, e) in enumerate(specs)]
    R = rows_for(1)
    S = {s.label: s for s in srvs}
    plans = {"cores": a.cores}
    for s in srvs:
        status(a, f"load {s.label}"); s.load(R)
    cells = a.cells.split(",")
    if a.mode == "main":
        # the first compile after the load, before any priming, on B
        plans["B_first_compile_after_load"] = {c: S["B"].analyze(CELLS[c][1](random.Random("plan"), None)) for c in CAPTURE_CELLS if c in cells}
        plans["B_prime"] = {}
        for c in CAPTURE_CELLS:
            if c not in cells: continue
            q = CELLS[c][1](random.Random("plan"), None)
            t0 = time.time(); n = 0
            while True:
                S["B"].conn.query(q); n += 1
                txt = S["B"].analyze(q)
                if any("order=captured" in l for l in txt): break
                if time.time() - t0 > 60: sys.exit(f"B never captured {c}: {txt}")
                time.sleep(0.2)
            plans["B_prime"][c] = {"statements": n, "seconds": round(time.time() - t0, 2), "analyze": txt}
    if a.mode == "probe":
        jn = ("SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
              "ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16")
        S["P1"].must(f"CREATE PLAN px_join FOR {jn} VARIANT v ELSE ORDER (u, l)")
        S["P2"].must("CREATE PLAN px_one FOR SELECT * FROM pc_users WHERE member_code = 'member00005' "
                     "VARIANT v ELSE ORDER (pc_users)")
        plans["show_plans"] = {k: [[(x.decode(errors='replace') if x is not None else None) for x in r]
                                   for r in S[k].conn.execute("SHOW PLANS")[1]] for k in ("P0", "P1", "P2")}
    for c in ("point-join-3rel", "join-no-literal", "join-no-literal-rev", "model-join", "pk-point-read", "point-join-2rel"):
        if c not in cells: continue
        q = CELLS[c][1](random.Random("plan"), None)
        plans[c] = {"stmt": q}
        sets = {}
        for s in srvs:
            plans[c][s.label] = s.analyze(q)
            _f, rows, _t, _n = s.conn.execute(q)
            sets[s.label] = sorted(tuple(r) for r in rows)
        plans[c]["rows"] = len(sets[srvs[0].label])
        plans[c]["multiset_equal_all"] = all(sets[l] == sets[srvs[0].label] for l in sets)
    json.dump(plans, open(a.plans, "w"), indent=1)
    out = []
    t0 = time.time(); n = len(srvs)
    for rnd in range(1, a.rounds + 1):
        for c in cells:
            order = srvs[(rnd - 1) % n:] + srvs[:(rnd - 1) % n]
            if rnd % 2 == 0: order = order[::-1]
            for s in order:
                status(a, f"round {rnd} {c} {s.label} elapsed {time.time() - t0:.0f}s")
                r = run_cell(s, c, a.ops, 1, rnd); r["host"] = host_state()
                out.append(r)
        json.dump(out, open(a.json, "w"))
    final = {}
    for c in cells:
        q = CELLS[c][1](random.Random("plan"), None)
        final[c] = {s.label: s.analyze(q) for s in srvs}
    json.dump(final, open(a.plans + ".final", "w"), indent=1)
    for s in srvs: s.stop()
    status(a, "DONE")


if __name__ == "__main__":
    main()
