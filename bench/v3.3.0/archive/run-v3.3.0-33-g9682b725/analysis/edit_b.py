import sys, re
sys.path.insert(0, "/home/cdkbs/bench-run")
from edlib import *

POINTER = ("Findings are read by the rule stated once in the summary (`summary-scenarios-" + DESC + ".md`, \"What counts as a finding\"); "
           "a cell whose spread is above 20 % is reported and carries no delta claim.")


def table_rows(doc, header_start):
    """yield (start, end) char span of the body rows of the table whose header line starts with header_start"""
    i = doc.s.index(header_start)
    k = doc.s.index("\n", i) + 1          # after header
    k = doc.s.index("\n", k) + 1          # after separator
    j = k
    while doc.s[j:j + 1] == "|":
        j = doc.s.index("\n", j) + 1
    return k, j


def map_rows(doc, header_start, fn):
    k, j = table_rows(doc, header_start)
    out = []
    for l in doc.s[k:j].splitlines():
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        cells = fn(cells)
        out.append("| " + " | ".join(cells) + " |")
    doc.s = doc.s[:k] + "\n".join(out) + "\n" + doc.s[j:]


# ================= scenario 1
d = Doc(1)
d.rep("**Row-count axis (rule 9).**", "**Row-count axis.**")
d.rep("## 7. The size sweep (rule 9)", "## 7. The size sweep")
d.sub(r"\*\*What counts as a finding\.\*\*[^\n]*\n", POINTER + "\n")
# examined units
d.rep("| no | 60,496 | same chain | 60,496 |", "| no | 60,496 rows | same chain | 60,496 rows |")
d.rep("(`reordered=1`, switch 0.19) | 5,393 |", "(`reordered=1`, switch 0.19) | 5,393 rows |")
d.rep("2,898 pages) | 5,744 |", "2,898 pages) | 5,744 rows |")
d.rep("(`reordered=1`, switch 0.84) | 2,873 |", "(`reordered=1`, switch 0.84) | 2,873 rows |")
d.rep("`memo_hits=358`) | 3,231 |", "`memo_hits=358`) | 3,231 rows |")
# duplicate lead-ins
d.rep("Scenario 1, `cores = 1`, `group`: planned (default) against `join_order = written`, 3 runs each, medians.\n\n", "")
d.rep("PG runs: 3, KDS runs: 3\n\n", "")
# runs column units in section 2
for tag in ("| 3 | `--seed 1 --bars-clustered btree", "| 3 | same |", "| 3 | `pg_scenario1_backtest.py", "| 3 each | "):
    pass
i, j = d.section(r"^## 2\. What was run", r"^## 3\.")
seg = d.s[i:j]
seg = re.sub(r"\| 3 each \| ", "| 3 runs each | ", seg)
seg = re.sub(r"\| 3 \| (?=[^|])", "| 3 runs | ", seg)
d.s = d.s[:i] + seg + d.s[j:]
# per-returned-row cost: subtract the fixed round trip
d.rep("| Per-row cost of a returned row | **7.5 µs per row** (`read-bar-range`, 200 rows, 1,494 µs) and 9.3 µs per row (`read-symbol-history`, 7,560 rows, 70.1 ms) | PostgreSQL: 3.6 µs and 4.1 µs per row |",
      "| Per-row cost of a returned row | **7.0 µs per row** (`read-bar-range`: (1,493.6 - 102.2) µs / 200 rows) and 9.3 µs per row (`read-symbol-history`: (70,055.3 - 102.2) µs / 7,560 rows) | PostgreSQL: 3.3 µs and 4.0 µs per row, after its own 70.9 µs fixed cost (2.1x and 2.3x) |")
