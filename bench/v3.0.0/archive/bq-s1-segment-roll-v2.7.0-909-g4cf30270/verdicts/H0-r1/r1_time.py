# Where in the window do group C2's >= p99 samples fall, and what do they line up with.
import json, glob, re
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
ROLL = re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')
for dur, cell in (('group', 'c2'), ('group', 'c2-iso'), ('relaxed', 'c2')):
    for r in (1, 2, 3):
        f = glob.glob(f'{B}/{dur}/{cell}/r{r}/*.json')[0]
        d = json.load(open(f))
        p99 = d['writer_from_schedule']['p99_us'] * 1000
        mf = d['measure_from_mono_ns']
        samp = sorted((s[0], s[1]) for c in d['clients'] if c['role'] == 'writer' for s in c['slow'])
        top = [s for s in samp if s[1] >= p99]
        # histogram by second of due time
        hist = {}
        for due, late in top:
            sec = int((due - mf) / 1e9)
            hist[sec] = hist.get(sec, 0) + 1
        allh = {}
        for due, late in samp:
            sec = int((due - mf) / 1e9)
            allh[sec] = allh.get(sec, 0) + 1
        per = {}
        for c in d['clients']:
            if c['role'] != 'writer':
                continue
            per[c['index']] = sum(1 for s in c['slow'] if s[1] >= p99)
        rolls = [tuple(int(x) for x in m.groups()) for line in open(f.replace('.json', '.s.log'), errors='replace')
                 for m in [ROLL.search(line)] if m]
        rsec = sorted(int((x[2] - mf) / 1e9) for x in rolls if mf <= x[2] <= d['stop_at_mono_ns'])
        # longest-late episodes: peak late per second
        peak = {}
        for due, late in samp:
            sec = int((due - mf) / 1e9)
            peak[sec] = max(peak.get(sec, 0), late // 1_000_000)
        print(f'{dur} {cell} r{r}: p99 {p99/1e6:.1f} ms  top n {len(top)} by sec {dict(sorted(hist.items()))}')
        print(f'   per-writer top {per}; slow(>=1ms) per sec {dict(sorted(allh.items()))}')
        print(f'   peak late ms per sec {dict(sorted(peak.items()))}')
        print(f'   roll seconds {rsec}')
        ml = d.get('meta_longest')
        print(f'   meta_longest {ml}')
