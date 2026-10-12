import glob, json, math, statistics as st
AB = '/home/cdkbs/bench-runs/bp-s1/v2/out/ab'
SHAPES = [('select-pk', 'literal'), ('select-pk', 'prepared'), ('update-lit', 'literal'), ('update-lit', 'prepared'),
          ('update-range', 'literal'), ('update-range', 'prepared')]


def bn(r):
    d = json.load(open(f'{AB}/bn-r{r}.json'))
    run = d['runs'][0]
    arms = {(a['shape'], a['path'], a['bin']): a for a in run['arms']}
    return d, run, arms


def commit(dur, side, r):
    f = glob.glob(f'{AB}/commit-{dur}-{side}-r{r}/*.json')
    assert len(f) == 1
    return json.load(open(f[0]))


def us(x, nd=None):
    if x is None:
        return 'n/a'
    a = abs(x)
    if a >= 1e6:
        return f'{x/1e6:.2f} s'
    if a >= 1e3:
        v = x / 1e3
        return f'{v:.2f} ms' if abs(v) < 10 else (f'{v:.1f} ms' if abs(v) < 100 else f'{v:.0f} ms')
    if a >= 100:
        return f'{x:.0f} µs'
    return f'{x:.2f} µs' if nd is None else f'{x:.{nd}f} µs'


def med(x):
    return st.median(x)
