import json,glob,statistics as S
D='/home/ukjin/bench-runs/eg-close-v3.5.1-33-g01bd189f4/json/'
def ld(c,arm):
    out=[]
    for r in range(1,6):
        try: out.append(json.load(open(f'{D}{c}-r{r}-{arm}.json')))
        except FileNotFoundError: pass
    return out
def spread(v): m=S.median(v); return (max(v)-min(v))/m*100
def verdict(o,n,higher_better=False):
    mo,mn=S.median(o),S.median(n); d=(mn-mo)/mo*100
    so,sn=spread(o),spread(n); sp=max(so,sn)
    if sp>20: v='no claim (spread >20%)'
    elif abs(d)<sp: v='inside the spread'
    else: v='outside the spread'
    return mo,mn,d,so,sn,v
def row(label,o,n,fmt='{:.1f}'):
    mo,mn,d,so,sn,v=verdict(o,n)
    return f"| {label} | {fmt.format(mo)} | {fmt.format(mn)} | {d:+.1f} % | {so:.1f} % / {sn:.1f} % | {v} |"
H="| metric | OLD median | NEW median | delta | spread OLD / NEW | verdict |\n|---|---|---|---|---|---|"
P=['p0_us','p25_us','p50_us','p95_us','p99_us']
out=[]
# cell 1
for cell in ('idle','after-load','recycled'):
    o=[ [c for c in j['cells'] if c['cell']==cell][0] for j in ld('c1','old')]
    n=[ [c for c in j['cells'] if c['cell']==cell][0] for j in ld('c1','new')]
    out.append(f"\n#### C1 `{cell}` ({len(o)}+{len(n)} rounds, 500 inserts each; rounds: OLD {len(o)}, NEW {len(n)})\n\n"+H)
    out.append(row('inserts/s (derived, 1e6/mean_us)',[1e6/c['mean_us'] for c in o],[1e6/c['mean_us'] for c in n],'{:.0f}'))
    out.append(row('mean_us',[c['mean_us'] for c in o],[c['mean_us'] for c in n]))
    for p in P: out.append(row(p,[c[p] for c in o],[c[p] for c in n]))
    out.append(row('server CPU us/insert',[c['server_cpu_us_per_op'] for c in o],[c['server_cpu_us_per_op'] for c in n],'{:.2f}'))
    wo=[(c['d_commit_wal_flush_count'],c['d_commit_wal_flush_us']) for c in o]; wn=[(c['d_commit_wal_flush_count'],c['d_commit_wal_flush_us']) for c in n]
    out.append(f"| d_commit_wal_flush_count per round | {[a for a,b in wo]} | {[a for a,b in wn]} | | | |")
    out.append(f"| d_commit_wal_flush_us per round | {[b for a,b in wo]} | {[b for a,b in wn]} | | | |")
    out.append(f"| booked us per wait (NEW) | | {[round(b/a,1) if a else None for a,b in wn]} | | | |")
    out.append(f"| d_wal_syncs per round | {[c['d_wal_syncs'] for c in o]} | {[c['d_wal_syncs'] for c in n]} | | | |")
    out.append(f"| per-round mean_us | {[round(c['mean_us'],1) for c in o]} | {[round(c['mean_us'],1) for c in n]} | | | |")
    out.append(f"| per-round server CPU us/insert | {[round(c['server_cpu_us_per_op'],2) for c in o]} | {[round(c['server_cpu_us_per_op'],2) for c in n]} | | | |")
def phases(c,names,tps=True):
    O=ld(c,'old');N=ld(c,'new')
    if tps:
        out.append("\n"+H); out.append(row('booking rate meta.tps (bookings/s)',[j['meta']['tps'] for j in O],[j['meta']['tps'] for j in N],'{:.0f}'))
        out.append(f"| per-round tps | {[round(j['meta']['tps']) for j in O]} | {[round(j['meta']['tps']) for j in N]} | | | |")
    for nm in names:
        g=lambda j:[p for p in j['phases'] if p['phase']==nm][0]
        o=[g(j) for j in O]; n=[g(j) for j in N]
        out.append(f"\n**phase `{nm}`** (ops/round {o[0]['ops']}; errors OLD {[p['errors'] for p in o]} NEW {[p['errors'] for p in n]})\n\n"+H)
        out.append(row('ops/s (derived, 1e6/mean_us)',[1e6/p['mean_us'] for p in o],[1e6/p['mean_us'] for p in n],'{:.0f}'))
        out.append(row('mean_us',[p['mean_us'] for p in o],[p['mean_us'] for p in n]))
        for p_ in P: out.append(row(p_,[p[p_] for p in o],[p[p_] for p in n]))
for c,names,tps in (('c2',['commit','freight-insert','cargo-lookup'],True),('c3',['commit','freight-insert','cargo-lookup'],True),('c4',['pk-user','loans-by-user','count-by-user'],False)):
    out.append(f"\n### {c}\n"); phases(c,names,tps)
    if c=='c3':
        out.append("\nverify failures / error replies per round (OLD | NEW): "+str([(sum(p['errors'] for p in a['phases']),b['meta']['verify'] if 'verify' in b['meta'] else None) for a,b in []]))
print('\n'.join(out))
