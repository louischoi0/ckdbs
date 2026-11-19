#!/usr/bin/env python3
"""CY close harness (derived from CX's cx_close.py).
  mode ab : servers A (CY's base on main), B (CY close, empty model), A2 (control, same
            binary as A), Bp (B after GATHER STATISTICS ON every relation); interleaved rounds.
  mode q14: one server of B, a background-priority admin session beside it; conditions
            OFF (idle session), ON (that session loops GATHER on the largest relation),
            OFF2 (control: second OFF); interleaved rounds."""
import argparse, json, os, random, shutil, statistics, subprocess, sys, time

TOOLS = sys.argv[sys.argv.index("--tools") + 1]
sys.path.insert(0, TOOLS)
import kwp
from bn_census import sched_ns
from bb_overhead_benchmark import host_state

SESSIONS, SYMBOLS = 7560, 8
BARS = SESSIONS * SYMBOLS
NOLTP = 2000


def bars_stmt(r): return f"SELECT * FROM pb_bars WHERE id = {r.randint(1, BARS)}"
def pj2_stmt(r):
    return ("SELECT b.id, b.close, s.ticker FROM pb_bars AS b "
            f"JOIN pb_symbols AS s ON b.symbol_id = s.id WHERE b.id = {r.randint(1, BARS)}")
def pj_stmt(r):
    return ("SELECT b.id, b.close, s.ticker, e.code FROM pb_bars AS b "
            "JOIN pb_symbols AS s ON b.symbol_id = s.id "
            "JOIN pb_exchanges AS e ON s.exchange_id = e.id "
            f"WHERE b.id = {r.randint(1, BARS)}")
def ins_stmt(r): return f"INSERT INTO pc_oltp (v) VALUES ({r.randint(1, 1000000)})"
def upd_stmt(r): return f"UPDATE pc_oltp SET v = {r.randint(1, 1000000)} WHERE id = {r.randint(1, NOLTP)}"
JNL = ("SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l "
       "ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16")
MJ = ("SELECT r.period_no, r.pnl_bp, m.name FROM pc_results AS r JOIN pc_models AS m "
      "ON r.model_id = m.id WHERE r.model_id = 3")

CELLS = {
    "pk-point-read": (1.0, bars_stmt),
    "insert-1row": (1.0, ins_stmt),
    "update-1row": (1.0, upd_stmt),
    "point-join-3rel": (1.0, pj_stmt),
    "point-join-2rel": (1.0, pj2_stmt),
    "join-no-literal": (1.0, lambda r: JNL),
    "model-join": (1.0, lambda r: MJ),
}
SCHEMA = {
    "pb_exchanges": "id int64, code varchar, country varchar",
    "pb_symbols": "id int64, ticker varchar, exchange_id int64",
    "pb_bars": "id int64, symbol_id int64, session_no int32, open int64, high int64, low int64, close int64, volume int64",
    "pc_users": "id int64, member_code varchar",
    "pc_loans": "id int64, user_id int64, book_id int64",
    "pc_models": "id int64, name varchar",
    "pc_results": "id int64, model_id int64, period_no int64, pnl_bp int64",
    "pc_oltp": "id int64, v int64",
}


def rows_for(seed):
    rng = random.Random(seed)
    R = {"pb_exchanges": [f"({i}, 'X{i}', 'C{i}')" for i in range(1, 3)],
         "pb_symbols": [f"({i}, 'SYM{i}', {1 + i % 2})" for i in range(1, SYMBOLS + 1)],
         "pb_bars": [], "pc_users": [f"({i}, 'member{i:05d}')" for i in range(1, 201)],
         "pc_loans": [f"({i}, {rng.randint(1, 200)}, {rng.randint(1, 2000)})" for i in range(1, 10001)],
         "pc_models": [f"({i}, 'model{i}')" for i in range(1, 9)],
         "pc_results": [f"({i}, {(i - 1) % 8 + 1}, {(i - 1) // 8 + 1}, {rng.randint(-500, 500)})" for i in range(1, 2873)],
         "pc_oltp": [f"({i}, {rng.randint(1, 1000000)})" for i in range(1, NOLTP + 1)]}
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
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cy_close"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def must(self, sql):
        tag, rows, _b, err = self.conn.query(sql)
        if err is not None: sys.exit(f"{self.label}: {sql[:100]} -> {err}")
        return rows

    def text(self, sql):
        _f, rows, _t, _n = self.conn.execute(sql)
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