d.rep("a row: a result row costs 7.5 to 9.3 µs against PostgreSQL's 3.6 to 4.1 µs.", "a row: a result row costs 7.0 to 9.3 µs above the fixed round trip against PostgreSQL's 3.3 to 4.0 µs.")
d.rep("a returned row costs 7.5 to 9.3 µs against 3.6 to 4.1 µs, at every size from 252 to 60,480 bars.", "a returned row costs 7.0 to 9.3 µs above the fixed round trip against 3.3 to 4.0 µs (2.1x to 2.3x), at 60,480 bars; the `bar-range` and `symbol-history` ratios to PostgreSQL are the same at 252, 1,008 and 10,080 bars (section 8).")
# contamination paragraph: summary only
d.sub(r"\*\*Contamination\.\*\* No KDS cell was discarded:.*?Scenario 1's PostgreSQL cells were not affected\.\n", "Contamination: none in this scenario's cells (summary, \"Contamination\"); the validity verdict of every cell is in the host table below.\n")
# bar-lookup discussion
bl = """**`bar-lookup` at `cores = 1`: a re-run.** The three `c1-g` runs of the matrix gave a warm `bar-lookup` of 11,052.4 / 11,263.5 / 12,514.6 qps (median 11,263.5), 14.4 % below v3.1.0's 13,152.9 qps, against spreads of 13.0 % now and 3.7 % then. That is below the 20 % exclusion line and larger than both spreads, so by the rule it was a candidate regression and the cell was repeated: `rerun-s1-c1-g-r1..3`, the same binary copy and arguments, 05:09 to 05:12 UTC, one at a time, no build, the monitor on, all three windows clean (`validity.log`).

| run | warm qps | warm p50 | cold qps | phase `read-bar-lookup` qps | mode |
|---|---|---|---|---|---|
| matrix run 1 (`s1-c1-g`) | 11,052.4 qps | 86.9 µs | 11,014.4 qps | 9,781.6 qps | slow |
| matrix run 2 (`r2-s1-c1-g`) | 11,263.5 qps | 88.7 µs | 11,982.6 qps | 12,551.5 qps | slow |
| matrix run 3 (`r3-s1-c1-g`) | 12,514.6 qps | 74.9 µs | 12,801.0 qps | 12,572.2 qps | fast |
| re-run 1 | 9,257.6 qps | 103.3 µs | 9,114.4 qps | 12,857.1 qps | slow |
| re-run 2 | 12,649.6 qps | 74.0 µs | 12,666.3 qps | 12,894.4 qps | fast |
| re-run 3 | 12,657.4 qps | 74.0 µs | 12,618.6 qps | 12,891.6 qps | fast |
| v3.1.0, three runs (cross-version) | 13,484.7 / 13,152.9 / 12,992.6 qps | - | - | 12,761.6 / 9,485.2 / 13,188.5 qps | fast x3 in the sweep, one slow in the phase |
| PostgreSQL 18.6 (fresh), three runs | 14,533.8 / 14,673.3 / 14,777.3 qps | - | - | - | - |

**Verdict: noise, not a regression.** The 100-statement sweep of this shape falls into one of two modes, a fast one (p50 74.0 to 74.9 µs, 12,515 to 12,657 qps) and a slow one (p50 86.9 to 103.3 µs, 9,258 to 11,264 qps), and which one a run lands in is not controlled: the slow mode took 3 of the 6 `c1-g` runs here (2 of the first 3, 1 of the 3 re-runs), and one of v3.1.0's three runs hit it in its separate 200-statement `read-bar-lookup` phase (9,485 qps), and the `c1-s` cell here has a slow run too (9,286 qps). Over all six runs the median is 11,889 qps and the spread 28.6 % (the re-runs alone: 12,650 qps, 26.9 %), above the 20 % line, so no delta is claimed for the shape at `cores = 1`. The fast-mode runs agree to 1.1 % (12,515 to 12,657 qps, median 12,650 qps) and sit 3.8 % under v3.1.0's three fast runs (13,153 qps, spread 3.7 %): that equals the larger spread, so it is not a finding by the rule either, though it has the sign of the 1 % to 11 % per-statement increase that scenario 2 shows (summary). `cores = 8` is steady where `cores = 1` is not (12,354 / 12,333 / 12,350 qps, spread 0.2 %, -0.3 % against v3.1.0), so the shape is not slower where the sweep always landed in the fast mode. Why `cores = 1` mixes the two modes is not explained by this run.

"""
d.rep("**Writes: the durability wait is the cost of a row.**", bl + "**Writes: the durability wait is the cost of a row.**")
d.rep("| `c1-g` `cross-join` warm |", "| `c1-g` `bar-lookup` warm | 11,263.5 qps | 13,152.9 qps | -14.4 % | **noise**: bimodal, six-run spread 28.6 %; fast-mode runs -3.8 % against a 3.7 % spread (section 3, re-run) |\n| `c1-g` `cross-join` warm |")
d.rep("13.0 % for `bar-lookup`. Two shapes are bimodal in a run (the sweep's 100 statements alternate between a fast and a slow mode): `point-join`\n(spread 1 % to 48 % depending on the cell, section 3 and section 6) and `bar-lookup` under `strict` (28.5 %).",
      "13.0 % for `bar-lookup` over its first three runs and 28.6 % over six (section 3). Two shapes are bimodal between runs (the sweep lands in a fast or a slow mode): `point-join`\n(spread 1 % to 48 % depending on the cell, section 3 and section 6) and `bar-lookup` (28.6 % at `c1-g`, 28.5 % under `strict`).")
