"""Median of per-run p50 (and pooled-run median of the per-run p50s) for the previous closes' B' arm, to set beside this run.
BJ close (v2.7.0-757-ge458e272): arms A = e5a7e1b7, B = e458e272; its B' binary is byte-identical to this run's A.
BH close (v2.7.0-712-g4015736d): arms A = 10593366, B = 4015736d.
"""
import glob, json, os, statistics

BASE = "/home/cdkbs/ckdbs/.claude/worktrees/bk-keystone-48/bench/v3.0.0/archive"
OUT = {}
for name, dirs, shapes in (
        ("BJ", ("bj-close-v2.7.0-757-ge458e272",),
         {"lit-point": "update-pk", "insert": "insert-named", "select-pk": "select-pk", "delete-pk": "delete-pk", "ping": "ping"}),
        ("BH", ("bh-close-v2.7.0-712-g4015736d",),
         {"insert": "insert", "insert-again": "insert-again", "select-pk": "select-pk", "update-pk": "update-pk",
          "delete-pk": "delete-pk", "range100": "range100", "ping": "ping"})):
    for cell in ("c1", "c2"):
        for rows in (200, 1000, 10000):
            fs = sorted(glob.glob(f"{BASE}/{dirs[0]}/overhead-{cell}/{cell}-n{rows}-r*.json"))
            if not fs:
                fs = sorted(glob.glob(f"{BASE}/{dirs[0]}/overhead-{cell}/n{rows}-r*.json"))
            js = [json.load(open(f)) for f in fs]
            if not js:
                continue
            for src, dst in shapes.items():
                for L in "AB":
                    try:
                        v = statistics.median(j["arms"][L][src]["p50_us"] for j in js)
                    except KeyError:
                        continue
                    OUT[f"{name}|{cell}|{rows}|{dst}|{L}"] = (round(v, 2), len(js))
for k, v in sorted(OUT.items()):
    print(k, v)
json.dump(OUT, open("/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/prev_close.json", "w"), indent=0)
