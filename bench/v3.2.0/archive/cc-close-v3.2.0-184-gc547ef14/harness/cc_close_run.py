#!/usr/bin/env python3
"""CC-close A/B harness. Servers: A (binary at 65dc45c5), A2 (same binary, noise control), B (closing commit).
Three servers started from one pristine data file, each pinned to one CPU, one active at a time.
Resumable: every finished block is stored in out/<phase>.json and skipped on a restart."""
import argparse, gzip, json, os, random, re, shutil, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/tools"
sys.path.insert(0, TOOLS)
import kwp
from bn_census import sched_ns

R = "/home/cdkbs/bench-runs/cc-close"
OUT = os.environ.get("CC_OUT", R + "/out")
STATUS = OUT + "/run.status"
BIN = {"A": R + "/kds_server_A", "A2": R + "/kds_server_A", "B": R + "/kds_server_B"}
PORT = {"A": 25701, "A2": 25702, "B": 25703}
PRISTINE = "/home/cdkbs/bench-runs/cc-s0/load/pristine.db"
CPU = 2
ROUNDS = int(os.environ.get("ROUNDS", "5"))
TARGET_S = 1.0
OPS_CAP = 10000


def status(line):
    tmp = STATUS + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, STATUS)


def host_state():
    try:
        la = open("/proc/loadavg").read().split()[0]
    except OSError:
        la = "?"
    r = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True)
    foreign = [l for l in r.stdout.splitlines() if "pgrep" not in l and "cc_close_run" not in l]
    return {"loadavg1": la, "foreign": len(foreign)}


# ---- statements (S0's generators: the scenario drivers' spelling A) ---------
def c1_stmt(tag):
    return ("SELECT r.model_id, r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name, m.family "
            f"FROM model_results_{tag} AS r JOIN models_{tag} AS m ON r.model_id = m.id")


def c2_stmt(suffix, org):
    return (f"SELECT c.org_id, SUM(f.cbm) FROM freights_{suffix} AS f "
            f"JOIN cargos_{suffix} AS c ON f.cargo_id = c.id WHERE c.org_id = {org} GROUP BY c.org_id")


def c3_stmt(tag, sess):
    return ("SELECT t.symbol_id, t.ret_bp, t.mom5_bp, t.mom20_bp, t.mom60_bp, t.mom120_bp, "
            "t.vol10_bp, t.vol20_bp, t.vol60_bp, b.close, s.ticker "
            f"FROM daily_stats_{tag} AS t JOIN daily_bars_{tag} AS b ON t.bar_id = b.id "
            f"JOIN symbols_{tag} AS s ON t.symbol_id = s.id WHERE t.session_no = {sess}")


NORG = {"n200": 20, "n1k": 50, "ca": 300, "df": 2000}
C3NS = {"x200": 25, "x1k": 125, "x10k": 1250, "x60k": 7560}

# overhead tables (created on every server identically)
OV_ROWS = 10000


def ov_point(r): return f"SELECT * FROM daily_bars_x10k WHERE id = {r.randint(1, 10000)}"


def ov_join3(r):
    return ("SELECT b.id, b.close, s.ticker, e.name FROM daily_bars_x10k AS b "
            "JOIN symbols_x10k AS s ON b.symbol_id = s.id JOIN ov_ex AS e ON s.exchange_id = e.id "
            f"WHERE b.id = {r.randint(1, 10000)}")


def ov_insert(r): return f"INSERT INTO ov_ins (a, b, c) VALUES ({r.randint(0, 10**9)}, {r.randint(0, 10**9)}, {r.randint(0, 1000)})"


def ov_update(r): return f"UPDATE ov_upd SET a = a + 1 WHERE id = {r.randint(1, OV_ROWS)}"


OVERHEAD = {"ov-point-read": ov_point, "ov-join3-tuplekey": ov_join3, "ov-insert": ov_insert, "ov-update": ov_update}


def gain_cells():
    c = {}
    for tag in ("r200", "r1k", "r2872", "r10k"):
        c[f"c1-compare-all-{tag}"] = (lambda r, tag=tag: c1_stmt(tag))
    for suf, label in (("n200", "n200"), ("n1k", "n1k"), ("ca", "ca10k"), ("df", "df200k")):
        c[f"c2-customer-statement-{label}"] = (lambda r, suf=suf: c2_stmt(suf, r.randint(1, NORG[suf])))
    return c


GAIN = gain_cells()


