import json, sys, os
out = sys.argv[1]
snaps = json.load(open(f"{out}/meta-snapshots.json"))
keys = ["plan_stats_generation", "plan_stats_relations", "plan_stats_posteriors", "plan_stats_part_drops", "plan_stats_seeds", "plan_stats_seed_pages",
        "plan_stats_publishes", "plan_stats_steward_us_max", "plan_memo_hits", "plan_memo_misses", "pool_resident", "pool_hits", "pool_misses", "pool_refused",
        "sched_system_consumed_us", "sched_system_polls", "sched_system_longest_poll_us"]
for srv in ("B", "A"):
    print("server", srv)
    print("| Snapshot | UTC | " + " | ".join(k.replace("plan_stats_", "ps_").replace("sched_system_", "sys_") for k in keys) + " | cpu ms |")
    print("|---|---|" + "---|" * (len(keys) + 1))
    prev = None
    for m in snaps:
        if m["server"] != srv: continue
        print(f"| {m['label']} | {m['t'][11:19]} | " + " | ".join(str(m["meta"].get(k)) for k in keys) + f" | {m['cpu_ns']/1e6:,.0f} |")
