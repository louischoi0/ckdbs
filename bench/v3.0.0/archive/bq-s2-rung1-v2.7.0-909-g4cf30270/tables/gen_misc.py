import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


def trio(js, f):
    return [f(j) for j in js]


def fmt3(v, fm):
    return ' / '.join(fm(x) for x in v)


# ---- delta vs BQ-S1 base
P('**The delta against BQ-S1\'s base: the same binary (`4cf30270`, sha256 `024ef4dc...`), the same cells, 3 runs each; the ratio is S2 median ÷ S1 median**')
P()
P('| cell | figure | BQ-S1 base r1 / r2 / r3 | BQ-S2 base r1 / r2 / r3 | S1 spread | S2 spread | median ratio S2 ÷ S1 |')
P('|---|---|---|---|---|---|---|')
for cell in CELLS + ['c3']:
    s1 = [load(S1, 'relaxed', cell, None, r) for r in (1, 2, 3)]
    s2 = [load(S2, 'relaxed', cell, 'base', r) for r in (1, 2, 3)]
    figs = []
    if cell != 'c3':
        k = 'read_from_schedule' if cell != 'c2' else 'writer_from_schedule'
        for p, nm in (('p50_us', 'schedule p50'), ('p99_us', 'schedule p99'), ('p99.9_us', 'schedule p99.9'), ('max_us', 'schedule max')):
            figs.append((nm, lambda j, k=k, p=p: j[k][p], us))
    figs.append(('hold p50 (rolls of the run)', lambda j: nr([x['hold_us'] for x in inwin(j)], 50), us))
    figs.append(('hold p99 (rolls of the run)', lambda j: nr([x['hold_us'] for x in inwin(j)], 99), us))
    figs.append(('rolls in window', lambda j: len(inwin(j)), lambda x: f'{x:.0f}'))
    figs.append(('summed hold per 30 s', lambda j: sum(x['hold_us'] for x in inwin(j)) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30, us))
    figs.append(('bulk writer rows/s', lambda j: j['inserter_rows_per_s'], lambda x: f'{x:,.0f} rows/s'))
    for nm, f, fm in figs:
        a, b = trio(s1, f), trio(s2, f)
        P(f'| {cell} | {nm} | {fmt3(a, fm)} | {fmt3(b, fm)} | {fm(max(a)-min(a))} | {fm(max(b)-min(b))} | {med(b)/med(a):.2f}x |')
P()

# ---- wait accounting
P('**Where the waits are, summed over the four cores per 30 s window (mean of the arm\'s runs; `SHOW META` `contention_*` deltas)**')
P()
P('The stream latch is the one a roll holds. "Blocked by a roll" is the part of the stream-latch wait the roll\'s hold covered; "drain" is core 0\'s group-commit drain; '
  'the sync gate is the wait on the WAL writer\'s sync in flight. Other latches are the ones this workload moves at all.')
