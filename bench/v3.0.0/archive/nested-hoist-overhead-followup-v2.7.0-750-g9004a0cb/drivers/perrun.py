import glob, json, re, statistics, sys
for d in sys.argv[1:]:
    print("==", d)
    rows = {}
    for f in sorted(glob.glob(f"{d}/n*-run*.json")):
        m = re.match(r".*/n(\d+)-run(\d+)\.json", f); n, r = int(m.group(1)), int(m.group(2))
        j = json.load(open(f))
        for arm in j["blocks"]["A"]:
            la = [x for b in j["blocks"]["A"][arm] for x in b["lat_us"]]
            lb = [x for b in j["blocks"]["B"][arm] for x in b["lat_us"]]
            rows.setdefault((n, arm), []).append((r, round(statistics.median(lb) - statistics.median(la), 1)))
    for k, v in sorted(rows.items()):
        print(k, " ".join(f"r{r}:{x:+}" for r, x in sorted(v)))
