import json,glob,re,math
from fractions import Fraction
def nr(xs,p):
    r=max(1,math.ceil(Fraction(str(p))*len(xs)/100))-1; return xs[r]
R=re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/'
for cell in ('c1-segregated-no0','c1-batch0','c1-spread'):
  for run in sorted(glob.glob(B+cell+'/r*')):
    d=json.load(open(glob.glob(run+'/bl-mixed-*.json')[0]))
    rolls=[(int(m[3]),int(m[4])) for l in open(glob.glob(run+'/*.s.log')[0],errors='replace') for m in [R.search(l)] if m]
    rd=[c for c in d['clients'] if c['role']=='reader']
    n=sum(c['ok']+c['errors']+c['refusals'] for c in rd)
    slow=[x for c in rd for x in c['slow']]
    out=[]
    for pad in (0,100e6,300e6):
        keep=[l for a,l in slow if not any(a<e+pad and a+l>s-pad for s,e in rolls)]
        xs=[0]*(n-len(keep))+sorted(keep)
        out.append((pad/1e6, round(nr(xs,99)/1e6,2), round(nr(xs,99.9)/1e6,1)))
    print(cell, run[-2:], out)
