import json
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/priors.json'))
r = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/restart.json'))
K = ('sched_system_polled_us', 'sched_system_polls', 'sched_system_consumed_us', 'sched_system_longest_poll_us', 'sched_wall_us', 'sched_idle_block_us')
for k, ph in (('B', 'after_mount'), ('B', 'after_statements'), ('B', 'after_idle'), ('A', 'after_statements'), ('A', 'after_idle')):
    print(k, ph, {x: d[k][ph].get(x) for x in K})
for ph in ('after_mount', 'after_statements', 'after_idle'):
    print('restart', ph, {x: r[ph].get(x) for x in K})
