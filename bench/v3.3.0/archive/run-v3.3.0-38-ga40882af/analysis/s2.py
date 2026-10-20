import sys, re, os
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *

CORE = [("s2-c1-g", "1 core", "group"), ("s2-c8-g", "8 cores", "group"), ("s2-c1-s", "1 core", "strict"), ("s2-c8-s", "8 cores", "strict")]
B1 = [("b1-s2-c1-g", "group"), ("b1-s2-c1-s", "strict")]
STMTS = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert", "operation-update", "org-update", "commit"]


def tps(d):
    return d["meta"]["tps"]


def throughput():
    s = ("| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | morning median | morning spread | delta | read |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n")
    for tag, c, dur in CORE + [(t, "1 core", d) for t, d in B1]:
        a, b = [tps(d) for d in N(tag)], [tps(d) for d in Mo(2, tag)]
        lim = max(spread(a) or 0, spread(b) or 0)
        dl = 100 * (med(a) - med(b)) / med(b)
        rd = "**finding**" if abs(dl) > lim else "inside the spread"
        runs_ = a + [None] * (3 - len(a))
        s += (f"| `{tag}` | {c} | {dur} | " + " | ".join(f0(x, ' tps') if x is None else f1(x, ' tps') for x in runs_) +
              f" | **{f1(med(a), ' tps')}** | {sp(spread(a))} | {f1(med(b), ' tps')} | {sp(spread(b))} | {delta(med(a), med(b))} | {rd} |\n")
    return s


def sweep_tbl():
    s = ("| bookers | durability | now: run 1 | run 2 | run 3 | median | spread | morning median | delta | conflicted (run 1) | verify failures (run 1) |\n|---|---|---|---|---|---|---|---|---|---|---|\n")
    for B in (1, 2, 4, 8, 16, 32):
        for dur, c in (("group", "g"), ("strict", "s")):
            if B == 8:
                tag = f"s2-c8-{c}"
            elif B in (16, 32):
                tag = f"sat-b{B}-s2-c8-{c}" if B == 32 else f"x-b{B}-s2-c8-{c}"
            else:
                tag = f"x-b{B}-s2-c8-{c}"
            a, b = N(tag, [""] if B != 8 else PFX), Mo(2, tag, [""] if B != 8 else PFX)
            if B == 16:   # saturation cells repeat the 16-booker point
                a2 = N(f"sat-b16-s2-c8-{c}", [""])
            ta = [tps(d) for d in a]
            tb = [tps(d) for d in b]
            runs_ = (ta + [None] * 3)[:3]
            r1 = a[0]["meta"] if a else {}
            s += (f"| {B} | {dur} | " + " | ".join('n/a' if x is None else f1(x, ' tps') for x in runs_) + f" | **{f1(med(ta), ' tps')}** | {sp(spread(ta))} | "
                  f"{f1(med(tb), ' tps')} | {delta(med(ta), med(tb))} | {r1.get('outcomes', {}).get('conflicted', 'n/a')} | "
                  f"{r1.get('verify', {}).get('failures', 'n/a')} / {r1.get('verify', {}).get('checks', 'n/a')} |\n")
    return s


def sat16():
    s = "| cell | now | morning | delta |\n|---|---|---|---|\n"
    for c in ("g", "s"):
        a = N(f"sat-b16-s2-c8-{c}", [""]); b = Mo(2, f"sat-b16-s2-c8-{c}", [""])
        if a and b:
            s += f"| `sat-b16-s2-c8-{c}` | {f1(tps(a[0]), ' tps')} | {f1(tps(b[0]), ' tps')} | {delta(tps(a[0]), tps(b[0]))} |\n"
    return s


def cpu_samples():
    s = "| cell | 1-s samples | mean busy | peak busy | mean steal |\n|---|---|---|---|---|\n"
    for B in (16, 32):
        for c in ("g", "s"):
            tag = f"sat-b{B}-s2-c8-{c}"
            p = f"{RUN}/logs/{tag}.cpu"
            if not os.path.exists(p):
                continue
            bs, st = [], []
            for l in open(p):
                m = re.search(r"busy=(\d+) steal=(\d+)", l)
                if m:
                    bs.append(int(m.group(1))); st.append(int(m.group(2)))
            s += f"| `{tag}` | {len(bs)} samples | {sum(bs) / len(bs):.0f} % | {max(bs)} % | {sum(st) / len(st):.1f} % |\n"
    return s


def pct_cells():
    out = ""
    for tag, c, dur in CORE:
        out += f"#### `{tag}` run 1 ({c}, {dur})\n\n" + pct_table(now(tag), set(p["phase"] for p in now(tag)["phases"] if not p["phase"].startswith(("ddl", "load"))))
        out += "\n"
    return out


