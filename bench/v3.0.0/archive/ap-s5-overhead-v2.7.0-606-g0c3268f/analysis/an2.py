import statistics as st
from an import *
print("### scan per-run paired p50 diff (us), arms scan-reject / again, and p25")
for n in (1000,10000,60000):
    runs=load('scan',n)
    d1=[r['arms']['B']['scan-reject']['p50_us']-r['arms']['A']['scan-reject']['p50_us'] for r in runs]
    d2=[r['arms']['B']['scan-reject-again']['p50_us']-r['arms']['A']['scan-reject-again']['p50_us'] for r in runs]
    pool=[(a+b)/2 for a,b in zip(d1,d2)]
    pl=d1+d2
    p25=[r['arms']['B'][a]['p25_us']-r['arms']['A'][a]['p25_us'] for r in runs for a in ('scan-reject','scan-reject-again')]
    pos=sum(1 for x in pl if x>0)
    A=st.median([r['arms']['A'][a]['p50_us'] for r in runs for a in ('scan-reject','scan-reject-again')])
    print(n,"pooled30 median",round(st.median(pl),1),"IQR",round(q(pl,.25),1),round(q(pl,.75),1),"pos",pos,"/30","p25 d",round(st.median(p25),1),"A p50",A,"ns/row",round(st.median(pl)*1000/n,2), "mean of run-pairs", round(st.mean(pool),1))
    print("   d1",[round(x) for x in d1]); print("   d2",[round(x) for x in d2])
print("### pooled over A/B all arms ping control per run p50 diff")
for cell,ns in (('point',(200,1000,10000)),('scan',(1000,10000,60000))):
    for n in ns:
        runs=load(cell,n)
        d=[r['arms']['B']['ping']['p50_us']-r['arms']['A']['ping']['p50_us'] for r in runs]
        print(cell,n,"ping d median",round(st.median(d),2),"IQR",round(q(d,.25),2),round(q(d,.75),2),"min",round(min(d),1),"max",round(max(d),1))
