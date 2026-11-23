#!/usr/bin/env python3
"""CW-S0's results tables, generated from the archive's runs/ (one JSON a run,
named by its cell's label):  python3 tables_s0.py [<archive>/runs]"""
import glob, json, os, statistics, sys

W = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs")
TSC_GHZ, FLOOR = 2.1, 55.0


def runs(prefix):
    return [json.load(open(f)) for f in sorted(glob.glob(os.path.join(W, prefix + ".json")))]


def per(r, k):
    return float(r["meta_delta"].get(k, 0) or 0) / r["statements"]


def cpu(r):
    return r["server_cpu_ns"] / r["statements"] / 1000.0


def ticks(r, kind):
    m = r["meta_delta"]
    calls = float(m.get(f"contention_cw_t_{kind}_calls", 0) or 0)
    t = float(m.get(f"contention_cw_t_{kind}_ticks", 0) or 0)
    if not calls:
        return 0.0, 0.0
    each = (t / calls - FLOOR) / TSC_GHZ
    return each, each * calls / r["statements"]


def fmt(xs, f="{:,.0f}", unit=""):
    if len(xs) == 1:
        return f.format(xs[0]) + unit
    return f.format(statistics.mean(xs)) + unit + " (" + " / ".join(f.format(x) for x in xs) + ")"


def loads(rs):
    ls = [float(r["host_before"]["loadavg"].split()[0]) for r in rs]
    return f"{min(ls):.2f}-{max(ls):.2f}" if len(ls) > 1 else f"{ls[0]:.2f}"


def m2():
    print("| shape | cell | runs | stmt/s | server CPU a statement | fetches a statement | frame-table waits a statement | frame-table wait a statement | page-latch waits a statement | page-latch spin turns a statement | p50 | p99 | load before |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    cells = [("c1", "M2-{s}-c1-r*"), ("c2", "M2-{s}-c2-r*"), ("c3i", "M2-{s}-c3-r*")]
    for s in ("point", "pkmix", "insert1", "pointN"):
        for lab, pat in cells + [("c3c", "C3c-{s}-r*")]:
            if s == "pointN":
                pat = {"c2": "M4-pointN-c2-r*", "c3i": "M4-pointN-c3-r*", "c3c": "C3c-pointN-r*"}.get(lab)
                if pat is None:
                    continue
            elif lab == "c3c" and s != "point":
                continue
            rs = runs(pat.format(s=s))
            if not rs:
                continue
            print(f"| `{s}` | {lab} | {len(rs)} | {fmt([r['statements_per_s'] for r in rs])} stmt/s"
                  f" | {fmt([cpu(r) for r in rs], '{:.2f}', ' us')}"
                  f" | {statistics.mean(per(r, 'pool_hits') + per(r, 'pool_misses') for r in rs):.2f}"
                  f" | {fmt([per(r, 'contention_frame_table_waits') for r in rs], '{:.3f}')}"
                  f" | {fmt([per(r, 'contention_frame_table_wait_us') * 1000 for r in rs], '{:.0f}', ' ns')}"
                  f" | {fmt([per(r, 'contention_page_latch_waits') for r in rs], '{:.3f}')}"
                  f" | {fmt([per(r, 'contention_page_latch_spin_turns') for r in rs], '{:.2f}')}"
                  f" | {fmt([r['p50_us'] for r in rs], '{:.1f}', ' us')}"
                  f" | {fmt([r['p99_us'] for r in rs], '{:.1f}', ' us')}"
                  f" | {loads(rs)} |")


def m1():
    print("| shape | cell | read fetches | write fetches | frame-table holds | page-latch CAS attempts | shared grants | exclusive grants | load before |")
    print("|---|---|---|---|---|---|---|---|---|")
    for lab, pat in (("point c1 armed", "M1-point-c1a"), ("point c2", "M1-point-c2"),
                     ("pkmix c2", "M1-pkmix-c2"), ("insert1 c2", "M1-insert1-c2")):
        r = runs(pat)[0]
        g = lambda k: per(r, "contention_cw_n_" + k)
        print(f"| `{lab.split()[0]}` | {' '.join(lab.split()[1:])} | {g('read_fetch'):.3f} | {g('write_fetch'):.3f}"
              f" | {g('ft_holds'):.2f} | {g('pl_cas'):.3f} | {g('pl_shared'):.3f} | {g('pl_excl'):.3f} | {loads([r])} |")


def m3():
    print("| shape | cell | read fetch, each | write fetch, each | release, each | fetch protocol a statement | its share of server CPU | server CPU a statement (this run) | load before |")
    print("|---|---|---|---|---|---|---|---|---|")
    rows = [("point", "c1 unarmed", "M3-point-c1u"), ("point", "c1 armed", "M3-point-c1a"),
            ("point", "c2", "M3-point-c2"), ("point", "c3c", "C3c-M3-point"), ("point", "c3i", "M3-point-c3"),
            ("pointN", "c2", "M3-pointN-c2"), ("pointN", "c3c", "C3c-M3-pointN"), ("pointN", "c3i", "M3-pointN-c3"),
            ("insert1", "c1 unarmed", "M3-insert1-c1u"), ("insert1", "c1 armed", "M3-insert1-c1a"),
            ("insert1", "c2", "M3-insert1-c2"), ("insert1", "c3c", "C3c-M3-insert1"),
            ("insert1", "c3i", "M3-insert1-c3")]
    for s, lab, pat in rows:
        r = runs(pat)[0]
        rf, rfs = ticks(r, "read_fetch")
        wf, wfs = ticks(r, "write_fetch")
        rl, rls = ticks(r, "release")
        tot = rfs + wfs + rls
        print(f"| `{s}` | {lab} | {rf:.0f} ns | {wf:.0f} ns" if wfs > 0.01 else f"| `{s}` | {lab} | {rf:.0f} ns | -", end="")
        print(f" | {rl:.0f} ns | {tot:.0f} ns | {100 * tot / 1000 / cpu(r):.1f} % | {cpu(r):.2f} us | {loads([r])} |")


def m5():
    print("| shape | arm | runs | stmt/s | server CPU a statement | load before |")
    print("|---|---|---|---|---|---|")
    for s in ("point", "insert1"):
        for arm, lab in (("c1u", "one core, unarmed"), ("c1a", "one core, armed")):
            rs = runs(f"M5-{s}-{arm}-r*")
            print(f"| `{s}` | {lab} | {len(rs)} | {fmt([r['statements_per_s'] for r in rs])} stmt/s"
                  f" | {fmt([cpu(r) for r in rs], '{:.2f}', ' us')} | {loads(rs)} |")


def devices():
    every = runs("*")
    ds = {r["data_device"] for r in every}
    bins = {}
    for r in every:
        bins[r["bin_sha256"]] = bins.get(r["bin_sha256"], 0) + 1
    cb = sum(1 for r in every if r["client_bound"])
    err = sum(r["errors"] + r["refusals"] for r in every)
    print("devices", ds, "binaries", bins, "client_bound runs", cb, "errors+refusals", err)


if __name__ == "__main__":
    for f in (m2, m1, m3, m5, devices):
        print(f"\n### {f.__name__}\n")
        f()
