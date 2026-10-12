"""The run record and hold counters at the end of one run. usage: endmeta.py <out-dir>/<cell> <rows> <run>"""
import json, sys

d, rows, run = sys.argv[1:4]
s = json.load(open(f"{d}/n{rows}-r{run}.json"))
for srv in "AB":
    m = s["meta_end"][srv]
    print(srv, {k.replace("checkpoint_last_", ""): v for k, v in m.items() if k.startswith("checkpoint_last_") and v})
    print("   ", {k: v for k, v in m.items() if k.startswith(("sched_hold", "sched_system", "sched_longest", "sched_unpolled", "wal_"))})
