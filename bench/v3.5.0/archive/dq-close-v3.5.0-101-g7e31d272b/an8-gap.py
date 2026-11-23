import json, glob
from statistics import median as med
D="/home/louis/dqbench/close/"
cfg=["A0","B0","A15","B15"]
data={c:{} for c in cfg}; thr={c:{} for c in cfg}; pp={c:{} for c in cfg}
passes=sorted({int(f.split('-')[-1][:-5]) for f in glob.glob(D+"m2g-A0-*.json")})
for c in cfg:
    for p in passes:
        for cell in json.load(open(D+f"m2g-{c}-{p}.json"))["cells"]:
            if cell["arm"]!="A": continue
            for s,st in enumerate(cell["stmts"]):
                if "refused" in st: continue
                data[c].setdefault(s+1,[]).append((st["cpu_us"],st["wall_us"]))
                pp[c].setdefault((s+1,p),[]).append(st["cpu_us"])
                for k,v in st.get("threads_us",{}).items():
                    thr[c].setdefault((s+1,k),[]).append(v)
q=lambda xs,p: sorted(xs)[int(p*(len(xs)-1))]
print("passes",passes)
for s in (1,2,3,4,5):
    print("stmt",s)
    for c in cfg:
        v=data[c][s]; cp=[x[0] for x in v]; w=[x[1] for x in v]
        print(f"  {c:4} n={len(v)} cpu med {med(cp):9.0f} p25 {q(cp,.25):9.0f} p75 {q(cp,.75):9.0f} min {min(cp):9.0f} max {max(cp):9.0f} | wall med {med(w):9.0f} p25 {q(w,.25):9.0f} p75 {q(w,.75):9.0f} | pass med", [round(med(pp[c][(s,p)])) for p in passes])
print("threads for #3 and #2 (median us by comm)")
for s in (2,3):
    for c in cfg:
        print(" ",s,c,{k:round(med(v),1) for (ss,k),v in thr[c].items() if ss==s})
