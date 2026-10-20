import sys, re
sys.path.insert(0, "/home/cdkbs/bench-run")
from edlib import *

s = Doc(0)
s.rep("(rule 4b)", "(a saturation cell, not a polite load)")
s.rep("- **Scenario 2's row-set axis is not swept** (rule 9): its sizes", "- **Scenario 2's row-set axis is not swept**: its sizes")
s.rep("above their spreads (0.5 %, 3.4 %)", "above their spreads (0.5 %, 3.3 %)")
s.rep("per-row result cost is about 2x PostgreSQL's (7.5 to 9.3 µs against 3.6 to 4.1 µs per returned row, at every size from 252 to 60,480 bars).",
      "per-row result cost is about 2x PostgreSQL's (7.0 to 9.3 µs against 3.3 to 4.0 µs per returned row, each above its engine's own fixed round trip, 2.1x to 2.3x; the `bar-range` and `symbol-history` ratios are the same at 252, 1,008, 10,080 and 60,480 bars).")
s.rep("- None of the five files has had a `critics-developer` review.",
      "- The five files had a `critics-developer` review. Its corrections to the derived numbers and causal claims are commit `b3d2a2ae`; the remaining findings (a `bar-lookup` re-run, units, one spread definition, dangling rule references, the 2x per-row claim, and the cuts of the repeated stamp, contamination, finding-rule and insight text) are applied in the commit after it.")
s.rep("from 2026-10-20 03:37 UTC to 04:44 UTC on one", "from 2026-10-20 03:37 UTC to 04:44 UTC (plus the three-cell `bar-lookup` re-run of scenario 1, 05:09 to 05:12 UTC) on one")
s.rep("section 1 of each file)", "the Stamp below)")

finding = """## What counts as a finding

Stated once, for all four results files. A cell is run three times (the booker sweep and the saturation cells once). **Spread = (max - min) / median over the runs of one cell**; `bench/README.md` names no spread, and this is the definition every file here uses, for this run's cells and for the v3.1.0 cells re-derived from its JSON. A difference between two medians that is smaller than the larger of the two spreads is not a finding. A cell whose spread is above 20 % is reported and carries no delta claim. A ratio
against a control that cannot be affected (a pk point read, a Cabin-free `pk` group) sets the floor of a comparison that has no repeat: scenario 4's is about 1.10x. Cross-version deltas are held to the same rule with v3.1.0's spread.

"""
s.rep("## Headlines\n", finding + "## Headlines\n")

# the movers: bar-lookup note after the third bullet (cores = 8 one-connection reads)
s.rep("- **Eight contended bookers:", "- **Scenario 1 `bar-lookup` at `cores = 1` (-14.4 % in the matrix) is noise.** A re-run of the cell (three runs) shows the shape landing in a fast mode (12,515 to 12,657 qps) or a slow one (9,258 to 11,264 qps) at random, 3 of 6 runs each; the six-run spread is 28.6 %, and the fast-mode runs are -3.8 % against v3.1.0 within a 3.7 % spread. Not a finding; scenario 1, section 3.\n- **Eight contended bookers:")

s.rep("- `bench/v3.3.0/archive/scenarioN-v3.3.0-33-g9682b725/`: each scenario's per-cell JSON (KDS and PostgreSQL), driver stdout, server configs.", "- `bench/v3.3.0/archive/scenarioN-v3.3.0-33-g9682b725/`: each scenario's per-cell JSON (KDS and PostgreSQL), driver stdout, server configs; scenario 1 also holds the three `rerun-s1-c1-g-r*` cells.")
s.rep("The unrelated `kds_server` on port 15432 was not touched.", "The three `bar-lookup` re-run cells were clean too (validity log). The unrelated `kds_server` on port 15432 was not touched.")
s.save()
print("ok")
