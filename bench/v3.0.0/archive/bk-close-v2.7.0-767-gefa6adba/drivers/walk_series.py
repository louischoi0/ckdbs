"""The walk (range100) across every data set that measured it, in the order it ran: slot p50s, the difference and its CI,
read from the analyze_bk.py JSON outputs so every number agrees with the delta tables. Writes run/analysis/tbl/walk-series.md."""
import glob, json, os, statistics, sys, time
sys.path.insert(0, "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad")
import analyze_bk as ab

base = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
A = base + "analysis/"
# (label, json, dir, cores, mode, run range, slot A binary, slot B binary, sign)
SETS = [
    ("main, pass 1", "c1p1.json", "overhead-c1", 1, "main", (0, 16), "A", "B", 1),
    ("main, pass 2", "c1p2.json", "overhead-c1", 1, "main", (16, 32), "A", "B", 1),
    ("null pair", "null.json", "null-c1", 1, "null", (0, 16), "A", "A (copy 2)", 1),
    ("main, `cores = 2`", "c2.json", "overhead-c2", 2, "main", (0, 16), "A", "B", 1),
    ("null pair, `cores = 2`", "null2.json", "null-c2", 2, "null", (0, 16), "A", "A (copy 2)", 1),
    ("strict cell's walk arm (600 statements)", "strict.json", "strict-c1", 1, "main", (0, 16), "A", "B", 1),
    ("slots exchanged", "flip.json", "flip-c1", 1, "flip", (0, 16), "B", "A", -1),
    ("copy control: B bytes in both slots", "nullB.json", "nullB-c1", 1, "nullnew", (0, 16), "B", "B (copy 2)", 1),
    ("copy control: A bytes in both slots", "nullA.json", "nullA-c1", 1, "null", (0, 16), "A", "A (copy 3)", 1),
    ("fresh copies: A (copy 3) against B (copy 2)", "fresh.json", "fresh-c1", 1, "main", (0, 16), "A (copy 3)", "B (copy 2)", 1),
]
out = ["| data set | window (UTC) | cores | rows | binary in slot A → slot B | slot A p50 | slot B p50 | slot B − slot A (95 % CI) | % of slot A | runs |",
       "|---|---|---|---|---|---|---|---|---|---|"]
for label, js_name, d, cores, mode, (lo, hi), ba, bb_, sign in SETS:
    p = A + js_name
    if not os.path.exists(p):
        continue
    js = json.load(open(p))
    for rows in (200, 1000, 10000):
        c = js.get(f"{rows}|range100")
        meta = js.get(f"{rows}|_meta")
        if not c or not meta:
            continue
        recs = ab.load(base + d, cores, rows, lo, hi)
        t0 = min(j["t_start"] for j in recs)
        t1 = max(j["t_end"] for j in recs)
        win = time.strftime("%H:%M", time.gmtime(t0)) + "-" + time.strftime("%H:%M", time.gmtime(t1))
        if sign == 1:
            sa, sb, dm, c0, c1 = c["pa"], c["pb"], c["dmed"], c["ci"][0], c["ci"][1]
        else:  # the analysis swapped the slots: its A (old) sat in slot B, its B (new) in slot A; raw slot B - slot A = -(new - old)
            sa, sb, dm, c0, c1 = c["pb"], c["pa"], -c["dmed"], -c["ci"][1], -c["ci"][0]
        out.append(f"| {label} | {win} | {cores} | {rows:,} | {ba} → {bb_} | {sa:.1f} µs | {sb:.1f} µs | {dm:+.2f} µs [{c0:+.2f}, {c1:+.2f}] | "
                   f"{dm / sa * 100:+.1f} % | {meta['n_kept']} |")
open(A + "tbl/walk-series.md", "w").write("\n".join(out) + "\n")
print("\n".join(out))
