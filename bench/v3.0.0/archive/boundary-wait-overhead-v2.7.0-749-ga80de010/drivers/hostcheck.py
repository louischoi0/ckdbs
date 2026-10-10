import json, glob
res = "/home/cdkbs/bench-runs/bw/res/"
tot = 0
bad = []
maxo = 0
max1 = 0
ld = []
perrun = []
for f in sorted(glob.glob(res + "*/*.host.json")):
    h = json.load(open(f))
    tot += 1
    name = f[len(res):]
    pin = ("6", "7") if name.startswith("c1") else ("3", "4", "5", "6", "7")
    o = [v for c, v in h["cpu_busy_mean"].items() if c not in pin]
    o1 = [v for c, v in h["cpu_busy_max_1s"].items() if c not in pin]
    maxo = max(maxo, max(o))
    max1 = max(max1, max(o1))
    ld.append(float(h["loadavg_start"].split()[0]))
    rcs = [h.get("rc"), h.get("rc_reader"), h.get("rc_writer"), h.get("rc_bystander")]
    if h["competing_start"] or h["competing_end"] or max(o) > 0.5 or any(x for x in rcs if x):
        bad.append((name, h["competing_start"], h["competing_end"], max(o), rcs))
    perrun.append((max(o), max(o1), name))
print(tot, "runs; flagged", len(bad), bad[:5])
print("max other-CPU run-mean", maxo, "max other-CPU 1s sample", max1, "load1 at start", min(ld), max(ld))
perrun.sort(reverse=True)
print(perrun[:3])
e = 0
for f in glob.glob(res + "c[23]*/summary.json"):
    for t, r in json.load(open(f)).items():
        for n in ("reader", "writer", "bystander"):
            if n in r:
                e += r[n]["errors"]
print("client errors total", e)
