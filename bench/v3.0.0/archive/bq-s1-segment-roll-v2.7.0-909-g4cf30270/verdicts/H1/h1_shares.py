"""H1 (workorder-bq section 3): a step's share = its time summed over the rolls
whose hold is at or above the hold's p99, over those rolls' summed hold;
rejected if no step reaches 40 %. Recomputed from the raw per-roll log lines.

Steps follow the order's section 1.1 a-g, from the log fields
(src/wal/stream.cpp, file_log_device.cpp at 4cf30270):
  a seal_us   b flush_us   c sync_gate_us + sync_us
  d body_wait_us + body_us   e header_us   f rename_us   g dirsync_us
  resid = hold - (a..g)  (PublishDurable, encode, the segments_mutex push)
usage: python3 -I h1_shares.py <out_dir> [window]   window: in|strict|all
"""
import glob, json, math, re, sys, random
from fractions import Fraction

ROLL = re.compile(r'\[wal\] roll (.*)$')
STEPS = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'resid']
SUB = ['c_gate', 'c_sync', 'c_sync_tail', 'c_sync_rest', 'd_wait', 'd_inline', 'create_resid']


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


def steps_of(r):
    s = {'a': r['seal_us'], 'b': r['flush_us'], 'c': r['sync_gate_us'] + r['sync_us'],
         'd': r['body_wait_us'] + r['body_us'], 'e': r['header_us'], 'f': r['rename_us'],
         'g': r['dirsync_us']}
    s['resid'] = r['hold_us'] - sum(s.values())
    s['c_gate'] = r['sync_gate_us']
    s['c_sync'] = r['sync_us']
    s['c_sync_tail'] = r['sync_tail_us']
    s['c_sync_rest'] = r['sync_rest_us']
    s['d_wait'] = r['body_wait_us']
    s['d_inline'] = r['body_us']
    s['create_resid'] = r['create_us'] - (s['d'] + s['e'] + s['f'] + s['g'])
    return s


def load(out, window):
    rolls = []
    for js in sorted(glob.glob(f'{out}/bq/*/*/r*/*.json')):
        parts = js.split('/')
        dur, cell, run = parts[-4], parts[-3], parts[-2]
        d = json.load(open(js))
        lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
        for line in open(js[:-5] + '.s.log'):
            m = ROLL.search(line)
            if not m:
                continue
            r = kv(m.group(1))
            if window == 'strict' and (r['start_ns'] < lo or r['end_ns'] > hi):
                continue
            if window == 'in' and not (lo <= r['start_ns'] < hi):
                continue
            r.update(dur=dur, cell=cell, run=run, st=steps_of(r))
            rolls.append(r)
    return rolls


def nearest_rank(xs, p):
    xs = sorted(xs)
    k = max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1
    return xs[min(k, len(xs) - 1)]


def shares(rs, keys=STEPS + SUB):
    tot = sum(r['hold_us'] for r in rs) or 1
    return {k: 100.0 * sum(r['st'][k] for r in rs) / tot for k in keys}


def top(rs, p):
    if not rs:
        return []
    thr = nearest_rank([r['hold_us'] for r in rs], p)
    return [r for r in rs if r['hold_us'] >= thr]


def fmt(sh, keys=STEPS):
    return ' '.join(f'{k}={sh[k]:5.1f}' for k in keys)


def lead(sh):
    k = max(STEPS, key=lambda k: sh[k])
    return k, sh[k]


def report(name, rs):
    if not rs:
        print(f'{name:34} rolls=0')
        return
    t99, t90 = top(rs, 99), top(rs, 90)
    thr = nearest_rank([r['hold_us'] for r in rs], 99)
    s99, s90, sall = shares(t99), shares(t90), shares(rs)
    print(f'{name:34} n={len(rs):3} p99={thr}us n>=p99={len(t99)} n>=p90={len(t90)}')
    print(f'{"":6}>=p99 {fmt(s99)} | lead {lead(s99)[0]} {lead(s99)[1]:.1f}')
    print(f'{"":6}  sub {fmt(s99, SUB)}')
    print(f'{"":6}>=p90 {fmt(s90)} | lead {lead(s90)[0]} {lead(s90)[1]:.1f}')
    print(f'{"":6}  sub {fmt(s90, SUB)}')
    print(f'{"":6}  all {fmt(sall)} | lead {lead(sall)[0]} {lead(sall)[1]:.1f}')
    print(f'{"":6}  sub {fmt(sall, SUB)}')
    print(f'{"":6}  top rolls: ' + '; '.join(
        f"{r['run']}/{r['cell']} hold={r['hold_us']} c={r['st']['c']} (gate={r['st']['c_gate']}) "
        f"d={r['st']['d']} e={r['st']['e']} g={r['st']['g']} body={r['body']}"
        for r in sorted(t99, key=lambda r: -r['hold_us'])[:4]))


def fragility(rs, seed=1):
    if len(rs) < 3:
        return
    runs = sorted(set(r['run'] for r in rs))
    per, loo = [], []
    for run in runs:
        sub = [r for r in rs if r['run'] == run]
        k, v = lead(shares(top(sub, 99)))
        per.append(f'{run}:{k}{v:.0f}(n{len(top(sub, 99))})')
        sub = [r for r in rs if r['run'] != run]
        k, v = lead(shares(top(sub, 99)))
        loo.append(f'-{run}:{k}{v:.0f}')
    rnd = random.Random(seed)
    best = {}
    B = 2000
    any_ge40 = 0
    for _ in range(B):
        bs = [rnd.choice(rs) for _ in rs]
        k, v = lead(shares(top(bs, 99)))
        best[k] = best.get(k, 0) + 1
        any_ge40 += v >= 40
    print(f'{"":6}fragility per-run {per} LOO {loo} bootstrap lead freq {best} '
          f'P(lead>=40)={any_ge40 / B:.3f}')


def main():
    out = sys.argv[1]
    window = sys.argv[2] if len(sys.argv) > 2 else 'in'
    rolls = load(out, window)
    print(f'# window={window} rolls={len(rolls)}')
    print('## per cell x durability, 3 runs pooled')
    for dur in ('relaxed', 'group'):
        for cell in sorted(set(r['cell'] for r in rolls if r['dur'] == dur)):
            rs = [r for r in rolls if r['dur'] == dur and r['cell'] == cell]
            report(f'{dur}/{cell}', rs)
            fragility(rs)
    print('## per durability, H0 cells pooled (C1 x3 placements + C2; iso arms have no rolls)')
    for dur in ('relaxed', 'group'):
        rs = [r for r in rolls if r['dur'] == dur and r['cell'] != 'c3']
        report(f'{dur}/C1+C2', rs)
        fragility(rs)
        rs = [r for r in rolls if r['dur'] == dur]
        report(f'{dur}/C1+C2+C3', rs)
        fragility(rs)
    print('## overall, both durabilities pooled (shown to say why it is wrong)')
    rs = [r for r in rolls if r['cell'] != 'c3']
    report('all/C1+C2', rs)
    print('## per run (each run its own p99)')
    for key in sorted(set((r['dur'], r['cell'], r['run']) for r in rolls)):
        rs = [r for r in rolls if (r['dur'], r['cell'], r['run']) == key]
        t = top(rs, 99)
        sh = shares(t)
        k, v = lead(sh)
        print(f"{'/'.join(key):30} n={len(rs):2} n>=p99={len(t)} lead={k} {v:.1f}  "
              f"c={sh['c']:.1f} d={sh['d']:.1f} e={sh['e']:.1f} g={sh['g']:.1f}")


main()
