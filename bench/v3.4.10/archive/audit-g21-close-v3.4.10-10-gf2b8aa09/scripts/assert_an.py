import json, sys, glob, os
root = sys.argv[1]
f = lambda x: f"{x:,.1f}"
for p in sorted(glob.glob(root + '/assert*.json')):
    d = json.load(open(p)); n = os.path.basename(p)
    print(f"\n### {n}  create_assertion_ms A {d['setup']['A']['create_assertion_ms']:.1f} B {d['setup']['B']['create_assertion_ms']:.1f}; host {d['host_before']['loadavg']}->{d['host_after']['loadavg']}; competing {len(d['host_before']['competing'])}/{len(d['host_after']['competing'])}")
    print("| arm | build | ops | p0 | p25 | p50 | p95 | p99 | QPS |")
    print("|---|---|---|---|---|---|---|---|---|")
    for arm in ('insert-assert-existing','insert-assert-newgroup','insert-none-existing','insert-none-newgroup','insert-none-again','select'):
        for b in ('A','B'):
            a = d['arms'][b][arm]
            print(f"| {arm} | {b} | {a['ops']:,} | {f(a['p0_us'])} | {f(a['p25_us'])} | {f(a['p50_us'])} | {f(a['p95_us'])} | {f(a['p99_us'])} | {a['qps']:,.0f} |")
    for b in ('A','B'):
        A = d['arms'][b]
        print(b, "assert-existing minus control p50: %+.1f; newgroup: %+.1f; noise floor (none vs none-again): %+.1f" % (
            A['insert-assert-existing']['p50_us']-A['insert-none-existing']['p50_us'],
            A['insert-assert-newgroup']['p50_us']-A['insert-none-newgroup']['p50_us'],
            A['insert-none-existing']['p50_us']-A['insert-none-again']['p50_us']))
