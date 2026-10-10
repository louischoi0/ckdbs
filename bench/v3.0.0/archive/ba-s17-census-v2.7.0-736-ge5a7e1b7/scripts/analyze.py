#!/usr/bin/env python3
"""BA-S17 census analysis: B (this run) against BA-S4's KDS rows. usage: analyze.py <B kds dir> <S4 archive dir> [contaminated.txt]

Reads every run JSON of both censuses. Per cell: median over runs of statements/s. Items follow BA-S4's analyze.py
(shares are of cores x span; the sync gate includes writer-thread waits, which count into slot 0)."""
import glob, json, os, statistics as st, sys
from collections import defaultdict

BD, S4 = sys.argv[1], sys.argv[2]
BAD = set(open(sys.argv[3]).read().split()) if len(sys.argv) > 3 and os.path.exists(sys.argv[3]) else set()


def load(d, skip=()):
    out = []
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        if os.path.basename(f) in skip:
            continue
        r = json.load(open(f)); r["_f"] = os.path.basename(f); out.append(r)
    return out


B = load(BD, BAD)
A = load(os.path.join(S4, "kds"))
PG = {}
for sub, cpus in (("pg-cpus0", "0"), ("pg-cpus0-2", "0,2"), ("pg-cpus0-2-4-6", "0,2,4,6")):
    for r in load(os.path.join(S4, sub)):
        PG.setdefault((r["shape"], r["durability"], cpus, r["sessions"]), []).append(r["statements_per_s"])


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None


def key(r): return (r["shape"], r["durability"], r["cores"], r["sessions"])


def client_bound(r):
    n = len(r["client_cpus"].split(","))
    per = defaultdict(float)
    for i, s in enumerate(r["client_cpu_share"]):
        per[i % n] += s
    return max(per.values()) > 0.80


def cells(rs):
    d = defaultdict(list)
    for r in rs:
        d[key(r)].append(r)
    return d


cb, ca = cells(B), cells(A)
ITEMS = {
    "P1 frame table": ["frame_table", "free_map", "free_map_flush"], "P2 window": ["window"],
    "P3 statistics": ["optimizer", "cabin_stats", "cabin_partition"], "P4 relation borrow": ["lock_partition", "lock_wait_for"],
    "P6 WAL append": ["wal_stream"], "P7 sync gate": ["wal_sync_gate"], "P7 snapshot ceiling": ["ceiling"],
    "P8 assertion dir": ["assertion_dir"], "superblock": ["superblock"],
}


def share(r, kinds):
    span = r["span_s"] * 1e6 * max(1, r["cores"])
    return sum(r["meta_delta"].get(f"contention_{k}_wait_us", 0) for k in kinds) / span


mode = sys.argv[4] if len(sys.argv) > 4 else "all"
print(f"B runs: {len(B)} in {len(cb)} cells; S4 KDS runs: {len(A)} in {len(ca)} cells; excluded as contaminated: {len(BAD)}\n")

if mode in ("all", "tp"):
    print("## Throughput per cell, B against BA-S4 (median of runs)\n")
    print("| shape | durability | cores | sessions | runs B | B stmt/s | S4 stmt/s | B vs S4 | B p50 | S4 p50 | B p99 | S4 p99 | B client-bound runs |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for k in sorted(cb):
        b = cb[k]; a = ca.get(k, [])
        tb = med(r["statements_per_s"] for r in b); ta = med(r["statements_per_s"] for r in a)
        d = f"{(tb / ta - 1) * 100:+.1f} %" if ta else "-"
        print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {len(b)} | {tb:,.0f} stmt/s | {ta:,.0f} stmt/s | {d} | {med(r['p50_us'] for r in b):,.0f} µs | "
              f"{med(r['p50_us'] for r in a):,.0f} µs | {med(r['p99_us'] for r in b):,.0f} µs | {med(r['p99_us'] for r in a):,.0f} µs | {sum(client_bound(r) for r in b)} of {len(b)} |")

