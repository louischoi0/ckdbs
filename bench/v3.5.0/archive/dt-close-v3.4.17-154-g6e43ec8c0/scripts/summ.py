import json, glob, os, sys
D = '/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0/'
cells = {}
for f in glob.glob(D + 'dt-*-r*.json'):
    d = json.load(open(f))
    cells[(d['cell'], d['arm'], d['run'])] = d


def get(c, a, fn):
    out = []
    for r in (0, 1):
        d = cells.get((c, a, r))
        if d:
            out.append(fn(d))
    return out


def pc(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


metrics = {
    'hot reads/s': lambda d: d['hot_service']['n'] / d['seconds'],
    'hot p99 (µs)': lambda d: d['hot_service']['p99_us'],
    'hot max (µs)': lambda d: d['hot_service']['max_us'],
    'walks finished': lambda d: len(d['walk_seconds']),
    'walk p50 (ms)': lambda d: pc(d['walk_seconds'], .5) * 1000,
    'walk p99 (ms)': lambda d: pc(d['walk_seconds'], .99) * 1000,
    'walk max (ms)': lambda d: max(d['walk_seconds']) * 1000,
    'walks over 1 s': lambda d: sum(1 for x in d['walk_seconds'] if x > 1.0),
    'WAL appended (MB)': lambda d: d['meta_delta']['wal_append_lsn'] / 1e6,
    'page images': lambda d: d['meta_delta']['pool_fetch_images'],
    'sync-gate wait (s)': lambda d: d['meta_delta']['contention_wal_sync_gate_wait_us'] / 1e6,
}


def fmt(xs):
    if not xs:
        return 'not run'
    return ' and '.join(f'{x:,.1f}' if abs(x) < 1000 and x != int(x) else f'{x:,.0f}' for x in xs)


def mid(xs):
    return sum(xs) / len(xs)


for c in ('ivw', 'ivu', 'ivr', 'i', 'aw', 'au'):
    print('###', c)
    print('| metric | A | B | C | C vs A (mean of runs) | C vs B |')
    print('|---|---|---|---|---|---|')
    for name, fn in metrics.items():
        if c in ('aw', 'au') and name.startswith('hot'):
            continue
        if c in ('ivr', 'i') and not name.startswith('hot') and name not in ('walks finished', 'walk p50 (ms)'):
            continue
        if c == 'i' and name != 'hot reads/s' and not name.startswith('hot'):
            continue
        try:
            a, b, cc = (get(c, x, fn) for x in 'ABC')
        except KeyError:
            continue
        if not a or not cc:
            continue
        if c == 'i' and not name.startswith('hot'):
            continue
        da = f'{mid(cc) - mid(a):+,.1f}'
        db = f'{mid(cc) - mid(b):+,.1f}' if b else 'n/a'
        print(f'| {name} | {fmt(a)} | {fmt(b)} | {fmt(cc)} | {da} | {db} |')
    print()
