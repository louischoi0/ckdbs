import json, glob, os, re, math, statistics as st
from fractions import Fraction
B='/home/cdkbs/bench-runs/bq/out-4cf30270'
def nr(xs,p):
    xs=sorted(xs); r=max(1,math.ceil(Fraction(str(p))*len(xs)/100))-1; return xs[r]
def load(path):
    j=glob.glob(path+'/bl-mixed-*.json'); assert len(j)==1,path
    d=json.load(open(j[0])); log=glob.glob(path+'/*.s.log')
    return d,(log[0] if log else None)
def nb_pct(d,role,q,allc=False):
    ins=[c for c in d['clients'] if c['role']=='inserter']
    insc={c['core'] for c in ins}
    cl=[c for c in d['clients'] if c['role']==role and (allc or c['core'] not in insc)]
    n=sum(c['ok']+c['errors']+c['refusals'] for c in cl)
    slow=sorted(s[1] for c in cl for s in c['slow'])
    assert all(c['slow_dropped']==0 for c in cl)
    rank=max(1,math.ceil(Fraction(str(q))*n/100))
    nfast=n-len(slow)
    if rank<=nfast: return None
    return round(slow[rank-nfast-1]/1000.0,1)
def runs(dur,cell):
    return [load(p) for p in sorted(glob.glob(f'{B}/bq/{dur}/{cell}/r*'))]
def med(x): return st.median(x)
out=[]
def P(*a): out.append(' '.join(str(x) for x in a))
for dur in ('relaxed','group'):
    P('=====',dur)
    for cell,iso,key,role in [('c1-segregated-no0','c1-iso-segregated-no0','read_from_schedule','reader'),
                     ('c1-batch0','c1-iso-batch0','read_from_schedule','reader'),
                     ('c1-spread','c1-iso-spread','read_from_schedule','reader'),
                     ('c2','c2-iso','writer_from_schedule','writer')]:
        M=runs(dur,cell); I=runs(dur,iso)
        for q,qk,lim in ((99,'p99_us',2),(99.9,'p99.9_us',5)):
            m=[d[key][qk] for d,_ in M]; i=[d[key][qk] for d,_ in I]
            mn=[nb_pct(d,role,q) for d,_ in M]; inb=[nb_pct(d,role,q) for d,_ in I]
            chk=[nb_pct(d,role,q,True) for d,_ in M]
            r=med(m)/med(i); lo=min(m)/max(i); hi=max(m)/min(i)
            P(cell,qk,'mixed',[round(x) for x in m],'iso',[round(x) for x in i],'ratio %.2f [%.2f-%.2f]'%(r,lo,hi),'PASS' if r<=lim else 'MISS','| NB mixed',mn,'NB iso',inb,'allchk',chk)
        P(cell,'moves_in_window',[d['moves_in_window'] for d,_ in M],'iso',[d['moves_in_window'] for d,_ in I],'ins core/core_end',[[(c['core'],c['core_end']) for c in d['clients'] if c['role']=='inserter'] for d,_ in M])
        if role=='writer':
            P(cell,'writer_service p50',[d['writer_service']['p50_us'] for d,_ in M],'iso',[d['writer_service']['p50_us'] for d,_ in I],'sched p50',[d['writer_from_schedule']['p50_us'] for d,_ in M])
    C=runs(dur,'c3')
    rr=[d['inserter_rows_per_s'] for d,_ in C]; lb=[d['log_bytes_per_s']/2**20 for d,_ in C]
    lb2=[d['meta_delta']['wal_append_lsn']/((d['m1_mono_ns']-d['m0_mono_ns'])/1e9)/2**20 for d,_ in C]
    P('C3 rows/s',rr,'mean %.0f spread %.0f (%.2f%%)'%(st.mean(rr),max(rr)-min(rr),100*(max(rr)-min(rr))/st.mean(rr)),'log MiB/s',[round(x,2) for x in lb],[round(x,2) for x in lb2],'mean %.2f'%st.mean(lb))
open('/home/cdkbs/bench-runs/bq/verdicts/BAR-r1/bar.out','w').write('\n'.join(out)+'\n'); print('\n'.join(out))
