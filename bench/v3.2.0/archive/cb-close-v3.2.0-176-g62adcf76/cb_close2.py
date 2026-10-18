#!/usr/bin/env python3
"""CB close measurement: A (opening commit) vs B (close) vs A2 (a second server
of A's binary, the noise-floor control), interleaved in alternating rounds."""
import argparse, json, os, random, shutil, statistics, subprocess, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/cb-join-planner/tools")
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
JNL = ("SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
       "ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16")
JNL_REV = ("SELECT l.book_id, u.member_code FROM pc_loans AS l JOIN pc_users AS u "
           "ON u.id = l.user_id WHERE u.id BETWEEN 1 AND 16")
MJ = ("SELECT r.period_no, r.pnl_bp, m.name FROM pc_results AS r JOIN pc_models AS m "
      "ON r.model_id = m.id WHERE r.model_id = 3")

# name: (ops multiplier, statement generator, is_fixed)
CELLS = {
    "pk-point-read": (1.0, bars_stmt),
    "point-join-3rel": (1.0, pj_stmt),
    "join-no-literal": (1.0, lambda r, a: JNL),
    "join-no-literal-rev": (0.5, lambda r, a: JNL_REV),
    "model-join": (1.0, lambda r, a: MJ),
}

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
    def __init__(self, label, binary, port, wd, cpu, extra=""):
        self.label, self.port = label, port
        self.data = os.path.join(wd, f"{label}.db")
        conf = os.path.join(wd, f"{label}.conf")
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        if os.path.exists(self.data): os.remove(self.data)
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = 1\ndurability = group\n"
                    f"buffer_pool_frames = 65536\n{extra}log_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(os.path.join(wd, f"{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cb_close2"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def must(self, sql):
        tag, rows, _b, err = self.conn.query(sql)
        if err is not None: sys.exit(f"{self.label}: {sql[:100]} -> {err}")
        return rows

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
            "counts_hash": hash(tuple(counts)) if False else sum(counts), "qps": ops / wall,
            "mean_us": statistics.fmean(lat) * 1e6, "p0": at(0), "p25": at(.25), "p50": at(.5),
            "p95": at(.95), "p99": at(.99), "server_cpu_us": cpu, "counts": counts[:50]}


def status(a, line):
    tmp = a.status + ".tmp"
    with open(tmp, "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, a.status)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin-a"); p.add_argument("--bin-b"); p.add_argument("--bin-c"); p.add_argument("--wd")
    p.add_argument("--rounds", type=int, default=4); p.add_argument("--ops", type=int, default=2500)
    p.add_argument("--cells", default=",".join(CELLS)); p.add_argument("--json"); p.add_argument("--status")
    p.add_argument("--plans"); p.add_argument("--server-cpu", type=int, default=2)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    srvs = [Srv("A", a.bin_a, 25501, a.wd, a.server_cpu), Srv("B", a.bin_b, 25502, a.wd, a.server_cpu),
            Srv("A2", a.bin_a, 25503, a.wd, a.server_cpu),
            Srv("C", a.bin_c, 25504, a.wd, a.server_cpu)]
    R = rows_for(1)
    for s in srvs:
        status(a, f"load {s.label}"); s.load(R)
    cells = a.cells.split(",")
    # plans + result equality
    plans = {}
    for c in ("point-join-3rel", "join-no-literal", "join-no-literal-rev", "model-join", "pk-point-read"):
        if c not in cells: continue
        q = CELLS[c][1](random.Random("plan"), None)
        plans[c] = {"stmt": q}
        sets = {}
        for s in srvs:
            _f2, r2, _t2, _n2 = s.conn.execute("ANALYZE " + q)
            plans[c][s.label] = [" | ".join((x.decode(errors="replace") if x is not None else "NULL") for x in row) for row in r2]
            _f, rows, _t, _n = s.conn.execute(q)
            sets[s.label] = sorted(tuple(r) for r in rows)
        plans[c]["rows"] = len(sets["A"])
        plans[c]["multiset_equal_A_B"] = sets["A"] == sets["B"]
        plans[c]["multiset_equal_A_A2"] = sets["A"] == sets["A2"]
        plans[c]["multiset_equal_A_C"] = sets["A"] == sets["C"]
    json.dump(plans, open(a.plans, "w"), indent=1)
    out = []
    t0 = time.time()
    for rnd in range(1, a.rounds + 1):
        for c in cells:
            order = srvs[(rnd - 1) % 4:] + srvs[:(rnd - 1) % 4]
            if rnd % 2 == 0: order = order[::-1]
            for s in order:
                status(a, f"round {rnd} {c} {s.label} elapsed {time.time() - t0:.0f}s")
                r = run_cell(s, c, a.ops, 1, rnd); r["host"] = host_state()
                out.append(r)
        json.dump(out, open(a.json, "w"))
    for s in srvs: s.stop()
    status(a, "DONE")


if __name__ == "__main__":
    main()