if mode in ("all", "items"):
    for name, rs in (("B", B), ("S4", A)):
        print(f"\n## Item shares, {name} (contended wait over cores x span; median of runs per cell)\n")
        print("| item | max share, all cells | cell | max share, clean cells (cores <= 2) | clean cell |")
        print("|---|---|---|---|---|")
        cc = cells(rs)
        for item, kinds in ITEMS.items():
            m = {k: med(share(r, kinds) for r in v) for k, v in cc.items()}
            km = max(m, key=m.get)
            cl = {k: v for k, v in m.items() if k[2] <= 2}
            kc = max(cl, key=cl.get)
            print(f"| {item} | {m[km] * 100:.2f} % | {km[0]} {km[1]} k{km[2]} s{km[3]} | {cl[kc] * 100:.2f} % | {kc[0]} {kc[1]} k{kc[2]} s{kc[3]} |")
        # P7 per durability, clean cells
    print("\n## P7 shares by durability (max over cells with cores >= 2 of the median share), B and S4\n")
    print("| item | durability | B max | B cell | S4 max | S4 cell |")
    print("|---|---|---|---|---|---|")
    for item in ("P7 sync gate", "P7 snapshot ceiling", "P6 WAL append"):
        for dur in ("relaxed", "group", "strict"):
            row = []
            for rs in (B, A):
                cc = cells(rs)
                m = {k: med(share(r, ITEMS[item]) for r in v) for k, v in cc.items() if k[1] == dur and k[2] >= 2}
                km = max(m, key=m.get) if m else None
                row += [f"{m[km] * 100:.2f} %", f"{km[0]} k{km[2]} s{km[3]}"] if km else ["-", "-"]
            print(f"| {item} | {dur} | " + " | ".join(row) + " |")

if mode in ("all", "stall"):
    print("\n## P12: carve and checkpoint, B\n")
    for k in ("contention_carve_longest_us", "contention_checkpoint_longest_us"):
        vals = [r["meta_longest"].get(k, 0) for r in B]
        over = [v for v in vals if v > 10_000]
        print(f"- `{k}` (max over each server's life): max {max(vals) / 1000:,.1f} ms, median {st.median(vals) / 1000:,.2f} ms, "
              f"p99 over runs {sorted(vals)[int(0.99 * len(vals))] / 1000:,.1f} ms; runs over 10 ms: {len(over)} of {len(vals)}")
    for k in ("contention_carve_longest_us", "contention_checkpoint_longest_us"):
        vals = [r["meta_longest"].get(k, 0) for r in A]
        over = [v for v in vals if v > 10_000]
        print(f"- S4 `{k}`: max {max(vals) / 1000:,.1f} ms, median {st.median(vals) / 1000:,.2f} ms; runs over 10 ms: {len(over)} of {len(vals)}")
    print("\nCarve longest by (durability, cores), B, max and median over runs (ms):\n")
    print("| durability | cores | runs | carve max | carve median | carve runs > 10 ms | checkpoint max | checkpoint median | checkpoint runs > 10 ms |")
    print("|---|---|---|---|---|---|---|---|---|")
    g = defaultdict(list)
    for r in B:
        g[(r["durability"], r["cores"])].append(r)
    for k in sorted(g):
        c = [r["meta_longest"].get("contention_carve_longest_us", 0) / 1000 for r in g[k]]
        h = [r["meta_longest"].get("contention_checkpoint_longest_us", 0) / 1000 for r in g[k]]
        print(f"| {k[0]} | {k[1]} | {len(c)} | {max(c):,.1f} ms | {st.median(c):,.2f} ms | {sum(x > 10 for x in c)} | {max(h):,.1f} ms | {st.median(h):,.2f} ms | {sum(x > 10 for x in h)} |")
    print("\nCarves and checkpoint runs inside the measured span (sum over runs), B: "
          f"carves {sum(r['meta_delta'].get('contention_carves', 0) for r in B):,.0f}, checkpoint runs {sum(r['meta_delta'].get('contention_checkpoint_runs', 0) for r in B):,.0f}")

