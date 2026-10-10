import glob, gzip, json, sys
D, rows = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(f"{D}/c*-n{rows}-r*.raw.json.gz")):
    r = json.load(gzip.open(f, "rt"))
    out = []
    for L in "AB":
        for a in ("lit-point", "lit-range", "insert", "select-pk", "delete-pk", "ping"):
            v = r["ab"][L][a]
            big = [x for x in v if x > 5000]
            if big:
                out.append(f"{L}:{a} n>5ms={len(big)} max={max(big) / 1000:.0f}ms")
    print(f.split("/")[-1][:14], "; ".join(out))
