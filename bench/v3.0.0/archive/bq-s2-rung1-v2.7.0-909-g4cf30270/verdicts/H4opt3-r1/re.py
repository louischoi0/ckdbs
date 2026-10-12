"""Independent recompute of H4opt3 verdict figures from raw BQ-S2 files."""
import glob, json, math, re, statistics as st, sys
from fractions import Fraction
OUT = '/home/cdkbs/bench-runs/bq/s2/out/relaxed'
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


def load(d):
    js = glob.glob(d + '/*.json')
    if not js:
        return None, None
    j = json.load(open(js[0]))
    rl = []
    for f in glob.glob(d + '/*.s.log'):
        for line in open(f):
            m = ROLL.search(line)
            if m:
                rl.append(kv(m.group(1)))
    return j, rl


def nrq(cs, q):
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1e6 for c in cs for s in c['slow'])
    drop = sum(c['slow_dropped'] for c in cs)
    if n == 0:
        return None
    k = max(1, math.ceil(Fraction(str(q)) * n / 100))
    fast = n - len(slow)
    if drop:
        return 'DROP'
    return slow[k - fast - 1] if k > fast else '<1'


def pct(xs, p):
    xs = sorted(xs)
    return xs[max(1, math.ceil(p * len(xs) / 100)) - 1] if xs else None


iso = {}
for cell in ('c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso'):
    j, _ = load(f'{OUT}/{cell}/base/r1')
    key = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
    iso[cell] = (j[key]['p99_us'] / 1e3, j[key]['p99.9_us'] / 1e3)
    role = 'reader' if cell.startswith('c1') else 'writer'
    cs = [c for c in j['clients'] if c['role'] == role]
    print('ISO', cell, iso[cell], 'own', nrq(cs, 99), nrq(cs, 99.9), 'load', j['host_before']['loadavg'],
          'ckpt/carve', {k: v for k, v in j.get('meta_longest', {}).items() if 'checkpoint' in k or 'carve' in k})

