import json
for res in ("res", "res-swap"):
    for n in (1000, 10000):
        rows = []
        for r in range(1, 9):
            try:
                d = json.load(open(f"/home/cdkbs/bench-runs/at/{res}/n{n}-run{r}.json"))
            except OSError:
                continue
            ce = d["create_engine"]
            rows.append((r, {l: (ce[l]["polls"], round(ce[l]["us"])) for l in "AB"}))
        print(res, n, rows[:3])
