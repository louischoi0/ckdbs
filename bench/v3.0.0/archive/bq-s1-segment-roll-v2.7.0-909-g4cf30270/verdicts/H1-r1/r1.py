#!/usr/bin/env python3
# Independent recomputation of H1 step shares from raw "[wal] roll" lines.
import glob, json, os, re, sys, random
from collections import defaultdict

ROOT = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
KV = re.compile(r'(\w+)=(\S+)')
STEPS = ['a', 'b', 'c', 'd', 'e', 'f', 'g']


def steps_of(r):
    return {
        'a': r['seal_us'], 'b': r['flush_us'],
        'c': r['sync_gate_us'] + r['sync_us'],
        'd': r['body_wait_us'] + r['body_us'],
        'e': r['header_us'], 'f': r['rename_us'], 'g': r['dirsync_us'],
        'gate': r['sync_gate_us'], 'tail': r['sync_tail_us'], 'rest': r['sync_rest_us'],
    }


def load(mode):
    data = defaultdict(list)   # (dur, cell) -> list of (run, roll)
    meta = {}
    for dur in ('relaxed', 'group'):
        for celld in sorted(glob.glob(f'{ROOT}/{dur}/*')):
            cell = os.path.basename(celld)
            for rund in sorted(glob.glob(celld + '/r*')):
                run = os.path.basename(rund)
                js = glob.glob(rund + '/*.json')
                logs = glob.glob(rund + '/*.s.log')
                if len(js) != 1 or len(logs) != 1:
                    print('WARN files', rund, len(js), len(logs), file=sys.stderr)
                j = json.load(open(js[0]))
                lo, hi = j['measure_from_mono_ns'], j['stop_at_mono_ns']
                rolls = []
                seen = set()
                for line in open(logs[0], errors='replace'):
                    if '[wal] roll ' not in line:
                        continue
                    r = {}
                    for k, v in KV.findall(line.split('[wal] roll', 1)[1]):
                        try:
                            r[k] = int(v)
                        except ValueError:
                            r[k] = v
                    if r['seg'] in seen:
                        print('WARN dup seg', rund, r['seg'], file=sys.stderr)
                    seen.add(r['seg'])
                    if mode == 'start' and not (lo <= r['start_ns'] < hi):
                        continue
                    if mode == 'strict' and not (lo <= r['start_ns'] and r['end_ns'] <= hi):
                        continue
                    rolls.append(r)
                meta[(dur, cell, run)] = len(rolls)
                for r in rolls:
                    data[(dur, cell)].append((run, r))
    return data, meta


def p99_nr(vals, q=0.99):
    s = sorted(vals)
    import math
    k = max(1, math.ceil(q * len(s)))
    return s[k - 1]


def shares(rolls, q=0.99):
    holds = [r['hold_us'] for _, r in rolls]
    if not holds:
        return None
    thr = p99_nr(holds, q) if q is not None else -1
    top = [r for _, r in rolls if r['hold_us'] >= thr]
    H = sum(r['hold_us'] for r in top)
    acc = defaultdict(int)
    for r in top:
        for k, v in steps_of(r).items():
            acc[k] += v
    sh = {k: 100.0 * acc[k] / H for k in acc}
    resid = 100.0 * (H - sum(acc[s] for s in STEPS)) / H
    return len(holds), thr, len(top), sh, resid


