import glob, json, statistics, sys
for d, cores in (("overhead-c1", 1), ("overhead-c2", 2), ("null-c1", 1)):
    for rows in (200, 1000, 10000):
        fs = glob.glob(f"/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/{d}/c{cores}-n{rows}-r*.json")
        js = [json.load(open(f)) for f in fs]
        if not js:
            continue
        line = []
        for c in "01234567":
            line.append(f"{c}:{statistics.median(j['cpu_busy'][c] for j in js):.2f}/{max(j['cpu_busy'][c] for j in js):.2f}")
        print(d, rows, len(js), " ".join(line))
