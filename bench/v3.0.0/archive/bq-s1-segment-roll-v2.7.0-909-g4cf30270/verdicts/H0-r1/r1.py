# Independent recomputation of H0 from raw bl-mixed JSON + server logs.
import json, glob, re, bisect, statistics as st
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
ROLL = re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')


def load(dur, cell, r):
    f = glob.glob(f'{B}/{dur}/{cell}/r{r}/*.json')[0]
    d = json.load(open(f))
    rolls = []
    for line in open(f.replace('.json', '.s.log'), errors='replace'):
        m = ROLL.search(line)
        if m:
            rolls.append(tuple(int(x) for x in m.groups()))
    return d, rolls


def delta(d, k):
    a = d['meta_m0'].get(k)
    b = d['meta_m1'].get(k)
    return (b or 0) - (a or 0)


def run(dur, cell, r, iso=False):
    d, rolls = load(dur, cell, r)
    m0, m1 = d['m0_mono_ns'], d['m1_mono_ns']
    win = [x for x in rolls if m0 <= x[2] <= m1]
    out = dict(n_rolls=len(win), wal_rolls=delta(d, 'wal_rolls'), all_rolls=len(rolls))
    out['max_hold'] = max((x[4] for x in win), default=0)
    out['roll_cores'] = sorted({x[1] for x in win})
    ins = [c for c in d['clients'] if c['role'] == 'inserter']
    out['ins_cores'] = [(c['core'], c['core_end']) for c in ins]
    bulk = set(out['roll_cores'])
    nb = [k for k in range(d['cores']) if k not in bulk]
    out['nonbulk'] = nb
    cov = sum(delta(d, f'load_c{k}_wal_roll_blocked_wait_us') for k in nb)
    strm = sum(delta(d, f'load_c{k}_waited_wal_stream_us') for k in nb)
    keys = set(d['meta_m0']) | set(d['meta_m1'])
    allw = sum(delta(d, k) for k in keys for c in nb
               if k.startswith(f'load_c{c}_waited_') and k.endswith('_us'))
    out['cov_ms'] = cov / 1000
    out['stream_ms'] = strm / 1000
    out['all_ms'] = allw / 1000
    out['share_stream'] = round(100 * cov / strm, 1) if strm else None
    out['share_all'] = round(100 * cov / allw, 1) if allw else None
    out['percore'] = {k: (round(delta(d, f'load_c{k}_wal_roll_blocked_wait_us') / 1000),
                          round(delta(d, f'load_c{k}_waited_wal_stream_us') / 1000))
                      for k in range(d['cores'])}
    role = 'reader' if d['readers'] else 'writer'
    out['role'] = role
    fs = d['read_from_schedule'] if role == 'reader' else d['writer_from_schedule']
    out['p99'] = fs.get('p99_us')
    out['p999'] = fs.get('p99.9_us')
    if not iso and out['p99']:
        thr = out['p99'] * 1000
        samp = [s for c in d['clients'] if c['role'] == role for s in c['slow'] if s[1] >= thr]
        ends = sorted(x[3] for x in win)
        spans = [(x[2], x[3]) for x in win]
        near = ovl = 0
        for due, late in samp:
            e = due + late
            i = bisect.bisect_right(ends, e) - 1
            if i >= 0 and e - ends[i] <= 20_000_000:
                near += 1
            if any(a <= e and due <= b for a, b in spans):
                ovl += 1
        wl = m1 - m0
        out['line'] = dict(n=len(samp), near=near, ovl=ovl,
                           chance=round(100 * len(win) * 20e6 / wl, 2),
                           cover=round(100 * sum(b - a for a, b in spans) / wl, 2))
    out['wal_syncs'] = delta(d, 'wal_syncs')
    out['moves'] = d.get('moves_in_window')
    out['svc'] = (d['read_service'] if role == 'reader' else d['writer_service'])
    out['md_holdmax'] = d['meta_delta'].get('wal_roll_hold_max_us')
    return out


cells = [('c1-segregated-no0', 'c1-iso-segregated-no0'), ('c1-batch0', 'c1-iso-batch0'),
         ('c1-spread', 'c1-iso-spread'), ('c2', 'c2-iso')]
for dur in ('relaxed', 'group'):
    for cell, isoc in cells:
        rs = [run(dur, cell, r) for r in (1, 2, 3)]
        iso = [run(dur, isoc, r, iso=True) for r in (1, 2, 3)]
        print(f'== {dur} {cell}')
        for i, o in enumerate(rs):
            print(f"  r{i+1}: rolls {o['n_rolls']}/{o['wal_rolls']} (log {o['all_rolls']}) maxhold {o['max_hold']} "
                  f"md_holdmax {o['md_holdmax']} rollcores {o['roll_cores']} ins {o['ins_cores']} nb {o['nonbulk']} moves {o['moves']}")
            print(f"      cov {o['cov_ms']:.1f} ms stream {o['stream_ms']:.1f} all {o['all_ms']:.1f} "
                  f"share_stream {o['share_stream']} share_all {o['share_all']} percore(cov,stream) {o['percore']}")
            print(f"      p99 {o['p99']} p99.9 {o['p999']} iso p99 {iso[i]['p99']} ratio {o['p99'] / iso[i]['p99']:.2f} "
                  f"line {o.get('line')}")
            print(f"      syncs {o['wal_syncs']} iso syncs {iso[i]['wal_syncs']} svc p50 {o['svc'].get('p50_us')} "
                  f"p99 {o['svc'].get('p99_us')} iso svc p50 {iso[i]['svc'].get('p50_us')} p99 {iso[i]['svc'].get('p99_us')}")
        cp = [o['p99'] for o in rs]
        ip = [o['p99'] for o in iso]
        print(f"  p99 med/med {st.median(cp) / st.median(ip):.2f} range {min(cp) / max(ip):.2f}-{max(cp) / min(ip):.2f} "
              f"spread cell {max(cp) - min(cp):.1f} iso {max(ip) - min(ip):.1f} pooled rolls {sum(o['n_rolls'] for o in rs)}")
