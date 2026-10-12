import json, sys
d = json.load(open(sys.argv[1]))
print({k: d[k] for k in d if k not in ('windows', 'backups', 'meta_end', 'host_start', 'host_end')})
for w in d['windows']:
    k = w['kinds']
    print(f"{w['label']:10s} {w['elapsed_s']:6.2f}s {w['stmts']:6d} {w['stmt_per_s']:8.1f}/s | " +
          " | ".join(f"{n} {k[n]['ops']} p50={k[n].get('p50_us')} p99={k[n].get('p99_us')}" for n in k))
for b in d['backups']:
    print(b)
print(d['host_start'], d['host_end'])
if len(sys.argv) > 2:
    rows = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
    print("max load", max(float(r['load']) for r in rows), "foreign samples", sum(1 for r in rows if r['foreign']))
    for r in rows:
        if r['foreign']:
            print(r['t'], r['foreign'][:2])
