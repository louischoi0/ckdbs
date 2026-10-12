"""BP-S6's A/B: lock cells at cores = 8, relaxed, plain and one held fence;
S6A = ccf9cfc1 (64 x 8 partitions), B = 3e77e7d1 (64 partitions)."""
import json, os, statistics

D = '/home/cdkbs/bench-runs/close/out/lock'
med = statistics.median
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        rows = []
        for r in range(3):
            f = f'{D}/{side}-locks-relaxed-k8-{mode}-r{r}.json'
            if not os.path.exists(f):
                continue
            m = json.load(open(f))
            b = json.load(open(f.replace('.json', '.bench.json')))
            stm = b['sessions'] * b['ops_per_session_per_arm'] * 6
            arm = b['arms']['B']['update-disjoint']
            waits = m['meta_after'].get('contention_lock_partition_waits', 0) - m['meta_before'].get('contention_lock_partition_waits', 0)
            rows.append((m['server_cpu_s'] * 1e6 / stm, arm['qps'], arm['p99_us'], waits / stm))
        if rows:
            c = list(zip(*rows))
            print(f'k8 {mode:5} {side:3} runs={len(rows)} us/stmt med {med(c[0]):.1f} {[round(x, 1) for x in c[0]]} '
                  f'update-disjoint qps med {med(c[1]):,.0f} p99 med {med(c[2]):.0f} '
                  f'partition waits/stmt {[f"{x:.2e}" for x in c[3]]}')
