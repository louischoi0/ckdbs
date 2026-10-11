import glob, gzip, json, sys
D = sys.argv[1]
arm = sys.argv[2] if len(sys.argv) > 2 else "insert"
for f in sorted(glob.glob(f"{D}/diag-c1-n1000-r*.raw.json.gz")):
    r = json.load(gzip.open(f, "rt"))
    out = []
    for L in "AB":
        v = r["ab"][L][arm]
        idx = [(i, round(x / 1000)) for i, x in enumerate(v) if x > 20000]
        out.append(f"{L}: {idx}")
    print(f.split("/")[-1][:18], " | ".join(out))
