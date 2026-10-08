"""Markdown tables for the purge cost. usage: gen_purge_tables.py"""
import glob, json, statistics, collections

B = "/home/cdkbs/bench-runs/bh-close2"
med = statistics.median


def load(d):
    rows = collections.defaultdict(list)
    for f in sorted(glob.glob(f"{B}/{d}/*.json")):
        j = json.load(open(f))
        rows[(j["durability"], j["cores"], j["window"], j["keys"])].append(j)
    return rows


def pc(arr, k): return med([a[k] for a in arr])


rows = load("purge-tp")
out = []
for dur, title in (("relaxed", "relaxed"), ("group", "group"), ("strict", "strict")):
    keys = sorted((k for k in rows if k[0] == dur), key=lambda k: (k[1], k[2], k[3]))
    if not keys:
        continue
    out.append(f"\n**durability = {title}**\n")
    out.append("| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for k in keys:
        _, cores, w, n = k
        js = rows[k]
        ps = [j["purge"] for j in js]; ds = [j["delete"] for j in js]
        pk = [j["purge_keys_per_s"] for j in js]; dk = [j["delete_keys_per_s"] for j in js]
        stmt = "`PURGE ... WHERE id = k`" if w == 1 else f"`PURGE ... WHERE id BETWEEN a AND a+{w-1}`"
        def us(x):
            return f"{x/1000:,.2f} ms" if x >= 2000 else f"{x:,.1f} µs"
        out.append(f"| {cores} | {stmt} | {n:,} | {len(js)} x {ps[0]['ops']:,} | {med(pk):,.0f} keys/s [{min(pk):,.0f}, {max(pk):,.0f}] | {1e6/med(pk):,.2f} µs | "
                   f"{us(pc(ps,'p0_us'))} / {us(pc(ps,'p25_us'))} / {us(pc(ps,'p50_us'))} / {us(pc(ps,'p95_us'))} / {us(pc(ps,'p99_us'))} | {med(dk):,.0f} keys/s | {us(pc(ds,'p50_us'))} |")
print("\n".join(out))

# latency table
lat = collections.defaultdict(list)
for f in sorted(glob.glob(f"{B}/purge-lat/c*-R*.json")):
    j = json.load(open(f))
    lat[(j["cores"], j["readers"], j["writers"])].append(j)
print("\n\n**DELETE commit to PURGE success**\n")
print("| cores | background load | reps x samples | p0 | p25 | p50 | p95 | p99 | worst single sample (any rep) | PURGE first attempt p50 / p99 | DELETE p50 / p99 | TXN_CONFLICT refusals | each reader: throughput, p50 | writer throughput |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for key in sorted(lat):
    cores, r, w = key
    js = lat[key]
    t = [j["delete_to_purge"] for j in js]; f1 = [j["purge_first_attempt"] for j in js]; d = [j["delete"] for j in js]
    bg = [s for j in js for s in j["background"]["reader"]]; bw = [s for j in js for s in j["background"]["writer"]]
    load = "none (idle server)" if r == 0 and w == 0 else f"{r} readers" + (f" + {w} writer" if w else "")
    rd = f"{med([s['qps'] for s in bg]):,.0f} qps, {med([s['p50_us'] for s in bg]):.0f} µs" if bg else "-"
    wr = f"{med([s['qps'] for s in bw]):,.0f} qps" if bw else "-"
    print(f"| {cores} | {load} | {len(js)} x {t[0]['ops']:,} | {pc(t,'p0_us'):.1f} µs | {pc(t,'p25_us'):.1f} µs | {pc(t,'p50_us'):.1f} µs | {pc(t,'p95_us'):.1f} µs | {pc(t,'p99_us'):.1f} µs | "
          f"{max(x['max_us'] for x in t)/1000:,.1f} ms | {pc(f1,'p50_us'):.1f} / {pc(f1,'p99_us'):.1f} µs | {pc(d,'p50_us'):.1f} / {pc(d,'p99_us'):.1f} µs | {sum(j['refusals'] for j in js)} of {sum(j['samples'] for j in js):,} | {rd} | {wr} |")

print("\n\n**Older snapshot held H ms after the DELETE**\n")
print("| cores | H | keys | PURGE total (DELETE reply to PURGED), min / median / max | first attempt, median | TXN_CONFLICT refusals | total minus H, median |")
print("|---|---|---|---|---|---|---|")
for f in sorted(glob.glob(f"{B}/purge-lat/snap-*.json")):
    j = json.load(open(f))
    by = collections.defaultdict(list)
    for r in j["snapshot_rows"]:
        by[r["hold_ms"]].append(r)
    for h, rs in sorted(by.items()):
        tot = [r["total_ms"] for r in rs]
        print(f"| {j['cores']} | {h} ms | {len(rs)} | {min(tot):.2f} / {med(tot):.2f} / {max(tot):.2f} ms | {med([r['first_attempt_ms'] for r in rs]):.2f} ms | {sum(r['refusals'] for r in rs)} of {len(rs)} | {med(tot)-h:.2f} ms |")