def memo_cells():
    c = {}
    for tag, ns in C3NS.items():
        c[f"c3-same-{tag}"] = (lambda r, tag=tag, ns=ns: c3_stmt(tag, 7))
        c[f"c3-vary-{tag}"] = (lambda r, tag=tag, ns=ns: c3_stmt(tag, r.randint(0, ns - 1)))
    return c


MEMO = memo_cells()


class Srv:
    def __init__(self, label, join_order="planned", fresh=True):
        self.label = label
        self.port = PORT[label]
        wd = R + "/run"
        self.data = f"{wd}/{label}.db"
        if fresh:
            for sfx in ("", ".wal"):
                p = self.data + sfx
                if os.path.isdir(p): shutil.rmtree(p)
                elif os.path.exists(p): os.remove(p)
            shutil.copy(PRISTINE, self.data)
            shutil.copytree(PRISTINE + ".wal", self.data + ".wal")
        conf = f"{wd}/{label}.conf"
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {self.port}\ncores = 1\ndurability = group\n"
                    f"buffer_pool_frames = 65536\njoin_order = {join_order}\n"
                    f"log_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(f"{wd}/{label}.stderr", "a")
        self.t_start = time.perf_counter()
        self.proc = subprocess.Popen(["taskset", "-c", str(CPU), BIN[label], "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 120
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", self.port, timeout=600, client_name="cc_close")
                break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.05)
        self.t_ready = time.perf_counter()

    def q(self, sql):
        tag, n, _b, err = self.conn.query(sql)
        if err is not None: sys.exit(f"{self.label}: {sql[:120]} -> {err}")
        return n

    def rows(self, sql):
        f, rows, _t, _n = self.conn.execute(sql)
        return [[None if x is None else kwp.render_value(fl, x) for fl, x in zip(f, r)] for r in rows], f

    def meta(self):
        rows, _ = self.rows("SHOW META")
        return {f"{r[1]}": r[2] for r in rows}

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(120)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def setup_ov(s):
    s.q("CREATE TABLE ov_ex (id int64, name varchar) BTREE")
    s.q("INSERT INTO ov_ex (id, name) VALUES (1, 'XNYS'), (2, 'XNAS')")
    s.q("CREATE TABLE ov_ins (id int64, a int64, b int64, c int32) BTREE")
    s.q("CREATE TABLE ov_upd (id int64, a int64, b int64, c int32) BTREE")
    rng = random.Random("ovupd")
    for i in range(1, OV_ROWS + 1, 500):
        vals = ", ".join(f"({k}, {rng.randint(0, 10**6)}, {rng.randint(0, 10**6)}, {k % 100})"
                         for k in range(i, min(i + 500, OV_ROWS + 1)))
        s.q(f"INSERT INTO ov_upd (id, a, b, c) VALUES {vals}")


def load_json(name, default):
    p = f"{OUT}/{name}"
    return json.load(open(p)) if os.path.exists(p) else default


def save_json(name, obj):
    p = f"{OUT}/{name}"
    json.dump(obj, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)


def run_block(srv, make, ops, seed, warm_ops=None):
    warm = random.Random(f"{seed}-warm")
    for _ in range(warm_ops if warm_ops is not None else max(2, ops // 10)):
        srv.q(make(warm))
    rng = random.Random(seed)
    stmts = [make(rng) for _ in range(ops)]
    lat, rows = [], 0
    cpu0 = sched_ns(srv.proc.pid); w0 = time.perf_counter()
    for q in stmts:
        t0 = time.perf_counter()
        n = srv.q(q)
        lat.append(time.perf_counter() - t0)
        rows += n if n else 0
    wall = time.perf_counter() - w0
    cpu = sched_ns(srv.proc.pid) - cpu0
    return {"ops": ops, "qps": ops / wall, "server_cpu_us": cpu / 1e3 / ops, "lat_us": [round(x * 1e6, 1) for x in lat],
            "rows_total": rows}


def calibrate(srv, cells, key):
    cal = load_json(f"ops-{key}.json", None)
    if cal is not None: return cal
    cal = {}
    for c, make in cells.items():
        rng = random.Random("cal")
        t0 = time.perf_counter(); k = 0
        while time.perf_counter() - t0 < 0.3 or k < 3:
            srv.q(make(rng)); k += 1
        per = (time.perf_counter() - t0) / k
        cal[c] = int(max(30, min(OPS_CAP, TARGET_S / per)))
    save_json(f"ops-{key}.json", cal)
    return cal


def rounds_phase(phase, srvs, cells, arms, ops_of, extra_pre=None):
    """arms: list of (server label, cell-transform) -> arm name; interleaved, rotated per cell and round."""
    results = load_json(f"{phase}.json", [])
    done = {(r["round"], r["cell"], r["arm"]) for r in results}
    t0 = time.time()
    for rnd in range(1, ROUNDS + 1):
        for ci, c in enumerate(cells):
            order = list(arms)
            k = (rnd + ci) % len(order)
            order = order[k:] + order[:k]
            if (rnd + ci) % 2: order = order[::-1]
            for arm in order:
                if (rnd, c, arm[0]) in done: continue
                status(f"{phase} round {rnd}/{ROUNDS} {c} {arm[0]} elapsed {time.time() - t0:.0f}s")
                host = host_state()
                make = arm[2](cells[c]) if len(arm) > 2 else cells[c]
                r = run_block(srvs[arm[1]], make, ops_of[c], f"{c}-{rnd}")
                r.update({"round": rnd, "cell": c, "arm": arm[0], "host": host})
                results.append(r)
                save_json(f"{phase}.json", results)
    return results


def analyze(srv, sql):
    rows, _ = srv.rows("ANALYZE " + sql)
    return [" | ".join("NULL" if x is None else str(x) for x in r) for r in rows]


def multiset(srv, sql):
    rows, _ = srv.rows(sql)
    return sorted(tuple(r) for r in rows)


KEYS = ["plan_stats_generation", "plan_stats_relations", "plan_stats_posteriors", "plan_stats_part_drops",
        "plan_stats_seeds", "plan_stats_seed_pages", "plan_stats_publishes", "plan_stats_steward_us_max",
        "plan_memo_hits", "plan_memo_misses", "pool_resident", "pool_hits", "pool_misses", "pool_refused",
        "sched_wall_us", "sched_system_polled_us", "sched_system_polls", "sched_system_consumed_us",
        "sched_system_longest_poll_us", "sched_idle_block_us", "recovery_checkpoint_us", "checkpoint_runs"]


def snap(metas, label, srvs, which=("A", "B")):
    for w in which:
        m = srvs[w].meta()
        metas.append({"label": label, "server": w, "t": time.strftime('%FT%TZ', time.gmtime()),
                      "cpu_ns": sched_ns(srvs[w].proc.pid), "meta": {k: m.get(k) for k in KEYS}})
    save_json("meta-snapshots.json", metas)


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(R + "/run", exist_ok=True)
    ph = load_json("phases-done.json", [])
    metas = load_json("meta-snapshots.json", [])
    status("setup")
    srvs = {k: Srv(k) for k in ("A", "A2", "B")}
    for s in srvs.values(): setup_ov(s)
    startup = {k: {"ready_s": round(s.t_ready - s.t_start, 3)} for k, s in srvs.items()}
    save_json("startup.json", startup)
    snap(metas, "after-setup", srvs)

    # ---- P1 OVERHEAD
    if "overhead" not in ph:
        ops_of = calibrate(srvs["A2"], OVERHEAD, "overhead")
        for c, make in OVERHEAD.items():                      # equal warm-up on every server, then >= 2 s for B
            for s in srvs.values():
                status(f"overhead warm {c} {s.label}")
                rng = random.Random(f"w-{c}")
                for _ in range(min(ops_of[c], 3000)): s.q(make(rng))
        time.sleep(2.5)
        arms = [("A", "A"), ("A2", "A2"), ("B", "B")]
        rounds_phase("overhead", srvs, OVERHEAD, arms, ops_of)
        snap(metas, "after-overhead", srvs)
        ph.append("overhead"); save_json("phases-done.json", ph)

    # ---- P2 COLD + plans + result equality
    if "cold" not in ph:
        cold = []
        for ci, (c, make) in enumerate(GAIN.items()):
            order = ["A", "B"] if ci % 2 == 0 else ["B", "A"]
            for lab in order:
                s = srvs[lab]
                rng = random.Random(f"cold-{c}")
                for i in range(20):
                    q = make(rng)
                    cp0 = sched_ns(s.proc.pid); t0 = time.perf_counter()
                    n = s.q(q)
                    dt = time.perf_counter() - t0
                    cold.append({"cell": c, "server": lab, "i": i + 1, "lat_us": round(dt * 1e6, 1),
                                 "cpu_us": round((sched_ns(s.proc.pid) - cp0) / 1e3, 1), "rows": n})
        save_json("cold.json", cold)
        snap(metas, "after-cold", srvs)
        ph.append("cold"); save_json("phases-done.json", ph)

    # ---- warm-up of every gain/memo cell on all three servers equally, then >= 2 s
    gains_ops = calibrate(srvs["A2"], GAIN, "gain")
    memo_ops = calibrate(srvs["A2"], MEMO, "memo")
    status("warm gain+memo")
    t_warm = time.time()
    for c, make in list(GAIN.items()) + list(MEMO.items()):
        n = gains_ops.get(c) or memo_ops[c]
        for s in srvs.values():
            rng = random.Random(f"w-{c}")
            for _ in range(min(n, 1500)): s.q(make(rng))
    while time.time() - t_warm < 6: time.sleep(0.2)          # >= 2 s after the last execution is more than met
    time.sleep(2.5)
    snap(metas, "after-warmup", srvs)

    if "plans" not in ph:
        plans = {}
        for c, make in list(GAIN.items()) + list(MEMO.items()):
            q = make(random.Random("plan"))
            plans[c] = {"stmt": q}
            sets = {}
            for lab, s in srvs.items():
                status(f"plan {c} {lab}")
                plans[c][lab] = analyze(s, q)
                sets[lab] = multiset(s, q)
            ref = sets["A"]
            plans[c]["rows"] = len(ref)
            plans[c]["multiset_equal_A_A2_B"] = all(v == ref for v in sets.values())
        save_json("plans.json", plans)
        ph.append("plans"); save_json("phases-done.json", ph)

    # ---- P3 GAIN
    if "gain" not in ph:
        arms = [("A", "A"), ("A2", "A2"), ("B", "B")]
        rounds_phase("gain", srvs, GAIN, arms, gains_ops)
        snap(metas, "after-gain", srvs)
        ph.append("gain"); save_json("phases-done.json", ph)

    # ---- P4 MEMO
    if "memo" not in ph:
        arms = [("A-same", "A"), ("A2-same", "A2"), ("B-same", "B"), ("A-vary", "A"), ("B-vary", "B")]
        # the arm decides which statement family it runs: same-literal or varying
        cells = {}
        for tag in C3NS:
            cells[tag] = None
        results = load_json("memo.json", [])
        done = {(r["round"], r["cell"], r["arm"]) for r in results}
        t0 = time.time()
        for rnd in range(1, ROUNDS + 1):
            for ci, tag in enumerate(C3NS):
                order = list(arms)
                k = (rnd + ci) % len(order)
                order = order[k:] + order[:k]
                if (rnd + ci) % 2: order = order[::-1]
                for arm, lab in order:
                    mode = arm.split("-")[1]
                    cell = f"c3-{mode}-{tag}"
                    if (rnd, f"c3-{tag}", arm) in done: continue
                    status(f"memo round {rnd}/{ROUNDS} {tag} {arm} elapsed {time.time() - t0:.0f}s")
                    host = host_state()
                    r = run_block(srvs[lab], MEMO[cell], memo_ops[f"c3-same-{tag}"], f"{cell}-{rnd}")
                    r.update({"round": rnd, "cell": f"c3-{tag}", "arm": arm, "host": host})
                    results.append(r)
                    save_json("memo.json", results)
        snap(metas, "after-memo", srvs)
        ph.append("memo"); save_json("phases-done.json", ph)

    # ---- P5 prior accuracy (after every steady phase)
    if "priors" not in ph:
        status("priors")
        time.sleep(11)                                       # past kReseedNs
        rows, f = srvs["B"].rows("SELECT * FROM sys.relation_stats")
        cols = [x["name"] for x in f]
        out = []
        for r in rows:
            d = dict(zip(cols, r))
            if d["namespace"] == "sys": continue
            if d["name"].startswith("ov_"): continue
            d["true_rows"] = srvs["B"].rows(f"SELECT COUNT(*) FROM {d['name']}")[0][0][0]
            out.append(d)
        save_json("priors.json", {"cols": cols, "rows": out})
        ph.append("priors"); save_json("phases-done.json", ph)

    # ---- P6 STABILITY, 60 s mixed run on B
    if "stability" not in ph:
        mix = ["c1-compare-all-r2872", "c1-compare-all-r10k", "c2-customer-statement-ca10k",
               "c2-customer-statement-n1k"]
        gens = [make for make in (GAIN[m] for m in mix)]
        vary = MEMO["c3-vary-x10k"]; same = MEMO["c3-same-x1k"]
        mixed = gens + [vary, same]
        names = mix + ["c3-vary-x10k", "c3-same-x1k"]
        s = srvs["B"]
        rng = random.Random("stab")
        samples = []
        orders = []
        def take_orders(t):
            for nm, mk in zip(names, mixed):
                q = mk(random.Random("plan"))
                lines = analyze(s, q)
                orders.append({"t": t, "cell": nm, "plan": lines})
        m0 = s.meta()
        take_orders(0)
        t0 = time.time(); next_obs = 15; i = 0; win_ops = 0; win_t = t0; wins = []
        cpu0 = sched_ns(s.proc.pid)
        while True:
            now = time.time() - t0
            if now >= 60: break
            if now >= next_obs:
                m = s.meta()
                samples.append({"t": round(now, 1), "gen": m.get("plan_stats_generation"),
                                "publishes": m.get("plan_stats_publishes"), "seeds": m.get("plan_stats_seeds"),
                                "memo_hits": m.get("plan_memo_hits"), "memo_misses": m.get("plan_memo_misses")})
                take_orders(next_obs); next_obs += 15
            mk = mixed[i % len(mixed)]; i += 1
            s.q(mk(rng)); win_ops += 1
            if time.time() - win_t >= 10:
                wins.append({"t": round(time.time() - t0, 1), "ops": win_ops}); win_ops = 0; win_t = time.time()
            if i % 50 == 0: status(f"stability {now:.0f}s ops {i}")
        cpu1 = sched_ns(s.proc.pid)
        m1 = s.meta()
        take_orders(60)
        save_json("stability.json", {"meta_before": m0, "meta_after": m1, "samples": samples, "orders": orders,
                                     "ops": i, "wins": wins, "cpu_us_per_op": (cpu1 - cpu0) / 1e3 / i})
        snap(metas, "after-stability", srvs)
        ph.append("stability"); save_json("phases-done.json", ph)

    # ---- P7 idle CPU of the three servers over 30 s: the steward's standing cost
    if "idle" not in ph:
        status("idle 30s")
        c0 = {k: sched_ns(s.proc.pid) for k, s in srvs.items()}
        mm0 = {k: s.meta() for k, s in srvs.items()}
        time.sleep(30)
        c1 = {k: sched_ns(s.proc.pid) for k, s in srvs.items()}
        mm1 = {k: s.meta() for k, s in srvs.items()}
        save_json("idle.json", {"seconds": 30, "cpu_us": {k: (c1[k] - c0[k]) / 1e3 for k in srvs},
                                "B_publishes": [mm0["B"].get("plan_stats_publishes"), mm1["B"].get("plan_stats_publishes")],
                                "B_seeds": [mm0["B"].get("plan_stats_seeds"), mm1["B"].get("plan_stats_seeds")],
                                "B_seed_pages": [mm0["B"].get("plan_stats_seed_pages"), mm1["B"].get("plan_stats_seed_pages")],
                                "B_steward_us_max": mm1["B"].get("plan_stats_steward_us_max"),
                                "B_sys_polled_us": [mm0["B"].get("sched_system_polled_us"), mm1["B"].get("sched_system_polled_us")],
                                "B_sys_consumed_us": [mm0["B"].get("sched_system_consumed_us"), mm1["B"].get("sched_system_consumed_us")],
                                "A_sys_consumed_us": [mm0["A"].get("sched_system_consumed_us"), mm1["A"].get("sched_system_consumed_us")]})
        ph.append("idle"); save_json("phases-done.json", ph)

    # ---- P8 restart: mount to first publish and to the warm plan (B, clean stop then start)
    if "restart" not in ph:
        status("restart")
        srvs["B"].stop()
        s = Srv("B", fresh=False)
        q = GAIN["c1-compare-all-r2872"](random.Random("plan"))
        t0 = time.perf_counter()
        first_pub = warm_plan = None
        tl = []
        while time.perf_counter() - t0 < 30:
            s.q(q)
            el = time.perf_counter() - t0
            m = s.meta()
            g = int(m.get("plan_stats_generation") or 0)
            if first_pub is None and g > 0: first_pub = el
            plan = analyze(s, q)
            planned = any("order=planned" in l or "order=memo" in l for l in plan)
            if planned and warm_plan is None:
                warm_plan = el
                tl.append(plan)
            if first_pub is not None and warm_plan is not None: break
        save_json("restart.json", {"ready_s": round(s.t_ready - s.t_start, 3), "first_publish_s": first_pub,
                                   "warm_plan_s": warm_plan, "plan": tl,
                                   "recovery_checkpoint_us": s.meta().get("recovery_checkpoint_us")})
        srvs["B"] = s
        ph.append("restart"); save_json("phases-done.json", ph)

    for s in srvs.values(): s.stop()
    status("DONE")


if __name__ == "__main__":
    main()
