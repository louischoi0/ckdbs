import sys, math
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *

SH = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre", "loans-by-daterange",
      "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]
SZ = [200, 1000, 10000]
MODES = ["none", "all"]
# rows a statement returns at 10,000 loans is not recorded by the driver; the ones known from the plans / morning text:
# books-by-genre ~125 rows


def q(d, sh):
    return 1e6 / ph(d, sh)["mean_us"]


def cellq(kind, c, n, m, sh, which="now"):
    if kind == "pg":
        rr = Mo(3, f"pg-s3-n{n}-{m}")
    elif which == "now":
        rr = N(f"s3-c{c}-n{n}-{m}")
    else:
        rr = Mo(3, f"s3-c{c}-n{n}-{m}")
    return [q(d, sh) for d in rr if ph(d, sh)]


def read_of(a, b, sa, sb):
    if a is None or b is None:
        return "n/a"
    lim = max(sa or 0, sb or 0)
    if lim > 20:
        return "no claim (spread > 20 %)"
    return "**finding**" if abs(100 * (a - b) / b) > lim else "inside spread"


def main_table(c):
    """one row per shape; columns: size/mode: now qps, morning qps, delta"""
    s = f"`cores = {c}`, derived qps (1,000,000 / mean µs), median of three runs; delta = now over the morning.\n\n"
    for n in SZ:
        s += f"**n = {n:,} loans**\n\n| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n"
        for sh in SH:
            row = f"| {sh} |"
            for m in MODES:
                a, b = cellq("k", c, n, m, sh), cellq("k", c, n, m, sh, "morn")
                row += (f" {f0(med(a), ' qps')} | {sp(spread(a))} | {f0(med(b), ' qps')} | {sp(spread(b))} | {delta(med(a), med(b))} | "
                        f"{read_of(med(a), med(b), spread(a), spread(b))} |")
            s += row + "\n"
        s += "\n"
    return s


def geomean(xs):
    xs = [x for x in xs if x]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else None


def geo_table():
    s = "| cell | geometric mean now over morning | min | max | shapes outside spread |\n|---|---|---|---|---|\n"
    for c in (1, 8):
        for n in SZ:
            for m in MODES:
                rt, out = [], 0
                for sh in SH:
                    a, b = cellq("k", c, n, m, sh), cellq("k", c, n, m, sh, "morn")
                    if med(a) and med(b):
                        rt.append(med(a) / med(b))
                        if read_of(med(a), med(b), spread(a), spread(b)) == "**finding**":
                            out += 1
                s += f"| cores {c}, n={n:,} {m} | {geomean(rt):.3f}x | {min(rt):.2f}x | {max(rt):.2f}x | {out} of 12 |\n"
    return s


def pg_ratio():
    s = "KDS `cores = 1` now over PostgreSQL (morning's run, reused), and the morning's KDS over the same PostgreSQL:\n\n"
    s += "| shape | " + " | ".join(f"n={n:,} {m}: now / morning" for n in SZ for m in MODES) + " |\n|---|" + "---|" * 6 + "\n"
    for sh in SH:
        row = f"| {sh} |"
        for n in SZ:
            for m in MODES:
                a, b, p = med(cellq("k", 1, n, m, sh)), med(cellq("k", 1, n, m, sh, "morn")), med(cellq("pg", 0, n, m, sh))
                row += f" {ratio(a, p)} / {ratio(b, p)} |"
        s += row + "\n"
    s += "\n| cells | geometric mean KDS now / PG | geometric mean KDS morning / PG | min now | max now |\n|---|---|---|---|---|\n"
    for n in SZ:
        for m in MODES:
            ra, rb = [], []
            for sh in SH:
                a, b, p = med(cellq("k", 1, n, m, sh)), med(cellq("k", 1, n, m, sh, "morn")), med(cellq("pg", 0, n, m, sh))
                ra.append(a / p); rb.append(b / p)
            s += f"| n={n:,} {m} | {geomean(ra):.2f}x | {geomean(rb):.2f}x | {min(ra):.2f}x | {max(ra):.2f}x |\n"
    return s


def pg_qps():
    s = "PostgreSQL 18.6 median qps (the morning's run):\n\n| shape | " + " | ".join(f"n={n:,} {m}" for n in SZ for m in MODES) + " |\n|---|" + "---|" * 6 + "\n"
    for sh in SH:
        s += f"| {sh} | " + " | ".join(f0(med(cellq('pg', 0, n, m, sh)), ' qps') for n in SZ for m in MODES) + " |\n"
    return s


