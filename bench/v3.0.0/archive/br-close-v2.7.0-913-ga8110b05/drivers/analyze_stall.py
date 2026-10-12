import json, glob, statistics as st
print("| run | binary | window | statements | statements/s raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | longest statement | statements over 100 ms |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
rows = {}
for f in sorted(glob.glob("/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/out/stall/*.json")):
    d = json.load(open(f))
    tag = f.split("stall-")[1].split(".")[0]
    w = d["windows"][-1]
    a = w["all"]
    ex = (w["stmts"] - a["slow_gt_1ms_n"]) / (w["elapsed_s"] - a["slow_gt_1ms_s"])
    share = 100 * a["slow_gt_1ms_s"] / w["elapsed_s"]
    rows.setdefault(tag[0], []).append((w["stmt_per_s"], ex, share, a["p99_us"], a["max_us"]))
    print(f"| {tag} | {tag[0]} | {w['elapsed_s']:.1f} s | {w['stmts']:,} | {w['stmt_per_s']:,.0f} /s | {ex:,.0f} /s | {share:.0f} % | {a['p50_us']:.1f} µs | {a['p99_us']:.1f} µs | {a['p999_us']:.1f} µs | {a['max_us'] / 1000:,.0f} ms | n/a |")
for k, v in rows.items():
    print(k, "raw", [round(x[0]) for x in v], "excl", [round(x[1]) for x in v], "share %", [round(x[2]) for x in v], "max ms", [round(x[4] / 1000) for x in v])
