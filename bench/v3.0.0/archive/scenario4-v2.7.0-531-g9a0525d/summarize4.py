#!/usr/bin/env python3
import json, re
J = "/home/cdkbs/bench-runs/rebaseline-9a0525d/json"
runs = {"A": "s4", "B": "cd2-s4"}
GROUPS = ["board", "tape200", "tape1k", "tape10k", "pk", "open", "close"]
ARMS = ["off", "on", "declared"]

def load(t):
    return json.load(open(f"{J}/{t}.json"))

def get(d, day, g, arm):
    for p in d["phases"]:
        if p["phase"] == f"d{day}-{g}[{arm}]":
            return p

print("### p50 (µs) and derived QPS by arm, per day - run A (default cooldown) / run B (cooldown 2 half-lives)")
print("| group | day | off A | off B | on A | on B | declared A | declared B | on/off speed-up (A / B) |")
print("|---|---|---|---|---|---|---|---|---|")
dA, dB = load("s4"), load("cd2-s4")
for g in GROUPS:
    for day in (1, 2, 3):
        cells = []
        sp = []
        vals = {}
        for arm in ARMS:
            for d, nm in ((dA, "A"), (dB, "B")):
                p = get(d, day, g, arm)
                vals[(arm, nm)] = p
                cells.append(f"{p['p50_us']:,.1f} µs / {1e6/p['mean_us']:,.0f} qps")
        for nm in "AB":
            sp.append(f"{vals[('off', nm)]['mean_us']/vals[('on', nm)]['mean_us']:.2f}x")
        print(f"| {g} | {day} | " + " | ".join(cells) + f" | {sp[0]} / {sp[1]} |")
print()
print("(the last column is the ratio of mean latencies, off over on; qps derived as 1,000,000 / mean us, one serial connection)")
print()
print("### full percentile table, run A, µs")
print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |")
print("|---|---|---|---|---|---|---|---|---|")
for p in dA["phases"]:
    print(f"| {p['phase']} | {p['ops']:,} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} |")
print()
print("### full percentile table, run B (cooldown 2 half-lives), µs")
print("| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |")
print("|---|---|---|---|---|---|---|---|---|")
for p in dB["phases"]:
    print(f"| {p['phase']} | {p['ops']:,} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} |")
print()
print("### server CPU seconds by arm and phase group (driver's /proc reading), A / B")
print("| arm | board | tape | open | close |")
print("|---|---|---|---|---|")
for arm in ARMS:
    row = []
    for g in ("board", "tape", "open", "close"):
        a = sum(v for k, v in dA["server_cpu_s"][arm].items() if k.endswith(g))
        b = sum(v for k, v in dB["server_cpu_s"][arm].items() if k.endswith(g))
        row.append(f"{a:.2f} s / {b:.2f} s")
    print(f"| {arm} | " + " | ".join(row) + " |")
print()
print("### optimizer lifecycle on the `on` arm (evidence snapshots)")
for nm, d in (("A", dA), ("B", dB)):
    print(f"run {nm}:")
    print("| t (s) | day | tag | managed | creates | extends | drops | entries |")
    print("|---|---|---|---|---|---|---|---|")
    for e in d["evidence"]:
        on = e.get("on")
        if not on or e["tag"] not in ("trading-block-3", "trading-block-9", "trading-block-12", "overnight"):
            continue
        m = re.search(r"managed=(\d+).*creates=(\d+) extends=(\d+) heals=(\d+) drops=(\d+)", on["header"])
        st = []
        for x in on["entries"]:
            r = re.search(r"rel=(\S+) .*state=(\S+) cabin_id=(\d+) pages=(\d+)", x)
            st.append(f"{r.group(1)}:{r.group(2)}/{r.group(4)}p")
        print(f"| {e['t_wall_s']} s | {e['day']} | {e['tag']} | {m.group(1)} | {m.group(2)} | {m.group(3)} | {m.group(5)} | {'; '.join(st)} |")
    print()
    print(f"final header: `{d['final']['on']['header']}`")
    print()
    print("busy-time TPS is in the driver's stdout; byte-identical verify per day: " + ", ".join(
        "PASS" if not v["mismatches"] else "FAIL" for v in d["verify"]))
    print()
