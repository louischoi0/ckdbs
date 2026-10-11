import glob, json, statistics, sys
base = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
for d, cores in (("overhead-c1", 1), ("overhead-c2", 2), ("null-c1", 1), ("strict-c1", 1)):
    for rows in (200, 1000, 10000):
        js = [json.load(open(f)) for f in glob.glob(f"{base}{d}/c{cores}-n{rows}-r*.json")]
        if not js:
            continue
        hot = [j for j in js if max(j["mount_ckpt_us"]["A"], j["mount_ckpt_us"]["B"]) > 100000]
        med = statistics.median(j["mount_ckpt_us"][L] for j in js for L in "AB")
        out = f"{d} n{rows}: {len(js)} runs, mount ckpt median {med:,.0f} us, > 100 ms in {len(hot)} runs ({[j['run'] for j in hot]})"
        if hot and len(hot) < len(js):
            keep = [j for j in js if j not in hot]
            for s in ("update-pk", "select-pk", "insert", "range100", "ping"):
                a = statistics.median(j["arms"]["B"][s]["p50_us"] - j["arms"]["A"][s]["p50_us"] for j in js)
                b = statistics.median(j["arms"]["B"][s]["p50_us"] - j["arms"]["A"][s]["p50_us"] for j in keep)
                out += f" | {s} d {a:+.2f}->{b:+.2f}"
            pa = statistics.median(j["arms"]["A"]["ping"]["p50_us"] for j in hot)
            pb = statistics.median(j["arms"]["A"]["ping"]["p50_us"] for j in keep)
            out += f" | ping A p50 hot {pa:.1f} vs rest {pb:.1f}"
        print(out)
