"""AZ-S7: raw tables (markdown) for every cell from the run directories. R is the run directory."""
import glob, json, os, statistics as st
R = "/home/cdkbs/bench-runs/az-s7"
ENG = {"B": "B (`273416c`)", "A": "A (`a59da9c`)"}


def row(a, arms):
    b, x = arms["B"][a], arms["A"][a]
    f = lambda s: f"{s['p0_us']} / {s['p25_us']} / {s['p50_us']} / {s['p95_us']} / {s['p99_us']} µs, {s['qps']:.0f} qps"
    return f"| `{a}` | {f(b)} | {f(x)} | {b['errors']} / {x['errors']} errors |"


def hdr(tag):
    d = json.load(open(f"{R}/{tag}/result.json"))
    hb, ha = d["host_before"], d["host_after"]
    pre = open(f"{R}/{tag}/host.txt").read().splitlines()[0]
    print(f"\n**`{tag}/result.json`** - {pre}; in-driver load before `{hb['loadavg']}`, after `{ha['loadavg']}`; competing builds: {'none' if not hb['competing'] and not ha['competing'] else hb['competing'] + ha['competing']}\n")
    return d


def arm_table(d):
    print("| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |")
    print("|---|---|---|---|")
    for a in d["arms"]["B"]:
        print(row(a, d["arms"]))


print("### Cell 1 raw (assertion admission)")
for n in (200, 1000, 10000):
    for r in range(1, 6):
        tag = f"c1-n{n}-run{r}"
        d = hdr(tag)
        print(f"preload {n}, create assertion B / A: {d['setup']['B']['create_assertion_ms']:.1f} / {d['setup']['A']['create_assertion_ms']:.1f} ms")
        print()
        arm_table(d)
print("\n### Cell 2 raw (child insert, fk)")
for r in range(1, 6):
    tag = f"c2-run{r}"
    d = hdr(tag)
    arm_table(d)
print("\n### Cell 3 raw (per-K, per-run medians of per-row wall time, engine time per row)")
for r in (1, 2, 3):
    tag = f"c3-run{r}"
    d = hdr(tag)
    print("| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |")
    print("|---|---|---|---|---|---|---|---|")
    for k in d["ks"]:
        for kind in ("fk", "plain"):
            c = {l: d["cells"][l][f"{kind}-K{k}"] for l in "BA"}
            m = {l: st.median(c[l]["txn_us"]) / k for l in "BA"}
            e = {l: c[l]["engine_us"] / (len(c[l]["txn_us"]) * k) for l in "BA"}
            print(f"| {k} | `{kind}` | {c['B']['reps']} | {m['B']:.1f} µs | {m['A']:.1f} µs | {e['B']:.1f} µs | {e['A']:.1f} µs | {c['B']['errors']} / {c['A']['errors']} |")
