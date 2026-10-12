"""The close A/B's tables: bn pairs (server CPU per statement, cores = 1) and
ba cells (cores = 4: stmt/s and latency), per binary, median and spread."""
import glob, json, statistics

OUT = '/home/cdkbs/bench-runs/close/out'
med = statistics.median
print('## cores = 1 (bn_census, interleaved blocks; server CPU us per statement)')
for pair in ('S2S1', 'S4S6BQ'):
    acc = {}
    for f in sorted(glob.glob(f'{OUT}/bisect/{pair}-r*.json')):
        for line in open(f.replace('.json', '.log')):
            if line.startswith('{'):
                d = json.loads(line)
                acc.setdefault((d['shape'], d['path'], d['bin']), []).append(d['server_cpu_us'])
    for (shape, path, side) in sorted(k for k in acc if k[2] == 'A'):
        a = acc[(shape, path, 'A')]
        b = acc.get((shape, path, 'B'), [])
        if not b:
            continue
        delta = [100 * (y - x) / x for x, y in zip(a, b)]
        print(f'{pair} {shape:12} {path:8} A {med(a):8.2f} {[round(x,2) for x in a]}  '
              f'{"B" if pair == "AB" else "Q"} {med(b):8.2f} {[round(x,2) for x in b]}  '
              f'paired delta % {[round(x,1) for x in delta]} median {med(delta):+.1f}')

print('\n## cores = 4 (ba_census, relaxed, 8 sessions, 20 s)')
for shape in ('point', 'insert1'):
    for side in ('A', 'Q', 'B'):
        rows = []
        for d in sorted(glob.glob(f'{OUT}/ba/{shape}-{side}-r*')):
            fs = glob.glob(d + '/*.json')
            if not fs:
                continue
            j = json.load(open(fs[0]))
            rows.append((j['statements_per_s'], j['p0_us'], j['p25_us'], j['p50_us'], j['p95_us'],
                         j['p99_us'], j['max_us']))
        if rows:
            cols = list(zip(*rows))
            print(f'{shape:8} {side} runs={len(rows)} stmt/s med {med(cols[0]):,.0f} {[round(x) for x in cols[0]]} '
                  f'p0/p25/p50/p95/p99/max med {[round(med(c), 1) for c in cols[1:]]}')
