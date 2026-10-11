import json, statistics
R = "/home/cdkbs/bench-runs/at"
for n in (1000, 10000):
    sh = {"A": [], "B": []}
    for r in range(1, 9):
        d = json.load(open(f"{R}/res/n{n}-run{r}.json"))
        for l in "AB":
            lat = d["create_lat_us"][l]
            # drop the 2 slot-B outliers? keep all; report both
            srt = sorted(lat)
            core = srt[:-2]
            sh[l].append((sum(lat), d["create_engine"][l]["us"], sum(core), len(lat)))
    for l in "AB":
        tot = statistics.median([e / c for c, e, _, _ in sh[l]])
        print(n, l, "engine/client sum median", round(tot, 3),
              "client mean", round(statistics.median([c / k for c, _, _, k in sh[l]]), 1),
              "engine per create", round(statistics.median([e / k for c, e, _, k in sh[l]]), 1))
