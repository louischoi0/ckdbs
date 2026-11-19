import json, collections
R = []
for f in ['run1/out.jsonl', 'run2/out-pressure.jsonl', 'run3/out-pressure.jsonl']:
    for l in open(f):
        r = json.loads(l)
        if r.get('recovery'):
            continue
        r['src'] = f
        R.append(r)
C = collections.OrderedDict()
for r in R:
    C.setdefault(r['cell'], {}).setdefault(r['label'], []).append(r)
a = lambda x: sum(x) / len(x)
name = lambda c: c.replace('-c1', ' | 1').replace('-c4', ' | 4')
rows_label = {'select-pk-10000': 'select-pk, 10K', 'update-lit-200': 'update 200', 'update-lit-1000': 'update 1K',
              'update-lit-10000': 'update 10K', 'update-lit-100000': 'update 100K',
              'update-spill-assign-10000': 'spill update 10K', 'insert-0': 'insert (from empty)',
              'update-lit-1000000': 'update 1M, pool 2,048'}
def lab(c):
    base, cores = c.rsplit('-c', 1)
    return rows_label[base], cores
print('### T1 throughput')
print('| Cell | cores | A run 1 | A run 2 | B run 1 | B run 2 | A mean | B mean | B vs A |')
print('|---|---|---|---|---|---|---|---|---|')
spreadA = []
for c, d in C.items():
    l, k = lab(c)
    A = [r['stmt_per_s'] for r in d['A']]; B = [r['stmt_per_s'] for r in d['B']]
    cells = [f"{x:,.0f} stmt/s" for x in A] + [''] * (2 - len(A))
    cellsB = [f"{x:,.0f} stmt/s" for x in B] + [''] * (2 - len(B))
    print(f"| {l} | {k} | {cells[0]} | {cells[1]} | {cellsB[0]} | {cellsB[1]} | {a(A):,.0f} stmt/s | {a(B):,.0f} stmt/s | {100*(a(B)/a(A)-1):+.1f} % |")
    if len(A) == 2:
        spreadA.append((abs(A[0]-A[1])/a(A)*100, abs(B[0]-B[1])/a(B)*100, c))
print('noise (|r1-r2|/mean %), A, B:', [(round(x, 1), round(y, 1), c) for x, y, c in spreadA])
print('max A', max(x for x, _, _ in spreadA), 'max B', max(y for _, y, _ in spreadA))
print()
print('### T2 latency')
print('| Cell | cores | build | n | p0 | p25 | p50 | p90 | p99 |')
print('|---|---|---|---|---|---|---|---|---|')
for c, d in C.items():
    l, k = lab(c)
    for L in 'AB':
        rs = d[L]
        n = sum(r['statements'] for r in rs)
        q = lambda p: a([r['lat_us'][p] for r in rs])
        print(f"| {l} | {k} | {L} | {n:,} | {q('0'):,.0f} us | {q('25'):,.0f} us | {q('50'):,.0f} us | {q('90'):,.0f} us | {q('99'):,.0f} us |")
print()
print('### T3 cost')
print('| Cell | cores | cpu A | cpu B | log A | log B | image B/stmt (B) | fetch_images B (per window) | ckpt dur A | ckpt dur B |')
print('|---|---|---|---|---|---|---|---|---|---|')
for c, d in C.items():
    l, k = lab(c)
    g = lambda L, f: a([f(r) for r in d[L]])
    cpu = lambda r: r['cpu_us_per_stmt']
    lg = lambda r: r['delta']['wal_append_lsn'] / r['statements']
    ckd = lambda r: a([x['duration_us'] for x in r['checkpoints']]) / 1000
    fi = [r['delta']['pool_fetch_images'] for r in d['B']]
    fis = [r['delta']['pool_fetch_images'] / r['statements'] for r in d['B']]
    print(f"| {l} | {k} | {g('A',cpu):.1f} us | {g('B',cpu):.1f} us | {g('A',lg):,.1f} B | {g('B',lg):,.1f} B | {a(fis)*8192:,.1f} B ({a(fis):.3f} img) | {', '.join(f'{x:,}' for x in fi)} | {g('A',ckd):.1f} ms | {g('B',ckd):.1f} ms |")
print()
print('### cpu per-round (A1 A2 B1 B2) and ckpt per round')
for c, d in C.items():
    print(c, [round(r['cpu_us_per_stmt'], 1) for r in d['A'] + d['B']], [round(a([x['duration_us'] for x in r['checkpoints']]) / 1000, 1) for r in d['A'] + d['B']])
