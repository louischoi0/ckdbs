#!/usr/bin/env python3
"""CC prior re-measure. B = 1a9168d3 (steward's descent-under-root-children prior), A = 65dc45c5 (CC-S0 opening commit).
Phases: priors (fresh B and A, cold pool), restart (B on its own file, cold pool again), gain (fresh clean A, A2, B).
Reuses the cc-close harness module for servers, blocks and statements."""
import json, os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H

R = "/home/cdkbs/bench-runs/cc-prior"
H.R = R; H.OUT = R + "/out"; H.STATUS = R + "/out/run.status"
H.BIN.update({"A": "/home/cdkbs/bench-runs/cc-close/kds_server_A", "A2": "/home/cdkbs/bench-runs/cc-close/kds_server_A",
              "B": R + "/kds_server_B"})
H.PORT.update({"A": 25801, "A2": 25802, "B": 25803})
H.ROUNDS = 5
OUT = H.OUT
KEEP = ("plan_stats_", "plan_memo_", "pool_", "sched_system_", "sched_wall_us", "sched_idle_block_us", "checkpoint_runs",
        "recovery_checkpoint_us")


def meta(s):
    m = s.meta()
    return {k: v for k, v in m.items() if k.startswith(KEEP)}


def stats_rows(s):
    rows, f = s.rows("SELECT * FROM sys.relation_stats")
    cols = [x["name"] for x in f]
    return cols, [dict(zip(cols, r)) for r in rows]


def touch_all(s, reps, light=False):
    """Planned statements that name every s1/s2 relation. light: LIMIT 1 on compare-all so few leaves are read."""
    rng = random.Random("prior-touch")
    for rep in range(reps):
        for tag in ("r200", "r1k", "r2872", "r10k"):
            q = H.c1_stmt(tag) + (" LIMIT 1" if light else "")
            s.q(q)
        for suf in ("n200", "n1k", "ca", "df"):
            s.q(H.c2_stmt(suf, rng.randint(1, H.NORG[suf])))
        for tag, ns in H.C3NS.items():
            s.q(H.c3_stmt(tag, rng.randint(0, ns - 1)))


def seeded_wait(s, tag, maxs=45):
    """poll SHOW META only (sys.relation_stats walks every relation's leaf level and would fault pages itself) until the
    generation is unchanged three polls running with all 28 relations published; read the view once, last."""
    t0 = time.time(); prevg = None; same = 0; log = []
    while time.time() - t0 < maxs:
        time.sleep(2)
        m = meta(s)
        log.append({"t": round(time.time() - t0, 1),
                    "meta": {k: m.get(k) for k in ("plan_stats_generation", "plan_stats_relations", "plan_stats_seeds",
                                                   "plan_stats_seed_pages", "plan_stats_publishes", "plan_stats_steward_us_max",
                                                   "pool_misses", "pool_hits")}})
        H.status(f"{tag} wait {log[-1]['t']}s gen={m.get('plan_stats_generation')} rel={m.get('plan_stats_relations')}")
        g = m.get("plan_stats_generation")
        same = same + 1 if g == prevg else 0
        prevg = g
        if same >= 3 and int(m.get("plan_stats_relations") or 0) >= 28: break
    return log


def true_counts(s, rows):
    out = []
    for d in rows:
        if d["namespace"] == "sys" or d["name"].startswith("ov_"): continue
        d = dict(d)
        d["true_rows"] = s.rows(f"SELECT COUNT(*) FROM {d['name']}")[0][0][0]
        out.append(d)
    return out


def phase_priors():
    H.status("priors start")
    res = {}
    srvs = {k: H.Srv(k) for k in ("B", "A")}
    for k, s in srvs.items():
        res[k] = {"after_mount": meta(s), "ready_s": round(s.t_ready - s.t_start, 3)}
    for k, s in srvs.items():
        H.status(f"priors touch {k}")
        touch_all(s, 5)
        res[k]["after_statements"] = meta(s)
        res[k]["t_stmts_end"] = time.time()
    # idle window: both idle together; B polled for seeding
    res["B"]["wait_log"] = seeded_wait(srvs["B"], "priors")
    time.sleep(max(0, 20 - (time.time() - res["A"]["t_stmts_end"])))
    for k, s in srvs.items():
        res[k]["after_idle"] = meta(s)
    cols, rows = stats_rows(srvs["B"])
    res["B"]["after_view"] = meta(srvs["B"])
    res["B"]["cols"] = cols
    res["B"]["rows"] = true_counts(srvs["B"], rows)
    res["B"]["after_count"] = meta(srvs["B"])
    json.dump(res, open(OUT + "/priors.json", "w"))
    # restart B on its own file: cold pool again, light touches, idle, read
    H.status("restart B")
    srvs["B"].stop()
    s = H.Srv("B", fresh=False)
    r2 = {"ready_s": round(s.t_ready - s.t_start, 3), "after_mount": meta(s)}
    touch_all(s, 1, light=True)
    r2["after_statements"] = meta(s)
    r2["wait_log"] = seeded_wait(s, "restart")
    r2["after_idle"] = meta(s)
    cols, rows = stats_rows(s)
    r2["after_view"] = meta(s)
    r2["rows"] = true_counts(s, rows)
    json.dump(r2, open(OUT + "/restart.json", "w"))
    s.stop(); srvs["A"].stop()
    H.status("priors done")


def phase_gain():
    H.status("gain start")
    CELLS = {c: H.GAIN[c] for c in ("c1-compare-all-r2872", "c1-compare-all-r10k", "c2-customer-statement-ca10k",
                                    "c2-customer-statement-n1k")}
    ops = json.load(open("/home/cdkbs/bench-runs/cc-close/out/ops-gain.json"))
    json.dump(ops, open(OUT + "/ops-gain.json", "w"))
    srvs = {k: H.Srv(k) for k in ("A", "A2", "B")}
    for c, make in CELLS.items():
        for k, s in srvs.items():
            H.status(f"gain warm {c} {k}")
            rng = random.Random("w-" + c)
            for _ in range(min(ops[c], 1500)): s.q(make(rng))
    time.sleep(8)
    plans = {}
    for c, make in CELLS.items():
        q = make(random.Random("plan"))
        plans[c] = {"stmt": q, **{k: H.analyze(s, q) for k, s in srvs.items()}}
    json.dump(plans, open(OUT + "/plans.json", "w"))
    H.rounds_phase("gain", srvs, CELLS, [("A", "A"), ("A2", "A2"), ("B", "B")], ops)
    json.dump({k: meta(s) for k, s in srvs.items()}, open(OUT + "/gain-meta.json", "w"))
    for s in srvs.values(): s.stop()
    H.status("gain done")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True); os.makedirs(R + "/run", exist_ok=True)
    which = sys.argv[1:] or ["priors", "gain"]
    if "priors" in which: phase_priors()
    if "gain" in which: phase_gain()
    H.status("ALL DONE")
    open(OUT + "/run.done", "w").write("done\n")
