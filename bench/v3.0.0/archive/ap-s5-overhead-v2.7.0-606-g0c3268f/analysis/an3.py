import an, statistics as st
from an import *
an.PFX='pin-'
for cell,ns in (('point',(200,1000,10000)),('scan',(1000,10000,60000))):
    for n in ns:
        runs=an.load(cell,n)
        print(f"## PINNED {cell} rows={n} runs={len(runs)}")
        for a in runs[0]['arms']['B']:
            d=[r['arms']['B'][a]['p50_us']-r['arms']['A'][a]['p50_us'] for r in runs]
            b=st.median([r['arms']['B'][a]['p50_us'] for r in runs]); aa=st.median([r['arms']['A'][a]['p50_us'] for r in runs])
            bd=[x['p50_us']-y['p50_us'] for r in runs for x,y in zip(r['blocks']['B'][a],r['blocks']['A'][a])]
            pos=sum(1 for x in d if x>0)
            print(f" {a:<18} B {b:.1f} A {aa:.1f} d {st.median(d):+.2f} [{q(d,.25):+.2f},{q(d,.75):+.2f}] min {min(d):+.1f} max {max(d):+.1f} pos {pos}/{len(d)} | blk d {st.median(bd):+.2f} [{q(bd,.25):+.2f},{q(bd,.75):+.2f}]")
