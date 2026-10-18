#!/usr/bin/env python3
"""CC-S0 measurement: planned vs written (two spellings) vs a written control server,
interleaved in rotating rounds. Resumable: finished blocks are skipped on restart."""
import argparse, json, os, random, shutil, statistics, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/tools"
sys.path.insert(0, TOOLS)
import kwp
from bn_census import sched_ns


def host_state():
    try:
        la = open("/proc/loadavg").read().split()[0]
    except OSError:
        la = "?"
    r = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest"], capture_output=True, text=True)
    foreign = [l for l in r.stdout.splitlines() if "pgrep" not in l and "cc_s0_run" not in l]
    return {"loadavg1": la, "foreign": len(foreign)}


# ---- statements ----------------------------------------------------------
def c1_stmt(tag, spelling):
    sel = "SELECT r.model_id, r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name, m.family "
    if spelling == "A":
        return sel + f"FROM model_results_{tag} AS r JOIN models_{tag} AS m ON r.model_id = m.id"
    return sel + f"FROM models_{tag} AS m JOIN model_results_{tag} AS r ON r.model_id = m.id"


def c2_stmt(suffix, spelling, org):
    where = f"WHERE c.org_id = {org} GROUP BY c.org_id"
    if spelling == "A":
        return (f"SELECT c.org_id, SUM(f.cbm) FROM freights_{suffix} AS f "
                f"JOIN cargos_{suffix} AS c ON f.cargo_id = c.id {where}")
    return (f"SELECT c.org_id, SUM(f.cbm) FROM cargos_{suffix} AS c "
            f"JOIN freights_{suffix} AS f ON f.cargo_id = c.id {where}")


def c3_stmt(tag, sess):
    return ("SELECT t.symbol_id, t.ret_bp, t.mom5_bp, t.mom20_bp, t.mom60_bp, t.mom120_bp, "
            "t.vol10_bp, t.vol20_bp, t.vol60_bp, b.close, s.ticker "
            f"FROM daily_stats_{tag} AS t JOIN daily_bars_{tag} AS b ON t.bar_id = b.id "
            f"JOIN symbols_{tag} AS s ON t.symbol_id = s.id WHERE t.session_no = {sess}")


# cell -> (spellings, generator(rng, spelling) -> sql)
def build_cells(s2):
    cells = {}
    for tag in ("r200", "r1k", "r2872", "r10k"):
        cells[f"c1-compare-all-{tag}"] = (("A", "B"), (lambda r, sp, tag=tag: c1_stmt(tag, sp)))
    for suf, (norg, label) in s2.items():
        cells[f"c2-customer-statement-{label}"] = (
            ("A", "B"), (lambda r, sp, suf=suf, norg=norg: c2_stmt(suf, sp, r.randint(1, norg))))
    for tag, ns in (("x200", 25), ("x1k", 125), ("x10k", 1250), ("x60k", 7560)):
        cells[f"c3-cross-join-{tag}"] = (("A",), (lambda r, sp, tag=tag, ns=ns: c3_stmt(tag, r.randint(0, ns - 1))))
    return cells


