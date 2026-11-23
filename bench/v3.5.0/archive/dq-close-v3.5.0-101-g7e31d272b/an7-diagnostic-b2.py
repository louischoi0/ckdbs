import json, glob
from statistics import median as med
D="/home/louis/dqbench/close/"
cfg=["A","Ar","B2","B20"]
data={c:{} for c in cfg}   # cfg -> stmt -> list of (cpu,wall) over all passes/rounds
perpass={c:{} for c in cfg}
extra={}
for c in cfg:
    for p in (1,2,3):
        f=D+f"m2e-{c}-{p}.json"
        for cell in json.load(open(f))["cells"]:
            if cell["arm"]!="A": continue
            for s,st in enumerate(cell["stmts"]):
                if "refused" in st: continue
                data[c].setdefault(s+1,[]).append((st["cpu_us"],st["wall_us"]))
                perpass[c].setdefault((s+1,p),[]).append(st["cpu_us"])
                extra.setdefault((c,s+1),[]).append((st["cabin_hits"],st["cabin_misses"],st["cabin_recorded"],st["inner_built"]))
def q(xs,p):
    xs=sorted(xs); return xs[int(p*(len(xs)-1))]
for s in (1,2,3,4,5):
    print("stmt",s)
    for c in cfg:
        v=data[c][s]; cp=[x[0] for x in v]; w=[x[1] for x in v]
        print(f"  {c:3} n={len(v)} cpu med {med(cp):9.0f} p25 {q(cp,.25):9.0f} p75 {q(cp,.75):9.0f} min {min(cp):9.0f} max {max(cp):9.0f} | wall med {med(w):9.0f} p25 {q(w,.25):9.0f} p75 {q(w,.75):9.0f}  | pass medians", [round(med(perpass[c][(s,p)])) for p in (1,2,3)], "ctr", med([e[0] for e in extra[(c,s)]]),med([e[1] for e in extra[(c,s)]]),med([e[2] for e in extra[(c,s)]]),med([e[3] for e in extra[(c,s)]]))
