import json
from statistics import median as med
D="/home/louis/dqbench/close/"
m=json.load(open(D+"m1c.json"))["m1"]
sh=["a_point","b_build","c_cabin","d_tuplekey"]
n=len(m)
print("lifetimes",n)
for s in sh:
    c={x:[v for lt in m for v in m[lt][s]["cpu_us"][x]] for x in ["A","A2","B","B2"]}
    print(s,"medians",{x:round(med(v),2) for x,v in c.items()})
    for x in ["A2","B","B2"]:
        allb=[b-a for lt in m for a,b in zip(m[lt][s]["cpu_us"]["A"],m[lt][s]["cpu_us"][x])]
        lts=[med([b-a for a,b in zip(m[lt][s]["cpu_us"]["A"],m[lt][s]["cpu_us"][x])]) for lt in m]
        fl=[med([b-a for a,b in zip(m[lt][s]["cpu_us"]["A"],m[lt][s]["cpu_us"]["A2"])]) for lt in m]
        print(f"   {x}-A pooled {med(allb):+.2f} us ({100*med(allb)/med(c['A']):+.2f}%) lifetime medians {min(lts):+.2f}..{max(lts):+.2f} positive {sum(v>0 for v in lts)}/{n} inside floor range {sum(min(fl)<=v<=max(fl) for v in lts)}/{n}")
    b2b=[med([b-a for a,b in zip(m[lt][s]["cpu_us"]["B"],m[lt][s]["cpu_us"]["B2"])]) for lt in m]
    print(f"   B2-B lifetime median {med(b2b):+.2f} positive {sum(v>0 for v in b2b)}/{n}")
    for x in ["A","A2","B","B2"]:
        xs=sorted(v for lt in m for v in m[lt][s]["lat_sample_us"][x]); q=lambda p: xs[int(p*(len(xs)-1))]
        print(f"   lat {x} p0 {xs[0]:.0f} p25 {q(.25):.0f} p50 {q(.5):.0f} p95 {q(.95):.0f} p99 {q(.99):.0f}")
print("rows", {s:m['0'][s]['rows_per_stmt'] for s in sh}, "ops", m['0']['a_point']['lat']['A']['ops'])
print([m[lt][s]['quiet_hold_s'] for lt in m for s in sh if m[lt][s]['quiet_hold_s']])
