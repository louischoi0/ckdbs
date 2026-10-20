"""Move the three cells that overlap a >= 50 % foreign sample (rc_valid.py) out of the jsonl files so the resumable sweeps re-run them."""
import json, os
RC = "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost"
bad = {"cells-new.jsonl": (2, "values", 10000), "cells-old.jsonl": (3, "split", 1000), "cells-send-new.jsonl": (1, "send", 10000)}
os.makedirs(f"{RC}/contaminated", exist_ok=True)
for f, (rep, mode, size) in bad.items():
    keep, drop = [], []
    for l in open(f"{RC}/{f}"):
        d = json.loads(l)
        (drop if (d["rep"], d["mode"], d["size"]) == (rep, mode, size) else keep).append(l)
    assert len(drop) == 1, (f, len(drop))
    open(f"{RC}/contaminated/{f}", "a").writelines(drop)
    open(f"{RC}/{f}", "w").writelines(keep)
    print(f, "moved", (rep, mode, size))
for m in ("done.new", "done.old", "done.send.new", "all.done", "all-send.done"):
    if os.path.exists(f"{RC}/{m}"):
        os.remove(f"{RC}/{m}")
