import json, glob, re, sys, os
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'

def meta(m):
    if isinstance(m, dict):
        return m
    out = {}
    for tok in str(m).split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try: out[k] = float(v)
            except ValueError: out[k] = v
    return out

def rolls(slog, a, b):
    rs = []
    for line in open(slog, errors='replace'):
        if '[wal] roll' not in line: continue
        d = dict(re.findall(r'(\w+)=(\S+)', line))
        s, e = int(d['start_ns']), int(d['end_ns'])
        if e > a and s < b:
            rs.append((s, e, d.get('core')))
    return rs

for dur in ('relaxed', 'group'):
    for cell in sorted(os.listdir(f'{B}/{dur}')):
        for r in ('r1', 'r2', 'r3'):
            js = glob.glob(f'{B}/{dur}/{cell}/{r}/*.json')
            if not js: continue
            j = json.load(open(js[0]))
            if 'runs' in j: j = j['runs'][0]
            m0, m1 = meta(j.get('meta_m0')), meta(j.get('meta_m1'))
            a, b = j.get('m0_mono_ns'), j.get('m1_mono_ns')
            sl = glob.glob(f'{B}/{dur}/{cell}/{r}/*.s.log')
            rl = rolls(sl[0], a, b) if sl and a else []
            hold = sum(min(e, b) - max(s, a) for s, e, _ in rl) / 1e6
            cores = {}
            for c, _ in [(x[2], 0) for x in rl]: cores[c] = cores.get(c, 0) + 1
            row = []
            for k in range(4):
                g = lambda n: (float(m1.get(f'load_c{k}_{n}', 0)) - float(m0.get(f'load_c{k}_{n}', 0))) / 1000
                row.append('c%d d%.0f s%.0f rc%.0f' % (k, g('wal_drain_latch_wait_us'), g('waited_wal_stream_us'), g('wal_roll_blocked_wait_us')))
            print(dur, cell, r, 'rolls', len(rl), 'hold_ms %.0f' % hold, cores, ' | '.join(row))
            if r == 'r1' and cell == 'c1-segregated-no0' and dur == 'relaxed':
                print(' keys', [k for k in j.keys()][:40])