def attribution(tags):
    s = ("| cell | " + " | ".join(STMTS) + " | unattributed | booking mean |\n|---|" + "---|" * (len(STMTS) + 2) + "\n")
    for tag in tags:
        for lab, fn in (("now", now), ("morning", lambda t: mor(2, t))):
            d = fn(tag)
            if not d:
                continue
            b = ph(d, "booking")
            tot = b["mean_us"] * b["ops"]
            parts = [ph(d, x)["mean_us"] * ph(d, x)["ops"] / tot * 100 for x in STMTS]
            s += f"| `{tag}` ({lab}) | " + " | ".join(f"{x:.1f} %" for x in parts) + f" | {100 - sum(parts):.1f} % | {b['mean_us']:,.0f} µs |\n"
    return s


def stmt_cmp(tags, names=("booking", "commit") + tuple(STMTS[:-1])):
    s = "| cell | phase | p0 now | p0 morning | p50 now | p50 morning | p50 now/morning | mean now | mean morning |\n|---|---|---|---|---|---|---|---|---|\n"
    for tag in tags:
        a, b = now(tag), mor(2, tag)
        for n in names:
            x, y = ph(a, n), ph(b, n)
            s += (f"| `{tag}` | {n} | {x['p0_us']:,.1f} µs | {y['p0_us']:,.1f} µs | {x['p50_us']:,.1f} µs | {y['p50_us']:,.1f} µs | "
                  f"{x['p50_us'] / y['p50_us']:.2f}x | {x['mean_us']:,.1f} µs | {y['mean_us']:,.1f} µs |\n")
    return s


def pg_cmp():
    P = Mo(2, "pg-s2-b1")
    s = "| engine / cell | run 1 | run 2 | run 3 | median | spread | KDS now / PG |\n|---|---|---|---|---|---|---|\n"
    pt = [tps(d) for d in P]
    for tag, lab in (("b1-s2-c1-g", "KDS `b1-s2-c1-g` (now)"), ("b1-s2-c1-s", "KDS `b1-s2-c1-s` (now)")):
        a = [tps(d) for d in N(tag)]
        s += f"| {lab} | " + " | ".join(f1(x, ' tps') for x in a) + f" | {f1(med(a), ' tps')} | {sp(spread(a))} | {ratio(med(a), med(pt))} |\n"
        b = [tps(d) for d in Mo(2, tag)]
        s += f"| KDS `{tag}` (morning) | " + " | ".join(f1(x, ' tps') for x in b) + f" | {f1(med(b), ' tps')} | {sp(spread(b))} | {ratio(med(b), med(pt))} |\n"
    s += "| PostgreSQL 18.6 (morning's run, reused) | " + " | ".join(f1(x, ' tps') for x in pt) + f" | {f1(med(pt), ' tps')} | {sp(spread(pt))} | 1.00x |\n"
    s += "\n"
    a, p = ph(now("b1-s2-c1-g"), "booking"), ph(P[0], "booking")
    s += "| statement | KDS `b1-s2-c1-g` p50 | PostgreSQL p50 | KDS/PG p50 | KDS mean | PG mean |\n|---|---|---|---|---|---|\n"
    for n in ["booking"] + STMTS:
        x, y = ph(now("b1-s2-c1-g"), n), ph(P[0], n)
        s += f"| {n} | {x['p50_us']:,.1f} µs | {y['p50_us']:,.1f} µs | {x['p50_us'] / y['p50_us']:.2f}x | {x['mean_us']:,.1f} µs | {y['mean_us']:,.1f} µs |\n"
    return s


def outcomes():
    s = ("| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks |\n|---|---|---|---|---|---|---|---|---|\n")
    for tag in ["s2-c1-g", "s2-c8-g", "s2-c1-s", "s2-c8-s", "b1-s2-c1-g", "b1-s2-c1-s"]:
        for i, p in enumerate(PFX):
            d = now(p + tag)
            if not d:
                continue
            m = d["meta"]; o = m["outcomes"]; a = m["axes"]
            s += (f"| `{p + tag}` | run {i + 1} | {o['committed']:,} | {o['rejected-capacity']} | {o['rejected-credit']} | {o['conflicted']} | {m['abandoned']} | "
                  f"{a['operations']} / {a['organizations']} / {a['read']} / {a['commit']} | {m['verify']['failures']} / {m['verify']['checks']} |\n")
    return s


def isolation():
    s = "| cell | isolation | tps now | tps morning | verify failures / checks | conflicted |\n|---|---|---|---|---|---|\n"
    for tag, base in (("rr-s2-c1-g", "s2-c1-g"), ("rr-s2-c8-g", "s2-c8-g")):
        a, b = now(tag), mor(2, tag)
        ba = N(base)
        s += (f"| `{base}` | server default, median of 3 | {f1(med([tps(d) for d in ba]), ' tps')} | {f1(med([tps(d) for d in Mo(2, base)]), ' tps')} | "
              f"{'/'.join(str(d['meta']['verify']['failures']) for d in ba)} of 400 | {'/'.join(str(d['meta']['outcomes']['conflicted']) for d in ba)} |\n")
        if a:
            s += (f"| `{tag}` | repeatable-read, 1 run | {f1(tps(a), ' tps')} | {f1(tps(b), ' tps') if b else 'n/a'} | {a['meta']['verify']['failures']} of {a['meta']['verify']['checks']} | "
                  f"{a['meta']['outcomes']['conflicted']} |\n")
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        print(globals()[f]())
