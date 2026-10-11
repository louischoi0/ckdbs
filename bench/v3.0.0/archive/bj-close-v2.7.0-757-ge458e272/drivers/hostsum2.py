import glob, json, collections
R = "/home/cdkbs/bench-runs/bj-close/b2/"
for name, pat in (("kept", "out-all/*-n*-r*.json"), ("bis", "out-bis/*.json"), ("discarded", "discarded/pass*/*.json")):
    ts = []; loads = []
    for f in glob.glob(R + pat):
        j = json.load(open(f))
        ts.append(j["t_start"])
        loads += [float(j["host_before"]["loadavg"].split()[0]), float(j["host_after"]["loadavg"].split()[0])]
    print(name, len(ts), min(ts), max(ts), "loadavg", min(loads), max(loads))
why = collections.Counter()
for f in glob.glob(R + "discarded/pass*/*.json"):
    j = json.load(open(f))
    kinds = set()
    for when in ("host_before", "host_after"):
        for c in j[when]["competing"]:
            rest = c.partition(" ")[2]
            if rest.startswith("ctest") or rest.startswith("cmake --build") or "cc1plus" in rest:
                kinds.add("build/ctest in list")
    sc = {2, 4} | ({3} if j["cores"] == 2 else set())
    o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
    if sum(o) / len(o) > 0.30:
        kinds.add("other>30%")
    if max(j["arms"]["A"]["ping"]["p50_us"], j["arms"]["B"]["ping"]["p50_us"]) > 53.0:
        kinds.add("control>53us")
    why[tuple(sorted(kinds))] += 1
print(why)
shell = 0
for f in glob.glob(R + "out-all/*-n*-r*.json"):
    j = json.load(open(f))
    for when in ("host_before", "host_after"):
        for c in j[when]["competing"]:
            rest = c.partition(" ")[2]
            if "pgrep" in rest or rest.startswith("/bin/bash"):
                shell += 1
print("kept-run list entries that are shells matching the pattern:", shell)
ob = collections.defaultdict(list)
for f in glob.glob(R + "out-all/*-n*-r*.json"):
    j = json.load(open(f))
    sc = {2, 4} | ({3} if j["cores"] == 2 else set())
    o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
    ob[(j["cores"], j["rows"])].append(sum(o) / len(o))
for k in sorted(ob):
    print(k, round(min(ob[k]), 3), round(max(ob[k]), 3))
