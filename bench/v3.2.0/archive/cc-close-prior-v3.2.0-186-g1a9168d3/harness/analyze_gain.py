import json, statistics as st
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/gain.json'))
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
    by = {a: {r['round']: r for r in rs if r['arm'] == a} for a in ('A', 'A2', 'B')}
    row = {}
    for a in ('A', 'A2', 'B'):
        cpu = [by[a][k]['server_cpu_us'] for k in rounds]
        lat = [x for k in rounds for x in by[a][k]['lat_us']]
        qps = [by[a][k]['qps'] for k in rounds]
        row[a] = {"n": len(lat), "cpu_mean": st.mean(cpu), "cpu_rounds": cpu, "spread_pct": (max(cpu) - min(cpu)) / st.mean(cpu) * 100,
                  "qps": st.mean(qps), "p0": min(lat), "p25": pct(lat, 25), "p50": pct(lat, 50), "p95": pct(lat, 95), "p99": pct(lat, 99)}
    dB = [(by['B'][k]['server_cpu_us'] / by['A'][k]['server_cpu_us'] - 1) * 100 for k in rounds]
    dA2 = [(by['A2'][k]['server_cpu_us'] / by['A'][k]['server_cpu_us'] - 1) * 100 for k in rounds]
    dBA2 = [(by['B'][k]['server_cpu_us'] / by['A2'][k]['server_cpu_us'] - 1) * 100 for k in rounds]
    floor = max(abs(st.mean(dA2)), row['A']['spread_pct'] / 2, 2.0)
    out[c] = {"arms": row, "B_vs_A_rounds": dB, "A2_vs_A_rounds": dA2, "B_vs_A": (row['B']['cpu_mean'] / row['A']['cpu_mean'] - 1) * 100,
              "A2_vs_A": (row['A2']['cpu_mean'] / row['A']['cpu_mean'] - 1) * 100, "B_vs_A2": (row['B']['cpu_mean'] / row['A2']['cpu_mean'] - 1) * 100,
              "qps_B_vs_A": (row['B']['qps'] / row['A']['qps'] - 1) * 100, "floor": floor}
    print(c)
    for a in ('A', 'A2', 'B'):
        r = row[a]
        print(f"  {a:3} n={r['n']} cpu={r['cpu_mean']:.1f}us qps={r['qps']:.0f} p0={r['p0']:.0f} p25={r['p25']:.0f} p50={r['p50']:.0f} p95={r['p95']:.0f} p99={r['p99']:.0f} spread={r['spread_pct']:.1f}%")
    print(f"  B vs A {out[c]['B_vs_A']:+.1f}% (rounds {[round(x,1) for x in dB]}) A2 vs A {out[c]['A2_vs_A']:+.1f}% B vs A2 {out[c]['B_vs_A2']:+.1f}% qps B vs A {out[c]['qps_B_vs_A']:+.1f}% floor {floor:.1f}")
json.dump(out, open('/home/cdkbs/bench-runs/cc-prior/out/gain-analysis.json', 'w'))
