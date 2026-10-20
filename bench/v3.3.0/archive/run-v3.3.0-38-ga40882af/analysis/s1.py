import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *

SHAPES = ["bar-lookup", "bar-range", "day-slice", "symbol-history", "cross-join", "point-join", "model-join"]
CELLS = [("c1-g", "cores = 1, group"), ("c8-g", "cores = 8, group"), ("c1-s", "cores = 1, strict")]
# rows a statement of the shape returns at the default size (60,480 bars)
ROWS = {"bar-range": 200, "symbol-history": 7560, "day-slice": 8, "model-join": 359}


def sv(rr, sh, key):
    out = []
    for d in rr:
        s = sweep(d).get(sh)
        if s and s.get(key):
            out.append(s[key]["qps"])
    return out


def finding(a, b, sa, sb):
    """read of a delta by the summary's rule"""
    if a is None or b is None:
        return "n/a"
    dlt = 100.0 * (a - b) / b
    lim = max(sa or 0, sb or 0)
    if lim > 20:
        return "spread > 20 %, no claim"
    return "**finding**" if abs(dlt) > lim else "inside the spread"


def matrix(cell):
    R, O = N("s1-" + cell), Mo(1, "s1-" + cell)
    s = ("| shape | cold now | warm now | spread | warm morning | spread | warm delta | read | cabin now | cabin morning | cabin delta |\n"
         "|---|---|---|---|---|---|---|---|---|---|---|\n")
    for sh in SHAPES:
        w, ow = sv(R, sh, "warm"), sv(O, sh, "warm")
        c, oc = sv(R, sh, "cabin"), sv(O, sh, "cabin")
        s += (f"| {sh} | {f1(med(sv(R, sh, 'cold')), ' qps')} | {f1(med(w), ' qps')} | {sp(spread(w))} | {f1(med(ow), ' qps')} | {sp(spread(ow))} | "
              f"{delta(med(w), med(ow))} | {finding(med(w), med(ow), spread(w), spread(ow))} | {f1(med(c), ' qps') if c else '-'} | "
              f"{f1(med(oc), ' qps') if oc else '-'} | {delta(med(c), med(oc)) if c and oc else '-'} |\n")
    return s


def pgratio(cell="c1-g"):
    R, O = N("s1-" + cell), Mo(1, "s1-" + cell)
    P = Mo(1, "pg-s1")
    s = ("| shape | KDS warm now | KDS warm morning | PG warm (morning's run) | KDS/PG now | KDS/PG morning | KDS cabin now | PG index | KDS cabin / PG index |\n"
         "|---|---|---|---|---|---|---|---|---|\n")
    for sh in SHAPES:
        w, ow, pw = med(sv(R, sh, "warm")), med(sv(O, sh, "warm")), med(sv(P, sh, "warm"))
        c, pc = med(sv(R, sh, "cabin")), med(sv(P, sh, "cabin"))
        s += (f"| {sh} | {f1(w, ' qps')} | {f1(ow, ' qps')} | {f1(pw, ' qps')} | {ratio(w, pw)} | {ratio(ow, pw)} | "
              f"{f1(c, ' qps') if c else '-'} | {f1(pc, ' qps') if pc else '-'} | {ratio(c, pc) if c and pc else '-'} |\n")
    return s


def phase_table(cell="c1-g"):
    R, O, P = N("s1-" + cell), Mo(1, "s1-" + cell), Mo(1, "pg-s1")
    s = ("| phase | ops | now qps | spread | morning qps | spread | delta | read | PG qps | KDS/PG now | KDS/PG morning |\n"
         "|---|---|---|---|---|---|---|---|---|---|---|\n")
    for p in R[0]["phases"]:
        n = p["phase"]
        a, b, c = pv(R, n), pv(O, n), pv(P, n)
        s += (f"| {n} | {p['ops']:,} | {f1(med(a))} | {sp(spread(a))} | {f1(med(b))} | {sp(spread(b))} | {delta(med(a), med(b))} | "
              f"{finding(med(a), med(b), spread(a), spread(b))} | {f1(med(c)) if c else '-'} | {ratio(med(a), med(c)) if c else '-'} | "
              f"{ratio(med(b), med(c)) if c else '-'} |\n")
    return s


