"""H0's criterion terms per cell, per run, with the spread (max - min).
usage: python3 -I summary.py <out_dir>  (imports h0.py from this directory)"""
import os
import statistics
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h0 import analyze, share  # noqa: E402

out = sys.argv[1]
for dur in ('relaxed', 'group'):
    for cell, isocell in (('c1-segregated-no0', 'c1-iso-segregated-no0'), ('c1-batch0', 'c1-iso-batch0'),
                          ('c1-spread', 'c1-iso-spread'), ('c2', 'c2-iso')):
        rows = analyze(f'{out}/bq/{dur}/{cell}')
        iso = [r['lat']['p99_us'] for r in analyze(f'{out}/bq/{dur}/{isocell}')]
        hold = [max(x['hold_us'] for x in r['inm']) for r in rows]
        p99 = [r['lat']['p99_us'] for r in rows]
        s_stream, s_all, nbs, abs_cov, abs_tot = [], [], [], [], []
        for r in rows:
            nb = [k for k in range(4) if k not in set(r['roll_cores'])]
            cov, tot, sh = share(r['pc'], nb)
            allt = sum(r['pc'][k]['all'] for k in nb)
            s_stream.append(None if sh is None else round(sh, 1))
            s_all.append(round(100 * cov / allt, 1) if allt else None)
            nbs.append(nb)
            abs_cov.append(round(cov / 1000))
            abs_tot.append(round(tot / 1000))
        med = statistics.median(p99) / statistics.median(iso)
        lo = min(p99) / max(iso)
        hi = max(p99) / min(iso)
        paired = [round(a / b, 2) for a, b in zip(p99, iso)]
        sp = lambda xs: round(max(x for x in xs if x is not None) - min(x for x in xs if x is not None), 1) if any(x is not None for x in xs) else None
        print(f'{dur:7} {cell:18} hold_max_us={hold} (spread {sp(hold)}) | non-bulk={nbs} '
              f'covered_ms={abs_cov} stream_ms={abs_tot} share_stream%={s_stream} (spread {sp(s_stream)}) '
              f'share_allkinds%={s_all} (spread {sp(s_all)}) | p99_us={p99} (spread {sp(p99)}) '
              f'iso_p99_us={iso} (spread {sp(iso)}) ratio median/median={med:.2f}x paired={paired} '
              f'range=[{lo:.2f}x, {hi:.2f}x]')
