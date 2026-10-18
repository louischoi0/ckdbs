import json, statistics as st
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/gain4.json'))
ARMS = ('A', 'A2', 'B', 'BO')
cells = []
for r in d:
    if r['cell'] not in cells: cells.append(r['cell'])


def pct(v, p):
    v = sorted(v); k = (len(v) - 1) * p / 100; f = int(k); c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


out = {}
for c in cells:
    rs = [r for r in d if r['cell'] == c]
    rounds = sorted({r['round'] for r in rs})
    by = {a: {r['round']: r for r in rs if r['arm'] == a} for a in ARMS}
    row = {}
    for a in ARMS:
        cpu = [by[a][k]['server_cpu_us'] for k in rounds]
        lat = [x for k in rounds for x in by[a][k]['lat_us']]
        qps = [by[a][k]['qps'] for k in rounds]
        row[a] = {"n": len(lat), "cpu_mean": st.mean(cpu), "cpu_rounds": cpu, "spread_pct": (max(cpu) - min(cpu)) / st.mean(cpu) * 100,
                  "qps": st.mean(qps), "p0": min(lat), "p25": pct(lat, 25), "p50": pct(lat, 50), "p95": pct(lat, 95), "p99": pct(lat, 99)}
    def pd(x, y): return [(by[x][k]['server_cpu_us'] / by[y][k]['server_cpu_us'] - 1) * 100 for k in rounds]
    res = {"arms": row, "floor": max(abs(st.mean(pd('A2', 'A'))), row['A']['spread_pct'] / 2, 2.0)}
    for x, y in (('B', 'A'), ('BO', 'A'), ('A2', 'A'), ('B', 'BO')):
        res[f"{x}_vs_{y}"] = (row[x]['cpu_mean'] / row[y]['cpu_mean'] - 1) * 100
        res[f"{x}_vs_{y}_rounds"] = pd(x, y)
    res["qps_B_vs_A"] = (row['B']['qps'] / row['A']['qps'] - 1) * 100
    res["qps_BO_vs_A"] = (row['BO']['qps'] / row['A']['qps'] - 1) * 100
    out[c] = res
    print(c, f"floor {res['floor']:.1f}")
    for a in ARMS:
        r = row[a]
        print(f"  {a:3} n={r['n']} cpu={r['cpu_mean']:.1f}us qps={r['qps']:.0f} p0={r['p0']:.0f} p25={r['p25']:.0f} p50={r['p50']:.0f} p95={r['p95']:.0f} p99={r['p99']:.0f} spread={r['spread_pct']:.1f}%")
    for x, y in (('B', 'A'), ('BO', 'A'), ('A2', 'A'), ('B', 'BO')):
        print(f"  {x} vs {y} {res[f'{x}_vs_{y}']:+.1f}%  rounds {[round(v, 1) for v in res[f'{x}_vs_{y}_rounds']]}")
json.dump(out, open('/home/cdkbs/bench-runs/cc-prior/out/gain4-analysis.json', 'w'))
