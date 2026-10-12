import json,glob,re,sys
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/'
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
for cell in ('c1-segregated-no0','c1-batch0','c1-spread'):
  for run in sorted(glob.glob(B+cell+'/r*')):
    d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
    rolls=[(int(m[2]),int(m[3]),int(m[4])) for l in open(glob.glob(run+'/*.s.log')[0],errors='replace') for m in [R.search(l)] if m]
    w0,w1=d['m0_mono_ns'],d['m1_mono_ns']
    rolls=[r for r in rolls if r[2]>w0 and r[1]<w1]
    rd=[c for c in d['clients'] if c['role']=='reader']
    ins=[c for c in d['clients'] if c['role']=='inserter'][0]
    ends=[c['core_end'] for c in rd]
    # per roll: number of distinct readers with a >=10ms sample overlapping the roll
    per=[]; 
    for core,s,e in rolls:
        n=0; n1=0
        for c in rd:
            hit=[x for x in c['slow'] if x[0]<e and x[0]+x[1]>s and x[1]>=10e6]
            if hit: n+=1
            if any(x[0]<e and x[0]+x[1]>s for x in c['slow']): n1+=1
        per.append((core,n,n1))
    # completion within 0..5ms after roll end among overlapping >=10ms samples
    ov=[];w5=0
    for c in rd:
        for a,l in c['slow']:
            if l<10e6: continue
            for core,s,e in rolls:
                if a<e and a+l>s:
                    ov.append(1)
                    if 0<=a+l-e<=5e6: w5+=1
                    break
    m0,m1=d['meta_m0'],d['meta_m1']
    so=[(k,(m1.get(k) or 0)-(m0.get(k) or 0)) for k in m1 if 'over_100ms' in k]
    print(cell,run[-2:],'readers end cores',ends,'ins core',ins['core'],'->',ins['core_end'],'rollers',sorted(set(r[0] for r in rolls)))
    print('   per roll (roller, readers>=10ms, readers any):',per)
    print('   within0-5ms of end: %d/%d=%.0f%%'%(w5,len(ov),100*w5/max(1,len(ov))), so)
