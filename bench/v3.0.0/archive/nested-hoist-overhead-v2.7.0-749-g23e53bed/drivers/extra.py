#!/usr/bin/env python3
"""Fragments for the results file: swap table, paired-block table, host table. Prints to fragments dir."""
import glob, json, math, re, statistics as st, sys

NH = "/home/cdkbs/bench-runs/nh"
FR = f"{NH}/fragments"
import os
os.makedirs(FR, exist_ok=True)
EXCL = {5}
SIZES = [200, 1000, 10000]


def q(vals, p):
    s = sorted(vals)
    k = (len(s) - 1) * p / 100
    f = math.floor(k); c = math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


def us(x):
    return f"{x/1000:,.2f} ms" if abs(x) >= 10000 else f"{x:,.1f} µs"


# ---- swap table (binary effect vs slot effect) ----
a = json.load(open(f"{NH}/res/analysis.json"))["cells"]
s = json.load(open(f"{NH}/res-swap/analysis.json"))["cells"]
ARMS = ["corr-exists-point", "corr-notexists-point", "corr-exists-scan", "unc-exists-point", "unc-exists-scan",
        "unc-in-point", "unc-in-scan", "unc-scalar-point", "nested-some-point", "nested-some-scan",
        "ctl-select-pk", "ctl-select-pk-again", "ctl-insert", "ctl-ping", "ctl-scan-filter", "ctl-scan-filter-again"]
out = ["| Arm | Rows | Δp50, B binary in slot B (7 runs) | Δp50, B binary in slot A, sign flipped to B − A (5 runs) | Effect that follows the binary | Effect that follows the slot |",
       "|---|---:|---:|---:|---:|---:|"]
for arm in ARMS:
    for n in SIZES:
        k = f"{n}|{arm}"
        x = a[k]["d_p50"]["median"]
        y = s[k]["d_p50"]["median"]  # swap: slot B holds A binary, so (slot B - slot A) = A - B
        out.append(f"| {arm} | {n:,} | {x:+.1f} µs | {-y:+.1f} µs | {(x + (-y))/2:+.1f} µs | {(x + y)/2:+.1f} µs |")
open(f"{FR}/swap.md", "w").write("\n".join(out) + "\n")

# ---- paired blocks ----
out = ["| Arm | Rows | Paired blocks | Median block Δp50 (B − A) | IQR of block Δ | Median relative Δ | Blocks with B slower |",
       "|---|---:|---:|---:|---:|---:|---:|"]
for arm in ["corr-exists-point", "corr-exists-scan", "unc-in-scan", "unc-exists-scan", "ctl-select-pk", "ctl-scan-filter", "ctl-scan-filter-again"]:
    for n in SIZES:
        diffs, rel = [], []
        for r in range(1, 9):
            if r in EXCL:
                continue
            d = json.load(open(f"{NH}/res/n{n}-run{r}.json"))
            for ba, bb in zip(d["blocks"]["A"][arm], d["blocks"]["B"][arm]):
                ma, mb = st.median(ba["lat_us"]), st.median(bb["lat_us"])
                diffs.append(mb - ma); rel.append(100 * (mb / ma - 1))
        out.append(f"| {arm} | {n:,} | {len(diffs)} | {diffs and st.median(diffs):+.2f} µs | [{q(diffs,25):+.2f}, {q(diffs,75):+.2f}] µs | "
                   f"{st.median(rel):+.2f} % | {sum(1 for x in diffs if x > 0)}/{len(diffs)} |")
open(f"{FR}/paired.md", "w").write("\n".join(out) + "\n")

# ---- host table ----
out = ["| Run | Rows | Load at start | Load at end | Competing build at start | Server CPU 6 busy | Client CPU 7 busy |", "|---:|---:|---:|---:|---|---:|---:|"]
for set_name, d in (("main", f"{NH}/res"), ("swap", f"{NH}/res-swap")):
    for f in sorted(glob.glob(f"{d}/n*-run*.host.txt"), key=lambda p: (int(re.search(r"run(\d+)", p).group(1)), int(re.search(r"/n(\d+)-", p).group(1)))):
        m = re.search(r"/n(\d+)-run(\d+)", f)
        t = open(f).read()
        la0 = re.search(r"loadavg_start=(\S+)", t).group(1)
        la1 = re.search(r"loadavg_end=(\S+)", t).group(1)
        comp = re.search(r"competing=(.*)", t).group(1)
        comp = "none" if comp == "none" else "yes: " + re.sub(r"^\d+ ", "", comp)[:60]
        cb = eval(re.search(r"cpu_busy=(.*)", t).group(1))
        label = f"{set_name} {m.group(2)}"
        out.append(f"| {label} | {int(m.group(1)):,} | {la0} | {la1} | {comp} | {cb[6]:.2f} | {cb[7]:.2f} |")
open(f"{FR}/host.md", "w").write("\n".join(out) + "\n")
print("ok")
