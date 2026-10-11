"""Sensitivity to the other session's benchmark on CPUs 6,7: the kept runs split by busy67 > 10 %."""
import glob, json, statistics, sys, random
sys.path.insert(0, "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad")
import analyze_bk as ab

base = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
rng = random.Random(1)
SH = ("insert", "insert-named", "select-pk", "update-pk", "update-hot", "delete-pk", "range100", "ping")
for d, cores in (("overhead-c1", 1), ("overhead-c2", 2)):
    for rows in (200, 1000, 10000):
        js = ab.load(base + d, cores, rows, 0, 10 ** 6)
        ref = statistics.median(j["arms"][L]["ping"]["p50_us"] for j in js for L in "AB")
        js = [j for j in js if not ab.reasons(j, ref, "main")]
        q = [j for j in js if ab.busy67(j) <= 0.10]
        f = [j for j in js if ab.busy67(j) > 0.10]
        line = f"{d} n{rows}: quiet67 {len(q)} runs / foreign67 {len(f)} runs |"
        for s in SH:
            def med(x):
                return statistics.median(j["arms"]["B"][s]["p50_us"] - j["arms"]["A"][s]["p50_us"] for j in x) if x else float("nan")
            line += f" {s} {med(q):+.2f}/{med(f):+.2f}"
        pq = statistics.median(j["arms"]["A"]["ping"]["p50_us"] for j in q) if q else float("nan")
        pf = statistics.median(j["arms"]["A"]["ping"]["p50_us"] for j in f) if f else float("nan")
        line += f" | A ping p50 {pq:.1f}/{pf:.1f}"
        print(line)
