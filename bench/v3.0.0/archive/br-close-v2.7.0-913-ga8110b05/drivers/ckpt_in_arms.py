import glob, json, statistics as st
R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05"
for cell in ("relaxed-c1", "relaxed-c4", "group-c1", "group-c4"):
    for rows in (200, 1000, 10000):
        v = []
        for f in sorted(glob.glob(f"{R}/out/m0/{cell}/n{rows}-r*.json")):
            d = json.load(open(f))
            for l in "AB":
                v.append(d["meta_end"][l].get("checkpoint_runs", 0))
        print(cell, rows, "checkpoint_runs at the end of a server-run: median", st.median(v), "max", max(v), "of", len(v), "server-runs; with >=1:", sum(1 for x in v if x >= 1))
