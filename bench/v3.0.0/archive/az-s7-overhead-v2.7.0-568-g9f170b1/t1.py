"""AZ-S7 cell 1 (assertion admission): per preload size, B - A p50 per arm over
clean block pairs (a block is slow when its p50 exceeds 1.12 x that server's
own minimum block for the arm, as t2b.py), the floor (insert-none-existing vs
insert-none-again on one server), engine time per op, whole-arm qps."""
import json, statistics as st, sys
R = "/home/cdkbs/bench-runs/az-s7b"
for n in (200, 1000, 10000):
    runs = [json.load(open(f"{R}/c1-n{n}-run{r}/result.json")) for r in range(1, 6)]
    arms = list(runs[0]["arms"]["B"])
    print(f"== preload {n}: create_assertion_ms B/A per run:",
          [(round(d['setup']['B']['create_assertion_ms'], 1), round(d['setup']['A']['create_assertion_ms'], 1)) for d in runs])
    print("arm | clean pairs | B-A p50 median (IQR) | B-A engine/op | B p50 | A p50 | slow B/A | qps B/A (median of runs)")
    for a in arms:
        thr = {l: 1.12 * min(b["p50_us"] for d in runs for b in d["blocks"][l][a]) for l in "AB"}
        dd, de, bv, av = [], [], [], []
        sB = sA = 0
        for d in runs:
            for b, x in zip(d["blocks"]["B"][a], d["blocks"]["A"][a]):
                fb, fa = b["p50_us"] > thr["B"], x["p50_us"] > thr["A"]
                sB += fb; sA += fa
                if not fb and not fa:
                    dd.append(b["p50_us"] - x["p50_us"]); de.append(b["engine_us_per_op"] - x["engine_us_per_op"])
                    bv.append(b["p50_us"]); av.append(x["p50_us"])
        s = sorted(dd)
        qb = st.median([d["arms"]["B"][a]["qps"] for d in runs]); qa = st.median([d["arms"]["A"][a]["qps"] for d in runs])
        print(f"{a:<24}| {len(dd)}/{sum(len(d['blocks']['B'][a]) for d in runs)} | {st.median(dd):+.2f} ({s[len(s)//4]:+.2f}..{s[3*len(s)//4]:+.2f}) | {st.median(de):+.2f} | {st.median(bv):.1f} | {st.median(av):.1f} | {sB}/{sA} | {qb:.0f}/{qa:.0f}")
    print("floor (same server, same block, none-existing vs none-again, all blocks):")
    for l in "BA":
        d_ = [x["p50_us"] - y["p50_us"] for d in runs for x, y in zip(d["blocks"][l]["insert-none-existing"], d["blocks"][l]["insert-none-again"])]
        s = sorted(d_)
        print(l, len(d_), f"median {st.median(d_):+.2f} IQR {s[len(s)//4]:+.2f}..{s[3*len(s)//4]:+.2f} abs-median {st.median([abs(v) for v in d_]):.2f}")
    print("assertion premium (assert - none) p50 per server, paired same block:")
    for l in "BA":
        for sh in ("existing", "newgroup"):
            d_ = [x["p50_us"] - y["p50_us"] for d in runs for x, y in zip(d["blocks"][l][f"insert-assert-{sh}"], d["blocks"][l][f"insert-none-{sh}"])]
            e_ = [x["engine_us_per_op"] - y["engine_us_per_op"] for d in runs for x, y in zip(d["blocks"][l][f"insert-assert-{sh}"], d["blocks"][l][f"insert-none-{sh}"])]
            s = sorted(d_)
            print(f"  {l} {sh}: {st.median(d_):+.2f} us (IQR {s[len(s)//4]:+.2f}..{s[3*len(s)//4]:+.2f}) engine {st.median(e_):+.2f} us, n={len(d_)}")
    print("errors:", [(d['arms'][l][a]['errors']) for d in runs for l in 'BA' for a in arms if d['arms'][l][a]['errors']])
