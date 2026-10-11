"""List runs that the discard rules would drop. usage: screen.py [--tags] <dir...>
Rules: (1) a real cmake --build / ctest / cc1plus in the before or after list; (2) other-CPU busy > 30 %;
(3) the SHOW META control's p50 (either arm) above 53 us (the quiet value is 49.3-51.2 us in every
quiet run of the first file): the round-trip floor itself moved, so the host did.
--tags prints only the run tags, for a script."""
import glob, json, sys
tags = "--tags" in sys.argv
dirs = [a for a in sys.argv[1:] if a != "--tags"]
n = 0
tot = 0
for d in dirs:
    for f in sorted(glob.glob(f"{d}/*-n*-r*.json")):
        j = json.load(open(f))
        tot += 1
        why = []
        for when in ("host_before", "host_after"):
            for c in j[when]["competing"]:
                rest = c.partition(" ")[2]
                if rest.startswith("ctest") or rest.startswith("cmake --build") or "cc1plus" in rest:
                    why.append(f"{when[5:]}:{rest[:30]}")
        sc = {2, 4} | ({3} if j["cores"] == 2 else set())
        o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
        ob = sum(o) / len(o)
        if ob > 0.30:
            why.append(f"other={ob:.2f}")
        pa, pb = j["arms"]["A"]["ping"]["p50_us"], j["arms"]["B"]["ping"]["p50_us"]
        if max(pa, pb) > 53.0:
            why.append(f"ping {pa}/{pb}")
        if why:
            n += 1
            if tags:
                print(j["tag"])
            else:
                print(f.split("/")[-1], f"other={ob:.2f}", why)
if not tags:
    print(n, "of", tot, "flagged")
