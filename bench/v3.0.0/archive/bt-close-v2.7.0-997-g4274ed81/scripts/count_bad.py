import json
R = "/home/cdkbs/bench-runs/bt-close/out/"
tot_cells = tot_bad = tot_runs = 0
for f in ("p1a", "p1b", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9"):
    d = json.load(open(R + f + ".json"))
    cells = sum(len(r["arms"]) for r in d["runs"])
    bad = sum(x["bad"] for r in d["runs"] for x in r["arms"])
    tot_cells += cells
    tot_bad += bad
    tot_runs += len(d["runs"])
    print(f, "runs", len(d["runs"]), "cells", cells, "bad", bad)
print("total runs", tot_runs, "cells", tot_cells, "bad", tot_bad)
