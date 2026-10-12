import json,glob,re,os,math,statistics as st,sys
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
pat=re.compile(r'\[wal\] roll (.*)')
def load(d):
    j=json.load(open(glob.glob(d+'/*.json')[0]))
    lo,hi=j['measure_from_mono_ns'],j['stop_at_mono_ns']
    rs=[];allr=0
    for line in open(glob.glob(d+'/*.s.log')[0]):
        m=pat.search(line)
        if not m: continue
        f={}
        for kv in m.group(1).split():
            k,v=kv.split('=');
            try: f[k]=int(v)
            except: f[k]=v
        allr+=1
        assert f['hold_us']==(f['end_ns']-f['start_ns'])//1000 or abs(f['hold_us']*1000-(f['end_ns']-f['start_ns']))<1000, f
        if f['start_ns']>=lo and f['end_ns']<=hi: rs.append(f)
    return rs,allr,j
def p99set(rs,q=0.99):
    h=sorted(r['hold_us'] for r in rs)
    if not h: return []
    k=max(1,math.ceil(q*len(h)))
    thr=h[k-1]
    return [r for r in rs if r['hold_us']>=thr]
def share(rs):
    G=sum(r['sync_gate_us'] for r in rs); S=sum(r['sync_us'] for r in rs)
    own=sum(r['sync_rest_us'] for r in rs)
    lo=hi=whole=0
    for r in rs:
        g=r['sync_gate_us']; wr=r['waited_sync_rest_us']; W=r['waited_sync_tail_us']+wr
        pre=max(0,W-g)
        lo+=max(0,wr-pre); hi+=min(wr,g); whole+=wr
    c=G+S
    if c==0: return None
    return dict(n=len(rs),stepc=c,own=100*own/c,lo=100*(own+lo)/c,hi=100*(own+hi)/c,whole=100*(own+whole)/c,gate=100*G/c,
                stepc_of_hold=100*c/sum(r['hold_us'] for r in rs))
out={}
for dur in ['relaxed','group']:
    for cell in sorted(os.listdir(f'{B}/{dur}')):
        runs=[]
        for rd in sorted(glob.glob(f'{B}/{dur}/{cell}/r*')):
            rs,allr,j=load(rd); runs.append((rd[-2:],rs,allr,j))
        pooled=[r for _,rs,_,_ in runs for r in rs]
        if not pooled: print(dur,cell,'no rolls'); continue
        P=p99set(pooled)
        s=share(P); a=share(pooled); p90=share(p99set(pooled,0.90))
        print(f"{dur:7} {cell:18} rolls={len(pooled):3} p99n={s['n']} lo={s['lo']:.1f} hi={s['hi']:.1f} whole={s['whole']:.1f} own={s['own']:.2f} gate%c={s['gate']:.1f} c%hold={s['stepc_of_hold']:.1f} | p90 lo-hi {p90['lo']:.1f}-{p90['hi']:.1f} | all lo-hi {a['lo']:.1f}-{a['hi']:.1f} whole {a['whole']:.1f}")
        for name,rs,allr,j in runs:
            if not rs: continue
            sp=share(p99set(rs)); ar=share(rs)
            top=p99set(rs)[0]
            print(f"     {name} n={len(rs)}/{allr} wal_rolls_delta={j['meta_delta'].get('wal_rolls')} p99 lo-hi {sp['lo']:.1f}-{sp['hi']:.1f} whole {sp['whole']:.1f} own {sp['own']:.2f} | all {ar['lo']:.1f}-{ar['hi']:.1f} | top hold={top['hold_us']} gate={top['sync_gate_us']} sync={top['sync_us']} rest={top['sync_rest_us']} segs={top['sync_segs']} ws={top['waited_syncs']} wt={top['waited_sync_tail_us']} wr={top['waited_sync_rest_us']} wsegs={top['waited_sync_segs']} maxsegs={max(r['sync_segs'] for r in rs)}")
