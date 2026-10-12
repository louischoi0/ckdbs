"""Loaders and statistics for the BP/BQ close A/B. Analysis only."""
import glob, json, math, os, statistics

OUT = '/home/cdkbs/bench-runs/close/out'
med = statistics.median
T975_2DF = 4.303  # t, two-sided 95 %, 2 degrees of freedom

PAIRS = {
    # name: (dir, A label, B label)
    'AB': ('bn', 'A cf889b63', 'B 3e77e7d1'),
    'AQ': ('bn', 'A cf889b63', 'Q 4cf30270'),
    'S2S1': ('bisect', 'A cf889b63', '1592007e'),
    'S4S6BQ': ('bisect', 'ccf9cfc1', 'B 3e77e7d1'),
    'MERGE': ('bisect2', 'ccf9cfc1', '6139c2de'),
    'S4': ('bisect2', '6139c2de', 'd555cb55'),
    'S6': ('bisect2', 'd555cb55', 'B 3e77e7d1'),
    'MERGEALIGNED': ('aligned', 'ccf9cfc1 aligned', '6139c2de aligned'),
    'PREMERGE_FIX': ('rollfix', 'ccf9cfc1', 'rollfix'),
    'CLOSE_FIX': ('rollfix', 'B 3e77e7d1', 'rollfix'),
}


def load_pair(name):
    """-> {(shape, path): {'A': [arm per run], 'B': [arm per run]}} (arm dicts as the driver wrote them)."""
    d = PAIRS[name][0]
    out = {}
    runs = []
    for f in sorted(glob.glob(f'{OUT}/{d}/{name}-r*.json')):
        j = json.load(open(f))
        run = j['runs'][0]
        runs.append(run)
        for a in run['arms']:
            out.setdefault((a['shape'], a['path']), {'A': [], 'B': []})[a['bin']].append(a)
    return out, runs


def binom_one_sided_lower(k, n):
    """P(X <= k), X ~ Bin(n, 1/2): chance of B < A in at most k of n blocks... used as P(X >= k) via symmetry."""
    return sum(math.comb(n, i) for i in range(0, k + 1)) / 2 ** n


def sign_p_lower(k, n):
    """One-sided p that B is lower than A in >= k of n blocks by chance."""
    return sum(math.comb(n, i) for i in range(k, n + 1)) / 2 ** n


def pair_stats(arms):
    """arms = {'A': [..], 'B': [..]} -> dict of server-CPU statistics."""
    a = [x['server_cpu_us'] for x in arms['A']]
    b = [x['server_cpu_us'] for x in arms['B']]
    d = [y - x for x, y in zip(a, b)]
    pct = [100 * (y - x) / x for x, y in zip(a, b)]
    n = len(d)
    mean_d = sum(d) / n
    sd = statistics.stdev(d) if n > 1 else float('nan')
    se = sd / math.sqrt(n) if n > 1 else float('nan')
    # blocks
    blk = []
    for xa, xb in zip(arms['A'], arms['B']):
        blk.append([y - x for x, y in zip(xa['block_server_us'], xb['block_server_us'])])
    below = [sum(1 for v in r if v < 0) for r in blk]
    allb = [v for r in blk for v in r]
    return dict(a=a, b=b, d=d, pct=pct, med_d=med(d), med_pct=med(pct), mean_d=mean_d, mean_pct=sum(pct) / n,
                se=se, lo=mean_d - T975_2DF * se, hi=mean_d + T975_2DF * se,
                spread_d=max(d) - min(d), spread_a=max(a) - min(a), spread_b=max(b) - min(b),
                blk=blk, below=below, nblk=len(allb), below_all=sum(1 for v in allb if v < 0),
                blk_mean=[sum(r) / len(r) for r in blk])
