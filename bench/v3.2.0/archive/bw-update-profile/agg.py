import json,glob,collections
def load(n,kind):
    tot=0;c=collections.Counter()
    per=[]
    for f in sorted(glob.glob(f'rep/{n}?.?.json')):
        d=json.load(open(f)); tot+=d['samples']; 
        m=dict(d[kind]); per.append((d['samples'],m)); 
        for k,v in m.items(): c[k]+=v
    return tot,c,per
import sys
kind=sys.argv[1]
tA,cA,pA=load('A',kind); tB,cB,pB=load('B',kind)
print(tA,tB)
keys=set(cA)|set(cB)
rows=[]
for k in keys:
    # per-sample share per dump -> ns/row? use fraction
    fa=[m.get(k,0)/s*100 for s,m in pA]; fb=[m.get(k,0)/s*100 for s,m in pB]
    rows.append((sum(fb)/len(fb)-sum(fa)/len(fa),k,fa,fb))
rows.sort(reverse=True)
for d,k,fa,fb in rows[:22]+rows[-6:]:
    print("%+6.2fpp A=%5.2f B=%5.2f  A[%s] B[%s] %s"%(d,sum(fa)/6,sum(fb)/6,' '.join('%.1f'%x for x in fa),' '.join('%.1f'%x for x in fb),k[:90]))
