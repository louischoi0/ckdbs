#!/usr/bin/env python3
"""compare.py OUTDIR [BASE_ARM [CAND_ARM ...]] [--md]

Per-scenario headline table over OUTDIR/<arm>-r<rep>/sN.json written by quad.sh.
Each cell is the median over reps of a throughput (qps = ops/elapsed from the driver, or
1e6/mean_us where the driver reports elapsed 0.0 - scenario 3 - which is exact for a serial
single-connection driver). `spread` is (max-min)/median over the arm's reps. A candidate
column shows its median, delta vs BASE_ARM, and `~` when |delta| <= max(spread of the two arms)
(inside the run's own noise floor: not a finding). Default arms: base, adv-off.
"""
import glob, json, os, statistics, sys

HEAD = [  # (scenario, label, phase-name matcher)
    ("s1", "s1 load-bars (insert)", "load-bars"),
    ("s1", "s1 result-insert", "result-insert"),
    ("s1", "s1 backtest-read (3-rel join, cold)", "backtest-read"),
    ("s1", "s1 backtest-replay (warm)", "backtest-replay"),
    ("s1", "s1 read-day-slice", "read-day-slice"),
    ("s1", "s1 read-bar-lookup (pk)", "read-bar-lookup"),
    ("s1", "s1 read-join-point (pk)", "read-join-point"),
    ("s1", "s1 agg-day-slice", "agg-day-slice"),
    ("s2", "s2 booking (txn, 8 bookers)", "booking"),
    ("s2", "s2 commit (committed TPS)", "commit"),
    ("s2", "s2 cargo-lookup", "cargo-lookup"),
    ("s2", "s2 charge-insert", "charge-insert"),
    ("s3", "s3 pk-user (control)", "pk-user"),
    ("s3", "s3 loans-by-user", "loans-by-user"),
    ("s3", "s3 loans-by-book", "loans-by-book"),
    ("s3", "s3 books-by-genre", "books-by-genre"),
    ("s3", "s3 loans-by-daterange", "loans-by-daterange"),
    ("s3", "s3 overdue", "overdue"),
    ("s3", "s3 join-loan-user", "join-loan-user"),
    ("s3", "s3 exists-correlated", "exists-correlated"),
    ("s3", "s3 count-by-user", "count-by-user"),
]
S4 = [("board", "d*-board"), ("tape10k", "d*-tape10k"), ("pk", "d*-pk"), ("open (insert)", "d*-open")]
S4ARMS = ["off", "on", "declared"]


def qps(p):
    return p["qps"] if p.get("qps", 0) > 0 else (1e6 / p["mean_us"] if p.get("mean_us") else 0.0)


def load(out, arm, s):
    res = []
    for d in sorted(glob.glob(os.path.join(out, "%s-r*" % arm))):
        f = os.path.join(d, s + ".json")
        if os.path.exists(f) and open(os.path.join(d, s + ".rc")).read().strip() == "0":
            res.append(json.load(open(f)))
    return res


def phase_vals(runs, name):
    vals = []
    for r in runs:
        for p in r["phases"]:
            if p["phase"] == name and p.get("ops", 0) > 0:
                vals.append(qps(p)); break
    return vals


def s4_vals(runs, pat, a):
    # pat like d*-board ; aggregate ops/elapsed over the days of one arm, per rep
    vals = []
    suffix = pat.split("-", 1)[1]
    for r in runs:
        ops = el = 0.0
        for p in r["phases"]:
            n = p["phase"]
            if n.endswith("[%s]" % a) and n.split("[")[0].split("-", 1)[-1] == suffix:
                ops += p["ops"]; el += p["elapsed_s"]
        if el > 0: vals.append(ops / el)
    return vals


def med(v): return statistics.median(v) if v else float("nan")
def spread(v): return (max(v) - min(v)) / med(v) * 100 if len(v) > 1 else float("nan")


def main():
    a = [x for x in sys.argv[1:] if x != "--md"]
    out, arms = a[0], a[1:] or ["base", "adv-off"]
    base, cands = arms[0], arms[1:]
    rows = []
    def row(label, getter):
        bv = getter(base)
        if not bv: return
        cells = ["%s" % label, "%d" % len(bv), "%.0f" % med(bv), "%.1f%%" % spread(bv)]
        for c in cands:
            cv = getter(c)
            if not cv: cells += ["-", "-", "-"]; continue
            d = (med(cv) / med(bv) - 1) * 100
            noise = max(spread(bv) if len(bv) > 1 else 0, spread(cv) if len(cv) > 1 else 0)
            cells += ["%.0f" % med(cv), "%+.1f%%%s" % (d, " ~" if abs(d) <= noise else ""), "%.1f%% (n=%d)" % (spread(cv), len(cv))]
        rows.append(cells)
    cache = {}
    def runs(arm, s):
        if (arm, s) not in cache: cache[(arm, s)] = load(out, arm, s)
        return cache[(arm, s)]
    for s, label, name in HEAD:
        row(label, lambda arm, s=s, name=name: phase_vals(runs(arm, s), name))
    for a4 in S4ARMS:
        for lab, pat in S4:
            row("s4 %s [%s arm]" % (lab, a4), lambda arm, pat=pat, a4=a4: s4_vals(runs(arm, "s4"), pat, a4))
    hdr = ["headline (qps, median of reps)", "n", base, "spread"]
    for c in cands: hdr += [c, "delta", "spread"]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "---|" * len(hdr))
    for r in rows: print("| " + " | ".join(r) + " |")
    print("\n`~` = |delta| <= the larger spread of the two arms (inside the noise floor).")


main()
