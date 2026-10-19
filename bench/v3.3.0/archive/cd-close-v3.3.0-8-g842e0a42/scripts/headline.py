#!/usr/bin/env python3
"""The headline matrix of the CD close: throughput per arm (derived from server CPU) and B - A against A' - A."""
import json, os, statistics as st

R = "/home/cdkbs/bench-runs/cd-close/out/"
med = st.median


def runs(files):
    out = []
    for f in files:
        p = R + f + ".json"
        if os.path.exists(p):
            out += json.load(open(p))["runs"]
    return out


def shape_vals(files, shape, path):
    rs = runs(files)
    return {arm: [next(a["server_cpu_us"] for a in r["arms"] if a["shape"] == shape and a["path"] == path and a["bin"] == arm) for r in rs]
            for arm in ("A", "A2", "B")}


def scan_vals(files, cell, path):
    rs = runs(files)
    rel = None
    for r in rs:
        for a in r["arms"]:
            if a["cell"] == cell and a["kind"] == "scan":
                rel, rows = a["rel"], a["rows"]
    out = {}
    for arm in ("A", "A2", "B"):
        v = []
        for r in rs:
            sc = next(a for a in r["arms"] if a["cell"] == cell and a["path"] == path and a["arm"] == arm)
            pt = next(a for a in r["arms"] if a["cell"] == "point:" + rel and a["path"] == path and a["arm"] == arm)
            v.append((sc["server_cpu_us"] - pt["server_cpu_us"]) / rows * 1000)
        out[arm] = v
    return out


def row(label, v, unit_div, unit):
    """unit_div: throughput = 1e6 / cpu_us (statements/s) or 1e9 / ns (rows/s)."""
    A, A2, B = v["A"], v["A2"], v["B"]
    dBA = [y - x for x, y in zip(A, B)]
    dAA = [abs(y - x) for x, y in zip(A, A2)]
    tp = lambda x: unit_div / x
    verdict = "inside" if abs(med(dBA)) <= max(dAA) else "**OUTSIDE**"
    sign = "slower" if med(dBA) > 0 else "faster"
    return (f"| {label} | {len(A)} | {tp(med(A)):,.0f} | {tp(med(A2)):,.0f} | {tp(med(B)):,.0f} | "
            f"{med(dBA):+.2f} {unit} ({100 * med(dBA) / med(A):+.1f} %, B {sign}) | {sum(1 for d in dBA if d > 0)} of {len(A)} | "
            f"{med(dAA):.2f} / {max(dAA):.2f} {unit} | {verdict} |")


print("| cell | runs | A | A' | B | B - A cost, paired median | B slower than A | A' - A typical / largest abs gap | verdict |")
print("|---|---|---|---|---|---|---|---|---|")
for label, files, shape in (("INSERT, no dropped column (positional, relation `i`)", ["u1", "u2", "u3", "u4"], "insert"),
                            ("INSERT, two dropped slot columns, not cleaned (column list)", ["u1", "u2", "u3", "u4"], "insert-dropped"),
                            ("INSERT, SPARE 2, no ALTER (column list)", ["u1", "u2"], "insert-spare"),
                            ("INSERT, no SPARE (column list)", ["u1", "u2"], "insert-plain"),
                            ("point SELECT by key, 1,000-row table", ["u1", "u2"], "select-pk"),
                            ("every-row UPDATE of 1,000 rows", ["u1", "u2", "u3", "u4"], "update-range")):
    for path in ("literal", "prepared"):
        unit = "us"
        print(row(f"{label}, {path}", shape_vals(files, shape, path), 1e6, unit).replace("| ", "| ", 1))
for cell, label in (("bn", "scan, net per row, 10,000 rows (`bn`)"),):
    for path in ("literal", "prepared"):
        print(row(f"{label}, {path}", scan_vals(["s1", "s2"], cell, path), 1e9, "ns"))
for cell, label in (("rows-200", "scan, net per row, 200 rows"), ("rows-1k", "scan, net per row, 1,000 rows")):
    for path in ("literal", "prepared"):
        print(row(f"{label}, {path}", scan_vals(["z"], cell, path), 1e9, "ns"))
