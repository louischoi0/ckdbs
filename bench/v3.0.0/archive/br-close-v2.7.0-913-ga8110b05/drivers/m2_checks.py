import json
d = json.load(open("/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/out/m2/m2.json"))
S, series = d["summary"], d["series"]
mk = S["marks"]
print("blocking max", max(s["wal_archive_blocking"] for s in series))
def at(t):
    b = series[0]
    for s in series:
        if s["t"] <= t:
            b = s
    return b
rates = []
for s in series:
    if s["t"] + 5 > mk["load_end"]:
        break
    e = at(s["t"] + 5)
    rates.append((e["wal_durable_lsn"] - s["wal_durable_lsn"]) / (1 << 20) / (e["t"] - s["t"]))
print("5 s windows", len(rates), "above 80:", sum(1 for r in rates if r > 80), "above 100:", sum(1 for r in rates if r > 100), "below 40:", sum(1 for r in rates if r < 40))
print("final", series[-1])
