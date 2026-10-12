"""BP-S3's A/B: A = 1592007e, B = ccf9cfc1 (release builds from exports),
cores = 1. The commit loops (ba_census insert1, one session) per durability
and side: stmt/s, latency percentiles, server CPU per statement if the
driver recorded it."""
import glob, json, statistics

OUT = '/home/cdkbs/bench-runs/bp-s1/v2/out/ab'
for dur in ('group', 'strict'):
    for side in ('a', 'b'):
        rows = []
        for r in (1, 2, 3):
            fs = glob.glob(f'{OUT}/commit-{dur}-{side}-r{r}/*.json')
            if not fs:
                continue
            d = json.load(open(fs[0]))
            md = d.get('meta_delta', {})
            cpu = None
            for k in ('sched_foreground_consumed_us',):
                if k in md:
                    cpu = md[k] / max(1, d['statements'])
                    break
            rows.append((d['statements_per_s'], d['p0_us'], d['p25_us'], d['p50_us'], d['p95_us'],
                         d['p99_us'], d['max_us'], cpu))
        if not rows:
            continue
        med = [statistics.median(c) if None not in c else None for c in zip(*rows)]
        print(f'{dur:6} {side} runs={len(rows)} stmt/s={[round(x[0]) for x in rows]} '
              f'median p0/p25/p50/p95/p99/max us = {[round(x, 1) for x in med[1:7]]} cpu/stmt={med[7]}')

print()
for f in sorted(glob.glob(f'{OUT}/bn-r*.json')):
    for line in open(f.replace('.json', '.log')):
        if line.startswith('{'):
            d = json.loads(line)
            print(f"{f[-9:-5]} {d['shape']:12} {d['path']:8} {d['bin']} server_cpu_us={d['server_cpu_us']} p50={d['p50_us']} p99={d['p99_us']}")
