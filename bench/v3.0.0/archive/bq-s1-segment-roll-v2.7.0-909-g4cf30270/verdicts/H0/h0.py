"""H0's verdict, recomputed from the raw run files (BQ order section 3, H0).

usage: python3 -I h0.py <out_dir>   (out_dir = .../out-4cf30270)

Per run it reads the driver JSON and the kept server log:
  * rolls: every "[wal] roll" line; "in window" = start_ns inside
    [measure_from_mono_ns, stop_at_mono_ns] (the driver's measured window,
    CLOCK_MONOTONIC, the engine's steady_clock); "in meta" = start inside
    [m0_mono_ns, m1_mono_ns] (the SHOW META delta's window).
  * the bulk core: the core= field of the rolls (the appender that crossed
    the segment end; at ~75-80 MB/s that is the inserter) inside the meta
    window, set against the inserter's connect core and its closing core.
  * per core k: load_c<k>_waited_wal_stream_us, load_c<k>_wal_roll_blocked_wait_us,
    load_c<k>_wal_drain_latch_wait_us, and every load_c<k>_waited_<kind>_us.
  * open-loop latency from the schedule (read_from_schedule for C1,
    writer_from_schedule for C2) and the per-client slow samples
    ([due ns, late ns], >= 1 ms) lined up with roll windows.
"""
import glob
import json
import os
import re
import sys

ROLL = re.compile(r'\[wal\] roll (.*)$')
RELEASE_NS = 20_000_000


def kv(text):
    out = {}
    for tok in text.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def load_run(js):
    d = json.load(open(js))
    rolls = []
    for line in open(js[:-5] + '.s.log'):
        m = ROLL.search(line)
        if m:
            rolls.append(kv(m.group(1)))
    return d, rolls


def per_core(md, cores=4):
    out = {}
    for k in range(cores):
        p = f'load_c{k}_'
        allwait = sum(v for key, v in md.items()
                      if key.startswith(p + 'waited_') and key.endswith('_us'))
        out[k] = dict(stream=md.get(p + 'waited_wal_stream_us', 0.0),
                      covered=md.get(p + 'wal_roll_blocked_wait_us', 0.0),
                      drain=md.get(p + 'wal_drain_latch_wait_us', 0.0),
                      gate=md.get(p + 'waited_wal_sync_gate_us', 0.0),
                      all=allwait)
    return out


def overlaps(t0, t1, wins):
    for s, e in wins:
        if s < t1 and e > t0:
            return True
    return False


def analyze(cell_dir):
    rows = []
    for js in sorted(glob.glob(cell_dir + '/r*/*.json')):
        d, rolls = load_run(js)
        lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
        m0, m1 = d['m0_mono_ns'], d['m1_mono_ns']
        inw = [r for r in rolls if lo <= r['start_ns'] <= hi]
        inm = [r for r in rolls if m0 <= r['start_ns'] <= m1]
        ins = [c for c in d['clients'] if c['role'] == 'inserter']
        roll_cores = sorted({r['core'] for r in inm})
        # meta_delta drops a key absent from meta_m0 (a per-core field is
        # absent while 0), so the counters are re-derived as m1 - m0 with an
        # absent m0 read as 0.
        m0d, m1d = d['meta_m0'], d['meta_m1']
        md = dict(d['meta_delta'])
        dropped = []
        for key, v1 in m1d.items():
            if key.startswith(('load_c', 'contention_')) and ('wait' in key or 'roll' in key):
                v = v1 - m0d.get(key, 0.0)
                if key not in d['meta_delta']:
                    dropped.append((key, v))
                md[key] = v
        pc = per_core(md)
        # The latency the criterion reads.
        lat = d.get('read_from_schedule') or d.get('writer_from_schedule') or {}
        role = 'reader' if d.get('read_from_schedule') else 'writer'
        # Slow samples lined up with the rolls.
        wins = [(r['start_ns'], r['end_ns']) for r in rolls]
        p99_ns = (lat.get('p99_us') or 0) * 1000
        n_slow = n_slow_ov = n_top = n_top_ov = n_top_rel = 0
        ends = sorted(e for _s, e in wins)
        for c in d['clients']:
            if c['role'] != role:
                continue
            for due, late in c['slow']:
                ov = overlaps(due, due + late, wins)
                n_slow += 1
                n_slow_ov += ov
                if late >= p99_ns:
                    n_top += 1
                    n_top_ov += ov
                    # Released by a roll: the sample completes within
                    # RELEASE_NS after some roll's end (a sharp test; the
                    # chance rate is rolls * RELEASE_NS / window).
                    done = due + late
                    n_top_rel += any(0 <= done - e <= RELEASE_NS for e in ends)
        n_rel_chance = len([e for e in ends if lo <= e <= hi]) * RELEASE_NS / (hi - lo)
        # Fraction of the window a roll covers (chance baseline).
        cov = sum(max(0, min(r['end_ns'], hi) - max(r['start_ns'], lo)) for r in rolls) / (hi - lo)
        rows.append(dict(run=os.path.basename(os.path.dirname(js)), d=d, inw=inw, inm=inm,
                         ins=[(c['core'], c['core_end']) for c in ins], roll_cores=roll_cores,
                         roll_core_seq=[r['core'] for r in inm], pc=pc, lat=lat, role=role,
                         slow=(n_slow, n_slow_ov, n_top, n_top_ov), rel=(n_top_rel, n_rel_chance), cov=cov,
                         hold_max_m1=d['meta_m1'].get('wal_roll_hold_max_us'),
                         hold_max_delta=md.get('wal_roll_hold_max_us'),
                         rolls_meta=md.get('wal_rolls'), dropped=dropped, md_raw=d['meta_delta']))
    return rows


