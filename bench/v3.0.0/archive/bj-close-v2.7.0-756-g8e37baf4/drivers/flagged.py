import glob, json, sys
D = sys.argv[1]
n = 0
for f in sorted(glob.glob(f"{D}/c*-n*-r*.json")):
    j = json.load(open(f))
    hits = []
    for when in ("host_before", "host_after"):
        for c in j[when]["competing"]:
            pid, _, rest = c.partition(" ")
            if rest.startswith("ctest") or rest.startswith("cmake --build") or "cc1plus" in rest:
                hits.append(f"{when[5:]}:{rest[:40]}")
    sc = {2, 4} | ({3} if j["cores"] == 2 else set())
    o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
    if hits:
        n += 1
        a = j["arms"]
        print(f.split("/")[-1], f"other={sum(o) / len(o):.2f}", hits, f"litpt A/B {a['A']['lit-point']['p50_us']}/{a['B']['lit-point']['p50_us']}",
              f"ping {a['A']['ping']['p50_us']}/{a['B']['ping']['p50_us']}")
print(n, "flagged")
