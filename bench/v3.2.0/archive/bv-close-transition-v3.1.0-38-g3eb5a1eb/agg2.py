import json,statistics as st
d=json.load(open("run/final/out.json"))
R=len(d["runs"])
cell={}
for r in d["runs"]:
    for c in r["cells"]: cell[(c["rows"],c["shape"],c["path"],c["bin"],r["run"])]=c
keys=sorted({k[:3] for k in cell}, key=lambda k:(k[1]!="update",k[2]!="literal",k[0]))
arms=["A1","A2","B","BT"]
print("## server CPU us/stmt: mean over runs (stdev)  | client QPS (1e6/mean wall) | lat us median over runs p0 p25 p50 p95 p99")
out={}
for k in keys:
    row=[]
    for a in arms:
        s=[cell[k+(a,r)]["server_cpu_us"] for r in range(R)]
        w=[cell[k+(a,r)]["mean_us"] for r in range(R)]
        lat=[round(st.median(cell[k+(a,r)][p] for r in range(R)),1) for p in ("p0_us","p25_us","p50_us","p95_us","p99_us")]
        row.append((a,round(st.mean(s),2),round(st.pstdev(s),2),round(1e6/st.mean(w)),lat))
    print(k,row)
    def dl(x,y): return [ (cell[k+(x,r)]["server_cpu_us"]-cell[k+(y,r)]["server_cpu_us"]) for r in range(R)]
    ab=[ (cell[k+("B",r)]["server_cpu_us"]-(cell[k+("A1",r)]["server_cpu_us"]+cell[k+("A2",r)]["server_cpu_us"])/2) for r in range(R)]
    out[k]=(st.mean(dl("A2","A1")),st.pstdev(dl("A2","A1")),st.mean(ab),st.pstdev(ab),st.mean(dl("BT","B")),st.pstdev(dl("BT","B")))
    print("   floor A2-A1 %.2f±%.2f | B-Abar %.2f±%.2f | BT-B %.2f±%.2f"%out[k])
# pooled
import itertools
for shape in ("update","insert"):
    ks=[k for k in keys if k[1]==shape]
    pool=lambda i:st.mean(out[k][i] for k in ks)
    print(shape,"pooled over 6 cells x runs: floor %.2f, B-A %.2f, BT-B %.2f"%(pool(0),pool(2),pool(4)))
    # absolute means
    for a in arms: print("  ",a, round(st.mean(cell[k+(a,r)]["server_cpu_us"] for k in ks for r in range(R)),2))
print("bad",sum(c["bad"] for c in cell.values()), "ops/cell/run",d["runs"][0]["cells"][0]["ops"])
print(set(d["runs"][0]["checks"].items()) )
for r in d["runs"]: print(r["host_before"]["loadavg"], r["host_after"]["loadavg"], r["host_before"]["competing"], r["host_after"]["competing"], r["checks"]["B/g violating UPDATE"][:40])
print(max(c["host"]["loadavg"] for c in cell.values()))
