#!/usr/bin/env python3
"""List cells whose host record is unclean or names competing tasks. usage: hostcheck.py OUTDIR w1 w2 ..."""
import json, sys
D = sys.argv[1].rstrip("/") + "/"
for f in sys.argv[2:]:
    for i, r in enumerate(json.load(open(D + f + ".json"))["runs"]):
        for a in r["arms"]:
            h = a["host"]
            if not h.get("clean", True) or h.get("competing"):
                print(f, i, a["shape"], a["path"], a["bin"], json.dumps(h)[:400])
