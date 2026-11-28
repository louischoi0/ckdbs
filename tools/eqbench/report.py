#!/usr/bin/env python3
"""Tables from a matrix.py run: one row per scenario cell, phase and engine
arm, the rounds summarised (median of the round means, the spread across
them), the client's and the server's CPU, each engine's breakdown and its
top wait and lock events.

    report.py --run DIR --engines kds,pg            > public.md   (committed)
    report.py --run DIR --engines kds,pg,oracle     > private.md  (bench/private/)

With `--engines` leaving Oracle out, no Oracle figure is read or written
(bench/README.md, "The Oracle control").
"""

import argparse
import glob
import json
import os
import re
import statistics
from collections import defaultdict

CATS = ["parse", "optimise", "parse_optimise", "execution", "disk_read", "commit", "lock", "network",
        "formatting_client"]
ENGINE_ORDER = {"kds": 0, "pg": 1, "oracle": 2}


def load(run, pass_, engines):
    out = defaultdict(list)  # (scenario, cell, arm) -> [json per round]
    for p in sorted(glob.glob(os.path.join(run, pass_, "r*-*.json"))):
        m = re.match(r"r(\d+)-(s\d)-(.+)-(kds|pg|oracle)(?:-(group|strict))?\.json$", os.path.basename(p))
        if not m:
            continue
        rnd, sc, cell, eng, dur = m.groups()
        if eng not in engines:
            continue
        d = json.load(open(p))
        arm = eng + (f"-{dur}" if dur else "")
        d["_round"] = int(rnd)
        out[(sc, cell, arm)].append(d)
    return out


def good(d):
    """A round that ran and passed every check: the only kind a figure comes
    from (EO §3, "a scenario whose checks fail on an engine reports no
    number for it")."""
    return not d.get("failed") and all(c["ok"] for c in d.get("checks", []))


def digests_line(timed, sc):
    """Scenario 4's answers compared across every run read - each arm, each
    engine, each round (EO §3): the cells run one arm a server, so no run
    compares them itself."""
    seen = {}
    for (s, cell, arm), ds in timed.items():
        if s != sc:
            continue
        for d in ds:
            if not good(d):
                continue
            for a, groups in d.get("meta", {}).get("digests", {}).items():
                key = json.dumps(groups, sort_keys=True)
                seen.setdefault(key, []).append(f"{cell} {arm} {a} r{d['_round']}")
    if not seen:
        return None
    if len(seen) == 1:
        runs = next(iter(seen.values()))
        return f"Answers: every group's digest equal across all {len(runs)} runs read (each arm, engine and round)."
    return "Answers DIFFER: " + " | ".join(f"{len(v)} runs ({', '.join(v[:3])}{', ...' if len(v) > 3 else ''})"
                                         for v in seen.values()) + "."


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def spread(xs):
    xs = [x for x in xs if x]
    if len(xs) < 2:
        return None
    m = statistics.median(xs)
    return 100.0 * (max(xs) - min(xs)) / m if m else None


def f(v, unit="", nd=1):
    if v is None:
        return "-"
    return f"{v:,.{nd}f}{(' ' + unit) if unit else ''}"


def arm_key(arm):
    e = arm.split("-")[0]
    return (ENGINE_ORDER.get(e, 9), arm)