P()
P('| cell group | arm | runs | roll hold, summed | stream-latch wait | of which covered by a roll hold | drain\'s share of it | sync-gate wait | window latch | frame-table | lock partition |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
GR = [('C1 (3 placements)', C1), ('C2', ['c2']), ('C3', ['c3'])]
for gname, cells in GR:
    for arm in ARMS:
        js = [load(S2, 'relaxed', c, arm, r) for c in cells for r in (1, 2, 3)]
        w = lambda k: st.mean(delta(j, k) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30 for j in js) / 1e6
        hold = st.mean(sum(x['hold_us'] for x in inwin(j)) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30 for j in js) / 1e6
        sw = w('contention_wal_stream_wait_us')
        rb = w('contention_wal_roll_blocked_wait_us')
        dr = w('contention_wal_drain_latch_wait_us')
        P(f'| {gname} | {arm} | {len(js)} | {hold:.1f} s | {sw:.1f} s | {rb:.1f} s ({100*rb/sw if sw else 0:.0f} %) | {dr:.1f} s ({100*dr/sw if sw else 0:.0f} %) | '
          f'{w("contention_wal_sync_gate_wait_us"):.1f} s | {w("contention_window_wait_us"):.2f} s | {w("contention_frame_table_wait_us"):.2f} s | {w("contention_lock_partition_wait_us"):.3f} s |')
P()
P('The same for BQ-S1\'s base, for the same groups:')
P()
P('| cell group | arm | runs | roll hold, summed | stream-latch wait | of which covered by a roll hold | drain\'s share of it | sync-gate wait | window latch | frame-table | lock partition |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
for gname, cells in GR:
    js = [load(S1, 'relaxed', c, None, r) for c in cells for r in (1, 2, 3)]
    w = lambda k: st.mean(delta(j, k) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30 for j in js) / 1e6
    hold = st.mean(sum(x['hold_us'] for x in inwin(j)) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30 for j in js) / 1e6
    sw = w('contention_wal_stream_wait_us')
    rb = w('contention_wal_roll_blocked_wait_us')
    dr = w('contention_wal_drain_latch_wait_us')
    P(f'| {gname} | base (BQ-S1) | {len(js)} | {hold:.1f} s | {sw:.1f} s | {rb:.1f} s ({100*rb/sw if sw else 0:.0f} %) | {dr:.1f} s ({100*dr/sw if sw else 0:.0f} %) | '
      f'{w("contention_wal_sync_gate_wait_us"):.1f} s | {w("contention_window_wait_us"):.2f} s | {w("contention_frame_table_wait_us"):.2f} s | {w("contention_lock_partition_wait_us"):.3f} s |')
P()

# ---- group iso
P('**The four isolated cells run at `group`, beside their `relaxed` twins (BQ-S2 base, 1 run each; no mixed `group` cell was run for any arm)**')
P()
P('| cell | durability | n | ' + ' | '.join(PCT_NAMES) + ' | load before | JSON written (UTC) |')
P('|---|---|---|' + '---|' * 7 + '---|---|')
for c in ['c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso']:
    for dur in ('relaxed', 'group'):
        j = load(S2, dur, c, 'base', 1)
        a = j[agg_key(j)]
        P(f'| {c} | {dur} | {a["n"]:,} | ' + ' | '.join(us(a[p]) for p in PCT) + f' | {" / ".join(j["host_before"]["loadavg"].split()[:3])} | {hhmm(j["_mtime"])} |')
P()

# ---- mount
P('**Mount probe v2 (clean `STOP`, restart timed from exec to the first accepted connection), two probes per size; the load is 2.5 segments of log at every size**')
P()
P('| segment size | probe | log written before the stop | mount (exec to listening) | completion checkpoint inside it | mount minus the checkpoint | analysis + redo | longest `INSERT` statement of the load |')
P('|---|---|---|---|---|---|---|---|')
for arm in ('base', 's16', 's256', 's1024'):
    for rep in (1, 2):
        m = json.load(open(f'{S2}/mount/{arm}-r{rep}.json'))
        rec = m['recovery']
        cp = rec['recovery_checkpoint_us'] / 1e6
        P(f'| {m["segment_mib"]:,} MiB | r{rep} | {m["log_bytes"]/1048576:,.0f} MiB ({m["log_bytes"]/1048576/m["segment_mib"]:.2f} segments) | {m["mount_s"]:.3f} s | {cp:.3f} s | '
          f'{m["mount_s"]-cp:.3f} s | {(rec["recovery_analysis_us"]+rec["recovery_redo_us"])/1e6:.3f} s | {m["longest_insert_s"]:.2f} s |')
P()
P('**Mount probe v1 (discarded): SIGKILL, restart timed**')
P()
P('| segment size | log written | mount | completion checkpoint | analysis + redo | records replayed |')
P('|---|---|---|---|---|---|')
for arm in ('base', 's16', 's256'):
    m = json.load(open(f'{S2}/mount-v1-superseded/{arm}-r1.json'))
    rec = m['recovery']
    P(f'| {m["segment_mib"]:,} MiB | {m["log_bytes"]/1048576:,.0f} MiB | {m["mount_s"]:.3f} s | {rec["recovery_checkpoint_us"]/1e6:.3f} s | '
      f'{(rec["recovery_analysis_us"]+rec["recovery_redo_us"])/1e6:.3f} s | {rec["recovery_records"]:,.0f} |')
P()
print('\n'.join(OUT))
