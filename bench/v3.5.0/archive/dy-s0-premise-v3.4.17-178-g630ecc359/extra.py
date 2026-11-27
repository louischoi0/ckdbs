import glob, re, math
KV = re.compile(r'(\w+)=([\w.]+)')
for dur in ('relaxed','group','strict'):
    rs=[]
    for f in glob.glob(f'/home/ukjin/bench-runs/dy-s0/out/{dur}-mixed-r*/*.s.log'):
        for l in open(f):
            if '[wal] roll' in l:
                rs.append({k:(int(v) if v.isdigit() else v) for k,v in KV.findall(l)})
    n=len(rs); print(dur,'all rolls in log (incl warmup/setup)',n,'hold<10ms',sum(r['hold_us']<10000 for r in rs),'hold>=100ms',sum(r['hold_us']>=100000 for r in rs),'hold>=10ms',sum(r['hold_us']>=10000 for r in rs))
    big=[r for r in rs if r['hold_us']>=10000]
    if big:
        print('  >=10ms: gate share', sum(r['sync_gate_us'] for r in big)/sum(r['hold_us'] for r in big), 'rolls with gate>=hold/2', sum(r['sync_gate_us']>=r['hold_us']/2 for r in big),'of',len(big))
        print('  non-gate part of hold p50/max us', sorted(r['hold_us']-r['sync_gate_us'] for r in big)[len(big)//2], max(r['hold_us']-r['sync_gate_us'] for r in big))
        print('  hold minus gate minus own sync minus header/dirsync (R1-like + rename) max', max(r['hold_us']-r['sync_gate_us']-r['sync_us']-r['header_us']-r['dirsync_us'] for r in big))
    # what would the hold be with only sync_gate removed
    xs=sorted(r['hold_us']-r['sync_gate_us'] for r in rs); print('  hold - gate: p50',xs[n//2],'p99',xs[min(n-1,math.ceil(.99*n)-1)],'max',xs[-1])
