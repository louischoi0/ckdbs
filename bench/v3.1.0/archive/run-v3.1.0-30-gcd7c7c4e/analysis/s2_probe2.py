import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

for r in ("", "r2-", "r3-"):
    for c in ("c1-g", "c8-g", "c1-s", "c8-s"):
        d = load(f"{r}s2-{c}")
        e = {p["phase"]: p["errors"] for p in d["phases"] if p["errors"]}
        fe = {p["phase"]: p["first_error"][:150] for p in d["phases"] if p["errors"] and p["phase"] in ("freight-insert", "charge-insert", "cargo-lookup")}
        print(f"{r}s2-{c}", e, fe)
