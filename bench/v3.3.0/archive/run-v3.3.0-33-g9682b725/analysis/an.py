"""Shared helpers for the v3.3.0-33-g9682b725 scenario 1-4 write-up."""
import json, os, statistics

NEW = "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/json"
WT = "/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4"
OLDD = WT + "/bench/v3.1.0/archive/scenario{n}-v3.1.0-30-gcd7c7c4e/json"
PFX = ["", "r2-", "r3-"]


def jl(path):
    try:
        return json.load(open(path))
    except Exception:
        return None


def new(tag):
    return jl(f"{NEW}/{tag}.json")


def old(n, tag):
    return jl(OLDD.format(n=n) + f"/{tag}.json")


def runs(fn, tagfmt, prefixes=PFX):
    """list of loaded JSON for each prefix that exists"""
    out = []
    for p in prefixes:
        d = fn(p + tagfmt)
        if d:
            out.append(d)
    return out


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


def f1(x, unit=""):
    return "n/a" if x is None else f"{x:,.1f}{unit}"


def f0(x, unit=""):
    return "n/a" if x is None else f"{x:,.0f}{unit}"


def sp(x):
    return "n/a" if x is None else f"{x:.1f} %"


def ratio(a, b):
    return "n/a" if (a is None or b is None or b == 0) else f"{a / b:.2f}x"


def delta(a, b):
    return "n/a" if (a is None or b is None or b == 0) else f"{100.0 * (a - b) / b:+.1f} %"


def pct_row(p):
    return (f"{p['ops']:,} ops | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | "
            f"{p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} errors")
