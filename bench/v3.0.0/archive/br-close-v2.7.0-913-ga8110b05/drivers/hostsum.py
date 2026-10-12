import json, sys, time
R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/logs"
print("| samples file (cells it covers) | UTC window | samples | 1-min load, min-max | mean busy, all 8 CPUs | samples with a foreign process >= 20 % of a CPU | those processes |")
print("|---|---|---|---|---|---|---|")
for name, what in (("host-samples-m1b.jsonl", "M1b cores 1 and cores 4"), ("host-samples-m1b-off.jsonl", "M1b cores 1, copier off the reactor CPU"),
                   ("host-samples-m3.jsonl", "M3 restores"), ("host-samples-stall.jsonl", "stall census")):
    rows = [json.loads(l) for l in open(f"{R}/{name}") if l.strip()]
    if not rows:
        continue
    loads = [float(r["load"]) for r in rows]
    busy = sum(sum(r["busy"].values()) / 8 for r in rows) / len(rows)
    foreign = [r for r in rows if r["foreign"]]
    procs = sorted({f.split(None, 2)[2][:70] for r in foreign for f in r["foreign"]})
    t0, t1 = rows[0]["t"], rows[-1]["t"]
    print(f"| `{name}` ({what}) | {time.strftime('%H:%M:%S', time.gmtime(t0))}-{time.strftime('%H:%M:%S', time.gmtime(t1))} | {len(rows)} | {min(loads):.2f}-{max(loads):.2f} | {busy:.0f} % | {len(foreign)} | {'; '.join(procs[:4]) or '-'} |")
