import sys, subprocess, collections, json, re
sys.path.insert(0, '/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column/tools')
import kds_sampler_report as R


def resolve(maps, pcs, binpath):
    rel = []
    for a in pcs:
        for lo, hi, off, path in maps:
            if lo <= a < hi and path.endswith('kds_server'):
                rel.append((a, a - lo + off))
                break
    out = {}
    if not rel:
        return out
    r = subprocess.run(['addr2line', '-a', '-i', '-f', '-C', '-e', binpath] + ['%x' % x for _, x in rel],
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    i = 0
    res = {}
    cur = None
    while i < len(lines):
        if lines[i].startswith('0x'):
            cur = int(lines[i], 16)
            res[cur] = []
            i += 1
        else:
            res[cur].append((lines[i], lines[i + 1]))
            i += 2
    for a, x in rel:
        out[a] = res.get(x, [])
    return out


def run(arm):
    fn = collections.Counter()
    ln = collections.Counter()
    for r in (1, 2, 3):
        for d in (0, 1):
            maps, samples = R.load('out/%s%d/dump.%d' % (arm, r, d))
            b = 'build-%s/kds_server' % arm
            rs = resolve(maps, {s[0] for s in samples}, b)
            n = len(samples)
            for s in samples:
                fr = rs.get(s[0])
                if not fr:
                    fn['[other]'] += 100.0 / n / 6
                    ln['[other]'] += 100.0 / n / 6
                    continue
                f, l = fr[0]
                f = R.trim(f)
                l = re.sub(r'.*/(src|include)/', r'\1/', l.split(' (')[0])
                fn[f] += 100.0 / n / 6
                ln[f + ' @ ' + l] += 100.0 / n / 6
    return fn, ln


fa, la = run('A')
fb, lb = run('B')
json.dump({'fa': fa, 'fb': fb, 'la': la, 'lb': lb}, open('inl.json', 'w'))
for name, a, b in (('FUNC', fa, fb), ('LINE', la, lb)):
    print('==', name)
    ks = sorted(set(a) | set(b), key=lambda k: b.get(k, 0) - a.get(k, 0), reverse=True)
    for k in ks[:18] + ks[-10:]:
        print('%+5.2f A=%5.2f B=%5.2f %s' % (b.get(k, 0) - a.get(k, 0), a.get(k, 0), b.get(k, 0), k[:120]))
