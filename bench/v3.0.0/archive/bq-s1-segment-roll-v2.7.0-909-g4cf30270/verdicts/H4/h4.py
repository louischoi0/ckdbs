"""H4 recomputation from raw files (BQ-S1 data at 4cf30270).

usage: python3 -I h4.py <out_dir>

Per cell, per durability, per run: the rolls whose [start_ns, end_ns] lies in
the driver's [measure_from_mono_ns, stop_at_mono_ns] window (same rule as
analyze_s1.py) and, as a cross-check, every roll in the log and the
meta_delta counters (window m0..m1).

Step d's own time for a roll is body_wait_us + body_us (file_log_device.cpp
CreateSegmentTimed: body_wait_ns is the wait on prepare_mutex_, body_ns the
inline BuildBody). The criterion's "missed rolls' summed hold" is the whole
hold of every roll whose body is not 'hit'.
"""
import glob
import json
import os
import re
import statistics
import sys

ROLL = re.compile(r'\[wal\] roll (.*)$')


def kv(t):
    o = {}
    for tok in t.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                o[k] = int(v)
            except ValueError:
                o[k] = v
    return o


def rolls(log, lo, hi):
    allr, win = [], []
    for line in open(log):
        m = ROLL.search(line)
        if not m:
            continue
        r = kv(m.group(1))
        allr.append(r)
        if lo <= r['start_ns'] and r['end_ns'] <= hi:
            win.append(r)
    return allr, win


def stepd(r):
    return r['body_wait_us'] + r['body_us']


def summarize(rs):
    H = sum(r['hold_us'] for r in rs)
    miss = [r for r in rs if r['body'] != 'hit']
    Hm = sum(r['hold_us'] for r in miss)
    D = sum(stepd(r) for r in rs)
    cnt = {b: sum(1 for r in rs if r['body'] == b) for b in ('hit', 'waited', 'inline')}
    return dict(n=len(rs), hold_sum=H, miss_hold=Hm,
                miss_share=(100 * Hm / H if H else None),
                stepd_sum=D, stepd_share=(100 * D / H if H else None),
                counts=cnt,
                other=sorted(set(r['body'] for r in rs) - {'hit', 'waited', 'inline'}),
                miss_detail=[(r['seg'], r['body'], r['body_wait_us'], r['body_us'],
                              r['hold_us'], r['sync_gate_us']) for r in miss],
                stepd_max=max((stepd(r) for r in rs), default=0),
                hold_p50=(statistics.median([r['hold_us'] for r in rs]) if rs else None))


def runs(cell_dir):
    out = []
    for js in sorted(glob.glob(cell_dir + '/r*/*.json')):
        d = json.load(open(js))
        d['_log'] = js[:-5] + '.s.log'
        d['_run'] = js.split('/')[-2]
        out.append(d)
    return out


def f(x, nd=1):
    return 'None' if x is None else f'{x:.{nd}f}'


out = sys.argv[1]
res = {}
for dur in ('relaxed', 'group'):
    for cell in sorted(os.listdir(f'{out}/bq/{dur}')):
        for d in runs(f'{out}/bq/{dur}/{cell}'):
            allr, win = rolls(d['_log'], d['measure_from_mono_ns'], d['stop_at_mono_ns'])
            sched = (d.get('read_from_schedule') or d.get('writer_from_schedule') or {})
            res[(dur, cell, d['_run'])] = (summarize(win), summarize(allr), d['meta_delta'], sched, d)

