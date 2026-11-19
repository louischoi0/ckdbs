import json, os, sys, statistics as st
D = sys.argv[1]
def L(c, a, r):
    p = f"{D}/de-{c}-{a}-r{r}.json"
    return json.load(open(p)) if os.path.exists(p) else None
for c in ("iii", "iv"):
    print("##", c)
    print("| round | arm | hot sched p50 | p95 | p99 | n | hot svc p50 | p95 | p99 | cold svc p50 | p95 | p99 | read share | on_cpu share | pr mean | pr max | would_wait | reads | pool misses | pages/s | load b/a | comp |")
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            if not d: continue
            h = d["hot_from_schedule"]; s = d["hot_service"]; co = d.get("cold_service") or {}
            pr = d["waits_delta"].get("io.page_read", {})
            n = d["reads_in_window"]
            print(f"| r{r} | {a} | {h['p50_us']:,.1f} | {h['p95_us']:,.1f} | {h['p99_us']:,.1f} | {h['n']:,} | {s['p50_us']:,.1f} | {s['p95_us']:,.1f} | {s['p99_us']:,.1f} | {co.get('p50_us','-')} | {co.get('p95_us','-')} | {co.get('p99_us','-')} | {d['read_share_of_one_core']*100:.2f}% | {d['on_cpu_share_of_one_core']*100:.2f}% | {pr.get('total_us',0)/max(1,n):.1f} | {pr.get('max_us_since_start')} | {d['pool_would_wait']} | {n:,} | {d['meta_delta']['pool_misses']:,.0f} | {n/d['window_s']:,.0f} | {d['host_before']['loadavg'].split()[0]}/{d['host_after']['loadavg'].split()[0]} | {d['host_before']['competing'] + d['host_after']['competing']} |")
print("\n## iii medians over 4 rounds")
for a in "AB":
    for key in ("hot_from_schedule", "hot_service", "cold_service"):
        xs = [L("iii", a, r)[key] for r in range(4)]
        print(a, key, {p: (round(st.median(x[p] for x in xs), 1), round(min(x[p] for x in xs), 1), round(max(x[p] for x in xs), 1)) for p in ("p50_us", "p95_us", "p99_us")})
    print(a, "on_cpu share", [round(L("iii", a, r)["on_cpu_share_of_one_core"] * 100, 2) for r in range(4)], "read share", [round(L("iii", a, r)["read_share_of_one_core"] * 100, 2) for r in range(4)])
print("excess iii-ii (ii from rounds 0,1 only; iii r0,r1)")