if mode in ("all", "refuse"):
    print("\n## Refusals and re-runs, B\n")
    tot = defaultdict(float)
    for r in B:
        for k, v in r["meta_delta"].items():
            if k.startswith("contention_refused_") or k == "contention_structural_reruns":
                tot[k] += v
    for k in sorted(tot):
        print(f"- `{k}`: {tot[k]:,.0f}")
    print(f"- client-visible `refusals` field (TXN_CONFLICT retried by the driver): {sum(r['refusals'] for r in B)}; errors: {sum(r['errors'] for r in B)}; first_error values: {sorted({str(r['first_error']) for r in B if r['first_error']})}")
    by = defaultdict(float)
    for r in B:
        by[(r["shape"], r["durability"], r["cores"])] += r["meta_delta"].get("contention_structural_reruns", 0)
    print("\nStructural re-runs by (shape, durability, cores):\n")
    for k, v in sorted(by.items()):
        if v:
            print(f"- {k}: {v:,.0f}")
    print("\nS4 refusals (client-visible `refusals`) for comparison: " + str(sum(r["refusals"] for r in A)) +
          "; S4 contention_refused_*: " + str({k: sum(r['meta_delta'].get(k, 0) for r in A) for k in sorted({k for r in A for k in r['meta_delta'] if k.startswith('contention_refused_')}) if sum(r['meta_delta'].get(k, 0) for r in A)}))

if mode in ("all", "premise"):
    print("\n## BA-R4's premise: a session's latency by its core, cores = 2\n")
    print("| census | shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients core 0 / peers | statements per client core 0 / peers |")
    print("|---|---|---|---|---|---|---|---|---|")
    for name, rs in (("B", B), ("S4", A)):
        for shape in ("insert1", "insertN", "trade"):
            for dur in ("group", "strict"):
                c0, pe = [], []
                for r in rs:
                    if r["shape"] == shape and r["durability"] == dur and r["cores"] == 2:
                        for c in r.get("by_client", []):
                            if c["statements"]:
                                (c0 if c["core"] == 0 else pe).append(c)
                f = lambda xs, q: f"{med(c[q] for c in xs):,.0f} µs" if xs else "-"
                print(f"| {name} | {shape} | {dur} | {f(c0, 'p50_us')} | {f(c0, 'p99_us')} | {f(pe, 'p50_us')} | {f(pe, 'p99_us')} | {len(c0)} / {len(pe)} | "
                      f"{med(c['statements'] for c in c0) if c0 else '-'} / {med(c['statements'] for c in pe) if pe else '-'} |")

if mode in ("all", "p11"):
    print("\n## P11 proxy: runs with a session on every core against the rest, same cell\n")
    for name, rs in (("B", B), ("S4", A)):
        gains = []
        lines = []
        for k, v in sorted(cells(rs).items()):
            if k[2] == 1:
                continue
            bal = [r for r in v if len(set(r["session_cores"])) == k[2]]
            unb = [r for r in v if len(set(r["session_cores"])) != k[2]]
            if bal and unb:
                g = med(r["statements_per_s"] for r in bal) / med(r["statements_per_s"] for r in unb) - 1
                gains.append((k, g, len(bal), len(unb)))
        print(f"- {name}: {len(gains)} cells with both kinds of run; every-core runs faster by over 10 % in {sum(g > 0.10 for _, g, _, _ in gains)}, slower by over 10 % in {sum(g < -0.10 for _, g, _, _ in gains)}; "
              f"median gain {st.median(g for _, g, _, _ in gains) * 100:+.1f} %" if gains else f"- {name}: none")
        by = defaultdict(list)
        for k, g, nb, nu in gains:
            by[k[1]].append(g)
        for dur, gs in by.items():
            print(f"  - {name} {dur}: {len(gs)} cells, median {st.median(gs) * 100:+.1f} %, faster>10%: {sum(x > 0.1 for x in gs)}, slower>10%: {sum(x < -0.1 for x in gs)}")
    # Also: all runs of a cell, spread (max/min) -> noise for the proxy
    print("\nBetween-run spread (max/min - 1) of statements/s inside a cell, median over cells: " +
          f"B {st.median((max(r['statements_per_s'] for r in v) / min(r['statements_per_s'] for r in v) - 1) * 100 for v in cb.values() if len(v) == 3):.1f} %, "
          f"S4 {st.median((max(r['statements_per_s'] for r in v) / min(r['statements_per_s'] for r in v) - 1) * 100 for v in ca.values() if len(v) == 3):.1f} %")

