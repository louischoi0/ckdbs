"""Across every run summary: the maximum of pool_resident and the sums of the pool's refusal and reclaim counters."""
import glob, json

R = "/home/cdkbs/bench-runs/bi-close-overhead-v2.7.0-784-gec827a19/out"
mx = {"pool_resident": 0}
tot = {k: 0 for k in ("pool_refused", "pool_reclaimed_inline", "pool_reclaimed_background", "pool_batches_inline", "pool_batches_background", "pool_dirty_queued")}
n = 0
bad = 0
for f in glob.glob(f"{R}/*/n*-r*.json"):
    s = json.load(open(f))
    n += 1
    for srv in "AB":
        pe = s["pool_end"][srv]
        mx["pool_resident"] = max(mx["pool_resident"], pe["pool_resident"])
        for k in tot:
            tot[k] += pe.get(k, 0)
    bad += sum(x["errors"] for srv in "AB" for x in s["arms"][srv].values())
print(n, "run files; max pool_resident", mx["pool_resident"], "of", 65536, "=", round(100 * mx["pool_resident"] / 65536, 2), "%; sums", tot, "; statement errors", bad)
