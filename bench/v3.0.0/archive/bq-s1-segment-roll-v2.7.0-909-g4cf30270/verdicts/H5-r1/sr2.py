import json,glob,re
run='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/c1-spread/r2'
d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
allr=[(int(m[1]),int(m[2]),int(m[3]),int(m[4]),int(m[5])) for l in open(glob.glob(run+'/*.s.log')[0],errors='replace') for m in [R.search(l)] if m]
w0,w1=d['m0_mono_ns'],d['m1_mono_ns']; mf=d['measure_from_mono_ns']; st=d['stop_at_mono_ns']
print('w0-mf ms',(w0-mf)/1e6,'w1-st ms',(w1-st)/1e6)
rd=[c for c in d['clients'] if c['role']=='reader']
for seg,core,s,e,h in allr:
    if e<=w0 or s>=w1: continue
    inside = s>=mf and e<=st
    hits=[]
    for c in rd:
        for a,l in c['slow']:
            if a<e and a+l>s: hits.append((c['index'],round((a-s)/1e6),round(l/1e6,1),round((a+l-e)/1e6,1)))
    print(seg,core,'hold',h//1000,'ms','inside',inside,'start-w0 s',round((s-w0)/1e9,2),hits[:8])
