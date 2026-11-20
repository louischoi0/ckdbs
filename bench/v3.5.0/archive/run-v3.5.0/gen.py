"""Table generator for the v3.5.0 scenario 1-4 run (07f4552a).
Comparators: scenarios 1-3 the v3.4.14 run on this host (bench/v3.4.14/archive/scenarioN-v3.4.14/json);
scenario 4 the v3.3.0-38 run on another host (bench/v3.3.0/archive/scenario4-v3.3.0-38-ga40882af/json).
Usage: python3 -I gen.py <fragment-dir>   (writes one markdown fragment per table)."""
import json, statistics, os, sys, math, glob

RUN = "/home/louis/bench-runs/scenarios-v3.5.0"
REPO = "/home/louis/ckdbs"
PA = REPO + "/bench/v3.4.14/archive"
P4 = REPO + "/bench/v3.3.0/archive/scenario4-v3.3.0-38-ga40882af/json"
FR = sys.argv[1] if len(sys.argv) > 1 else RUN + "/frag"
os.makedirs(FR, exist_ok=True)
PFX = ["", "r2-", "r3-"]
OLD = "v3.4.14"


def jl(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def N(tag):
    return [d for d in (jl(f"{RUN}/json/{p}{tag}.json") for p in PFX) if d]


def P(n, tag):
    return [d for d in (jl(f"{PA}/scenario{n}-v3.4.14/json/{p}{tag}.json") for p in PFX) if d]


def med(x):
    x = [v for v in x if v is not None]
    return statistics.median(x) if x else None


def spread(x):
    x = [v for v in x if v is not None]
    if len(x) < 2:
        return None
    m = statistics.median(x)
    return 100 * (max(x) - min(x)) / m if m else None


def f1(x, u=""):
    return "n/a" if x is None else f"{x:,.1f}{u}"


def f0(x, u=""):
    return "n/a" if x is None else f"{x:,.0f}{u}"


def sp(x):
    return "n/a" if x is None else f"{x:.1f} %"


def ratio(a, b):
    return "n/a" if not a or not b else f"{a / b:.2f}x"


def delta(a, b):
    return "n/a" if not a or not b else f"{100 * (a - b) / b:+.1f} %"


def read(a, b, sa, sb):
    if not a or not b:
        return "n/a"
    if sa is None and sb is None:
        return "single run, no claim"
    d = 100 * (a - b) / b
    lim = max(sa or 0, sb or 0)
    if lim > 20:
        return "spread > 20 %, no claim"
    return "**finding**" if abs(d) > lim else "inside the spread"


def ph(d, n):
    for p in d["phases"]:
        if p["phase"] == n:
            return p


def pv(rr, n, k="qps"):
    return [ph(d, n)[k] for d in rr if ph(d, n)]


def sweep(d):
    return {s["shape"]: s for s in d["meta"]["qps_sweep"]}


def sv(rr, sh, key):
    o = []
    for d in rr:
        s = sweep(d).get(sh)
        if s and s.get(key):
            o.append(s[key]["qps"])
    return o


def put(n, t):
    open(f"{FR}/{n}.md", "w").write(t)


def pct_table(d):
    s = "| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |\n|---|---|---|---|---|---|---|---|---|\n"
    for p in d["phases"]:
        s += (f"| {p['phase']} | {p['ops']:,} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | "
              f"{p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} |\n")
    return s


S1 = ["bar-lookup", "bar-range", "day-slice", "symbol-history", "cross-join", "point-join", "model-join"]


def s1():
    for cell in ("c1-g", "c8-g", "c1-s"):
        R, O = N("s1-" + cell), P(1, "s1-" + cell)
        s = (f"| shape | cold now | warm now | spread | warm {OLD} | spread | delta | read | cabin now | cabin {OLD} |\n"
             "|---|---|---|---|---|---|---|---|---|---|\n")
        for sh in S1:
            w, ow, c, oc = sv(R, sh, "warm"), sv(O, sh, "warm"), sv(R, sh, "cabin"), sv(O, sh, "cabin")
            s += (f"| {sh} | {f1(med(sv(R, sh, 'cold')), ' qps')} | {f1(med(w), ' qps')} | {sp(spread(w))} | "
                  f"{f1(med(ow), ' qps')} | {sp(spread(ow))} | {delta(med(w), med(ow))} | {read(med(w), med(ow), spread(w), spread(ow))} | "
                  f"{f1(med(c), ' qps') if c else '-'} | {f1(med(oc), ' qps') if oc else '-'} |\n")
        put("s1-mat-" + cell, s)
    R, O = N("s1-c1-g"), P(1, "s1-c1-g")
    s = f"| phase | ops | now | spread | {OLD} | spread | delta | read |\n|---|---|---|---|---|---|---|---|\n"
    for p in R[0]["phases"]:
        n = p["phase"]
        a, b = pv(R, n), pv(O, n)
        s += (f"| {n} | {p['ops']:,} | {f1(med(a), ' qps')} | {sp(spread(a))} | {f1(med(b), ' qps')} | {sp(spread(b))} | "
              f"{delta(med(a), med(b))} | {read(med(a), med(b), spread(a), spread(b))} |\n")
    put("s1-phases", s)
    put("s1-pct-c1-g", pct_table(R[0]))
    s = f"| bars | shape | warm now | spread | warm {OLD} | spread | delta | read |\n|---|---|---|---|---|---|---|---|\n"
    for lab, tn in (("252", "s1s-n252-c1-g"), ("1,008", "s1s-n1008-c1-g"), ("10,080", "s1s-n10080-c1-g")):
        R, O = N(tn), P(1, tn)
        for sh in S1:
            w, ow = sv(R, sh, "warm"), sv(O, sh, "warm")
            s += (f"| {lab} | {sh} | {f1(med(w), ' qps')} | {sp(spread(w))} | {f1(med(ow), ' qps')} | {sp(spread(ow))} | "
                  f"{delta(med(w), med(ow))} | {read(med(w), med(ow), spread(w), spread(ow))} |\n")
    put("s1-size", s)
    ok = [d["meta"].get("verify_problems") == [] for t in ("s1-c1-g", "s1-c8-g", "s1-c1-s", "s1s-n252-c1-g",
                                                             "s1s-n1008-c1-g", "s1s-n10080-c1-g") for d in N(t)]
    put("s1-verify", f"{sum(ok)} of {len(ok)}")


CELLS2 = [("s2-c1-g", "1", "group"), ("s2-c8-g", "8", "group"), ("s2-c1-s", "1", "strict"), ("s2-c8-s", "8", "strict"),
          ("b1-s2-c1-g", "1", "group"), ("b1-s2-c1-s", "1", "strict")]


def s2():
    s = (f"| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | {OLD} median | spread | delta | read |\n"
         "|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    for t, c, du in CELLS2:
        a = [d["meta"]["tps"] for d in N(t)]
        b = [d["meta"]["tps"] for d in P(2, t)]
        r = [f1(x, " tps") for x in a] + ["n/a"] * (3 - len(a))
        s += (f"| `{t}` | {c} | {du} | {r[0]} | {r[1]} | {r[2]} | **{f1(med(a), ' tps')}** | {sp(spread(a))} | "
              f"{f1(med(b), ' tps')} | {sp(spread(b))} | {delta(med(a), med(b))} | {read(med(a), med(b), spread(a), spread(b))} |\n")
    put("s2-tp", s)
    s = (f"| bookers | durability | tps now | runs | spread | {OLD} | delta | conflicted (run 1) | verify failures / checks (run 1) |\n"
         "|---|---|---|---|---|---|---|---|---|\n")
    for B in (1, 2, 4, 8, 16, 32):
        for du, c in (("group", "g"), ("strict", "s")):
            if B == 8:
                t = f"s2-c8-{c}"; a = N(t); o = P(2, t)
            elif B == 32:
                t = f"sat-b32-s2-c8-{c}"; a = N(t)[:1]; o = P(2, t)[:1]
            else:
                t = f"x-b{B}-s2-c8-{c}"; a = N(t)[:1]; o = P(2, t)[:1]
            if not a:
                continue
            av = [d["meta"]["tps"] for d in a]; ov = [d["meta"]["tps"] for d in o]
            m = a[0]["meta"]
            s += (f"| {B} | {du} | **{f1(med(av), ' tps')}** | {len(av)} | {sp(spread(av))} | {f1(med(ov), ' tps')} | "
                  f"{delta(med(av), med(ov))} | {m['outcomes']['conflicted']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    put("s2-sweep", s)
    s = "| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | verify failures / checks |\n|---|---|---|---|---|---|---|---|\n"
    for t, _, _ in CELLS2:
        for i, d in enumerate(N(t)):
            m = d["meta"]; o = m["outcomes"]
            s += (f"| `{PFX[i]}{t}` | {i + 1} | {o['committed']:,} | {o['rejected-capacity']} | {o['rejected-credit']} | "
                  f"{o['conflicted']} | {m['abandoned']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    put("s2-outcomes", s)
    put("s2-pct-s2-c1-g", pct_table(N("s2-c1-g")[0]))
    s = "| cell | booking mean now | commit share now | booking mean " + OLD + " | commit share " + OLD + " |\n|---|---|---|---|---|\n"
    for t, _, _ in CELLS2:
        row = [f"`{t}`"]
        for d in (N(t)[0], (P(2, t) or [None])[0]):
            if not d:
                row += ["n/a", "n/a"]; continue
            b = ph(d, "booking"); c = ph(d, "commit")
            row += [f"{b['mean_us']:,.0f} µs", f"{100 * c['mean_us'] * c['ops'] / (b['mean_us'] * b['ops']):.1f} %"]
        s += "| " + " | ".join(row) + " |\n"
    put("s2-commit", s)


S3 = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre", "loans-by-daterange",
      "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]


def q(RR, sh):
    return [1e6 / ph(d, sh)["mean_us"] for d in RR if ph(d, sh)]


def s3():
    for C in (1, 8):
        s = f"| loans | index | shape | now | spread | runs | {OLD} | spread | delta | read |\n|---|---|---|---|---|---|---|---|---|---|\n"
        for n in (200, 1000, 10000):
            for M in ("none", "all"):
                t = f"s3-c{C}-n{n}-{M}"
                R = N(t) + ([d for d in (jl(f"{RUN}/json/{p}{t}.json") for p in ("r2-", "r3-")) if d] if False else [])
                O = P(3, t)
                for sh in S3:
                    a, b = q(R, sh), q(O, sh)
                    s += (f"| {n:,} | {M} | {sh} | {f0(med(a), ' qps')} | {sp(spread(a))} | {len(a)} | {f0(med(b), ' qps')} | "
                          f"{sp(spread(b))} | {delta(med(a), med(b))} | {read(med(a), med(b), spread(a), spread(b))} |\n")
        put(f"s3-tp-c{C}", s)
    put("s3-pct-10000-none", pct_table(N("s3-c1-n10000-none")[0]))
    ok = []
    for f in glob.glob(RUN + "/json/*s3-*.json"):
        if "/ab-" in f:
            continue
        d = jl(f)
        ok.append(d is not None and d["meta"].get("verify_problems") == [])
    put("s3-verify", f"{sum(ok)} of {len(ok)}")


def abpairs(fmt):
    o = [jl(f"{RUN}/json/ab-old{i}-{fmt}.json") for i in (1, 2, 3)]
    n = [jl(f"{RUN}/json/ab-new{i}-{fmt}.json") for i in (1, 2, 3)]
    return [d for d in o if d], [d for d in n if d]


def abrow(lab, o, n, u):
    return (f"| {lab} | {f1(med(o), u)} | {sp(spread(o))} | {f1(med(n), u)} | {sp(spread(n))} | {ratio(med(n), med(o))} | "
            f"{read(med(n), med(o), spread(n), spread(o))} |\n")


def ab():
    H = "| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |\n|---|---|---|---|---|---|---|\n"
    o, n = abpairs("s1-c1-g"); s = H
    for sh in S1:
        s += abrow(f"s1 {sh} warm", sv(o, sh, "warm"), sv(n, sh, "warm"), " qps")
    for p_ in ("backtest-read", "result-insert", "compare-one", "load-bars", "agg-by-session"):
        s += abrow(f"s1 phase {p_}", pv(o, p_), pv(n, p_), " qps")
    put("ab-s1", s)
    o, n = abpairs("s2-c1-g")
    s = H + abrow("s2 8 bookers, cores 1, group", [d["meta"]["tps"] for d in o], [d["meta"]["tps"] for d in n], " tps")
    o2, n2 = abpairs("b1-s2-c1-s")
    s += abrow("s2 1 booker, cores 1, strict", [d["meta"]["tps"] for d in o2], [d["meta"]["tps"] for d in n2], " tps")
    for nm in ("commit", "recipe-read", "charge-insert"):
        s += abrow(f"s2 1 booker strict, {nm} (1e6 / mean)", [1e6 / ph(d, nm)["mean_us"] for d in o2 if ph(d, nm)],
                   [1e6 / ph(d, nm)["mean_us"] for d in n2 if ph(d, nm)], " qps")
    put("ab-s2", s)
    o, n = abpairs("s3-c1-n10000-none"); s = H
    for sh in S3:
        s += abrow(f"s3 {sh} (10,000 loans, unindexed)", q(o, sh), q(n, sh), " qps")
    put("ab-s3", s)


G4 = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]


def s4():
    out = {}
    for tag in ("s4", "cd2-s4"):
        d = jl(f"{RUN}/json/{tag}.json"); o = jl(f"{P4}/{tag}.json")
        if not d:
            put(f"s4-{tag}", "no result\n"); continue
        m = d["meta"]
        s = (f"| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | "
             f"off now over v3.3.0-38 | on now over v3.3.0-38 | declared now over v3.3.0-38 |\n|---|---|---|---|---|---|---|---|---|---|\n")
        for g in G4:
            for day in (1, 2, 3):
                cells = {}
                for arm in ("off", "on", "declared"):
                    cells[arm] = (ph(d, f"d{day}-{g}[{arm}]"), ph(o, f"d{day}-{g}[{arm}]") if o else None)
                def pq(x):
                    return "n/a" if not x else f"{x['p50_us']:,.1f} µs / {1e6 / x['mean_us']:,.0f} qps"
                def rr(a, b):
                    return "n/a" if not a or not b else f"{b['mean_us'] / a['mean_us']:.2f}x"
                s += (f"| {g} | {day} | {pq(cells['off'][0])} | {pq(cells['on'][0])} | {pq(cells['declared'][0])} | "
                      f"{rr(cells['on'][0], cells['off'][0])} | {rr(cells['declared'][0], cells['off'][0])} | "
                      + " | ".join(rr(cells[a][0], cells[a][1]) for a in ("off", "on", "declared")) + " |\n")
        put(f"s4-{tag}", s)
        cpu = d.get("server_cpu_s", {})
        cs = "| arm | server CPU on the boards, 3 days | on the tapes | v3.3.0-38 boards | v3.3.0-38 tapes |\n|---|---|---|---|---|\n"
        for arm in ("off", "on", "declared"):
            def tot(c, k):
                return sum(v for kk, v in c.get(arm, {}).items() if kk.endswith(k)) if c else None
            oc = o.get("server_cpu_s", {}) if o else {}
            cs += f"| {arm} | {f1(tot(cpu, 'board'), ' s')} | {f1(tot(cpu, 'tape'), ' s')} | {f1(tot(oc, 'board'), ' s')} | {f1(tot(oc, 'tape'), ' s')} |\n"
        put(f"s4-cpu-{tag}", cs)
        ver = d.get("verify", [])
        fin = d.get("final", {}).get("on", {}).get("header", {})
        ents = d.get("final", {}).get("on", {}).get("entries", [])
        put(f"s4-meta-{tag}",
            f"elapsed {m.get('elapsed_s')} s, client_errors {m.get('client_errors')}, slot_overruns {m.get('slot_overruns')}, "
            f"first_error {m.get('first_error')}; byte comparison: "
            + ", ".join(f"day {v['day']} {v['statements']} statements {len(v['mismatches'])} mismatches" for v in ver)
            + f"; controller final (on): {json.dumps(fin)}; entries: "
            + "; ".join(f"{e.get('rel')}.{e.get('column')} {e.get('state')} pages {e.get('pages')} last {e.get('last_action')}" for e in ents)
            + "\n")


if __name__ == "__main__":
    for f in (sys.argv[2:] or ["s1", "s2", "s3", "ab", "s4"]):
        try:
            globals()[f]()
        except Exception as e:  # a missing cell must not hide the others
            put("error-" + f, f"{type(e).__name__}: {e}\n")
