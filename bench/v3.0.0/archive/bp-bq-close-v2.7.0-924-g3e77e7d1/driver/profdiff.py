"""Self-time shares of two sides of a sampler report, top movers.
usage: profdiff.py REPORT A B"""
import re, sys

txt = open(sys.argv[1]).read()
secs = txt.split('=== ')[1:]


def selfmap(s):
    part = s.split('-- self --')[1].split('-- inclusive --')[0]
    n = int(re.search(r'samples (\d+)', s).group(1))
    m = {}
    for line in part.strip().splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+(\d+)\s+(.*)', line)
        if mm:
            m[mm.group(3)] = int(mm.group(2))
    return n, m


def agg(side):
    N, tot = 0, {}
    for s in secs:
        if s.startswith(side + ' '):
            n, m = selfmap(s)
            N += n
            for k, v in m.items():
                tot[k] = tot.get(k, 0) + v
    return N, tot


a, b = sys.argv[2], sys.argv[3]
Na, ta = agg(a)
Nb, tb = agg(b)
print(f'samples {a} {Na} {b} {Nb}')
keys = set(ta) | set(tb)
for k in sorted(keys, key=lambda k: -abs(tb.get(k, 0) / Nb - ta.get(k, 0) / Na))[:14]:
    print(f'{100 * ta.get(k, 0) / Na:6.2f}% -> {100 * tb.get(k, 0) / Nb:6.2f}%  {k[:90]}')
