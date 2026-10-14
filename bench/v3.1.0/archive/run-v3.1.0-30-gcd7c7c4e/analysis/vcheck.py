import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

starts, ends = timeline_windows()
samples = monitor_samples()
print("| cell | window UTC (after gate) | samples | build/test | heavy>=50% | minor 10-50% | who | gate wait | loadavg at start |")
print("|---|---|---|---|---|---|---|---|---|")
bad = []
for tag, (e, rc) in sorted(ends.items(), key=lambda kv: kv[1][0]):
    try:
        tm, la, gw, procs = precheck(tag if os.path.exists(f"{L}/{tag}.precheck") else tag + "-off")
    except Exception as ex:
        tm, la, gw, procs = starts.get(tag), "?", 0, "?"
    n, b, h, mi, det = validity(tag, samples, ts(tm), ts(e))
    flag = " **FLAG**" if (b or h) else ""
    if b or h:
        bad.append(tag)
    print(f"| `{tag}` | {tm[11:19]} to {e[11:19]} | {n} | {b} | {h} | {mi} | {','.join(det) or '-'} | {gw} s | {la} |{flag}")
print()
print("flagged:", bad)
