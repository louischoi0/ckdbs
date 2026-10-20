import sys, re, os
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *

LOGS = "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/logs"
CELLS = [("s2-c1-g", 1, "group"), ("s2-c8-g", 8, "group"), ("s2-c1-s", 1, "strict"), ("s2-c8-s", 8, "strict")]
STM = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert",
       "operation-update", "org-update", "commit"]


def tps(d):
    return d["meta"]["tps"]


def R(base, o=False):
    return runs((lambda t: old(2, t)) if o else new, base)


def throughput(cells, title=None):
    print("| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | v3.1.0 median (cross-version) | v3.1.0 spread | delta of medians |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for tag, c, dur in cells:
        a, o = R(tag), R(tag, True)
        t = [tps(d) for d in a]
        ot = [tps(d) for d in o]
        cols = [f1(x, " tps") for x in t] + ["n/a"] * (3 - len(t))
        sprd = (max(t) / min(t) - 1) * 100 if len(t) > 1 else None
        osp = (max(ot) / min(ot) - 1) * 100 if len(ot) > 1 else None
        print(f"| `{tag}` | {c} | {dur} | " + " | ".join(cols) + f" | **{f1(med(t), ' tps')}** | {sp(sprd)} | "
              f"{f1(med(ot), ' tps')} | {sp(osp)} | {delta(med(t), med(ot))} |")
    print()


def shares():
    print("| cell | " + " | ".join(STM) + " | unattributed (BEGIN, refused attempts, client, socket) | booking mean |")
    print("|---|" + "---|" * (len(STM) + 2))
    for tag, c, dur in CELLS:
        for lab, d in (("now", new(tag)), ("v3.1.0", old(2, tag))):
            if not d:
                continue
            b = ph(d, "booking")
            tot = b["mean_us"] * b["ops"]
            parts = []
            s = 0
            for n in STM:
                p = ph(d, n)
                v = p["mean_us"] * p["ops"] / tot * 100
                s += v
                parts.append(f"{v:.1f} %")
            print(f"| `{tag}` ({lab}) | " + " | ".join(parts) + f" | {100 - s:.1f} % | {b['mean_us']:,.0f} µs |")
    print()


def conflict_lines(tag):
    p = f"{LOGS}/{tag}.serverlog"
    if not os.path.exists(p):
        return None
    t = open(p, errors="replace").read().splitlines()
    c = [l for l in t if "TXN_CONFLICT" in l]
    d = [l for l in c if "gave up after" in l]
    return len(c), len(c) - len(d), len(d)


def outcomes(tags):
    print("| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks | log conflict lines (row / descent) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for base in tags:
        for i, p in enumerate(PFX):
            d = new(p + base)
            if not d:
                continue
            m = d["meta"]
            o = m["outcomes"]
            ax = m["axes"]
            cl = conflict_lines(p + base)
            cls = f"{cl[0]} ({cl[1]} / {cl[2]})" if cl else "n/a"
            v = m["verify"]
            print(f"| `{p}{base}` | {i + 1} | {o['committed']:,} | {o['rejected-capacity']:,} | {o['rejected-credit']:,} | {o['conflicted']} | {m['abandoned']} | "
                  f"{ax['operations']} / {ax['organizations']} / {ax['read']} / {ax['commit']} | {v['failures']} / {v['checks']} | {cls} |")
    print()


def pct(cells):
    for tag in cells:
        d = new(tag)
        if not d:
            continue
        print(f"#### `{tag}` run 1\n")
        print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
        print("|---|---|---|---|---|---|---|---|---|")
        for n in ["booking"] + STM + ["manifest-scan", "voyage-rollup", "customer-statement"]:
            p = ph(d, n)
            if p:
                print(f"| {n} | " + pct_row(p) + " |")
        print()


def bookers():
    print("| bookers | durability | now: run 1 | run 2 | run 3 | median | spread | v3.1.0 (1 run, cross-version) | delta | conflicted (run 1) | abandoned (run 1) | verify failures (run 1) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for B in (1, 2, 4, 8, 16, 32):
        for dur, s in (("group", "g"), ("strict", "s")):
            if B == 8:
                base = f"s2-c8-{s}"
            elif B in (1, 2, 4, 16):
                base = f"x-b{B}-s2-c8-{s}"
            else:
                base = f"sat-b{B}-s2-c8-{s}"
            a = R(base)
            if not a:
                continue
            o = R(base, True)
            t = [tps(d) for d in a]
            cols = [f1(x, " tps") for x in t] + ["n/a"] * (3 - len(t))
            sprd = (max(t) / min(t) - 1) * 100 if len(t) > 1 else None
            ot = [tps(d) for d in o]
            m = a[0]["meta"]
            print(f"| {B} | {dur} | " + " | ".join(cols) + f" | **{f1(med(t), ' tps')}** | {sp(sprd)} | {f1(med(ot), ' tps')} | {delta(med(t), med(ot))} | "
                  f"{m['outcomes']['conflicted']} | {m['abandoned']} | {m['verify']['failures']} / {m['verify']['checks']} |")
    print()


def iso():
    print("| cell | isolation | tps | verify failures / checks | conflicted | v3.1.0 tps (cross-version) |")
    print("|---|---|---|---|---|---|")
    for c, base in ((1, "s2-c1-g"), (8, "s2-c8-g")):
        for lab, tag in (("server default (read-committed), median of 3", base), ("repeatable-read, 1 run", "rr-" + base)):
            a = R(tag) if "rr-" not in tag else R(tag, False)[:1]
            if not a:
                continue
            ov = R(tag, True)
            vf = "/".join(str(d["meta"]["verify"]["failures"]) for d in a)
            print(f"| `{tag}` | {lab} | {f1(med([tps(d) for d in a]), ' tps')} | {vf} of {a[0]['meta']['verify']['checks']} | "
                  f"{'/'.join(str(d['meta']['outcomes']['conflicted']) for d in a)} | {f1(med([tps(d) for d in ov]), ' tps')} |")
    print()


def b1():
    print("| cell | run 1 | run 2 | run 3 | median | spread | v3.1.0 median (cross-version) | delta | commit p50 (run 1) | commit share of booking |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for tag in ("b1-s2-c1-g", "b1-s2-c1-s"):
        a, o = R(tag), R(tag, True)
        t = [tps(d) for d in a]
        cols = [f1(x, " tps") for x in t] + ["n/a"] * (3 - len(t))
        sprd = (max(t) / min(t) - 1) * 100 if len(t) > 1 else None
        d = a[0]
        c, b = ph(d, "commit"), ph(d, "booking")
        print(f"| `{tag}` | " + " | ".join(cols) + f" | **{f1(med(t), ' tps')}** | {sp(sprd)} | {f1(med([tps(x) for x in o]), ' tps')} | "
              f"{delta(med(t), med([tps(x) for x in o]))} | {c['p50_us']:,.1f} µs | {c['mean_us'] * c['ops'] / (b['mean_us'] * b['ops']) * 100:.1f} % |")
    print()


def pg():
    print("| engine / cell | run 1 | run 2 | run 3 | median | spread | KDS/PG |")
    print("|---|---|---|---|---|---|---|")
    pgm = []
    rows = []
    P = runs(new, "pg-s2-b1")
    pt = [tps(d) if "tps" in d["meta"] else None for d in P]
    pgm = med(pt)
    for tag in ("b1-s2-c1-g", "b1-s2-c1-s"):
        t = [tps(d) for d in R(tag)]
        print(f"| KDS `{tag}` (this run) | " + " | ".join(f1(x, " tps") for x in t) + f" | {f1(med(t), ' tps')} | {sp(spread(t))} | {ratio(med(t), pgm)} |")
    cols = [f1(x, " tps") for x in pt]
    print(f"| PostgreSQL 18.6, `synchronous_commit = on`, fresh | " + " | ".join(cols) + f" | {f1(pgm, ' tps')} | {sp(spread(pt))} | 1.00x |")
    print()
    return P


if __name__ == "__main__":
    w = sys.argv[1]
    {"tp": lambda: throughput(CELLS), "shares": shares, "out": lambda: outcomes([c[0] for c in CELLS] + ["b1-s2-c1-g", "b1-s2-c1-s"]),
     "pct": lambda: pct(sys.argv[2:]), "bk": bookers, "iso": iso, "b1": b1, "pg": pg}[w]()
