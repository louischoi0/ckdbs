#!/usr/bin/env python3
"""Render dr_s0_premise.py JSON as the markdown tables of DR-S0's results file.

    dr_s0_report.py heap|prefix|btree RESULT.json     one run's tables
    dr_s0_report.py summary HEAP.json BTREE-100K.json BTREE-1M.json
                                                      the figures the file's first section quotes

Every cell carries its unit. Server CPU is the median of a cell's rounds
with their range; each latency percentile is the median of the rounds'.
The ANALYZE step is the first round's.
"""
import json, sys
from statistics import median

PCTS = ("p0", "p25", "p50", "p95", "p99")


def group(path):
    d = json.load(open(path))
    arms = {}
    for c in d["cells"]:
        arms.setdefault(str(c["arm"]), []).append(c)
    return d, arms


def phase(c, k, j=None):
    return c[k][j] if j is not None else c[k]


def med(cells, k, field="cpu_us", j=None):
    xs = [phase(c, k, j)[field] for c in cells if k in c]
    return median(xs) if xs else None


def cpu(cells, k, j=None):
    xs = [phase(c, k, j)["cpu_us"] for c in cells if k in c]
    return f"{median(xs):,.1f} us ({min(xs):,.1f}..{max(xs):,.1f})" if xs else "-"


def lat(cells, k, j=None):
    ps = [phase(c, k, j)["lat_us"] for c in cells if k in c]
    return " / ".join(f"{median(p[q] for p in ps):,.0f}" for q in PCTS) + " us" if ps else "-"


def p50(cells, k, j=None):
    return median(phase(c, k, j)["lat_us"]["p50"] for c in cells if k in c)


def load(cells, k):
    xs = [c[k]["cpu_us_row"] for c in cells]
    rs = [c[k]["rows_s"] for c in cells]
    return f"{median(xs):,.2f} us/row ({min(xs):,.2f}..{max(xs):,.2f})", f"{median(rs):,.0f} rows/s"


def step(cells, k, j=None):
    v = cells[0].get(k)
    if v is None:
        return "-"
    return "`" + (v[j] if j is not None else v).replace("step 0 ", "") + "`"


def table(header, rows):
    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    for r in rows:
        print("| " + " | ".join(str(x) for x in r) + " |")
    print()


def stamp(d):
    waits = [c.get("settle_s", 0) for c in d["cells"]]
    print(f"Run started {d['started_utc']} UTC, {d['elapsed_s']} s; binary sha256 "
          f"`{d['sha256_16']}`; quiet-gate hold {d['quiet_hold_s']} s; 1-minute load "
          f"{d['host_before']['loadavg'].split()[0]} before and "
          f"{d['host_after']['loadavg'].split()[0]} after; the wait for a quiet server after a "
          f"load {min(waits)} s to {max(waits)} s a cell.\n")


STATEMENT = ["server CPU a statement, median (range)",
             "client latency p0 / p25 / p50 / p95 / p99", "ANALYZE step"]


def heap(path):
    d, arms = group(path)
    stamp(d)
    print("M2, the load:\n")
    table(["form", "server CPU a row, median (range)", "rows a second"],
          [[a, *load(cs, "m2_load")] for a, cs in arms.items()])
    for k, title, sk in (
            ("m1_read", "M1, a point read naming the whole key", "m1_step"),
            ("m3_range", "M3, a range of 100 rows bounded inclusively at both ends", "m3_range_step"),
            ("m3_page", "M3, a page: `key >= x ORDER BY key LIMIT 100`", "m3_page_step"),
            ("m3_bare", "M3, the bare `ORDER BY key LIMIT 100`", "m3_bare_step"),
            ("m4_plain", "M4, an `UPDATE` of a column no index covers, by the whole key", None),
            ("m4_move", "M4, an `UPDATE` that moves the key's last column", None)):
        print(title + ":\n")
        table(["form"] + (STATEMENT if sk else STATEMENT[:2]),
              [[a, cpu(cs, k), lat(cs, k)] + ([step(cs, sk)] if sk else [])
               for a, cs in arms.items() if k in cs[0]])


