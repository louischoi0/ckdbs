#!/usr/bin/env python3
"""flag.py DIR: moves every run of DIR whose 1 s pre-start sample or during-run sample shows a CPU other than
6/7 above 0.5 busy (or that started beside a compile/ctest) into DIR-excluded/<n>-round<k>-..., prints the count."""
import ast, glob, os, re, shutil, sys
d = sys.argv[1]
base = f"/home/cdkbs/bench-runs/ew/{d}"
ex = f"{base}-excluded"
os.makedirs(ex, exist_ok=True)
moved = 0
for h in sorted(glob.glob(f"{base}/n*.host.txt")):
    t = open(h).read()
    bf = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
    qb = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
    comp = re.search(r"competing_compile_or_ctest=(.*)", t).group(1)
    bad = comp != "none" or any(v > 0.5 for x in (bf, qb) for k, v in x.items() if k not in (6, 7))
    if not bad:
        continue
    stem = h[:-len(".host.txt")]
    k = 1
    while glob.glob(f"{ex}/round{k}-{os.path.basename(stem)}.*"):
        k += 1
    for f in glob.glob(stem + ".*"):
        shutil.move(f, f"{ex}/round{k}-{os.path.basename(f)}")
    moved += 1
for f in ("analysis.json", "pooled.json"):
    if os.path.exists(f"{base}/{f}"):
        os.remove(f"{base}/{f}")
print(d, "moved", moved)
