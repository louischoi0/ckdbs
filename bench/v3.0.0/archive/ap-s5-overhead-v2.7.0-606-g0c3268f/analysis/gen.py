import an, statistics as st
from an import q
def med(x): return st.median(x)
def series(pfx,cell,n):
    an.PFX=pfx; return an.load(cell,n)
def qps(r,l,a): return r['arms'][l][a]['qps']
def row_delta(runs,a,m='p50_us'):
    d=[r['arms']['B'][a][m]-r['arms']['A'][a][m] for r in runs]
    return med(d),q(d,.25),q(d,.75),sum(1 for x in d if x>0),len(d)
def fmtd(t): return f"{t[0]:+.2f} ({t[1]:+.2f} to {t[2]:+.2f})"
def shortlines(pfx,label):
    out=[]
    for cell,ns in (('point',(200,1000,10000)),('scan',(1000,10000,60000))):
        for n in ns:
            runs=series(pfx,cell,n)
            for a in runs[0]['arms']['B']:
                A=med([r['arms']['A'][a]['p50_us'] for r in runs]); B=med([r['arms']['B'][a]['p50_us'] for r in runs])
                qa=1e6/A; qb=1e6/B
                d=row_delta(runs,a); d90=row_delta(runs,a,'p90_us'); d99=row_delta(runs,a,'p99_us')
                pct=d[0]/A*100
                out.append(f"| {cell} | {n:,} | {a} | {len(runs)} | {qa:,.0f} | {qb:,.0f} | {A:,.1f} µs | {B:,.1f} µs | {d[0]:+.2f} µs ({d[1]:+.2f} to {d[2]:+.2f}) | {pct:+.2f} % | {d[3]}/{d[4]} |")
    return out
def dist(pfx,cell,n,arms):
    runs=series(pfx,cell,n); out=[]
    for a in arms:
        for l in ('A','B'):
            v=lambda m: med([r['arms'][l][a][m] for r in runs])
            ops=runs[0]['arms'][l][a]['ops']
            out.append(f"| {n:,} | {a} | {l} | {ops:,} | {v('p0_us'):,.1f} | {v('p25_us'):,.1f} | {v('p50_us'):,.1f} | {v('p90_us'):,.1f} | {v('p95_us'):,.1f} | {v('p99_us'):,.1f} |")
    return out
if __name__=='__main__':
    import sys
    pfx=sys.argv[1]
    print("\n".join(shortlines(pfx,pfx)))
