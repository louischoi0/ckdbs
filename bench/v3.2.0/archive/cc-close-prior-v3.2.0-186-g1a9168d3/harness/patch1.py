p = '/home/cdkbs/bench-runs/cc-prior/harness/prior_run.py'
s = open(p).read()
old = s[s.index('def seeded_wait'):s.index('def true_counts')]
new = '''def seeded_wait(s, tag, maxs=45):
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


'''
s = s.replace(old, new)
s = s.replace('''    log, cols, rows = seeded_wait(srvs["B"], "priors", 40)
    res["B"]["wait_log"] = log
    time.sleep(max(0, 20 - (time.time() - res["A"]["t_stmts_end"])))
    for k, s in srvs.items():
        res[k]["after_idle"] = meta(s)
''', '''    res["B"]["wait_log"] = seeded_wait(srvs["B"], "priors")
    time.sleep(max(0, 20 - (time.time() - res["A"]["t_stmts_end"])))
    for k, s in srvs.items():
        res[k]["after_idle"] = meta(s)
    cols, rows = stats_rows(srvs["B"])
    res["B"]["after_view"] = meta(srvs["B"])
''')
s = s.replace('''    log2, cols, rows = seeded_wait(s, "restart", 40)
    r2["wait_log"] = log2
    r2["after_idle"] = meta(s)
''', '''    r2["wait_log"] = seeded_wait(s, "restart")
    r2["after_idle"] = meta(s)
    cols, rows = stats_rows(s)
    r2["after_view"] = meta(s)
''')
open(p, 'w').write(s)
print('seeded_wait(srvs' in s, 'after_view' in s)
