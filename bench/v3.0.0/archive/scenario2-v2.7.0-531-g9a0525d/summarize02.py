#!/usr/bin/env python3
"""Tables for the scenario0/scenario2 results files, from the archived JSON."""
import json, os, statistics, sys
J = "/home/cdkbs/bench-runs/rebaseline-9a0525d/json"
L = "/home/cdkbs/bench-runs/rebaseline-9a0525d/logs"

def load(tag):
    return json.load(open(f"{J}/{tag}.json"))

def ph(d, name):
    for p in d["phases"]:
        if p["phase"] == name:
            return p
    return None

def loadavg(tag):
    for l in open(f"{L}/{tag}.precheck"):
        if l.startswith("loadavg"):
            return " / ".join(l.split(":")[1].split()[:3])

def pgrep(tag):
    t = open(f"{L}/{tag}.precheck").read()
    return "none" if "  none" in t else "SEE FILE"

CELLS = ["c1-g", "c8-g", "c1-s", "c8-s"]
DUR = {"g": "group", "s": "strict"}
PREV0 = {"c1-g": 750.6, "c8-g": 794.3, "c1-s": 212.1, "c8-s": 213.0}
PREV2 = {"c1-g": 589.0, "c8-g": 308.5, "c1-s": 553.0, "c8-s": 294.0}
which = sys.argv[1]
sc = "s0" if which == "0" else "s2"
prev = PREV0 if which == "0" else PREV2
reps = ["", "r2-", "r3-"]

print("### TPS, three runs")
print("| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | f6ed10c |")
print("|---|---|---|---|---|---|---|---|---|")
for c in CELLS:
    v = [load(f"{r}{sc}-{c}")["meta"]["tps"] for r in reps]
    med = statistics.median(v)
    print(f"| `{sc}-{c}` | {c[1]} | {DUR[c[-1]]} | " + " | ".join(f"{x:,.1f} tps" for x in v) +
          f" | **{med:,.1f} tps** | {100*(max(v)/min(v)-1):.1f} % | {prev[c]:,.1f} tps |")
print()
print("### Delta of the median against f6ed10c")
print("| cell | median | f6ed10c | delta |")
print("|---|---|---|---|")
for c in CELLS:
    v = [load(f"{r}{sc}-{c}")["meta"]["tps"] for r in reps]
    med = statistics.median(v)
    print(f"| `{sc}-{c}` | {med:,.1f} tps | {prev[c]:,.1f} tps | {100*(med/prev[c]-1):+.1f} % |")
print()

main = "txn" if which == "0" else "booking"
extra = (["trade-insert", "account-update", "profit-scan", "profit-insert"] if which == "0" else
         ["commit", "freight-insert", "charge-insert", "operation-update", "org-update",
          "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "manifest-scan"])
for rep in reps:
    print(f"### Percentiles, run {reps.index(rep)+1} (`{rep}{sc}-*`), µs")
    print("| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for c in CELLS:
        d = load(f"{rep}{sc}-{c}")
        for name in [main] + extra:
            p = ph(d, name)
            if not p: continue
            print(f"| `{sc}-{c}` | {name} | {p['ops']:,} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} |")
    print()

print("### per-cell host stamp")
print("| cell | run | precheck UTC | loadavg 1/5/15 | build/ctest processes |")
print("|---|---|---|---|---|")
for rep in reps:
    for c in CELLS:
        tag = f"{rep}{sc}-{c}"
        t = open(f"{L}/{tag}.precheck").readline().split(": ")[1].strip()
        print(f"| `{tag}` | {reps.index(rep)+1} | {t} | {loadavg(tag)} | {pgrep(tag)} |")
print()

if which == "0":
    print("### errors / torn")
    print("| cell | run | committed | torn | error replies (all phases) | driver exit |")
    print("|---|---|---|---|---|---|")
    for rep in reps:
        for c in CELLS:
            tag = f"{rep}{sc}-{c}"
            d = load(tag)
            errs = sum(p["errors"] for p in d["phases"])
            rc = open(f"{L}/{tag}.rc").read().strip()
            print(f"| `{tag}` | {reps.index(rep)+1} | {d['meta']['committed']:,} | {d['meta']['torn']} | {errs} | {rc} |")
else:
    print("### outcomes, conflicts and the invariant check")
    print("| cell | run | committed | rejected-capacity | rejected-credit | conflicted | axes (op / org / read / commit) | verify failures / checks | error replies (all phases) | exit |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for rep in reps:
        for c in CELLS:
            tag = f"{rep}{sc}-{c}"
            m = load(tag)["meta"]
            d = load(tag)
            o = m["outcomes"]; a = m["axes"]
            errs = sum(p["errors"] for p in d["phases"])
            rc = open(f"{L}/{tag}.rc").read().strip()
            print(f"| `{tag}` | {reps.index(rep)+1} | {o['committed']:,} | {o['rejected-capacity']} | {o['rejected-credit']} | {o['conflicted']} | {a['operations']} / {a['organizations']} / {a['read']} / {a['commit']} | {m['verify']['failures']} / {m['verify']['checks']} | {errs} | {rc} |")

print()
print("### wait shares (run 1; phase mean x ops over the unit's mean x ops)")
if which == "0":
    print("| cell | trade-insert | account-update | unattributed (driver, socket, scheduling between statements) | txn mean |")
    print("|---|---|---|---|---|")
    for c in CELLS:
        d = load(f"{sc}-{c}")
        t = ph(d,"txn"); a=ph(d,"trade-insert"); b=ph(d,"account-update")
        tot = t["mean_us"]*t["ops"]
        sa = a["mean_us"]*a["ops"]/tot; sb=b["mean_us"]*b["ops"]/tot
        print(f"| `{sc}-{c}` | {100*sa:.1f} % | {100*sb:.1f} % | {100*(1-sa-sb):.1f} % | {t['mean_us']:,.0f} µs |")
else:
    names=["cargo-lookup","credit-lookup","capacity-read","recipe-read","freight-insert","charge-insert","operation-update","org-update","commit"]
    print("| cell | "+" | ".join(names)+" | unattributed (BEGIN, refused attempts, client, socket) | booking mean |")
    print("|---|"+"---|"*(len(names)+2))
    for c in CELLS:
        d = load(f"{sc}-{c}")
        t = ph(d,"booking"); tot=t["mean_us"]*t["ops"]
        sh=[]
        for n in names:
            p=ph(d,n); sh.append(p["mean_us"]*p["ops"]/tot)
        print(f"| `{sc}-{c}` | "+" | ".join(f"{100*x:.1f} %" for x in sh)+f" | {100*(1-sum(sh)):.1f} % | {t['mean_us']:,.0f} µs |")
