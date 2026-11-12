"""Tables for the CQ-S0 results file, from one pass's JSON summaries.
usage: gen_tables.py <pass dir> <tables.md out>   (prints the prose numbers)"""
import json
import statistics as st
import sys

A, OUT = sys.argv[1], sys.argv[2]
ORDER = ["hot", "hot-last", "hot-last-piped", "disjoint", "disjoint-piped"]


def load(f):
    return json.load(open(f"{A}/{f}.json"))["cells"]


def grp(cells):
    by = {}
    for c in cells:
        by.setdefault((c["k"], c["arm"]), []).append(c)
    return by


def keys(by):
    return [(k, arm) for k in sorted({k for k, _ in by}) for arm in ORDER if (k, arm) in by]


def tput(cells):
    by = grp(cells)
    out = ["| k | arm | committed TPS, the three repetitions | median | refused decided-newer | share of attempts | other refusals | oracle | server CPU |",
           "|---|---|---|---|---|---|---|---|---|"]
    for k, arm in keys(by):
        cs = by[(k, arm)]
        tps = sorted(c["tps"] for c in cs)
        dn = sum(c["refused"].get("decided_newer", 0) for c in cs)
        oth = sum(v for c in cs for a, v in c["refused"].items() if a != "decided_newer")
        com = sum(c["committed"] for c in cs)
        ok = sum(1 for c in cs if c["oracle"]["ok"])
        cpu = st.median(c["server_cpu_s"] / (c["seconds"] + 1.0) for c in cs)
        out.append(f"| {k} | {arm} | `{tps[0]:,.0f}` / `{tps[1]:,.0f}` / `{tps[2]:,.0f} tps` | "
                   f"`{st.median(tps):,.0f} tps` | `{dn:,} txns` | `{100 * dn / (dn + com + oth):.1f} %` | "
                   f"`{oth} txns` | exact, `{ok} of {len(cs)} cells` | `{cpu:.2f} cores` |")
    return "\n".join(out)


def lat(cells, what):
    by = grp(cells)
    out = ["| k | arm | p0 | p25 | p50 | p95 | p99 | max |", "|---|---|---|---|---|---|---|---|"]
    for k, arm in keys(by):
        cs = by[(k, arm)]
        if not cs[0][what]:
            continue
        f = lambda key: f"`{st.median(c[what][key] for c in cs):,.0f} us`"
        out.append(f"| {k} | {arm} | {f('p0_us')} | {f('p25_us')} | {f('p50_us')} | "
                   f"{f('p95_us')} | {f('p99_us')} | {f('max_us')} |")
    return "\n".join(out)


def waits(cells):
    by = grp(cells)
    out = ["| k | arm | new `sys.lock_waits` rows, per repetition | outcomes | unit/mode | waited p50 | waited p99 |",
           "|---|---|---|---|---|---|---|"]
    for k, arm in keys(by):
        cs = by[(k, arm)]
        n = [c["lock_waits"].get("new_rows", 0) for c in cs]
        oc, um = {}, {}
        for c in cs:
            for a, b in c["lock_waits"].get("by_outcome", {}).items():
                oc[a] = oc.get(a, 0) + b
            for a, b in c["lock_waits"].get("by_unit_mode", {}).items():
                um[a] = um.get(a, 0) + b
        p50 = [c["lock_waits"]["waited_p50_us"] for c in cs if "waited_p50_us" in c["lock_waits"]]
        p99 = [c["lock_waits"]["waited_p99_us"] for c in cs if "waited_p99_us" in c["lock_waits"]]
        sat = " (ring full)" if all(x == 16384 for x in n) else ""
        out.append(f"| {k} | {arm} | `{n[0]:,}` / `{n[1]:,}` / `{n[2]:,} rows`{sat} | "
                   f"{', '.join(f'{a} `{b:,}`' for a, b in oc.items()) or 'none'} | {', '.join(um) or '-'} | "
                   f"{('`%s us`' % format(st.median(p50), ',.0f')) if p50 else '-'} | "
                   f"{('`%s us`' % format(st.median(p99), ',.0f')) if p99 else '-'} |")
    return "\n".join(out)


