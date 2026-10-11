#!/usr/bin/env python3
"""Reads the raw per-run JSON of the four passes and prints the markdown tables of the results file.
Percentiles are nearest-rank (bench_common.nearest_rank) over each run's pooled per-statement latencies;
each table cell is the median over the pairs of the per-run value; deltas are per-pair (B-A), median and
linear-interpolated quartiles over the pairs."""
import glob, json, math, re, statistics, sys

D = "/home/cdkbs/bench-runs/ew"
ARMS = ["ins-bulk", "upd-range", "sel-range"]
ARMNAME = {"ins-bulk": "INSERT 250 wide rows", "upd-range": "UPDATE 200 rows, 1 col", "sel-range": "SELECT 200-row range (control)"}
SIZES = [200, 1000, 10000]
PCTS = [0, 25, 50, 75, 95, 99]


def nr(s, p):
    if p == 0:
        return s[0]
    return s[max(1, -(-p * len(s) // 100)) - 1]


def quart(vals, p):
    s = sorted(vals); k = (len(s) - 1) * p / 100; f = math.floor(k); c = math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


def load(res):
    out = {}  # (n, arm, label) -> list of per-run dict (ordered by run)
    for f in sorted(glob.glob(f"{D}/{res}/n*-run*.json"), key=lambda x: (int(re.search(r"n(\d+)-", x).group(1)), int(re.search(r"run(\d+)", x).group(1)))):
        m = re.search(r"n(\d+)-run(\d+)", f)
        n, r = int(m.group(1)), int(m.group(2))
        d = json.load(open(f))
        for lab, arms in d["blocks"].items():
            for arm, bl in arms.items():
                lat = sorted(x for b in bl for x in b["lat_us"])
                e = {f"p{p}": nr(lat, p) for p in PCTS}
                e["mean"] = statistics.fmean(lat); e["n"] = len(lat); e["run"] = r
                e["eng"] = sum(b["engine_us"] for b in bl) / len(lat)
                out.setdefault((n, arm, lab), []).append(e)
    return out


def summ(res):
    data = load(res)
    cells = {}
    for (n, arm, lab) in list(data):
        if lab != "A":
            continue
        A = sorted(data[(n, arm, "A")], key=lambda e: e["run"]); B = sorted(data[(n, arm, "B")], key=lambda e: e["run"])
        c = {"runs": len(A), "n_ops": A[0]["n"]}
        for k in [f"p{p}" for p in PCTS] + ["mean", "eng"]:
            c["A_" + k] = statistics.median(a[k] for a in A); c["B_" + k] = statistics.median(b[k] for b in B)
            diffs = [b[k] - a[k] for a, b in zip(A, B)]
            c["d_" + k] = (statistics.median(diffs), quart(diffs, 25), quart(diffs, 75), sum(1 for x in diffs if x > 0))
        cells[(n, arm)] = c
    return cells


main, swap, ctl, ctlswap = summ("res-bulk"), summ("res-bulk-swap"), summ("res-bulk-ctl"), summ("res-bulk-ctl-swap")


def fd(t, w=6):
    return f"{t[0]:+.2f} [{t[1]:+.2f}, {t[2]:+.2f}]"


print("### QPS matrix (derived: 1,000,000 / p50 us per statement, serial single connection; median over pairs)\n")
print("| rows | statement | A (base) QPS | B (change) QPS | B vs A | C (A + dead padding) QPS | C vs A |")
print("|---|---|---|---|---|---|---|")
for n in SIZES:
    for arm in ARMS:
        c = main[(n, arm)]
        qa, qb = 1e6 / c["A_p50"], 1e6 / c["B_p50"]
        cc = ctl.get((n, arm))
        if cc:
            qca, qcb = 1e6 / cc["A_p50"], 1e6 / cc["B_p50"]
            cs = f"{qcb:,.0f} | {100 * (qcb / qca - 1):+.2f} %"
        else:
            cs = "not run | -"
        print(f"| {n} | {ARMNAME[arm]} | {qa:,.0f} | {qb:,.0f} | {100 * (qb / qa - 1):+.2f} % | {cs} |")
print()
for n in SIZES:
    print(f"### Latency distribution, rows = {n} (us; median over 8 pairs of each run's nearest-rank percentile; ops per run per server)\n")
    print("| statement | ops | bin | p0 | p25 | p50 | p95 | p99 | mean |")
    print("|---|---|---|---|---|---|---|---|---|")
    for arm in ARMS:
        c = main[(n, arm)]
        for lab, nm in (("A", "A"), ("B", "B")):
            print(f"| {ARMNAME[arm] if lab == 'A' else ''} | {c['n_ops'] if lab == 'A' else ''} | {nm} | " + " | ".join(
                f"{c[lab + '_p' + str(p)]:.1f}" for p in (0, 25, 50, 95, 99)) + f" | {c[lab + '_mean']:.1f} |")
    print()
print("### B - A per-pair deltas (us), main pass: median [Q1, Q3] over 8 pairs, count of pairs with B > A\n")
print("| rows | statement | d p0 | d p25 | d p50 | d p75 | d p99 | d mean | d engine us/op | pairs B>A at p50 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for n in SIZES:
    for arm in ARMS:
        c = main[(n, arm)]
        print(f"| {n} | {arm} | " + " | ".join(fd(c["d_" + k]) for k in ("p0", "p25", "p50", "p75", "p99", "mean", "eng")) + f" | {c['d_p50'][3]}/8 |")
print()
print("### C - A per-pair deltas (us), layout control, main pass (8 pairs)\n")
print("| rows | statement | d p50 | d p99 | d mean | d engine us/op |")
print("|---|---|---|---|---|---|")
for n in (1000, 10000):
    for arm in ARMS:
        c = ctl[(n, arm)]
        print(f"| {n} | {arm} | " + " | ".join(fd(c["d_" + k]) for k in ("p50", "p99", "mean", "eng")) + " |")
print()
print("### Slot-swap decomposition (us at p50): x = (d_main - d_swap)/2 is the binary effect, s = (d_main + d_swap)/2 the slot effect\n")
print("| rows | statement | B vs A: d_main | d_swap | x (binary) | s (slot) | C vs A: d_main | d_swap | x (binary) | s (slot) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for n in (1000, 10000):
    for arm in ARMS:
        row = []
        for m, s in ((main, swap), (ctl, ctlswap)):
            dm, ds = m[(n, arm)]["d_p50"][0], s[(n, arm)]["d_p50"][0]
            row += [f"{dm:+.2f}", f"{ds:+.2f}", f"{(dm - ds) / 2:+.2f}", f"{(dm + ds) / 2:+.2f}"]
        print(f"| {n} | {arm} | " + " | ".join(row) + " |")
print()
# noise floor: arms with no encode (sel-pk, ctl-ping) across the four passes, p50 and p99 per-pair medians
fl = {"p50": [], "p99": []}
for src in (main, ctl, swap, ctlswap):
    for (n, arm), c in src.items():
        if arm in ("sel-range",):
            for k in fl:
                fl[k].append(abs(c["d_" + k][0]))
print("noise floor, no-encode arms, all passes: max |median d p50| = %.2f us, max |median d p99| = %.2f us (n cells %d)" % (max(fl["p50"]), max(fl["p99"]), len(fl["p50"])))
# the same over the encode arms for reference
for src, nm in ((main, "B-A main"), (swap, "B-A swap"), (ctl, "C-A main"), (ctlswap, "C-A swap")):
    for arm in ("ins-bulk", "upd-range"):
        v50 = [c["d_p50"][0] for (n, a), c in src.items() if a == arm]
        v99 = [c["d_p99"][0] for (n, a), c in src.items() if a == arm]
        print(f"  {nm:9} {arm:11} p50 deltas over sizes {['%+.2f' % x for x in v50]} p99 {['%+.2f' % x for x in v99]}")
# engine accounting table
print("\n### Engine vs client (A binary, main pass, medians)\n")
print("| rows | statement | client mean us | engine us/op (sched_foreground_polled_us / ops) | client minus engine us |")
print("|---|---|---|---|---|")
for n in SIZES:
    for arm in ARMS:
        c = main[(n, arm)]
        print(f"| {n} | {arm} | {c['A_mean']:.1f} | {c['A_eng']:.1f} | {c['A_mean'] - c['A_eng']:.1f} |")