# section 11 trim
d.replace_section(r"^## 11\. What the run says about the engine", r"^## 12\. Archive",
"""## 11. What the run says about the engine

1. **A heap-by-design append loses nothing to a btree here.** The driver's heap-shaped tables are hidden-key btrees now, the hidden key is issued in order, so every insert is a tail append: `result-insert` runs 742.1 qps against 749 qps for the BTREE copy at v3.1.0 (`heap-and-tuple.md` §3.1b).
2. **Where KDS loses to PostgreSQL is the row, not the plan.** A returned row costs 7.0 to 9.3 µs above the fixed round trip against 3.3 to 4.0 µs (2.1x to 2.3x), which puts the cost in result delivery (tuple decode, projection, KWP row-batch encoding) rather than in the examined-row walk. The client is `tools/kwp.py` for KDS and `tools/pg_wire.py` for PostgreSQL, each a Python
   decoder, so whether the factor of two lives in the engine or in the two decoders is **not separated by this run**.
3. **The join planner has nothing to win here** (section 6): `compare-all` reads 14x fewer pages after the reorder and runs at the same 57.7 qps, because its 17 ms is 2,872 returned rows at about 6 µs each.

""")
d.save()

# ================= scenario 2
d = Doc(2)
d.rep("**The row-set axis is not swept** (rule 9):", "**The row-set axis is not swept:**")
d.sub(r"Eight contended bookers\. Each cell ran three times;[^\n]*\n", POINTER + "\n")
map_rows(d, "| cell | run | committed", lambda c: [c[0], f"run {c[1]}", f"{c[2]} bookings", f"{c[3]} bookings", f"{c[4]} bookings", f"{c[5]} conflicts", f"{c[6]} bookings", c[7] + " conflicts", c[8] + " checks", re.sub(r"^(\d+) ", r"\1 lines ", c[9])])
map_rows(d, "| cell | isolation | tps", lambda c: [c[0], c[1], c[2], c[3].replace(" of 400", " of 400 checks"), (c[4] + " conflicts"), c[5]])
def bk(c):
    n = c[0]
    c[0] = f"{n} booker" + ("" if n == "1" else "s")
    c[9] = c[9] + " conflicts"
    c[10] = c[10] + " bookings"
    c[11] = c[11] + " checks"
    return c
map_rows(d, "| bookers | durability | now: run 1", bk)
d.sub(r"\*\*Contamination\.\*\* No KDS cell was discarded \(140 validity evaluations[^\n]*\n\n", "")
d.replace_section(r"^## 11\. What the run says about the engine", r"^## 12\. Archive",
"""## 11. What the run says about the engine

1. **A faster engine cannot show in this scenario without a faster device or a cheaper group commit.** The commit is 55.6 % of a booking with one booker (identical to PostgreSQL's to the microsecond) and 57.2 % under `group` at eight bookers and eight cores (50.5 % under `strict`); the statements around it (reads at 66 to 87 µs, writes at 45 to 62 µs p50) are not what a booking waits for.
2. **Eight cores do their job on commit-bound work**: `cores = 8` over `cores = 1` is 3.02x under `group` and 2.85x under `strict` for 8 bookers, and `group` over `strict` is 1.17x, the staged `strict` commit costing little over `group` once clients overlap (BA-S7).

""")
d.save()