if mode in ("all", "sync"):
    print("\n## Synced write shapes at 16 sessions: B, S4, PostgreSQL `on`\n")
    print("| shape | durability | B cores=1 | B cores=2 | B cores=4 | S4 cores=1 | S4 cores=2 | S4 cores=4 | PG on, CPUs 0 | PG on, CPUs 0,2 | PG on, CPUs 0,2,4,6 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for shape in ("trade", "insert1", "insertN", "recent"):
        for dur in ("group", "strict"):
            row = []
            for cc in (cb, ca):
                for k in (1, 2, 4):
                    v = cc.get((shape, dur, k, 16), [])
                    row.append(f"{med(r['statements_per_s'] for r in v):,.0f} stmt/s" if v else "-")
            for cpus in ("0", "0,2", "0,2,4,6"):
                v = PG.get((shape, "on", cpus, 16))
                row.append(f"{med(v):,.0f} stmt/s" if v else "-")
            print(f"| {shape} | {dur} | " + " | ".join(row) + " |")
    print("\nUnsynced (relaxed) at 16 sessions: B / S4 / PG off\n")
    print("| shape | B k1 | B k2 | B k4 | S4 k1 | S4 k2 | S4 k4 | PG off CPUs 0 | PG off 0,2 | PG off 0,2,4,6 |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for shape in ("point", "trade", "insert1", "insertN", "recent"):
        row = []
        for cc in (cb, ca):
            for k in (1, 2, 4):
                v = cc.get((shape, "relaxed", k, 16), [])
                row.append(f"{med(r['statements_per_s'] for r in v):,.0f} stmt/s" if v else "-")
        for cpus in ("0", "0,2", "0,2,4,6"):
            v = PG.get((shape, "off", cpus, 16))
            row.append(f"{med(v):,.0f} stmt/s" if v else "-")
        print(f"| {shape} | " + " | ".join(row) + " |")

if mode in ("all", "counters"):
    print("\n## Other counters (sum over runs), B and S4\n")
    for name, rs in (("B", B), ("S4", A)):
        tot = defaultdict(float)
        for r in rs:
            for k, v in r["meta_delta"].items():
                if k.startswith("contention_") and not k.endswith(("_waits", "_wait_us")):
                    tot[k] += v
        print(f"- {name}: " + "; ".join(f"`{k}` {v:,.0f}" for k, v in sorted(tot.items())))
        for k in ("wal_syncs_inline", "wal_syncs_writer"):
            pass
        sy = {k: sum(r["meta_delta"].get(k, 0) for r in rs) for k in sorted({k for r in rs for k in r["meta_delta"] if "sync" in k and k.startswith("wal")})}
        print(f"  - wal sync counters: {sy}")

if mode in ("all", "host"):
    print("\n## Host\n")
    loads = [float(r["host_before"]["loadavg"].split()[0]) for r in B]
    print(f"- B runs: {len(B)}; 1-min load before a run: median {med(loads):.2f}, max {max(loads):.2f}; wait_s max {max(r['wait_s'] for r in B):.0f}")
    print(f"- binaries: {sorted({r['bin_sha256'] for r in B})}; devices: {sorted({r['data_device'] for r in B})}")
