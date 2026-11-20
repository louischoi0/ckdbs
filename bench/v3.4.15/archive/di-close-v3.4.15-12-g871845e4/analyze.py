#!/usr/bin/env python3
"""DI close analysis: python3 -I analyze.py raw1.jsonl run2_dir
Driver 1 (statement cells, A vs B) and driver 2 (CF's Waystone arms A, A-opt, B, B-opt)."""
import glob, json, os, statistics as st, sys

rows = [json.loads(l) for l in open(sys.argv[1])]
cells = [c for c in ["select1", "select8", "insert1", "insert8", "fk1"] if any(r["cn"] == c for r in rows)]
by = {(r["cn"], r["arm"], r["round"]): r for r in rows}
rounds = sorted({r["round"] for r in rows})


def get(cn, arm, f):
    return [by[(cn, arm, k)][f] for k in rounds if (cn, arm, k) in by]


print("== driver 1 per cell and arm (spread = max - min over lifetimes)")
for cn in cells:
    for a in "AB":
        c = get(cn, a, "cpu_us_per_stmt"); q = get(cn, a, "qps")
        print(f"{cn} {a} n_life={len(c)} cpu_mean={st.mean(c):.2f} cpu_spread={max(c)-min(c):.2f} "
              f"qps_mean={st.mean(q):.0f} qps_spread={max(q)-min(q):.0f} stmts={sum(get(cn,a,'stmts'))} "
              f"bad={sum(get(cn,a,'bad'))}")
print("== latency (us), median of per-lifetime percentiles")
for cn in cells:
    for a in "AB":
        print(cn, a, sum(get(cn, a, "stmts")), *[f"{p}={st.median(get(cn,a,p)):g}" for p in ("p0", "p25", "p50", "p90", "p99")])
print("== paired B - A per round: CPU us/stmt, then QPS %")
for cn in cells:
    A = get(cn, "A", "cpu_us_per_stmt"); B = get(cn, "B", "cpu_us_per_stmt")
    d = [b - a for a, b in zip(A, B)]
    qa = get(cn, "A", "qps"); qb = get(cn, "B", "qps")
    dq = [100 * (b - a) / a for a, b in zip(qa, qb)]
    sp = max(max(A) - min(A), max(B) - min(B))
    print(f"{cn}: " + " ".join(f"{v:+.2f}" for v in d) +
          f" | mean={st.mean(d):+.2f} us ({100*st.mean(d)/st.mean(A):+.1f}% of A) B_slower_in={sum(v>0 for v in d)}/{len(d)}"
          f" arm_spread={sp:.2f} outside_spread={'yes' if abs(st.mean(d))>sp else 'no'}"
          f" | qps%: " + " ".join(f"{v:+.1f}" for v in dq) + f" mean={st.mean(dq):+.1f}% B_slower_in={sum(v<0 for v in dq)}/{len(dq)}")
print("== loads (loadavg at the end of each lifetime), A/B interleaved by round")
for cn in cells:
    print(cn, " ".join(r["load"] for r in rows if r["cn"] == cn))

if len(sys.argv) > 2:
    runs = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(sys.argv[2], "*-r*.json")))]
    arms = ["A", "A-opt", "B", "B-opt"]
    d2 = {}
    for r in runs:
        for c in r["cells"]:
            d2[(c["cell"], r["arm"], r["rep"])] = c
    reps = sorted({r["rep"] for r in runs})
    c2 = [c for c in ["pk", "chain3", "scanprobe"] if any(k[0] == c for k in d2)]

    def g2(cell, arm, f):
        return [d2[(cell, arm, r)][f] for r in reps if (cell, arm, r) in d2]
    print("\n== driver 2 per cell and arm (reps=%d)" % len(reps))
    for cell in c2:
        for a in arms:
            u = g2(cell, a, "server_cpu_us"); q = g2(cell, a, "qps")
            print(f"{cell} {a} n={len(u)} qps_mean={st.mean(q):.0f} qps_spread={max(q)-min(q):.0f} "
                  f"cpu_mean={st.mean(u):.2f} cpu_spread={max(u)-min(u):.2f} p50={st.median(g2(cell,a,'p50_us')):.1f} "
                  f"p99={st.median(g2(cell,a,'p99_us')):.1f} ops_each={d2[(cell,a,reps[0])]['ops']} "
                  f"replays={sorted({c for c in g2(cell,a,'analyze_replays')})}")
    print("== driver 2 paired deltas per rep (X - Y): CPU us/stmt, then qps %")
    for cell in c2:
        for x, y in [("B", "A"), ("B-opt", "A-opt"), ("A-opt", "A"), ("B-opt", "B")]:
            X = g2(cell, x, "server_cpu_us"); Y = g2(cell, y, "server_cpu_us")
            d = [p - q for p, q in zip(X, Y)]
            qx = g2(cell, x, "qps"); qy = g2(cell, y, "qps")
            dq = [100 * (p - q) / q for p, q in zip(qx, qy)]
            sp = max(max(X) - min(X), max(Y) - min(Y))
            print(f"{cell} {x}-{y}: " + " ".join(f"{v:+.2f}" for v in d) +
                  f" | mean={st.mean(d):+.2f} us ({100*st.mean(d)/st.mean(Y):+.1f}%) first_dearer_in={sum(v>0 for v in d)}/{len(d)}"
                  f" arm_spread={sp:.2f} outside={'yes' if abs(st.mean(d))>sp else 'no'}"
                  f" | qps%: " + " ".join(f"{v:+.1f}" for v in dq) + f" mean={st.mean(dq):+.1f}% first_slower_in={sum(v<0 for v in dq)}/{len(dq)}")
    print("== digests")
    for cell in c2:
        ds = {c["digest"] for k, c in d2.items() if k[0] == cell}
        print(cell, "identical" if len(ds) == 1 else f"DIFFER {ds}", f"across {sum(1 for k in d2 if k[0]==cell)} runs")
    print("== run prechecks")
    for r in runs:
        pc = r["precheck"]
        print(f"{r['arm']}-r{r['rep']} {r['finished']} load={pc['loadavg']} waited={pc.get('quiet_waited_s',0)}s competing={len(pc['competing'])}")


