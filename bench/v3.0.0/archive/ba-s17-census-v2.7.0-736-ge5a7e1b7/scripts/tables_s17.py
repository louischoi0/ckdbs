"""Tables for the BA-S17 census file that analysis.md and scaling.md do not carry.
usage: tables_s17.py <B kds dir> <S4 archive dir> <pgctl dir>
Reads run JSONs only (median over the runs of a cell). Prints markdown to stdout."""
import collections, glob, json, statistics as st, sys

BD, S4, PGC = sys.argv[1], sys.argv[2], sys.argv[3]


def load(d):
    o = collections.defaultdict(list)
    for f in sorted(glob.glob(d + "/*.json")):
        r = json.load(open(f))
        o[(r["shape"], r["durability"], r["cores"], r["sessions"])].append(r)
    return o


B, A = load(BD), load(S4 + "/kds")
PG = collections.defaultdict(list)
for sub, cpus in (("pg-cpus0", "0"), ("pg-cpus0-2", "0,2")):
    for f in glob.glob(S4 + "/" + sub + "/*.json"):
        r = json.load(open(f))
        PG[(r["shape"], r["durability"], cpus, r["sessions"])].append(r["statements_per_s"])


def m(D, k, f="statements_per_s"):
    v = D.get(k)
    return st.median(r[f] for r in v) if v else None


def fmt(x):
    return "-" if x is None else format(x, ",.0f") + " stmt/s"


SESS = (1, 2, 4, 8, 16)
print("### Relaxed, every session count\n")
print("| shape | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |")
print("|---|---|---|---|---|---|---|---|")
for shape in ("point", "trade", "insert1", "insertN", "recent"):
    for k, cpus in ((1, "0"), (2, "0,2")):
        for name, D in (("B", B), ("BA-S4", A)):
            print(f"| {shape} | {k} | KDS `relaxed`, {name} | " + " | ".join(fmt(m(D, (shape, "relaxed", k, s))) for s in SESS) + " |")
        print(f"| {shape} | CPUs {cpus} | PostgreSQL `off`, BA-S4 | " + " | ".join(fmt(st.median(PG[(shape, "off", cpus, s)])) for s in SESS) + " |")

print("\n### What the second core buys at 16 sessions (`cores = 2` over `cores = 1`, PostgreSQL CPUs 0,2 over CPU 0)\n")
print("| shape | durability | B | BA-S4 | PostgreSQL (BA-S4) |")
print("|---|---|---|---|---|")
for shape in ("point", "trade", "insert1", "insertN", "recent"):
    for dur, pgd in (("relaxed", "off"), ("group", "on"), ("strict", "on")):
        rb = m(B, (shape, dur, 2, 16)) / m(B, (shape, dur, 1, 16))
        ra = m(A, (shape, dur, 2, 16)) / m(A, (shape, dur, 1, 16))
        rp = st.median(PG[(shape, pgd, "0,2", 16)]) / st.median(PG[(shape, pgd, "0", 16)])
        print(f"| {shape} | {dur} | {rb:.2f}x | {ra:.2f}x | {rp:.2f}x (`{pgd}`) |")

print("\n### Latency distribution of one statement (median over the cell's 3 runs of each run's percentile)\n")
print("| cell | census | statements per run | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|")
for shape, dur, k, s in (("insert1", "relaxed", 2, 16), ("insert1", "group", 2, 1), ("insert1", "group", 2, 16),
                         ("insert1", "strict", 2, 1), ("insert1", "strict", 2, 16), ("insert1", "group", 1, 16), ("point", "relaxed", 1, 1)):
    for name, D in (("B", B), ("BA-S4", A)):
        v = D[(shape, dur, k, s)]
        g = lambda f: st.median(r[f] for r in v)
        print(f"| {shape} `{dur}` `cores = {k}`, {s} session{'s' if s != 1 else ''} | {name} | {st.median(r['statements'] for r in v):,.0f} statements | "
              f"{g('p0_us'):,.0f} µs | {g('p25_us'):,.0f} µs | {g('p50_us'):,.0f} µs | {g('p95_us'):,.0f} µs | {g('p99_us'):,.0f} µs |")

print("\n### Same-day PostgreSQL control (BA-S4's `on` cells re-run after the KDS census)\n")
print("| shape | CPUs | sessions | today (3 runs) | BA-S4 median | today over BA-S4 |")
print("|---|---|---|---|---|---|")
rows = []
for sub in sorted(glob.glob(PGC + "/pg-cpus*")):
    cpus = sub.split("cpus")[1].replace("-", ",")
    d = collections.defaultdict(list)
    for f in sorted(glob.glob(sub + "/*.json")):
        r = json.load(open(f))
        d[(r["shape"], cpus, r["sessions"])].append(r["statements_per_s"])
    for k, v in sorted(d.items()):
        base = st.median(PG[(k[0], "on", k[1], k[2])])
        rows.append((k, v, base))
        print(f"| {k[0]} | {k[1]} | {k[2]} | " + ", ".join(f"{x:,.0f} stmt/s" for x in v) + f" | {base:,.0f} stmt/s | {(st.median(v) / base - 1) * 100:+.1f} % |")

print("\n### Cells BA did not change, B over BA-S4 at `cores = 1` (median of cell medians)\n")
print("| durability | sessions | shapes | B over BA-S4, per shape | median |")
print("|---|---|---|---|---|")
for dur in ("relaxed", "group", "strict"):
    for s in SESS:
        xs = [(sh, m(B, (sh, dur, 1, s)) / m(A, (sh, dur, 1, s)) - 1) for sh in ("point", "trade", "insert1", "insertN", "recent")]
        print(f"| {dur} | {s} session{'s' if s != 1 else ''} | 5 shapes | " + ", ".join(f"{sh} {x * 100:+.1f} %" for sh, x in xs) + f" | {st.median(x for _, x in xs) * 100:+.1f} % |")

print("\n### Between-run spread inside a cell (max/min - 1 of statements/s over the 3 runs)\n")
for name, D in (("B", B), ("BA-S4", A)):
    sp = [(max(r["statements_per_s"] for r in v) / min(r["statements_per_s"] for r in v) - 1) * 100 for v in D.values()]
    print(f"- {name}: median {st.median(sp):.1f} %, p90 {sorted(sp)[int(0.9 * len(sp))]:.1f} %, max {max(sp):.1f} % over {len(sp)} cells")