def prefix(path):
    d, arms = group(path)
    stamp(d)
    table(["run length", "load, server CPU a row", "point read, server CPU a statement",
           "read latency p0 / p25 / p50 / p95 / p99", "ANALYZE step"],
          [[a, load(cs, "m5_load")[0], cpu(cs, "m5_read"), lat(cs, "m5_read"), step(cs, "m5_step")]
           for a, cs in sorted(arms.items(), key=lambda x: int(x[0]))])


def btree(path):
    d, arms = group(path)
    stamp(d)
    print("M6, the load:\n")
    table(["arm", "server CPU a row, median (range)", "rows a second",
           "leaf divisions (relation / index)", "data file", "log written"],
          [[a, *load(cs, "m6_load"),
            f"{cs[0]['m6_leaf_divisions'][0]:,} / "
            + (f"{cs[0]['m6_index_divisions'][0]:,}" if "m6_index_divisions" in cs[0] else "-"),
            f"{median(c['m6_data_bytes'] for c in cs) / 1e6:,.1f} MB",
            f"{median(c['m6_wal_durable_lsn'] for c in cs) / 1e6:,.1f} MB"]
           for a, cs in arms.items()])
    print("M7 to M9:\n")
    rows = []
    for a, cs in arms.items():
        rows.append([a, "M7 present", cpu(cs, "m7_present"), lat(cs, "m7_present"),
                     step(cs, "m7_step")])
        names = ["M8 absent"] if len(cs[0]["m8_absent"]) == 1 else [
            "M8 absent, the home", "M8 absent, the index"]
        for j, name in enumerate(names):
            rows.append([a, name, cpu(cs, "m8_absent", j), lat(cs, "m8_absent", j),
                         step(cs, "m8_step", j)])
        for k, t in (("m9_range", "M9 range"), ("m9_page", "M9 page"), ("m9_bare", "M9 bare")):
            rows.append([a, t, cpu(cs, k), lat(cs, k), step(cs, k + "_step")])
    table(["arm", "shape"] + STATEMENT, rows)