def share(pc, cores, key='stream'):
    tot = sum(pc[k][key] for k in cores)
    cov = sum(pc[k]['covered'] for k in cores)
    return cov, tot, (100 * cov / tot if tot else None)


def fmt(x, n=1):
    return 'None' if x is None else f'{x:.{n}f}'


def main(out):
    for dur in ('relaxed', 'group'):
        print(f'=================== {dur} ===================')
        iso = {}
        for cell in ('c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso'):
            iso[cell] = [r['lat'].get('p99_us') for r in analyze(f'{out}/bq/{dur}/{cell}')]
            rr = analyze(f'{out}/bq/{dur}/{cell}')
            print(f'{cell:24} iso p99_us={iso[cell]} p99.9_us={[r["lat"].get("p99.9_us") for r in rr]} '
                  f'rolls_in_window={[len(r["inw"]) for r in rr]}')
        for cell, isocell in (('c1-segregated-no0', 'c1-iso-segregated-no0'),
                              ('c1-batch0', 'c1-iso-batch0'), ('c1-spread', 'c1-iso-spread'),
                              ('c2', 'c2-iso')):
            rows = analyze(f'{out}/bq/{dur}/{cell}')
            print(f'--- {cell} (iso {isocell} p99 {iso[isocell]}; spread {fmt(max(iso[isocell]) - min(iso[isocell]))})')
            for r in rows:
                holds = [x['hold_us'] for x in r['inw']]
                holds_m = [x['hold_us'] for x in r['inm']]
                pc = r['pc']
                print(f'  {r["run"]}: rolls win={len(r["inw"])} meta={len(r["inm"])} (counter {r["rolls_meta"]}) '
                      f'hold max win={max(holds) if holds else None} max meta={max(holds_m) if holds_m else None} '
                      f'(meta_m1 max {r["hold_max_m1"]}, meta_delta max {r["hold_max_delta"]}) '
                      f'p50={sorted(holds)[len(holds)//2] if holds else None}us')
                print(f'      inserter connect->end core={r["ins"]} roll cores (meta window, in order)={r["roll_core_seq"]} '
                      f'moves_in_window={r["d"]["moves_in_window"]}')
                for k in range(4):
                    c = pc[k]
                    print(f'      c{k}: stream={c["stream"]:.0f} covered={c["covered"]:.0f} drain={c["drain"]:.0f} '
                          f'sync_gate={c["gate"]:.0f} all_kinds={c["all"]:.0f}')
                bulk = set(r['roll_cores'])
                nb = [k for k in range(4) if k not in bulk]
                cov, tot, sh = share(pc, nb)
                alltot = sum(pc[k]['all'] for k in nb)
                print(f'      non-bulk cores={nb} (bulk = every core that rolled in the meta window {sorted(bulk)}): '
                      f'covered={cov:.0f} / stream={tot:.0f} = {fmt(sh)}%; / all latch kinds={alltot:.0f} = '
                      f'{fmt(100 * cov / alltot if alltot else None)}%')
                # Analyzer's definition, for the comparison.
                abulk = 0 if 'batch0' in cell else 3
                anb = [k for k in range(4) if k != abulk]
                acov, atot, ash = share(per_core(r['md_raw']), anb)
                if r['dropped']:
                    print('      keys meta_delta dropped (absent at m0), m1-m0: ' + ', '.join(f'{k}={v:.0f}' for k, v in r['dropped']))
                print(f'      analyze_s1.py definition (bulk=c{abulk}): {fmt(ash)}%')
                lat = r['lat']
                ratio = lat.get('p99_us') / max(iso[isocell]) if lat.get('p99_us') else None
                print(f'      {r["role"]} from_schedule p0={lat.get("p0_us")} p50={lat.get("p50_us")} '
                      f'p99={lat.get("p99_us")} p99.9={lat.get("p99.9_us")} max={lat.get("max_us")} n={lat.get("n")}; '
                      f'p99 / max(iso p99)={fmt(ratio, 2)}x, / min(iso p99)={fmt(lat.get("p99_us") / min(iso[isocell]), 2)}x')
                ns, nso, nt, nto = r['slow']
                print(f'      slow samples (>=1 ms late)={ns}, overlapping a roll={nso} '
                      f'({fmt(100 * nso / ns if ns else None)}%); samples >= p99={nt}, overlapping a roll={nto} '
                      f'({fmt(100 * nto / nt if nt else None)}%); rolls cover {100 * r["cov"]:.1f}% of the window; '
                      f'>=p99 samples completing <=20 ms after a roll end={r["rel"][0]}/{nt} '
                      f'({fmt(100 * r["rel"][0] / nt if nt else None)}%, chance {100 * r["rel"][1]:.1f}%)')
                rbc = r['d'].get('read_by_core') or r['d'].get('writer_by_core')
                print('      by connect core p99: ' + ', '.join(f'c{int(float(k))}={v.get("p99_us")}' for k, v in sorted(rbc.items())))


if __name__ == '__main__':
    main(sys.argv[1])
