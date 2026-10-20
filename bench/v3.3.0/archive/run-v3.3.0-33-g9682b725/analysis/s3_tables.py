import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *

SHAPES = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre",
          "loans-by-daterange", "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]
COLS = [(n, m) for n in (200, 1000, 10000) for m in ("none", "all")]
PGPFX = ["", "r2-", "r3-"]


def q(d, sh):
    p = ph(d, sh)
    return 1e6 / p["mean_us"] if p else None


def cellq(kind, c, n, m, sh):
    """list of derived qps over runs. kind: new|old|pg"""
    if kind == "new":
        R = runs(new, f"s3-c{c}-n{n}-{m}")
    elif kind == "old":
        R = runs(lambda t: old(3, t), f"s3-c{c}-n{n}-{m}")
    else:
        R = runs(new, f"pg-s3-n{n}-{m}")
    return [q(d, sh) for d in R if ph(d, sh)]


def table(kind, c, other=None):
    print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
    print("|---|" + "---|" * len(COLS))
    for sh in SHAPES:
        row = []
        for n, m in COLS:
            a = med(cellq(kind, c, n, m, sh))
            if other:
                b = med(cellq(other, c, n, m, sh))
                row.append(ratio(a, b))
            else:
                row.append(f0(a, " qps"))
        print(f"| {sh} | " + " | ".join(row) + " |")
    print()


def spreads(c):
    print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
    print("|---|" + "---|" * len(COLS))
    for sh in SHAPES:
        print(f"| {sh} | " + " | ".join(sp(spread(cellq("new", c, n, m, sh))) for n, m in COLS) + " |")
    print()


def geo(kind, other, c=1):
    import math
    out = []
    for n, m in COLS:
        r = []
        for sh in SHAPES:
            a, b = med(cellq(kind, c, n, m, sh)), med(cellq(other, c, n, m, sh))
            if a and b:
                r.append(a / b)
        out.append((n, m, math.exp(sum(math.log(x) for x in r) / len(r)) if r else None, min(r) if r else None, max(r) if r else None))
    print("| cells | geometric mean | min | max |")
    print("|---|---|---|---|")
    for n, m, g, lo, hi in out:
        print(f"| n={n:,} {m} | {g:.2f}x | {lo:.2f}x | {hi:.2f}x |")
    print()


def pct(tag):
    d = new(tag)
    print(f"#### `{tag}` run 1\n")
    print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in d["phases"]:
        print(f"| {p['phase']} | " + pct_row(p) + " |")
    print()


def plans():
    print("| shape | n=200 none | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|")
    ds = [new("s3-c1-n200-none"), new("s3-c1-n10000-none"), new("s3-c1-n10000-all")]
    for sh in SHAPES[1:-1]:
        print(f"| {sh} | " + " | ".join("+".join(d["meta"]["plans"].get(sh, ["?"])) if d else "n/a" for d in ds) + " |")
    print()


if __name__ == "__main__":
    w = sys.argv[1]
    if w == "tp":
        print("`cores = 1`:\n"); table("new", 1)
        print("`cores = 8`:\n"); table("new", 8)
    elif w == "delta":
        print("`cores = 1`:\n"); table("new", 1, "old")
        print("`cores = 8`:\n"); table("new", 8, "old")
    elif w == "sp":
        spreads(1); spreads(8)
    elif w == "pg":
        print("Ratio of KDS `cores = 1` median qps over PostgreSQL 18.6 median qps (fresh, same window):\n")
        table("new", 1, "pg")
        print("PostgreSQL 18.6 median qps:\n")
        table("pg", 1)
        geo("new", "pg")
    elif w == "pct":
        for t in sys.argv[2:]:
            pct(t)
    elif w == "plans":
        plans()
