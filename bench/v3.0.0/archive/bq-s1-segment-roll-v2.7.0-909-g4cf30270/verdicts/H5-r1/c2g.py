import json,glob,re,sys
run=sys.argv[1]
d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
rolls=[(int(m[3]),int(m[4])) for l in open(glob.glob(run+'/*.s.log')[0],errors='replace') for m in [R.search(l)] if m]
w0,w1=d['m0_mono_ns'],d['m1_mono_ns']
rolls=[r for r in rolls if r[1]>w0 and r[0]<w1]
print('writer_rate',d.get('writer_rate'),'writers',d.get('writers'),'window s',(w1-w0)/1e9)
for c in d['clients']:
    if c['role']!='writer': continue
    s=c['slow']; lat=sorted(x[1] for x in s)
    print(c['index'],c['core'],c['core_end'],'ok',c['ok'],'slow',len(s),'p50 slow',lat[len(lat)//2]/1e6,'max',lat[-1]/1e6,
          'first due-w0 s',(s[0][0]-w0)/1e9,'last',(s[-1][0]-w0)/1e9)
# overlap fraction
big=[x for c in d['clients'] if c['role']=='writer' for x in c['slow'] if x[1]>=10e6]
ov=sum(1 for a,l in big if any(a<e and a+l>s for s,e in rolls))
print('big',len(big),'ov',ov, 'rolls',len(rolls), 'roll start-w0 s', [(round((s-w0)/1e9,2)) for s,e in rolls][:8])
# time series: for writer 0, latency vs due
c=[c for c in d['clients'] if c['role']=='writer'][0]
for a,l in c['slow'][:2000:100]: print(round((a-w0)/1e9,3), round(l/1e6,1))
