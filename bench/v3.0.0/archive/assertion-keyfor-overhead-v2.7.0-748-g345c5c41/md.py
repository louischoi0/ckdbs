import glob, json, statistics as st
OUT = "/home/cdkbs/bench-runs/keyfor"
ARMS = [("insert-assert-existing", "INSERT, asserted, existing group"),
        ("insert-assert-newgroup", "INSERT, asserted, new group"),
        ("update-assert", "UPDATE g, asserted"),
        ("delete-assert", "DELETE, asserted"),
        ("insert-none-existing", "ctl: INSERT, no assertion"),
        ("insert-none-again", "ctl: INSERT repeated (noise floor)"),
        ("update-none", "ctl: UPDATE g, no assertion"),
        ("update-none-again", "ctl: UPDATE repeated (noise floor)"),
        ("delete-none", "ctl: DELETE, no assertion"),
        ("select", "ctl: pk SELECT"),
        ("ping", "ctl: SHOW META")]


def q(v, p):
    v = sorted(v)
    i = (len(v) - 1) * p
    lo = int(i)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


for g in ("int", "date"):
    print(f"\n#### group column `{g}`\n")
    print("| rows | statement | A p50 | B p50 | B−A p50 (median of 7 runs) | IQR of B−A | runs B>A | A QPS | B QPS | engine B−A per stmt |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for n in (200, 1000, 10000):
        runs = [json.load(open(f)) for f in sorted(glob.glob(f"{OUT}/{g}-n{n}-run*/result.json"))]
        for a, desc in ARMS:
            Ap = st.median(r["arms"]["A"][a]["p50_us"] for r in runs)
            Bp = st.median(r["arms"]["B"][a]["p50_us"] for r in runs)
            d = [r["arms"]["B"][a]["p50_us"] - r["arms"]["A"][a]["p50_us"] for r in runs]
            Am = st.median(r["arms"]["A"][a]["mean_us"] for r in runs)
            Bm = st.median(r["arms"]["B"][a]["mean_us"] for r in runs)
            ops = runs[0]["ops_per_arm"]
            eng = [r["engine"]["B"][a]["us"] / ops - r["engine"]["A"][a]["us"] / ops for r in runs]
            print(f"| {n:,} | {desc} | {Ap:.1f} µs | {Bp:.1f} µs | {st.median(d):+.2f} µs | {q(d,.25):+.2f} to {q(d,.75):+.2f} µs | "
                  f"{sum(1 for x in d if x>0)}/{len(d)} | {1e6/Ap:,.0f} qps | {1e6/Bp:,.0f} qps | {st.median(eng):+.2f} µs |")
