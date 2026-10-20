import json, sys
for f in sys.argv[1:]:
    d = json.load(open(f"/home/cdkbs/bench-runs/bo-close/out/{f}.json"))
    print(f, len(d["runs"]))
    for i, r in enumerate(d["runs"]):
        hs = [a["host"] for a in r["arms"]]
        print(" run", i, r["host_before"].get("loadavg"), "competing", {str(h.get("competing")) for h in hs},
              "clean", all(h.get("clean", True) for h in hs), "max attempts", max(h.get("attempts", 1) for h in hs),
              "max waited", max(h.get("quiet_waited_s", 0) for h in hs), "wait_pct max", max(h.get("wait_pct", 0) for h in hs))
