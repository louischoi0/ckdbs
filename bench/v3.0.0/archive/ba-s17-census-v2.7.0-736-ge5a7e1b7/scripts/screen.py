#!/usr/bin/env python3
"""Mark census runs that overlapped foreign load. usage: screen.py <kds dir> <samples.jsonl>
A run's window is [mtime - 30 s, mtime] (the JSON is written when the run ends; setup + warm-up + span is ~20-25 s).
Contaminated when, in the window, (a) a process named kds_tests / ctest / cc1plus / cmake is among the busy foreign
processes, or (b) CPUs 1 and 3 (no server and no client of this census is ever pinned to them) averaged over 10 % busy.
Prints the contaminated file names, one per line, and a summary on stderr."""
import glob, json, os, statistics as st, sys

d, sf = sys.argv[1], sys.argv[2]
S = [json.loads(l) for l in open(sf)]
bad = []
n = 0
for f in sorted(glob.glob(os.path.join(d, "*.json"))):
    m = os.path.getmtime(f)
    w = [s for s in S if m - 30 <= s["t"] <= m]
    n += 1
    if not w:
        bad.append((os.path.basename(f), "no samples"))
        continue
    named = any(any(k in x for k in ("kds_tests", "ctest", "cc1plus", "cmake")) for s in w for x in s["foreign"])
    idle = st.mean((s["busy"]["1"] + s["busy"]["3"]) / 2 for s in w)
    if named or idle > 10:
        bad.append((os.path.basename(f), f"named={named} idle_cpus_busy={idle:.0f}%"))
for b, why in bad:
    print(b)
    print(f"{b}: {why}", file=sys.stderr)
print(f"{len(bad)} of {n} runs flagged", file=sys.stderr)
