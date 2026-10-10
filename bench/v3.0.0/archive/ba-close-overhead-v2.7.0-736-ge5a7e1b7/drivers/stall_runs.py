"""Per run, the elapsed-time gap B - A of an arm (ms) and the largest single statement (max_us) of each server.
usage: stall_runs.py <dir> <arm> <rows>"""
import glob, json, statistics as st, sys

d, arm, rows = sys.argv[1], sys.argv[2], sys.argv[3]
fs = sorted((f for f in glob.glob(f"{d}/n{rows}-r*.json") if not f.endswith(".raw.json")), key=lambda f: int(f.split("-r")[1].split(".")[0]))
gaps, ma, mb = [], [], []
for f in fs:
    s = json.load(open(f))
    a, b = s["arms"]["A"][arm], s["arms"]["B"][arm]
    gaps.append((b["elapsed_s"] - a["elapsed_s"]) * 1000)
    ma.append(a["max_us"] / 1000)
    mb.append(b["max_us"] / 1000)
print(f"{d} {arm} {rows} rows, {len(fs)} runs")
print("  B - A elapsed (ms) per run:", " ".join(f"{g:+.0f}" for g in gaps))
print(f"  median {st.median(gaps):+.1f} ms; runs with B - A over +40 ms: {sum(g > 40 for g in gaps)}; under -40 ms: {sum(g < -40 for g in gaps)}")
print("  largest single statement, A (ms):", " ".join(f"{x:.0f}" for x in ma))
print("  largest single statement, B (ms):", " ".join(f"{x:.0f}" for x in mb))
print(f"  runs whose largest statement exceeds 40 ms: A {sum(x > 40 for x in ma)}, B {sum(x > 40 for x in mb)}")
