import json, statistics as st
R="/home/cdkbs/bench-runs/ay-s11"
runs=[json.load(open(f"{R}/c2-run{r}/result.json")) for r in range(1,6)]
arms=list(runs[0]["arms"]["B"])
arms=[a for a in arms if not a.endswith(":stmt")]
print("per-block p50 (us) values, all runs, by server, for one arm to view modes:")
for a in ("child-update","ping"):
    for l in "BA":
        print(a,l,[round(b["p50_us"]) for d in runs for b in d["blocks"][l][a]])
print()
def q(v,p): v=sorted(v); return v[min(len(v)-1,int(p*len(v)))]
for a in arms:
    pb=[];pe=[];pm=[]
    lowB=[];lowA=[];hB=[];hA=[]
    perrun=[]
    for d in runs:
        b=d["blocks"]["B"][a]; x=d["blocks"]["A"][a]
        dd=[bb["p50_us"]-xx["p50_us"] for bb,xx in zip(b,x)]
        pb+=dd
        pm+=[bb["mean_us"]-xx["mean_us"] for bb,xx in zip(b,x)]
        pe+=[bb["engine_us_per_op"]-xx["engine_us_per_op"] for bb,xx in zip(b,x)]
        perrun.append(st.median(dd))
        lowB+=[bb["p50_us"] for bb in b]; lowA+=[xx["p50_us"] for xx in x]
    print(f"{a:<26} paired-block p50 B-A: median {st.median(pb):+.2f} (per-run medians {[round(v,1) for v in perrun]}) IQR {q(pb,.25):+.1f}..{q(pb,.75):+.1f}; "
          f"min-block p50 B {min(lowB):.1f} A {min(lowA):.1f} | q25-of-blocks B {q(lowB,.25):.1f} A {q(lowA,.25):.1f} | engine/op paired median {st.median(pe):+.2f}; mean-diff median {st.median(pm):+.2f}")
