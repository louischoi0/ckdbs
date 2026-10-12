"""H5's measurement half, recomputed from the raw files.
usage: python3 -I h5.py <out_dir> <json_out>
Per run: the rolls in the meta window [m0_mono_ns, m1_mono_ns] by the core that
rolled (the roll line's core= - the inserter's core at that instant), and per
core k the meta_delta fields waited_wal_stream_us, wal_drain_latch_wait_us,
wal_roll_blocked_wait_us, waited_wal_sync_gate_us, moves_out. Then the open-loop
clients' slow samples ([due ns, latency ns], >= 1 ms) lined up with roll windows."""
import bisect
import glob
import json
import math
import os
import re
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


def rolls(log):
    out = []
    for line in open(log):
        m = ROLL.search(line)
        if m:
            out.append(kv(m.group(1)))
    return sorted(out, key=lambda r: r['start_ns'])


def nr(xs, q):  # nearest rank, as bl_mixed.py
    xs = sorted(xs)
    if not xs:
        return None
    return xs[max(1, math.ceil(q / 100 * len(xs))) - 1]


def pctl_from_slow(slow_lat, n, q):
    """Nearest-rank q-th of n samples of which only those >= 1 ms are kept."""
    if not n:
        return None
    k = max(1, math.ceil(q / 100 * n))
    fast = n - len(slow_lat)
    if k <= fast:
        return '<1ms'
    s = sorted(slow_lat)
    return round(s[k - fast - 1], 1)


PRE_MS, POST_MS = 5, 30


def align(samples, rs):
    """A sample [due, due+lat] is roll-aligned when it overlaps a roll window
    and completes within [end - PRE_MS, end + POST_MS] of that roll's end."""
    ends = [r['end_ns'] for r in rs]
    out = []
    for due, lat in samples:
        c = due + lat
        i = bisect.bisect_left(ends, c - POST_MS * 1_000_000)
        hit = False
        while i < len(rs) and rs[i]['end_ns'] <= c + PRE_MS * 1_000_000:
            r = rs[i]
            if r['start_ns'] < c and r['end_ns'] > due:
                hit = True
                break
            i += 1
        out.append(hit)
    return out


def overlap_any(samples, rs):
    starts = [r['start_ns'] for r in rs]
    out = []
    for due, lat in samples:
        c = due + lat
        i = bisect.bisect_right(starts, c)
        out.append(any(rs[j]['end_ns'] > due for j in range(max(0, i - 3), i)))
    return out


def end_offsets(samples, rs):
    """For each sample overlapping a roll: completion minus that roll's end, ms."""
    offs = []
    for due, lat in samples:
        c = due + lat
        best = None
        for r in rs:
            if r['start_ns'] < c and r['end_ns'] > due:
                o = (c - r['end_ns']) / 1e6
                if best is None or abs(o) < abs(best):
                    best = o
        if best is not None:
            offs.append(best)
    return offs


