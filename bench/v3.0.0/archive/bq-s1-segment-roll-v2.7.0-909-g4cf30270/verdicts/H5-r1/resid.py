import json,glob,re
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
C=re.compile(r'\[checkpoint\] run: core=(\d+) .*?start_ns=(\d+) end_ns=(\d+)')
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/'
for cell in ('c1-segregated-no0','c1-batch0','c1-spread'):
  for run in sorted(glob.glob(B+cell+'/r*')):
    d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
    L=open(glob.glob(run+'/*.s.log')[0],errors='replace').read().splitlines()
    rolls=[(int(m[3]),int(m[4])) for l in L for m in [R.search(l)] if m]
    cps=[(int(m[2]),int(m[3])) for l in L for m in [C.search(l)] if m]
    w0,w1=d['m0_mono_ns'],d['m1_mono_ns']
    cpw=[(s,e) for s,e in cps if e>w0 and s<w1]
    rd=[c for c in d['clients'] if c['role']=='reader']
    res=[]
    for c in rd:
      for a,l in c['slow']:
        if l<10e6: continue
        if any(a<e and a+l>s and e-5e6<=a+l<=e+30e6 for s,e in rolls): continue
        res.append((a,l))
    big=[x for x in res if x[1]>=30e6]
    inc=sum(1 for a,l in big if any(a<e and a+l>s for s,e in cpw))
    print(cell,run[-2:],'cp runs',len(cpw),[round((e-s)/1e9,1) for s,e in cpw],'resid>=30ms',len(big),'in cp',inc, '%.0f%%'%(100*inc/max(1,len(big))))
