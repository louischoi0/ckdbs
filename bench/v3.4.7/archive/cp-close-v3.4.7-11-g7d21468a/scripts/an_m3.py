import json, sys, glob, os, re
root = sys.argv[1]
def f(x): return f"{x:,.1f}"
for d in sorted(x for x in glob.glob(root + '/m3-*') if os.path.isdir(x)):
    name = os.path.basename(d)
    kind = 's2' if '-s2-' in name else 's0'
    print(f"\n### {name}")
    try:
        j = json.load(open(f"{d}/{kind}.json"))
    except Exception as e:
        print("no json", e); continue
    log = open(f"{d}/{kind}.run.log").read()
    m = re.search(r"committed\s+(\d+)\s+([\d.]+) TPS", log) or re.search(r"transactions.*", log)
    if m: print("committed line:", m.group(0))
    for l in log.splitlines():
        if re.search(r"verify|rolled back|ran out|conflicted|torn|skipped|rejected|committed|TPS", l): print("  ", l.strip())
    ph = j['phases']
    print("| phase | ops | errors | p0 | p25 | p50 | p95 | p99 | max | rate |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for p in ph:
        if p['phase'] in ('booking','commit','operation-update','org-update','freight-insert','charge-insert','txn','trade-insert','account-update','cargo-read','manifest'):
            print(f"| {p['phase']} | {p['ops']:,} | {p['errors']} | {f(p['p0_us'])} | {f(p['p25_us'])} | {f(p['p50_us'])} | {f(p['p95_us'])} | {f(p['p99_us'])} | {f(p['max_us'])} | {p['qps']:,.0f}/s |")
    print("phases seen:", [p['phase'] for p in ph])
    v = json.load(open(f"{d}/{kind}-post.views.json"))
    lw = v['lock_waits_since_baseline']
    print("lock_waits:", lw['rows_total'], "granted", lw['granted'], "deadlock", lw['deadlock'], "fault_net", lw['fault_net'], "cancelled", lw['cancelled'], "sum_us", lw.get('waited_us_sum'), "max", lw.get('waited_us_max'), "p50", lw.get('waited_us_p50'))
    for r in lw['by_unit_mode_outcome']: print("   ", r)
    for r in lw.get('by_relation_unit_mode_outcome', []): print("   ", r)
    dl = v['wait_stats']['delta']
    parts = dl['db_time_parts_us']; tot = dl['db_time_us']
    print("DB time", f"{tot:,}", {k: f"{x:,} ({100*x/tot:.3f}%)" for k, x in parts.items()})
    print("app.lock_wait", dl['lock_wait'], "app.lock_conflict", dl['lock_conflict_count'], "share of DB", dl['lock_wait_share_of_db_time'])
    mp = v['meta']['picked']; md = v['meta']['delta']
    print("meta:", {k: mp[k] for k in mp if 'lock' in k or 'fence' in k or 'inflight' in k})
    st = v['statement_stats']
    print("stmt refused (since boot)", st['refused'], [ (k['kind'], k['refused']) for k in st['by_kind'] if k['refused']])
    print("ash lock_wait samples", v['ash']['lock_wait']['samples'])
    # start/stop errors
    print("errors", v['errors'])