def render(run, engines):
    timed = load(run, "timed", engines)
    prof = load(run, "profile", engines) if "pg" in engines else {}
    lines = []
    cells = sorted({(sc, cell) for sc, cell, _ in timed})
    for sc, cell in cells:
        arms = sorted({a for s, c, a in timed if (s, c) == (sc, cell)}, key=arm_key)
        lines.append(f"\n### {sc} `{cell}`\n")
        # checks
        bad = []
        for arm in arms:
            for d in timed[(sc, cell, arm)]:
                if d.get("failed"):
                    bad.append(f"{arm} r{d['_round']}: run failed (rc {d.get('rc')})")
                for c in d.get("checks", []):
                    if not c["ok"]:
                        bad.append(f"{arm} r{d['_round']}: {c['name']} {c['detail']}")
        # Every figure below is from the rounds that passed.
        runs = {arm: [d for d in timed[(sc, cell, arm)] if good(d)] for arm in arms}
        lines.append("Checks: " + ("every check passed in every round" if not bad else "; ".join(bad)) +
                     ". Rounds with figures (passed of run): " +
                     ", ".join(f"{a} {len(runs[a])} of {len(timed[(sc, cell, a)])}" for a in arms) + ".\n")
        phases = []
        for arm in arms:
            for d in runs[arm]:
                for p in d.get("phases", []):
                    if p["name"] not in phases:
                        phases.append(p["name"])
        # latency and rate table
        lines.append("| phase | arm | ops a round | mean (spread) | p0 | p25 | p50 | p95 | p99 | rate | client CPU/op | decode/op | server CPU/op | errors |")
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for ph in phases:
            for arm in arms:
                ps = [p for d in runs[arm] for p in d.get("phases", []) if p["name"] == ph]
                if not ps:
                    continue
                means = [p["latency"]["mean_us"] for p in ps]
                L = lambda k: med([p["latency"][k] for p in ps])
                errs = defaultdict(int)
                for p in ps:
                    for k, v in p["errors"].items():
                        errs[k] += v
                sp = spread(means)
                lines.append(
                    f"| {ph} | {arm} | {f(med([p['latency']['ops'] for p in ps]), 'ops', 0)} | "
                    f"{f(med(means), 'µs')} ({f(sp, '%')}) | {f(L('p0_us'), 'µs')} | {f(L('p25_us'), 'µs')} | "
                    f"{f(L('p50_us'), 'µs')} | {f(L('p95_us'), 'µs')} | {f(L('p99_us'), 'µs')} | "
                    f"{f(med([p['throughput_ops'] for p in ps]), 'ops/s', 0)} | "
                    f"{f(med([p['client_cpu_us_per_op'] for p in ps]), 'µs')} | "
                    f"{f(med([p['client_decode_us_per_op'] for p in ps]), 'µs', 2)} | "
                    f"{f(med([p['server_cpu_us_per_op'] for p in ps]), 'µs')} | "
                    f"{', '.join(f'{k} {v}' for k, v in sorted(errs.items())) or '0 errors'} |")
        # per-kind latency, where a phase mixes kinds
        kinds_rows = []
        for ph in phases:
            for arm in arms:
                ps = [p for d in runs[arm] for p in d.get("phases", []) if p["name"] == ph]
                names = sorted({k for p in ps for k in p.get("kinds", {})})
                for k in names:
                    ks = [p["kinds"][k] for p in ps if k in p.get("kinds", {})]
                    kinds_rows.append(
                        f"| {ph} | {k} | {arm} | {f(med([x['ops'] for x in ks]), 'ops', 0)} | "
                        f"{f(med([x['mean_us'] for x in ks]), 'µs')} | {f(med([x['p50_us'] for x in ks]), 'µs')} | "
                        f"{f(med([x['p95_us'] for x in ks]), 'µs')} | {f(med([x['p99_us'] for x in ks]), 'µs')} |")
        if kinds_rows:
            lines.append("\n| phase | kind | arm | ops a round | mean | p50 | p95 | p99 |")
            lines.append("|---|---|---|---|---|---|---|---|")
            lines += kinds_rows
        # breakdown
        lines.append("\nWhere the time went, µs an operation by each engine's own accounting "
                     "(`-`: not reported by the engine; PostgreSQL's from the profile pass):\n")
        lines.append("| phase | arm | " + " | ".join(CATS) + " | top events (an op: count, µs) |")
        lines.append("|---|---|" + "---|" * len(CATS) + "---|")
        for ph in phases:
            for arm in arms:
                src = [d for d in prof.get((sc, cell, arm), []) if good(d)] if arm == "pg" else runs[arm]
                if not src:
                    continue
                ps = [p for d in src for p in d.get("phases", []) if p["name"] == ph]
                if not ps:
                    continue
                row = []
                for c in CATS:
                    row.append(f(med([p["breakdown"].get(c) for p in ps]), "µs", 2))
                # Each round's totals over that round's own operations, then
                # the median across rounds: rounds differ in their counts, so
                # a mean of totals over a median count is no round's figure.
                per_round = defaultdict(lambda: ([], []))
                for p in ps:
                    n = max(1, p["latency"]["ops"])
                    for e in p.get("events", []):
                        if e["class"] == "Idle":
                            continue
                        per_round[e["name"]][0].append(e["count"] / n)
                        per_round[e["name"]][1].append(e["total_us"] / n)
                ev = {k: (statistics.median(c + [0.0] * (len(ps) - len(c))),
                          statistics.median(t + [0.0] * (len(ps) - len(t)))) for k, (c, t) in per_round.items()}
                top = sorted(ev.items(), key=lambda kv: -kv[1][1])[:4]
                evs = "; ".join(f"{n} ({c:,.2f}, {t:,.1f} µs)" for n, (c, t) in top) or "none"
                lines.append(f"| {ph} | {arm} | " + " | ".join(row) + f" | {evs} |")
    s4 = digests_line(timed, "s4")
    if s4:
        lines.append(f"\n### s4 answers\n\n{s4}\n")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--run", required=True)
    p.add_argument("--engines", required=True)
    a = p.parse_args()
    print(render(a.run, set(a.engines.split(","))))


if __name__ == "__main__":
    main()