print('# H4 criterion cell: C3 (rolls in the measurement window)')
for dur in ('relaxed', 'group'):
    shares, dshares, wins = [], [], []
    for run in ('r1', 'r2', 'r3'):
        s, sa, md, _, d = res[(dur, 'c3', run)]
        wins.append(s)
        shares.append(s['miss_share'])
        dshares.append(s['stepd_share'])
        meta_d = md.get('wal_roll_body_wait_us', 0) + md.get('wal_roll_body_us', 0)
        print(f'{dur:7} c3 {run}: rolls={s["n"]} (log {sa["n"]}, meta {md.get("wal_rolls")}) '
              f'counts={s["counts"]} other={s["other"]} meta hit/waited/inline='
              f'{md.get("wal_roll_body_hit")}/{md.get("wal_roll_body_waited")}/{md.get("wal_roll_body_inline")} '
              f'hold_sum={s["hold_sum"]}us missed_hold={s["miss_hold"]}us share={f(s["miss_share"], 2)}% '
              f'stepd_sum={s["stepd_sum"]}us (meta {meta_d}) stepd_share={f(s["stepd_share"], 3)}% '
              f'| all-log: missed share={f(sa["miss_share"], 2)}% counts={sa["counts"]}')
        for m in s['miss_detail']:
            print(f'{"":22}miss seg={m[0]} body={m[1]} body_wait_us={m[2]} body_us={m[3]} '
                  f'hold_us={m[4]} sync_gate_us={m[5]}')
        for m in sa['miss_detail']:
            if m not in s['miss_detail']:
                print(f'{"":22}(outside window) miss seg={m[0]} body={m[1]} body_wait_us={m[2]} '
                      f'body_us={m[3]} hold_us={m[4]}')
    vs = [x for x in shares if x is not None]
    H = sum(s['hold_sum'] for s in wins)
    Hm = sum(s['miss_hold'] for s in wins)
    print(f'{dur:7} c3 per-run missed-hold share: {[f(x, 2) for x in shares]} '
          f'mean={f(statistics.mean(vs), 2)} min={f(min(vs), 2)} max={f(max(vs), 2)} '
          f'spread={f(max(vs) - min(vs), 2)} pp; pooled={f(100 * Hm / H, 2)}%; '
          f'step-d own share {[f(x, 3) for x in dshares]}')

print('\n# Body outcomes in every cell (window)')
for (dur, cell, run), (s, sa, md, sched, d) in sorted(res.items()):
    if s['n'] == 0:
        continue
    print(f'{dur:7} {cell:20} {run}: rolls={s["n"]} {s["counts"]} '
          f'missed_share={f(s["miss_share"], 2)}% stepd_sum={s["stepd_sum"]}us '
          f'stepd_max={s["stepd_max"]}us stepd_share={f(s["stepd_share"], 3)}% hold_p50={s["hold_p50"]} '
          f'waits={[(m[2], m[3]) for m in s["miss_detail"]]}')

print('\n# Upper bound: C1/C2 non-bulk schedule p99/p99.9 with every step-d microsecond removed')
iso_of = {'c1-batch0': 'c1-iso-batch0', 'c1-segregated-no0': 'c1-iso-segregated-no0',
          'c1-spread': 'c1-iso-spread', 'c2': 'c2-iso'}
for dur in ('relaxed', 'group'):
    for cell, iso in iso_of.items():
        iso99 = [res[(dur, iso, r)][3]['p99_us'] for r in ('r1', 'r2', 'r3')]
        iso999 = [res[(dur, iso, r)][3]['p99.9_us'] for r in ('r1', 'r2', 'r3')]
        bar99 = 2 * max(iso99)
        bar999 = 5 * max(iso999)
        for r in ('r1', 'r2', 'r3'):
            s, sa, md, sched, d = res[(dur, cell, r)]
            p99, p999 = sched['p99_us'], sched['p99.9_us']
            lb99 = p99 - s['stepd_sum']
            lb999 = p999 - s['stepd_sum']
            print(f'{dur:7} {cell:18} {r}: p99={p99} p99.9={p999} stepd_total={s["stepd_sum"]} '
                  f'stepd_max_roll={s["stepd_max"]} hold_p50={s["hold_p50"]} -> p99>={f(lb99)} vs 2x iso '
                  f'(iso max {max(iso99)})={f(bar99)} meets={lb99 <= bar99}; p99.9>={f(lb999)} vs 5x iso '
                  f'(iso max {max(iso999)})={f(bar999)} meets={lb999 <= bar999}')
