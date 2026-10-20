import json, sys
P = "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/plans/"
for t in sys.argv[1:]:
    d = json.load(open(P + t + ".json"))
    print("=====", t)
    for k, v in d.items():
        if isinstance(v, dict):
            print("--", k, "\n", v["sql"], "\n", v["analyze"])
        elif k in ("join-no-literal", "exists-correlated", "join-loan-user"):
            print("--", k, "\n", v)
