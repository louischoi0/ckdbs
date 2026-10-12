"""H3 measurement half, recomputed from raw per-roll lines.
usage: python3 -I h3.py <out_dir>
Step c of a roll = sync_gate_us (waiting out the writer's sync in flight)
+ sync_us (the roll's own FileLogDevice::SyncTimed). The non-tail fdatasync
time inside step c = own sync_rest_us + the part of the waited-out syncs'
rest that fell inside the gate wait. The waited-out totals count a sync in
flight at the wait's start whole (stream.hpp RollRecord), so that part is
bounded: pre = max(0, W - G) is the waited syncs' time before the wait began
(W = waited tail+rest, G = gate wait; gaps between syncs make the true pre
larger, never smaller). Each SyncTimed syncs the non-tail fds first, so pre
may be all rest: lo = max(0, waited_rest - pre); hi = min(waited_rest, G).
"""
import glob, json, math, os, re, sys
from fractions import Fraction

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


def pct(xs, p):
    xs = sorted(xs)
    k = max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1
    return xs[min(k, len(xs) - 1)]


def rolls(js, window=True):
    d = json.load(open(js))
    lo, hi = d.get('measure_from_mono_ns'), d.get('stop_at_mono_ns')
    out, all_ = [], 0
    for line in open(js[:-5] + '.s.log'):
        m = ROLL.search(line)
        if not m:
            continue
        r = kv(m.group(1))
        all_ += 1
        if window and (r['start_ns'] < lo or r['end_ns'] > hi):
            continue
        G, S = r['sync_gate_us'], r['sync_us']
        W = r['waited_sync_tail_us'] + r['waited_sync_rest_us']
        pre = max(0, W - G)
        r['C'] = G + S
        r['wr_lo'] = max(0, r['waited_sync_rest_us'] - pre)
        r['wr_hi'] = min(r['waited_sync_rest_us'], G)
        r['pre'] = pre
        r['hold_chk'] = (r['end_ns'] - r['start_ns']) // 1000 - r['hold_us']
        out.append(r)
    return d, out, all_


def agg(top):
    s = lambda f: sum(r[f] for r in top)
    C = s('C') or 1
    return dict(n=len(top), hold=s('hold_us'), C=s('C'),
                c_of_hold=round(100 * s('C') / max(1, s('hold_us')), 1),
                gate_of_c=round(100 * s('sync_gate_us') / C, 1),
                own_sync_of_c=round(100 * s('sync_us') / C, 2),
                own_rest_of_c=round(100 * s('sync_rest_us') / C, 2),
                own_rest_of_own=round(100 * s('sync_rest_us') / max(1, s('sync_us')), 1),
                share_lo=round(100 * (s('sync_rest_us') + s('wr_lo')) / C, 1),
                share_hi=round(100 * (s('sync_rest_us') + s('wr_hi')) / C, 1),
                waited_rest_of_waited=round(100 * s('waited_sync_rest_us') / max(1, s('waited_sync_rest_us') + s('waited_sync_tail_us')), 1),
                waited_over_gate=round(s('waited_sync_rest_us') + s('waited_sync_tail_us')) / max(1, s('sync_gate_us')),
                analyzer_style=round(100 * (s('sync_rest_us') + s('waited_sync_rest_us')) / C, 1),
                segs=[r['sync_segs'] for r in top],
                wsegs=[round(r['waited_sync_segs'] / r['waited_syncs'], 1) if r['waited_syncs'] else None for r in top],
                nwaited=[r['waited_syncs'] for r in top],
                hold_us=[r['hold_us'] for r in top])


out = sys.argv[1]
for dur in ('relaxed', 'group'):
    for cell in sorted(os.listdir(f'{out}/bq/{dur}')):
        pooled, per = [], []
        for js in sorted(glob.glob(f'{out}/bq/{dur}/{cell}/r*/*.json')):
            d, rs, n_all = rolls(js)
            if not rs:
                continue
            pooled += rs
            p99 = pct([r['hold_us'] for r in rs], 99)
            a = agg([r for r in rs if r['hold_us'] >= p99])
            a['rolls_in_window'] = len(rs)
            a['rolls_in_log'] = n_all
            a['meta_rolls'] = d['meta_delta'].get('wal_rolls')
            a['segs_max_run'] = max(r['sync_segs'] for r in rs)
            a['hold_chk_max'] = max(abs(r['hold_chk']) for r in rs)
            per.append((os.path.basename(os.path.dirname(js)), a))
        if not pooled:
            continue
        holds = [r['hold_us'] for r in pooled]
        print(f'== {dur} {cell} rolls={len(pooled)} hold p50={pct(holds, 50)} p99={pct(holds, 99)} max={max(holds)}')
        for lab, p in (('>=p99', 99), ('>=p90', 90), ('all', 0)):
            th = pct(holds, p) if p else -1
            a = agg([r for r in pooled if r['hold_us'] >= th])
            if p != 99:
                for k in ('segs', 'wsegs', 'hold_us', 'nwaited'):
                    a.pop(k)
            print(f'  pooled {lab}: {a}')
        for lab, a in per:
            print(f'  {lab} (per-run >=p99): {a}')
        b = {}
        for r in pooled:
            b.setdefault(min(r['sync_segs'] // 10, 3), []).append(r)
        print('  by live segs (all rolls) n,lo,hi,own_rest_of_own:',
              {f'{10 * k}-{10 * k + 9 if k < 3 else "+"}': (len(v), agg(v)['share_lo'], agg(v)['share_hi'], agg(v)['own_rest_of_own'])
               for k, v in sorted(b.items())})