def fmt(sh):
    order = sorted(STEPS, key=lambda s: -sh[s])
    s = ' '.join(f'{k}={sh[k]:.1f}' for k in order if sh[k] >= 0.05)
    return s + f' | gate={sh["gate"]:.1f} tail={sh["tail"]:.1f} rest={sh["rest"]:.1f}'


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'start'
    data, meta = load(mode)
    print('mode', mode)
    print('rolls per run:')
    for k in sorted(meta):
        print('  ', k, meta[k])
    H0 = ['c1-batch0', 'c1-segregated-no0', 'c1-spread', 'c2']
    for q, label in ((0.99, '>=p99'), (0.90, '>=p90'), (None, 'all')):
        print('\n==', label)
        for dur in ('relaxed', 'group'):
            for cell in sorted({c for d, c in data if d == dur}):
                res = shares(data[(dur, cell)], q)
                if res is None:
                    print(f'{dur:8s} {cell:20s} no rolls')
                    continue
                n, thr, nt, sh, resid = res
                print(f'{dur:8s} {cell:20s} n={n:4d} thr={thr/1000:8.1f}ms top={nt:3d} {fmt(sh)} resid={resid:.2f}')
            pool = [x for c in H0 for x in data[(dur, c)]]
            n, thr, nt, sh, resid = shares(pool, q)
            print(f'{dur:8s} {"H0-pool":20s} n={n:4d} thr={thr/1000:8.1f}ms top={nt:3d} {fmt(sh)} resid={resid:.2f}')
    # per-run p99 leader
    print('\n== per-run >=p99 leader')
    for dur in ('relaxed', 'group'):
        for cell in sorted({c for d, c in data if d == dur}):
            out = []
            for run in ('r1', 'r2', 'r3'):
                rr = [x for x in data[(dur, cell)] if x[0] == run]
                if not rr:
                    continue
                n, thr, nt, sh, _ = shares(rr)
                lead = max(STEPS, key=lambda s: sh[s])
                out.append(f'{run}:n={n},top={nt},{lead}{sh[lead]:.0f}')
            print(f'{dur:8s} {cell:20s} ' + '  '.join(out))
    # leave-one-run-out and bootstrap on group H0 pool
    random.seed(1)
    for dur in ('relaxed', 'group'):
        pool = [x for c in H0 for x in data[(dur, c)]]
        lo = []
        for run in ('r1', 'r2', 'r3'):
            sub = [x for x in pool if x[0] != run]
            _, _, _, sh, _ = shares(sub)
            lead = max(STEPS, key=lambda s: sh[s])
            lo.append(f'{lead}{sh[lead]:.0f}')
        cnt = defaultdict(int); ge40 = 0
        B = 2000
        for _ in range(B):
            s = [random.choice(pool) for _ in pool]
            _, _, _, sh, _ = shares(s)
            lead = max(STEPS, key=lambda k: sh[k])
            cnt[lead] += 1
            ge40 += sh[lead] >= 40
        print(f'{dur} H0 pool LORO {lo} bootstrap lead {dict(cnt)} P(>=40)={ge40/B:.3f}')
    # absolutes
    print('\n== absolutes (H0 pool)')
    for dur in ('relaxed', 'group'):
        pool = [r for c in H0 for _, r in data[(dur, c)]]
        def q3(vals):
            return (p99_nr(vals, 0.5) / 1000, p99_nr(vals, 0.99) / 1000, max(vals) / 1000)
        hold = [r['hold_us'] for r in pool]
        c = [r['sync_gate_us'] + r['sync_us'] for r in pool]
        g = [r['dirsync_us'] for r in pool]
        efg = [r['header_us'] + r['rename_us'] + r['dirsync_us'] for r in pool]
        gate = [r['sync_gate_us'] for r in pool]
        body = defaultdict(int)
        for r in pool:
            body[r['body']] += 1
        print(dur, 'n', len(pool), 'hold p50/p99/max %.1f/%.1f/%.1f' % q3(hold),
              'c %.1f/%.1f/%.1f' % q3(c), 'g %.1f/%.1f/%.1f' % q3(g), 'efg %.1f/%.1f/%.1f' % q3(efg),
              'gate p50 %.1f sum %.1fs of hold %.1fs' % (p99_nr(gate, .5) / 1000, sum(gate) / 1e6, sum(hold) / 1e6),
              dict(body))
        # e+f+g share of all hold
        print('   e+f+g share of all hold %.1f%%' % (100 * sum(efg) / sum(hold)))


main()
