#!/usr/bin/env python3
"""slot.py MAINDIR SWAPDIR SIZE -> binary effect x = (d_main - d_swap)/2, slot effect s = (d_main + d_swap)/2 (p50, us)."""
import json, sys
m = json.load(open(f"{sys.argv[1]}/analysis.json"))["cells"]
s = json.load(open(f"{sys.argv[2]}/analysis.json"))["cells"]
n = sys.argv[3]
print("| arm | main d p50 | swap d p50 | binary effect x | slot effect s | x as % of A p50 | main d mean | swap d mean | x mean |")
print("|---|---|---|---|---|---|---|---|---|")
for k in m:
    if not k.startswith(n + "|") or k not in s:
        continue
    dm, ds = m[k]["d_p50"]["median"], s[k]["d_p50"]["median"]
    mm, ms = m[k]["d_mean"]["median"], s[k]["d_mean"]["median"]
    x = (dm - ds) / 2; sl = (dm + ds) / 2
    print(f"| {k.split('|')[1]} | {dm:+.2f} | {ds:+.2f} | {x:+.2f} | {sl:+.2f} | {100 * x / m[k]['A_p50']:+.2f} % | {mm:+.2f} | {ms:+.2f} | {(mm - ms) / 2:+.2f} |")
