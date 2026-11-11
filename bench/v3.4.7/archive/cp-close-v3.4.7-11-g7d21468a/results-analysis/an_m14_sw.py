import json, sys, glob, os
root = sys.argv[1]
def f(x, u=' µs'): return f"{x:,.1f}{u}"
def cpu(path):
    d = json.load(open(path))
    t = sum(int(r['total_us']) for r in d['wait_stats_rows'] if r['event'] == 'cpu.on_cpu')
    lw = d['lock_waits']
    return t, d, lw
def cells(label_pool):
    return ','.join(label_pool['cores'])

print("## M1")
for rep in (1, 2, 3):
    p = f"{root}/m1swap/m1-rep{rep}.json"
    if not os.path.exists(p): continue
    d = json.load(open(p))
    cb, cc = cells(d['pools']['base']), cells(d['pools']['close'])
    clean = ('0' not in d['pools']['base']['cores']) and ('0' not in d['pools']['close']['cores'])
    print(f"\nrep {rep}: cores base={cb} close={cc} -> {'CLEAN' if clean else 'BIASED'}; host before {d['host_before']['loadavg']} after {d['host_after']['loadavg']}")
    print("| arm | binary | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS | err |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for arm in d['arms']['base']:
        for lab in ('base', 'close'):
            a = d['arms'][lab][arm]
            print(f"| {arm} | {lab} | {a['ops']:,} | {f(a['p0_us'])} | {f(a['p25_us'])} | {f(a['p50_us'])} | {f(a['p95_us'])} | {f(a['p99_us'])} | {f(a['mean_us'])} | {a['qps']:,.0f} QPS | {a['errors']} |")
    # cpu
    cb_, _, _ = cpu(f"{root}/m1swap/m1-rep{rep}-base.views.json")
    cc_, _, _ = cpu(f"{root}/m1swap/m1-rep{rep}-close.views.json")
    n = 6 * 40000
    print(f"cpu.on_cpu base {cb_:,} close {cc_:,} base-close {cb_-cc_:,} us; per stmt {(cb_-cc_)/n:.2f} us (n={n})")
    for arm in ('update-hot','update-disjoint','update-disjoint-again','insert-omitted','select-hot'):
        b = d['arms']['base'][arm]['p50_us']; c = d['arms']['close'][arm]['p50_us']
        print(f"  p50 {arm}: {b} -> {c} ({c-b:+.1f})")

print("\n## M4")
p = f"{root}/m4/m4.json"
if os.path.exists(p):
    d = json.load(open(p))
    print("cores", {k: v['cores'] for k, v in d['pools'].items()}, "host", d['host_before']['loadavg'], d['host_after']['loadavg'])
    print("| arm | binary | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS | first error |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for arm in d['arms']['base']:
        for lab in ('base', 'close'):
            a = d['arms'][lab][arm]
            print(f"| {arm} | {lab} | {a['ops']:,} | {a['errors']:,} | {f(a['p0_us'])} | {f(a['p25_us'])} | {f(a['p50_us'])} | {f(a['p95_us'])} | {f(a['p99_us'])} | {f(a['max_us'])} | {a['qps']:,.0f} QPS | {a['first_error']} |")
    for lab in ('base', 'close'):
        t, v, lw = cpu(f"{root}/m4/m4-{lab}.views.json")
        ws = {}
        for r in v['wait_stats_rows']:
            if r['event'] in ('app.lock_wait',):
                pass
        app = [r for r in v['wait_stats']['app_rows']]
        lwc = sum(int(r['count']) for r in app if r['event'] == 'app.lock_wait')
        lwt = sum(int(r['total_us']) for r in app if r['event'] == 'app.lock_wait')
        lcc = sum(int(r['count']) for r in app if r['event'] == 'app.lock_conflict')
        print(lab, "lock_waits", json.dumps(lw)[:600], "app.lock_wait count/us", lwc, lwt, "lock_conflict", lcc, "cpu", t)