def writes():
    s = "| rows per BEGIN/COMMIT | " + " | ".join(f"{c[0]} now" for c in CELLS) + " | " + " | ".join(f"{c[0]} morning" for c in CELLS) + " | PG |\n|---|" + "---|" * 7 + "\n"
    P = Mo(1, "pg-s1")
    for b in (1, 10, 100, 1000):
        row = []
        for fn in (lambda t: N(t), lambda t: Mo(1, t)):
            for c, _ in CELLS:
                v = [x["qps"] for d in fn("s1-" + c) for x in d["meta"]["write_sweep"] if x["batch"] == b]
                row.append(f"{f0(med(v), ' rows/s')} ({sp(spread(v))})")
        pv_ = [x["qps"] for d in P for x in d["meta"]["write_sweep"] if x["batch"] == b]
        s += f"| {b} | " + " | ".join(row) + f" | {f0(med(pv_), ' rows/s')} |\n"
    return s


def conc():
    s = "| connections | " + " | ".join(f"{c[0]} now" for c in CELLS) + " | " + " | ".join(f"{c[0]} morning" for c in CELLS) + " | PG |\n|---|" + "---|" * 7 + "\n"
    P = Mo(1, "pg-s1")
    for n in (1, 2, 4, 8):
        row = []
        for fn in (lambda t: N(t), lambda t: Mo(1, t)):
            for c, _ in CELLS:
                v = [x["qps"] for d in fn("s1-" + c) for x in d["meta"]["concurrency_sweep"] if x["connections"] == n]
                row.append(f"{f1(med(v), ' qps')} ({sp(spread(v))})")
        pv_ = [x["qps"] for d in P for x in d["meta"]["concurrency_sweep"] if x["connections"] == n]
        s += f"| {n} | " + " | ".join(row) + f" | {f1(med(pv_), ' qps')} |\n"
    return s


SIZES = [("n252", 252, 252), ("n1008", 1008, 252), ("n10080", 10080, 1260), ("", 60480, 7560)]


def sizes():
    s = ("| bars | shape | warm now | spread | warm morning | delta | read | PG warm | KDS/PG now | KDS/PG morning |\n"
         "|---|---|---|---|---|---|---|---|---|---|\n")
    for sz, bars, _ in SIZES:
        if sz:
            R, O, P = N(f"s1s-{sz}-c1-g"), Mo(1, f"s1s-{sz}-c1-g"), Mo(1, f"pg-s1s-{sz}")
        else:
            R, O, P = N("s1-c1-g"), Mo(1, "s1-c1-g"), Mo(1, "pg-s1")
        for sh in SHAPES:
            w, ow, pw = sv(R, sh, "warm"), sv(O, sh, "warm"), sv(P, sh, "warm")
            s += (f"| {bars:,} | {sh} | {f1(med(w), ' qps')} | {sp(spread(w))} | {f1(med(ow), ' qps')} | {delta(med(w), med(ow))} | "
                  f"{finding(med(w), med(ow), spread(w), spread(ow))} | {f1(med(pw), ' qps')} | {ratio(med(w), med(pw))} | {ratio(med(ow), med(pw))} |\n")
    return s


def perrow():
    """per returned row, bar-range (200 rows) and symbol-history (sessions per symbol rows), by size, warm sweep mean"""
    s = ("| bars | shape | rows returned | fixed round trip (bar-lookup mean) now / PG | KDS now µs/row | KDS morning µs/row | PG µs/row | KDS now / PG | KDS morning / PG |\n"
         "|---|---|---|---|---|---|---|---|---|\n")
    for sz, bars, sess in SIZES:
        if sz:
            R, O, P = N(f"s1s-{sz}-c1-g"), Mo(1, f"s1s-{sz}-c1-g"), Mo(1, f"pg-s1s-{sz}")
        else:
            R, O, P = N("s1-c1-g"), Mo(1, "s1-c1-g"), Mo(1, "pg-s1")

        def mean(rr, sh):
            return med([sweep(d)[sh]["warm"]["mean_us"] for d in rr if sweep(d).get(sh) and sweep(d)[sh].get("warm")])
        fx, fxp = mean(R, "bar-lookup"), mean(P, "bar-lookup")
        fxo = mean(O, "bar-lookup")
        for sh, rows in (("bar-range", 200), ("symbol-history", sess)):
            a, b, c = [(mean(rr, sh) - f) / rows for rr, f in ((R, fx), (O, fxo), (P, fxp))]
            s += (f"| {bars:,} | {sh} | {rows:,} | {fx:,.1f} µs / {fxp:,.1f} µs | {a:.2f} µs | {b:.2f} µs | {c:.2f} µs | {a / c:.2f}x | {b / c:.2f}x |\n")
    return s


def cellrun_pct(cell, label):
    d = now("s1-" + cell)
    return f"#### phases, run 1 of `s1-{cell}` ({label})\n\n" + pct_table(d)


def pg_pct(names):
    d = mor(1, "pg-s1")
    return pct_table(d, names, "PG")


if __name__ == "__main__":
    for f in sys.argv[1:]:
        print(globals()[f]())
