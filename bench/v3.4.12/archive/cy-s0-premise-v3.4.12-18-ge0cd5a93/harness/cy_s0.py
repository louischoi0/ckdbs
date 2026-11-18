#!/usr/bin/env python3
"""CY-S0 driver (derived from CC-S0's cc_s0_run.py/cc_s0_load.py). Modes: load | run.
Servers: F fresh planned server (blind first compile), P planned (stats published after warm-up),
W written, W2 written control (same spelling as W:<first order>)."""
import argparse, json, os, random, shutil, subprocess, sys, time
TOOLS = "/home/louis/ckdbs/.claude/worktrees/cx-plan-change-option/tools"
sys.path.insert(0, TOOLS)
import kwp
from bn_census import sched_ns
SIZES = (200, 1000, 10000)
SKIP = {"cross-join-10000:b-t-s", "cross-join-10000:s-t-b"}   # quadratic orders, cut for the budget
NORG = {200: 20, 1000: 50, 10000: 300}


def must(c, sql):
    t, n, b, e = c.query(sql)
    if e is not None: sys.exit(f"{sql[:100]} -> {e}")


def ins(c, table, cols, rows):
    for i in range(0, len(rows), 500):
        must(c, f"INSERT INTO {table} ({cols}) VALUES " + ", ".join(rows[i:i + 500]))


def load(c):
    for n in SIZES:
        rng = random.Random(n); t = f"{n}"
        must(c, f"CREATE TABLE models_{t} (id int64, name varchar, family int32, lookback int32, top_k int32, param_bp int64) BTREE")
        must(c, f"CREATE TABLE model_results_{t} (id int64, model_id int64, period_no int32, session_no int32, pnl_bp int64, equity_bp int64, positions int32, trades int32) BTREE")
        ins(c, f"models_{t}", "id, name, family, lookback, top_k, param_bp", [f"({i}, 'model{i}', {i % 3}, {5 * i}, 3, {rng.randint(0, 900)})" for i in range(1, 9)])
        ins(c, f"model_results_{t}", "id, model_id, period_no, session_no, pnl_bp, equity_bp, positions, trades",
            [f"({i}, {(i - 1) % 8 + 1}, {(i - 1) // 8 + 1}, {((i - 1) // 8 + 1) * 21}, {rng.randint(-500, 500)}, {10000 + i}, 3, {rng.randint(0, 6)})" for i in range(1, n + 1)])
        ns = n // 8
        must(c, f"CREATE TABLE symbols_{t} (id int64, ticker varchar, exchange_id int64, sector int32, listed_session int32) BTREE")
        must(c, f"CREATE TABLE daily_bars_{t} (id int64, symbol_id int64, session_id int64, session_no int32, open int64, high int64, low int64, close int64, volume int64) BTREE")
        must(c, f"CREATE TABLE daily_stats_{t} (id int64, bar_id int64, symbol_id int64, session_no int32, ret_bp int64, mom5_bp int64, mom20_bp int64, mom60_bp int64, mom120_bp int64, vol10_bp int64, vol20_bp int64, vol60_bp int64) BTREE")
        ins(c, f"symbols_{t}", "id, ticker, exchange_id, sector, listed_session", [f"({s}, 'SYM{s}', {1 + s % 2}, {s % 5}, 0)" for s in range(1, 9)])
        bars, stats, k = [], [], 0
        for sess in range(ns):
            for s in range(1, 9):
                k += 1; cl = 10000 + rng.randint(-300, 300)
                bars.append(f"({k}, {s}, {sess + 1}, {sess}, {cl}, {cl + 5}, {cl - 5}, {cl}, {rng.randint(1000, 90000)})")
                stats.append(f"({k}, {k}, {s}, {sess}, " + ", ".join(str(rng.randint(-300, 300)) for _ in range(8)) + ")")
        ins(c, f"daily_bars_{t}", "id, symbol_id, session_id, session_no, open, high, low, close, volume", bars)
        ins(c, f"daily_stats_{t}", "id, bar_id, symbol_id, session_no, ret_bp, mom5_bp, mom20_bp, mom60_bp, mom120_bp, vol10_bp, vol20_bp, vol60_bp", stats)
        must(c, f"CREATE TABLE cargos_{t} (id int64, org_id int64, op_id int64, cbm int64, status int32) BTREE")
        must(c, f"CREATE TABLE freights_{t} (id int64, cargo_id int64, cbm int64, price_per_cbm int64, booked_day int32, status int32) BTREE")
        ins(c, f"cargos_{t}", "id, org_id, op_id, cbm, status", [f"({i}, {(i - 1) % NORG[n] + 1}, {i % 50 + 1}, {rng.randint(1, 40)}, 1)" for i in range(1, n + 1)])
        ins(c, f"freights_{t}", "id, cargo_id, cbm, price_per_cbm, booked_day, status", [f"({i}, {i}, {rng.randint(1, 40)}, {rng.randint(10, 90)}, {i % 365}, 1)" for i in range(1, int(n * 0.9) + 1)])
        must(c, f"CREATE TABLE users_{t} (id int64, member_code varchar, status int32) BTREE")
        must(c, f"CREATE TABLE loans_{t} (id int64, user_id int64, book_id int64, status int32) BTREE")
        ins(c, f"users_{t}", "id, member_code, status", [f"({i}, 'M{i}', 1)" for i in range(1, n + 1)])
        ins(c, f"loans_{t}", "id, user_id, book_id, status", [f"({i}, {(i - 1) % n + 1}, {rng.randint(1, 5000)}, 1)" for i in range(1, 5 * n + 1)])


