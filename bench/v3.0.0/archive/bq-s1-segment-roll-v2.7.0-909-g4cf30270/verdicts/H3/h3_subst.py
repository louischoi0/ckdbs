"""Is the non-tail time additive (breadth) or substitutive (whichever fdatasync
runs first absorbs the device/journal flush)? Per waited sync and per own sync:
compare total sync time when rest is ~0 vs when rest is large, and correlate
rest with the live segment count. usage: python3 -I h3_subst.py <out_dir>
"""
import glob, json, os, re, statistics, sys

ROLL = re.compile(r'\[wal\] roll (.*)$')


def kv(t):
    o = {}
    for tok in t.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                o[k] = int(v)
            except ValueError:
                o[k] = v
    return o


out = sys.argv[1]
for dur in ('relaxed', 'group'):
    W_lo, W_hi, own_lo, own_hi, pairs = [], [], [], [], []
    dev = set()
    for js in glob.glob(f'{out}/bq/{dur}/*/r*/*.json'):
        d = json.load(open(js))
        dev.add(json.dumps(d.get('data_device'), sort_keys=True))
        lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
        for line in open(js[:-5] + '.s.log'):
            m = ROLL.search(line)
            if not m:
                continue
            r = kv(m.group(1))
            if r['start_ns'] < lo or r['end_ns'] > hi:
                continue
            n = r['waited_syncs']
            if n:
                per = (r['waited_sync_tail_us'] + r['waited_sync_rest_us']) / n
                frac = r['waited_sync_rest_us'] / max(1, r['waited_sync_tail_us'] + r['waited_sync_rest_us'])
                (W_lo if frac < 0.05 else W_hi if frac > 0.3 else []).append(per)
                pairs.append((r['waited_sync_segs'] / n, r['waited_sync_rest_us'] / n))
            if r['sync_us'] > 0:
                (own_lo if r['sync_rest_us'] < 0.05 * r['sync_us'] else own_hi if r['sync_rest_us'] > 0.3 * r['sync_us'] else []).append(r['sync_us'])
    f = lambda xs: (len(xs), round(statistics.median(xs)) if xs else None, round(statistics.mean(xs)) if xs else None)
    print(f'== {dur} devices={dev}')
    print('  waited syncs, per-sync total us (n, median, mean): rest<5%:', f(W_lo), ' rest>30%:', f(W_hi))
    print('  own sync total us (n, median, mean): rest<5%:', f(own_lo), ' rest>30%:', f(own_hi))
    if len(pairs) > 2:
        xs, ys = zip(*pairs)
        print('  corr(segments per waited sync, rest us per waited sync) =', round(statistics.correlation(xs, ys), 3))
        big = sorted(ys)
        print('  rest us per waited sync: p50', round(big[len(big) // 2]), 'p90', round(big[int(len(big) * .9)]), 'max', round(big[-1]),
              ' share of syncs with rest >10 ms:', round(100 * sum(1 for y in ys if y > 10000) / len(ys), 1), '%')
