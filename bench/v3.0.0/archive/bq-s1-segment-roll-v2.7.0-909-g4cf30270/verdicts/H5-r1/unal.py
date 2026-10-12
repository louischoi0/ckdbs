import json,glob,re,collections
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/'
for cell in ('c1-segregated-no0','c1-batch0','c1-spread'):
  for run in sorted(glob.glob(B+cell+'/r*')):
    d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
    rolls=[(int(m[3]),int(m[4])) for l in open(glob.glob(run+'/*.s.log')[0],errors='replace') for m in [R.search(l)] if m]
    rd=[c for c in d['clients'] if c['role']=='reader']
    h=collections.Counter(); tot=0
    for c in rd:
      for a,l in c['slow']:
        if l<10e6: continue
        ov=[(s,e) for s,e in rolls if a<e and a+l>s]
        if not ov: continue
        if any(e-5e6<=a+l<=e+30e6 for s,e in ov): continue
        tot+=1
        dl=min(((a+l-e)/1e6 for s,e in ov), key=abs)
        b='end<-5ms' if dl< -5 else ('30-100ms after' if dl<=100 else '>100ms after')
        h[b]+=1
    print(cell,run[-2:],tot,dict(h))
