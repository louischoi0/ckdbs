"""Does opt3's short hold persist through the run? Median hold and header by third of the in-window rolls, by time."""
import importlib.util, io, contextlib, statistics as st
spec = importlib.util.spec_from_file_location('o', '/home/cdkbs/bench-runs/bq/verdicts/s2/h4opt3/opt3.py')
o = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(o)
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
    for arm in ('base', 'opt3'):
        for r in (1, 2, 3):
            R = o.run(cell, arm, r)
            w = sorted(R['win'], key=lambda x: x['start_ns'])
            lo, hi = R['j']['measure_from_mono_ns'], R['j']['stop_at_mono_ns']
            thirds = [[], [], []]
            for x in w:
                thirds[min(2, int(3 * (x['start_ns'] - lo) / (hi - lo)))].append(x)
            s = ' | '.join(f'n={len(t)} hold med {st.median([x["hold_us"] for x in t]) / 1e3:.0f} ms '
                           f'max {max(x["hold_us"] for x in t) / 1e3:.0f}' if t else 'n=0' for t in thirds)
            first_slow = next((round((x['start_ns'] - lo) / 1e9, 1) for x in w if x['hold_us'] > 150000), None)
            print(f'{cell:18} {arm:4} r{r}: {s} | first >150 ms roll at {first_slow} s')