class Srv:
    def __init__(self, label, binary, port, wd, pristine, cpu, join_order):
        self.label, self.port, self.join_order = label, port, join_order
        self.data = os.path.join(wd, f"{label}.db")
        for suffix in ("", ".wal"):
            path = self.data + suffix
            if os.path.isdir(path): shutil.rmtree(path)
            elif os.path.exists(path): os.remove(path)
        shutil.copy(pristine, self.data)
        if os.path.isdir(pristine + ".wal"):
            shutil.copytree(pristine + ".wal", self.data + ".wal")
        conf = os.path.join(wd, f"{label}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = 1\ndurability = group\n"
                    f"buffer_pool_frames = 65536\njoin_order = {join_order}\n"
                    f"log_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(os.path.join(wd, f"{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 60
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cc_s0_run"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(60)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def status(path, line):
    tmp = path + ".tmp"
    with open(tmp, "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(tmp, path)


def run_block(srv, make, spelling, ops, seed):
    warm = random.Random(f"{seed}-warm")
    for _ in range(max(2, ops // 10)):
        if srv.conn.query(make(warm, spelling))[3] is not None: sys.exit(f"{srv.label} warm failed")
    rng = random.Random(seed)
    stmts = [make(rng, spelling) for _ in range(ops)]
    lat, counts = [], []
    cpu0 = sched_ns(srv.proc.pid); w0 = time.perf_counter()
    for q in stmts:
        t0 = time.perf_counter()
        _t, n, _b, err = srv.conn.query(q)
        lat.append(time.perf_counter() - t0)
        if err is not None: sys.exit(f"{srv.label}: {q[:100]} -> {err}")
        counts.append(n)
    wall = time.perf_counter() - w0
    cpu = sched_ns(srv.proc.pid) - cpu0
    return {"ops": ops, "qps": ops / wall, "server_cpu_us": cpu / 1e3 / ops, "lat_us": [x * 1e6 for x in lat],
            "rows_total": sum(counts)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin"); p.add_argument("--pristine"); p.add_argument("--wd"); p.add_argument("--out")
    p.add_argument("--status"); p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--s2", help="json {suffix: [norg, label]}"); p.add_argument("--cells", default="")
    p.add_argument("--cpu", type=int, default=2); p.add_argument("--port-base", type=int, default=25601)
    p.add_argument("--target-s", type=float, default=1.0)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    s2 = {k: tuple(v) for k, v in json.loads(a.s2).items()}
    cells = build_cells(s2)
    names = a.cells.split(",") if a.cells else list(cells)
    srvs = {"P": Srv("P", a.bin, a.port_base, a.wd, a.pristine, a.cpu, "planned"),
            "W": Srv("W", a.bin, a.port_base + 1, a.wd, a.pristine, a.cpu, "written"),
            "W2": Srv("W2", a.bin, a.port_base + 2, a.wd, a.pristine, a.cpu, "written")}
    done, results = set(), []
    if os.path.exists(a.out):
        results = json.load(open(a.out))
        done = {(r["round"], r["cell"], r["arm"]) for r in results}
    # plans and result equality (once)
    plans_path = a.out + ".plans"
    if not os.path.exists(plans_path):
        plans = {}
        for c in names:
            sps, make = cells[c]
            q = {sp: make(random.Random("plan"), sp) for sp in sps}
            plans[c] = {"stmt": q}
            sets = {}
            for key, srv in srvs.items():
                for sp in sps:
                    status(a.status, f"plan {c} {key}:{sp}")
                    _f, rows, _t, _n = srv.conn.execute("ANALYZE " + q[sp])
                    plans[c][f"{key}:{sp}"] = [" | ".join((x.decode(errors="replace") if x is not None else "NULL")
                                                          for x in row) for row in rows]
                    _f, rows, _t, _n = srv.conn.execute(q[sp])
                    sets[f"{key}:{sp}"] = sorted(tuple(r) for r in rows)
            ref = next(iter(sets.values()))
            plans[c]["rows"] = len(ref)
            plans[c]["multiset_equal_all_arms"] = all(v == ref for v in sets.values())
        json.dump(plans, open(plans_path, "w"), indent=1)
    # calibration: ops per cell from W2:A, fixed across arms
    cal_path = a.out + ".ops"
    if os.path.exists(cal_path):
        ops_of = json.load(open(cal_path))
    else:
        ops_of = {}
        for c in names:
            sps, make = cells[c]
            rng = random.Random("cal")
            t0 = time.perf_counter(); k = 0
            while time.perf_counter() - t0 < 0.3 or k < 3:
                srvs["W2"].conn.query(make(rng, "A")); k += 1
            per = (time.perf_counter() - t0) / k
            ops_of[c] = int(max(30, min(3000, a.target_s / per)))
        json.dump(ops_of, open(cal_path, "w"))
    t0 = time.time()
    arms = [("P", "A"), ("P", "B"), ("W", "A"), ("W", "B"), ("W2", "A")]
    for rnd in range(1, a.rounds + 1):
        for ci, c in enumerate(names):
            sps, make = cells[c]
            order = [x for x in arms if x[1] in sps]
            k = (rnd + ci) % len(order)
            order = order[k:] + order[:k]
            if (rnd + ci) % 2: order = order[::-1]
            for key, sp in order:
                if (rnd, c, f"{key}:{sp}") in done: continue
                status(a.status, f"round {rnd}/{a.rounds} {c} {key}:{sp} elapsed {time.time() - t0:.0f}s")
                host = host_state()
                r = run_block(srvs[key], make, sp, ops_of[c], f"{c}-{rnd}")
                r.update({"round": rnd, "cell": c, "arm": f"{key}:{sp}", "host": host})
                results.append(r)
                json.dump(results, open(a.out + ".tmp", "w")); os.replace(a.out + ".tmp", a.out)
    for s in srvs.values(): s.stop()
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
