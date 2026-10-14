import json, collections, re
d = json.load(open('inl.json'))


def byfile(l):
    c = collections.Counter()
    for k, v in l.items():
        m = re.search(r' @ (.*):\d+$', k)
        f = m.group(1) if m else k
        if '/c++/' in f or 'bits/' in f:
            f = 'libstdc++ headers'
        c[f] += v
    return c


a = byfile(d['la'])
b = byfile(d['lb'])
ks = sorted(set(a) | set(b), key=lambda k: b.get(k, 0) - a.get(k, 0), reverse=True)
for k in ks[:14] + ks[-8:]:
    print('%+5.2f A=%5.2f B=%5.2f %s' % (b.get(k, 0) - a.get(k, 0), a.get(k, 0), b.get(k, 0), k))
print('sum A', sum(a.values()), 'sum B', sum(b.values()))
