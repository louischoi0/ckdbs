"""Table generator for the scenario 1-4 run at v3.4.17-159-g9f8f8b354 (9f8f8b354).
Adapted from bench/v3.5.0/archive/run-v3.5.0/gen.py.
Comparator run: the v3.5.0 run (07f4552a), measured 2026-11-20 on ANOTHER host, read from
bench/v3.5.0/archive/scenarioN-v3.5.0/json - a ratio against it is host plus engine.
Engine reading: the interleaved A/B on this host against the 07f4552a binary (ab-old*/ab-new*).
Usage: python3 -I gen.py <repo-root> [fragment-dir]   (writes one markdown fragment per table)."""
import json, statistics, os, sys, glob

RUN = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/out"
REPO = sys.argv[1]
PA = REPO + "/bench/v3.5.0/archive"
FR = sys.argv[2] if len(sys.argv) > 2 else RUN + "/tables"
os.makedirs(FR, exist_ok=True)
PFX = ["", "r2-", "r3-"]
OLD = "v3.5.0 run"
OLDE = "07f4552a"


def jl(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def N(tag):
    return [d for d in (jl(f"{RUN}/json/{p}{tag}.json") for p in PFX) if d]


def P(n, tag):
    return [d for d in (jl(f"{PA}/scenario{n}-v3.5.0/json/{p}{tag}.json") for p in PFX) if d]


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
    if not d:
        return None
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
        s = (f"| shape | cold now | warm now | spread | cabin now | warm {OLD} (other host) | spread | now over {OLD} (host + engine) |\n"
             "|---|---|---|---|---|---|---|---|\n")
        for sh in S1:
            w, ow, c = sv(R, sh, "warm"), sv(O, sh, "warm"), sv(R, sh, "cabin")
            s += (f"| {sh} | {f1(med(sv(R, sh, 'cold')), ' qps')} | {f1(med(w), ' qps')} | {sp(spread(w))} | "
                  f"{f1(med(c), ' qps') if c else '-'} | {f1(med(ow), ' qps')} | {sp(spread(ow))} | {ratio(med(w), med(ow))} |\n")
        put("s1-mat-" + cell, s)
    R, O = N("s1-c1-g"), P(1, "s1-c1-g")
    s = f"| phase | ops | now | spread | {OLD} (other host) | spread | now over {OLD} (host + engine) |\n|---|---|---|---|---|---|---|\n"
    for p in R[0]["phases"]:
        n = p["phase"]
        a, b = pv(R, n), pv(O, n)
        s += (f"| {n} | {p['ops']:,} | {f1(med(a), ' qps')} | {sp(spread(a))} | {f1(med(b), ' qps')} | {sp(spread(b))} | "
              f"{ratio(med(a), med(b))} |\n")
    put("s1-phases", s)
    put("s1-pct-c1-g", pct_table(R[0]))
    s = f"| bars | shape | warm now | spread | warm {OLD} (other host) | spread | now over {OLD} (host + engine) |\n|---|---|---|---|---|---|---|\n"
    for lab, tn in (("252", "s1s-n252-c1-g"), ("1,008", "s1s-n1008-c1-g"), ("10,080", "s1s-n10080-c1-g")):
        R, O = N(tn), P(1, tn)
        for sh in S1:
            w, ow = sv(R, sh, "warm"), sv(O, sh, "warm")
            s += (f"| {lab} | {sh} | {f1(med(w), ' qps')} | {sp(spread(w))} | {f1(med(ow), ' qps')} | {sp(spread(ow))} | "
                  f"{ratio(med(w), med(ow))} |\n")
    put("s1-size", s)
    ok = [d["meta"].get("verify_problems") == [] for t in ("s1-c1-g", "s1-c8-g", "s1-c1-s", "s1s-n252-c1-g",
                                                             "s1s-n1008-c1-g", "s1s-n10080-c1-g") for d in N(t)]
    okab = [d["meta"].get("verify_problems") == [] for d in abpairs("s1-c1-g")[0] + abpairs("s1-c1-g")[1]]
    put("s1-verify", f"main {sum(ok)} of {len(ok)}; A/B {sum(okab)} of {len(okab)}\n")
    # write sweep and connection sweep of run 1, if the JSON carries them
    m = R[0]["meta"]
    put("s1-meta-keys", ", ".join(sorted(m.keys())) + "\n")


CELLS2 = [("s2-c1-g", "1", "group"), ("s2-c8-g", "8", "group"), ("s2-c1-s", "1", "strict"), ("s2-c8-s", "8", "strict"),
          ("b1-s2-c1-g", "1", "group"), ("b1-s2-c1-s", "1", "strict")]


def tps(rr):
    return [d["meta"]["tps"] for d in rr]


def s2():
    s = (f"| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | {OLD} median (other host) | spread | now over {OLD} (host + engine) |\n"
         "|---|---|---|---|---|---|---|---|---|---|---|\n")
    for t, c, du in CELLS2:
        a = tps(N(t))
        b = tps(P(2, t))
        r = [f1(x, " tps") for x in a] + ["n/a"] * (3 - len(a))
        s += (f"| `{t}` | {c} | {du} | {r[0]} | {r[1]} | {r[2]} | **{f1(med(a), ' tps')}** | {sp(spread(a))} | "
              f"{f1(med(b), ' tps')} | {sp(spread(b))} | {ratio(med(a), med(b))} |\n")
    put("s2-tp", s)
    s = (f"| bookers | durability | tps now | runs | spread | {OLD} (other host) | now over {OLD} (host + engine) | conflicted (run 1) | verify failures / checks (run 1) |\n"
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
            av = tps(a); ov = tps(o)
            m = a[0]["meta"]
            s += (f"| {B} | {du} | **{f1(med(av), ' tps')}** | {len(av)} | {sp(spread(av))} | {f1(med(ov), ' tps')} | "
                  f"{ratio(med(av), med(ov))} | {m['outcomes']['conflicted']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    put("s2-sweep", s)
    s = "| cell | tps now | conflicted | retries | verify failures / checks | " + OLD + " tps (other host) | its conflicted | its verify failures / checks |\n|---|---|---|---|---|---|---|---|\n"
    for t in ("rr-s2-c1-g", "rr-s2-c8-g", "sat-b16-s2-c8-g", "sat-b16-s2-c8-s"):
        a = N(t)[:1]; o = P(2, t)[:1]
        if not a:
            s += f"| `{t}` | not run | - | - | - | - | - | - |\n"; continue
        m = a[0]["meta"]; om = o[0]["meta"] if o else None
        s += (f"| `{t}` | {f1(m['tps'], ' tps')} | {m['outcomes']['conflicted']} | {m['retries']} | {m['verify']['failures']} / {m['verify']['checks']} | "
              + (f"{f1(om['tps'], ' tps')} | {om['outcomes']['conflicted']} | {om['verify']['failures']} / {om['verify']['checks']} |\n" if om else "n/a | n/a | n/a |\n"))
    put("s2-extras", s)
    s = "| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | verify failures / checks |\n|---|---|---|---|---|---|---|---|\n"
    for t, _, _ in CELLS2:
        for i, d in enumerate(N(t)):
            m = d["meta"]; o = m["outcomes"]
            s += (f"| `{PFX[i]}{t}` | {i + 1} | {o['committed']:,} | {o['rejected-capacity']} | {o['rejected-credit']} | "
                  f"{o['conflicted']} | {m['abandoned']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    put("s2-outcomes", s)
    put("s2-pct-s2-c1-g", pct_table(N("s2-c1-g")[0]))
    put("s2-pct-s2-c8-g", pct_table(N("s2-c8-g")[0]))
    s = "| cell | booking mean now | commit share now | booking mean " + OLD + " (other host) | commit share " + OLD + " |\n|---|---|---|---|---|\n"
    for t, _, _ in CELLS2:
        row = [f"`{t}`"]
        for d in (N(t)[0], (P(2, t) or [None])[0]):
            if not d:
                row += ["n/a", "n/a"]; continue
            b = ph(d, "booking"); c = ph(d, "commit")
            row += [f"{b['mean_us']:,.0f} µs", f"{100 * c['mean_us'] * c['ops'] / (b['mean_us'] * b['ops']):.1f} %"]
        s += "| " + " | ".join(row) + " |\n"
    put("s2-commit", s)
    # where the time goes: per-phase share of the booking wall in run 1 of two cells
    for t in ("s2-c1-g", "s2-c8-g", "b1-s2-c1-s"):
        d = N(t)[0]
        tot = sum(p["mean_us"] * p["ops"] for p in d["phases"] if not p["phase"].startswith("load-") and p["phase"] not in ("ddl", "booking", "sync"))
        s = "| phase | ops | mean | p50 | p99 | share of the summed statement time |\n|---|---|---|---|---|---|\n"
        for p in d["phases"]:
            if p["phase"].startswith("load-") or p["phase"] in ("ddl", "sync"):
                continue
            sh = "-" if p["phase"] == "booking" else f"{100 * p['mean_us'] * p['ops'] / tot:.1f} %"
            s += f"| {p['phase']} | {p['ops']:,} | {p['mean_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {sh} |\n"
        put("s2-time-" + t, s)


S3 = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre", "loans-by-daterange",
      "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]


def q(RR, sh):
    return [1e6 / ph(d, sh)["mean_us"] for d in RR if ph(d, sh)]


def s3():
    for C in (1, 8):
        s = f"| loans | index | shape | now | spread | runs | {OLD} (other host) | spread | now over {OLD} (host + engine) |\n|---|---|---|---|---|---|---|---|---|\n"
        for n in (200, 1000, 10000):
            for M in ("none", "all"):
                t = f"s3-c{C}-n{n}-{M}"
                R = N(t)
                O = P(3, t)
                for sh in S3:
                    a, b = q(R, sh), q(O, sh)
                    s += (f"| {n:,} | {M} | {sh} | {f0(med(a), ' qps')} | {sp(spread(a))} | {len(a)} | {f0(med(b), ' qps')} | "
                          f"{sp(spread(b))} | {ratio(med(a), med(b))} |\n")
        put(f"s3-tp-c{C}", s)
    put("s3-pct-10000-none", pct_table(N("s3-c1-n10000-none")[0]))
    put("s3-pct-10000-all", pct_table(N("s3-c1-n10000-all")[0]))
    ok = []
    for f in glob.glob(RUN + "/json/*s3-*.json"):
        d = jl(f)
        ok.append(d is not None and d["meta"].get("verify_problems") == [])
    put("s3-verify", f"{sum(ok)} of {len(ok)} (main and A/B)\n")


AB_PRE, AB_N, AB_OUT = "", 3, "ab"


def abpairs(fmt):
    o = [jl(f"{RUN}/json/{AB_PRE}ab-old{i}-{fmt}.json") for i in range(1, AB_N + 1)]
    n = [jl(f"{RUN}/json/{AB_PRE}ab-new{i}-{fmt}.json") for i in range(1, AB_N + 1)]
    return [d for d in o if d], [d for d in n if d]


def abrow(lab, o, n, u):
    return (f"| {lab} | {f1(med(o), u)} | {sp(spread(o))} | {f1(med(n), u)} | {sp(spread(n))} | {ratio(med(n), med(o))} | "
            f"{read(med(n), med(o), spread(n), spread(o))} |\n")


H = f"| measure | `{OLDE}` engine | spread | `9f8f8b354` engine | spread | new over old | read |\n|---|---|---|---|---|---|---|\n"


def pin():
    global AB_PRE, AB_N, AB_OUT
    AB_PRE, AB_N, AB_OUT = "pin-", 5, "pin-ab"
    try:
        ab()
    finally:
        AB_PRE, AB_N, AB_OUT = "", 3, "ab"


def ab():
    A = AB_OUT
    o, n = abpairs("s1-c1-g"); s = H
    put(A + "-counts", f"s1 {len(o)}/{len(n)}; " + "; ".join(f"{f} {len(abpairs(f)[0])}/{len(abpairs(f)[1])}" for f in ("s2-c1-g", "b1-s2-c1-s", "s2-c8-g", "s3-c1-n10000-none", "s3-c1-n10000-all")) + "\n")
    for sh in S1:
        s += abrow(f"s1 {sh} cold", sv(o, sh, "cold"), sv(n, sh, "cold"), " qps")
        s += abrow(f"s1 {sh} warm", sv(o, sh, "warm"), sv(n, sh, "warm"), " qps")
        if sv(o, sh, "cabin"):
            s += abrow(f"s1 {sh} cabin", sv(o, sh, "cabin"), sv(n, sh, "cabin"), " qps")
    put(A + "-s1-sweep", s)
    s = H
    for p_ in [p["phase"] for p in n[0]["phases"]]:
        s += abrow(f"s1 phase {p_}", pv(o, p_), pv(n, p_), " qps")
    put(A + "-s1-phases", s)
    s = H
    for lab, fmt in (("8 bookers, cores 1, group", "s2-c1-g"), ("1 booker, cores 1, strict", "b1-s2-c1-s"), ("8 bookers, cores 8, group", "s2-c8-g")):
        o, n = abpairs(fmt)
        s += abrow(f"s2 {lab}: tps", tps(o), tps(n), " tps")
    put(A + "-s2-tps", s)
    for fmt in ("s2-c1-g", "b1-s2-c1-s", "s2-c8-g"):
        o, n = abpairs(fmt); s = H
        for nm in [p["phase"] for p in n[0]["phases"]]:
            if nm == "ddl":
                continue
            s += abrow(f"{nm} (1e6 / mean)", [1e6 / ph(d, nm)["mean_us"] for d in o if ph(d, nm)],
                       [1e6 / ph(d, nm)["mean_us"] for d in n if ph(d, nm)], " qps")
        put(A + "-s2-phases-" + fmt, s)
    s = "| cell | arm | run | tps | committed | conflicted | retries | verify failures / checks |\n|---|---|---|---|---|---|---|---|\n"
    for fmt in ("s2-c1-g", "b1-s2-c1-s", "s2-c8-g"):
        o, n = abpairs(fmt)
        for arm, rr in (("old", o), ("new", n)):
            for i, d in enumerate(rr):
                m = d["meta"]
                s += (f"| `{fmt}` | {arm} | {i + 1} | {f1(m['tps'], ' tps')} | {m['outcomes']['committed']:,} | {m['outcomes']['conflicted']} | "
                      f"{m['retries']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    put(A + "-s2-outcomes", s)
    for M in ("none", "all"):
        o, n = abpairs(f"s3-c1-n10000-{M}"); s = H
        lab = "unindexed" if M == "none" else "indexed"
        for nm in [p["phase"] for p in n[0]["phases"]]:
            s += abrow(f"s3 {nm} (10,000 loans, {lab})", q(o, nm), q(n, nm), " qps")
        put(A + "-s3-" + M, s)


G4 = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]


def s4():
    for tag in ("s4", "r2-s4", "cd2-s4", "ab-old1-s4"):
        d = jl(f"{RUN}/json/{tag}.json"); o = jl(f"{PA}/scenario4-v3.5.0/json/{'s4' if tag in ('r2-s4', 'ab-old1-s4') else tag}.json")
        if not d:
            put(f"s4-{tag}", "no result\n"); continue
        m = d["meta"]
        s = (f"| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | "
             f"on over off, {OLD} | declared over off, {OLD} |\n|---|---|---|---|---|---|---|---|---|\n")
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
                      f"{rr(cells['on'][1], cells['off'][1])} | {rr(cells['declared'][1], cells['off'][1])} |\n")
        put(f"s4-{tag}", s)
        cpu = d.get("server_cpu_s", {})
        cs = f"| arm | server CPU on the boards, 3 days | on the tapes | {OLD} boards (other host) | {OLD} tapes (other host) |\n|---|---|---|---|---|\n"
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
    # the engine, isolated: the measured binary's run A against the 07f4552a binary's run, this host
    for ntag, frag in (("s4", "s4-engine"), ("r2-s4", "s4-engine-r2")):
      n = jl(f"{RUN}/json/{ntag}.json"); o = jl(f"{RUN}/json/ab-old1-s4.json")
      if n and o:
        s = ("| group | arm | day 1 old / new mean | new over old | day 2 old / new mean | new over old | day 3 old / new mean | new over old |\n"
             "|---|---|---|---|---|---|---|---|\n")
        for g in G4:
            for arm in ("off", "on", "declared"):
                row = f"| {g} | {arm} |"
                for day in (1, 2, 3):
                    a, b = ph(o, f"d{day}-{g}[{arm}]"), ph(n, f"d{day}-{g}[{arm}]")
                    row += (f" {a['mean_us']:,.1f} µs / {b['mean_us']:,.1f} µs | {a['mean_us'] / b['mean_us']:.2f}x |" if a and b else " n/a | n/a |")
                s += row + "\n"
        put(frag, s)


if __name__ == "__main__":
    for f in (sys.argv[3:] or ["s1", "s2", "s3", "ab", "pin", "s4"]):
        try:
            globals()[f]()
        except Exception as e:  # a missing cell must not hide the others
            import traceback
            put("error-" + f, f"{type(e).__name__}: {e}\n{traceback.format_exc()}\n")