def pct_cells(c=1):
    out = ""
    for n in SZ:
        for m in MODES:
            tag = f"s3-c{c}-n{n}-{m}"
            d = now(tag)
            out += f"#### `{tag}` run 1 (cores = {c})\n\n" + pct_table(d, set(p["phase"] for p in d["phases"] if p["phase"] not in ("ddl",))) + "\n"
    return out


def pg_pct_10k():
    out = ""
    for m in MODES:
        d = mor(3, f"pg-s3-n10000-{m}")
        out += f"#### PostgreSQL 18.6, `pg-s3-n10000-{m}` run 1 (the morning's run)\n\n" + pct_table(d, set(p["phase"] for p in d["phases"] if p["phase"] != "ddl")) + "\n"
    return out


def index_buys():
    s = "| shape | n=200 all/none | n=1,000 all/none | n=10,000 all/none (now) | n=10,000 all/none (morning) | PG n=10,000 all/none |\n|---|---|---|---|---|---|\n"
    for sh in SH:
        row = f"| {sh} |"
        for n in SZ:
            row += f" {ratio(med(cellq('k', 1, n, 'all', sh)), med(cellq('k', 1, n, 'none', sh)))} |"
            if n == 10000:
                row += f" {ratio(med(cellq('k', 1, n, 'all', sh, 'morn')), med(cellq('k', 1, n, 'none', sh, 'morn')))} | {ratio(med(cellq('pg', 0, n, 'all', sh)), med(cellq('pg', 0, n, 'none', sh)))} |"
        s += row + "\n"
    return s


def perrow():
    s = ("| shape | KDS mean n=200 | KDS mean n=10,000 | KDS per extra row now | KDS per extra row morning | PG per extra row | KDS now / PG |\n|---|---|---|---|---|---|---|\n")
    for sh in ("pk-user", "loans-by-user", "loans-by-book", "books-by-genre"):
        def mean(rr):
            return med([ph(d, sh)["mean_us"] for d in rr])
        a2, a1 = mean(N("s3-c1-n200-none")), mean(N("s3-c1-n10000-none"))
        m2, m1 = mean(Mo(3, "s3-c1-n200-none")), mean(Mo(3, "s3-c1-n10000-none"))
        p2, p1 = mean(Mo(3, "pg-s3-n200-none")), mean(Mo(3, "pg-s3-n10000-none"))
        ex = 9800
        na, nm, npg = (a1 - a2) / ex * 1000, (m1 - m2) / ex * 1000, (p1 - p2) / ex * 1000
        s += f"| {sh} | {a2:,.1f} µs | {a1:,.1f} µs | {na:.1f} ns | {nm:.1f} ns | {npg:.1f} ns | {'%.2fx' % (na / npg) if abs(npg) > 5 else '- (a flat shape)'} |\n"
    return s


def ab():
    """old vs new client, n=10000 unindexed, cores 1, 3 pairs"""
    import glob
    s = "| shape | old: 1 / 2 / 3 | old median | spread | new: 1 / 2 / 3 | new median | spread | new over old | read |\n|---|---|---|---|---|---|---|---|---|\n"
    for sh in SH:
        o = [q(now(f"ab-old{i}-s3-c1-n10000-none"), sh) for i in (1, 2, 3) if now(f"ab-old{i}-s3-c1-n10000-none")]
        n_ = [q(now(f"ab-new{i}-s3-c1-n10000-none"), sh) for i in (1, 2, 3) if now(f"ab-new{i}-s3-c1-n10000-none")]
        if not o or not n_:
            continue
        lim = max(spread(o) or 0, spread(n_) or 0)
        dl = 100 * (med(n_) - med(o)) / med(o)
        rd = "no claim (spread > 20 %)" if lim > 20 else ("**finding**" if abs(dl) > lim else "inside spread")
        s += (f"| {sh} | " + " / ".join(f0(x) for x in o) + f" | {f0(med(o), ' qps')} | {sp(spread(o))} | " + " / ".join(f0(x) for x in n_) +
              f" | {f0(med(n_), ' qps')} | {sp(spread(n_))} | {ratio(med(n_), med(o))} ({dl:+.1f} %) | {rd} |\n")
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        a = f.split(":")
        print(globals()[a[0]](*[int(x) for x in a[1:]]))
