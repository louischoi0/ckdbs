"""AZ-S7 cell 1: the assertion premium of B against the premium of A.
((B assert - B none) - (A assert - A none)) per block position, p50 us, with the
engine's own time per op; also each raw premium. Reads the c1-n* runs."""
import json, statistics as st
R = "/home/cdkbs/bench-runs/az-s7"


def q(v, p):
    v = sorted(v)
    return v[int(p * (len(v) - 1))]


for n in (200, 1000, 10000):
    runs = [json.load(open(f"{R}/c1-n{n}-run{r}/result.json")) for r in range(1, 6)]
    print(f"== preload {n}")
    for sh in ("existing", "newgroup"):
        d_, e_, pb, pa = [], [], [], []
        for d in runs:
            for i in range(len(d["blocks"]["B"][f"insert-assert-{sh}"])):
                def g(l, k, f):
                    return d["blocks"][l][k][i][f]
                prem = {l: g(l, f"insert-assert-{sh}", "p50_us") - g(l, f"insert-none-{sh}", "p50_us") for l in "BA"}
                eprem = {l: g(l, f"insert-assert-{sh}", "engine_us_per_op") - g(l, f"insert-none-{sh}", "engine_us_per_op") for l in "BA"}
                d_.append(prem["B"] - prem["A"]); e_.append(eprem["B"] - eprem["A"])
                pb.append(prem["B"]); pa.append(prem["A"])
        print(f"  {sh}: premium B {st.median(pb):+.2f} A {st.median(pa):+.2f}; B-A of premium {st.median(d_):+.2f} us "
              f"(IQR {q(d_, .25):+.2f}..{q(d_, .75):+.2f}) engine {st.median(e_):+.2f} (IQR {q(e_, .25):+.2f}..{q(e_, .75):+.2f}) n={len(d_)}")
