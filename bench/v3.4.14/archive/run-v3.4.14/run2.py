import sys
sys.path.insert(0, "/home/louis/bench-runs/scenarios-v3.4.14")
import gen
from gen import *

# prefer this host's PostgreSQL 17 cells when they exist; else the v3.3.0-33 PG 18.6 cells (another host)
_old_G = gen.G
def PGN(tag):
    return [d for d in (jl(f"{RUN}/json/{p}{tag}.json") for p in PFX) if d]
def G2(n, tag):
    return PGN(tag) or _old_G(n, tag)
gen.G = G2

def extra3():
    s = "| shape | n=200 all/none | n=1,000 all/none | n=10,000 all/none (now) | n=10,000 all/none (v3.3.0-38) | PG n=10,000 all/none |\n|---|---|---|---|---|---|\n"
    now = lambda n, M: N(f"s3-c1-n{n}-{M}")
    old = lambda n, M: P(3, f"s3-c1-n{n}-{M}")
    pg = lambda n, M: G2(3, f"pg-s3-n{n}-{M}")
    def m(src, n, M, sh):
        return med(q(src(n, M), sh))
    for sh in S3:
        r = [ratio(m(now, n, "all", sh), m(now, n, "none", sh)) for n in (200, 1000, 10000)]
        s += f"| {sh} | {r[0]} | {r[1]} | {r[2]} | {ratio(m(old,10000,'all',sh), m(old,10000,'none',sh))} | {ratio(m(pg,10000,'all',sh), m(pg,10000,'none',sh))} |\n"
    put("s3-index", s)
    s = "| shape | mean n=200 | mean n=10,000 | per extra relation row now | v3.3.0-38 | PG |\n|---|---|---|---|---|---|\n"
    for sh in ("pk-user", "loans-by-user", "loans-by-book", "books-by-genre"):
        def mm(src, n):
            return med([ph(d, sh)["mean_us"] for d in src(n, "none") if ph(d, sh)])
        def per(src):
            return (mm(src, 10000) - mm(src, 200)) / 9800 * 1000
        s += f"| {sh} | {mm(now,200):.1f} µs | {mm(now,10000):.1f} µs | {per(now):.1f} ns | {per(old):.1f} ns | {per(pg):.1f} ns |\n"
    put("s3-perrow", s)

if __name__ == "__main__":
    for f in sys.argv[1:]:
        (globals().get(f) or getattr(gen, f))()
