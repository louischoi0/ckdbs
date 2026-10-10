"""Per run and per server, the single longest statement over all arms (ms) and the arm it fell in.
usage: stall_any.py <dir> <rows>"""
import collections, glob, json, statistics as st, sys

d, rows = sys.argv[1], sys.argv[2]
fs = [f for f in glob.glob(f"{d}/n{rows}-r*.json") if not f.endswith(".raw.json")]
res = {"A": [], "B": []}
where = {"A": collections.Counter(), "B": collections.Counter()}
for f in fs:
    s = json.load(open(f))
    for srv in "AB":
        arm, v = max(((a, x["max_us"]) for a, x in s["arms"][srv].items() if a not in ("ping", "ping2", "iso")), key=lambda t: t[1])
        res[srv].append(v / 1000)
        if v > 40000:
            where[srv][arm] += 1
for srv in "AB":
    v = sorted(res[srv])
    print(f"{d} {rows} rows server {srv}: {len(v)} runs; longest statement per run (ms) median {st.median(v):.0f}, min {v[0]:.0f}, max {v[-1]:.0f}; "
          f"runs over 40 ms: {sum(x > 40 for x in v)}; arms: {dict(where[srv])}")
