import glob, json, collections, re
tmin = "9"; tmax = ""
loads = []
comp = collections.Counter()
nrec = 0
ob_all = []
for f in sorted(glob.glob("/home/cdkbs/bench-runs/bj-close/out-all/c*-n*-r*.json")):
    j = json.load(open(f))
    nrec += 1
    tmin = min(tmin, j["t_start"]); tmax = max(tmax, j["t_start"])
    loads += [float(j["host_before"]["loadavg"].split()[0]), float(j["host_after"]["loadavg"].split()[0])]
    for c in j["host_before"]["competing"] + j["host_after"]["competing"]:
        pid, _, rest = c.partition(" ")
        key = re.sub(r"snapshot-bash-\d+-\w+\.sh", "snapshot", rest)[:70] + (" ... " + ("pgrep-self" if "pgrep" in rest else ""))
        comp[(pid, key)] += 1
print("run records", nrec, "first start", tmin, "last start", tmax)
print("loadavg1 min/max", min(loads), max(loads))
for k, v in comp.most_common(12):
    print(v, k)
# bisect/diag
for d in ("out-bis", "out-diag"):
    ts = [json.load(open(f))["t_start"] for f in glob.glob(f"/home/cdkbs/bench-runs/bj-close/{d}/*.json")]
    print(d, len(ts), min(ts), max(ts))
