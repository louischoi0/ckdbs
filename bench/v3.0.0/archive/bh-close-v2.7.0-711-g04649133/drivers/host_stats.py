import glob, json, sys
A = sys.argv[1]
comp = set(); loads = []; n = 0
for f in glob.glob(A + "/**/*.json", recursive=True):
    try:
        j = json.load(open(f))
    except Exception:
        continue
    if not isinstance(j, dict) or "host_before" not in j:
        continue
    n += 1
    for k in ("host_before", "host_after"):
        for c in j[k]["competing"]:
            comp.add(c[:90])
        loads.append(float(j[k]["loadavg"].split()[0]))
print(n, "run records;", "loadavg", min(loads), max(loads))
for c in sorted(comp):
    print(" competing:", c)
