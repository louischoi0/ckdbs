import json, glob, re, sys
sys.path.insert(0,'/home/cdkbs/bench-runs/bq/verdicts/BAR-r1')
B='/home/cdkbs/bench-runs/bq/out-4cf30270'
from rolls import parse, cover
for dur in ('group','relaxed'):
  for rd in sorted(glob.glob(f'{B}/bq/{dur}/c2/r*')):
    d=json.load(open(glob.glob(rd+'/bl-mixed-*.json')[0])); log=glob.glob(rd+'/*.s.log')[0]
    rolls,cks=parse(log)
    w0,w1=d['measure_from_mono_ns'],d['stop_at_mono_ns']
    samp=[s for c in d['clients'] if c['role']=='writer' for s in c['slow'] if s[1]>=50_000_000]
    siv=[(a,a+l) for a,l in samp]
    riv=[(int(r['start_ns']),int(r['end_ns'])) for r in rolls if w0<=int(r['start_ns'])<=w1]
    print(dur,rd[-2:],'stall-union cover %.0f%%'%(100*cover(siv,w0,w1)),'n',len(samp),'rolls',len(riv))
    # for each roll, how many writer samples (any latency) are in flight
    for s,e in riv[:6]:
        inflight=[l for c in d['clients'] if c['role']=='writer' for a,l in c['slow'] if a<e and a+l>s]
        print('   roll',(s-w0)/1e6,'ms hold',(e-s)/1e3,'us; slow writer samples in flight',len(inflight), 'max lat ms',max(inflight)/1e6 if inflight else None)
    if dur=='group':
        # samples: earliest/latest due
        print('   sample due span ms',(min(a for a,_ in samp)-w0)/1e6,(max(a for a,_ in samp)-w0)/1e6)
