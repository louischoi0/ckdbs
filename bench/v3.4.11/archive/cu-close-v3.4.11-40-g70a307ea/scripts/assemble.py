#!/usr/bin/env python3
"""Assemble the CU close results file from head.md and the generated tables.
Usage: assemble.py <head.md> <directory of the generated tables> <out.md>"""
import sys

O = sys.argv[2].rstrip("/") + "/"
head = open(sys.argv[1]).read()


def section(path, start, end=None):
    s = open(O + path).read()
    i = s.index(start)
    j = s.index(end, i) if end else len(s)
    return s[i:j].rstrip() + "\n"


q1 = open(O + "q1.md").read()
rows = open(O + "rows.md").read()
parts = {
    "@ROWS_POOLED@": section("rows.md", "| shape | paired run-cells", "\n### The shapes CU moved"),
    "@ROWS_CELLS@": section("rows.md", "| shape | rows | arm | units/run"),
    "@ROWS_HOST@": rows[:rows.index("\n")] + "\n",
    "@Q1_DELTAS@": section("q1.md", "| shape | rows | units per run | A | B - A (SE)", "\n### Off the set's shape: pooled"),
    "@Q1_POOLED@": section("q1.md", "| shape | paired run-cells", "\n### The shapes CU moved"),
    "@Q1_MOVED@": section("q1.md", "| shape | rows | units per run | A | A2 |", "\n### Every cell"),
    "@Q1_CELLS@": section("q1.md", "| shape | rows | arm | units/run"),
    "@LISTS_CPU@": section("lists.md", "| column | rows | n | LA walk", "\n## Latency per list"),
    "@LISTS_LAT@": section("lists.md", "| column | rows | n | arm | groups", "\n## Per value"),
    "@LISTS_PER@": section("lists.md", "| column | rows | n | L set CPU per value", "\n## Host"),
    "@LISTS_HOST@": section("lists.md", "load average", "\n## Plans"),
    "@LISTS_PLANS@": section("lists.md", "- `id`, 100,000 rows"),
    "@SMALL_CPU@": section("small.md", "| column | rows | n | LA walk", "\n## Latency per list"),
    "@SMALL_LAT@": section("small.md", "| column | rows | n | arm | groups", "\n## Per value"),
    "@SMALL_HOST@": section("small.md", "load average", "\n## Plans"),
    "@S0CMP@": section("s0cmp.md", "| column | rows | n | walk at CU-S0", "\ncells the close measured"),
    "@Q1_HOST@": q1[:q1.index("\n")] + "\n",
}
for k, v in parts.items():
    assert head.count(k) == 1, k
    head = head.replace(k, v.rstrip("\n"))
assert "@" not in head.replace("@{", ""), [w for w in head.split() if w.startswith("@")]
open(sys.argv[3], "w").write(head)
