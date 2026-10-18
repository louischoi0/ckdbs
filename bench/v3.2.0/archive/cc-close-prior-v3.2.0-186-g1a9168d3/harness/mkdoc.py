import json, gzip, statistics as st
O = '/home/cdkbs/bench-runs/cc-prior/out/'
OLD = '/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14/R1/'
pri = json.load(open(O + 'priors.json')); rst = json.load(open(O + 'restart.json'))
old = {r['name']: r for r in json.load(open(OLD + 'priors.json'))['rows']}
rr = {r['name']: r for r in rst['rows']}
g1 = json.load(open(O + 'gain-analysis.json')); g2 = json.load(open(O + 'gain4-analysis.json'))
g5 = json.load(open(O + 'gain5.json'))


def n(x): return f"{int(x):,}"


# ---- prior table
rows = []; byh = {}; byh_old = {}
for r in pri['B']['rows']:
    if r['est_rows'] is None: continue
    t = int(r['true_rows']); e = int(r['est_rows']); oe = int(old[r['name']]['est_rows']); e2 = int(rr[r['name']]['est_rows'])
    h = int(r['height'])
    byh.setdefault(h, []).append(e / t); byh_old.setdefault(h, []).append(oe / t)
    rows.append(f"| {r['name']} | {h} | {n(r['leaves'])} | {n(t)} | {n(e)} | **{e / t:.2f}** | {n(e2)} ({e2 / t:.2f}) | {n(oe)} | {oe / t:.2f} |")
ptab = "\n".join(rows)
hrows = []
for h in sorted(byh):
    hrows.append(f"| {h} | {len(byh[h])} | {min(byh[h]):.2f}-{max(byh[h]):.2f} | {min(byh_old[h]):.2f}-{max(byh_old[h]):.2f} |")
htab = "\n".join(hrows)
allr = [x for v in byh.values() for x in v]

# ---- steward cost
def d(a, b, k): return int(b[k]) - int(a[k])
def srow(label, w, a, b, extra=""):
    return (f"| {label} | {(int(b['sched_wall_us']) - int(a['sched_wall_us'])) / 1e6:.1f} s | {d(a, b, 'plan_stats_seeds') if a.get('plan_stats_seeds') is not None else '-'} | "
            f"{d(a, b, 'plan_stats_seed_pages') if a.get('plan_stats_seeds') is not None else '-'} | {d(a, b, 'plan_stats_publishes') if a.get('plan_stats_seeds') is not None else '-'} | "
            f"{b.get('plan_stats_steward_us_max') or '-'} µs | {d(a, b, 'sched_system_consumed_us')} µs / {d(a, b, 'sched_system_polls')} | {d(a, b, 'pool_misses')} | {d(a, b, 'pool_hits')} |")
P = pri
s_rows = [
    srow("B fresh server, idle after statements", 0, P['B']['after_statements'], P['B']['after_idle']),
    srow("A (65dc45c5) control, same statements, idle", 0, P['A']['after_statements'], P['A']['after_idle']),
    srow("B restarted on its file, idle after light statements", 0, rst['after_statements'], rst['after_idle']),
    srow("B, one `SELECT * FROM sys.relation_stats` (cold)", 0, P['B']['after_idle'], P['B']['after_view']),
]
stab = "\n".join(s_rows)
mis_view = d(P['B']['after_idle'], P['B']['after_view'], 'pool_misses'); hit_view = d(P['B']['after_idle'], P['B']['after_view'], 'pool_hits')
wl = pri['B']['wait_log']
wtxt = ", ".join(f"{int(l['meta']['plan_stats_relations'])} at {l['t']:.0f} s" for l in wl[:5])


# ---- gain tables
OLDR1 = {"c1-compare-all-r2872": -35.6, "c1-compare-all-r10k": -36.6, "c2-customer-statement-ca10k": -29.1, "c2-customer-statement-n1k": -60.4}
names = {"c1-compare-all-r2872": "scenario 1 compare-all, 2,872 results", "c1-compare-all-r10k": "scenario 1 compare-all, 10,000 results",
         "c2-customer-statement-ca10k": "scenario 2 customer statement, 10,000 cargos", "c2-customer-statement-n1k": "scenario 2 customer statement, 1,000 cargos"}
grow = []
for c in names:
    a = g2[c]['arms']
    grow.append(f"| {names[c]} | {a['A']['cpu_mean']:.0f} µs / {a['A']['qps']:.0f} | {a['B']['cpu_mean']:.0f} µs / {a['B']['qps']:.0f} | "
                f"**{g1[c]['B_vs_A']:+.1f} %** | **{g2[c]['B_vs_A']:+.1f} %** | {g2[c]['BO_vs_A']:+.1f} % | {g2[c]['B_vs_BO']:+.1f} % | {g2[c]['A2_vs_A']:+.1f} % / {g1[c]['A2_vs_A']:+.1f} % | ±{g2[c]['floor']:.1f} % | {OLDR1[c]:+.1f} % |")
gtab = "\n".join(grow)
lrows = []
for c in names:
    for a in ('A', 'A2', 'B', 'BO'):
        r = g2[c]['arms'][a]
        lrows.append(f"| {names[c]} | {a} | {n(r['n'])} | {r['cpu_mean']:.1f} µs | {r['qps']:.0f} | {r['p0']:,.0f} | {r['p25']:,.0f} | {r['p50']:,.0f} | {r['p95']:,.0f} | {r['p99']:,.0f} | {r['spread_pct']:.1f} % |")
ltab = "\n".join(lrows)
l1 = []
for c in names:
    for a in ('A', 'A2', 'B'):
        r = g1[c]['arms'][a]
        l1.append(f"| {names[c]} | {a} | {n(r['n'])} | {r['cpu_mean']:.1f} µs | {r['qps']:.0f} | {r['p0']:,.0f} | {r['p25']:,.0f} | {r['p50']:,.0f} | {r['p95']:,.0f} | {r['p99']:,.0f} | {r['spread_pct']:.1f} % |")
l1tab = "\n".join(l1)
# gain5
g5rows = []
for c in ("c2-customer-statement-ca10k", "c2-customer-statement-n1k"):
    rs = [r for r in g5 if r['cell'] == c]; rounds = sorted({r['round'] for r in rs})
    by = {a: {r['round']: r for r in rs if r['arm'] == a} for a in ('A', 'B1', 'B2')}
    m = {a: st.mean(by[a][k]['server_cpu_us'] for k in rounds) for a in by}
    g5rows.append(f"| {names[c]} | {m['A']:.0f} µs | {m['B2']:.0f} µs ({(m['B2'] / m['A'] - 1) * 100:+.1f} %) | {m['B1']:.0f} µs ({(m['B1'] / m['A'] - 1) * 100:+.1f} %) |")
g5tab = "\n".join(g5rows)
hosts = []
for f, fn in (('gain.json.gz', 'gain'), ('gain4.json.gz', 'gain4'), ('gain5.json.gz', 'gain5')):
    dd = json.load(gzip.open(O + f)) if False else None
print(ptab); print(htab); print(stab); print(wtxt); print(gtab); print(ltab[:200])
json.dump({"ptab": ptab, "htab": htab, "stab": stab, "wtxt": wtxt, "gtab": gtab, "ltab": ltab, "l1tab": l1tab, "g5tab": g5tab,
           "mis_view": mis_view, "hit_view": hit_view, "lo": min(allr), "hi": max(allr)}, open(O + 'tables.json', 'w'))
