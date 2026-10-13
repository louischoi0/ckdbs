import json, sys
R = "/home/cdkbs/bench-runs/bt-close/out/"
for f in sys.argv[1:]:
    d = json.load(open(R + f + ".json"))
    for r in d["runs"]:
        u = sorted({(x.get("shape") or x.get("cell"), x["path"]) for x in r["arms"] if not x["host"].get("clean", False)})
        if u:
            print(f, "run", r["run"], u)
