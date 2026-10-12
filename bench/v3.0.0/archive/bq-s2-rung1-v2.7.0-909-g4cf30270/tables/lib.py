"""BQ-S2 results-file analysis: reads the raw driver JSON and `[wal] roll` lines.
Read-only. Run as `nice python3 -I`.  (-I drops the script dir from sys.path, so
callers use runpy-free exec of this file or sys.path insert via importlib.)"""
import glob, gzip, json, math, os, re, statistics as st, time
from fractions import Fraction

S2 = '/home/cdkbs/bench-runs/bq/s2/out'
S1 = '/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/bench/v3.0.0/archive/bq-s1-segment-roll-v2.7.0-909-g4cf30270/bq'
ARMS = ['base', 'opt3', 's16', 's256', 's1024']
SEG = {'base': 64, 'opt3': 64, 's16': 16, 's256': 256, 's1024': 1024}
C1 = ['c1-segregated-no0', 'c1-batch0', 'c1-spread']
CELLS = C1 + ['c2']
ISO = {'c1-segregated-no0': 'c1-iso-segregated-no0', 'c1-batch0': 'c1-iso-batch0',
       'c1-spread': 'c1-iso-spread', 'c2': 'c2-iso'}
PCT = ['p0_us', 'p25_us', 'p50_us', 'p95_us', 'p99_us', 'p99.9_us', 'max_us']
PCT_NAMES = ['p0', 'p25', 'p50', 'p95', 'p99', 'p99.9', 'max']
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


def _open(p):
    return gzip.open(p, 'rt') if p.endswith('.gz') else open(p)


_cache = {}


def load(root, dur, cell, arm, r):
    key = (root, dur, cell, arm, r)
    if key in _cache:
        return _cache[key]
    if root == S2:
        d = f'{root}/{dur}/{cell}/{arm}/r{r}'
    else:
        d = f'{root}/{dur}/{cell}/r{r}'
    js = glob.glob(d + '/*.json') + glob.glob(d + '/*.json.gz')
    assert len(js) == 1, d
    j = json.load(_open(js[0]))
    lg = glob.glob(d + '/*.s.log')
    assert len(lg) == 1, d
    j['_rolls'] = [kv(m.group(1)) for m in map(ROLL.search, open(lg[0])) if m]
    j['_dir'] = d
    j['_mtime'] = os.path.getmtime(js[0])
    _cache[key] = j
    return j


def nr(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    return xs[max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1]


def inwin(j):
    lo, hi = j['m0_mono_ns'], j['m1_mono_ns']
    return [x for x in j['_rolls'] if lo <= x['start_ns'] <= hi]


def delta(j, k):
    return j['meta_m1'].get(k, 0) - j['meta_m0'].get(k, 0)


def us(x, nd=None):
    """microseconds -> string with unit"""
    if x is None:
        return 'n/a'
    a = abs(x)
    if a >= 1e6:
        return f'{x / 1e6:.2f} s' if a < 1e7 else f'{x / 1e6:.1f} s'
    if a >= 1e3:
        v = x / 1e3
        return f'{v:.1f} ms' if abs(v) < 100 else f'{v:.0f} ms'
    return f'{x:.0f} µs' if a >= 100 else f'{x:.1f} µs'


def med(xs):
    return st.median(xs)


def agg_key(j):
    return 'read_from_schedule' if j['readers'] else 'writer_from_schedule'


def svc_key(j):
    return 'read_service' if j['readers'] else 'writer_service'


def hhmm(t):
    return time.strftime('%H:%M:%S', time.gmtime(t))
