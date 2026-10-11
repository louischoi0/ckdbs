import glob, json, statistics, sys
base = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
for d, cores, rows in (("overhead-c1", 1, 10000), ("null-c1", 1, 10000), ("flip-c1", 1, 10000), ("flip-c1", 1, 1000), ("overhead-c1", 1, 1000)):
    js = sorted((json.load(open(f)) for f in glob.glob(f"{base}{d}/c{cores}-n{rows}-r*.json")), key=lambda j: j["run"])
    print(f"== {d} n{rows}: raw slot values (slot A p50, slot B p50, B-A) for range100 by run; mean A {statistics.mean(j['arms']['A']['range100']['p50_us'] for j in js):.1f}, mean B {statistics.mean(j['arms']['B']['range100']['p50_us'] for j in js):.1f}")
    ev = [j["arms"]["B"]["range100"]["p50_us"] - j["arms"]["A"]["range100"]["p50_us"] for j in js if j["run"] % 2 == 0]
    od = [j["arms"]["B"]["range100"]["p50_us"] - j["arms"]["A"]["range100"]["p50_us"] for j in js if j["run"] % 2 == 1]
    print(f"   by run parity (start order): even runs {statistics.median(ev):+.1f} (n={len(ev)}), odd runs {statistics.median(od):+.1f} (n={len(od)})")
    print("   " + " ".join(f"r{j['run']}:{j['arms']['A']['range100']['p50_us']:.0f}/{j['arms']['B']['range100']['p50_us']:.0f}" for j in js))
