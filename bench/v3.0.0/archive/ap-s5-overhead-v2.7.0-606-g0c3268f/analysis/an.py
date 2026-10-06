import json,glob,statistics as st,sys,re
R='/home/cdkbs/bench-runs/ap-s5'
PFX=''
def q(v,p):
    v=sorted(v); 
    if len(v)==1: return v[0]
    k=(len(v)-1)*p; f=int(k); c=min(f+1,len(v)-1); return v[f]+(v[c]-v[f])*(k-f)
def load(cell,n):
    out=[]
    for f in sorted(glob.glob(f'{R}/{PFX}{cell}-n{n}-run*/result.json'), key=lambda x:int(re.search(r'run(\d+)',x).group(1))):
        out.append(json.load(open(f)))
    return out
def fmt(v): return f"{v:+.2f}"
def table(cell,n,metrics=('p50_us','p90_us','p99_us','mean_us')):
    runs=load(cell,n)
    arms=list(runs[0]['arms']['B'].keys())
    print(f"## {cell} rows={n} runs={len(runs)}")
    for a in arms:
        row=[]
        for m in metrics:
            d=[r['arms']['B'][a][m]-r['arms']['A'][a][m] for r in runs]
            b=[r['arms']['B'][a][m] for r in runs]; aa=[r['arms']['A'][a][m] for r in runs]
            row.append(f"{m[:-3]} B {st.median(b):.1f} A {st.median(aa):.1f} d {fmt(st.median(d))} [{fmt(q(d,.25))},{fmt(q(d,.75))}]")
        # per block p50 diffs
        bd=[]
        for r in runs:
            for x,y in zip(r['blocks']['B'][a],r['blocks']['A'][a]): bd.append(x['p50_us']-y['p50_us'])
        eng=[ (r['engine']['B'][a]['us']/max(1,r['engine']['B'][a]['polls'])) - (r['engine']['A'][a]['us']/max(1,r['engine']['A'][a]['polls'])) for r in runs]
        print(f" {a:<18}"+" | ".join(row)+f" | blkp50 d {fmt(st.median(bd))} [{fmt(q(bd,.25))},{fmt(q(bd,.75))}] | eng/poll d {fmt(st.median(eng))}")
    return runs
if __name__=='__main__':
    for cell,ns in (('point',(200,1000,10000)),('scan',(1000,10000,60000))):
        for n in ns: table(cell,n)
