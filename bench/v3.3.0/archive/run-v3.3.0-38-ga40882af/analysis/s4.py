import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *

GROUPS = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]
ARMS = ["off", "on", "declared"]


def P(d, day, g, arm):
    return ph(d, f"d{day}-{g}[{arm}]")


def qd(p):
    return 1e6 / p["mean_us"]


def arm_table():
    A, B = now("s4"), now("cd2-s4")
    s = ("| group | day | off A (p50 / qps) | on A (p50 / qps) | declared A (p50 / qps) | on over off, mean (A / B) | declared over off, mean (A / B) |\n|---|---|---|---|---|---|---|\n")
    for g in GROUPS:
        for day in (1, 2, 3):
            cells = []
            for arm in ARMS:
                p = P(A, day, g, arm)
                cells.append(f"{p['p50_us']:,.1f} µs / {qd(p):,.0f} qps")
            r = []
            for arm in ("on", "declared"):
                ra = P(A, day, g, "off")["mean_us"] / P(A, day, g, arm)["mean_us"]
                rb = P(B, day, g, "off")["mean_us"] / P(B, day, g, arm)["mean_us"]
                r.append(f"{ra:.2f}x / {rb:.2f}x")
            s += f"| {g} | day {day} | " + " | ".join(cells) + " | " + " | ".join(r) + " |\n"
    return s


def vs_morning():
    s = "Derived qps now over the morning (same binary, old client), per arm, run A and run B:\n\n| group | day | off A | on A | declared A | off B | on B | declared B |\n|---|---|---|---|---|---|---|---|\n"
    for g in GROUPS:
        for day in (1, 2, 3):
            row = []
            for tag in ("s4", "cd2-s4"):
                for arm in ARMS:
                    a, b = P(now(tag), day, g, arm), P(mor(4, tag), day, g, arm)
                    row.append(f"{qd(a) / qd(b):.2f}x")
            s += f"| {g} | {day} | " + " | ".join(row) + " |\n"
    return s


def pooled_vs_morning():
    """per group and arm: median over 3 days x 2 runs of now/morning derived-qps ratio, with min/max"""
    s = "| group | arm | median of 6 (3 days x 2 runs) now/morning | min | max |\n|---|---|---|---|---|\n"
    for g in GROUPS:
        for arm in ARMS:
            rs = []
            for tag in ("s4", "cd2-s4"):
                for day in (1, 2, 3):
                    a, b = P(now(tag), day, g, arm), P(mor(4, tag), day, g, arm)
                    rs.append(qd(a) / qd(b))
            s += f"| {g} | {arm} | {med(rs):.3f}x | {min(rs):.2f}x | {max(rs):.2f}x |\n"
    return s


def pct_run(tag="s4"):
    d = now(tag)
    return pct_table(d)


def pg_pct():
    d = mor(4, "pg-s4")
    return pct_table(d)


def lifecycle(tag):
    d = now(tag)
    s = "| t (s) | day | tag | managed | creates | extends | drops | entries |\n|---|---|---|---|---|---|---|---|\n"
    for e in d["evidence"]:
        if e["tag"] not in ("trading-block-3", "trading-block-9", "trading-block-12", "overnight"):
            continue
        o = e["on"]
        h = o["header"]
        ent = "; ".join(f"{x['rel_name'].replace('_a_a', '_a')}:{x['state']}/{x['pages']}p" for x in o["entries"])
        s += f"| {e['t_wall_s']:.1f} s | day {e['day']} | {e['tag']} | {h['managed']} Cabins | {h['creates']} creates | {h['extends']} extends | {h['drops']} drops | {ent} |\n"
    f = d["final"]["on"]["header"]
    s += "\nfinal header: " + " ".join(f"{k}={v}" for k, v in f.items()) + "\n"
    s += "verify per day: " + ", ".join("PASS" if not v["mismatches"] else "FAIL" for v in d["verify"]) + "\n"
    return s


def cpu():
    s = "| arm | board A | board B | board morning A / B | tape A | tape B | tape morning A / B | open A | close A |\n|---|---|---|---|---|---|---|---|---|\n"

    def tot(d, arm, g):
        return sum(v for k, v in d["server_cpu_s"][arm].items() if k.endswith("-" + g))
    for arm in ARMS:
        a, b = now("s4"), now("cd2-s4")
        ma, mb = mor(4, "s4"), mor(4, "cd2-s4")
        s += (f"| {arm} | {tot(a, arm, 'board'):.2f} s | {tot(b, arm, 'board'):.2f} s | {tot(ma, arm, 'board'):.2f} s / {tot(mb, arm, 'board'):.2f} s | "
              f"{tot(a, arm, 'tape'):.2f} s | {tot(b, arm, 'tape'):.2f} s | {tot(ma, arm, 'tape'):.2f} s / {tot(mb, arm, 'tape'):.2f} s | "
              f"{tot(a, arm, 'open'):.2f} s | {tot(a, arm, 'close'):.2f} s |\n")
    return s


def pg_cmp():
    PG = Mo(4, "pg-s4")
    s = ("| group | PG18 day 1 (no index): p50 / median qps (spread) | KDS off | KDS on | KDS declared | off / PG | on / PG | declared / PG | morning: off / PG | on / PG | declared / PG |\n"
         "|---|---|---|---|---|---|---|---|---|---|---|\n")
    for g in ["open", "board", "tape200", "tape1k", "tape10k", "pk", "close"]:
        pq = [qd(ph(d, f"d1-{g}[pg]")) for d in PG]
        pp50 = med([ph(d, f"d1-{g}[pg]")["p50_us"] for d in PG])
        k, m = {}, {}
        for arm in ARMS:
            k[arm] = med([qd(P(now(t), 1, g, arm)) for t in ("s4", "cd2-s4")])
            m[arm] = med([qd(P(mor(4, t), 1, g, arm)) for t in ("s4", "cd2-s4")])
        s += (f"| {g} | {pp50:,.1f} µs / {med(pq):,.0f} qps ({sp(spread(pq))}) | " + " | ".join(f"{k[a]:,.0f} qps" for a in ARMS) + " | " +
              " | ".join(ratio(k[a], med(pq)) for a in ARMS) + " | " + " | ".join(ratio(m[a], med(pq)) for a in ARMS) + " |\n")
    return s


def ab():
    s = ("| group (day 1) | arm | old: 1 / 2 / 3 | old median | spread | new: 1 / 2 / 3 | new median | spread | new over old | read |\n|---|---|---|---|---|---|---|---|---|---|\n")
    for g in GROUPS:
        for arm in ARMS:
            o = [qd(P(now(f"ab-old{i}-s4"), 1, g, arm)) for i in (1, 2, 3) if now(f"ab-old{i}-s4")]
            n_ = [qd(P(now(f"ab-new{i}-s4"), 1, g, arm)) for i in (1, 2, 3) if now(f"ab-new{i}-s4")]
            if not o or not n_:
                continue
            lim = max(spread(o) or 0, spread(n_) or 0)
            dl = 100 * (med(n_) - med(o)) / med(o)
            rd = "no claim (spread > 20 %)" if lim > 20 else ("**finding**" if abs(dl) > lim else "inside spread")
            s += (f"| {g} | {arm} | " + " / ".join(f0(x) for x in o) + f" | {f0(med(o), ' qps')} | {sp(spread(o))} | " + " / ".join(f0(x) for x in n_) +
                  f" | {f0(med(n_), ' qps')} | {sp(spread(n_))} | {ratio(med(n_), med(o))} ({dl:+.1f} %) | {rd} |\n")
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        a = f.split(":")
        print(globals()[a[0]](*a[1:]))
