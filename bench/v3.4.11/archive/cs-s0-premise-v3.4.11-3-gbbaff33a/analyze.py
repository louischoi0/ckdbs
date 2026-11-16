import json,sys,collections
d=json.load(open(sys.argv[1]));cells=d["cells"]
g=collections.defaultdict(list)
for c in cells:g[(c["cores"],c["dur"],c["conns"],c["k"],c["arm"])].append(c)
def mean(x):return sum(x)/len(x)
print("cores dur conns k | W gps | A gps | A2 gps | W/A | spread(A,A2 all reps) | Wp50 Ap50 | Wp99 Ap99 | Wwal Awal | verdict")
for cores in (1,4):
 for dur in ("strict","group","relaxed"):
  for conns in (1,8):
   for k in (1,4,16):
    key=lambda arm:g.get((cores,dur,conns,k,arm),[])
    if not key("W"):continue
    W,A,A2=[mean([c["gps"] for c in key(x)]) for x in("W","A","A2")]
    ctl=[c["gps"] for c in key("A")+key("A2")]
    base=mean(ctl);spread=(max(ctl)-min(ctl))/base
    delta=(W-base)/base
    p=lambda arm,f:mean([c["lat"][f] for c in key(arm)])
    wal=lambda arm:mean([c["wal_bytes_per_group"] for c in key(arm)])
    v="W costlier" if -delta>spread else ("inside noise" if abs(delta)<=spread else "W faster")
    print(f"{cores} {dur} {conns} {k} | {W:.0f} | {A:.0f} | {A2:.0f} | {delta*100:+.1f}% | {spread*100:.1f}% | {p('W','p50_us'):.0f} {p('A','p50_us'):.0f} | {p('W','p99_us'):.0f} {p('A','p99_us'):.0f} | {wal('W'):.0f} {wal('A'):.0f} | {v}")
for p in d["probes"]:print(p)
errs=[c["err"] for c in cells if c["err"]];print("errors:",errs[:3])
