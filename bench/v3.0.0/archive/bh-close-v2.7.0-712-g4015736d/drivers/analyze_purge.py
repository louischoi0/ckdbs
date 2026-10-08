import glob, json, statistics, sys, collections

B = "/home/cdkbs/bench-runs/bh-close2"
rows = collections.defaultdict(list)
for f in sorted(glob.glob(f"{B}/{sys.argv[1] if len(sys.argv) > 1 else 'purge-tp'}/*.json")):
    j = json.load(open(f))
    rows[(j["durability"], j["cores"], j["window"], j["keys"])].append(j)


def med(xs):
    return statistics.median(xs)


print("## purge throughput (B only): medians over runs, [min,max]")
for key in sorted(rows, key=lambda k: (k[0] != "relaxed", k[0], k[1], k[2], k[3])):
    js = rows[key]
    dur, cores, w, n = key
    pk = [j["purge_keys_per_s"] for j in js]; dk = [j["delete_keys_per_s"] for j in js]
    ps = [j["purge"] for j in js]; ds = [j["delete"] for j in js]
    def pc(arr, k): return med([a[k] for a in arr])
    loads = [float(j["host_before"]["loadavg"].split()[0]) for j in js]
    print(f"{dur} c{cores} window={w} keys={n} runs={len(js)} stmts/run={ps[0]['ops']}: "
          f"PURGE keys/s med {med(pk):,.0f} [{min(pk):,.0f},{max(pk):,.0f}] stmts/s {med(pk)/w:,.1f} | "
          f"PURGE stmt p0/25/50/95/99 us = {pc(ps,'p0_us'):.1f}/{pc(ps,'p25_us'):.1f}/{pc(ps,'p50_us'):.1f}/{pc(ps,'p95_us'):.1f}/{pc(ps,'p99_us'):.1f} max {max(p['max_us'] for p in ps):.0f} mean {pc(ps,'mean_us'):.1f} | "
          f"DELETE keys/s med {med(dk):,.0f} [{min(dk):,.0f},{max(dk):,.0f}] stmt p0/25/50/95/99 us = {pc(ds,'p0_us'):.1f}/{pc(ds,'p25_us'):.1f}/{pc(ds,'p50_us'):.1f}/{pc(ds,'p95_us'):.1f}/{pc(ds,'p99_us'):.1f} | "
          f"us/key purge {1e6/med(pk):.2f} | errs {sum(p['errors'] for p in ps)+sum(d['errors'] for d in ds)} bad {sum(j['bad_purge_replies'] for j in js)} | "
          f"reins placed {js[0]['reinserted_keys']}/{n} sep_refused {js[0]['reinsert_refused_separator']} other {len(js[0]['reinsert_refused_other'])} | load {min(loads)}-{max(loads)}")

print("\n## DELETE -> PURGE under readers (B only): per cell, median over 3 reps of each percentile; max = max over reps")
lat = collections.defaultdict(list)
for f in sorted(glob.glob(f"{B}/purge-lat/c*-R*.json")):
    j = json.load(open(f))
    lat[(j["cores"], j["readers"], j["writers"])].append(j)
for key in sorted(lat):
    js = lat[key]
    cores, r, w = key
    t = [j["delete_to_purge"] for j in js]
    f1 = [j["purge_first_attempt"] for j in js]
    d = [j["delete"] for j in js]
    def pc(arr, k): return med([a[k] for a in arr])
    bg = [s for j in js for s in j["background"]["reader"]]
    bw = [s for j in js for s in j["background"]["writer"]]
    rq = med([s["qps"] for s in bg]) if bg else 0
    wq = med([s["qps"] for s in bw]) if bw else 0
    rp50 = med([s["p50_us"] for s in bg]) if bg else 0
    gt = sum(1 for j in js for _ in [0])
    print(f"c{cores} R={r} W={w} reps={len(js)} n/rep={t[0]['ops']}: DELETE->PURGE us p0/25/50/95/99 = {pc(t,'p0_us'):.1f}/{pc(t,'p25_us'):.1f}/{pc(t,'p50_us'):.1f}/{pc(t,'p95_us'):.1f}/{pc(t,'p99_us'):.1f} "
          f"mean {pc(t,'mean_us'):.1f} max {max(x['max_us'] for x in t):.0f} | DELETE alone p50 {pc(d,'p50_us'):.1f} p99 {pc(d,'p99_us'):.1f} | "
          f"refusals {sum(j['refusals'] for j in js)} bad {sum(j['bad'] for j in js)} sep_refused/rep {js[0]['reinsert_refused_separator']} other {sum(j['reinsert_refused_other'] for j in js)} | "
          f"readers qps/each {rq:,.0f} p50 {rp50:.1f} us, writer qps {wq:,.0f} | "
          f"load {min(float(j['host_before']['loadavg'].split()[0]) for j in js)}-{max(float(j['host_after']['loadavg'].split()[0]) for j in js)}")

print("\n## snapshot hold")
for f in sorted(glob.glob(f"{B}/purge-lat/snap-*.json")):
    j = json.load(open(f))
    print(f, "cores", j["cores"])
    by = collections.defaultdict(list)
    for r in j["snapshot_rows"]:
        by[r["hold_ms"]].append(r)
    for h, rs in sorted(by.items()):
        tot = [r["total_ms"] for r in rs]
        print(f"  hold {h} ms: total ms min/med/max = {min(tot)}/{med(tot)}/{max(tot)}; first attempt ms med {med([r['first_attempt_ms'] for r in rs])}; refusals {sum(r['refusals'] for r in rs)} of {len(rs)} keys; wait beyond hold ms med {med(tot)-h:.2f}")
