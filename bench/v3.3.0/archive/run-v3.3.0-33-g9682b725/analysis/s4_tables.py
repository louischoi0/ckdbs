import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *

GROUPS = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]
ARMS = ["off", "on", "declared"]


def P(d, day, g, arm):
    return ph(d, f"d{day}-{g}[{arm}]")


def runs4():
    return {"A": new("s4"), "B": new("cd2-s4")}, {"A": old(4, "s4"), "B": old(4, "cd2-s4")}


def tp():
    N, O = runs4()
    print("| group | day | " + " | ".join(f"{a} A (p50 / qps)" for a in ARMS) + " | on over off, mean (A / B) | declared over off, mean (A / B) |")
    print("|---|---|" + "---|" * 5)
    for g in GROUPS:
        for day in (1, 2, 3):
            cells = []
            for a in ARMS:
                p = P(N["A"], day, g, a)
                cells.append(f"{p['p50_us']:,.1f} µs / {1e6 / p['mean_us']:,.0f} qps" if p else "n/a")
            def rr(arm):
                out = []
                for k in "AB":
                    d = N[k]
                    if not d:
                        out.append("n/a")
                        continue
                    o, x = P(d, day, g, "off"), P(d, day, g, arm)
                    out.append(f"{o['mean_us'] / x['mean_us']:.2f}x" if o and x else "n/a")
                return " / ".join(out)
            print(f"| {g} | {day} | " + " | ".join(cells) + f" | {rr('on')} | {rr('declared')} |")
    print()


def cross():
    N, O = runs4()
    print("Derived qps (1,000,000 / mean µs), run A, now over v3.1.0 (cross-version), and run B:\n")
    print("| group | day | off A | on A | declared A | off B | on B | declared B |")
    print("|---|---|---|---|---|---|---|---|")
    for g in GROUPS:
        for day in (1, 2, 3):
            row = []
            for k in "AB":
                for a in ARMS:
                    n, o = N[k] and P(N[k], day, g, a), O[k] and P(O[k], day, g, a)
                    row.append(f"{o['mean_us'] / n['mean_us']:.2f}x" if n and o else "n/a")
            print(f"| {g} | {day} | " + " | ".join(row) + " |")
    print()


def busy():
    N, _ = runs4()
    print("Driver busy-time TPS is not in the JSON; board and tape phases are paced, so the derived qps above is the unit.\n")


def cpu():
    N, O = runs4()
    print("| arm | board A | board B | board v3.1.0 A / B | tape A | tape B | tape v3.1.0 A / B | open A | close A |")
    print("|---|---|---|---|---|---|---|---|---|")
    def s(d, a, k):
        if not d:
            return None
        return sum(v for kk, v in d["server_cpu_s"][a].items() if kk.endswith(k))
    for a in ARMS:
        r = []
        for key in ("board", "tape"):
            r += [f"{s(N['A'], a, key):.2f} s" if N["A"] else "n/a", f"{s(N['B'], a, key):.2f} s" if N["B"] else "n/a",
                  f"{s(O['A'], a, key):.2f} s / {s(O['B'], a, key):.2f} s"]
        print(f"| {a} | " + " | ".join(r) + f" | {s(N['A'], a, 'open'):.2f} s | {s(N['A'], a, 'close'):.2f} s |")
    print()


def tail():
    N, O = runs4()
    print("| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | v3.1.0 run A p95 |")
    print("|---|---|---|---|---|---|---|---|")
    for day in (1, 2, 3):
        for a in ARMS:
            p, pb, po = P(N["A"], day, "board", a), P(N["B"], day, "board", a), P(O["A"], day, "board", a)
            print(f"| {day} | {a} | {p['mean_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {pb['p95_us']:,.1f} µs | {po['p95_us']:,.1f} µs |")
    print()


def hdr(h):
    return " ".join(f"{k}={v}" for k, v in h.items())


def life():
    N, _ = runs4()
    for k in "AB":
        d = N[k]
        print(f"run {k}:\n")
        print("| t (s) | day | tag | managed | creates | extends | drops | entries |")
        print("|---|---|---|---|---|---|---|---|")
        for e in d["evidence"]:
            if "on" not in e or not e["on"]["header"]:
                continue
            kv = e["on"]["header"]
            if not e["tag"].startswith(("trading-block-3", "trading-block-9", "trading-block-12", "overnight")):
                continue
            ent = "; ".join(f"{x['rel_name']}:{x['state']}/{x['pages']}p" for x in e["on"]["entries"])
            print(f"| {e['t_wall_s']} s | {e['day']} | {e['tag']} | {kv['managed']} Cabins | {kv['creates']} creates | {kv['extends']} extends | {kv['drops']} drops | {ent} |")
        print(f"\nfinal header: `{hdr(d['final']['on']['header'])}`")
        print("verify per day: " + ", ".join("PASS" if not v["mismatches"] else "FAIL" for v in d["verify"]) + "\n")


def pct():
    N, _ = runs4()
    d = N["A"]
    print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in d["phases"]:
        if p["phase"].startswith(("load", "d1-", "d2-")) or p["phase"].startswith("d3-"):
            print(f"| {p['phase']} | " + pct_row(p) + " |")
    print()


def pgcmp():
    N, _ = runs4()
    PG = runs(new, "pg-s4")
    print("| group | PG18 day 1 median qps (fresh, n runs) | KDS off | KDS on | KDS declared | off/PG | on/PG | declared/PG |")
    print("|---|---|---|---|---|---|---|---|")
    return PG


if __name__ == "__main__":
    {"tp": tp, "cross": cross, "cpu": cpu, "tail": tail, "life": life, "pct": pct}[sys.argv[1]]()
