"""Walk (range100) p50 by (binary, slot): median over kept runs of the per-run p50, per data set; written to run/analysis/tbl/walk-slots.md."""
import glob, json, statistics, sys
sys.path.insert(0, "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad")
import analyze_bk as ab

base = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
out = []
for rows in (1000, 10000):
    cells = {}
    for name, d, mode in (("main (A in slot A, B in slot B)", "overhead-c1", "main"), ("null (A in both)", "null-c1", "null"),
                          ("flipped (B in slot A, A in slot B)", "flip-c1", "flip")):
        js = ab.load(base + d, 1, rows, 0, 10 ** 6)
        if not js:
            continue
        ref = statistics.median(j["arms"][L]["ping"]["p50_us"] for j in js for L in "AB")
        js = [j for j in js if not ab.reasons(j, ref, mode)]
        sa = statistics.median(j["arms"]["A"]["range100"]["p50_us"] for j in js)
        sb = statistics.median(j["arms"]["B"]["range100"]["p50_us"] for j in js)
        cells[name] = (sa, sb, len(js))
    out.append(f"\n**{rows:,} rows**, `COUNT/MIN/MAX` over 100 ids, p50 as the median of the per-run p50s (`cores = 1`):\n")
    out.append("| data set | binary in slot A (port 32011) | binary in slot B (port 32012) | slot A p50 | slot B p50 | runs |")
    out.append("|---|---|---|---|---|---|")
    for name, (sa, sb, n) in cells.items():
        ba, bb_ = {"main": ("A", "B"), "null": ("A", "A"), "flipped": ("B", "A")}[name.split()[0]]
        out.append(f"| {name} | {ba} | {bb_} | {sa:.1f} µs | {sb:.1f} µs | {n} |")
open(base + "analysis/tbl/walk-slots.md", "w").write("\n".join(out).strip() + "\n")
print("\n".join(out))
