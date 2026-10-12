#!/usr/bin/env python3
"""Host sampler: once a second, per-CPU busy % (from /proc/stat) and the processes using >= 20 % of a CPU
whose command is not ours (not under /home/cdkbs/bench-runs, not this script). One JSON line per sample."""
import json, os, subprocess, sys, time

out = open(sys.argv[1], "a", buffering=1)


def stat():
    d = {}
    with open("/proc/stat") as f:
        for l in f:
            if l.startswith("cpu") and l[3].isdigit():
                p = l.split()
                v = list(map(int, p[1:9]))
                d[int(p[0][3:])] = (sum(v), v[3] + v[4])  # total, idle+iowait
    return d


prev = stat()
while True:
    time.sleep(1)
    cur = stat()
    busy = {c: round(100 * (1 - (cur[c][1] - prev[c][1]) / max(1, cur[c][0] - prev[c][0])), 1) for c in cur}
    prev = cur
    ps = subprocess.run(["ps", "-eo", "pid,psr,pcpu,args", "--sort=-pcpu", "--no-headers"], capture_output=True, text=True).stdout.splitlines()
    foreign = []
    for l in ps[:12]:
        p = l.split(None, 3)
        if len(p) < 4:
            continue
        if float(p[2]) >= 20 and "bench-runs" not in p[3] and "sampler.py" not in p[3] and "ps -eo" not in p[3]:
            foreign.append(f"{p[0]}@{p[1]} {p[2]}% {p[3][:90]}")
    with open("/proc/loadavg") as f:
        load = f.read().split()[0]
    out.write(json.dumps({"t": round(time.time(), 1), "load": load, "busy": busy, "foreign": foreign}) + "\n")
