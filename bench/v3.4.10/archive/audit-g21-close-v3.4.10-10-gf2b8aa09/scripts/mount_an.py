import json, sys, glob, statistics as st, math, re, collections
root = sys.argv[1]
def pct(q, p):
    q = sorted(q); return q[max(0, min(len(q)-1, math.ceil(p/100*len(q))-1))]
cells = collections.defaultdict(lambda: collections.defaultdict(list))
for p in sorted(glob.glob(root + '/mt-*.json')):
    m = re.match(r'mt-(\w+)-(\d+)-(A|B)-r(\d)\.json', p.split('/')[-1])
    name, rows, l, r = m.groups()
    ms = json.load(open(p))['mounts']
    cells[(name, rows)][l].append(ms)
print('| cell | build | mounts | p0 | p25 | p50 | p95 | p99 | mean | analysis mean | checkpoint mean |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
res = {}
for (name, rows), byl in cells.items():
    parts = [('all', None)] if 'crash' not in name else [('first mount', 'first'), ('mounts 2-7', 'rest')]
    for tag, which in parts:
        for l in ('A', 'B'):
            w = []; an = []; ck = []
            for ms in byl[l]:
                s = ms[:1] if which == 'first' else ms[1:] if which == 'rest' else ms
                w += [m['wall_ms'] for m in s]; an += [m['recovery_analysis_us']/1000 for m in s]; ck += [m['recovery_checkpoint_us']/1000 for m in s]
            if not w: continue
            res[(name, rows, tag, l)] = w
            print(f"| {name} {rows} {tag} | {l} | {len(w)} | {min(w):.1f} | {pct(w,25):.1f} | {st.median(w):.1f} | {pct(w,95):.1f} | {pct(w,99):.1f} | {st.mean(w):.1f} | {st.mean(an):.1f} | {st.mean(ck):.1f} |")
print()
for (name, rows, tag, l), w in res.items():
    if l != 'A' or (name, rows, tag, 'B') not in res: continue
    b = res[(name, rows, tag, 'B')]
    print(f"{name} {rows} {tag}: B-A mean {st.mean(b)-st.mean(w):+.1f} ms, p50 {st.median(b)-st.median(w):+.1f} ms; A range {min(w):.1f}-{max(w):.1f}")
