import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bp_lib import *
for dur in ('group', 'strict'):
    for side in ('a', 'b'):
        for r in (1, 2, 3):
            d = commit(dur, side, r)
            ml = d.get('meta_longest', {})
            md = d['meta_delta']
            print(dur, side, r, 'max', us(d['max_us']), 'fg_longest_poll', us(md.get('sched_foreground_longest_poll_us')), 'polls>10ms', md.get('sched_foreground_polls_over_10ms'), '>100ms', md.get('sched_foreground_polls_over_100ms'),
                  'sys_longest', us(md.get('sched_system_longest_poll_us')), 'ml', {k: us(v) for k, v in ml.items()}, 'ckpt_runs', md.get('checkpoint_runs'), 'unpolled>100', md.get('sched_unpolled_over_100ms'),
                  'syncs_writer', md.get('contention_syncs_writer'), 'ring_full', md.get('wal_ring_full'))
for r in (1, 2, 3):
    d, run, arms = bn(r)
    print('bn', r, run['idle_cpu_us_per_s'])
