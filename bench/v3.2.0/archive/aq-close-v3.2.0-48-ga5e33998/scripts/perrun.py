import json, sys
for f in sys.argv[1:]:
    runs = json.load(open(f"/home/cdkbs/bench-runs/aq-close/out/{f}.json"))["runs"]
    shapes = []
    for a in runs[0]["arms"]:
        if a["shape"] not in shapes:
            shapes.append(a["shape"])
    for sh in shapes:
        for path in ("literal", "prepared"):
            row = []
            for r in runs:
                d = {a["bin"]: a["server_cpu_us"] for a in r["arms"] if a["shape"] == sh and a["path"] == path}
                if not d:
                    continue
                row.append(" ".join("%s %.1f" % (k, v) for k, v in sorted(d.items())))
            print(f, sh, path, " | ".join(row))
