#!/usr/bin/env python3
"""bw_an1.py MAINDIR SWAPDIR [EXCLUDE.json] -> analysis of cell 1.

Per (rows, arm): per-run percentiles from the raw samples; the A and B columns are medians over runs of the
per-run percentile; the delta is B - A within a pair, summarised as median [Q1, Q3] over pairs.
Swap pass: slot A ran binary B. x = (d_main - d_swap) / 2 is the binary effect, s = (d_main + d_swap) / 2 the slot effect.
Excluded runs (a JSON list of tags, as 'main:n1000-run3') are left out of both the pairs and the medians.
"""
import glob, gzip, json, math, re, statistics, sys

MAIN, SWAP = sys.argv[1], sys.argv[2]
EXC = set(json.load(open(sys.argv[3]))) if len(sys.argv) > 3 else set()


def nr(s, p):
    return s[max(1, -(-int(p) * len(s) // 100)) - 1]


def pct(v):
    s = sorted(v)
    return {"p0": s[0], "p25": nr(s, 25), "p50": nr(s, 50), "p75": nr(s, 75), "p99": nr(s, 99), "max": s[-1],
            "mean": statistics.fmean(s)}


def q(vals, p):
    s = sorted(vals)
    k = (len(s) - 1) * p / 100
    f, c = math.floor(k), math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


def load(dirname, key):
    data = {}
    eng = {}
    for f in sorted(glob.glob(f"{dirname}/n*-run*.lat.json.gz")):
        m = re.match(r".*/n(\d+)-run(\d+)\.lat", f)
        n, r = int(m.group(1)), int(m.group(2))
        if f"{key}:n{n}-run{r}" in EXC:
            continue
        j = json.load(open(f[:-12] + ".json"))
        assert j["verify_ok"], f
        raw = json.load(gzip.open(f, "rt"))
        for lab, arms in raw.items():
            for arm, lat in arms.items():
                e = pct(lat)
                en = j["engine"][lab][arm]
                e["eng_us"] = en["us"] / en["polls"]
                data.setdefault((n, arm, lab), {})[r] = e
    return data


main, swap = load(MAIN, "main"), load(SWAP, "swap")
STATS = ("p0", "p25", "p50", "p75", "p99", "mean", "eng_us")


def deltas(data, n, arm, key):
    A, B = data.get((n, arm, "A"), {}), data.get((n, arm, "B"), {})
    runs = sorted(set(A) & set(B))
    return [B[r][key] - A[r][key] for r in runs], runs


out = {}
for (n, arm) in sorted({(n, a) for (n, a, _) in main}):
    cell = {}
    for key in STATS:
        dm, rm = deltas(main, n, arm, key)
        ds, rs = deltas(swap, n, arm, key)
        c = {"pairs_main": len(dm), "pairs_swap": len(ds)}
        Am = [main[(n, arm, "A")][r][key] for r in rm]
        Bm = [main[(n, arm, "B")][r][key] for r in rm]
        c["A"], c["B"] = statistics.median(Am), statistics.median(Bm)
        c["Aq"], c["Bq"] = [q(Am, 25), q(Am, 75)], [q(Bm, 25), q(Bm, 75)]
        c["d_main"] = {"med": statistics.median(dm), "q1": q(dm, 25), "q3": q(dm, 75),
                       "B_gt_A": sum(1 for x in dm if x > 0)}
        if ds:
            c["d_swap"] = {"med": statistics.median(ds), "q1": q(ds, 25), "q3": q(ds, 75),
                           "B_gt_A": sum(1 for x in ds if x > 0)}
            c["x"] = (c["d_main"]["med"] - c["d_swap"]["med"]) / 2
            c["s"] = (c["d_main"]["med"] + c["d_swap"]["med"]) / 2
        cell[key] = c
    out[f"{n}|{arm}"] = cell
json.dump(out, open(f"{MAIN}/../c1-analysis.json", "w"), indent=1)
for k, cell in out.items():
    c = cell["p50"]
    print(f"{k:<22} pairs {c['pairs_main']}/{c['pairs_swap']}  p50 A={c['A']:.1f} B={c['B']:.1f}  "
          f"d_main={c['d_main']['med']:+.2f} [{c['d_main']['q1']:+.2f},{c['d_main']['q3']:+.2f}] B>A {c['d_main']['B_gt_A']}  "
          + (f"d_swap={c['d_swap']['med']:+.2f} x={c['x']:+.2f} s={c['s']:+.2f}" if "x" in c else "")
          + f"  | p99 d_main={cell['p99']['d_main']['med']:+.2f}  mean d={cell['mean']['d_main']['med']:+.2f}  eng d={cell['eng_us']['d_main']['med']:+.2f}")
