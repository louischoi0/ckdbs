import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *
from s4_tables import runs4, P, ARMS

GROUPS = ["open", "board", "tape200", "tape1k", "tape10k", "pk", "close"]


def pgcmp():
    N, _ = runs4()
    PG = runs(new, "pg-s4")
    print(f"PG runs: {len(PG)}\n")
    print("| group | PG18 day 1 (no index): p50 / median qps (spread) | KDS off | KDS on | KDS declared | off / PG | on / PG | declared / PG |")
    print("|---|---|---|---|---|---|---|---|")
    for g in GROUPS:
        pq = [1e6 / ph(d, f"d1-{g}[pg]")["mean_us"] for d in PG]
        p50 = med([ph(d, f"d1-{g}[pg]")["p50_us"] for d in PG])
        kq = {}
        for a in ARMS:
            v = [1e6 / P(N[k], 1, g, a)["mean_us"] for k in "AB" if N[k]]
            kq[a] = med(v)
        print(f"| {g} | {p50:,.1f} µs / {f0(med(pq), ' qps')} ({sp(spread(pq))}) | " + " | ".join(f0(kq[a], " qps") for a in ARMS) + " | "
              + " | ".join(ratio(kq[a], med(pq)) for a in ARMS) + " |")
    print()


def pgpct():
    PG = runs(new, "pg-s4")
    d = PG[0]
    print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in d["phases"]:
        print(f"| {p['phase']} | " + pct_row(p) + " |")
    print()


if __name__ == "__main__":
    {"cmp": pgcmp, "pct": pgpct}[sys.argv[1]]()
