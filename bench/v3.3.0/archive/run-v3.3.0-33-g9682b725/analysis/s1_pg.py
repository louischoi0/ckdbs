import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *
from s1_tables import SHAPES, sweep, cellruns


def pgtables():
    PGR = runs(new, "pg-s1")
    K = cellruns("s1-c1-g")
    print(f"PG runs: {len(PGR)}, KDS runs: {len(K)}\n")
    print("| shape | KDS cold | PG cold | KDS/PG cold | KDS warm | PG warm | KDS/PG warm | KDS Cabin | PG index built | KDS/PG indexed-or-cabin |")
    print("|---|---|---|---|---|---|---|---|---|---|")

    def g(rr, sh, key):
        v = []
        for d in rr:
            s = sweep(d).get(sh)
            if s and s.get(key):
                v.append(s[key]["qps"])
        return med(v)
    for sh in SHAPES:
        kc, pc, kw, pw, kb, pb = g(K, sh, "cold"), g(PGR, sh, "cold"), g(K, sh, "warm"), g(PGR, sh, "warm"), g(K, sh, "cabin"), g(PGR, sh, "cabin")
        print(f"| {sh} | {f1(kc, ' qps')} | {f1(pc, ' qps')} | {ratio(kc, pc)} | {f1(kw, ' qps')} | {f1(pw, ' qps')} | {ratio(kw, pw)} | "
              f"{f1(kb, ' qps') if kb else '-'} | {f1(pb, ' qps') if pb else '-'} | {ratio(kb, pb) if kb and pb else '-'} |")
    print()
    print("| phase (median over runs) | KDS `c1-g` | PG18 | KDS/PG |")
    print("|---|---|---|---|")
    for p in new("s1-c1-g")["phases"]:
        n = p["phase"]
        a = [ph(d, n)["qps"] for d in K if ph(d, n)]
        b = [ph(d, n)["qps"] for d in PGR if ph(d, n)]
        if b:
            print(f"| {n} | {f1(med(a), ' qps')} | {f1(med(b), ' qps')} | {ratio(med(a), med(b))} |")
    print()
    print("| rows per BEGIN/COMMIT | KDS c1-g | PG18 | KDS/PG |")
    print("|---|---|---|---|")
    for b in (1, 10, 100, 1000):
        a = med([x["qps"] for d in K for x in d["meta"]["write_sweep"] if x["batch"] == b])
        p = med([x["qps"] for d in PGR for x in d["meta"]["write_sweep"] if x["batch"] == b])
        print(f"| {b} | {f0(a, ' rows/s')} | {f0(p, ' rows/s')} | {ratio(a, p)} |")
    print()
    print("| connections | KDS c1-g | KDS c8-g | PG18 | KDS c1-g/PG | KDS c8-g/PG |")
    print("|---|---|---|---|---|---|")
    K8 = cellruns("s1-c8-g")
    for n in (1, 2, 4, 8):
        a = med([x["qps"] for d in K for x in d["meta"]["concurrency_sweep"] if x["connections"] == n])
        a8 = med([x["qps"] for d in K8 for x in d["meta"]["concurrency_sweep"] if x["connections"] == n])
        p = med([x["qps"] for d in PGR for x in d["meta"]["concurrency_sweep"] if x["connections"] == n])
        print(f"| {n} | {f1(a, ' qps')} | {f1(a8, ' qps')} | {f1(p, ' qps')} | {ratio(a, p)} | {ratio(a8, p)} |")
    print()


def pgsizes():
    print("| bars | shape | KDS warm | PG warm | KDS/PG warm | KDS Cabin | PG index | KDS/PG indexed |")
    print("|---|---|---|---|---|---|---|---|")
    for n, bars in (("n252", 252), ("n1008", 1008), ("n10080", 10080)):
        K = runs(new, f"s1s-{n}-c1-g")
        PGR = runs(new, f"pg-s1s-{n}")
        for sh in SHAPES:
            def g(rr, key):
                return med([sweep(d)[sh][key]["qps"] for d in rr if sweep(d).get(sh) and sweep(d)[sh].get(key)])
            kw, pw, kc, pc = g(K, "warm"), g(PGR, "warm"), g(K, "cabin"), g(PGR, "cabin")
            print(f"| {bars:,} | {sh} | {f1(kw, ' qps')} | {f1(pw, ' qps')} | {ratio(kw, pw)} | {f1(kc, ' qps') if kc else '-'} | {f1(pc, ' qps') if pc else '-'} | {ratio(kc, pc) if kc and pc else '-'} |")
    print()


if __name__ == "__main__":
    {"pg": pgtables, "pgsizes": pgsizes}[sys.argv[1]]()
