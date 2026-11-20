import json, collections
def load(p):
    d = json.load(open(p)); g = collections.OrderedDict()
    for c in d['cells']: g.setdefault((c['cell'], c['arm']), []).append(c)
    return d, g
dB, gB = load('s4.json')
dA, gA = load('/home/louis/ckdbs/.claude/worktrees/cv-ordered-index-walk/bench/v3.4.15/archive/cv-s0-premise-v3.4.15-1-gf4138860/s0.json')
m = lambda l, f: sum(f(x) for x in l)/len(l)
print("| cell | arm | engine | n/rep | p0 | p25 | p50 | p75 | p99 | stmts/s | server CPU/stmt | load (per cell reps) |")
for k in gB:
    for lab, g in (('A (f4138860)', gA), ('B (7d11ba44)', gB)):
        if k not in g: continue
        l = g[k]
        print(f"| {k[0]} | {k[1]} | {lab} | {m(l,lambda x:x['lat']['n']):,.0f} | " + " | ".join(f"{m(l,lambda x:x['lat'][p]):,.1f} µs" for p in ('p0_us','p25_us','p50_us','p75_us','p99_us')) + f" | {m(l,lambda x:x['sps']):,.0f} stmts/s | {m(l,lambda x:x['server_cpu_us_per_stmt']):,.1f} µs | {', '.join(x['host']['loadavg'].split()[0] for x in l)}; cc1plus {','.join(x['host']['cc1plus'] for x in l)} |")
print(json.dumps(dB['checks'], indent=1)[:2500])
print(json.dumps(dB['plans'], indent=1)[:6000])
