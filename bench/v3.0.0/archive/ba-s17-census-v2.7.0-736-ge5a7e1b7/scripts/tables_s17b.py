"""Carve and checkpoint table for the BA-S17 census file. usage: tables_s17b.py <B kds dir> <S4 kds dir>"""
import collections, glob, json, statistics as st, sys


def load(d):
    g = collections.defaultdict(list)
    for f in sorted(glob.glob(d + "/*.json")):
        r = json.load(open(f))
        g[(r["durability"], r["cores"])].append(r)
    return g


gb, ga = load(sys.argv[1]), load(sys.argv[2])
print("### Carve and checkpoint longest, by durability and cores (max over each server's life; 75 runs per row)\n")
print("| durability | cores | S4 carve median / max | B carve median / max | S4 carve runs over 10 ms | B carve runs over 10 ms | S4 checkpoint median / max | B checkpoint median / max | S4 checkpoint runs over 10 ms | B checkpoint runs over 10 ms |")
print("|---|---|---|---|---|---|---|---|---|---|")
f = lambda xs: f"{st.median(xs):,.1f} ms / {max(xs):,.1f} ms"
o = lambda xs: f"{sum(x > 10 for x in xs)} of {len(xs)} runs"
for dur in ("relaxed", "group", "strict"):
    for k in (1, 2, 4):
        cs = {}
        for name, g in (("S4", ga), ("B", gb)):
            cs[name] = ([r["meta_longest"].get("contention_carve_longest_us", 0) / 1000 for r in g[(dur, k)]],
                        [r["meta_longest"].get("contention_checkpoint_longest_us", 0) / 1000 for r in g[(dur, k)]])
        print(f"| {dur} | {k} | {f(cs['S4'][0])} | {f(cs['B'][0])} | {o(cs['S4'][0])} | {o(cs['B'][0])} | "
              f"{f(cs['S4'][1])} | {f(cs['B'][1])} | {o(cs['S4'][1])} | {o(cs['B'][1])} |")
