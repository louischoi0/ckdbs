import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

print('| durability | cell | run | core 0 drain wait / window | core 0 scheduler stalls over 100 ms outside a poll (sched_hold_out_run_over_100ms, m1 - m0) | rolls in window | longest stall outside a poll (sched_hold_out_run_longest_us, m1) |')
print('|---|---|---|---|---|---|---|')
for dur in DURS:
    for cell in ['c3', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']:
        for r, d, sl in runs(dur, cell):
            secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
            w, _ = rolls(sl, d)
            dr = delta(d, 'load_c0_wal_drain_latch_wait_us') / 1e6
            print(f'| {dur} | {cell} | r{r} | {dr:.1f} s / {secs:.1f} s = {100 * dr / secs:.0f} % | {delta(d, "sched_hold_out_run_over_100ms"):.0f} | {len(w)} | {fmt(float(d["meta_longest"].get("sched_hold_out_run_longest_us", 0)))} |')
