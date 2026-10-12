"""Shared loaders for the BQ-S1 census tables. Read-only over
/home/cdkbs/bench-runs/bq/out-4cf30270. Counters are m1 - m0 with a key absent
from m0 read as 0 (the driver's own meta_delta drops such keys)."""
import glob, json, math, os, re

OUT = __import__('os').environ.get('BQ_OUT', '/home/cdkbs/bench-runs/bq/out-4cf30270')
ROLL = re.compile(r'\[wal\] roll (.*)$')

CELLS = ['c3', 'c2', 'c2-iso', 'c1-segregated-no0', 'c1-iso-segregated-no0', 'c1-batch0',
         'c1-iso-batch0', 'c1-spread', 'c1-iso-spread']
DURS = ['relaxed', 'group']


def nr(xs, q):
    """Nearest rank on a list; q in [0, 100]."""
    xs = sorted(xs)
    if not xs:
        return None
    if q <= 0:
        return xs[0]
    k = max(1, math.ceil(q / 100.0 * len(xs)))
    return xs[min(k, len(xs)) - 1]


def fmt(us):
    """A latency in microseconds, unit in the cell."""
    if us is None:
        return 'n/a'
    if us < 1000:
        return f'{us:.0f} µs' if us >= 100 else f'{us:.1f} µs'
    if us < 1e6:
        ms = us / 1e3
        return f'{ms:.1f} ms' if ms < 100 else f'{ms:.0f} ms'
    return f'{us / 1e6:.2f} s'


def run_dir(base, dur, cell, r):
    return f'{OUT}/{base}/{dur}/{cell}/r{r}' if base == 'bq' else None


def load(path):
    js = glob.glob(path + '/bl-mixed-*.json')
    d = json.load(open(js[0]))
    sl = glob.glob(path + '/*.s.log')[0]
    return d, sl


def delta(d, k):
    m0, m1 = d['meta_m0'], d['meta_m1']
    try:
        return float(m1.get(k, 0)) - float(m0.get(k, 0))
    except (TypeError, ValueError):
        return 0.0


def m1v(d, k):
    try:
        return float(d['meta_m1'].get(k, 0))
    except (TypeError, ValueError):
        return 0.0


def rolls(slog, d):
    """Roll records whose start lies in [m0, m1], as dicts of ints (body kept as str)."""
    out = []
    allr = []
    for line in open(slog):
        m = ROLL.search(line)
        if not m:
            continue
        kv = dict(t.split('=', 1) for t in m.group(1).split() if '=' in t)
        rec = {k: (v if k == 'body' else int(v)) for k, v in kv.items()}
        allr.append(rec)
        if d['m0_mono_ns'] <= rec['start_ns'] <= d['m1_mono_ns']:
            out.append(rec)
    return out, allr


def runs(dur, cell, n=3):
    for r in range(1, n + 1):
        p = f'{OUT}/bq/{dur}/{cell}/r{r}'
        d, sl = load(p)
        yield r, d, sl
