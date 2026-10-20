import re
D = "/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/bench/v3.3.0/"
DESC = "v3.3.0-33-g9682b725"
F = {
    1: D + f"results-scenario1-backtest-{DESC}.md",
    2: D + f"results-scenario2-freight-{DESC}.md",
    3: D + f"results-scenario3-library-{DESC}.md",
    4: D + f"results-scenario4-cabinopt-days-{DESC}.md",
    0: D + f"summary-scenarios-{DESC}.md",
}


class Doc:
    def __init__(self, k):
        self.p = F[k]
        self.s = open(self.p).read()

    def rep(self, old, new, n=1):
        c = self.s.count(old)
        assert c == n, (self.p[-60:], old[:70], c)
        self.s = self.s.replace(old, new)

    def sub(self, pat, repl, n=1, flags=re.S):
        self.s, c = re.subn(pat, repl, self.s, flags=flags)
        assert c == n, (self.p[-60:], pat[:70], c)

    def section(self, start_pat, end_pat):
        """span (i, j) of text from the start regex match to the start of the end regex match after it"""
        m = re.search(start_pat, self.s, re.M)
        assert m, start_pat
        e = re.compile(end_pat, re.M).search(self.s, m.end())
        assert e, end_pat
        return m.start(), e.start()

    def replace_section(self, start_pat, end_pat, text):
        i, j = self.section(start_pat, end_pat)
        self.s = self.s[:i] + text + self.s[j:]

    def save(self):
        open(self.p, "w").write(self.s)


def stamp_rows(doc):
    """rows of the stamp table as {first cell: full line}"""
    i, j = doc.section(r"^## 1\. Stamp", r"^## 2\.")
    rows = {}
    order = []
    for l in doc.s[i:j].splitlines():
        if l.startswith("| ") and not l.startswith("| Field") and not l.startswith("|---"):
            k = l.split("|")[1].strip()
            rows.setdefault(k, []).append(l)
            order.append(k)
    return rows, order
