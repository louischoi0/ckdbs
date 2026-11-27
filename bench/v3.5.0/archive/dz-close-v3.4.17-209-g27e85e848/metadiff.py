import glob, json, math, sys
d = sys.argv[1]
shape = sys.argv[2] if len(sys.argv) > 2 else 'insert1-group-k2'
rounds = range(1, 13)


def load(lbl):
    out = []
    for r in rounds:
        fs = glob.glob(f'{d}/json-{lbl}-{shape}-r{r}/*.json')
        if fs:
            out.append(json.load(open(fs[0])))
    return out


A = load('A')
B = load('B')
keys = set()
for j in A + B:
    keys |= set(j['meta_delta'])
rows = []
for k in keys:
    sa = sum(j['meta_delta'].get(k, 0) for j in A) / len(A)
    sb = sum(j['meta_delta'].get(k, 0) for j in B) / len(B)
    if max(abs(sa), abs(sb)) < 1:
        continue
    rows.append((abs(math.log((abs(sb) + 1) / (abs(sa) + 1))), k, sa, sb))
rows.sort(reverse=True)
for r in rows[:40]:
    print('%-58s A=%.0f B=%.0f' % (r[1], r[2], r[3]))
print('statements A', [j.get('statements') for j in A])
print('statements B', [j.get('statements') for j in B])
