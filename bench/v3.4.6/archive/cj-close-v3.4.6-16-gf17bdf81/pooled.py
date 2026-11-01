import json, glob, sys, random
d = sys.argv[1]
fs = [json.load(open(f)) for f in sorted(glob.glob(d + "/*.json"))]
ARMS = "ABCO"
random.seed(1)


def pooled(g, a, key):
    if key == "qps":
        return sum(f['arms'][a]['ops'] for f in g) / sum(f['arms'][a]['elapsed_s'] for f in g)
    if key == "poll_us_op":
        return sum(f['arms'][a]['fg_polled_us'] for f in g) / sum(f['arms'][a]['ops'] for f in g)
    if key == "cpu_us_op":
        return sum(f['arms'][a]['server_cpu_s'] for f in g) * 1e6 / sum(f['arms'][a]['ops'] for f in g)


def boot(g, x, y, key, n=2000):
    out = []
    for _ in range(n):
        s = [random.choice(g) for _ in g]
        out.append((pooled(s, x, key) / pooled(s, y, key) - 1) * 100)
    out.sort()
    return out[int(.025 * n)], out[int(.975 * n)]


for sel in [None] + sorted({f['rows'] for f in fs}):
    g = [f for f in fs if sel is None or f['rows'] == sel]
    print(f"\n-- rows {sel or 'all'} n_runs={len(g)} ops/arm={sum(f['arms']['A']['ops'] for f in g)}")
    for key in ("qps", "poll_us_op", "cpu_us_op"):
        print(f"{key:11}", " ".join(f"{a}={pooled(g, a, key):8.3f}" for a in ARMS))
        for x, y, nm in (("B", "C", "B/C G1"), ("B", "O", "B/O G2"), ("B", "A", "B/A mile"), ("A", "C", "A/C ctl"), ("O", "C", "O/C"), ("O", "A", "O/A")):
            r = (pooled(g, x, key) / pooled(g, y, key) - 1) * 100
            lo, hi = boot(g, x, y, key)
            print(f"    {nm:9} {r:+6.2f}%  95%CI [{lo:+6.2f},{hi:+6.2f}]")
