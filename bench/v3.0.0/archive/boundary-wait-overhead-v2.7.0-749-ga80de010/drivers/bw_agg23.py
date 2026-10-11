#!/usr/bin/env python3
"""bw_agg23.py SUMMARY.json [EXCLUDE.json] -> per-cell aggregation over triplets.

Slots: A (binary A), B (binary B), A2 (binary A again: the floor). Per triplet t the deltas are B - A and
A2 - A. Stats are medians over triplets of per-run stats; delta = median [Q1, Q3] over triplets, with the count of
triplets where the delta is positive. Excluded runs (tags) remove their whole triplet.
"""
import json, math, statistics, sys

S = json.load(open(sys.argv[1]))
EXC = set(json.load(open(sys.argv[2]))) if len(sys.argv) > 2 else set()


def q(vals, p):
    s = sorted(vals)
    k = (len(s) - 1) * p / 100
    f, c = math.floor(k), math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


tri = {}
for tag, r in S.items():
    t = r["host"]["triplet"]
    tri.setdefault(t, {})[r["host"]["slot"]] = (tag, r)
good = {}
for t, d in tri.items():
    if len(d) == 3 and not any(tag in EXC for tag, _ in d.values()):
        good[t] = {k: v[1] for k, v in d.items()}


def metric(r, conn, op, stat):
    if conn == "ceiling":
        return r["ceiling"][op]
    c = r[conn]
    if stat == "qps":
        return c["unit_qps_window"] if conn == "writer" else (c["qps"] if "qps" in c else None)
    if stat == "n":
        return c["ops"][op]["n"]
    return c["ops"][op][stat]


def summarise(conn, op, stat):
    vals = {s: [metric(good[t][s], conn, op, stat) for t in sorted(good)] for s in ("A", "B", "A2")}
    res = {"triplets": len(good)}
    for s in vals:
        res[s] = statistics.median(vals[s])
        res[s + "_q"] = [q(vals[s], 25), q(vals[s], 75)]
    for name, x in (("B-A", "B"), ("A2-A", "A2")):
        dd = [b - a for a, b in zip(vals["A"], vals[x])]
        res[name] = {"med": statistics.median(dd), "q1": q(dd, 25), "q3": q(dd, 75), "pos": sum(1 for z in dd if z > 0)}
    return res


out = {}
if not good:
    print("no complete triplets")
    sys.exit(1)
first = next(iter(good.values()))["A"]
conns = [c for c in ("reader", "bystander", "writer") if c in first]
for conn in conns:
    for op in first[conn]["ops"]:
        for stat in ("p0", "p25", "p50", "p75", "p99", "max", "mean", "qps", "n"):
            out[f"{conn}|{op}|{stat}"] = summarise(conn, op, stat)
for op in ("contention_ceiling_waits", "contention_ceiling_wait_us"):
    out[f"ceiling|{op}|v"] = summarise("ceiling", op, "v")
json.dump(out, open(sys.argv[1].replace("summary.json", "agg.json"), "w"), indent=1)
print("triplets", len(good), "of", len(tri))
for conn in conns:
    for op in first[conn]["ops"]:
        for stat in ("p0", "p25", "p50", "p75", "p99", "max", "qps"):
            r = out[f"{conn}|{op}|{stat}"]
            print(f"{conn:<9} {op:<7} {stat:<4} A={r['A']:.1f} B={r['B']:.1f} A2={r['A2']:.1f}  B-A={r['B-A']['med']:+.1f} [{r['B-A']['q1']:+.1f},{r['B-A']['q3']:+.1f}] pos {r['B-A']['pos']}/{r['triplets']}"
                  f"  A2-A={r['A2-A']['med']:+.1f} [{r['A2-A']['q1']:+.1f},{r['A2-A']['q3']:+.1f}]")
for op in ("contention_ceiling_waits", "contention_ceiling_wait_us"):
    r = out[f"ceiling|{op}|v"]
    print(f"ceiling {op}: A={r['A']:.0f} B={r['B']:.0f} A2={r['A2']:.0f}")
