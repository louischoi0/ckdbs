#!/usr/bin/env python3
"""Per-rep QPS for chosen cells: perrep.py RUNSDIR (reuses compare.py's loaders)."""
import importlib.util, sys, re
src = open("/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-harness/compare.py").read()
src = src.replace("\nmain()\n", "\n")
ns = {}
exec(compile(src, "compare", "exec"), ns)
out = sys.argv[1]
arms = ["base", "c13", "c134"]
def show(label, getter):
    print("%-34s" % label, end="")
    for a in arms:
        print(" %s=[%s]" % (a, " ".join("%.0f" % v for v in getter(a))), end="")
    print()
for s, label, name in ns["HEAD"]:
    if name in ("load-bars", "result-insert", "backtest-replay", "backtest-read", "read-day-slice", "agg-day-slice"):
        show(label, lambda a, s=s, name=name: ns["phase_vals"](ns["load"](out, a, s), name))
for arm in ("off", "on", "declared"):
    for lab, pat in ns["S4"]:
        show("s4 %s [%s]" % (lab, arm), lambda a, pat=pat, arm=arm: ns["s4_vals"](ns["load"](out, a, "s4"), pat, arm))
