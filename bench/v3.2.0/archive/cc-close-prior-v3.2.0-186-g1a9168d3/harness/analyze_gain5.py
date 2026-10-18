import json, statistics as st
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/gain5.json'))
for c in sorted({r['cell'] for r in d}):
    rs = [r for r in d if r['cell'] == c]
    rounds = sorted({r['round'] for r in rs})
    by = {a: {r['round']: r for r in rs if r['arm'] == a} for a in ('A', 'B1', 'B2')}
    m = {a: st.mean(by[a][k]['server_cpu_us'] for k in rounds) for a in by}
    print(c, {a: round(v, 1) for a, v in m.items()},
          {a: f"{(m[a] / m['A'] - 1) * 100:+.1f}%" for a in ('B1', 'B2')},
          {a: [round((by[a][k]['server_cpu_us'] / by['A'][k]['server_cpu_us'] - 1) * 100, 1) for k in rounds] for a in ('B1', 'B2')})
