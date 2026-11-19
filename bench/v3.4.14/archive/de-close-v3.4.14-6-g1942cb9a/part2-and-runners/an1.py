import json,glob,sys,os
D=sys.argv[1]
cells=["i","ii","iii","iv","v","vscan","iii4"]
def L(c,a,r):
    p=f"{D}/de-{c}-{a}-r{r}.json"
    return json.load(open(p)) if os.path.exists(p) else None
P=["p0_us","p25_us","p50_us","p95_us","p99_us","p99.9_us","max_us","n"]
def row(d):
    return " | ".join(f"{d[k]:,.1f}" if k!="n" else f"{d[k]:,}" for k in P)
def sec(title,key):
    print("\n###",title)
    print("| cell | round | arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |\n|---|---|---|---|---|---|---|---|---|---|---|")
    for c in cells:
        for r in (0,1):
            for a in "AB":
                d=L(c,a,r)
                if d and d.get(key): print(f"| {c} | r{r} | {a} | {row(d[key])} |")
sec("hot schedule","hot_from_schedule"); sec("hot service","hot_service")
sec("cold service","cold_service"); sec("cold schedule","cold_from_schedule")
print("\n### waits etc")
print("| cell | r | arm | load before/after (1min) | wait_s | competing | on_cpu s | pr n | pr total s | pr mean us | read share | on_cpu share | would_wait | reruns | reads_in_win | pool hits | pool misses | dev B/read | sched.rq n/total s | idle_block s | fg_polled s |\n|"+"---|"*21)
for c in cells:
    for r in (0,1):
        for a in "AB":
            d=L(c,a,r)
            if not d: continue
            w=d["waits_delta"]; pr=d["io_hist_delta"]
            m=d["meta_delta"]
            comp=d["host_before"]["competing"]+d["host_after"]["competing"]
            n=d["reads_in_window"]; t=d["read_us_in_window"]
            rq=w.get("sched.run_queue",{})
            print(f"| {c} | r{r} | {a} | {d['host_before']['loadavg'].split()[0]}/{d['host_after']['loadavg'].split()[0]} | {d['wait_s']} | {comp or '-'} | {w['cpu.on_cpu']['total_us']/1e6:.3f} | {n:,} | {t/1e6:.3f} | {t/n if n else 0:.1f} | {d['read_share_of_one_core']*100:.2f}% | {d['on_cpu_share_of_one_core']*100:.2f}% | {d['pool_would_wait']} | {d['miss_reruns']} | {n:,} | {m['pool_hits']:,.0f} | {m['pool_misses']:,.0f} | {d['device_bytes_per_read']} | {rq.get('count',0):,}/{rq.get('total_us',0)/1e6:.3f} | {m['sched_idle_block_us']/1e6:.2f} | {m['sched_foreground_polled_us']/1e6:.2f} |")
print("\n### io hist")
for c in cells:
    for r in (0,1):
        for a in "AB":
            d=L(c,a,r)
            if d and d["io_hist_delta"]: print(c,r,a,d["io_hist_delta"], "errors",d["errors"])
print("\n### hot_by_core / session reads (iii4)")
for r in (0,1):
    for a in "AB":
        d=L("iii4",a,r)
        if d:
            for k,v in d["hot_by_core"].items(): print("iii4",r,a,"core",k,row(v))
