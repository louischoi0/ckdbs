import json, glob, statistics
R = "/home/cdkbs/bench-runs/be-close"
for d in ("overhead", "warmscan"):
    comp = set(); lo = 9; hi = 0; waits = []; ck = []
    for f in glob.glob(f"{R}/{d}/*.json"):
        j = json.load(open(f))
        for k in ("host_before", "host_after"):
            comp |= set(j[k]["competing"])
            l = float(j[k]["loadavg"].split()[0]); lo = min(lo, l); hi = max(hi, l)
        waits.append(j["wait_s"])
        ck.append(j["mount_ckpt_us"])
    print(d, "runs", len(waits), "competing", comp, "load1 range", lo, hi, "max wait", max(waits))
    print(" ckpt_us median A/B", statistics.median(c["A"] for c in ck), statistics.median(c["B"] for c in ck), "max", max(c["A"] for c in ck), max(c["B"] for c in ck))