def summary(heap_path, b100_path, b1m_path):
    """The figures the results file's first section quotes, from the medians."""
    _, h = group(heap_path)
    pk = [a for a in h if a not in ("ref_btree", "ref_heap", "nopk_heap")]

    def span(k, field="cpu_us"):
        xs = [med(h[a], k, field) for a in pk if k in h[a][0]]
        return min(xs), max(xs)

    ref = med(h["ref_heap"], "m1_read")
    lo, hi = span("m1_read")
    print(f"M1 ref_btree {med(h['ref_btree'], 'm1_read'):.1f} ref_heap {ref:.1f} us; pk "
          f"{lo:.1f}..{hi:.1f} us; +{lo - ref:.1f}..+{hi - ref:.1f} us "
          f"({100 * (lo / ref - 1):.0f} %..{100 * (hi / ref - 1):.0f} %)")
    wr = med(h["ref_heap"], "m4_plain")
    lo, hi = span("m4_plain")
    print(f"M4 ref_heap {wr:.1f} us; pk {lo:,.0f}..{hi:,.0f} us; {lo / wr:.0f}x..{hi / wr:.0f}x")
    lo, hi = span("m4_move")
    print(f"M4 move {lo:,.0f}..{hi:,.0f} us")
    no = med(h["nopk_heap"], "m2_load", "cpu_us_row")
    lo, hi = span("m2_load", "cpu_us_row")
    print(f"M2 ref_heap {med(h['ref_heap'], 'm2_load', 'cpu_us_row'):.2f} nopk {no:.2f} us/row; "
          f"pk {lo:.2f}..{hi:.2f}; +{lo - no:.1f}..+{hi - no:.1f} us "
          f"(+{100 * (lo / no - 1):.0f} %..+{100 * (hi / no - 1):.0f} %)")
    lo, hi = span("m3_range")
    rh, rb = med(h["ref_heap"], "m3_range"), med(h["ref_btree"], "m3_range")
    print(f"M3 range ref_heap {rh:.1f} ref_btree {rb:.1f} pk {lo:.1f}..{hi:.1f} us "
          f"({lo / rh:.1f}x..{hi / rh:.1f}x heap, {lo / rb:.1f}x..{hi / rb:.1f}x btree)")
    for k in ("m3_page", "m3_bare"):
        print(k, {a: round(med(h[a], k)) for a in h if k in h[a][0]})
    for name, path in (("100k", b100_path), ("1M", b1m_path)):
        _, b = group(path)
        f2, p1, f0 = b["F2e"], b["P1e"], b["F0"]
        r2, r1 = med(f2, "m7_present"), med(p1, "m7_present")
        q2, q1 = p50(f2, "m7_present"), p50(p1, "m7_present")
        l2, l1 = med(f2, "m6_load", "cpu_us_row"), med(p1, "m6_load", "cpu_us_row")
        print(f"{name}: M7 cpu F0 {med(f0, 'm7_present'):.1f} F2e {r2:.1f} P1e {r1:.1f} us "
              f"(F2e ahead {r1 - r2:+.1f} us); p50 F0 {p50(f0, 'm7_present'):.1f} F2e {q2:.1f} "
              f"P1e {q1:.1f} us (F2e ahead {q1 - q2:+.1f} us, {100 * (q1 - q2) / r1:.1f} % of "
              f"P1e's CPU)")
        print(f"   M6 F0 {med(f0, 'm6_load', 'cpu_us_row'):.2f} F2e {l2:.2f} P1e {l1:.2f} us/row "
              f"(F2e +{l2 - l1:.2f} us, +{100 * (l2 / l1 - 1):.0f} %; break-even "
              f"{(l2 - l1) / (q1 - q2):.1f} keyed reads an insert by p50)")
        print(f"   M8 cpu F0 {med(f0, 'm8_absent', j=0):.1f} F2e {med(f2, 'm8_absent', j=0):.1f}+"
              f"{med(f2, 'm8_absent', j=1):.1f} P1e {med(p1, 'm8_absent', j=0):.1f} us; p50 F2e "
              f"{p50(f2, 'm8_absent', 0):.1f}+{p50(f2, 'm8_absent', 1):.1f} P1e "
              f"{p50(p1, 'm8_absent', 0):.1f} us")
        print(f"   M9 range F0 {med(f0, 'm9_range'):.1f} F2e {med(f2, 'm9_range'):.1f} P1e "
              f"{med(p1, 'm9_range'):.1f} us; page F2e/P1e "
              f"{med(f2, 'm9_page') / med(p1, 'm9_page'):.2f}x bare "
              f"{med(f2, 'm9_bare') / med(p1, 'm9_bare'):.2f}x")
        d2, d1 = f2[0]["m6_data_bytes"], p1[0]["m6_data_bytes"]
        w2, w1 = f2[0]["m6_wal_durable_lsn"], p1[0]["m6_wal_durable_lsn"]
        print(f"   divisions F2e {f2[0]['m6_leaf_divisions'][0]:,}; data F2e/P1e "
              f"{d2 / 1e6:.1f}/{d1 / 1e6:.1f} MB (+{100 * (d2 / d1 - 1):.0f} %); log "
              f"{w2 / 1e6:.1f}/{w1 / 1e6:.1f} MB (+{100 * (w2 / w1 - 1):.1f} %)")


if __name__ == "__main__":
    if sys.argv[1] == "summary":
        summary(*sys.argv[2:5])
    else:
        {"heap": heap, "prefix": prefix, "btree": btree}[sys.argv[1]](sys.argv[2])
