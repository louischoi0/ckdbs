#!/usr/bin/env python3
"""Cell 4 under relaxed (p4b), strict (p4s) and group (p4g): dirty pass and clean re-run, one row per size and rep."""
import json

R = "/home/cdkbs/bench-runs/cd-close/out/"
print("| durability | rows | rep | dirty pass: rows/s | dirty wall | server CPU | roll hold / rolls | longest non-yielding run | clean re-run wall | log bytes of the re-run |")
print("|---|---|---|---|---|---|---|---|---|---|")
for lab, f in (("relaxed", "p4b"), ("strict", "p4s"), ("group", "p4g")):
    d = json.load(open(R + f + ".json"))
    for r in d["cell4"]:
        if r["rows"] < 10000:
            continue
        x, c = r["dirty"], r["clean"]
        m = x["meta_delta"]
        run = max(m.get("sched_hold_out_run_longest_us", 0), m.get("sched_hold_in_run_longest_us", 0)) / 1e3
        print(f"| {lab} | {r['rows']:,} | {r['rep']} | {r['rows'] / x['wall_s']:,.0f} | {x['wall_s'] * 1e3:.1f} ms | {x['server_cpu_s'] * 1e3:.0f} ms | "
              f"{m.get('wal_roll_hold_us', 0) / 1e3:.0f} ms / {m.get('wal_rolls', 0)} | {run:.0f} ms | {c['wall_s'] * 1e3:.2f} ms | {c['wal_bytes']} |")
