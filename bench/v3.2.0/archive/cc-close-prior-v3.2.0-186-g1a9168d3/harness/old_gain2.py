import json, gzip, statistics as st
A = '/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14/R1/'
d = json.load(gzip.open(A + 'gain2.json.gz'))
for c in sorted({r['cell'] for r in d}):
    rs = [r for r in d if r['cell'] == c]
    arms = sorted({r['arm'] for r in rs})
    m = {a: st.mean(r['server_cpu_us'] for r in rs if r['arm'] == a) for a in arms}
    print(c, {a: round(v, 1) for a, v in m.items()}, {a: f"{(m[a] / m['A'] - 1) * 100:+.1f}%" for a in arms})
p = json.load(open(A + 'plans.json'))
for c in ('c2-customer-statement-ca10k', 'c1-compare-all-r2872'):
    print(c, '|'.join(p[c]['B'])[:600])
g = json.load(gzip.open(A + 'gain.json.gz'))
for c in sorted({r['cell'] for r in g}):
    rs = [r for r in g if r['cell'] == c]
    m = {a: st.mean(r['server_cpu_us'] for r in rs if r['arm'] == a) for a in ('A', 'A2', 'B')}
    print('gain R1', c, {a: round(v, 1) for a, v in m.items()}, f"B vs A {(m['B'] / m['A'] - 1) * 100:+.1f}%")