def cells():
    C = {}
    for n in SIZES:
        t = str(n); ns = n // 8; norg = NORG[n]
        sel = "r.model_id, r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name, m.family"
        C[f"compare-all-{n}"] = {
            "r-m": lambda r, t=t, sel=sel: f"SELECT {sel} FROM model_results_{t} AS r JOIN models_{t} AS m ON r.model_id = m.id",
            "m-r": lambda r, t=t, sel=sel: f"SELECT {sel} FROM models_{t} AS m JOIN model_results_{t} AS r ON r.model_id = m.id"}
        s1 = "r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name"
        C[f"compare-one-{n}"] = {
            "r-m": lambda r, t=t, s1=s1: f"SELECT {s1} FROM model_results_{t} AS r JOIN models_{t} AS m ON r.model_id = m.id WHERE r.model_id = {r.randint(1, 8)}",
            "m-r": lambda r, t=t, s1=s1: f"SELECT {s1} FROM models_{t} AS m JOIN model_results_{t} AS r ON r.model_id = m.id WHERE r.model_id = {r.randint(1, 8)}"}
        C[f"customer-statement-{n}"] = {
            "f-c": lambda r, t=t, o=norg: f"SELECT c.org_id, SUM(f.cbm) FROM freights_{t} AS f JOIN cargos_{t} AS c ON f.cargo_id = c.id WHERE c.org_id = {r.randint(1, o)} GROUP BY c.org_id",
            "c-f": lambda r, t=t, o=norg: f"SELECT c.org_id, SUM(f.cbm) FROM cargos_{t} AS c JOIN freights_{t} AS f ON f.cargo_id = c.id WHERE c.org_id = {r.randint(1, o)} GROUP BY c.org_id"}
        proj = "t.symbol_id, t.ret_bp, t.mom5_bp, t.mom20_bp, t.mom60_bp, t.mom120_bp, t.vol10_bp, t.vol20_bp, t.vol60_bp, b.close, s.ticker"

        def x(order, t=t, ns=ns, proj=proj):
            fr = {"t-b-s": f"daily_stats_{t} AS t JOIN daily_bars_{t} AS b ON t.bar_id = b.id JOIN symbols_{t} AS s ON t.symbol_id = s.id",
                  "t-s-b": f"daily_stats_{t} AS t JOIN symbols_{t} AS s ON t.symbol_id = s.id JOIN daily_bars_{t} AS b ON t.bar_id = b.id",
                  "b-t-s": f"daily_bars_{t} AS b JOIN daily_stats_{t} AS t ON t.bar_id = b.id JOIN symbols_{t} AS s ON t.symbol_id = s.id",
                  "s-t-b": f"symbols_{t} AS s JOIN daily_stats_{t} AS t ON t.symbol_id = s.id JOIN daily_bars_{t} AS b ON t.bar_id = b.id"}[order]
            return lambda r: f"SELECT {proj} FROM {fr} WHERE t.session_no = {r.randint(0, ns - 1)}"
        C[f"cross-join-{n}"] = {o: x(o) for o in ("t-b-s", "t-s-b", "b-t-s", "s-t-b")}
        C[f"join-no-literal-{n}"] = {
            "u-l": lambda r, t=t: f"SELECT l.book_id, u.member_code FROM users_{t} AS u JOIN loans_{t} AS l ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16",
            "l-u": lambda r, t=t: f"SELECT l.book_id, u.member_code FROM loans_{t} AS l JOIN users_{t} AS u ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16"}
    return C


