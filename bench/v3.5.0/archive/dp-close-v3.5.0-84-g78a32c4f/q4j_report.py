import json, statistics as st
d = json.load(open('/home/louis/bench-runs/dp-close/archive/q234/dp_close_q4j.json'))
v = d['q4j']['depths']
c = v['cpu_rounds']
print(v['host_before'], v['host_after'], v['rows_per_stmt'])
for x, a in v['arms'].items():
    l = a['lat']
    print(x, a['cpu_us_median'], l['ops'], l['p0'], l['p25'], l['p50'], l['p95'], l['p99'])
for dd in (1, 2, 4, 8):
    dl = [m - h for m, h in zip(c[f'm{dd}'], c[f'H{dd}'])]
    print(f"m{dd}-H{dd}: median {st.median(dl):+.2f} min {min(dl):+.2f} max {max(dl):+.2f}")
dl = [m - h for m, h in zip(c['H1b'], c['H1'])]
print(f"floor H1b-H1: median {st.median(dl):+.2f} min {min(dl):+.2f} max {max(dl):+.2f}")
print(v['sample']['m8'])