def run_cell(s, name, ops, seed, rnd, tag):
    mult, make = CELLS[name]
    ops = max(1, int(ops * mult))
    warm = random.Random(f"{seed}-{name}-{rnd}-warm")
    for _ in range(max(1, ops // 10)):
        if s.conn.query(make(warm))[3] is not None: sys.exit(f"{s.label} warm {name}")
    rng = random.Random(f"{seed}-{name}-{rnd}")
    stmts = [make(rng) for _ in range(ops)]
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
    return {"engine": tag, "cell": name, "round": rnd, "ops": ops, "rows_total": sum(counts),
            "qps": ops / wall, "mean_us": statistics.fmean(lat) * 1e6, "p0": at(0), "p25": at(.25),
            "p50": at(.5), "p75": at(.75), "p90": at(.9), "p99": at(.99), "server_cpu_us": cpu}


def gen(s):
    for l in s.text("SELECT * FROM sys.meta"):
        if "plan_stats_generation" in l: return int(l.split(" | ")[-1])
    return -1


def status(a, line):
    tmp = a.status + ".tmp"
    with open(tmp, "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, a.status)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", choices=["ab", "q14"], required=True)
    p.add_argument("--tools"); p.add_argument("--bin-a"); p.add_argument("--bin-b"); p.add_argument("--wd")
    p.add_argument("--rounds", type=int, default=8); p.add_argument("--ops", type=int, default=4000)
    p.add_argument("--cells", default=",".join(CELLS)); p.add_argument("--json"); p.add_argument("--status")
    p.add_argument("--plans"); p.add_argument("--server-cpu", default="2"); p.add_argument("--cores", type=int, default=1)
    p.add_argument("--base-port", type=int, default=25701); p.add_argument("--gatherer"); p.add_argument("--gather-cpu", default="5")
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    cells = a.cells.split(",")
    R = rows_for(1)
    plans = {"cores": a.cores}
    out = []
    if a.mode == "ab":
        specs = [("A", a.bin_a), ("B", a.bin_b), ("A2", a.bin_a), ("Bp", a.bin_b)]
        srvs = [Srv(l, b, a.base_port + i, a.wd, a.server_cpu, a.cores) for i, (l, b) in enumerate(specs)]
        S = {s.label: s for s in srvs}
        for s in srvs: status(a, f"load {s.label}"); s.load(R)
        plans["bp_gathers"] = {t: [x.decode() if isinstance(x, bytes) else str(x) for x in S["Bp"].text(f"GATHER STATISTICS ON {t}")] for t in SCHEMA}
        plans["bp_sys_statistics"] = S["Bp"].text("SELECT * FROM sys.statistics")
        plans["b_sys_statistics"] = S["B"].text("SELECT * FROM sys.statistics")
        for c in cells:
            if c.startswith("insert") or c.startswith("update"): continue
            q = CELLS[c][1](random.Random("plan"))
            plans[c] = {"stmt": q}
            sets = {}
            for s in srvs:
                plans[c][s.label] = s.text("ANALYZE " + q)
                _f, rows, _t, _n = s.conn.execute(q)
                sets[s.label] = sorted(tuple(r) for r in rows)
            plans[c]["multiset_equal_all"] = all(sets[l] == sets["A"] for l in sets)
        json.dump(plans, open(a.plans, "w"), indent=1)
        t0 = time.time(); n = len(srvs)
        for rnd in range(1, a.rounds + 1):
            for c in cells:
                order = srvs[(rnd - 1) % n:] + srvs[:(rnd - 1) % n]
                if rnd % 2 == 0: order = order[::-1]
                for s in order:
                    status(a, f"round {rnd} {c} {s.label} elapsed {time.time() - t0:.0f}s")
                    r = run_cell(s, c, a.ops, 1, rnd, s.label); r["host"] = host_state(); out.append(r)
            json.dump(out, open(a.json, "w"))
        for s in srvs: s.stop()
    else:
        s = Srv("B", a.bin_b, a.base_port, a.wd, a.server_cpu, a.cores)
        s.load(R)
        g = subprocess.Popen(["taskset", "-c", a.gather_cpu, sys.executable, a.gatherer, str(a.base_port), "pb_bars", TOOLS],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if g.stdout.readline().strip() != "ready": sys.exit("gatherer did not start")
        plans["sys_meta"] = s.text("SELECT * FROM sys.meta")
        t0 = time.perf_counter(); plans["one_gather"] = s.text("GATHER STATISTICS ON pb_bars")
        plans["one_gather_s"] = time.perf_counter() - t0
        plans["sys_statistics"] = s.text("SELECT * FROM sys.statistics")
        json.dump(plans, open(a.plans, "w"), indent=1)
        conds = ["OFF", "ON", "OFF2"]
        t0 = time.time(); n = len(conds)
        for rnd in range(1, a.rounds + 1):
            for c in cells:
                order = conds[(rnd - 1) % n:] + conds[:(rnd - 1) % n]
                if rnd % 2 == 0: order = order[::-1]
                for cond in order:
                    status(a, f"round {rnd} {c} {cond} elapsed {time.time() - t0:.0f}s")
                    if cond == "ON":
                        gen0 = gen(s); g.stdin.write("on\n"); g.stdin.flush()
                    r = run_cell(s, c, a.ops, 1, rnd, cond)
                    if cond == "ON":
                        g.stdin.write("off\n"); g.stdin.flush()
                        r["gathers"] = json.loads(g.stdout.readline())
                        r["gen_delta"] = gen(s) - gen0; r["wall_s"] = r["ops"] / r["qps"]
                    r["host"] = host_state(); out.append(r)
            json.dump(out, open(a.json, "w"))
        g.stdin.write("quit\n"); g.stdin.flush(); g.wait(30)
        s.stop()
    status(a, "DONE")


if __name__ == "__main__":
    main()