def run(js, cell, dur):
    d = json.load(open(js))
    rs_all = rolls(js[:-5] + '.s.log')
    lo, hi = d['m0_mono_ns'], d['m1_mono_ns']
    rs = [r for r in rs_all if r['end_ns'] > lo and r['start_ns'] < hi]
    win_s = (hi - lo) / 1e9
    md = d['meta_delta']
    cores = {}
    for r in rs:
        cores[r['core']] = cores.get(r['core'], 0) + 1
    hold = sum(min(r['end_ns'], hi) - max(r['start_ns'], lo) for r in rs) / 1e3
    rec = {'cell': cell, 'dur': dur, 'run': js.split('/')[-2], 'win_s': round(win_s, 2),
           'rolls': len(rs), 'rolls_whole_in_measure': sum(
               1 for r in rs_all if r['start_ns'] >= d['measure_from_mono_ns'] and r['end_ns'] <= d['stop_at_mono_ns']),
           'roll_cores': cores, 'hold_in_win_us': round(hold),
           'hold_p50': nr([r['hold_us'] for r in rs], 50),
           'hold_max': max([r['hold_us'] for r in rs], default=None),
           'moves_in_window': d.get('moves_in_window'),
           'targets': d.get('targets'), 'roles': d.get('roles'), 'cores_end': d.get('cores_end')}
    # Per-core fields are absent from SHOW META while 0, and the driver's
    # meta_delta keeps only keys present in m0 - so a counter that was 0 at m0
    # is missing from meta_delta. Recomputed from the raw snapshots, absent = 0.
    m0, m1 = d['meta_m0'], d['meta_m1']
    assert not d.get('meta_ctl_moved')
    per = {}
    for k in range(4):
        def g(f):
            key = f'load_c{k}_{f}'
            return m1.get(key, 0.0) - m0.get(key, 0.0)
        per[k] = dict(stream=g('waited_wal_stream_us'), drain=g('wal_drain_latch_wait_us'),
                      rollcov=g('wal_roll_blocked_wait_us'), gate=g('waited_wal_sync_gate_us'),
                      moves_out=g('moves_out'))
    rec['per_core'] = per
    rec['c0_sched'] = {k: md.get(k) for k in ('sched_hold_out_run_over_100ms', 'sched_hold_out_run_over_10ms',
                                             'sched_unpolled_over_100ms', 'sched_longest_unpolled_us')}
    rec['contention'] = {k: md.get('contention_' + k) for k in (
        'wal_stream_wait_us', 'wal_drain_latch_wait_us', 'wal_roll_blocked_wait_us')}
    rec['meta_core'] = (d.get('meta_core_m0'), d.get('meta_core_m1'), d.get('meta_ctl_moved'))
    for role, key in (('reader', 'read_from_schedule'), ('writer', 'writer_from_schedule')):
        cl = [c for c in d['clients'] if c['role'] == role]
        if not cl:
            continue
        n = (d.get(key) or {}).get('n')
        samples = [tuple(s) for c in cl for s in c['slow']]
        lat = [s[1] / 1e3 for s in samples]
        al = align(samples, rs)
        ov = overlap_any(samples, rs)
        big = [(s, a) for s, a in zip(samples, al) if s[1] >= 10_000_000]
        lat_na = [x for x, a in zip(lat, al) if not a]
        offs = end_offsets([s for s in samples if s[1] >= 10_000_000], rs)
        rec[role] = dict(
            n=n, slow=len(samples), dropped=sum(c['slow_dropped'] for c in cl),
            p99_recomp=pctl_from_slow(lat, n, 99), p999_recomp=pctl_from_slow(lat, n, 99.9),
            p99_json=(d.get(key) or {}).get('p99_us'), p999_json=(d.get(key) or {}).get('p99.9_us'),
            ge10ms=len(big), ge10ms_aligned=sum(a for _, a in big),
            stall_ms_ge10=round(sum(s[1] for s, _ in big) / 1e6),
            stall_ms_ge10_aligned=round(sum(s[1] for s, a in big if a) / 1e6),
            ge10ms_overlap_any=sum(o for s, o in zip(samples, ov) if s[1] >= 10_000_000),
            p99_without_aligned=pctl_from_slow(lat_na, n, 99),
            p999_without_aligned=pctl_from_slow(lat_na, n, 99.9),
            roll_cover_frac=round(hold / 1e6 / win_s, 3),
            end_offset_ms_hist={b: sum(1 for o in offs if lo_ <= o < hi_) for b, (lo_, hi_) in {
                '<-50': (-1e9, -50), '-50..-5': (-50, -5), '-5..0': (-5, 0), '0..5': (0, 5), '5..30': (5, 30),
                '30..100': (30, 100), '>=100': (100, 1e9)}.items()},
            by_client=[])
        for c in cl:
            sm = [tuple(s) for s in c['slow']]
            a = align(sm, rs)
            b = [(s, x) for s, x in zip(sm, a) if s[1] >= 10_000_000]
            rec[role]['by_client'].append(dict(i=c['index'], core=c['core'], core_end=c['core_end'],
                                               ge10=len(b), al=sum(x for _, x in b),
                                               max_ms=round(max([s[1] for s in sm], default=0) / 1e6, 1)))
    return rec


def main():
    out, jout = sys.argv[1], sys.argv[2]
    allrec = []
    for dur in ('relaxed', 'group'):
        for cell in sorted(os.listdir(f'{out}/bq/{dur}')):
            for js in sorted(glob.glob(f'{out}/bq/{dur}/{cell}/r*/*.json')):
                allrec.append(run(js, cell, dur))
    json.dump(allrec, open(jout, 'w'), indent=1, default=str)
    for r in allrec:
        print(f"\n{r['dur']} {r['cell']} {r['run']} win={r['win_s']}s rolls={r['rolls']} "
              f"(whole-in-measure {r['rolls_whole_in_measure']}) by_core={r['roll_cores']} "
              f"hold_in_win={r['hold_in_win_us'] / 1e6:.2f}s p50={r['hold_p50']} max={r['hold_max']} "
              f"moves_in_win={r['moves_in_window']} meta_core={r['meta_core']}")
        print(f"   roles={r['roles']} targets={r['targets']} cores_end={r['cores_end']}")
        for k, p in r['per_core'].items():
            if any(p.values()):
                sh = f"{100 * p['drain'] / p['stream']:.1f}%" if p['stream'] else '-'
                print(f"   c{k}: stream={p['stream'] / 1e3:9.1f}ms drain={p['drain'] / 1e3:9.1f}ms ({sh}) "
                      f"rollcov={p['rollcov'] / 1e3:9.1f}ms gate={p['gate'] / 1e3:9.1f}ms moves_out={p['moves_out']:.0f}")
        print(f"   core0 sched: {r['c0_sched']}")
        for role in ('reader', 'writer'):
            if role in r:
                x = r[role]
                print(f"   {role}: n={x['n']} slow={x['slow']} dropped={x['dropped']} "
                      f"p99 json/recomp={x['p99_json']}/{x['p99_recomp']} p99.9={x['p999_json']}/{x['p999_recomp']}")
                print(f"      >=10ms: {x['ge10ms']} aligned={x['ge10ms_aligned']} overlap_any={x['ge10ms_overlap_any']} "
                      f"stall_ms={x['stall_ms_ge10']} aligned_ms={x['stall_ms_ge10_aligned']} cover={x['roll_cover_frac']} "
                      f"| w/o aligned p99={x['p99_without_aligned']} p99.9={x['p999_without_aligned']}")
                print(f"      end-offset hist (ms, >=10ms samples overlapping a roll): {x['end_offset_ms_hist']}")
                print('      ' + ' '.join(f"[{c['i']}:{c['core']:.0f}->{c['core_end']:.0f} {c['al']}/{c['ge10']} "
                                         f"max={c['max_ms']}]" for c in x['by_client']))


if __name__ == '__main__':
    main()
