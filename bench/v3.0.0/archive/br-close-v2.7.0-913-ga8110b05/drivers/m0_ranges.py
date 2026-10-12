import json, sys
r = json.load(open("/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/out/m0/analysis.json"))
def rng(cell, arms, key, scale=1.0):
    v = [r[f"{cell}|{a}|{n}"][key][0] for a in arms for n in (200, 1000, 10000) if f"{cell}|{a}|{n}" in r]
    ci = [r[f"{cell}|{a}|{n}"][key][1] for a in arms for n in (200, 1000, 10000) if f"{cell}|{a}|{n}" in r]
    return min(v), max(v), min(c[0] for c in ci), max(c[1] for c in ci), len(v)
PK = ["insert", "insert-again", "select-pk", "update-pk"]
for cell in ("relaxed-c1", "relaxed-c4", "aa-c1", "aa-c4"):
    for name, arms in (("pk shapes", PK), ("iso control", ["iso"]), ("INSERT+replicate", ["insert", "insert-again"]), ("SELECT", ["select-pk"]), ("UPDATE", ["update-pk"])):
        for key in ("p50", "p99", "qps", "cpu"):
            lo, hi, clo, chi, n = rng(cell, arms, key)
            print(f"{cell:11s} {name:17s} {key:4s} median-of-delta range {lo:+.2f}..{hi:+.2f}  CI envelope {clo:+.2f}..{chi:+.2f}  (n={n})")
for cell in ("group-c1", "group-c4"):
    for name, arms in (("INSERT+replicate", ["insert", "insert-again"]), ("iso", ["iso"])):
        for key in ("p50", "p99", "qps", "cpu"):
            lo, hi, clo, chi, n = rng(cell, arms, key)
            print(f"{cell:11s} {name:17s} {key:4s} median-of-delta range {lo:+.2f}..{hi:+.2f}  CI envelope {clo:+.2f}..{chi:+.2f}  (n={n})")
# sd of per-run p50 delta for the INSERT shapes
for cell in ("relaxed-c1", "relaxed-c4", "group-c1", "group-c4"):
    print(cell, "sd50 INSERT by rows", [round(r[f"{cell}|insert|{n}"]["sd50"], 2) for n in (200, 1000, 10000)])
# SHOW META
import glob
