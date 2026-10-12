import json, glob
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
for dur in ('relaxed', 'group'):
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2', 'c2-iso'):
        out = []
        for r in ('r1', 'r2', 'r3'):
            j = json.load(open(glob.glob(f'{B}/{dur}/{cell}/{r}/*.json')[0]))
            key = 'writer_from_schedule' if cell.startswith('c2') else 'read_from_schedule'
            s = j[key]
            out.append('%s/%s' % (s.get('p99_us'), s.get('p99.9_us')))
        print(dur, cell, out)
# roll hold distribution at group
import re
for cell in ('c1-segregated-no0', 'c2'):
    hs = []
    for r in ('r1', 'r2', 'r3'):
        sl = glob.glob(f'{B}/group/{cell}/{r}/*.s.log')[0]
        for line in open(sl, errors='replace'):
            if '[wal] roll' in line:
                m = re.search(r'hold_us=(\d+)', line)
                if m: hs.append(int(m.group(1)) / 1000)
    hs.sort()
    print('group', cell, 'n', len(hs), 'p50', hs[len(hs)//2], 'frac>=10ms', sum(h >= 10 for h in hs) / len(hs))
