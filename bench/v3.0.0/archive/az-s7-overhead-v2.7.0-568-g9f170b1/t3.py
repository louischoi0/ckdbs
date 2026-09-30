import json, statistics as st
R="/home/cdkbs/bench-runs/az-s7b"
runs=[json.load(open(f"{R}/c3-run{r}/result.json")) for r in (1,2,3,4,5)]
ks=runs[0]["ks"]
def q(v,p): v=sorted(v); return v[int(p*(len(v)-1))]
print("K | reps pooled | per-row median us: B fk, A fk, B plain, A plain | fk-plain: B, A | paired B-A per row: fk (IQR), plain (IQR) | p25 per row B fk / A fk")
tab={}
for k in ks:
    v={}
    for kind in ("fk","plain"):
        for l in "BA":
            v[(kind,l)]=[t/k for d in runs for t in d["cells"][l][f"{kind}-K{k}"]["txn_us"]]
    pd={}
    for kind in ("fk","plain"):
        pd[kind]=[b/1-a/1 for d in runs for b,a in zip([t/k for t in d["cells"]["B"][f"{kind}-K{k}"]["txn_us"]],[t/k for t in d["cells"]["A"][f"{kind}-K{k}"]["txn_us"]])]
    m={x:st.median(y) for x,y in v.items()}
    tab[k]=(m,pd)
    errs=sum(d["cells"][l][f"{kind}-K{k}"]["errors"] for d in runs for l in "BA" for kind in ("fk","plain"))
    print(f"K={k:<6} n={len(v[('fk','B')])} | {m[('fk','B')]:.1f} {m[('fk','A')]:.1f} {m[('plain','B')]:.1f} {m[('plain','A')]:.1f} | {m[('fk','B')]-m[('plain','B')]:+.1f} {m[('fk','A')]-m[('plain','A')]:+.1f} | fk {st.median(pd['fk']):+.2f} ({q(pd['fk'],.25):+.1f}..{q(pd['fk'],.75):+.1f}) plain {st.median(pd['plain']):+.2f} ({q(pd['plain'],.25):+.1f}..{q(pd['plain'],.75):+.1f}) | {q(v[('fk','B')],.25):.1f}/{q(v[('fk','A')],.25):.1f}  errors={errs}")
print("engine us/row (sum polled us / rows):")
for k in ks:
    line=f"K={k:<6}"
    for kind in ("fk","plain"):
        for l in "BA":
            us=sum(d["cells"][l][f"{kind}-K{k}"]["engine_us"] for d in runs); rows=sum(len(d["cells"][l][f"{kind}-K{k}"]["txn_us"])*k for d in runs)
            line+=f" {kind}-{l} {us/rows:.1f}"
    print(line)
print("fk stmt-by-position (8 octiles) K=16384, pooled runs, us:")
for kind in ("fk","plain"):
  for l in "BA":
    for k in (1024,4096,16384):
        acc=[[0,0] for _ in range(8)]
        for d in runs:
            for i,(s,n) in enumerate(d["cells"][l][f"{kind}-K{k}"]["stmt_us_by_pos"]): acc[i][0]+=s;acc[i][1]+=n
        print(kind,l,k," ".join(f"{s/n:.1f}" for s,n in acc))
