import json, glob, re
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread'):
    for r in ('r1', 'r2', 'r3'):
        j = json.load(open(glob.glob(f'{B}/group/{cell}/{r}/*.json')[0]))
        a, b = j['m0_mono_ns'], j['m1_mono_ns']
        sl = glob.glob(f'{B}/group/{cell}/{r}/*.s.log')[0]
        rl = []
        for line in open(sl, errors='replace'):
            if '[wal] roll' in line:
                d = dict(re.findall(r'(\w+)=(\S+)', line))
                rl.append((int(d['start_ns']), int(d['end_ns'])))
        samples = []
        n = 0
        for c in j['clients']:
            if c.get('role') != 'reader': continue
            n += c.get('n', 0) or 0
            for due, lat in c.get('slow', []):
                samples.append((due, lat))
        samples.sort(key=lambda x: -x[1])
        tot = sum(c.get('n', 0) or 0 for c in j['clients'] if c.get('role') == 'reader')
        k = max(1, len(samples))
        def aligned(due, lat):
            end = due + lat
            for s, e in rl:
                if due < e and end > s and e - 5e6 <= end <= e + 30e6:
                    return True
            return False
        def overlap(due, lat):
            end = due + lat
            return any(due < e and end > s for s, e in rl)
        top = samples[:20]
        ge1 = [x for x in samples if x[1] >= 1e6]
        lt10 = [x for x in ge1 if x[1] < 10e6]
        print(cell, r, 'slow>=1ms', len(ge1), 'of', [c.get('n') for c in j['clients'] if c.get('role') == 'reader'][:1],
              '| 1-10ms aligned', sum(aligned(*x) for x in lt10), '/', len(lt10), 'overlap', sum(overlap(*x) for x in lt10),
              '| top20 aligned', sum(aligned(*x) for x in top), 'top20 ms', [round(x[1] / 1e6, 1) for x in top[:8]])