def cores(cells):
    """How the eight sessions were spread at each cell's start."""
    spreads = {}
    moved = 0
    for c in cells:
        s = c["cores_start"]
        spreads[tuple(sorted(s.count(x) for x in range(4)))] = spreads.get(
            tuple(sorted(s.count(x) for x in range(4))), 0) + 1
        moved += sum(1 for a, b in zip(s, c["cores_end"]) if a != b)
    return spreads, moved


with open(OUT, "w") as o:
    for name, f in (("M1 group", "m1-group"), ("M2 strict", "m2-strict")):
        c = load(f)
        o.write(f"### {name} throughput\n\n{tput(c)}\n\n### {name} txn latency\n\n{lat(c, 'txn')}\n\n"
                f"### {name} debit latency\n\n{lat(c, 'debit')}\n\n### {name} commit latency\n\n"
                f"{lat(c, 'commit')}\n\n### {name} waits\n\n{waits(c)}\n\n")

# ---- the prose numbers -------------------------------------------------
for name, f in (("group", "m1-group"), ("strict", "m2-strict")):
    cells = load(f)
    by = grp(cells)
    M = {key: st.median(c["tps"] for c in cs) for key, cs in by.items()}
    R = {key: (min(c["tps"] for c in cs), max(c["tps"] for c in cs)) for key, cs in by.items()}
    for k in sorted({k for k, _ in by}):
        g = lambda arm: M.get((k, arm))
        hot, dis = g("hot"), g("disjoint")
        print(f"== {name} k={k}: " + " ".join(f"{a}={g(a):,.0f}{R[(k, a)]}" for a in ORDER if g(a)))
        print(f"   disjoint/hot {dis / hot:.2f}x  hot is {100 * hot / dis:.0f}% of disjoint (loss {100 - 100 * hot / dis:.0f}%)")
        if g("hot-last"):
            print(f"   hot-last/hot {g('hot-last') / hot:.2f}x  recovers {100 * (g('hot-last') - hot) / (dis - hot):.0f}% of gap")
        if g("hot-last-piped"):
            hp, dp = g("hot-last-piped"), g("disjoint-piped")
            print(f"   piped/hot {hp / hot:.2f}x  recovers {100 * (hp - hot) / (dis - hot):.0f}%  piped/disjoint-piped {100 * hp / dp:.1f}%")
        for arm in ORDER:
            cs = by.get((k, arm))
            if not cs:
                continue
            dn = sum(c["refused"].get("decided_newer", 0) for c in cs)
            com = sum(c["committed"] for c in cs)
            oth = {a: v for c in cs for a, v in c["refused"].items() if a != "decided_newer"}
            d = cs[0]["debit"] and (st.median(c["debit"]["p50_us"] for c in cs), st.median(c["debit"]["p99_us"] for c in cs))
            cm = (st.median(c["commit"]["p50_us"] for c in cs))
            print(f"   {arm}: refused {dn} share {100 * dn / (dn + com):.2f}% other {oth} debit p50/p99 {d} commit p50 {cm:.0f} load {[c['host']['loadavg'].split()[0] for c in cs]} comp {[len(c['host']['competing']) for c in cs]} fatal {sum(len(c['fatal']) for c in cs)}")
    sp, mv = cores(cells)
    print("   core spreads at start (sorted counts per core -> cells):", sp, "sessions that ended on another core:", mv, "of", 8 * len(cells))
    print("   oracle ok:", sum(c["oracle"]["ok"] for c in cells), "of", len(cells))
c = load("m4p-group")[0]
print("M4p", c["committed"], c["tps"], c["refused"], c["lock_waits"], c["debit"], c["oracle"], c["cores_start"], c["server_cpu_s"])
