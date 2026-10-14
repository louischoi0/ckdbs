import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

CELLS = ["c1-g", "c8-g", "c1-s", "c8-s"]
DUR = {"g": "group", "s": "strict"}
REPS = ["", "r2-", "r3-"]
EXTRA = ["commit", "freight-insert", "charge-insert", "operation-update", "org-update",
         "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "manifest-scan"]


def tpsl(rep, c, old=None):
    return [load(f"{r}s2-{c}", old)["meta"]["tps"] for r in REPS]


print("### throughput (tps), three runs, against the 9a0525d rebaseline (three runs)")
print("| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | 9a0525d median | 9a0525d spread | delta of medians |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for c in CELLS:
    v = tpsl(REPS, c)
    o = tpsl(REPS, c, 2)
    m, om = statistics.median(v), statistics.median(o)
    print(f"| `s2-{c}` | {c[1]} | {DUR[c[-1]]} | " + " | ".join(f"{x:,.1f} tps" for x in v) +
          f" | **{m:,.1f} tps** | {100*(max(v)/min(v)-1):.1f} % | {om:,.1f} tps | {100*(max(o)/min(o)-1):.1f} % | {100*(m/om-1):+.1f} % |")
print()

print("### core and durability ratios (medians)")
med = {c: statistics.median(tpsl(REPS, c)) for c in CELLS}
omed = {c: statistics.median(tpsl(REPS, c, 2)) for c in CELLS}
print(f"cores 8 over 1, group: {med['c8-g']/med['c1-g']:.2f}x (9a0525d {omed['c8-g']/omed['c1-g']:.2f}x); strict: {med['c8-s']/med['c1-s']:.2f}x (9a0525d {omed['c8-s']/omed['c1-s']:.2f}x)")
print(f"group over strict, cores 1: {med['c1-g']/med['c1-s']:.2f}x (9a0525d {omed['c1-g']/omed['c1-s']:.2f}x); cores 8: {med['c8-g']/med['c8-s']:.2f}x (9a0525d {omed['c8-g']/omed['c8-s']:.2f}x)")
print()

main = ["booking"] + EXTRA
for ri, rep in enumerate(REPS):
    print(f"### percentiles, run {ri+1} (`{rep}s2-*`), full")
    print("| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for c in CELLS:
        d = load(f"{rep}s2-{c}")
        for name in main:
            p = ph(d, name)
            if not p:
                continue
            print(f"| `s2-{c}` | {name} | {pct_row(p)} |")
    print()

print("### p50 and mean per phase, this run (run 1) against 9a0525d run 1 (cross-version; same driver and arguments)")
print("| cell | phase | p50 now | p50 9a0525d | ratio now/then | mean now | mean 9a0525d |")
print("|---|---|---|---|---|---|---|")
for c in CELLS:
    d, o = load(f"s2-{c}"), load(f"s2-{c}", 2)
    for name in ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update", "cargo-lookup", "recipe-read"]:
        p, q = ph(d, name), ph(o, name)
        print(f"| `s2-{c}` | {name} | {fmt_us(p['p50_us'])} | {fmt_us(q['p50_us'])} | {p['p50_us']/q['p50_us']:.2f}x | {fmt_us(p['mean_us'])} | {fmt_us(q['mean_us'])} |")
print()

print("### wait shares, run 1 (phase mean x ops over booking mean x ops), now / 9a0525d")
names = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert", "operation-update", "org-update", "commit"]
print("| cell | " + " | ".join(names) + " | unattributed (BEGIN, refused attempts, client, socket) | booking mean |")
print("|---|" + "---|" * (len(names) + 2))
for c in CELLS:
    for lab, d in (("now", load(f"s2-{c}")), ("9a0525d", load(f"s2-{c}", 2))):
        t = ph(d, "booking")
        tot = t["mean_us"] * t["ops"]
        sh = [ph(d, n)["mean_us"] * ph(d, n)["ops"] / tot for n in names]
        print(f"| `s2-{c}` ({lab}) | " + " | ".join(f"{100*x:.1f} %" for x in sh) + f" | {100*(1-sum(sh)):.1f} % | {t['mean_us']:,.0f} µs |")
print()

print("### outcomes, conflicts and the invariant check")
print("| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks | error replies (all phases) | exit |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for ri, rep in enumerate(REPS):
    for c in CELLS:
        tag = f"{rep}s2-{c}"
        d = load(tag)
        m = d["meta"]
        o, a = m["outcomes"], m["axes"]
        errs = sum(p["errors"] for p in d["phases"])
        rc = open(f"{L}/{tag}.rc").read().strip()
        print(f"| `{tag}` | {ri+1} | {o['committed']:,} bookings | {o['rejected-capacity']} bookings | {o['rejected-credit']} bookings | {o['conflicted']} conflicts | {m['abandoned']} | {a['operations']} / {a['organizations']} / {a['read']} / {a['commit']} conflicts | {m['verify']['failures']} / {m['verify']['checks']} checks | {errs} replies | exit {rc} |")
print()

for tag in ("rr-s2-c1-g", "rr-s2-c8-g"):
    if os.path.exists(f"{J}/{tag}.json"):
        d = load(tag)
        m = d["meta"]
        print(f"rr {tag}: tps {m['tps']:.1f} committed {m['outcomes']['committed']} conflicted {m['outcomes']['conflicted']} axes {m['axes']} verify {m['verify']['failures']}/{m['verify']['checks']} isolation {m['isolation']}")
        for name in ["booking", "commit", "operation-update", "org-update"]:
            print(f"| `{tag}` | {name} | {pct_row(ph(d, name))} |")
print()

for c in ["c1-g", "c1-s"]:
    for r in REPS:
        tag = f"{r}b1-s2-{c}"
        if os.path.exists(f"{J}/{tag}.json"):
            d = load(tag)
            m = d["meta"]
            print(f"b1 {tag}: tps {m['tps']:.1f} verify {m['verify']['failures']}/{m['verify']['checks']} conflicts {m['outcomes']['conflicted']}")
            for name in ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read"]:
                print(f"| `{tag}` | {name} | {pct_row(ph(d, name))} | mean {ph(d, name)['mean_us']:.1f}")
