"""Headline fragments for the summary, from the same JSON as gen.py's tables.
Usage: python3 -I sumgen.py <repo-root>   (-I drops the script directory from sys.path, so it is added here)"""
import sys
sys.path.insert(0, "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354")
import gen
from gen import N, P, med, spread, f1, f0, sp, ratio, read, sv, pv, tps, q, put

s = ("| measure | now (`9f8f8b354`, this host) | spread | `v3.5.0` run (`07f4552a`, other host) | spread | now over `v3.5.0` run (host + engine) |\n"
     "|---|---|---|---|---|---|\n")


def row(lab, a, b, u, f=f1):
    return f"| {lab} | {f(med(a), u)} | {sp(spread(a))} | {f(med(b), u)} | {sp(spread(b))} | {ratio(med(a), med(b))} |\n"


R1, O1 = N("s1-c1-g"), P(1, "s1-c1-g")
for sh, lab in (("day-slice", "1 backtest, `day-slice` warm (60,480 bars)"), ("cross-join", "1 backtest, `cross-join` warm"),
                ("bar-range", "1 backtest, `bar-range` warm"), ("model-join", "1 backtest, `model-join` warm")):
    s += row(lab, sv(R1, sh, "warm"), sv(O1, sh, "warm"), " qps")
s += row("1 backtest, `result-insert` phase", pv(R1, "result-insert"), pv(O1, "result-insert"), " qps")
for t, lab in (("s2-c1-g", "2 freight, 8 bookers, `cores = 1`, `group`"), ("s2-c8-g", "2 freight, 8 bookers, `cores = 8`, `group`"),
               ("s2-c8-s", "2 freight, 8 bookers, `cores = 8`, `strict`"), ("b1-s2-c1-s", "2 freight, 1 booker, `strict`"),
               ("sat-b32-s2-c8-g", "2 freight, 32 bookers, `cores = 8`, `group` (one run)")):
    s += row(lab, tps(N(t)), tps(P(2, t)), " tps")
for t, sh, lab in (("s3-c1-n10000-none", "loans-by-user", "3 library, `loans-by-user` (10,000 loans, unindexed)"),
                   ("s3-c1-n10000-none", "join-no-literal", "3 library, `join-no-literal` (10,000 loans, unindexed)"),
                   ("s3-c1-n10000-none", "exists-correlated", "3 library, `exists-correlated` (10,000 loans, unindexed)"),
                   ("s3-c1-n10000-all", "loans-by-user", "3 library, `loans-by-user` (10,000 loans, indexed)")):
    s += row(lab, q(N(t), sh), q(P(3, t), sh), " qps", f0)
put("sum-headlines", s)

gen.AB_PRE, gen.AB_N = "pin-", 5
H = ("| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |\n|---|---|---|---|---|---|---|\n")
s = H
o, n = gen.abpairs("s1-c1-g")
for sh in ("day-slice", "cross-join", "bar-range", "model-join", "symbol-history"):
    s += gen.abrow(f"1 backtest, `{sh}` warm", sv(o, sh, "warm"), sv(n, sh, "warm"), " qps")
for p_ in ("backtest-read", "result-insert", "load-bars", "agg-by-session"):
    s += gen.abrow(f"1 backtest, phase `{p_}`", pv(o, p_), pv(n, p_), " qps")
for lab, fmt in (("8 bookers, `cores = 1`, `group`", "s2-c1-g"), ("1 booker, `cores = 1`, `strict`", "b1-s2-c1-s"), ("8 bookers, `cores = 8`, `group`", "s2-c8-g")):
    o, n = gen.abpairs(fmt)
    s += gen.abrow(f"2 freight, {lab}", tps(o), tps(n), " tps")
for M, lab in (("none", "unindexed"), ("all", "indexed")):
    o, n = gen.abpairs(f"s3-c1-n10000-{M}")
    for sh in ("pk-user", "loans-by-user", "join-no-literal", "exists-correlated", "count-by-user"):
        s += gen.abrow(f"3 library, `{sh}` (10,000 loans, {lab})", q(o, sh), q(n, sh), " qps")
put("sum-engine", s)

# scenario 4 headline: on over off and declared over off, board and tape10k, per run
s = "| group | run | `on` over `off`, days 1 / 2 / 3 | `declared` over `off`, days 1 / 2 / 3 | `on` over `off`, `v3.5.0` run |\n|---|---|---|---|---|\n"
for g in ("board", "tape10k", "pk"):
    for tag, lab in (("s4", "A (`s4`, overlapped a build)"), ("r2-s4", "A again (`r2-s4`)"), ("cd2-s4", "B (`cd2-s4`)"), ("ab-old1-s4", "`07f4552a` binary (`ab-old1-s4`)")):
        d = gen.jl(f"{gen.RUN}/json/{tag}.json")
        o = gen.jl(f"{gen.PA}/scenario4-v3.5.0/json/{'s4' if tag in ('r2-s4', 'ab-old1-s4') else tag}.json")
        if not d:
            continue

        def r3(dd, arm):
            out = []
            for day in (1, 2, 3):
                a, b = gen.ph(dd, f"d{day}-{g}[{arm}]"), gen.ph(dd, f"d{day}-{g}[off]")
                out.append(f"{b['mean_us'] / a['mean_us']:.2f}x" if a and b else "n/a")
            return " / ".join(out)
        s += f"| `{g}` | {lab} | {r3(d, 'on')} | {r3(d, 'declared')} | {r3(o, 'on') if o else 'n/a'} |\n"
put("sum-s4", s)
print("ok")
