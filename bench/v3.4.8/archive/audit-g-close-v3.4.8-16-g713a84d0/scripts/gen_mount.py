import json, statistics as st, math
S = '/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad'


def pct(q, p):
    q = sorted(q)
    k = max(0, min(len(q) - 1, math.ceil(p / 100 * len(q)) - 1))
    return q[k]


def line(name, b, w, an=None, ck=None):
    print(f"| {name} | {b} | {len(w)} | {min(w):.1f} | {pct(w,25):.1f} | {st.median(w):.1f} | {pct(w,95):.1f} | {pct(w,99):.1f} | {st.mean(w):.1f} |" +
          (f" {st.mean(an):.1f} | {st.mean(ck):.1f} |" if an is not None else ' - | - |'))


print('| cell | build | mounts | p0 | p25 | p50 | p95 | p99 (= max at n=21) | mean | `recovery_analysis` mean | `recovery_checkpoint` mean |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for name, pat, first in (('empty log (rows 0)', 'mount-{l}-0-r{r}', None),
                         ('4,000 rows, clean stop', 'mount-{l}-4000-r{r}', None),
                         ('4,000 rows, SIGKILL: first mount (replays about 29.4k records)', 'mountcrash-{l}-r{r}', 'first'),
                         ('4,000 rows, SIGKILL: mounts 2-7 of each run', 'mountcrash-{l}-r{r}', 'rest')):
    for l in 'AB':
        w = []; an = []; ck = []
        for r in (1, 2, 3):
            d = json.load(open(f"{S}/out/{pat.format(l=l, r=r)}.json"))
            ms = d['mounts']
            if first == 'first':
                ms = ms[:1]
            elif first == 'rest':
                ms = ms[1:]
            w += [m['wall_ms'] for m in ms]
            an += [m['recovery_analysis_us'] / 1000 for m in ms]
            ck += [m['recovery_checkpoint_us'] / 1000 for m in ms]
        line(name, l, w, an, ck)
