"""Cell 3: this run (B = 9f170b1) against the 566 file's run (B = 273416c); A = a59da9c in both.
Reads the 566 archive's gzipped c3 JSON and this run's c3 JSON."""
import gzip, json, statistics as st
OLD = "/home/cdkbs/ckdbs/.claude/worktrees/az-s7-close/bench/v3.0.0/archive/az-s7-overhead-v2.7.0-566-g273416c"
NEW = "/home/cdkbs/bench-runs/az-s7b"
old = [json.load(gzip.open(f"{OLD}/c3-run{r}/result.json.gz")) for r in (1, 2, 3)]
new = [json.load(open(f"{NEW}/c3-run{r}/result.json")) for r in (1, 2, 3, 4, 5)]


def q(v, p):
    v = sorted(v)
    return v[int(p * (len(v) - 1))]


def per_row(runs, l, kind, k):
    return [t / k for d in runs for t in d["cells"][l][f"{kind}-K{k}"]["txn_us"]]


def paired(runs, kind, k):
    return [b - a for b, a in zip(per_row(runs, "B", kind, k), per_row(runs, "A", kind, k))]


print("K | fk B old/new | fk A old/new | plain B old/new | plain A old/new | paired fk old/new | paired plain old/new | fk-plain")
for k in (1, 64, 1024, 4096, 16384):
    def m(runs, l, kind):
        return st.median(per_row(runs, l, kind, k))
    po, pn = paired(old, "fk", k), paired(new, "fk", k)
    plo, pln = paired(old, "plain", k), paired(new, "plain", k)
    print(f"{k} | {m(old,'B','fk'):.1f}/{m(new,'B','fk'):.1f} | {m(old,'A','fk'):.1f}/{m(new,'A','fk'):.1f} | {m(old,'B','plain'):.1f}/{m(new,'B','plain'):.1f} | {m(old,'A','plain'):.1f}/{m(new,'A','plain'):.1f} | {st.median(po):+.2f}/{st.median(pn):+.2f} | {st.median(plo):+.2f}/{st.median(pln):+.2f} | "
          f"B {m(old,'B','fk')-m(old,'B','plain'):+.1f}/{m(new,'B','fk')-m(new,'B','plain'):+.1f} A {m(old,'A','fk')-m(old,'A','plain'):+.1f}/{m(new,'A','fk')-m(new,'A','plain'):+.1f}")
print("octile B-A per fk transaction position")
for k in (4096, 16384):
    for nm, runs in (("old", old), ("new", new)):
        acc = {l: [[0, 0] for _ in range(8)] for l in "BA"}
        for d in runs:
            for l in "BA":
                for i, (s, n) in enumerate(d["cells"][l][f"fk-K{k}"]["stmt_us_by_pos"]):
                    acc[l][i][0] += s
                    acc[l][i][1] += n
        print(k, nm, " ".join(f"{acc['B'][i][0]/acc['B'][i][1]-acc['A'][i][0]/acc['A'][i][1]:+.1f}" for i in range(8)))
for k in (4096, 16384):
    for nm, runs in (("old", old), ("new", new)):
        print(k, nm, "rows/s B fk", round(1e6 / st.median(per_row(runs, "B", "fk", k))), "A fk", round(1e6 / st.median(per_row(runs, "A", "fk", k))))