def selectivity_counts():
    out = []
    for n in SIZES:
        t = str(n)
        out += [(f"compare-one-{n}", "r.model_id = 3", f"SELECT COUNT(*) FROM model_results_{t} WHERE model_id = 3", f"SELECT COUNT(*) FROM model_results_{t}"),
                (f"customer-statement-{n}", "c.org_id = 3", f"SELECT COUNT(*) FROM cargos_{t} WHERE org_id = 3", f"SELECT COUNT(*) FROM cargos_{t}"),
                (f"cross-join-{n}", "t.session_no = 3", f"SELECT COUNT(*) FROM daily_stats_{t} WHERE session_no = 3", f"SELECT COUNT(*) FROM daily_stats_{t}"),
                (f"join-no-literal-{n}", "u.id >= 1", f"SELECT COUNT(*) FROM users_{t} WHERE id >= 1", f"SELECT COUNT(*) FROM users_{t}"),
                (f"join-no-literal-{n}", "u.id <= 16", f"SELECT COUNT(*) FROM users_{t} WHERE id <= 16", f"SELECT COUNT(*) FROM users_{t}"),
                (f"join-no-literal-{n}", "u.id BETWEEN 1 AND 16 (joint)", f"SELECT COUNT(*) FROM users_{t} WHERE id BETWEEN 1 AND 16", f"SELECT COUNT(*) FROM users_{t}")]
    return out


