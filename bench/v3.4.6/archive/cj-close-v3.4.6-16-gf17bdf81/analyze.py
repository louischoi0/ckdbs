import json, glob, sys, statistics as st
d = sys.argv[1]
fs = [json.load(open(f)) for f in sorted(glob.glob(d + "/*.json"))]
ARMS = "ABCO"
rows_set = sorted({f["rows"] for f in fs})
def med(x): return st.median(x)
def pct(a, b): return (a / b - 1) * 100
print("runs", len(fs))
for rows in rows_set:
    g = [f for f in fs if f["rows"] == rows]
    print(f"\n== rows {rows}  runs={len(g)}  errors={sum(f['arms'][a]['errors'] for f in g for a in ARMS)}")
    # per-metric medians across runs
    for key in ("qps", "cpu_us_per_op", "mean_us", "p0_us", "p25_us", "p50_us", "p95_us", "p99_us"):
        print(f"{key:14}", " ".join(f"{a}={med([f['arms'][a][key] for f in g]):9.2f}" for a in ARMS))
    # paired per-run ratios (the noise floor: spread of ratios)
    for name, x, y in (("B vs C (G1)", "B", "C"), ("B vs O (G2)", "B", "O"), ("B vs A (milestone)", "B", "A"), ("A vs C", "A", "C"), ("O vs C", "O", "C")):
        for key, sign in (("qps", 1), ("cpu_us_per_op", -1)):
            r = [pct(f["arms"][x][key], f["arms"][y][key]) for f in g]
            # throughput loss: for qps, loss = -pct; for cpu, loss = +pct (cost up)
            loss = [-v if key == "qps" else v for v in r]
            mm = med(loss)
            q = sorted(loss)
            print(f"  {name:20} {key:14} loss median {mm:+6.2f}%  mean {st.fmean(loss):+6.2f}%  min {q[0]:+6.2f} max {q[-1]:+6.2f}  sd {st.pstdev(loss):.2f}")
    # pooled-ratio medians
    print("  fg polled us/poll:", " ".join(f"{a}={sum(f['arms'][a]['fg_polled_us'] for f in g)/max(1,sum(f['arms'][a]['fg_polls'] for f in g)):.2f}" for a in ARMS))
    print("  polls/op:", " ".join(f"{a}={sum(f['arms'][a]['fg_polls'] for f in g)/sum(f['arms'][a]['ops'] for f in g):.3f}" for a in ARMS))
