import glob, json, re, math, collections, statistics as st
OUT = '/home/cdkbs/bench-runs/bq/s2/out'
R = OUT + '/relaxed'
ARMS = ['s16', 'base', 's256', 's1024', 'opt3']
SIZE = {'s16': 16, 'base': 64, 's256': 256, 's1024': 1024, 'opt3': 64}
KV = re.compile(r'(\w+)=(\S+)')


def js(d):
    f = glob.glob(d + '/*.json')
    return json.load(open(f[0])) if f else None


def rolls(d):
    out = []
    for lf in glob.glob(d + '/*.s.log'):
        for l in open(lf):
            if '[wal] roll ' in l:
                out.append(dict(KV.findall(l)))
    return out


def med(x):
    return st.median(x) if x else float('nan')


def pct(x, p):
    x = sorted(x)
    if not x:
        return float('nan')
    return x[min(len(x) - 1, max(0, math.ceil(p * len(x)) - 1))]


iso = {}
for cell in ['c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso']:
    j = js(f'{R}/{cell}/base/r1')
    k = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
    iso[cell.replace('-iso', '')] = (j[k]['p99_us'] / 1e3, j[k]['p99.9_us'] / 1e3)
    print('ISO', cell, iso[cell.replace('-iso', '')], j['host_before']['loadavg'], 'moves', j.get('moves'), j.get('meta_longest'))
print()
for cell in ['c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2']:
    k = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
    ip99, ip999 = iso[cell]
    for arm in ARMS:
        rows = []
        for r in (1, 2, 3):
            d = f'{R}/{cell}/{arm}/r{r}'
            j = js(d)
            if not j:
                rows.append(None)
                continue
            s = j[k]
            w0, w1 = j['measure_from_mono_ns'], j['stop_at_mono_ns']
            rl = [x for x in rolls(d) if w0 <= int(x['start_ns']) < w1]
            rows.append((round(s['p99_us'] / 1e3, 2), round(s['p99_us'] / 1e3 / ip99, 1), round(s['p99.9_us'] / 1e3, 1),
                         round(s['max_us'] / 1e6, 2), len(rl), sorted(set(x['core'] for x in rl)), j['errors']))
        p99s = [x[0] for x in rows if x]
        print(cell, arm, 'medp99', med(p99s), 'ratio_med', round(med(p99s) / ip99, 1),
              'p999/iso', [round(x[2] / ip999, 1) for x in rows if x])
        for x in rows:
            print('   ', x)
print()
for arm in ARMS:
    js3 = [js(f'{R}/c3/{arm}/r{r}') for r in (1, 2, 3)]
    v = [j['inserter_rows_per_s'] for j in js3]
    mx = [j['inserter']['max_us'] / 1e6 for j in js3]
    print('C3', arm, [round(x) for x in v], 'med', round(med(v)), 'mean', round(st.mean(v)), 'max_s', [round(m, 2) for m in mx])
print()
F = ['sync_gate_us', 'sync_us', 'body_wait_us', 'header_us', 'rename_us', 'dirsync_us', 'seal_us', 'flush_us', 'body_us', 'create_us']
for arm in ARMS:
    pool = []
    perwin = []
    rate = []
    lograte = []
    syncrate = []
    for cell in ['c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3']:
        for r in (1, 2, 3):
            d = f'{R}/{cell}/{arm}/r{r}'
            j = js(d)
            rl = rolls(d)
            pool += rl
            w0, w1 = j['measure_from_mono_ns'], j['stop_at_mono_ns']
            held = 0
            n = 0
            for x in rl:
                a, b = int(x['start_ns']), int(x['end_ns'])
                held += max(0, min(b, w1) - max(a, w0))
                if w0 <= a < w1:
                    n += 1
            perwin.append(held / 1e9)
            rate.append(n / 30)
            md = j['meta_delta']
            secs = (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9
            lograte.append(md.get('wal_append_lsn', 0) / secs / (SIZE[arm] * 2 ** 20))
            if cell == 'c3':
                syncrate.append(round(md.get('wal_syncs', 0) / secs, 2))
    h = [int(x['hold_us']) / 1e3 for x in pool]
    print(arm, 'n', len(h), 'p50', round(med(h)), 'p90', round(pct(h, .9)), 'max', round(max(h)), 'ms/MiB', round(med(h) / SIZE[arm], 2))
    print('   steps med', {f: round(med([int(x[f]) / 1e3 for x in pool]), 2) for f in F})
    tot = sum(int(x['hold_us']) for x in pool)
    print('   share gate', round(100 * sum(int(x['sync_gate_us']) for x in pool) / tot, 1), 'bw',
          round(100 * sum(int(x['body_wait_us']) for x in pool) / tot, 1))
    waited = [x for x in pool if int(x['body_wait_us']) > 0]
    print('   body', collections.Counter(x['body'] for x in pool), 'bw>0', len(waited),
          'bw-roll hold share', round(100 * sum(int(x['hold_us']) for x in waited) / tot, 1))
    hit = [x for x in pool if int(x['body_wait_us']) == 0]
    print('   bw==0 hold med', round(med([int(x['hold_us']) / 1e3 for x in hit]), 1), 'gate',
          round(med([int(x['sync_gate_us']) / 1e3 for x in hit]), 1), 'n', len(hit))
    wt = [int(x['waited_sync_tail_us']) / 1e3 / int(x['waited_syncs']) for x in pool if int(x.get('waited_syncs', 0)) > 0]
    wt2 = [int(x['waited_sync_tail_us']) / 1e3 for x in pool]
    print('   waited tail per-sync med/p90', round(med(wt), 1), round(pct(wt, .9), 1), ' summed med/p90', round(med(wt2), 1), round(pct(wt2, .9), 1))
    print('   roll-held s/window med', round(med(perwin), 2), 'range', round(min(perwin), 2), round(max(perwin), 2),
          'rolls/s med', round(med(rate), 3), 'mean', round(st.mean(rate), 3), 'lograte/size med', round(med(lograte), 3), 'c3 syncs/s', syncrate)
print()
for f in sorted(glob.glob(OUT + '/mount/*.json')):
    m = json.load(open(f))
    rc = m['recovery']
    print(f.split('/')[-1], 'mount', m['mount_s'], 'ckpt', rc['recovery_checkpoint_us'] / 1e6, 'net',
          round(m['mount_s'] - rc['recovery_checkpoint_us'] / 1e6, 3), 'an+redo', round((rc['recovery_analysis_us'] + rc['recovery_redo_us']) / 1e6, 3),
          'longest_ins', m['longest_insert_s'], 'rolls', m['rolls'])
