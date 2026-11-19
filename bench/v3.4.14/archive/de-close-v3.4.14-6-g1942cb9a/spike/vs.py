import glob
import json
import os
import sys

d = sys.argv[1]
cells = sys.argv[2].split(",")
for f in sorted(glob.glob(os.path.join(d, "de-*.json"))):
    r = json.load(open(f))
    if r["cell"] not in cells:
        continue
    w = r["waits_delta"]
    m = r["meta_delta"]
    print(r["cell"], r.get("arm"), "r%d" % r["run"],
          "hits", int(m.get("pool_hits", 0)), "misses", int(m.get("pool_misses", 0)),
          "on_cpu_us", w.get("cpu.on_cpu", {}).get("total_us"),
          "runq", w.get("sched.run_queue", {}).get("count"), w.get("sched.run_queue", {}).get("total_us"),
          "sched_iter", int(m.get("sched_iterations", 0)), "idle_blocks", int(m.get("sched_idle_blocks", 0)),
          "polls", int(m.get("sched_polls_foreground", m.get("sched_polls", 0))),
          "hot p50", r["hot_from_schedule"].get("p50_us"), "svc p50", r["hot_service"].get("p50_us"),
          "window", r["window_s"])
r = json.load(open(sorted(glob.glob(os.path.join(d, "de-vscan-B-*.json")))[0]))
print([k for k in r["meta_delta"] if k.startswith("sched_")][:40])
