import sys, re
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

GROUPS = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]
ARMS = ["off", "on", "declared"]
dA, dB = load("s4"), load("cd2-s4")
oA, oB = load("s4", 4), load("cd2-s4", 4)


def get(d, day, g, arm):
    for p in d["phases"]:
        if p["phase"] == f"d{day}-{g}[{arm}]":
            return p


print("### p50 and derived qps by arm and day, now: run A (defaults) / run B (cooldown 2 half-lives)")
print("| group | day | off A | off B | on A | on B | declared A | declared B | on over off, mean (A / B) |")
print("|---|---|---|---|---|---|---|---|---|")
for g in GROUPS:
    for day in (1, 2, 3):
        cells, vals = [], {}
        for arm in ARMS:
            for d, nm in ((dA, "A"), (dB, "B")):
                p = get(d, day, g, arm)
                vals[(arm, nm)] = p
                cells.append(f"{fmt_us(p['p50_us'])} / {1e6/p['mean_us']:,.0f} qps")
        sp = [f"{vals[('off', nm)]['mean_us']/vals[('on', nm)]['mean_us']:.2f}x" for nm in "AB"]
        print(f"| {g} | {day} | " + " | ".join(cells) + f" | {sp[0]} / {sp[1]} |")
print()
print("### cross-version: derived qps now over 9a0525d (run A and run B), per arm")
print("| group | day | off A | on A | declared A | off B | on B | declared B |")
print("|---|---|---|---|---|---|---|---|")
for g in GROUPS:
    for day in (1, 2, 3):
        row = []
        for d, o in ((dA, oA), (dB, oB)):
            for arm in ARMS:
                p, q = get(d, day, g, arm), get(o, day, g, arm)
                row.append(f"{(1e6/p['mean_us'])/(1e6/q['mean_us']):.2f}x")
        print(f"| {g} | {day} | " + " | ".join(row) + " |")
print()
print("### the same, 9a0525d absolute qps (run A / run B) for side-by-side")
for g in GROUPS:
    for day in (1, 2, 3):
        print(f"| {g} | {day} | " + " | ".join(f"{1e6/get(o, day, g, a)['mean_us']:,.0f} qps" for o in (oA, oB) for a in ARMS) + " |")
print()
print("### busy-time TPS (driver) -- not in JSON meta; see stdout")
print()
for nm, d in (("A", dA), ("B", dB)):
    print(f"### full percentile table, run {nm}")
    print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in d["phases"]:
        print(f"| {p['phase']} | {p['ops']:,} ops | {fmt_us(p['p0_us'])} | {fmt_us(p['p25_us'])} | {fmt_us(p['p50_us'])} | {fmt_us(p['p95_us'])} | {fmt_us(p['p99_us'])} | {fmt_us(p['max_us'])} | {p['errors']} errors |")
    print()
print("### server CPU seconds by arm and phase group, now A / B  |  9a0525d A / B")
print("| arm | board | tape | open | close |")
print("|---|---|---|---|---|")
for arm in ARMS:
    row = []
    for g in ("board", "tape", "open", "close"):
        v = [sum(x for k, x in d["server_cpu_s"][arm].items() if k.endswith(g)) for d in (dA, dB, oA, oB)]
        row.append(f"{v[0]:.2f} s / {v[1]:.2f} s  |  {v[2]:.2f} s / {v[3]:.2f} s")
    print(f"| {arm} | " + " | ".join(row) + " |")
print()
print("### optimizer lifecycle on the `on` arm")
for nm, d in (("A", dA), ("B", dB)):
    print(f"run {nm}:")
    print("| t (s) | day | tag | managed | creates | extends | drops | entries |")
    print("|---|---|---|---|---|---|---|---|")
    for e in d["evidence"]:
        on = e.get("on")
        if not on or e["tag"] not in ("trading-block-3", "trading-block-9", "trading-block-12", "overnight"):
            continue
        m = re.search(r"managed=(\d+).*creates=(\d+) extends=(\d+) heals=(\d+) drops=(\d+)", on["header"])
        st = []
        for x in on["entries"]:
            r = re.search(r"rel=(\S+) .*state=(\S+) cabin_id=(\d+) pages=(\d+)", x)
            st.append(f"{r.group(1)}:{r.group(2)}/{r.group(4)}p")
        print(f"| {e['t_wall_s']} s | {e['day']} | {e['tag']} | {m.group(1)} Cabins | {m.group(2)} creates | {m.group(3)} extends | {m.group(5)} drops | {'; '.join(st)} |")
    print(f"final header: `{d['final']['on']['header']}`")
    print("verify per day: " + ", ".join("PASS" if not v["mismatches"] else "FAIL" for v in d["verify"]))
    print()
print("old finals:")
print(oA["final"]["on"]["header"])
print(oB["final"]["on"]["header"])
print("meta keys:", list(dA.keys()), {k: v for k, v in dA.get("meta", {}).items()} if "meta" in dA else "")
