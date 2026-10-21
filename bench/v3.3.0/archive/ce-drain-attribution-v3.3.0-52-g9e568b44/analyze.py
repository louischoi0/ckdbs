#!/usr/bin/env python3
"""Drain attribution: A/B/C tables from json/drain-r<round>-<bin>.jsonl. Prints markdown."""
import glob, json, os, re, statistics as st, sys
RUN = "/home/cdkbs/bench-runs/ce-drain-attribution-v3.3.0-52-g9e568b44"
SH = ["range-1", "range-100", "range-1000", "range-10000"]
cells = {}   # (shape, bin) -> {round: cell}
for f in sorted(glob.glob(f"{RUN}/json/drain-r*-*.jsonl")):
    m = re.search(r"drain-r(\d+)-([ABC])\.jsonl$", f)
    r, b = int(m.group(1)), m.group(2)
    for l in open(f):
        d = json.loads(l)
        cells.setdefault((d["shape"], b), {})[r] = d


def pct(v, p):
    v = sorted(v)
    if len(v) == 1:
        return v[0]
    k = (len(v) - 1) * p / 100
    lo = int(k); hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def med(v): return st.median(v)
def spread(v): return (max(v) - min(v)) / med(v) * 100


def col(shape, b, key):
    return [cells[(shape, b)][r][key] for r in sorted(cells.get((shape, b), {}))]


print("### Per-run values (wall us/stmt | server CPU us/stmt), by round")
for s in SH:
    for key, nm in (("wall_us_per_stmt", "wall"), ("server_cpu_us_per_stmt", "server CPU")):
        print(f"\n{s} {nm}")
        for b in "ABC":
            rs = sorted(cells.get((s, b), {}))
            print(f"  {b}: " + "  ".join(f"r{r}={cells[(s,b)][r][key]:.1f}" for r in rs))

print("\n### Main table")
print("| shape | rows | n runs A/B/C | A wall | B wall | C wall | B-A | C-A | C-B | spread A/B/C | A srv CPU | B srv CPU | C srv CPU | C-A CPU | B-A CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for s in SH:
    w = {b: col(s, b, "wall_us_per_stmt") for b in "ABC"}
    c = {b: col(s, b, "server_cpu_us_per_stmt") for b in "ABC"}
    if not all(w.values()):
        continue
    mw = {b: med(w[b]) for b in "ABC"}; mc = {b: med(c[b]) for b in "ABC"}
    rows = cells[(s, "A")][min(cells[(s, "A")])]["rows"]
    def dl(x, y, m): return f"{m[x]-m[y]:+.1f} us ({(m[x]/m[y]-1)*100:+.1f} %)"
    print(f"| {s} | {rows} | {len(w['A'])}/{len(w['B'])}/{len(w['C'])} | {mw['A']:.1f} | {mw['B']:.1f} | {mw['C']:.1f} | {dl('B','A',mw)} | {dl('C','A',mw)} | {dl('C','B',mw)} | "
          f"{spread(w['A']):.1f}/{spread(w['B']):.1f}/{spread(w['C']):.1f} % | {mc['A']:.1f} | {mc['B']:.1f} | {mc['C']:.1f} | {dl('C','A',mc)} | {dl('B','A',mc)} |")

print("\n### Per-round paired differences (wall us/stmt; same round, three servers in sequence)")
for s in SH:
    rs = sorted(set(cells.get((s, "A"), {})) & set(cells.get((s, "B"), {})) & set(cells.get((s, "C"), {})))
    for lab, x, y in (("B-A", "B", "A"), ("C-A", "C", "A"), ("C-B", "C", "B")):
        dd = [cells[(s, x)][r]["wall_us_per_stmt"] - cells[(s, y)][r]["wall_us_per_stmt"] for r in rs]
        sg = sum(1 for d in dd if d > 0)
        print(f"{s} {lab}: " + " ".join(f"{d:+.1f}" for d in dd) + f"  (positive in {sg}/{len(dd)}; median {med(dd):+.1f})")

print("\n### Pooled latency per statement (us)")
print("| shape | binary | statements | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|")
for s in SH:
    for b in "ABC":
        v = [x for r in cells.get((s, b), {}) for x in cells[(s, b)][r]["lat_us"]]
        if v:
            print(f"| {s} | {b} | {len(v):,} | {pct(v,0):.1f} | {pct(v,25):.1f} | {pct(v,50):.1f} | {pct(v,95):.1f} | {pct(v,99):.1f} |")
