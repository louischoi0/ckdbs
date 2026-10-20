import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *

SHAPES = ["bar-lookup", "bar-range", "day-slice", "symbol-history", "cross-join", "point-join", "model-join"]
CELLS = [("c1-g", "cores = 1, group"), ("c8-g", "cores = 8, group"), ("c1-s", "cores = 1, strict")]


def sweep(d):
    return {s["shape"]: s for s in d["meta"]["qps_sweep"]}


def cellruns(tag, oldtag=False):
    if oldtag:
        return runs(lambda t: old(1, t), tag)
    return runs(new, tag)


def mat(tagn, tago, label):
    R = cellruns(tagn)
    O = cellruns(tago, True)
    print(f"#### {label} ({len(R)} runs now, {len(O)} runs at v3.1.0)\n")
    print("| shape | cold | warm | spread of warm | cabin | cabin over warm | warm v3.1.0 (cross-version) | warm delta | cabin v3.1.0 | cabin delta |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for sh in SHAPES:
        def g(runs_, key):
            v = []
            for d in runs_:
                s = sweep(d).get(sh)
                if s and s.get(key):
                    v.append(s[key]["qps"])
            return v
        cold, warm, cab = g(R, "cold"), g(R, "warm"), g(R, "cabin")
        ow, oc = g(O, "warm"), g(O, "cabin")
        print(f"| {sh} | {f1(med(cold), ' qps')} | {f1(med(warm), ' qps')} | {sp(spread(warm))} | "
              f"{f1(med(cab), ' qps') if cab else 'n/a'} | {ratio(med(cab), med(warm)) if cab else '-'} | "
              f"{f1(med(ow), ' qps')} | {delta(med(warm), med(ow))} | {f1(med(oc), ' qps') if oc else 'n/a'} | "
              f"{delta(med(cab), med(oc)) if cab and oc else '-'} |")
    print()


def writes():
    print("| rows per BEGIN/COMMIT | " + " | ".join(f"{c[0]} now" for c in CELLS) + " | " + " | ".join(f"{c[0]} v3.1.0" for c in CELLS) + " |")
    print("|---|" + "---|" * 6)
    for b in (1, 10, 100, 1000):
        row = []
        for off in (False, True):
            for c, _ in CELLS:
                R = cellruns(("s1b-" if off else "s1-") + c, off)
                v = [x["qps"] for d in R for x in d["meta"]["write_sweep"] if x["batch"] == b]
                row.append(f"{f0(med(v), ' rows/s')} ({sp(spread(v))})")
        print(f"| {b} row{'s' if b > 1 else ''} | " + " | ".join(row) + " |")
    print()


def conc():
    print("| connections | " + " | ".join(f"{c[0]} now" for c in CELLS) + " | " + " | ".join(f"{c[0]} v3.1.0" for c in CELLS) + " |")
    print("|---|" + "---|" * 6)
    for n in (1, 2, 4, 8):
        row = []
        for off in (False, True):
            for c, _ in CELLS:
                R = cellruns(("s1b-" if off else "s1-") + c, off)
                v = [x["qps"] for d in R for x in d["meta"]["concurrency_sweep"] if x["connections"] == n]
                row.append(f"{f1(med(v), ' qps')} ({sp(spread(v))})")
        print(f"| {n} | " + " | ".join(row) + " |")
    print()


PHASES = None


def phases_table():
    for c, label in CELLS:
        d = new("s1-" + c)
        if not d:
            continue
        print(f"#### phases, run 1 of `s1-{c}` ({label})\n")
        print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
        print("|---|---|---|---|---|---|---|---|---|")
        for p in d["phases"]:
            print(f"| {p['phase']} | " + pct_row(p) + " |")
        print()


def phase_medians():
    """whole-relation reads and loads: median qps over 3 runs per cell, now vs v3.1.0"""
    names = [p["phase"] for p in new("s1-c1-g")["phases"]]
    print("| phase (cores = 1, group) | ops | now: median qps | spread | v3.1.0 median qps (cross-version) | delta |")
    print("|---|---|---|---|---|---|")
    R = cellruns("s1-c1-g")
    O = cellruns("s1b-c1-g", True)
    for n in names:
        a = [ph(d, n)["qps"] for d in R if ph(d, n)]
        b = [ph(d, n)["qps"] for d in O if ph(d, n)]
        ops = ph(R[0], n)["ops"]
        print(f"| {n} | {ops:,} ops | {f1(med(a), ' qps')} | {sp(spread(a))} | {f1(med(b), ' qps')} | {delta(med(a), med(b))} |")
    print()


def sweep_sizes():
    print("| bars | cell | runs | shape | cold | warm | cabin | spread of warm | v3.1.0 warm (runs) | warm delta |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for n, bars in (("n252", 252), ("n1008", 1008), ("n10080", 10080)):
        R = runs(new, f"s1s-{n}-c1-g")
        O = runs(lambda t: old(1, t), f"s1s-{n}-c1-g")
        for sh in SHAPES:
            def g(rr, key):
                return [sweep(d)[sh][key]["qps"] for d in rr if sweep(d).get(sh) and sweep(d)[sh].get(key)]
            w, c = g(R, "warm"), g(R, "cabin")
            ow = g(O, "warm")
            print(f"| {bars:,} | s1s-{n} | {len(R)} | {sh} | {f1(med(g(R, 'cold')), ' qps')} | {f1(med(w), ' qps')} | "
                  f"{f1(med(c), ' qps') if c else '-'} | {sp(spread(w))} | {f1(med(ow), ' qps')} ({len(O)}) | {delta(med(w), med(ow))} |")
    print()


if __name__ == "__main__":
    which = sys.argv[1]
    if which == "mat":
        for c, label in CELLS:
            mat("s1-" + c, "s1b-" + c, f"cell {c}: {label}")
    elif which == "writes":
        writes()
    elif which == "conc":
        conc()
    elif which == "phases":
        phases_table()
    elif which == "pm":
        phase_medians()
    elif which == "sizes":
        sweep_sizes()
