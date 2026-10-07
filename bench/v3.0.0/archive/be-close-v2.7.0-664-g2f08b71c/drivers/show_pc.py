import json, sys
for arm in "BA":
    d = json.load(open(f"/home/cdkbs/bench-runs/be-close/pastcap/cap{sys.argv[1]}-{arm}.json"))
    print("=====", arm, "cap", d["cap"], "datafile MiB", round(d["datafile_mib"]), "idle rss", round(d["rss_idle_mib"], 1), "load", d["loadavg_start"], "->", d["loadavg_end"])
    for k in ("load_big2", "load_big4"):
        if k in d:
            L = d[k]
            sl = L["stmt_latency_us"]
            print(k, "rows", L["rows"], "s", L["elapsed_s"], "rows/s", L["rows_per_s"], "errs", L["errors"], "retried", L["refusals_retried"],
                  "stmt p0/25/50/90/99/max us", [round(sl[x]) for x in ("p0", "p25", "p50", "p90", "p99", "max")],
                  "rss min/max/last", [round(L["rss"][x]) for x in ("min_mib", "max_mib", "last_mib")], "hwm", round(L["vmhwm_mib"]), "samples", L["rss"]["samples"])
            print("   pool_end", {k2: v for k2, v in L["pool_end"].items() if v}, "refused", L["pool_end"].get("pool_refused"))
            print("   trace", L["rss_trace_mib"][:12])
    a = d["p2a_single_scan"]
    print("2a", a["seconds"], a["reply"], {k: v for k, v in a["pool_delta"].items() if v}, "resident_after", a["pool_resident_after"])
    for k in ("p2b_probes_pass1", "p2c_probes_pass2"):
        if k in d:
            p = d[k]; l = p["probe_latency_us"]
            print(k, "days", p["days"], "s", p["elapsed_s"], "errs", p["errors"], "lat p0/25/50/90/99/max us", [round(l[x]) for x in ("p0", "p25", "p50", "p90", "p99", "max")],
                  {k2: v for k2, v in p["pool_delta"].items() if v})
    if "p2c_hot_repeat" in d:
        h = d["p2c_hot_repeat"]
        for k in ("first_probe_us", "repeat_probe_us"):
            l = h[k]
            print("hot", k, "n", l["n"], [round(l[x]) for x in ("p0", "p25", "p50", "p90", "p99", "max")])
        print("hot pool", {k2: v for k2, v in h["pool_delta"].items() if v})
    print("rss_part2", d["rss_part2"])
    print("pool_after_part2", {k: v for k, v in d["pool_after_part2"].items() if v})
    if "part3" in d:
        p = d["part3"]
        for k in ("warm0", "warm1", "warm2", "before_scan", "after_scan", "after_scan_pass2"):
            l = p[k]["lat"]
            print("p3", k, "p50/p99", round(l["p50"], 1), round(l["p99"], 1), "sum_s", round(l["sum_s"], 4), {k2: v for k2, v in p[k]["pool_delta"].items() if k2 in ("pool_hits", "pool_misses", "pool_reclaimed_inline")})
        s = p["scan_4x"]
        print("p3 scan", s["seconds"], {k2: v for k2, v in s["pool_delta"].items() if v}, "resident_after", s["pool_resident_after"])
        print("p3 rss", p["rss"])