def status(path, line):
    with open(path + ".tmp", "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(path + ".tmp", path)


class Srv:
    def __init__(self, label, binary, port, wd, pristine, cpu, jo):
        self.label = label; self.data = os.path.join(wd, f"{label}.db")
        for s in ("", ".wal"):
            p = self.data + s
            if os.path.isdir(p): shutil.rmtree(p)
            elif os.path.exists(p): os.remove(p)
        shutil.copy(pristine, self.data)
        if os.path.isdir(pristine + ".wal"): shutil.copytree(pristine + ".wal", self.data + ".wal")
        conf = os.path.join(wd, f"{label}.conf")
        open(conf, "w").write(f"data_file = {self.data}\nport = {port}\ncores = 1\ndurability = group\nbuffer_pool_frames = 65536\njoin_order = {jo}\nlog_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        import socket
        _s = socket.socket()
        if _s.connect_ex(("127.0.0.1", port)) == 0: sys.exit(f"port {port} already served by a stale server")
        _s.close()
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf], stdout=open(os.path.join(wd, f"{label}.stderr"), "w"), stderr=subprocess.STDOUT)
        t = time.time() + 60
        while True:
            try: self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cy_s0"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def analyze(self, sql):
        _f, rows, _t, _n = self.conn.execute("ANALYZE " + sql)
        return [" | ".join(x.decode(errors="replace") if x is not None else "NULL" for x in row) for row in rows]

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(60)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def block(srv, gen, ops, seed):
    w = random.Random(f"{seed}-w")
    for _ in range(max(2, min(ops // 10, 20))): srv.conn.query(gen(w))
    rng = random.Random(seed); stm = [gen(rng) for _ in range(ops)]
    lat = []; cpu0 = sched_ns(srv.proc.pid); rows = 0
    tstop = time.perf_counter() + 4.0
    for q in stm:
        if len(lat) >= 3 and time.perf_counter() > tstop: break
        t0 = time.perf_counter(); _t, n, _b, e = srv.conn.query(q); lat.append((time.perf_counter() - t0) * 1e6)
        if e is not None: sys.exit(f"{q[:90]} -> {e}")
        rows += n
    return {"ops": len(lat), "lat_us": lat, "cpu_us": (sched_ns(srv.proc.pid) - cpu0) / 1e3 / len(lat), "rows": rows}   # NOTE: run 3 ran with "/ ops" (planned ops); an.py rescales to completed ops


def host():
    la = open("/proc/loadavg").read().split()[0]
    r = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True)
    return {"load1": la, "foreign": len([l for l in r.stdout.splitlines() if "pgrep" not in l])}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode"); p.add_argument("--bin"); p.add_argument("--wd"); p.add_argument("--out"); p.add_argument("--status")
    p.add_argument("--rounds", type=int, default=3); p.add_argument("--target-s", type=float, default=0.5)
    p.add_argument("--sizes", default="200,1000,10000"); p.add_argument("--cpu", type=int, default=2)
    a = p.parse_args(); os.makedirs(a.wd, exist_ok=True); pristine = os.path.join(a.wd, "pristine.db")
    if a.mode == "load":
        for s in ("", ".wal"):
            q = pristine + s
            if os.path.isdir(q): shutil.rmtree(q)
            elif os.path.exists(q): os.remove(q)
        conf = os.path.join(a.wd, "L.conf")
        open(conf, "w").write(f"data_file = {pristine}\nport = 25600\ncores = 1\ndurability = group\nbuffer_pool_frames = 65536\nlog_file = L.log\nlog_dir = {a.wd}\nlog_level = warn\n")
        pr = subprocess.Popen([a.bin, "--config", conf], stdout=open(os.path.join(a.wd, "L.stderr"), "w"), stderr=subprocess.STDOUT)
        for _ in range(100):
            try: c = kwp.Connection("127.0.0.1", 25600, timeout=600, client_name="cy_s0_load"); break
            except OSError: time.sleep(0.2)
        status(a.status, "loading"); load(c); c.close(); pr.terminate(); pr.wait(60); status(a.status, "LOAD DONE"); return
    C = cells(); keep = tuple(int(x) for x in a.sizes.split(","))
    names = [n for n in C if int(n.rsplit("-", 1)[1]) in keep]
    res = {"blind": {}, "informed": {}, "sel": [], "blocks": []}
    F = Srv("F", a.bin, 25611, a.wd, pristine, a.cpu, "planned")
    for n in names:
        status(a.status, f"blind {n}")
        res["blind"][n] = F.analyze(next(iter(C[n].values()))(random.Random("plan")))
    F.stop()
    P = Srv("P", a.bin, 25601, a.wd, pristine, a.cpu, "planned")
    W = Srv("W", a.bin, 25602, a.wd, pristine, a.cpu, "written")
    W2 = Srv("W2", a.bin, 25603, a.wd, pristine, a.cpu, "written")
    for lab, q, wq, woq in selectivity_counts():
        if int(lab.rsplit("-", 1)[1]) in keep:
            res["sel"].append({"cell": lab, "conjunct": q, "with": int.from_bytes(W.conn.execute(wq)[1][0][0], "little"), "without": int.from_bytes(W.conn.execute(woq)[1][0][0], "little")})
    for n in names:
        gen = next(iter(C[n].values())); rng = random.Random("w"); prev = None
        for it in range(6):
            status(a.status, f"warm {n} it{it}")
            for _ in range(40): P.conn.query(gen(rng))
            time.sleep(1.2); an = P.analyze(gen(random.Random("plan")))
            if an == prev and any("gen=" in l and "gen=0" not in l for l in an): break
            prev = an
        res["informed"][n] = an
    json.dump(res, open(a.out + ".plans.json", "w"), indent=1)
    ops_of = {}
    for n in names:
        gen = next(iter(C[n].values())); rng = random.Random("cal"); t0 = time.perf_counter(); k = 0
        while time.perf_counter() - t0 < 0.25 or k < 3: W2.conn.query(gen(rng)); k += 1
        ops_of[n] = int(max(30, min(2000, a.target_s / ((time.perf_counter() - t0) / k))))
    t00 = time.time()
    for rnd in range(1, a.rounds + 1):
        for ci, n in enumerate(names):
            first = next(iter(C[n]))
            arms = [("P", first)] + [("W", o) for o in C[n] if f"{n}:{o}" not in SKIP] + [("W2", first)]
            k = (rnd + ci) % len(arms); arms = arms[k:] + arms[:k]
            if (rnd + ci) % 2: arms = arms[::-1]
            for srv, o in arms:
                status(a.status, f"round {rnd}/{a.rounds} {n} {srv}:{o} elapsed {time.time() - t00:.0f}s")
                h = host(); r = block({"P": P, "W": W, "W2": W2}[srv], C[n][o], ops_of[n], f"{n}-{rnd}")
                r.update({"round": rnd, "cell": n, "arm": f"{srv}:{o}", "host": h}); res["blocks"].append(r)
                json.dump(res, open(a.out + ".tmp", "w")); os.replace(a.out + ".tmp", a.out)
    for s in (P, W, W2): s.stop()
    status(a.status, "DONE")


main()
