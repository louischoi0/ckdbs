import re, glob, json
SEG = 64 << 20


def kv(s):
    return dict(re.findall(r'(\w+)=(\S+)', s))


def g(m, k):
    if isinstance(m, dict):
        if k in m:
            return m[k]
        for v in m.values():
            x = g(v, k)
            if x is not None:
                return x
    return None


for dur in ('relaxed', 'group'):
    for r in (1, 2, 3):
        d = f'/home/cdkbs/bench-runs/bq/out-4cf30270/bq/{dur}/c3/r{r}'
        log = glob.glob(d + '/*.s.log')[0]
        print(f'== {dur} r{r}')
        per = {}
        t0 = None
        pend = None
        lines = open(log).read().splitlines()
        for ln in lines:
            if 'anchor published' in ln:
                pend = kv(ln)
            elif '[checkpoint] run:' in ln:
                k = kv(ln)
                a = pend
                pend = None
                tag = '(idle)' if int(k['steps']) <= 1 else ''
                st = int(k['start_ns'])
                if t0 is None and int(k['steps']) > 1:
                    t0 = st
                c = int(a['core'])
                per[c] = int(a['redo_start'])
                fold = min(per.values())
                print(f" t={(st-(t0 or st))/1e9:7.2f} core={c} wall={int(k['wall_us'])/1e3:6.0f}ms dirty={k['dirty_at_start']:>6} parks={k['gate_parks']:>3} lgr={k['log_gate_run_n']} redo={int(a['redo_start'])/SEG:6.2f} begin={int(a['checkpoint_lsn'])/SEG:6.2f} fold={fold/SEG:6.2f} per={[round(per[x]/SEG,2) for x in sorted(per)]} pubsync={k['publish_sync_n']} dpark={int(k['data_park_us'])/1e3:.0f}ms {tag}")
        rolls = [kv(l) for l in lines if '[wal] roll' in l]
        if rolls:
            print(' rolls', len(rolls), 'last seg', rolls[-1]['seg'],
                  'sync_segs', [x['sync_segs'] for x in rolls][::6] + [rolls[-1]['sync_segs']],
                  'rest_us', [x['sync_rest_us'] for x in rolls][::6] + [rolls[-1]['sync_rest_us']],
                  'waited_segs last', rolls[-1]['waited_sync_segs'])
        other = [l for l in lines if re.search(r'recycl|detach|remov|unlink', l, re.I)]
        print(' recycle-ish lines:', len(other))
        for l in other[:5]:
            print('  ', l[:200])
        j = json.load(open(glob.glob(d + '/*.json')[0]))
        m0, m1 = j.get('meta_m0', {}), j.get('meta_m1', {})
        for k in ('checkpoint_runs', 'wal_segments_removed', 'wal_append_lsn'):
            print(f'  {k}: {g(m0,k)} -> {g(m1,k)}')
