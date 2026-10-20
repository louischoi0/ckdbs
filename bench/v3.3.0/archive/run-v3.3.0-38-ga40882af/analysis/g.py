"""Generators for the v3.3.0-38-ga40882af write-up: now (new client) vs the morning (v3.3.0-33-g9682b725,
same binary, old client) vs PostgreSQL (morning's fresh run, reused)."""
import json, os, statistics, re

RUN = "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af"
WT = "/home/cdkbs/ckdbs/.claude/worktrees/kwp-decoder-fast-path"
MA = WT + "/bench/v3.3.0/archive"
MD = "v3.3.0-33-g9682b725"
DESC = "v3.3.0-38-ga40882af"
NOWJ = RUN + "/json"
PFX = ["", "r2-", "r3-"]


def jl(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def now(tag):
    return jl(f"{NOWJ}/{tag}.json")


def mor(n, tag):
    return jl(f"{MA}/scenario{n}-{MD}/json/{tag}.json")


def runs(fn, tag, prefixes=PFX):
    out = []
    for p in prefixes:
        d = fn(p + tag)
        if d:
            out.append(d)
    return out


def N(tag, pf=PFX):
    return runs(now, tag, pf)


def Mo(n, tag, pf=PFX):
    return runs(lambda t: mor(n, t), tag, pf)


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def spread(xs):
    xs = [x for x in xs if x is not None]
    if len(xs) < 2:
        return None
    m = statistics.median(xs)
    return 100.0 * (max(xs) - min(xs)) / m if m else None


def ph(d, name):
    for p in d["phases"]:
        if p["phase"] == name:
            return p
    return None


def f1(x, u=""):
    return "n/a" if x is None else f"{x:,.1f}{u}"


def f0(x, u=""):
    return "n/a" if x is None else f"{x:,.0f}{u}"


def sp(x):
    return "n/a" if x is None else f"{x:.1f} %"


def ratio(a, b):
    return "n/a" if (a is None or b is None or b == 0) else f"{a / b:.2f}x"


def delta(a, b):
    return "n/a" if (a is None or b is None or b == 0) else f"{100.0 * (a - b) / b:+.1f} %"


def pv(rr, name, key="qps"):
    return [ph(d, name)[key] for d in rr if ph(d, name)]


def pct_row(p):
    return (f"{p['ops']:,} ops | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | "
            f"{p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} errors")


PCT_HEAD = ("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |\n|---|---|---|---|---|---|---|---|---|\n")


def pct_table(d, names=None, label=None):
    s = PCT_HEAD
    for p in d["phases"]:
        if names is None or p["phase"] in names:
            s += f"| {label + ' ' if label else ''}{p['phase']} | " + pct_row(p) + " |\n"
    return s


def sweep(d):
    return {s["shape"]: s for s in d["meta"]["qps_sweep"]}


def write(name, text):
    p = f"{WT}/bench/v3.3.0/{name}-{DESC}.md"
    open(p, "w").write(text)
    print("wrote", p, len(text.splitlines()), "lines")
