import sys, glob, os
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

rows = []
for f in glob.glob(f"{J}/*.json"):
    tag = os.path.basename(f)[:-5]
    try:
        d = load(tag)
    except Exception:
        continue
    p = ph(d, "load-cargos") or ph(d, "load-bars") and None
    if p is None:
        continue
    pre = L + f"/{tag}.precheck"
    if not os.path.exists(pre):
        continue
    rows.append((precheck(tag)[0], tag, p["p0_us"], p["p50_us"]))
for r in sorted(rows):
    print(r[0][5:19], f"{r[1]:<22}", "load-cargos p0 %.0f p50 %.0f" % (r[2], r[3]))
