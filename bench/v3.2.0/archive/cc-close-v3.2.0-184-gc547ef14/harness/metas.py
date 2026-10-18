import json
for m in json.load(open("/home/cdkbs/bench-runs/cc-close/out/meta-snapshots.json")):
    if m["server"] == "B":
        print(m["label"], m["t"], {k.replace("plan_", "").replace("stats_", ""): v for k, v in m["meta"].items() if k.startswith("plan") or k.startswith("pool_")})
