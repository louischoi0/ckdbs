import json, os, sys
D = sys.argv[1]
def L(c, a, r):
    p = f"{D}/de-{c}-{a}-r{r}.json"
    return json.load(open(p)) if os.path.exists(p) else None
print("## resident cells: spread (A r0,r1 | B r0,r1) for hot-from-schedule and service")
for c in ("i", "ii", "v", "vscan", "iii4"):
    for key in ("hot_from_schedule", "hot_service"):
        for p in ("p0_us", "p50_us", "p95_us", "p99_us", "p99.9_us"):
            a = [L(c, "A", r)[key][p] for r in (0, 1)]
            b = [L(c, "B", r)[key][p] for r in (0, 1)]
            sa = (min(a), max(a)); sb = (min(b), max(b))
            overlap = not (sb[0] > sa[1] or sa[0] > sb[1])
            print(f"{c:6} {key:18} {p:9} A {a[0]:8.1f} {a[1]:8.1f} | B {b[0]:8.1f} {b[1]:8.1f} | A spread {sa[1]-sa[0]:6.1f} B spread {sb[1]-sb[0]:6.1f} | meanB-meanA {sum(b)/2-sum(a)/2:+7.1f} ({(sum(b)/sum(a)-1)*100:+5.1f}%) overlap={overlap}")
print("\n## excess iii - ii (same round), from schedule and service")
for a in "AB":
    for r in (0, 1):
        for key in ("hot_from_schedule", "hot_service"):
            t = L("iii", a, r)[key]; u = L("ii", a, r)[key]
            print(a, r, key, " ".join(f"{p}: {t[p]-u[p]:+.1f} ({t[p]/u[p]:.2f}x)" for p in ("p50_us", "p95_us", "p99_us")))
print("\n## iv, scanner rates and hot completion")
for a in "AB":
    for r in (0, 1):
        d = L("iv", a, r)
        print(a, r, "reads_in_window", d["reads_in_window"], "pages/s", d["reads_in_window"] / d["window_s"], "MB/s(8192)", d["reads_in_window"] / d["window_s"] * 8192 / 1e6,
              "hot n", d["hot_from_schedule"]["n"], "hot/s", d["hot_from_schedule"]["n"] / d["window_s"], "proc read_bytes MB/s", d["proc_io_delta"]["read_bytes"] / d["window_s"] / 1e6)
print("\n## vscan: pool_hits/64 slices")
for a in "AB":
    for r in (0, 1):
        d = L("vscan", a, r)
        print(a, r, d["meta_delta"]["pool_hits"], d["meta_delta"]["pool_hits"] / 64 / d["window_s"], "slices/s ; on_cpu", d["waits_delta"]["cpu.on_cpu"]["total_us"] / 1e6)
print("\n## hosts")
for c in ("i", "ii", "iii", "iv", "v", "vscan", "iii4"):
    for r in (0, 1):
        for a in "AB":
            d = L(c, a, r)
            print(c, r, a, d["host_before"]["loadavg"].split()[:3], d["host_after"]["loadavg"].split()[:3], "wait_s", d["wait_s"], "comp", d["host_before"]["competing"], d["host_after"]["competing"])
print("\n## misses per read")
for c in ("ii", "iii", "iv", "iii4"):
    for a in "AB":
        for r in (0, 1):
            d = L(c, a, r)
            w = d["waits_delta"]
            print(c, a, r, "pr max us", w.get("io.page_read", {}).get("max_us_since_start"), "rq max", w.get("sched.run_queue", {}).get("max_us_since_start"), "misses", d["meta_delta"]["pool_misses"], "reads", d["reads_in_window"], "ratio", round(d["meta_delta"]["pool_misses"] / max(1, d["reads_in_window"]), 3), "hits/read", round(d["meta_delta"]["pool_hits"] / max(1, d["reads_in_window"]), 2))
print("keys", list(L("iii", "A", 0).keys()))
d = L("iii", "A", 0)
print(d["cgroup"])
