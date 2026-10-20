import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *


def pairs(fmt):
    """fmt like 'ab-{v}{i}-s1-c1-g' -> (old list, new list) of loaded JSON, pair order 1..3"""
    o = [now(fmt.format(v="old", i=i)) for i in (1, 2, 3)]
    n = [now(fmt.format(v="new", i=i)) for i in (1, 2, 3)]
    return [x for x in o if x], [x for x in n if x]


def read(a, b, sa, sb):
    lim = max(sa or 0, sb or 0)
    dl = 100 * (a - b) / b
    if lim > 20:
        return "no claim (spread > 20 %)"
    return "**finding**" if abs(dl) > lim else "inside the spread"


def row(label, o, n_, unit=" qps", lower_better=False):
    if not o or not n_:
        return ""
    lim = max(spread(o) or 0, spread(n_) or 0)
    r = med(n_) / med(o)
    f = f1 if med(o) < 100 else f0
    return (f"| {label} | " + " / ".join(f(x) for x in o) + f" | {f(med(o), unit)} | {sp(spread(o))} | " + " / ".join(f(x) for x in n_) +
            f" | {f(med(n_), unit)} | {sp(spread(n_))} | {r:.3f}x | {read(med(n_), med(o), spread(o), spread(n_))} |\n")


HEAD = ("| {what} | old client: pair 1 / 2 / 3 | old median | old spread | new client: pair 1 / 2 / 3 | new median | new spread | new over old | read |\n"
        "|---|---|---|---|---|---|---|---|---|\n")


def s1_sweep(fmt="ab-{v}{i}-s1-c1-g", shapes=None):
    O, Nn = pairs(fmt)
    sh = shapes or [s["shape"] for s in O[0]["meta"]["qps_sweep"]]
    s = HEAD.format(what="shape (warm qps)")
    for x in sh:
        s += row(x, [sweep(d)[x]["warm"]["qps"] for d in O], [sweep(d)[x]["warm"]["qps"] for d in Nn])
    for x in sh:
        if sweep(O[0]).get(x, {}).get("cabin"):
            s += row(x + " (cabin)", [sweep(d)[x]["cabin"]["qps"] for d in O], [sweep(d)[x]["cabin"]["qps"] for d in Nn])
    return s


def phases(fmt, names=None, key="qps"):
    O, Nn = pairs(fmt)
    s = HEAD.format(what="phase (qps)")
    for p in O[0]["phases"]:
        if names and p["phase"] not in names:
            continue
        s += row(p["phase"], pv(O, p["phase"], key), pv(Nn, p["phase"], key))
    return s


def s2_tps(fmt):
    O, Nn = pairs(fmt)
    s = HEAD.format(what="cell (tps)")
    s += row("tps", [d["meta"]["tps"] for d in O], [d["meta"]["tps"] for d in Nn], " tps")
    return s


def s2_stmts(fmt, names):
    O, Nn = pairs(fmt)
    s = "| statement (p50 µs) | old: 1 / 2 / 3 | old median | new: 1 / 2 / 3 | new median | new / old |\n|---|---|---|---|---|---|\n"
    for n in names:
        a, b = pv(O, n, "p50_us"), pv(Nn, n, "p50_us")
        s += f"| {n} | " + " / ".join(f1(x) for x in a) + f" | {f1(med(a), ' µs')} | " + " / ".join(f1(x) for x in b) + f" | {f1(med(b), ' µs')} | {med(b) / med(a):.3f}x |\n"
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        a = f.split(":")
        print(globals()[a[0]](*a[1:]))
