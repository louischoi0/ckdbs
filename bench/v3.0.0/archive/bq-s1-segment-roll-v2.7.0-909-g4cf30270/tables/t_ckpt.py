import sys, re, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

RUN = re.compile(r'\[checkpoint\] run: (.*)$')
LOADCELLS = ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']
print('| durability | cell | run | checkpoint runs in the 30 s window | run wall (min / median / max) | pages written (sum) | MiB written (8 KiB pages) | MiB/s | log MiB/s | log_gate_run waits (runs with n>0) | data_park_us (max) |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
tot = {}
for dur in DURS:
    for cell in LOADCELLS:
        for r, d, sl in runs(dur, cell):
            rs = []
            for line in open(sl):
                m = RUN.search(line)
                if not m:
                    continue
                kv = dict(t.split('=', 1) for t in m.group(1).split() if '=' in t)
                rs.append({k: int(v) for k, v in kv.items()})
            w = [x for x in rs if d['m0_mono_ns'] <= x['start_ns'] <= d['m1_mono_ns']]
            if not w:
                print(f'| {dur} | {cell} | r{r} | 0 | | | | | | | |')
                continue
            secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
            pages = sum(x['pages'] for x in w)
            walls = [x['wall_us'] for x in w]
            lg = sum(1 for x in w if x.get('log_gate_run_n', 0) > 0)
            dp = max(x.get('data_park_us', 0) for x in w)
            logm = delta(d, 'wal_append_lsn') / secs / 2**20
            print(f'| {dur} | {cell} | r{r} | {len(w)} | {fmt(min(walls))} / {fmt(statistics.median(walls))} / {fmt(max(walls))} | {pages:,} | {pages * 8 / 1024:.0f} | {pages * 8 / 1024 / secs:.1f} | {logm:.1f} | {lg} of {len(w)} | {fmt(dp)} |')
