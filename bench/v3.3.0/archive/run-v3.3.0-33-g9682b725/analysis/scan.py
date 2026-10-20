import re, glob
D = "/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/bench/v3.3.0/"
fs = sorted(glob.glob(D + "results-scenario*-v3.3.0-33-g9682b725.md")) + glob.glob(D + "summary-scenarios-v3.3.0-33-g9682b725.md")
for f in fs:
    seen = {}
    for n, l in enumerate(open(f).read().splitlines(), 1):
        if re.search(r"rule (4b|9|\d)", l, re.I) and "README" not in l:
            print("RULE", f[-45:], n, l[:100])
        if "max/min" in l or "max minus" in l:
            print("SPREADDEF", f[-45:], n, l[:100])
        if l.startswith("|") and not l.startswith("|--"):
            cells = [c.strip() for c in l.strip().strip("|").split("|")]
            for i, c in enumerate(cells):
                if re.fullmatch(r"\d[\d,]*", c) and i > 0:
                    seen.setdefault(i, []).append((n, c))
    # report columns with bare ints, by line-range grouping
    for i, v in seen.items():
        print("BARE", f[-45:], "col", i, "lines", v[0][0], "..", v[-1][0], "n=", len(v), "e.g.", v[0][1])
