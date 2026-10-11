#!/usr/bin/env python3
"""Per-cell summary over several result dirs; paired-by-block statistics too.
usage: report.py DIR [DIR ...]   (each DIR holds n<rows>-run<r>.json from nh_run.py)
"""
import glob, json, math, re, statistics, sys


def q(vals, p):
    s = sorted(vals)
    k = (len(s) - 1) * p / 100
    f = math.floor(k); c = math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)


def nr(s, p):
    s = sorted(s)
    return s[max(1, -(-p * len(s) // 100)) - 1]


for d in sys.argv[1:]:
    cells = {}
    for f in sorted(glob.glob(f"{d}/n*-run*.json")):
        m = re.match(r".*/n(\d+)-run(\d+)\.json", f)
        n, r = int(m.group(1)), int(m.group(2))
        j = json.load(open(f))
        for arm in j["blocks"]["A"]:
            A = j["blocks"]["A"][arm]; B = j["blocks"]["B"][arm]
            la = [x for b in A for x in b["lat_us"]]; lb = [x for b in B for x in b["lat_us"]]
            ea = sum(b["engine_us"] for b in A) / len(la); eb = sum(b["engine_us"] for b in B) / len(lb)
            blk = [statistics.median(b2["lat_us"]) - statistics.median(b1["lat_us"]) for b1, b2 in zip(A, B)]
            c = cells.setdefault((n, arm), {"dp50": [], "deng": [], "a50": [], "b50": [], "blk": [], "pa": [], "pb": [], "ea": []})
            c["a50"].append(statistics.median(la)); c["b50"].append(statistics.median(lb))
            c["dp50"].append(c["b50"][-1] - c["a50"][-1]); c["deng"].append(eb - ea); c["ea"].append(ea)
            c["blk"].extend(blk); c["pa"].extend(la); c["pb"].extend(lb)
    print("==", d)
    for (n, arm), c in sorted(cells.items()):
        dp = c["dp50"]; bl = c["blk"]
        print(f"{n:>6} {arm:<18} runs={len(dp)} A_p50={statistics.median(c['a50']):9.1f} B_p50={statistics.median(c['b50']):9.1f} "
              f"d50={statistics.median(dp):+8.2f} IQR[{q(dp,25):+8.2f},{q(dp,75):+8.2f}] B>A {sum(x>0 for x in dp)}/{len(dp)} "
              f"ns/row={statistics.median(dp)*1000/n:+6.1f} [{q(dp,25)*1000/n:+6.1f},{q(dp,75)*1000/n:+6.1f}] "
              f"dEng={statistics.median(c['deng']):+7.2f} [{q(c['deng'],25):+7.2f},{q(c['deng'],75):+7.2f}] "
              f"| blocks {len(bl)} medΔ={statistics.median(bl):+8.2f} [{q(bl,25):+8.2f},{q(bl,75):+8.2f}] B>A {sum(x>0 for x in bl)}/{len(bl)} "
              f"| pooled A p0/25/50/95/99={min(c['pa']):.1f}/{nr(c['pa'],25):.1f}/{nr(c['pa'],50):.1f}/{nr(c['pa'],95):.1f}/{nr(c['pa'],99):.1f} "
              f"B={min(c['pb']):.1f}/{nr(c['pb'],25):.1f}/{nr(c['pb'],50):.1f}/{nr(c['pb'],95):.1f}/{nr(c['pb'],99):.1f} nA={len(c['pa'])}")
