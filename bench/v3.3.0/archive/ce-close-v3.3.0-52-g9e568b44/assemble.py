#!/usr/bin/env python3
"""Fills @@table FILE REGEX N@@ lines of results.tmpl.md with the Nth contiguous markdown table whose first line matches REGEX.
usage: assemble.py OUT.md"""
import os, re, sys

R = os.path.dirname(os.path.abspath(__file__))
out = []
for line in open(f"{R}/results.tmpl.md").read().split("\n"):
    m = re.match(r"^@@table (\S+) (.+) (\d+)@@$", line)
    if not m:
        out.append(line)
        continue
    f, rx, n = m.group(1), m.group(2), int(m.group(3))
    lines = open(f"{R}/{f}").read().split("\n")
    hits = [i for i, l in enumerate(lines) if re.search(rx, l)]
    if len(hits) <= n:
        sys.exit(f"no table for {line}: hits={len(hits)}")
    i = hits[n]
    while i < len(lines) and lines[i].startswith("|"):
        out.append(lines[i])
        i += 1
open(sys.argv[1], "w").write("\n".join(out))