# ================= scenario 3
d = Doc(3)
d.rep("| rows (rule 9) |", "| rows |")
d.sub(r"\*\*What counts as a finding\.\*\*[^\n]*\n", POINTER + " At 200 and 1,000 loans most cells carry 20 % to 43 % spreads (one run in three falls into a slow mode, section 10).\n")
d.rep("`cores = 1` and `cores = 8`:\n\n`cores = 1`:", "`cores = 1`:")
d.rep("Ratio of KDS `cores = 1` median qps over PostgreSQL 18.6 median qps (fresh, same window):\n\n", "")
d.rep("| no | 10,106 (pages 159) |", "| no | 10,106 rows (159 pages) |")
d.rep("| 4,652 (pages 61) |", "| 4,652 rows (61 pages) |")
d.sub(r"\*\*Contamination\.\*\* No KDS cell was discarded\. The PostgreSQL batch[^\n]*\n", "Contamination: none in the KDS cells; the PostgreSQL re-run is described in the summary (\"Contamination\").\n")
d.replace_section(r"^## 11\. What the run says about the engine", r"^## 12\. Archive",
"""## 11. What the run says about the engine

1. **KDS walks a row cheaper than PostgreSQL (about 24 ns against 34 ns) and returns a row dearer** (the `books-by-genre` figure in section 8). The crossover is why the equality shapes are ahead at 10,000 loans and the range and wide-result shapes are behind.
2. **The planner is conservative and costs nothing measurable here**: it keeps the written order unless it wins by the margin, and the planned arm is never faster than the written one on the three join shapes (section 6). This is the first data point on CB/CC in this scenario.

""")
d.save()

# ================= scenario 4
d = Doc(4)
d.rep("**Row-set axis (rule 9).**", "**Row-set axis.**")
d.rep("The\n`tape200` result is the row-count lesson (rule 9):", "The\n`tape200` result is the row-count lesson:")
d.rep("**Cross-version (now over v3.1.0, derived qps per arm, run A and run B):**\n\nDerived qps (1,000,000 / mean µs), run A, now over v3.1.0 (cross-version), and run B:\n\n", "**Cross-version (now over v3.1.0, derived qps (1,000,000 / mean µs) per arm, run A and run B):**\n\n")
d.rep("PG runs: 3\n\n", "")
d.sub(r"\*\*Contamination\.\*\* No cell was discarded\. Per-cell host stamps", "Per-cell host stamps")
d.replace_section(r"^## 10\. What the run says about the engine", r"^## 11\. Archive",
"""## 10. What the run says about the engine

1. **The Cabin's multiple over a walk moved because the walk improved, not the Cabin.** `off` is 1.18x and 1.20x v3.1.0 on the 10,000-row shapes, the per-row cost of an unserved equality fell from 34.6 to 26.5 ns (PostgreSQL's 40.5 ns), and the served arms did not move (a Cabin-served probe is a round trip, about 102 µs), so the multiple went from 1.99x/2.77x (v3.1.0, medians over the six days) to 1.78x/2.31x on `board`/`tape10k`.
   Scenario 3's integer-column equality walk does not show the same gain (`loans-by-user` 1.00x at 10,000 loans); scenario 1's (`day-slice` on `int32 session_no`) shows a smaller one (+7.3 % `c1-g`, +8.9 % `c1-s`). The walks differ in the filtered column's type (`varchar` symbol here, integers there) among other things, and this run does not separate them.
2. **The declared arm's tail is the open question this scenario keeps asking**: p95 779 to 794 µs against a 467 to 486 µs walk, in both runs, on three days, at two versions; it is a property of the declared Cabin's miss path and nothing in this run explains it.

""")
d.save()
print("ok")
