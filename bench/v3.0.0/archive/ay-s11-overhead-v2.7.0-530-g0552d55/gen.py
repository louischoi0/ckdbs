import json, statistics as st
R = "/home/cdkbs/bench-runs/ay-s11"


def row(s):
    return f"{s['p0_us']:.1f} / {s['p25_us']:.1f} / {s['p50_us']:.1f} / {s['p95_us']:.1f} / {s['p99_us']:.1f} µs, {s['qps']:.0f} qps"


print("### Cell 1 raw")
for r in (1, 2, 3):
    d = json.load(open(f"{R}/c1-run{r}/result.json"))
    print(f"\n**`c1-run{r}/result.json`** - load before `{d['host_before']['loadavg']}`, after `{d['host_after']['loadavg']}`; competing builds: {d['host_before']['competing'] or 'none'}\n")
    print("| arm | B (`0552d55`): p0 / p25 / p50 / p95 / p99, throughput | A (`58198cb`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |\n|---|---|---|---|")
    for a in d["arms"]["B"]:
        print(f"| `{a}` | {row(d['arms']['B'][a])} | {row(d['arms']['A'][a])} | {d['arms']['B'][a]['errors']} / {d['arms']['A'][a]['errors']} |")
print("\n### Cell 2 raw (median across the five runs of each run's whole-arm summary, slow-mode blocks included)\n")
runs = [json.load(open(f"{R}/c2-run{r}/result.json")) for r in range(1, 6)]
print("| arm | B: p0 / p25 / p50 / p95 / p99, throughput | A: p0 / p25 / p50 / p95 / p99, throughput |\n|---|---|---|")
for a in runs[0]["arms"]["B"]:
    def med(l):
        return " / ".join(f"{st.median([d['arms'][l][a][k] for d in runs]):.1f}" for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + f" µs, {st.median([d['arms'][l][a]['qps'] for d in runs]):.0f} qps"
    print(f"| `{a}` | {med('B')} | {med('A')} |")
print("\nPer run, whole-arm p50 B / A in µs (a slow-mode block moves an arm's p50 by ~24 µs):\n")
print("| arm | run 1 | run 2 | run 3 | run 4 | run 5 |\n|---|---|---|---|---|---|")
for a in runs[0]["arms"]["B"]:
    print(f"| `{a}` | " + " | ".join(f"{d['arms']['B'][a]['p50_us']:.1f} / {d['arms']['A'][a]['p50_us']:.1f} µs" for d in runs) + " |")
print("\nLoad before each run:")
for r in range(1, 6):
    d = runs[r - 1]
    print(f"- `c2-run{r}`: before `{d['host_before']['loadavg']}`, after `{d['host_after']['loadavg']}`, competing: {d['host_before']['competing'] or 'none'}")
print("\n### Cell 3 raw\n")
c3 = [json.load(open(f"{R}/c3-run{r}/result.json")) for r in (1, 2, 3)]
for r, d in enumerate(c3, 1):
    print(f"- `c3-run{r}`: before `{d['host_before']['loadavg']}`, after `{d['host_after']['loadavg']}`, competing: {d['host_before']['competing'] or 'none'}")
print("\nPer K and kind, per row = transaction wall time / K; min, p25, median, p75 over every rep of the three runs, in µs per row.\n")
print("| K | kind | reps | B: min / p25 / median / p75 | A: min / p25 / median / p75 |\n|---|---|---|---|---|")
for k in c3[0]["ks"]:
    for kind in ("fk", "plain"):
        cells = []
        for l in "BA":
            v = sorted(t / k for d in c3 for t in d["cells"][l][f"{kind}-K{k}"]["txn_us"])
            cells.append(f"{v[0]:.1f} / {v[len(v)//4]:.1f} / {v[len(v)//2]:.1f} / {v[3*len(v)//4]:.1f} µs")
        print(f"| {k} | `{kind}` | {len(v)} | {cells[0]} | {cells[1]} |")
