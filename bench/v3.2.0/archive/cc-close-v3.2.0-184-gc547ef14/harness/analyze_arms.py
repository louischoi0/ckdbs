import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import render as R

out, phase = sys.argv[1], sys.argv[2]
g = R.groups(R.load(out, phase))
arms = sorted({a for c in g.values() for a in c})
t, S = R.dist_table(g, arms)
print(t)
print()
for cell in g:
    base = S[(cell, "A")]["cpu"]
    print(cell, {a: round(R.d(base, S[(cell, a)]["cpu"]), 1) for a in arms if a != "A"}, "spread", {a: round(S[(cell, a)]["spread"], 1) for a in arms})
