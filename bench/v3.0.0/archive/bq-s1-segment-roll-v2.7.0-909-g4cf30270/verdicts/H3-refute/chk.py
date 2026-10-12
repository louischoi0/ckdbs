import glob,json,re,os,math,sys
D='/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
rx=re.compile(r'\[wal\] roll (.*)')
def rolls(dur,cell):
    out=[]
    for rd in sorted(glob.glob(f'{D}/{dur}/{cell}/r*')):
        js=[f for f in glob.glob(rd+'/bl-mixed-*.json')]
        j=json.load(open(js[0]))
        a=j.get('measure_from_mono_ns'); b=j.get('stop_at_mono_ns')
        for f in glob.glob(rd+'/*.s.log'):
            for line in open(f):
                m=rx.search(line)
                if not m: continue
                d=dict(kv.split('=',1) for kv in m.group(1).split())
                r={k:(int(v) if v.isdigit() else v) for k,v in d.items()}
                if a and b and not (r['start_ns']>=a and r['end_ns']<=b): continue
                r['run']=os.path.basename(rd); out.append(r)
    return out
def share(rs):
    num_lo=num_hi=den=0
    for r in rs:
        G=r['sync_gate_us']; W=r['waited_sync_tail_us']+r['waited_sync_rest_us']
        pre=max(0,W-G); rest=r['waited_sync_rest_us']
        num_lo+=r['sync_rest_us']+max(0,rest-pre); num_hi+=r['sync_rest_us']+min(rest,G)
        den+=G+r['sync_us']
    return (100*num_lo/den,100*num_hi/den) if den else (float('nan'),)*2
def top(rs):
    h=sorted(r['hold_us'] for r in rs); 
    if not h: return []
    p=h[max(0,math.ceil(0.99*len(h))-1)]
    return [r for r in rs if r['hold_us']>=p]
for dur in ['relaxed','group']:
    allr=[]; c12=[]
    for cell in ['c1-batch0','c1-segregated-no0','c1-spread','c2','c3']:
        rs=rolls(dur,cell); allr+=rs
        if cell!='c3': c12+=rs
        t=top(rs)
        print(dur,cell,len(rs),'top',len(t),'share lo-hi %.1f-%.1f'%share(t),'all %.1f-%.1f'%share(rs))
    for name,s in [('pooled all cells',allr),('pooled C1+C2',c12)]:
        t=top(s); print(dur,name,len(s),'top',len(t),'%.1f-%.1f'%share(t), [ (r['run'],r['hold_us']) for r in t])