# ---- quiet-pair block: lifetimes another session loaded are set aside, said so -----------------
# A lifetime is "loaded" when its server CPU/stmt exceeds 1.10 x the lowest lifetime of its own arm
# and cell (another session's build or driver landing inside it; the host's one-binary floor is
# about 0.3 us on 22 us, so 10 % is wide of the floor and narrow of the spikes). Pairs where either
# lifetime is loaded are dropped, symmetrically; what remains is reported beside the all-lifetime
# numbers above, which stay the record.
def robust(label, X, Y, xs_name, ys_name, tol=1.10):
    mx, my = min(X), min(Y)
    keep = [i for i, (x, y) in enumerate(zip(X, Y)) if x <= tol * mx and y <= tol * my]
    dropped = [i + 1 for i in range(len(X)) if i not in keep]
    Xk = [X[i] for i in keep]; Yk = [Y[i] for i in keep]
    d = [x - y for x, y in zip(Xk, Yk)]
    sp = max(max(Xk) - min(Xk), max(Yk) - min(Yk))
    print(f"{label} {xs_name}-{ys_name}: kept={len(keep)}/{len(X)} dropped_pairs(index)={dropped} "
          f"{xs_name}_mean={st.mean(Xk):.2f} spread={max(Xk)-min(Xk):.2f} {ys_name}_mean={st.mean(Yk):.2f} spread={max(Yk)-min(Yk):.2f} "
          f"delta_mean={st.mean(d):+.2f} median={st.median(d):+.2f} first_dearer_in={sum(v>0 for v in d)}/{len(d)} "
          f"arm_spread={sp:.2f} outside={'yes' if abs(st.mean(d))>sp else 'no'} "
          f"delta_pct_of_{ys_name}={100*st.mean(d)/st.mean(Yk):+.1f}%")


print("\n== robust: driver 1, spiked pairs dropped (CPU us/stmt)")
for cn in cells:
    robust(cn, get(cn, "B", "cpu_us_per_stmt"), get(cn, "A", "cpu_us_per_stmt"), "B", "A")
if len(sys.argv) > 2:
    print("\n== robust: driver 2, spiked pairs dropped (CPU us/stmt)")
    for cell in c2:
        for x, y in [("B", "A"), ("B-opt", "A-opt"), ("A-opt", "A"), ("B-opt", "B")]:
            robust(cell, g2(cell, x, "server_cpu_us"), g2(cell, y, "server_cpu_us"), x, y)

print("\n== quiet pairs, driver 1: per cell and arm over the kept rounds (same rule: both lifetimes <= 1.10 x their own arm's lowest CPU)")
for cn in cells:
    X = get(cn, "A", "cpu_us_per_stmt"); Y = get(cn, "B", "cpu_us_per_stmt")
    mx, my = min(X), min(Y)
    keep = [i for i, (x, y) in enumerate(zip(X, Y)) if x <= 1.10 * mx and y <= 1.10 * my]
    print(f"{cn} kept rounds {[i+1 for i in keep]}")
    for a in "AB":
        sel = lambda f: [get(cn, a, f)[i] for i in keep]
        c = sel("cpu_us_per_stmt"); q = sel("qps")
        print(f"  {a} n={len(c)} cpu_mean={st.mean(c):.2f} cpu_spread={max(c)-min(c):.2f} qps_mean={st.mean(q):.0f} qps_spread={max(q)-min(q):.0f} "
              f"stmts={sum(sel('stmts'))} " + " ".join(f"{p}={st.median(sel(p)):g}" for p in ("p0", "p25", "p50", "p90", "p99")))
