#!/usr/bin/env python3
"""Summarize the AM-S1 A/B run: one row per cell, txn percentiles, META counters."""
import json, os, re, sys
R = sys.argv[1] if len(sys.argv) > 1 else "/home/cdkbs/bench-runs/am-s1-page-latch"
run = json.load(open(os.path.join(R, "run.json")))
print("describe:", run.get("describe_tree"), "| sha A:", run["sha256"]["A"][:12], "B:", run["sha256"]["B"][:12])
print("started:", run.get("started"), "finished:", run.get("finished"))
META_KEYS = ["wal_syncs", "wal_group_commits", "wal_group_batches", "wal_mean_group_batch",
             "recovery_checkpoint_us", "sched_wall_us", "sched_idle_block_us",
             "sched_foreground_polled_us", "sched_iterations", "undo_pages_recycled"]
hdr = (f"{'cell':6} {'arm':3} {'dur':6} {'wall':>6} {'TPS':>7} {'p25':>8} {'p50':>8} {'p95':>9} "
       f"{'p99':>9} {'max':>10} {'ld-p99':>8} {'pre-load':>9} {'post':>9}")
print(hdr)
rows = []
for c in run["cells"]:
    name = c["cell"]
    j = json.load(open(os.path.join(R, f"{name}.json")))
    ph = {p["phase"]: p for p in j["phases"]}
    t = ph["txn"]; lu = ph["load-users"]
    pre = c["precheck"]["loadavg"].split()[0]; post = c["postcheck_loadavg"].split()[0]
    print(f"{name:6} {c['arm']:3} {c['durability']:6} {j['meta']['seconds']:6.1f} {j['meta']['tps']:7.1f} "
          f"{t['p25_us']:8.1f} {t['p50_us']:8.1f} {t['p95_us']:9.1f} {t['p99_us']:9.1f} {t['max_us']:10.1f} "
          f"{lu['p99_us']:8.1f} {pre:>9} {post:>9}")
    meta = dict(re.findall(r"(\w+)=([^\s]+)", str(c.get("meta", ""))))
    rows.append((name, c["arm"], c["durability"], {k: meta.get(k) for k in META_KEYS},
                 c["precheck"]["pgrep"], c["driver"]["seconds"], c["server_start"]))
print()
print("phase p50 (us) per cell: trade-insert / account-update / profit-scan / profit-insert, then p99")
for c in run["cells"]:
    name = c["cell"]
    j = json.load(open(os.path.join(R, f"{name}.json")))
    ph = {p["phase"]: p for p in j["phases"]}
    print(f"{name:6} {c['arm']:3} {c['durability']:6} "
          f"{ph['trade-insert']['p50_us']:8.1f} {ph['account-update']['p50_us']:8.1f} "
          f"{ph['profit-scan']['p50_us']:8.1f} {ph['profit-insert']['p50_us']:8.1f}   "
          f"p99: {ph['trade-insert']['p99_us']:9.1f} {ph['account-update']['p99_us']:9.1f} "
          f"{ph['profit-scan']['p99_us']:9.1f} {ph['profit-insert']['p99_us']:9.1f}")
print()
print("META per cell:")
for name, arm, dur, m, pg, secs, start in rows:
    print(f"{name:6} {arm} {dur:6} start={start} driver_s={secs:6.1f} " + " ".join(f"{k}={v}" for k, v in m.items()))
print()
print("precheck pgrep (other than the autotrade server and run_ab.py itself):")
for name, arm, dur, m, pg, secs, start in rows:
    other = [l for l in pg.splitlines() if "autotrade" not in l and "run_ab.py" not in l]
    print(f"{name:6}: {other if other else '(none)'}")
