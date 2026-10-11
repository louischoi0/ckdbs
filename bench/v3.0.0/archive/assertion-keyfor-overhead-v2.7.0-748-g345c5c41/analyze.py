#!/usr/bin/env python3
import glob, json, statistics as st, sys

OUT = "/home/cdkbs/bench-runs/keyfor"
ARMS = ["insert-assert-existing", "insert-assert-newgroup", "insert-none-existing", "insert-none-newgroup",
        "insert-none-again", "update-assert", "update-none", "update-none-again", "delete-assert",
        "delete-none", "select", "ping"]
KEYS = ["p0_us", "p25_us", "p50_us", "p75_us", "p95_us", "p99_us", "mean_us"]


def q(v, p):
    v = sorted(v)
    i = (len(v) - 1) * p
    lo = int(i)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


def load(g, n):
    runs = []
    for f in sorted(glob.glob(f"{OUT}/{g}-n{n}-run*/result.json")):
        runs.append(json.load(open(f)))
    return runs


def main():
    out = {}
    for g in ("int", "date"):
        for n in (200, 1000, 10000):
            runs = load(g, n)
            if not runs:
                continue
            print(f"\n### {g} n={n} runs={len(runs)}")
            errs = sum(r["arms"][l][a]["errors"] for r in runs for l in "AB" for a in ARMS)
            enf = all("enforcing=1" in r["setup"][l]["show_assertions"] or "enforcing" in r["setup"][l]["show_assertions"] for r in runs for l in "AB")
            print("errors total", errs, "show_assertions sample:", runs[0]["setup"]["B"]["show_assertions"][:120].replace("\\n", " | "))
            for a in ARMS:
                row = {}
                for k in KEYS:
                    Av = [r["arms"]["A"][a][k] for r in runs]
                    Bv = [r["arms"]["B"][a][k] for r in runs]
                    d = [b - x for b, x in zip(Bv, Av)]
                    row[k] = (st.median(Av), st.median(Bv), st.median(d), q(d, .25), q(d, .75),
                              sum(1 for x in d if x > 0), len(d))
                # block-level p50 delta (each block pairs A and B)
                bd = []
                for r in runs:
                    for ba, bb in zip(r["blocks"]["A"][a], r["blocks"]["B"][a]):
                        bd.append(bb["p50_us"] - ba["p50_us"])
                eng = []
                for r in runs:
                    ops = r["ops_per_arm"]
                    eng.append(r["engine"]["B"][a]["us"] / ops - r["engine"]["A"][a]["us"] / ops)
                out[(g, n, a)] = (row, bd, eng)
                p = row["p50_us"]
                m = row["mean_us"]
                print(f"{a:<24} p50 A={p[0]:7.1f} B={p[1]:7.1f} dB-A={p[2]:+6.2f} [{p[3]:+6.2f},{p[4]:+6.2f}] pos={p[5]}/{p[6]} | "
                      f"mean dB-A={m[2]:+6.2f} [{m[3]:+6.2f},{m[4]:+6.2f}] | blockp50 med={st.median(bd):+5.2f} "
                      f"[{q(bd,.25):+5.2f},{q(bd,.75):+5.2f}] | engine us/op d={st.median(eng):+5.2f}")
    json.dump({f"{k[0]}|{k[1]}|{k[2]}": v for k, v in out.items()}, open(OUT + "/analysis.json", "w"), indent=1)


main()
