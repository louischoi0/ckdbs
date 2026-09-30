import json, statistics as st
R="/home/cdkbs/bench-runs/az-s7"
runs=[json.load(open(f"{R}/c2-run{r}/result.json")) for r in range(1,6)]
arms=[a for a in runs[0]["arms"]["B"] if not a.endswith(":stmt")]
print("arm | clean pairs/40 | B-A p50 median (IQR) | B-A engine/op median | B abs p50 | A abs p50 | slow blocks B/A")
res={}
for a in arms:
    thrs={l:1.12*min(bb["p50_us"] for d in runs for bb in d["blocks"][l][a]) for l in "AB"}
    dd=[];de=[];bv=[];av=[];slowB=slowA=0;ddm=[]
    for d in runs:
        for bb,xx in zip(d["blocks"]["B"][a],d["blocks"]["A"][a]):
            sb,sa=bb["p50_us"]>thrs["B"],xx["p50_us"]>thrs["A"]
            slowB+=sb;slowA+=sa
            if not sb and not sa:
                dd.append(bb["p50_us"]-xx["p50_us"]);de.append(bb["engine_us_per_op"]-xx["engine_us_per_op"]);ddm.append(bb["mean_us"]-xx["mean_us"])
                bv.append(bb["p50_us"]);av.append(xx["p50_us"])
    s=sorted(dd)
    res[a]=(len(dd),st.median(dd),s[len(s)//4],s[3*len(s)//4],st.median(de),st.median(bv),st.median(av),slowB,slowA,st.median(ddm))
    print(f"{a:<26}| {len(dd)} | {st.median(dd):+.2f} ({s[len(s)//4]:+.2f}..{s[3*len(s)//4]:+.2f}) | {st.median(de):+.2f} | {st.median(bv):.1f} | {st.median(av):.1f} | {slowB}/{slowA} | meandiff {st.median(ddm):+.2f}")
# noise floor inside a server: child-update vs child-update-again, within same server same block, clean
print("floor: child-update vs child-update-again, same server, clean blocks")
for l in "BA":
    d_=[]
    for d in runs:
        for x,y in zip(d["blocks"][l]["child-update"],d["blocks"][l]["child-update-again"]):
            if x["p50_us"]<60 and y["p50_us"]<60: d_.append(x["p50_us"]-y["p50_us"])
    s=sorted(d_); print(l,len(d_),f"median {st.median(d_):+.2f} IQR {s[len(s)//4]:+.2f}..{s[3*len(s)//4]:+.2f} abs-median {st.median([abs(v) for v in d_]):.2f}")
print("whole-arm qps (5-run median) B vs A")
for a in arms:
    qb=st.median([d["arms"]["B"][a]["qps"] for d in runs]);qa=st.median([d["arms"]["A"][a]["qps"] for d in runs])
    print(f"{a:<26} B {qb:.0f} A {qa:.0f}")
print("max latency per arm per run (ms) - stall check")
for a in arms:
    print(a,[ (round(d["arms"]["B"][a]["max_us"]/1000,1),round(d["arms"]["A"][a]["max_us"]/1000,1)) for d in runs])
