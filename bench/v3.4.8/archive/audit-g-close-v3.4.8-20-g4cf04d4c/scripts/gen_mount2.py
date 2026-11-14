import json, statistics as st, math
S = '/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/out2'


def pct(q, p):
    q = sorted(q)
    return q[max(0, min(len(q) - 1, math.ceil(p / 100 * len(q)) - 1))]


cells = (('empty log (rows 0)', 'mount-0', None),
         ('4,000 rows, clean stop', 'mount-4000', None),
         ('4,000 rows, SIGKILL: first mount (replays about 29k records)', 'mountcrash-4000', 'first'),
         ('4,000 rows, SIGKILL: mounts 2-7 of each run', 'mountcrash-4000', 'rest'))
print('| cell | build | mounts | p0 | p25 | p50 | p95 | p99 (= max) | mean | `recovery_analysis` mean | `recovery_checkpoint` mean |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
means = {}
for name, pat, which in cells:
    for l in 'ABC':
        w = []; an = []; ck = []
        for r in (1, 2, 3):
            ms = json.load(open(f'{S}/{pat}-{l}-r{r}.json'))['mounts']
            ms = ms[:1] if which == 'first' else ms[1:] if which == 'rest' else ms
            w += [m['wall_ms'] for m in ms]; an += [m['recovery_analysis_us'] / 1000 for m in ms]; ck += [m['recovery_checkpoint_us'] / 1000 for m in ms]
        means[(name, l)] = (st.mean(w), st.median(w), min(w), max(w), st.mean(an))
        print(f"| {name} | {l} | {len(w)} | {min(w):.1f} | {pct(w,25):.1f} | {st.median(w):.1f} | {pct(w,95):.1f} | {pct(w,99):.1f} | {st.mean(w):.1f} | {st.mean(an):.1f} | {st.mean(ck):.1f} |")
print()
print('| cell | B - A mean | C - A mean | C - A p50 | C - B mean | share of B-A removed | A max vs C min | C min vs B max |')
print('|---|---|---|---|---|---|---|---|')
for name, pat, which in cells:
    a = means[(name, 'A')]; b = means[(name, 'B')]; c = means[(name, 'C')]
    print(f"| {name} | {b[0]-a[0]:+.1f} ms | {c[0]-a[0]:+.1f} ms | {c[1]-a[1]:+.1f} ms | {c[0]-b[0]:+.1f} ms | {100*(1-(c[0]-a[0])/(b[0]-a[0])):.0f} % | {a[3]:.1f} / {c[2]:.1f} | {c[2]:.1f} / {b[3]:.1f} |")
print()
for l in 'ABC':
    for pat in ('mount-0', 'mount-4000', 'mountcrash-4000'):
        print(l, pat, [[m['wall_ms'] for m in json.load(open(f'{S}/{pat}-{l}-r{r}.json'))['mounts']] for r in (1, 2, 3)])
