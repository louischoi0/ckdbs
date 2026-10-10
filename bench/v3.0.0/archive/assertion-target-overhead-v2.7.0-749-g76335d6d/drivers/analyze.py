#!/usr/bin/env python3
"""analyze.py RESDIR -> RESDIR/analysis.json. Per (size, arm, label, run) percentiles from raw samples."""
import glob, gzip, json, math, re, statistics, sys

RES = sys.argv[1]


def nr(s, p):
    rank = max(1, -(-int(p) * len(s) // 100)) - 1
    return s[rank]


def pct(vals):
    s = sorted(vals)
    return {"p0": s[0], "p25": nr(s, 25), "p50": nr(s, 50), "p75": nr(s, 75), "p99": nr(s, 99),
            "mean": statistics.fmean(s), "n": len(s)}


def q(vals, p):
    s = sorted(vals)
    k = (len(s) - 1) * p / 100
    f, c = math.floor(k), math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


data = {}
for f in sorted(glob.glob(f"{RES}/n*-run*.json")):
    if f.endswith(".lat.json.gz"):
        continue
    m = re.match(r".*/n(\d+)-run(\d+)\.json", f)
    n, r = int(m.group(1)), int(m.group(2))
    d = json.load(open(f))
    assert d["verify_ok"], f
    raw = json.load(gzip.open(f[:-5] + ".lat.json.gz", "rt"))
    for lab, arms in raw.items():
        for arm, lat in arms.items():
            e = pct(lat)
            bl = d["blocks"][lab][arm]
            e["engine_us_per_op"] = d["engine"][lab][arm]["us"] / d["engine"][lab][arm]["polls"]
            e["run"] = r
            assert d["arms"][lab][arm]["errors"] == 0
            data.setdefault((n, arm, lab), []).append(e)
    for lab in ("A", "B"):
        for kind, key in (("create", "create_lat_us"), ("drop", "drop_lat_us")):
            e = pct(d[key][lab])
            e["run"] = r
            if kind == "create":
                e["engine_us_per_op"] = d["create_engine"][lab]["us"] / d["create_engine"][lab]["polls"]
            data.setdefault((n, kind, lab), []).append(e)

out = {}
keys = sorted({(n, arm) for (n, arm, lab) in data})
for (n, arm) in keys:
    A = sorted(data[(n, arm, "A")], key=lambda e: e["run"])
    B = sorted(data[(n, arm, "B")], key=lambda e: e["run"])
    cell = {"runs": len(A)}
    for key in ("p0", "p25", "p50", "p75", "p99", "mean", "engine_us_per_op"):
        if key not in A[0]:
            continue
        diffs = [b[key] - a[key] for a, b in zip(A, B)]
        cell["d_" + key] = {"median": statistics.median(diffs), "q1": q(diffs, 25), "q3": q(diffs, 75),
                            "B_gt_A": sum(1 for x in diffs if x > 0), "n": len(diffs), "all": diffs}
        cell["A_" + key] = statistics.median([a[key] for a in A])
        cell["B_" + key] = statistics.median([b[key] for b in B])
        cell["Aiqr_" + key] = [q([a[key] for a in A], 25), q([a[key] for a in A], 75)]
        cell["Biqr_" + key] = [q([b[key] for b in B], 25), q([b[key] for b in B], 75)]
    out[f"{n}|{arm}"] = cell
json.dump({"cells": out}, open(f"{RES}/analysis.json", "w"), indent=1)
print("cells", len(out), "runs", {k: len(v) for k, v in {(n): [1] for (n, _) in keys}.items()})
