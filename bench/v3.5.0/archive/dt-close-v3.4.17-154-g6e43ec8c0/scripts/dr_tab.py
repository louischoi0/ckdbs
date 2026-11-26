import json, sys, statistics as st
d = json.load(open(sys.argv[1]))
cells = d['cells']
print('quiet_hold_s total', sum(c.get('quiet_hold_s', 0) for c in cells), 'elapsed', d['elapsed_s'], 'sha', d['sha256_16'], 'started', d['started_utc'])
print('host_before', d['host_before'], 'host_after', d['host_after'])
print('max cell host cc1plus', set(str(c['host'].get('cc1plus')) for c in cells), 'loads', sorted(set(c['host']['loadavg'].split()[0] for c in cells))[-3:])
rounds = sorted({c['round'] for c in cells})
phases = [('read', 'point SELECT by key'), ('update', 'point UPDATE by key'), ('insert', 'single-row INSERT'), ('delete', 'DELETE by key'), ('range', 'range, 100 rows'), ('walked', 'walked UPDATE, 100 rows of 100,000')]
for rel in ('btree', 'heap'):
    for ph, name in phases:
        v = {}
        for c in cells:
            v.setdefault(c['arm'], {})[c['round']] = c[rel][ph]['cpu_us'] * 1000.0
        if not v.get('A'):
            continue
        med = {a: st.median(v[a].values()) for a in v}
        lo = {a: min(v[a].values()) for a in v}
        hi = {a: max(v[a].values()) for a in v}
        dBA = [v['B'][r] - v['A'][r] for r in rounds]
        dA2A = [v['A2'][r] - v['A'][r] for r in rounds]
        dBA2 = [v['B'][r] - v['A2'][r] for r in rounds]
        print(f"{rel:5} {name:34} A {med['A']:>10,.0f} ({lo['A']:,.0f}-{hi['A']:,.0f}) A2 {med['A2']:>10,.0f} C {med['B']:>10,.0f} ({lo['B']:,.0f}-{hi['B']:,.0f}) "
              f"| C-A {med['B']-med['A']:+,.0f} ({(med['B']-med['A'])/med['A']*100:+.1f}%) A2-A {med['A2']-med['A']:+,.0f} (rng {min(dA2A):+,.0f}..{max(dA2A):+,.0f}) "
              f"C-A2 {med['B']-med['A2']:+,.0f} ({(med['B']-med['A2'])/med['A2']*100:+.1f}%) per-round C-A {min(dBA):+,.0f}..{max(dBA):+,.0f} C>A {sum(1 for x in dBA if x>0)}/15 medianround {st.median(dBA):+,.0f}")
