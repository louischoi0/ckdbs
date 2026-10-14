import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

starts, ends = timeline_windows()
groups = {
    "s2": lambda t: "s2-" in t and "b1-" not in t and not t.startswith("rr-"),
    "rr": lambda t: t.startswith("rr-"),
    "b1": lambda t: "b1-s2" in t,
    "s3": lambda t: "-s3-" in t or t.startswith("s3-"),
    "s1u": lambda t: t.startswith("s1u"),
    "s1b": lambda t: "s1b" in t,
    "s4": lambda t: t in ("s4", "cd2-s4"),
}
for g, f in groups.items():
    tags = [t for t in ends if f(t)]
    pre = []
    for t in tags:
        p = t if os.path.exists(f"{L}/{t}.precheck") else t + "-off"
        pre.append(precheck(p)[0])
    print(g, len(tags), min(pre), max(e for t, (e, rc) in ends.items() if t in tags))
