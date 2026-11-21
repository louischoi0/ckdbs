#!/usr/bin/env python3
"""DG-S0 attribution (beside dg_s0.py): for each cell, the share of the
writers' samples of 10 ms or more (from the schedule) that start inside a fill
built ahead, inside a fill's fsync, or inside a checkpoint run (the server
log's "[checkpoint] run:" line, its start and end); and for the rolls of 80 ms
or more and the inserter's statements of 100 ms or more, what each overlaps.
Usage: dg_s0_attr.py [out-dir]
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dg_s0  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else dg_s0.OUT
KV = re.compile(r"(\w+)=(-?\d+)")


def checkpoints(d):
    js = glob.glob(os.path.join(d, "*.json"))[0]
    out = []
    for line in open(js[:-5] + ".s.log"):
        if "[checkpoint] run:" in line:
            kv = {k: int(v) for k, v in KV.findall(line)}
            s, e = kv.get("start_ns"), kv.get("end_ns")
            if s and e:
                out.append((s, e))
    return out


def inside(t, ivs):
    return any(a <= t <= b for a, b in ivs)


def hits(iv, ivs):
    return any(min(iv[1], b) > max(iv[0], a) for a, b in ivs)


for cell in sorted({os.path.basename(p) for p in glob.glob(os.path.join(OUT, "r*", "*"))}):
    n_w = in_fill = in_sync = in_ck = 0
    long_rolls, long_ins = [], []
    for d in sorted(glob.glob(os.path.join(OUT, "r*", cell))):
        r = dg_s0.load(d)
        if not r:
            continue
        fills = [(f["start_ns"], f["end_ns"]) for f in r["_fills"] if f.get("ahead") == 1]
        syncs = [(f["end_ns"] - f["fsync_us"] * 1000, f["end_ns"]) for f in r["_fills"]
                 if f.get("ahead") == 1]
        ck = checkpoints(d)
        w0, w1 = r["_w"]
        for c in r["clients"]:
            if c["role"] == "writer":
                for t, ns in c["slow"]:
                    if ns >= 10_000_000 and w0 <= t <= w1:
                        n_w += 1
                        in_fill += inside(t, fills)
                        in_sync += inside(t, syncs)
                        in_ck += inside(t, ck)
            if c["role"] == "inserter":
                for t, ns in c["slow"]:
                    if ns >= 100_000_000 and w0 <= t <= w1:
                        long_ins.append((ns // 1_000_000, hits((t, t + ns), fills), hits((t, t + ns), ck)))
        for x in r["_rolls"]:
            if x["hold_us"] >= 80_000:
                iv = (x["start_ns"], x["end_ns"])
                long_rolls.append((x["hold_us"] // 1000, x["sync_gate_us"] // 1000,
                                   hits(iv, fills), hits(iv, ck)))
    line = f"{cell}: writer samples >= 10 ms {n_w}"
    if n_w:
        line += f" (start in a fill {in_fill}, in its fsync {in_sync}, in a checkpoint run {in_ck})"
    nonfill_rolls = [x for x in long_rolls if not x[2]]
    nonfill_ins = [x for x in long_ins if not x[1]]
    line += (f"; rolls >= 80 ms {len(long_rolls)}, not in a fill {len(nonfill_rolls)}"
             f" [(hold ms, gate ms, in a checkpoint run)] {[(a, b, d) for a, b, _, d in nonfill_rolls]}")
    line += (f"; inserter stmts >= 100 ms {len(long_ins)}, not in a fill {len(nonfill_ins)}, "
             f"of those in a checkpoint run {sum(1 for x in nonfill_ins if x[2])}")
    print(line)