meets_any = []
ISOK = {'c1-segregated-no0': 'c1-iso-segregated-no0', 'c1-batch0': 'c1-iso-batch0',
        'c1-spread': 'c1-iso-spread', 'c2': 'c2-iso'}
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
    isokey = ISOK.get(cell)
    for arm in ('base', 'opt3', 's16', 's256', 's1024'):
        pool, out = [], []
        for d in sorted(glob.glob(f'{OUT}/{cell}/{arm}/r*')):
            j, rl = load(d)
            if j is None:
                continue
            lo, hi = j['measure_from_mono_ns'], j['stop_at_mono_ns']
            win = [r for r in rl if r['start_ns'] >= lo and r['end_ns'] <= hi]
            pool += win
            rc = {r['core'] for r in win}
            role = 'reader' if j['readers'] else 'writer'
            cs = [c for c in j['clients'] if c['role'] == role and int(c['core']) not in rc]
            ic = {int(c['core']) for c in j['clients'] if c['role'] == 'inserter'}
            cs2 = [c for c in j['clients'] if c['role'] == role and int(c['core']) not in ic]
            p99, p999 = nrq(cs, 99), nrq(cs, 99.9)
            md = j['meta_delta']
            rec = dict(r=d[-2:], rolls=len(win), mrolls=md.get('wal_rolls', 0), rc=sorted(rc),
                       nbc=sorted({int(c['core']) for c in cs}), n=sum(c['ok'] for c in cs),
                       p99=p99, p999=p999, i99=nrq(cs2, 99) if cs2 else None,
                       i999=nrq(cs2, 99.9) if cs2 else None,
                       rows=round(j['inserter_rows_per_s'] or 0),
                       lbps=round((j['log_bytes_per_s'] or 0) / 2**20, 1),
                       err=j['errors'], ref=j['refusals'], mv=j['moves_in_window'],
                       hold=round(sum(r['hold_us'] for r in win) / 1e6, 2),
                       load=j['host_before']['loadavg'].split()[:3],
                       bw=sum(r['body'] == 'waited' for r in win), bi=sum(r['body'] == 'inline' for r in win),
                       bh=sum(r['body'] == 'hit' for r in win))
            if isokey and isinstance(p99, float) and isinstance(p999, float):
                rec['x99'] = round(p99 / iso[isokey][0])
                rec['x999'] = round(p999 / iso[isokey][1])
                if p99 <= 2 * iso[isokey][0] and p999 <= 5 * iso[isokey][1]:
                    meets_any.append((cell, arm, d))
            elif isokey:
                meets_any.append((cell, arm, d, 'nonfloat', p99, p999))
            out.append(rec)
        if not out:
            continue
        print(f'\n== {cell} {arm}')
        for r in out:
            print('  ', {k: (round(v, 1) if isinstance(v, float) else v) for k, v in r.items()})
        if arm in ('base', 'opt3'):
            h = [r['hold_us'] / 1e3 for r in pool]
            tot = sum(r['hold_us'] for r in pool)
            g = sum(r['sync_gate_us'] for r in pool)
            wt = [r['waited_sync_tail_us'] / 1e3 for r in pool if r['waited_syncs'] > 0]
            hd = [r['header_us'] / 1e3 for r in pool]
            cr = sum(r['create_us'] for r in pool)
            bwu = sum(r['body_wait_us'] for r in pool)
            missed = sum(r['hold_us'] for r in pool if r['body'] != 'hit')
            print(f'   POOL rolls={len(h)} p50={pct(h,50):.1f} p90={pct(h,90):.1f} p99={pct(h,99):.1f} '
                  f'max={max(h):.1f} mean={st.mean(h):.1f} gate%={100*g/tot:.1f} '
                  f'hdr%={100*sum(r["header_us"] for r in pool)/tot:.1f} hdr p50={pct(hd,50):.2f} '
                  f'p99={pct(hd,99):.1f} create%={100*cr/tot:.1f} bodywait%={100*bwu/tot:.2f} '
                  f'missed%={100*missed/tot:.1f} wtail p50={pct(wt,50) if wt else None} '
                  f'p99={pct(wt,99) if wt else None}')
            for d in sorted(glob.glob(f'{OUT}/{cell}/{arm}/r*')):
                j, rl = load(d)
                lo, hi = j['measure_from_mono_ns'], j['stop_at_mono_ns']
                w = [r for r in rl if r['start_ns'] >= lo and r['end_ns'] <= hi]
                tot = sum(r['hold_us'] for r in w)
                wt = [r['waited_sync_tail_us'] / 1e3 for r in w if r['waited_syncs'] > 0]
                hd = [r['header_us'] / 1e3 for r in w]
                thirds = []
                for k in range(3):
                    a, b = lo + k * (hi - lo) // 3, lo + (k + 1) * (hi - lo) // 3
                    hh = [r['hold_us'] / 1e3 for r in w if a <= r['start_ns'] < b]
                    thirds.append((round(st.median(hh), 1) if hh else None, round(max(hh)) if hh else None))
                first150 = min([(r['start_ns'] - lo) / 1e9 for r in w if r['hold_us'] > 150000], default=None)
                print(f'     {d[-2:]} gate%={100*sum(r["sync_gate_us"] for r in w)/tot:.1f} '
                      f'hdr%={100*sum(r["header_us"] for r in w)/tot:.1f} '
                      f'create%={100*sum(r["create_us"] for r in w)/tot:.1f} hdr p50={pct(hd,50):.2f} '
                      f'p99={pct(hd,99):.1f} wtail p50={pct(wt,50) if wt else None} '
                      f'p99={pct(wt,99) if wt else None} thirds={thirds} '
                      f'first>150ms@{round(first150,1) if first150 is not None else None}')
print('\nMEETS BAR (relaxed, both terms):', meets_any)
