#!/usr/bin/env python3
"""Reads res/n<rows>-run<r>.json; prints per-cell statistics as JSON + markdown tables."""
import glob, json, math, os, re, statistics, sys

RES = sys.argv[1]
EXCL = {int(x) for x in sys.argv[2].split(",")} if len(sys.argv) > 2 and sys.argv[2] else set()
SIZES = [200, 1000, 10000]
ARMS = None


def nr(sorted_vals, p):
    rank = max(1, -(-int(p) * len(sorted_vals) // 100)) - 1
    return sorted_vals[rank]


def pct(vals):
    s = sorted(vals)
    return {"p0": s[0], "p25": nr(s, 25), "p50": nr(s, 50), "p75": nr(s, 75), "p99": nr(s, 99),
            "mean": statistics.fmean(s), "n": len(s)}


def q(vals, p):
    s = sorted(vals)
    # linear interpolation quartile on per-run diffs (n=8)
    k = (len(s) - 1) * p / 100
    f = math.floor(k); c = math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


data = {}  # (n, arm, label) -> list of per-run dicts
errs = 0
runs_seen = {}
for f in sorted(glob.glob(f"{RES}/n*-run*.json")):
    m = re.match(r".*/n(\d+)-run(\d+)\.json", f)
    n, r = int(m.group(1)), int(m.group(2))
    if r in EXCL:
        continue
    d = json.load(open(f))
    runs_seen.setdefault(n, []).append(r)
    for lab, arms in d["blocks"].items():
        for arm, bl in arms.items():
            lat = [x for b in bl for x in b["lat_us"]]
            eng = sum(b["engine_us"] for b in bl)
            ops = len(lat)
            e = pct(lat)
            e["engine_us_per_op"] = eng / ops
            e["run"] = r
            e["total_rows"] = d["rowcount"][lab][arm]["total_rows"]
            e["errors"] = d["errors"][lab][arm]
            errs += e["errors"]
            data.setdefault((n, arm, lab), []).append(e)

arms = []
for (n, arm, lab) in data:
    if arm not in arms:
        arms.append(arm)
out = {}
for n in SIZES:
    for arm in arms:
        A = sorted(data.get((n, arm, "A"), []), key=lambda e: e["run"])
        B = sorted(data.get((n, arm, "B"), []), key=lambda e: e["run"])
        if not A:
            continue
        cell = {"runs": len(A)}
        for key in ("p50", "p99", "mean", "engine_us_per_op", "p0", "p25", "p75"):
            diffs = [b[key] - a[key] for a, b in zip(A, B)]
            cell["d_" + key] = {"median": statistics.median(diffs), "q1": q(diffs, 25), "q3": q(diffs, 75),
                                "B_gt_A": sum(1 for x in diffs if x > 0), "n": len(diffs)}
            cell["A_" + key] = statistics.median([a[key] for a in A])
            cell["B_" + key] = statistics.median([b[key] for b in B])
        # pooled-over-runs distributions
        for lab, S in (("A", A), ("B", B)):
            pass
        cell["rows_equal"] = all(a["total_rows"] == b["total_rows"] for a, b in zip(A, B))
        cell["A_rows"] = A[0]["total_rows"]
        cell["B_rows"] = B[0]["total_rows"]
        out[f"{n}|{arm}"] = cell
json.dump({"cells": out, "errors_total": errs, "runs": {str(k): sorted(v) for k, v in runs_seen.items()}},
          open(f"{RES}/analysis.json", "w"), indent=1)

# pooled-run percentiles need the raw samples again
pooled = {}
for f in sorted(glob.glob(f"{RES}/n*-run*.json")):
    m = re.match(r".*/n(\d+)-run(\d+)\.json", f)
    n = int(m.group(1))
    if int(m.group(2)) in EXCL:
        continue
    d = json.load(open(f))
    for lab, a in d["blocks"].items():
        for arm, bl in a.items():
            pooled.setdefault((n, arm, lab), []).extend(x for b in bl for x in b["lat_us"])
pc = {f"{n}|{arm}|{lab}": pct(v) for (n, arm, lab), v in pooled.items()}
json.dump(pc, open(f"{RES}/pooled.json", "w"), indent=1)
print("runs per size:", {k: len(v) for k, v in runs_seen.items()}, "errors:", errs)
